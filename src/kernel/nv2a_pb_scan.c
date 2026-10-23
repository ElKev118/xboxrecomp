/*
 * Read-only survey of the pushbuffer a title submits.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; nothing
 * here executes them, so the framebuffer stays black however far the game
 * gets. Before any of that can be made to draw, the question is what it
 * actually asks for -- which methods, on which object classes, how many of
 * them -- because that is the difference between "the existing PGRAPH
 * translator nearly covers this" and "this needs a real one".
 *
 * Purely a reader: it walks the buffer and counts, and never writes to guest
 * memory or to the GPU state. Enabled with RECOMP_PB_SCAN.
 *
 * Pushbuffer encoding (NV20/NV2A), one dword per command header:
 *   (w & 0xE0030003) == 0x00000000  increasing methods
 *   (w & 0xE0030003) == 0x40000000  non-increasing (same method, count params)
 *   (w & 0x00000003) == 0x00000001  jump
 *   (w & 0x00000003) == 0x00000002  call
 *   (w & 0xFFFF0003) == 0x00020000  return
 * For a method header: count = (w >> 18) & 0x7FF, subchannel = (w >> 13) & 7,
 * method = w & 0x1FFC.
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define PB_MAX_METHODS 4096

static struct { uint32_t method, subch, count; } s_seen[PB_MAX_METHODS];
static int s_seen_count;

/* Parse health. An inventory is only worth reading if the walk stayed in step
 * with the command stream: a decoder that desynchronises produces plausible
 * looking method numbers out of parameter data, and the counts then describe
 * nothing. Unrecognised words are the tell. */
static uint32_t s_tot_words, s_tot_unknown, s_tot_jumps, s_tot_segments;
static uint32_t s_tot_calls;

/* Executing is opt-in separately from surveying: a survey is read-only, while
 * the executor writes to guest memory. */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern int nv2a_pb_exec_bulk(uint32_t method, const uint32_t *params, uint32_t n);
extern int nv2a_pb_exec_bulk_inc(uint32_t method, const uint32_t *params, uint32_t n);
extern void nv2a_pb_exec_report(void);
static int s_exec_enabled = -1;
static int s_scan_enabled = -1;

/* Clears the NV2A request bits (xbox_memory_layout.c); called every 16
 * words while executing, so kickoffs are answered mid-walk: every one of them
 * is the title's main thread spinning until it is. */
extern void xbox_Nv2aAckBusyBits(void);
static uint32_t s_ack_tick;
/* Words between answers: 16, or 256 when a thread of its own answers the
 * kickoff (RECOMP_NV2A_KICK_SPIN) and the walk only covers the rest. */
static uint32_t s_ack_mask = 0x0Fu;

/* Either switch, read once. The answer "not set" has to be cached as well as
 * "set": this runs on every pass of the NV2A poll loop, and on the default
 * path -- neither switch set -- a cache that only remembered "set" called
 * getenv on every pass, which walks the whole environment. */
static int pb_walk_enabled(void)
{
    if (s_exec_enabled < 0)
        s_exec_enabled = getenv("RECOMP_PB_EXEC") != NULL;
    if (s_scan_enabled < 0)
        s_scan_enabled = getenv("RECOMP_PB_SCAN") != NULL;
    {
        static int ack_read;
        if (!ack_read) {
            const char *e = getenv("RECOMP_NV2A_KICK_SPIN");
            ack_read = 1;
            if (e && atoi(e))
                s_ack_mask = 0xFFu;
            /* RECOMP_NV2A_SYNC_KICKS=1 (diagnostic): never answer a kickoff
             * mid-walk, only once the walk has caught up with PUT (the poll
             * loop's own answer), so the title waits at each kickoff for the
             * GPU to have done everything before it -- a GPU with no lag, to
             * tell an executor that runs behind from a title bug. */
            e = getenv("RECOMP_NV2A_SYNC_KICKS");
            if (e && atoi(e))
                s_ack_mask = 0xFFFFFFFFu;
        }
    }
    return s_exec_enabled || s_scan_enabled;
}

/* Slot + 1 of each (subchannel, method) in s_seen. note() runs for every
 * pushbuffer word, so a linear search here was the executor's largest cost
 * (about 40% of its thread in X-Men Legends' levels). */
static uint16_t s_seen_slot[8][0x2000 / 4];

static void note_slow(uint32_t subch, uint32_t method)
{
    uint16_t *slot = &s_seen_slot[subch & 7][(method & 0x1FFC) / 4];

    /* The inventory is only ever printed under RECOMP_PB_SCAN
     * (nv2a_pb_scan_report); taking it anyway was 5 % of the executor. */
    if (!s_scan_enabled)
        return;
    if (*slot) {
        s_seen[*slot - 1].count++;
        return;
    }
    if (s_seen_count >= PB_MAX_METHODS) {
        /* Silently dropping past the cap is how a truncated inventory reads as
         * "the title never does that" -- exactly the wrong conclusion when the
         * inventory is being used to decide what to implement. */
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[PB] method table full at %d -- inventory is"
                            " truncated\n", PB_MAX_METHODS);
        }
    }
    if (s_seen_count < PB_MAX_METHODS) {
        s_seen[s_seen_count].method = method;
        s_seen[s_seen_count].subch  = subch;
        s_seen[s_seen_count].count  = 1;
        s_seen_count++;
        *slot = (uint16_t)s_seen_count;
    }
}

/* The check inline: a call per pushbuffer word was itself 2.6 % of the
 * executor with the inventory off. */
#define note(subch, method) (s_scan_enabled ? note_slow((subch), (method)) : (void)0)

/* note() for a run of n words of one method. */
static void note_n(uint32_t subch, uint32_t method, uint32_t n)
{
    uint16_t slot;

    note(subch, method);
    slot = s_seen_slot[subch & 7][(method & 0x1FFC) / 4];
    if (slot)
        s_seen[slot - 1].count += n - 1;
}

/* NV097 (Kelvin 3D class) methods worth naming. The point of the survey is to
 * decide what a translator has to implement, and a bare method number does not
 * answer that -- "0x1808 x412" only means something once it reads
 * INLINE_ARRAY. Unnamed ones still get counted. */
static const struct { uint32_t m; const char *name; } NV097_NAMES[] = {
    { 0x0000, "SET_OBJECT" },
    { 0x0100, "NO_OPERATION" },
    { 0x0104, "SET_WARNING_ENABLE" },
    { 0x0130, "SET_FLIP_READ" },
    { 0x0200, "SET_SURFACE_CLIP_HORIZONTAL" },
    { 0x0204, "SET_SURFACE_CLIP_VERTICAL" },
    { 0x0208, "SET_SURFACE_FORMAT" },
    { 0x020C, "SET_SURFACE_PITCH" },
    { 0x0210, "SET_SURFACE_COLOR_OFFSET" },
    { 0x0214, "SET_SURFACE_ZETA_OFFSET" },
    { 0x0300, "SET_ALPHA_TEST_ENABLE" },
    { 0x0304, "SET_BLEND_ENABLE" },
    { 0x030C, "SET_DEPTH_TEST_ENABLE" },
    { 0x0310, "SET_DITHER_ENABLE" },
    { 0x0314, "SET_LIGHTING_ENABLE" },
    { 0x033C, "SET_CULL_FACE_ENABLE" },
    { 0x0340, "SET_DEPTH_MASK" },
    { 0x0350, "SET_CLEAR_DEPTH_VALUE" },
    { 0x1D8C, "SET_CLEAR_DEPTH" },
    { 0x1D90, "SET_COLOR_CLEAR_VALUE" },
    { 0x1D94, "CLEAR_SURFACE" },
    { 0x1D6C, "SET_ZSTENCIL_CLEAR" },
    { 0x0B80, "SET_TRANSFORM_PROGRAM" },
    { 0x0B00, "SET_TRANSFORM_CONSTANT" },
    { 0x1720, "SET_VERTEX_DATA_ARRAY_OFFSET" },
    { 0x1760, "SET_VERTEX_DATA_ARRAY_FORMAT" },
    { 0x17FC, "SET_BEGIN_END" },
    { 0x1800, "ARRAY_ELEMENT16" },
    { 0x1808, "INLINE_ARRAY" },
    { 0x1810, "DRAW_ARRAYS" },
    { 0x1B00, "SET_TEXTURE_OFFSET" },
    { 0x1B04, "SET_TEXTURE_FORMAT" },
    { 0x1B08, "SET_TEXTURE_ADDRESS" },
    { 0x1B0C, "SET_TEXTURE_CONTROL0" },
    { 0x1B14, "SET_TEXTURE_IMAGE_RECT" },
    { 0x0FD8, "SET_COMBINER_*" },
    { 0x0000, NULL },
};

static const char *nv097_name(uint32_t m)
{
    int i;
    for (i = 0; NV097_NAMES[i].name; i++)
        if (NV097_NAMES[i].m == m)
            return NV097_NAMES[i].name;
    return "";
}

void nv2a_pb_scan_report(void)
{
    int i;

    if (s_exec_enabled > 0) {
        nv2a_pb_exec_report();
        fprintf(stderr, "[PB] %u pushbuffer calls followed\n", s_tot_calls);
    }
    if (!s_seen_count || !getenv("RECOMP_PB_SCAN"))
        return;
    fprintf(stderr, "[PB] %u segments, %u words, %u jumps, %u calls,"
                    " %u unrecognised -- %d distinct (subchannel, method)"
                    " pairs\n",
            s_tot_segments, s_tot_words, s_tot_jumps, s_tot_calls,
            s_tot_unknown, s_seen_count);
    for (i = 0; i < s_seen_count; i++)
        fprintf(stderr, "  [PB]   subch %u  method 0x%04X  x%-6u %s\n",
                s_seen[i].subch, s_seen[i].method, s_seen[i].count,
                nv097_name(s_seen[i].method));
    fflush(stderr);
}

/* Walk the pushbuffer the way the GPU's DMA engine does, from our own read
 * position up to the title's DMA_PUT.
 *
 * This used to scan only the bytes between the previous PUT and this one,
 * stop at the first jump, skip subroutine calls, and skip the whole segment
 * whenever PUT wrapped back to the start of the ring. Each of those drops
 * commands: XDK D3D sends state and vertex-program uploads through CALLs into
 * pre-built pushbuffers, and every wrap lost a segment. What survived drew
 * with stale state -- untransformed meshes run through the pre-transformed
 * pass-through program, garbled text.
 *
 * So: keep a GET of our own (physical, like the register), follow JUMP and
 * CALL/RETURN (NV2A has one level of subroutine), and stop when GET reaches
 * PUT. Addresses are physical; the contiguous window at XBOX_CONTIG_BASE is
 * the physical view.
 *
 * ponytail: if the walk desynchronises (a run of words that decode as
 * nothing), GET snaps to PUT rather than wandering through guest RAM. */
#define PB_PHYS_MASK 0x0FFFFFFCu
#define PB_RAM_BYTES (64u * 1024u * 1024u)   /* XBOX_CONTIG_SIZE */

/* A jump or call outside RAM means the walk is reading data as commands. */
/* The last command headers decoded, so a desync can be traced back to the
 * command that caused it. */
#define PB_HIST 8
static struct { uint32_t at, w; } s_hist[PB_HIST];
static unsigned s_hist_n;
/* The last jumps/calls/returns taken, and resyncs (w = 0xFFFFFFFF). */
static struct { uint32_t at, w, to; } s_flow[PB_HIST];
static unsigned s_flow_n;
static void pb_flow(uint32_t at, uint32_t w, uint32_t to)
{
    s_flow[s_flow_n % PB_HIST].at = at;
    s_flow[s_flow_n % PB_HIST].w = w;
    s_flow[s_flow_n % PB_HIST].to = to;
    s_flow_n++;
}

static int pb_bad_target(uint32_t w, uint32_t at, uint32_t target, uint32_t put)
{
    static unsigned total;
    unsigned k;
    if (target < PB_RAM_BYTES)
        return 0;
    if (++total <= 16 || total % 100 == 0) {
        fprintf(stderr, "  [PB] bad target #%u 0x%08X from word 0x%08X at 0x%08X"
                        " (PUT 0x%08X); resync\n", total, target, w, at, put);
        if (total <= 4)
            for (k = 0; k < PB_HIST && k < s_hist_n; k++) {
                unsigned i = (s_hist_n - 1 - k) % PB_HIST;
                fprintf(stderr, "  [PB]   earlier header 0x%08X at 0x%08X\n",
                        s_hist[i].w, s_hist[i].at);
            }
        if (total <= 4)
            for (k = 0; k < PB_HIST && k < s_flow_n; k++) {
                unsigned i = (s_flow_n - 1 - k) % PB_HIST;
                fprintf(stderr, "  [PB]   earlier flow 0x%08X at 0x%08X -> 0x%08X\n",
                        s_flow[i].w, s_flow[i].at, s_flow[i].to);
            }
    }
    return 1;
}
static uint32_t s_get = 0xFFFFFFFFu, s_ret;
static int      s_in_call;

/* A method whose parameters the walk has not all reached: the header said
 * `count` words and PUT came first. The DMA engine stalls at PUT and carries
 * on with the same method when PUT moves; reading the next word as a new
 * header instead would decode vertex data as commands. (D3D streams large
 * inline arrays across several kicks, so this is routine, not a desync.) */
static uint32_t s_pend_left, s_pend_i, s_pend_method, s_pend_subch;
static int      s_pend_noninc;

/* DMA_GET is the GPU's fetch pointer: nothing at or past it has been read, and
 * XDK D3D relies on exactly that. Its fence wait reads GET and, when the fence
 * is far enough on, patches a "wake me" software method into the pushbuffer at
 * the fence and sleeps on an event. If GET lagged the executor -- it used to
 * move only when the walk reached PUT -- D3D patched commands the executor had
 * already passed and slept for good. So under RECOMP_PB_EXEC the walk publishes
 * its position before every command (s_pub is the last value it wrote); a GET
 * that is not what it last wrote means the title moved it, and the walk
 * follows. */
extern volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset);
#define NV2A_USER_DMA_GET 0x800044u
static uint32_t s_pub = 0xFFFFFFFFu;

/* The title moved GET itself: XDK D3D resets the ring by writing DMA_PUT and
 * DMA_GET together (CDevice::KickOff's restart path). Walking on from the old
 * position would read whatever lies between as commands. */
void nv2a_pb_resync(uint32_t get_phys)
{
    if (!pb_walk_enabled())
        return;
    pb_flow(s_get, 0xFFFFFFFFu, get_phys & PB_PHYS_MASK);
    s_get = get_phys & PB_PHYS_MASK;
    s_in_call = 0;
    s_pend_left = 0;
    s_pub = s_get;
}

/* Feed the parameters of the method in progress to the survey and the
 * executor, up to PUT. */
static void pb_consume(const uint8_t *mem, uint32_t put, uint32_t *words)
{
    while (s_pend_left && s_get < PB_RAM_BYTES && s_get != put) {
        uint32_t m = s_pend_noninc ? s_pend_method : s_pend_method + s_pend_i * 4;
        uint32_t param = *(const uint32_t *)(mem + (0x80000000u | s_get));

        note(s_pend_subch, m);
        /* Same walk, two consumers: the survey counts, the executor
         * acts. Keeping them on one decode means they can never
         * disagree about what the stream said. */
        if (s_exec_enabled)
            nv2a_pb_exec_method(s_pend_subch, m, param);
        s_get = (s_get + 4) & PB_PHYS_MASK;
        s_pend_i++;
        s_pend_left--;
        (*words)++;
    }
}

void nv2a_pb_scan(uint32_t put_phys)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t put = put_phys & PB_PHYS_MASK;
    volatile uint32_t *get_reg = NULL;
    uint32_t words = 0, jumps = 0, unknown = 0;

    if (!pb_walk_enabled())
        return;
    if (s_get == 0xFFFFFFFFu) {               /* never resynced: start at PUT */
        s_get = put, s_pend_left = 0;
        return;
    }
    if (s_exec_enabled)
        get_reg = xbox_Nv2aRegPtr(NV2A_USER_DMA_GET);

    pb_consume(mem, put, &words);             /* the method PUT stalled inside */

    while (s_get != put) {
        uint32_t at, target;
        uint32_t w;

        if (get_reg) {
            uint32_t g = *get_reg;

            if (g != s_pub) {                 /* the title moved GET mid-walk */
                pb_flow(s_get, 0xFFFFFFFFu, g & PB_PHYS_MASK);
                s_get = g & PB_PHYS_MASK;
                s_in_call = 0;
                s_pend_left = 0;
                s_pub = s_get;
                continue;
            }
            *get_reg = s_get;
            s_pub = s_get;
        }
        at = s_get;

        /* Keep answering kickoffs while executing; see xbox_Nv2aAckBusyBits. */
        if (s_exec_enabled && (++s_ack_tick & s_ack_mask) == 0)
            xbox_Nv2aAckBusyBits();

        if (s_get >= PB_RAM_BYTES) {
            s_get = put, s_pend_left = 0;
            break;
        }
        w = *(const uint32_t *)(mem + (0x80000000u | s_get));
        s_hist[s_hist_n % PB_HIST].at = at;
        s_hist[s_hist_n % PB_HIST].w = w;
        s_hist_n++;

        if (++words > 0x400000u || unknown > 64) {
            s_get = put, s_pend_left = 0;                      /* lost sync, or runaway */
            break;
        }
        s_get = (s_get + 4) & PB_PHYS_MASK;

        if ((w & 0xE0000003u) == 0x20000000u) {         /* old-style jump */
            target = w & 0x1FFFFFFCu;
            if (pb_bad_target(w, at, target, put)) { s_get = put, s_pend_left = 0; break; }
            pb_flow(at, w, target);
            s_get = target;
            jumps++;
            continue;
        }
        if ((w & 3u) == 1u) {                            /* jump */
            target = w & 0xFFFFFFFCu;
            if (pb_bad_target(w, at, target, put)) { s_get = put, s_pend_left = 0; break; }
            pb_flow(at, w, target);
            s_get = target;
            jumps++;
            continue;
        }
        if ((w & 3u) == 2u) {                            /* call */
            target = w & 0xFFFFFFFCu;
            if (pb_bad_target(w, at, target, put)) { s_get = put, s_pend_left = 0; break; }
            if (s_in_call) {
                /* One level only: the hardware raises a CALL error and stops
                 * the pusher. Taking it would overwrite the return address
                 * and send the first RETURN to the wrong place. */
                s_in_call = 0;
                s_get = put, s_pend_left = 0;
                break;
            }
            s_ret = s_get;
            s_in_call = 1;
            s_tot_calls++;
            pb_flow(at, w, target);
            s_get = target;
            continue;
        }
        if ((w & 0xFFFF0003u) == 0x00020000u) {          /* return */
            if (s_in_call) {
                pb_flow(at, w, s_ret);
                s_get = s_ret;
                s_in_call = 0;
            }
            continue;
        }
        if ((w & 0x00030003u) == 0u) {
            uint32_t count  = (w >> 18) & 0x7FFu;
            uint32_t subch  = (w >> 13) & 7u;
            uint32_t method =  w & 0x1FFCu;
            int noninc = (w & 0xE0000000u) == 0x40000000u;

            /* PUT bounds the parameters too. A count that runs past it must
             * not carry s_get beyond PUT (the outer loop would then replay a
             * lap of stale ring contents), nor be forgotten (the leftover
             * parameters would be read as headers): the method stays pending
             * and the next walk finishes it. */
            s_pend_left = count;
            s_pend_i = 0;
            s_pend_method = method;
            s_pend_subch = subch;
            s_pend_noninc = noninc;
            pb_consume(mem, put, &words);
            unknown = 0;
            continue;
        }
        unknown++;
    }

    if (get_reg) {                            /* what the poll thread writes too */
        *get_reg = s_get;
        s_pub = s_get;
    }
    s_tot_words += words;
    s_tot_unknown += unknown;
    s_tot_jumps += jumps;
    s_tot_segments++;
}

/* RECOMP_PB_WALK=dma selects the DMA-engine walk above. The default is the
 * segment scan below, which is what the executor did before the walk existed.
 *
 * The walk is the more faithful of the two: it follows JUMPs and ring wraps,
 * keeps its own GET, publishes it before every command, and answers kickoffs
 * mid-walk. That fidelity changes what XDK D3D does. Told exactly how far the
 * GPU has got, D3D's fence code takes its "GPU far behind" path: it patches a
 * software method (NO_OPERATION with a value) into the pushbuffer and sleeps
 * on an event that the GPU's interrupt for that method signals. A runtime that
 * does not deliver that interrupt leaves the title asleep for good -- a title
 * that ran under the segment scan froze on the first screen whose pushbuffer
 * made the walk long enough. So the walk is opt-in, for projects that deliver
 * the GPU's software-method interrupt. */
int nv2a_pb_dma_walk(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_PB_WALK");
        on = e && !strcmp(e, "dma");
    }
    return on;
}

/* Walk commands from va to end_va. Returns the words
 * consumed.
 *
 * A CALL runs a subroutine -- a pushbuffer somewhere else in memory -- up to
 * its RETURN, then carries on after the CALL. RenderWare on the Xbox compiles
 * static geometry into pushbuffers and draws it with a CALL, so skipping CALLs
 * loses whole meshes. The NV2A has one level of subroutine, so a CALL inside a
 * call is not followed. nv2a_pb_scan_follow walks the ring itself and calls
 * this for each CALL; a JUMP outside a call ends the walk here.
 * Addresses are physical, like PUT's, and reach guest memory through the
 * contiguous window. */

/* A method whose parameters ran past the end of the last top-level segment.
 *
 * PUT can stop inside a method: D3D kicks off with a long parameter list
 * (inline vertices, a constant upload) only partly before it. The segment
 * used to end there and forget the method, so the next segment -- starting
 * at that PUT -- read the rest of the parameters as command headers: vertex
 * data run as methods, state lost from the batch it belonged to, and, at the
 * end of a lap, the ring's closing JUMP never recognised. Ghost Recon 2 then
 * restarted the ring on stale words and presented the same two frames over
 * and over. The DMA walk keeps the method pending (s_pend_*); so does this. */
typedef struct {
    uint32_t left, method, subch;
    int noninc;
} PbPending;
static PbPending s_seg_pend;

static int s_dry;                   /* decoding only: execute nothing */

static void pb_seg_params(const uint8_t *mem, uint32_t *va, uint32_t end_va,
                          uint32_t *words)
{
    /* Runs of parameters in one step where the words themselves do not
     * matter (the dry pass) or are plain data (indices, inline vertices):
     * word by word, the walk was most of the executor thread's time and the
     * frame rate's ceiling in Ghost Recon 2 play. */
    if (s_seg_pend.left && *va != end_va) {
        uint32_t off = *va & 0x0FFFFFFFu;
        uint32_t room = off + 4 <= PB_RAM_BYTES ? (PB_RAM_BYTES - off) / 4 : 0;
        uint32_t n = s_seg_pend.left;
        if (n > room)
            n = room;
        if (end_va > *va && (end_va - *va) / 4 < n)
            n = (end_va - *va) / 4;
        if (n && s_dry) {
            s_seg_pend.left -= n;
            if (!s_seg_pend.noninc)
                s_seg_pend.method += 4 * n;
            *va += 4 * n;
            *words += n;
        } else if (n && s_exec_enabled && s_seg_pend.noninc && s_seg_pend.subch == 0
                   && nv2a_pb_exec_bulk(s_seg_pend.method,
                                        (const uint32_t *)(mem + *va), n)) {
            note_n(0, s_seg_pend.method, n);
            if (((s_ack_tick + n) ^ s_ack_tick) & ~s_ack_mask)
                xbox_Nv2aAckBusyBits();
            s_ack_tick += n;
            s_seg_pend.left -= n;
            *va += 4 * n;
            *words += n;
        } else if (n && s_exec_enabled && !s_seg_pend.noninc && s_seg_pend.subch == 0
                   && nv2a_pb_exec_bulk_inc(s_seg_pend.method,
                                            (const uint32_t *)(mem + *va), n)) {
            uint32_t k;
            for (k = 0; k < n; k++)
                note(0, s_seg_pend.method + 4 * k);
            if (((s_ack_tick + n) ^ s_ack_tick) & ~s_ack_mask)
                xbox_Nv2aAckBusyBits();
            s_ack_tick += n;
            s_seg_pend.method += 4 * n;
            s_seg_pend.left -= n;
            *va += 4 * n;
            *words += n;
        }
    }
    while (s_seg_pend.left && *va != end_va
           && (*va & 0x0FFFFFFFu) + 4 <= PB_RAM_BYTES) {
        /* Answer kickoffs mid-walk; see pb_follow. */
        if (!s_dry && s_exec_enabled && (++s_ack_tick & s_ack_mask) == 0)
            xbox_Nv2aAckBusyBits();
        if (!s_dry)
            note(s_seg_pend.subch, s_seg_pend.method);
        if (s_exec_enabled && !s_dry)
            nv2a_pb_exec_method(s_seg_pend.subch, s_seg_pend.method,
                                *(const uint32_t *)(mem + *va));
        if (!s_seg_pend.noninc)
            s_seg_pend.method += 4;
        s_seg_pend.left--;
        *va += 4;
        (*words)++;
    }
}

/* The last top-level words decoded as headers, for nv2a_pb_segment_dump:
 * when a lap's closing JUMP is not where the ring says it should be, this
 * is what the walk took for commands on its way there. */
#define PB_SEG_HIST 24
static struct { uint32_t at, w; } s_seg_hist[PB_SEG_HIST];
static unsigned s_seg_hist_n;

void nv2a_pb_segment_dump(void)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    unsigned k, n = s_seg_hist_n < PB_SEG_HIST ? s_seg_hist_n : PB_SEG_HIST;

    fprintf(stderr, "  [PB]   pending at segment start: %u params of %u:%04X%s\n",
            s_seg_pend.left, s_seg_pend.subch, s_seg_pend.method,
            s_seg_pend.noninc ? " (non-inc)" : "");
    for (k = 0; k < n; k++) {
        unsigned i = (s_seg_hist_n - n + k) % PB_SEG_HIST;
        uint32_t at = s_seg_hist[i].at, w = s_seg_hist[i].w;
        fprintf(stderr, "  [PB]   header %08X at %08X: subch %u method %04X"
                " count %u%s\n", w, at & 0x0FFFFFFFu, (w >> 13) & 7u,
                w & 0x1FFCu, (w >> 18) & 0x7FFu,
                (w & 3u) == 1u ? " = JUMP" : (w & 3u) == 2u ? " = CALL" : "");
    }
    if (n) {
        uint32_t at = s_seg_hist[(s_seg_hist_n - 1) % PB_SEG_HIST].at;
        fprintf(stderr, "  [PB]   words before it:");
        for (k = 16; k > 0; k--)
            fprintf(stderr, " %08X", *(const uint32_t *)(mem + at - 4 * k));
        fprintf(stderr, "\n");
    }
}

static uint32_t pb_walk_segment(uint32_t va, uint32_t end_va, int in_call,
                                uint32_t *jumps, uint32_t *unknown)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t words = 0;

    if (!in_call)
        pb_seg_params(mem, &va, end_va, &words);  /* the previous segment's */

    if (end_va > (0x80000000u | PB_RAM_BYTES))
        end_va = 0x80000000u | PB_RAM_BYTES;
    while (va < end_va && words < 0x100000u) {
        uint32_t w = *(const uint32_t *)(mem + va);
        if (!in_call) {
            s_seg_hist[s_seg_hist_n % PB_SEG_HIST].at = va;
            s_seg_hist[s_seg_hist_n % PB_SEG_HIST].w = w;
            s_seg_hist_n++;
        }
        va += 4;
        words++;

        if ((w & 3u) == 1u || (w & 0xE0000003u) == 0x20000000u) {
            uint32_t target = (w & 3u) == 1u ? (w & 0xFFFFFFFCu)
                                             : (w & 0x1FFFFFFCu);
            (*jumps)++;
            if (!in_call)
                break;                        /* the ring: a jump ends it */
            if ((target & 0x0FFFFFFFu) >= PB_RAM_BYTES)
                break;
            va = 0x80000000u | (target & 0x0FFFFFFFu);
            end_va = va + 0x400000u;
            if (end_va > (0x80000000u | PB_RAM_BYTES))
                end_va = 0x80000000u | PB_RAM_BYTES;
            continue;
        }
        if ((w & 0xFFFF0003u) == 0x00020000u) {  /* RETURN */
            if (in_call)
                break;
            continue;
        }
        if ((w & 3u) == 2u) {                    /* CALL */
            if (!in_call) {
                uint32_t sub = 0x80000000u | ((w & 0xFFFFFFFCu) & 0x0FFFFFFFu);
                s_tot_calls++;
                words += pb_walk_segment(sub, sub + 0x400000u, 1, jumps, unknown);
            }
            continue;
        }
        if ((w & 0x00030003u) == 0u) {
            uint32_t count  = (w >> 18) & 0x7FFu;
            uint32_t subch  = (w >> 13) & 7u;
            uint32_t method =  w & 0x1FFCu;
            int noninc = (w & 0xE0000000u) == 0x40000000u;

            if (!in_call) {
                s_seg_pend.left = count;
                s_seg_pend.method = method;
                s_seg_pend.subch = subch;
                s_seg_pend.noninc = noninc;
                pb_seg_params(mem, &va, end_va, &words);
                continue;
            }
            for (uint32_t i = 0; i < count && va < end_va; i++) {
                uint32_t m = noninc ? method : method + i * 4;
                if (!s_dry)
                    note(subch, m);
                if (s_exec_enabled && !s_dry)
                    nv2a_pb_exec_method(subch, m,
                                        *(const uint32_t *)(mem + va));
                va += 4;
                words++;
            }
            continue;
        }
        (*unknown)++;
    }
    return words;
}

/* The default walk: from the previous PUT to this one, the way the GPU's
 * DMA engine goes -- following every JUMP -- but without publishing GET
 * (see nv2a_pb_dma_walk for why that is opt-in).
 *
 * The segment scan below stops at the first JUMP, taking it for the ring's
 * wrap. It is not always that: XDK D3D runs a recorded pushbuffer by
 * JUMPing to it, and the recorded buffer JUMPs back into the ring when it
 * is done. Ghost Recon 2 draws its characters and vehicles that way, so
 * stopping lost them, lost the rest of the segment after the jump back, and
 * at the end of a lap mistook the jump out for the ring's closing JUMP --
 * the ring then restarted on stale words and replayed old frames. Following
 * the JUMPs, the walk wraps the ring the same way it leaves it: by going
 * where the JUMP says, until it is at PUT. */
/* The ring's extent as the PUTs seen in it say (physical), for telling a
 * JUMP that stays in the ring from one into a recorded pushbuffer. */
static uint32_t s_ring_lo, s_ring_hi;

static int pb_in_ring(uint32_t phys)
{
    phys &= PB_PHYS_MASK;
    return s_ring_hi && phys + 0x10000u >= s_ring_lo && phys <= s_ring_hi + 0x10000u;
}

/* The JUMPs a dry walk took, printed when it finds no path: which one led
 * away from PUT (a ring JUMP taken for a recorded buffer, a return to the
 * wrong call site). */
#define PB_JUMP_LOG 48
typedef struct { uint32_t at, target; uint8_t in_rec, in_ring; } PbJump;
static PbJump s_jump_log[PB_JUMP_LOG];

static int pb_follow(uint32_t from_phys, uint32_t put_phys, int dry)
{
    /* A recorded pushbuffer (D3D RunPushBuffer) is entered by a JUMP from
     * the ring and left by a JUMP back to just after it. D3D patches that
     * return JUMP every time the buffer runs, and this walk runs behind the
     * title: by the time it reads the buffer, the return names the latest
     * call site, not the one being walked. Followed as written it skipped
     * everything between the two calls -- the models the title draws that
     * way never appeared, fence releases were lost -- or, when the latest
     * call site was the one ahead, went round between them for ever. So
     * remember where the ring was left and go back there. */
    int in_rec = 0;
    uint32_t rec_target = 0;              /* the recorded buffer jumped to */
    uint32_t ret_va = 0;
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t va  = 0x80000000u | (from_phys & PB_PHYS_MASK);
    uint32_t end = 0x80000000u | (put_phys  & PB_PHYS_MASK);
    uint32_t start = va;
    uint32_t words = 0, jumps = 0, unknown = 0;

    s_dry = dry;
    pb_seg_params(mem, &va, end, &words);        /* the method PUT split */

    while (va != end && words < 0x400000u && unknown <= 64) {
        /* Keep answering kickoffs while executing (xbox_Nv2aAckBusyBits):
         * this thread is also the one that clears the bit every D3D kickoff
         * spins on, and running a pushbuffer -- rasterising a frame on the
         * CPU -- takes long enough that a kickoff waiting for the whole walk
         * waits that long. Ghost Recon 2's movie player kicks off once per
         * decoded frame and spent 99 % of its time in that spin (sampled,
         * sub_0042F790): the cinematic ran at a frame a second and its audio
         * starved. The DMA walk always did this; the follow walk forgot. */
        if (!dry && s_exec_enabled && (++s_ack_tick & s_ack_mask) == 0)
            xbox_Nv2aAckBusyBits();
        if (va == start && words) {
            va = start + 4;                       /* round the ring: not a */
            break;                                /* way to PUT (see below) */
        }
        if ((va & 0x0FFFFFFFu) + 4 > PB_RAM_BYTES)
            break;                                /* walked off guest RAM */
        uint32_t w = *(const uint32_t *)(mem + va);
        s_seg_hist[s_seg_hist_n % PB_SEG_HIST].at = va;
        s_seg_hist[s_seg_hist_n % PB_SEG_HIST].w = w;
        s_seg_hist_n++;
        va += 4;
        words++;

        if ((w & 3u) == 1u || (w & 0xE0000003u) == 0x20000000u) {
            uint32_t target = (w & 3u) == 1u ? (w & 0xFFFFFFFCu)
                                             : (w & 0x1FFFFFFCu);
            if ((target & 0x0FFFFFFFu) >= PB_RAM_BYTES)
                break;                            /* data read as a JUMP */
            if (dry && jumps < PB_JUMP_LOG)       /* for a lost path's report */
                s_jump_log[jumps] = (PbJump){ (va - 4) & PB_PHYS_MASK,
                                              target & PB_PHYS_MASK, (uint8_t)in_rec,
                                              (uint8_t)pb_in_ring(target) };
            jumps++;
            if (!in_rec && !pb_in_ring(target)) {
                in_rec = 1;                       /* into a recorded buffer */
                ret_va = va;                      /* the word after the JUMP */
                rec_target = target & PB_PHYS_MASK;
            } else if (in_rec && pb_in_ring(target)) {
                /* Back to where we left. D3D jumps out in one of two ways:
                 *
                 *   JUMP buf | ret, x, 0x80000000|buf_end, 0, 0x80000000|buf | ...
                 *   JUMP buf | commands ...
                 *
                 * The first leaves a five-word gap whose first word is the
                 * return address and whose fifth repeats the buffer's own
                 * address; the second carries on right after the JUMP.
                 * Telling them apart by "the next word looks like a nearby
                 * address" mistook a method header of the second kind for a
                 * return address (Ghost Recon 2's buffer 0x13B5FE4: header
                 * 0x0004A310 after a JUMP at 0x4A2D4): 56 bytes of commands
                 * skipped, a fence release among them, or a parameter 1 read
                 * as a JUMP to 0 -- the brief flashes and 2 s fence stalls.
                 * The fifth word says which it is. */
                uint32_t after = *(const uint32_t *)(mem + ret_va);
                uint32_t fifth = *(const uint32_t *)(mem + ret_va + 16);
                uint32_t ret_phys = ret_va & PB_PHYS_MASK;
                static unsigned n_skip, n_plain;
                in_rec = 0;
                if (fifth == (0x80000000u | rec_target)
                    && (after & 3u) == 0 && after > ret_phys && after - ret_phys <= 64) {
                    va = 0x80000000u | after;
                    n_skip++;
                } else {
                    va = ret_va;
                    n_plain++;
                }
                if (!dry && (n_skip + n_plain) % 20000 == 1)
                    fprintf(stderr, "  [PB] recorded-buffer returns: %u past D3D's"
                            " gap, %u right after the JUMP\n", n_skip, n_plain);
                continue;
            }
            va = 0x80000000u | (target & PB_PHYS_MASK);
            continue;
        }
        if ((w & 0xFFFF0003u) == 0x00020000u)     /* RETURN outside a call */
            continue;
        if ((w & 3u) == 2u) {                     /* CALL */
            uint32_t sub = 0x80000000u | (w & PB_PHYS_MASK);
            if ((w & PB_PHYS_MASK) >= PB_RAM_BYTES)
                break;                            /* data read as a CALL */
            s_tot_calls++;
            words += pb_walk_segment(sub, sub + 0x400000u, 1, &jumps, &unknown);
            continue;
        }
        if ((w & 0x00030003u) == 0u) {
            s_seg_pend.left   = (w >> 18) & 0x7FFu;
            s_seg_pend.method =  w & 0x1FFCu;
            s_seg_pend.subch  = (w >> 13) & 7u;
            s_seg_pend.noninc = (w & 0xE0000000u) == 0x40000000u;
            pb_seg_params(mem, &va, end, &words);
            unknown = 0;
            continue;
        }
        unknown++;
    }
    s_dry = 0;
    if (va != end) {
        if (dry) {
            static unsigned said_dry;
            if (said_dry++ < 12) {
                fprintf(stderr, "  [PB] dry walk %08X -> %08X stopped at %08X"
                        " (%u words, %u jumps, %u unknown)%s\n", from_phys, put_phys,
                        va & 0x0FFFFFFFu, words, jumps, unknown,
                        va == start + 4 ? " -- came round to the start" : "");
                fprintf(stderr, "  [PB]   ring %08X..%08X; jumps (at->target rec/ring):",
                        s_ring_lo, s_ring_hi);
                for (uint32_t j = 0; j < jumps && j < PB_JUMP_LOG; j++)
                    fprintf(stderr, " %X->%X%s%s", s_jump_log[j].at, s_jump_log[j].target,
                            s_jump_log[j].in_rec ? "r" : "", s_jump_log[j].in_ring ? "" : "*");
                fprintf(stderr, "\n");
                /* The ring around each JUMP out to a recorded buffer: the
                 * words D3D leaves after it (its gap and return address) are
                 * what the return logic reads. */
                for (uint32_t j = 0; j < jumps && j < PB_JUMP_LOG; j++) {
                    uint32_t a = s_jump_log[j].at, k;
                    if (s_jump_log[j].in_rec || s_jump_log[j].in_ring)
                        continue;
                    fprintf(stderr, "  [PB]   at %08X:", a);
                    for (k = 0; k < 10; k++)
                        fprintf(stderr, " %08X",
                                *(const uint32_t *)(mem + (0x80000000u | (a + k * 4))));
                    fprintf(stderr, "\n");
                }
                nv2a_pb_segment_dump();
            }
        }
        if (!dry) {
            static unsigned said;
            if (said++ < 4) {
                fprintf(stderr, "  [PB] walk %08X -> %08X lost sync at %08X"
                        " (%u words, %u jumps)\n", from_phys, put_phys,
                        va & 0x0FFFFFFFu, words, jumps);
                nv2a_pb_segment_dump();
            }
        }
        return 0;
    }
    if (!dry) {
        s_tot_words += words;
        s_tot_unknown += unknown;
        s_tot_jumps += jumps;
        s_tot_segments++;
    }
    return 1;
}

/* Walk only a path that is known to arrive. A first pass decodes without
 * executing; only if it reaches PUT does the second run the commands.
 *
 * There is not always a path. A title that makes a second device (Ghost
 * Recon 2: one for the boot splash, one for the game) moves PUT into a new
 * ring that nothing in the old one jumps to; walked anyway, the old ring
 * went round and round -- 4 M words, 4000 jumps -- running stale device
 * init and frames, after which the title sat on its title screen and START
 * did nothing. Without a path, the walk drops what is pending and starts
 * again at this PUT, which D3D only ever sets at a command boundary. */
/* The thread that executes the pushbuffer, for a host-side sampling
 * profiler (the game's RECOMP_SAMPLE_THREAD=pb): a duplicated handle, set on
 * the first walk. */
void *g_pb_exec_thread;

static void pb_scan_follow(uint32_t from_phys, uint32_t put_phys)
{
    PbPending saved;

#ifdef _WIN32
    if (!g_pb_exec_thread)
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                        GetCurrentProcess(), (HANDLE *)&g_pb_exec_thread,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT
                        | THREAD_QUERY_INFORMATION, FALSE, 0);
#endif

    if (!pb_walk_enabled() || (from_phys & PB_PHYS_MASK) == (put_phys & PB_PHYS_MASK))
        return;
    /* Learn the ring's extent from the PUTs (the extent before this PUT is
     * kept for telling a new ring from a lost path, below). */
    uint32_t old_lo = s_ring_lo, old_hi = s_ring_hi;
    {
        uint32_t f = from_phys & PB_PHYS_MASK, p = put_phys & PB_PHYS_MASK;
        if (!s_ring_hi) { s_ring_lo = s_ring_hi = f; }
        if (f < s_ring_lo) s_ring_lo = f;
        if (p < s_ring_lo) s_ring_lo = p;
        if (f > s_ring_hi) s_ring_hi = f;
        if (p > s_ring_hi) s_ring_hi = p;
    }
    saved = s_seg_pend;
    if (!pb_follow(from_phys, put_phys, 1)) {
        static unsigned said;
        if (said++ < 8)
            fprintf(stderr, "  [PB] no path %08X -> %08X: ring changed, not"
                    " walked\n", from_phys, put_phys);
        s_seg_pend.left = 0;
        /* A new ring (a second device): learn its extent afresh -- but only
         * when PUT really is somewhere else. Mid-mission Ghost Recon 2 lost
         * a path inside its own ring (0x34958 -> 0x1375EC, came round to the
         * start), and shrinking the extent to one point made every JUMP of
         * the ring look like a JUMP into a recorded buffer: eight segments
         * dropped in a row, their draws (a flash of missing or wrong models)
         * and a fence release with them (the title then waited 2 s). */
        {
            uint32_t p = put_phys & PB_PHYS_MASK;
            if (!old_hi || p + 0x100000u < old_lo || p > old_hi + 0x100000u)
                s_ring_lo = s_ring_hi = p;    /* elsewhere: a new ring */
            else {
                s_ring_lo = old_lo;           /* same ring: keep what we knew */
                s_ring_hi = old_hi;
            }
        }
        return;
    }
    s_seg_pend = saved;
    pb_follow(from_phys, put_phys, 0);
}

/* Time spent executing (performance counter ticks), for a back end to report
 * how busy this thread really is: its idle loop spins, so thread CPU time
 * does not tell work from waiting. */
static volatile LONGLONG s_busy_qpc;

long long nv2a_pb_busy_ticks(void)
{
    return s_busy_qpc;
}

void nv2a_pb_scan_follow(uint32_t from_phys, uint32_t put_phys)
{
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    pb_scan_follow(from_phys, put_phys);
    QueryPerformanceCounter(&b);
    s_busy_qpc += b.QuadPart - a.QuadPart;
}
