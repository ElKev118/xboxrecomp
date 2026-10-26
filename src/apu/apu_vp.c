/*
 * MCPX APU Voice Processor - Standalone extraction from xemu
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "apu_state.h"
#include "apu.h"
#include "fpconv.h"
#include <stdarg.h>

static void stream_trace(MCPXAPUState *d, uint16_t v, const char *fmt, ...);

/* #define DEBUG_MCPX */

#ifdef DEBUG_MCPX
#define DPRINTF(fmt, ...) fprintf(stderr, fmt, ## __VA_ARGS__)
#else
#define DPRINTF(fmt, ...) do { } while (0)
#endif

/* ============================================================
 * Voice list register table
 * ============================================================ */

static const struct {
    hwaddr top, current, next;
} voice_list_regs[] = {
    { NV_PAPU_TVL2D, NV_PAPU_CVL2D, NV_PAPU_NVL2D }, /* 2D */
    { NV_PAPU_TVL3D, NV_PAPU_CVL3D, NV_PAPU_NVL3D }, /* 3D */
    { NV_PAPU_TVLMP, NV_PAPU_CVLMP, NV_PAPU_NVLMP }, /* MP */
};

/* ============================================================
 * Notify status helper
 * ============================================================ */

static void set_notify_status(MCPXAPUState *d, uint32_t v, int notifier,
                              int status)
{
    hwaddr notify_offset = d->regs[NV_PAPU_FENADDR];
    notify_offset += 16 * (MCPX_HW_NOTIFIER_BASE_OFFSET +
                           v * MCPX_HW_NOTIFIER_COUNT + notifier);
    notify_offset += 15;

    stb_phys(address_space_memory, notify_offset, (uint8_t)status);
    stb_phys(address_space_memory, notify_offset - 1, 1);

    qatomic_or(&d->regs[NV_PAPU_ISTS],
               NV_PAPU_ISTS_FEVINTSTS | NV_PAPU_ISTS_FENINTSTS);
    apu_irq_stats.notifies++;
    d->set_irq = true;
}

/* ============================================================
 * Filter helpers
 * ============================================================ */

static void voice_reset_filters(MCPXAPUState *d, uint16_t v)
{
    assert(v < MCPX_HW_MAX_VOICES);
    memset(&d->vp.filters[v].svf, 0, sizeof(d->vp.filters[v].svf));
    hrtf_filter_clear_history(&d->vp.filters[v].hrtf);
    if (d->vp.filters[v].resampler) {
        src_reset(d->vp.filters[v].resampler);
    }
    d->vp.filters[v].rs_have = 0;
    d->vp.filters[v].rs_pos = 0.0f;
}

static bool voice_should_mute(uint16_t v)
{
    bool m = (g_dbg_voice_monitor >= 0) && (v != g_dbg_voice_monitor);
    return m || mcpx_apu_debug_is_muted(v);
}

/* ============================================================
 * Utility functions
 * ============================================================ */

static float clampf(float v, float mn, float mx)
{
    if (v < mn) return mn;
    if (v > mx) return mx;
    return v;
}

static float attenuate(uint16_t vol)
{
    vol &= 0xFFF;
    return (vol == 0xFFF) ? 0.0f : powf(10.0f, vol / (64.0f * -20.0f));
}

/* ============================================================
 * Voice register accessors (read/write voice struct in RAM)
 * ============================================================ */

static uint32_t voice_get_mask(MCPXAPUState *d, uint16_t voice_handle,
                               hwaddr offset, uint32_t mask)
{
    hwaddr voice = d->regs[NV_PAPU_VPVADDR] + voice_handle * NV_PAVS_SIZE;
    return (ldl_le_phys(address_space_memory, voice + offset) & mask) >>
           ctz32(mask);
}

static void voice_set_mask(MCPXAPUState *d, uint16_t voice_handle,
                           hwaddr offset, uint32_t mask, uint32_t val)
{
    hwaddr voice = d->regs[NV_PAPU_VPVADDR] + voice_handle * NV_PAVS_SIZE;
    uint32_t v = ldl_le_phys(address_space_memory, voice + offset) & ~mask;
    stl_le_phys(address_space_memory, voice + offset,
                v | ((val << ctz32(mask)) & mask));
}

/* ============================================================
 * Voice off / lock
 * ============================================================ */

static void voice_off(MCPXAPUState *d, uint16_t v)
{
    stream_trace(d, v, "voice_off");
    {
        /* RECOMP_APU_OFF_TRACE: every voice turned off, buffer or stream,
         * and where its notifier lands -- a driver that waits for a stop
         * to be acknowledged waits on that byte. */
        static int on = -1;
        if (on < 0)
            on = apu_env_on("RECOMP_APU_OFF_TRACE");
        if (on > 0) {
            extern unsigned long apu_vp_frames(void);
            fprintf(stderr, "[APU-OFF] t=%lu f=%lu v=%u stream=%d active=%d fenaddr=%08X\n",
                    (unsigned long)GetTickCount(), apu_vp_frames(), (unsigned)v,
                    voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                   NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0,
                    voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                   NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE) != 0,
                    (unsigned)d->regs[NV_PAPU_FENADDR]);
        }
    }
    voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                   NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 0);

    bool stream = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0;
    int notifier = MCPX_HW_NOTIFIER_SSLA_DONE;
    if (stream) {
        apu_irq_stats.stream_offs++;
        assert(v < MCPX_HW_MAX_VOICES);
        assert(d->vp.ssl[v].ssl_index <= 1);
        notifier += d->vp.ssl[v].ssl_index;
    }
    set_notify_status(d, v, notifier, NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
}

static void voice_lock(MCPXAPUState *d, uint16_t v, bool lock)
{
    assert(v < MCPX_HW_MAX_VOICES);
    qemu_mutex_lock(&d->lock);

    uint64_t mask = 1ULL << (v % 64);
    if (lock) {
        d->vp.voice_locked[v / 64] |= mask;
    } else {
        d->vp.voice_locked[v / 64] &= ~mask;
    }

    qemu_cond_signal(&d->cond);
    qemu_mutex_unlock(&d->lock);
}

static bool is_voice_locked(MCPXAPUState *d, uint16_t v)
{
    assert(v < MCPX_HW_MAX_VOICES);
    uint64_t mask = 1ULL << (v % 64);
    return (qatomic_read(&d->vp.voice_locked[v / 64]) & mask) != 0;
}

/* ============================================================
 * Stream voice event trace (RECOMP_APU_STREAM_TRACE)
 *
 * A title that streams audio (a movie player, streamed music) queues
 * packets through the voice's two SSLs and waits for each SSL's "done"
 * notifier before queueing more. When playback stalls, this log tells
 * which side stopped: the title (no further SET_VOICE_SSL) or the VP
 * (no further SSL done, or the voice ran dry / went idle). Budgeted so
 * a long run cannot flood the log.
 * ============================================================ */

static int s_strace = -1;
static int s_strace_budget = 4000;
static uint8_t s_strace_dry[MCPX_HW_MAX_VOICES]; /* dry logged since last SSL set */
static unsigned long s_vp_frames;   /* VP frames run: 1 frame = 32 samples at 48 kHz */
/* The VP frame count, as a clock other logs can share with this one. */
unsigned long apu_vp_frames(void) { return s_vp_frames; }

/* The flat stereo mix (RECOMP_APU_MIXDOWN=vp), built per frame by
 * voice_process and read by the DSP stage's downmix. */
float g_vp_flat[NUM_SAMPLES_PER_FRAME][2];

int apu_vp_flat_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_MIXDOWN");
        on = e && !strcmp(e, "vp");
    }
    return on;
}

/* The VP's HRTF on the 3D voices, RECOMP_APU_HRTF: 0 off, 1 all of them (as
 * xemu), "itd" the interaural delay alone, "dense" (default) only the voices
 * whose sample is playing on APU_DENSE_VOICES voices or more at once.
 * Ghost Recon 2's rain is ~60 voices playing one drop sample a few ms
 * apart; with nothing to set them apart the copies sum into a comb filter, a
 * metallic ring over the rain, and the HRIR (not the delay alone) breaks it
 * up. On every voice it also dulls and quietens speech and shots, which
 * sounded better dry; "dense" keeps them dry. */
#define APU_HRTF_OFF   0
#define APU_HRTF_FULL  1
#define APU_HRTF_ITD   2
#define APU_HRTF_DENSE 3
#define APU_DENSE_VOICES 8
static int apu_hrtf_mode(void)
{
    static int m = -1;
    if (m < 0) {
        const char *e = getenv("RECOMP_APU_HRTF");
        m = APU_HRTF_DENSE;
        if (e && !strcmp(e, "0")) m = APU_HRTF_OFF;
        else if (e && !strcmp(e, "1")) m = APU_HRTF_FULL;
        else if (e && !strcmp(e, "itd")) m = APU_HRTF_ITD;
        fprintf(stderr, "[APU] HRTF on the 3D voices: %s\n",
                m == APU_HRTF_OFF ? "off" : m == APU_HRTF_FULL ? "all" :
                m == APU_HRTF_ITD ? "delay only" : "voices sharing a sample");
    }
    return m;
}

/* For "dense": how many voices played each sample base address in the last
 * VP frame (open addressing, counted this frame into s_ba_cnt[s_ba_cur],
 * read from the other), and for each 3D voice the base address of the
 * source that fed its multipass bin. */
#define BA_SLOTS 512
static struct { uint32_t ba; uint16_t n; } s_ba_cnt[2][BA_SLOTS];
static int s_ba_cur;
static uint32_t s_mp_src_ba;                       /* last source in the MP list */
static uint32_t s_3d_src_ba[MCPX_HW_MAX_3D_VOICES];

static void ba_frame_start(void)
{
    s_ba_cur ^= 1;
    memset(s_ba_cnt[s_ba_cur], 0, sizeof s_ba_cnt[0]);
}

static void ba_count(uint32_t ba)
{
    unsigned h = (ba * 2654435761u) >> 23, k;
    for (k = 0; k < BA_SLOTS; k++, h = (h + 1) % BA_SLOTS) {
        if (s_ba_cnt[s_ba_cur][h].n == 0) s_ba_cnt[s_ba_cur][h].ba = ba;
        if (s_ba_cnt[s_ba_cur][h].ba == ba) { s_ba_cnt[s_ba_cur][h].n++; return; }
    }
}

static unsigned ba_voices(uint32_t ba)
{
    unsigned h = (ba * 2654435761u) >> 23, k;
    for (k = 0; k < BA_SLOTS; k++, h = (h + 1) % BA_SLOTS) {
        if (s_ba_cnt[s_ba_cur ^ 1][h].n == 0) return 0;
        if (s_ba_cnt[s_ba_cur ^ 1][h].ba == ba) return s_ba_cnt[s_ba_cur ^ 1][h].n;
    }
    return 0;
}

/* Level of those voices (the port's "Ambience" setting, RECOMP_APU_GAIN_DENSE
 * at start, default 0.6): the rain sat loud over the squad's speech. */
static volatile float s_dense_gain = -1.0f;

void xbox_AudioSetAmbience(float gain)
{
    s_dense_gain = gain < 0.0f ? 0.0f : gain > 1.0f ? 1.0f : gain;
}

float xbox_AudioGetAmbience(void)
{
    if (s_dense_gain < 0.0f) {
        const char *e = getenv("RECOMP_APU_GAIN_DENSE");
        s_dense_gain = e && *e ? (float)atof(e) : 0.6f;
    }
    return s_dense_gain;
}

/* Each 3D voice slot delays those voices by its own fixed 0-20 ms
 * (RECOMP_APU_DENSE_DELAY=0 turns it off). The title starts the drops on a
 * 16 ms tick; shifting each slot by a different amount scatters them, which
 * leaves less of the comb for the HRIR to break up. The line is fed all the
 * time, so a voice turning dense reads real history. */
#define DENSE_DL_LEN 1024
static float s_dl[MCPX_HW_MAX_3D_VOICES][DENSE_DL_LEN];
static unsigned s_dl_pos[MCPX_HW_MAX_3D_VOICES];

static void dense_delay(uint16_t v, float samples[][2], bool dense)
{
    static int on = -1;
    unsigned delay, i;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_DENSE_DELAY");
        on = !(e && *e && atoi(e) == 0);
    }
    if (!on) return;
    delay = ((v * 2654435761u) >> 16) % 960;          /* 0..20 ms at 48 kHz */
    for (i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        unsigned w = s_dl_pos[v];
        s_dl[v][w] = samples[i][0];
        if (dense)
            samples[i][0] = samples[i][1] =
                s_dl[v][(w + DENSE_DL_LEN - delay) % DENSE_DL_LEN];
        s_dl_pos[v] = (w + 1) % DENSE_DL_LEN;
    }
}

/* Each voice's gain on each of its 8 outputs as last mixed, and the frame it
 * was mixed in (the volume ramp in voice_process). */
static float s_gain[MCPX_HW_MAX_VOICES][8];
/* Submix pairing check (RECOMP_APU_TRACE): sources written to a bin since a
 * multipass voice last read it, and how the reads went. */
static int s_mp_writes[NUM_MIXBINS];
static unsigned s_mp_reads, s_mp_empty, s_mp_multi;
static unsigned long s_gain_frame[MCPX_HW_MAX_VOICES];

static bool voice_is_stream(MCPXAPUState *d, uint16_t v)
{
    return voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                          NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0;
}

static void stream_trace(MCPXAPUState *d, uint16_t v, const char *fmt, ...)
{
    va_list ap;
    if (s_strace < 0)
        s_strace = apu_env_on("RECOMP_APU_STREAM_TRACE");
    if (s_strace <= 0 || s_strace_budget <= 0)
        return;
    if (v >= MCPX_HW_MAX_VOICES || !voice_is_stream(d, v))
        return;
    s_strace_budget--;
    fprintf(stderr, "[APU-STREAM] f=%lu v=%u idx=%d seg=%d A=%u/%u B=%u/%u act=%u pau=%u ",
            s_vp_frames, (unsigned)v,
            d->vp.ssl[v].ssl_index, d->vp.ssl[v].ssl_seg,
            (unsigned)d->vp.ssl[v].base[0], (unsigned)d->vp.ssl[v].count[0],
            (unsigned)d->vp.ssl[v].base[1], (unsigned)d->vp.ssl[v].count[1],
            voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                           NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE),
            voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                           NV_PAVS_VOICE_PAR_STATE_PAUSED));
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ============================================================
 * HRIR coefficient setter
 * ============================================================ */

static void set_hrir_coeff_tar(MCPXAPUState *d, int channel, int coeff_idx,
                               int8_t value)
{
    int entry = d->vp.hrtf.current_entry;
    d->vp.hrtf.entries[entry].hrir[channel][coeff_idx] = int8_to_float(value);
}

/* ============================================================
 * Front-End method dispatch
 * ============================================================ */

static void fe_method(MCPXAPUState *d, uint32_t method, uint32_t argument)
{
    unsigned int slot;

    d->regs[NV_PAPU_FEDECMETH] = method;
    d->regs[NV_PAPU_FEDECPARAM] = argument;
    unsigned int selected_handle, list;

    switch (method) {
    case NV1BA0_PIO_VOICE_LOCK:
        voice_lock(d, (uint16_t)d->regs[NV_PAPU_FECV], argument & 1);
        break;

    case NV1BA0_PIO_SET_ANTECEDENT_VOICE:
        d->regs[NV_PAPU_FEAV] = argument;
        break;

    case NV1BA0_PIO_VOICE_ON: {
        selected_handle = argument & NV1BA0_PIO_VOICE_ON_HANDLE;
        stream_trace(d, (uint16_t)selected_handle, "VOICE_ON arg=%08X feav=%08X",
                     argument, d->regs[NV_PAPU_FEAV]);

        bool locked = is_voice_locked(d, (uint16_t)selected_handle);
        if (!locked) voice_lock(d, (uint16_t)selected_handle, true);

        list = GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_LST);
        if (list != NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_INHERIT) {
            unsigned int top_reg = voice_list_regs[list - 1].top;
            voice_set_mask(d, (uint16_t)selected_handle,
                           NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           d->regs[top_reg]);
            d->regs[top_reg] = selected_handle;
        } else {
            unsigned int antecedent_voice =
                GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_VALUE);
            assert(antecedent_voice != 0xFFFF);

            uint32_t next_handle = voice_get_mask(
                d, (uint16_t)antecedent_voice, NV_PAVS_VOICE_TAR_PITCH_LINK,
                NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
            voice_set_mask(d, (uint16_t)selected_handle,
                           NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           next_handle);
            voice_set_mask(d, (uint16_t)antecedent_voice,
                           NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           selected_handle);
        }

        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_OFFSET,
                       NV_PAVS_VOICE_PAR_OFFSET_CBO, 0);
        d->vp.ssl[selected_handle].ssl_seg = 0;
        d->vp.ssl[selected_handle].ssl_index = 0;

        unsigned int ea_start = GET_MASK(argument, NV1BA0_PIO_VOICE_ON_ENVA);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EACUR, ea_start);
        if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY) {
            uint16_t delay_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENV0, NV_PAVS_VOICE_CFG_ENV0_EA_DELAYTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, delay_time * 16);
        } else if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK) {
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, 0);
        } else if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD) {
            uint16_t hold_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENVA, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, hold_time * 16);
        }

        unsigned int ef_start = GET_MASK(argument, NV1BA0_PIO_VOICE_ON_ENVF);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR, ef_start);
        if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY) {
            uint16_t delay_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENV1, NV_PAVS_VOICE_CFG_ENV0_EA_DELAYTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, delay_time * 16);
        } else if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK) {
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, 0);
        } else if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD) {
            uint16_t hold_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENVF, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, hold_time * 16);
        }

        voice_reset_filters(d, (uint16_t)selected_handle);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 1);

        if (!locked) voice_lock(d, (uint16_t)selected_handle, false);
        break;
    }

    case NV1BA0_PIO_VOICE_RELEASE: {
        selected_handle = argument & NV1BA0_PIO_VOICE_ON_HANDLE;
        stream_trace(d, (uint16_t)selected_handle, "VOICE_RELEASE");

        bool locked = is_voice_locked(d, (uint16_t)selected_handle);
        if (!locked) voice_lock(d, (uint16_t)selected_handle, true);

        uint16_t rr;
        rr = (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
            NV_PAVS_VOICE_TAR_LFO_ENV, NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                       NV_PAVS_VOICE_CUR_ECNT_EACOUNT, rr * 16);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EACUR,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE);

        rr = (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
            NV_PAVS_VOICE_CFG_MISC, NV_PAVS_VOICE_CFG_MISC_EF_RELEASERATE);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                       NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, rr * 16);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE);

        if (!locked) voice_lock(d, (uint16_t)selected_handle, false);
        break;
    }

    case NV1BA0_PIO_VOICE_OFF:
        stream_trace(d, (uint16_t)(argument & NV1BA0_PIO_VOICE_OFF_HANDLE),
                     "VOICE_OFF method");
        voice_off(d, (uint16_t)(argument & NV1BA0_PIO_VOICE_OFF_HANDLE));
        break;

    case NV1BA0_PIO_VOICE_PAUSE:
        voice_set_mask(d, (uint16_t)(argument & NV1BA0_PIO_VOICE_PAUSE_HANDLE),
                       NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_PAUSED,
                       (argument & NV1BA0_PIO_VOICE_PAUSE_ACTION) != 0);
        break;

    case NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY:
        d->vp.hrtf.current_entry =
            GET_MASK(argument, NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY_HANDLE);
        break;

    case NV1BA0_PIO_SET_CURRENT_VOICE:
        d->regs[NV_PAPU_FECV] = argument;
        break;

    case NV1BA0_PIO_SET_VOICE_CFG_VBIN:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_VBIN, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_FMT:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENV0:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENV0, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENVA:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENVA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENV1:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENV1, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENVF:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENVF, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_MISC:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_MISC, 0xFFFFFFFF, argument);
        break;

    case NV1BA0_PIO_SET_VOICE_TAR_HRTF: {
        int handle = GET_MASK(argument, NV1BA0_PIO_SET_VOICE_TAR_HRTF_HANDLE);
        int current_voice = d->regs[NV_PAPU_FECV];
        voice_set_mask(d, (uint16_t)current_voice,
                       NV_PAVS_VOICE_CFG_HRTF_TARGET,
                       NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE, handle);
        if (current_voice < MCPX_HW_MAX_3D_VOICES &&
            handle != HRTF_NULL_HANDLE) {
            assert(handle < HRTF_ENTRY_COUNT);
            hrtf_filter_set_target_params(&d->vp.filters[current_voice].hrtf,
                                          d->vp.hrtf.entries[handle].hrir,
                                          d->vp.hrtf.entries[handle].itd);
        }
        break;
    }

    case NV1BA0_PIO_SET_VOICE_TAR_VOLA:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_VOLB:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLB, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_VOLC:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLC, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_LFO_ENV:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_LFO_ENV, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_FCA:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_FCA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_FCB:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_FCB, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_PITCH:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_PITCH_LINK,
                       NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH,
                       (argument & NV1BA0_PIO_SET_VOICE_TAR_PITCH_STEP) >> 16);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_BASE:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CUR_PSL_START,
                       NV_PAVS_VOICE_CUR_PSL_START_BA, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_LBO:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CUR_PSH_SAMPLE,
                       NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_BUF_CBO:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_PAR_OFFSET,
                       NV_PAVS_VOICE_PAR_OFFSET_CBO, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_EBO:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_PAR_NEXT,
                       NV_PAVS_VOICE_PAR_NEXT_EBO, argument);
        break;

    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE:
        d->vp.inbuf_sge_handle = argument & NV1BA0_PIO_SET_CURRENT_INBUF_SGE_HANDLE;
        break;

    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET: {
        hwaddr sge_address =
            d->regs[NV_PAPU_VPSGEADDR] + d->vp.inbuf_sge_handle * 8;
        stl_le_phys(address_space_memory, sge_address,
                    argument & NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET_PARAMETER);
        break;
    }

    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE:
        d->vp.outbuf_sge_handle =
            argument & NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_HANDLE;
        break;

    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET: {
        hwaddr sge_address =
            d->regs[NV_PAPU_VPSGEADDR] + d->vp.outbuf_sge_handle * 8;
        stl_le_phys(address_space_memory, sge_address,
                    argument & NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET_PARAMETER);
        break;
    }

    case NV1BA0_PIO_SET_VOICE_SSL_A: {
        apu_irq_stats.ssl_sets++;
        int ssl = 0;
        int current_voice = d->regs[NV_PAPU_FECV];
        assert(current_voice < MCPX_HW_MAX_VOICES);
        d->vp.ssl[current_voice].base[ssl] =
            GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_BASE);
        d->vp.ssl[current_voice].count[ssl] =
            (uint8_t)GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_COUNT);
        s_strace_dry[current_voice] = 0;
        stream_trace(d, (uint16_t)current_voice, "SET_SSL_A arg=%08X", argument);
        break;
    }
    case NV1BA0_PIO_SET_VOICE_SSL_B: {
        apu_irq_stats.ssl_sets++;
        int ssl = 1;
        int current_voice = d->regs[NV_PAPU_FECV];
        assert(current_voice < MCPX_HW_MAX_VOICES);
        d->vp.ssl[current_voice].base[ssl] =
            GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_BASE);
        d->vp.ssl[current_voice].count[ssl] =
            (uint8_t)GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_COUNT);
        s_strace_dry[current_voice] = 0;
        stream_trace(d, (uint16_t)current_voice, "SET_SSL_B arg=%08X", argument);
        break;
    }

    case NV1BA0_PIO_SET_CURRENT_SSL: {
        assert((argument & 0x3f) == 0);
        assert(argument < (MCPX_HW_MAX_SSL_PRDS * NV_PSGE_SIZE));
        d->vp.ssl_base_page = argument;
        break;
    }

    case NV1BA0_PIO_SET_HRTF_SUBMIXES:
        d->vp.hrtf_submix[0] = (uint8_t)((argument >> 0) & 0x1f);
        d->vp.hrtf_submix[1] = (uint8_t)((argument >> 8) & 0x1f);
        d->vp.hrtf_submix[2] = (uint8_t)((argument >> 16) & 0x1f);
        d->vp.hrtf_submix[3] = (uint8_t)((argument >> 24) & 0x1f);
        break;

    case NV1BA0_PIO_SET_HRTF_HEADROOM:
        if (d->vp.hrtf_headroom != (uint8_t)(argument & NV1BA0_PIO_SET_HRTF_HEADROOM_AMOUNT))
            fprintf(stderr, "[APU] HRTF headroom %u -> %u\n", d->vp.hrtf_headroom,
                    (unsigned)(argument & NV1BA0_PIO_SET_HRTF_HEADROOM_AMOUNT));
        d->vp.hrtf_headroom = (uint8_t)(argument & NV1BA0_PIO_SET_HRTF_HEADROOM_AMOUNT);
        break;

    case SE2FE_IDLE_VOICE:
        if (d->regs[NV_PAPU_FETFORCE1] & NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE) {
            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FEMETHMODE;
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FEMETHMODE_TRAPPED;
            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FETRAPREASON;
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FETRAPREASON_REQUESTED;
            /* Latched here, once per trap; update_irq no longer re-derives
             * it from FECTL (see there). */
            qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_FETINTSTS);
            d->set_irq = true;
            apu_irq_stats.traps++;
        }
        break;

    default:
        /* Handle range-based cases that can't use case ranges in MSVC */
        if (method >= NV1BA0_PIO_SET_HRIR && method < NV1BA0_PIO_SET_HRIR_X) {
            assert(d->vp.hrtf.current_entry < HRTF_ENTRY_COUNT);
            slot = (method - NV1BA0_PIO_SET_HRIR) / 4;
            int8_t left0 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_LEFT0);
            int8_t right0 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_RIGHT0);
            int8_t left1 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_LEFT1);
            int8_t right1 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_RIGHT1);
            int coeff_idx = slot * 2;
            set_hrir_coeff_tar(d, 0, coeff_idx, left0);
            set_hrir_coeff_tar(d, 1, coeff_idx, right0);
            set_hrir_coeff_tar(d, 0, coeff_idx + 1, left1);
            set_hrir_coeff_tar(d, 1, coeff_idx + 1, right1);
        } else if (method == NV1BA0_PIO_SET_HRIR_X) {
            assert(d->vp.hrtf.current_entry < HRTF_ENTRY_COUNT);
            int8_t left30 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_LEFT30);
            int8_t right30 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_RIGHT30);
            int16_t itd = (int16_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_ITD);
            set_hrir_coeff_tar(d, 0, 30, left30);
            set_hrir_coeff_tar(d, 1, 30, right30);
            d->vp.hrtf.entries[d->vp.hrtf.current_entry].itd = s6p9_to_float(itd);
        } else if (method >= NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET &&
                   method < NV1BA0_PIO_SET_SSL_SEGMENT_LENGTH + 8 * 64) {
            assert((method & 0x3) == 0);
            hwaddr addr = d->regs[NV_PAPU_VPSSLADDR]
                          + (d->vp.ssl_base_page * 8)
                          + (method - NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET);
            stl_le_phys(address_space_memory, addr, argument);
        } else if (method >= NV1BA0_PIO_SET_SUBMIX_HEADROOM &&
                   method <= NV1BA0_PIO_SET_SUBMIX_HEADROOM + 4 * (NUM_MIXBINS - 1)) {
            assert((method & 3) == 0);
            slot = (method - NV1BA0_PIO_SET_SUBMIX_HEADROOM) / 4;
            if (d->vp.submix_headroom[slot] != (uint8_t)(argument & NV1BA0_PIO_SET_SUBMIX_HEADROOM_AMOUNT))
                fprintf(stderr, "[APU] submix %u headroom %u -> %u\n", slot,
                        d->vp.submix_headroom[slot],
                        (unsigned)(argument & NV1BA0_PIO_SET_SUBMIX_HEADROOM_AMOUNT));
            d->vp.submix_headroom[slot] =
                (uint8_t)(argument & NV1BA0_PIO_SET_SUBMIX_HEADROOM_AMOUNT);
        } else if ((method >= NV1BA0_PIO_SET_OUTBUF_BA &&
                    method < NV1BA0_PIO_SET_OUTBUF_BA + 32) ||
                   (method >= NV1BA0_PIO_SET_OUTBUF_LEN &&
                    method < NV1BA0_PIO_SET_OUTBUF_LEN + 32)) {
            /* Outbuf base/length - ignore for now */
        } else {
            /* Unknown method - silently ignore */
            DPRINTF("Unknown FE method: 0x%08X arg=0x%08X\n", method, argument);
        }
        break;
    }
}

/* ============================================================
 * VP MMIO read/write (exposed to apu_core.c)
 * ============================================================ */

uint64_t mcpx_apu_vp_read(void *opaque, hwaddr addr, unsigned int size)
{
    (void)opaque; (void)size;

    switch (addr) {
    case NV1BA0_PIO_FREE:
        return 0x80; /* Always pretend queue is empty */
    default:
        break;
    }
    return 0;
}

void mcpx_apu_vp_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    (void)size;

    /* Dispatch known methods through fe_method */
    fe_method(d, (uint32_t)addr, (uint32_t)val);
}

/* ============================================================
 * SGE data pointer resolution
 * ============================================================ */

static hwaddr get_data_ptr(hwaddr sge_base, unsigned int max_sge, uint32_t addr)
{
    unsigned int entry = addr / TARGET_PAGE_SIZE;
    assert(entry <= max_sge);
    uint32_t prd_address =
        ldl_le_phys(address_space_memory, sge_base + entry * 4 * 2);
    return prd_address + addr % TARGET_PAGE_SIZE;
}

/* ============================================================
 * Envelope processing
 * ============================================================ */

static float voice_step_envelope(MCPXAPUState *d, uint16_t v, uint32_t reg_0,
                           uint32_t reg_a, uint32_t rr_reg, uint32_t rr_mask,
                           uint32_t lvl_reg, uint32_t lvl_mask,
                           uint32_t count_mask, uint32_t cur_mask)
{
    uint8_t cur = (uint8_t)voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask);
    switch (cur) {
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_OFF:
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, 0);
        voice_set_mask(d, v, lvl_reg, lvl_mask, 0xFF);
        return 1.0f;

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        voice_set_mask(d, v, lvl_reg, lvl_mask, 0x00);
        if (count == 0) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
        } else {
            count--;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return 0.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t attack_rate =
            (uint16_t)voice_get_mask(d, v, reg_0, NV_PAVS_VOICE_CFG_ENV0_EA_ATTACKRATE);
        float value;
        if (attack_rate == 0) {
            value = 255.0f;
        } else {
            if (count <= (uint32_t)(attack_rate * 16)) {
                value = (count * 0xFF) / (float)(attack_rate * 16);
            } else {
                value = 255.0f;
            }
        }
        voice_set_mask(d, v, lvl_reg, lvl_mask, (uint32_t)value);
        if (count == (uint32_t)(attack_rate * 16)) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
            uint16_t hold_time =
                (uint16_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            count = hold_time * 16;
        } else {
            count++;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return value / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        voice_set_mask(d, v, lvl_reg, lvl_mask, 0xFF);
        if (count == 0) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
            uint16_t decay_rate =
                (uint16_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_DECAYRATE);
            count = decay_rate * 16;
        } else {
            count--;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return 1.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_DECAY: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t decay_rate =
            (uint16_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_DECAYRATE);
        uint8_t sustain_level =
            (uint8_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_SUSTAINLEVEL);
        float value;
        if (decay_rate == 0) {
            value = 0.0f;
        } else {
            value = 255.0f * powf(0.99988799f, (decay_rate * 16 - count) *
                                                   4096.0f / decay_rate);
        }
        if (value <= (sustain_level + 0.2f) || (value > 255.0f)) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
        } else {
            count--;
            voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
            voice_set_mask(d, v, lvl_reg, lvl_mask, (uint32_t)value);
        }
        return value / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_SUSTAIN: {
        uint8_t sustain_level =
            (uint8_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_SUSTAINLEVEL);
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, 0x00);
        voice_set_mask(d, v, lvl_reg, lvl_mask, sustain_level);
        return sustain_level / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t release_rate = (uint16_t)voice_get_mask(d, v, rr_reg, rr_mask);
        if (release_rate == 0) count = 0;
        float value = 0;
        if (count == 0) {
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, ++cur);
        } else {
            float pos = clampf(1 - count / (release_rate * 16.0f), 0, 1);
            uint8_t lvl = (uint8_t)voice_get_mask(d, v, lvl_reg, lvl_mask);
            value = powf((float)M_E, -6.91f * pos) * lvl;
            count--;
            voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        }
        return value / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_FORCE_RELEASE:
        if (count_mask == NV_PAVS_VOICE_CUR_ECNT_EACOUNT) {
            voice_off(d, v);
        }
        return 0.0f;

    default:
        fprintf(stderr, "[APU] Unknown envelope state 0x%x\n", cur);
        return 0.0f;
    }
}

/* ============================================================
 * Per-voice source statistics (RECOMP_APU_TRACE)
 *
 * What each voice fed the mixer over the last second: its format word, where
 * its data was read from and how that address resolved, the amplitude
 * envelope and the first two volumes/bins, plus the RMS and zero-crossing
 * rate of the fetched samples. The zero-crossing rate separates music
 * (~0.1-0.2) from white noise (~0.5) without a spectrum, so a voice that
 * reads garbage shows up by itself.
 *
 * RECOMP_APU_VOICE_DUMP=<voice> also writes that voice's fetched samples,
 * before gain, as raw s16 stereo 48 kHz to RECOMP_APU_VOICE_DUMP_FILE
 * (default voice_dump.raw in the working directory).
 * ============================================================ */

struct VpVoiceStat {
    uint32_t frames, zc;
    double   sq, sum;                 /* sum: the mean says the DC offset */
    float    last, ea, rate;
    uint32_t addr, fmt;
    uint16_t vol0, vol1;
    uint8_t  bin0, bin1;
    double   e3d;                     /* energy this voice put into bins 6-9 */
    double   emp;                     /* multipass: energy found in its source bin */
    int      mpbin;                   /* multipass source bin, -1 if not multipass */
    const char *cls;
    unsigned hrtf;                    /* 3D: frames the HRTF ran on it */
};
static struct VpVoiceStat s_vstat[MCPX_HW_MAX_VOICES];
static int s_vstat_on = -1;
static int s_vdump_voice = -1;      /* -2: every voice, one file each */
static FILE *s_vdump;
static FILE *s_vdump_all[MCPX_HW_MAX_VOICES];
static uint8_t s_vdump_failed[MCPX_HW_MAX_VOICES]; /* open failed: no retry */
static char s_vdump_prefix[512];
static unsigned s_vstat_frames, s_vstat_secs;
static volatile int s_vstat_snap;   /* one report on request, then off again */

static void vp_stat_init(void);

/* The F6 capture (xbox_AudioCapture): the voice table for the next second,
 * whether RECOMP_APU_TRACE is on or not. */
void apu_vp_snapshot_request(void)
{
    if (s_vstat_on < 0)
        vp_stat_init();
    if (s_vstat_on > 0)
        return;                        /* already reporting every second */
    for (unsigned v = 0; v < MCPX_HW_MAX_VOICES; v++)
        s_vstat[v].frames = 0, s_vstat[v].zc = 0, s_vstat[v].sq = 0, s_vstat[v].sum = 0;
    s_vstat_frames = 0;
    s_vstat_snap = 1;
    s_vstat_on = 1;
    fprintf(stderr, "[AUDIO-CAP] voice table follows in a second ([APU-VSTAT] lines)\n");
}

static void vp_stat_init(void)
{
    const char *e;
    s_vstat_on = apu_env_on("RECOMP_APU_TRACE");
    e = getenv("RECOMP_APU_VOICE_DUMP");
    if (e && *e) {
        const char *path = getenv("RECOMP_APU_VOICE_DUMP_FILE");
        if (!strcmp(e, "all")) {
            /* RECOMP_APU_VOICE_DUMP=all: <file>.<voice>.raw for each voice
             * that renders anything (voice slots are not stable between
             * runs, so a sound cannot be picked by number in advance). */
            s_vdump_voice = -2;
            snprintf(s_vdump_prefix, sizeof(s_vdump_prefix), "%s",
                     path && *path ? path : "voice_dump");
            fprintf(stderr, "[APU] voice dump: every voice -> %s.<voice>.raw\n",
                    s_vdump_prefix);
            return;
        }
        s_vdump_voice = atoi(e);
        s_vdump = fopen(path && *path ? path : "voice_dump.raw", "wb");
        fprintf(stderr, "[APU] voice dump: voice %d -> %s\n", s_vdump_voice,
                s_vdump ? (path && *path ? path : "voice_dump.raw") : "(open failed)");
    }
}

static void vp_stat_samples(uint32_t v, const float samples[][2], int n,
                            hwaddr first_addr, uint32_t fmt)
{
    struct VpVoiceStat *s = &s_vstat[v];
    if (s_vstat_on < 0) vp_stat_init();
    if (!s_vstat_on && (int)v != s_vdump_voice && s_vdump_voice != -2) return;
    if (n <= 0) return;
    if (s->frames == 0) {
        s->addr = (uint32_t)first_addr;
        s->cls = mcpx_apu_phys_class(first_addr);
        s->fmt = fmt;
    }
    for (int i = 0; i < n; i++) {
        float x = samples[i][0];
        s->sq += (double)x * x;
        s->sum += x;
        if ((x < 0) != (s->last < 0)) s->zc++;
        s->last = x;
    }
    s->frames += (uint32_t)n;
    if (s_vdump_voice == -2 && !s_vdump_all[v] && !s_vdump_failed[v]) {
        char name[600];
        snprintf(name, sizeof(name), "%s.%u.raw", s_vdump_prefix, (unsigned)v);
        s_vdump_all[v] = fopen(name, "wb");
        s_vdump_failed[v] = s_vdump_all[v] == NULL;
    }
    {
        FILE *df = s_vdump_voice == -2 ? s_vdump_all[v]
                 : (int)v == s_vdump_voice ? s_vdump : NULL;
        int16_t out[NUM_SAMPLES_PER_FRAME * 2];
        int m = n < NUM_SAMPLES_PER_FRAME ? n : NUM_SAMPLES_PER_FRAME;
        if (!df)
            return;
        for (int i = 0; i < m; i++) {
            for (int c = 0; c < 2; c++) {
                float x = samples[i][c] * 32767.0f;
                if (x > 32767.0f) x = 32767.0f;
                if (x < -32768.0f) x = -32768.0f;
                out[2 * i + c] = (int16_t)x;
            }
        }
        fwrite(out, sizeof(int16_t) * 2, (size_t)m, df);
    }
}

static void vp_stat_report(void)
{
    if (s_vstat_on <= 0 && s_vdump_voice == -1) return;
    if (++s_vstat_frames % 1500) return;
    /* Flush the dumps once a second, trace or not: a watchdog _exit or a
     * crash skips the stdio buffers. */
    if (s_vdump) fflush(s_vdump);
    for (unsigned v = 0; v < MCPX_HW_MAX_VOICES; v++)
        if (s_vdump_all[v]) fflush(s_vdump_all[v]);
    if (s_vstat_on <= 0) return;
    s_vstat_secs++;
    fprintf(stderr, "[APU-MP] t=%us multipass reads %u: %u found their bin empty, %u found"
            " more than one source\n", s_vstat_secs, s_mp_reads, s_mp_empty, s_mp_multi);
    s_mp_reads = s_mp_empty = s_mp_multi = 0;
    for (unsigned v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        struct VpVoiceStat *s = &s_vstat[v];
        if (!s->frames && s->e3d == 0 && s->emp == 0) continue;
        fprintf(stderr, "[APU-VSTAT] t=%us v=%u fmt=%08X stream=%u st=%u ss=%u cs=%u spb=%u"
                " rate=%.3f ea=%.2f vol=%03X/%03X bin=%u/%u addr=%08X %s"
                " frames=%u rms=%.0f dc=%.0f zcr=%.2f to3d=%.0f mpbin=%d mpin=%.0f hrtf=%u\n",
                s_vstat_secs, v, s->fmt,
                (unsigned)((s->fmt & NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0),
                (unsigned)((s->fmt & NV_PAVS_VOICE_CFG_FMT_STEREO) != 0),
                (unsigned)((s->fmt & NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE) >> ctz32(NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE)),
                (unsigned)((s->fmt & NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE) >> ctz32(NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE)),
                (unsigned)((s->fmt & NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK) >> ctz32(NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK)),
                s->rate, s->ea, s->vol0, s->vol1, s->bin0, s->bin1, s->addr,
                s->cls ? s->cls : "?", s->frames,
                sqrt(s->sq / (s->frames ? s->frames : 1)) * 32768.0,
                s->sum / (s->frames ? s->frames : 1) * 32768.0,
                (double)s->zc / (s->frames ? s->frames : 1),
                sqrt(s->e3d / 48000.0) * 32768.0, s->mpbin,
                sqrt(s->emp / 48000.0) * 32768.0, s->hrtf);
        s->frames = 0; s->zc = 0; s->sq = 0; s->sum = 0;
        s->e3d = 0; s->emp = 0; s->mpbin = -1; s->hrtf = 0;
    }
    if (s_vstat_snap) {                /* the F6 snapshot: one second only */
        s_vstat_snap = 0;
        s_vstat_on = 0;
        fflush(stderr);
    }
}

/* ============================================================
 * Ring monitor (RECOMP_APU_RING)
 *
 * A title that streams compressed audio decodes it on the CPU into a looping
 * PCM buffer voice and keeps writing ahead of the play cursor. If the writer
 * falls behind, the VP plays the previous lap's data again: no digital zero,
 * nothing a check on the mix can see, and only a stutter to the ear.
 *
 * Each looping PCM buffer is split into 1 KB blocks, and the samples the VP
 * fetches are hashed per block as it plays them. When the cursor leaves a
 * block it played whole, that hash is compared with the one from the
 * block's previous play. Once some block of a buffer has played differently
 * on two laps, the buffer is a ring (one loaded once and looped -- a sound
 * effect -- never changes and is never judged). In a ring, a non-silent
 * block that plays exactly as it did one lap before is a stale-lap replay.
 *
 * "lead" is how far ahead of the cursor the writer is: the blocks after the
 * one being entered whose memory has changed since they were last played,
 * in source frames.
 *
 *   [APU-RING] stale v=<voice> blk=<b>/<n> lap=<laps> ms=<wall ms>
 *       first 200 stale-lap replays
 *   [APU-RING] t=<s> v=<voice> bytes=<ring size> entries=<blocks played>
 *       stale=<this second> stale_total=<run> lead_min=<frames> lead_avg=<frames>
 *       once a VP second for each ring that played
 * ============================================================ */

#define RING_BLK      1024u
#define RING_MAX_BLKS 1024u

struct VpRing {
    uint32_t ba, bytes, nblk;
    uint32_t cur;                 /* block being played, ~0: none */
    uint32_t cur_frames;          /* frames of it played so far */
    uint64_t acc;                 /* hash of those frames */
    bool     acc_sound;
    uint32_t laps;
    bool     dynamic;
    uint32_t entries, stale, stale_total;
    uint32_t lead_min, lead_n;
    uint64_t lead_sum;
    uint64_t played[RING_MAX_BLKS];   /* hash of its last whole play */
    uint64_t mem[RING_MAX_BLKS];      /* memory hash when it was last entered */
    uint8_t  seen[RING_MAX_BLKS];     /* bit 0: played, bit 1: mem valid */
};
static struct VpRing *s_ring[MCPX_HW_MAX_VOICES];
static int s_ring_on = -1;
static unsigned s_ring_logged;
static unsigned s_ring_frames, s_ring_secs;

static uint64_t ring_hash_block(MCPXAPUState *d, uint32_t ba, uint32_t off,
                                uint32_t len)
{
    uint64_t h = 0x9E3779B97F4A7C15ull;
    while (len) {
        uint32_t lin = ba + off;
        uint32_t run = TARGET_PAGE_SIZE - lin % TARGET_PAGE_SIZE;
        if (run > len) run = len;
        const uint8_t *p = mcpx_apu_phys(
            get_data_ptr(d->regs[NV_PAPU_VPSGEADDR], 0xFFFFFFFF, lin));
        uint32_t i = 0;
        for (; i + 8 <= run; i += 8) {
            uint64_t w;
            memcpy(&w, p + i, 8);
            h = (h ^ w) * 0x100000001B3ull;
            h ^= h >> 29;
        }
        for (; i < run; i++)
            h = (h ^ p[i]) * 0x100000001B3ull;
        off += run;
        len -= run;
    }
    return h;
}

/* The cursor left block r->cur. */
static void ring_close_block(struct VpRing *r, uint32_t v, uint32_t frames_per_blk)
{
    uint32_t b = r->cur;
    if (b == ~0u || r->cur_frames < frames_per_blk)
        return;                              /* not played whole */
    if (r->seen[b] & 1) {
        if (r->acc != r->played[b]) {
            r->dynamic = true;
        } else if (r->dynamic && r->acc_sound) {
            r->stale++;
            r->stale_total++;
            if (s_ring_logged < 200) {
                s_ring_logged++;
                fprintf(stderr, "[APU-RING] stale v=%u blk=%u/%u lap=%u ms=%llu\n",
                        v, b, r->nblk, r->laps,
                        (unsigned long long)(qemu_clock_get_us(QEMU_CLOCK_REALTIME) / 1000));
            }
        }
    }
    r->played[b] = r->acc;
    r->seen[b] |= 1;
}

/* The VP fetched n frames of a looping PCM buffer voice, the first at
 * offset first; frames are frame_bytes apart in the buffer. */
static void vp_ring_check(MCPXAPUState *d, uint32_t v, uint32_t ba,
                          uint32_t ebo, uint32_t frame_bytes, uint32_t first,
                          const float samples[][2], int n)
{
    struct VpRing *r;
    uint32_t bytes = (ebo + 1) * frame_bytes;
    uint32_t fpb;

    if (s_ring_on < 0)
        s_ring_on = apu_env_on("RECOMP_APU_RING");
    if (!s_ring_on || v >= MCPX_HW_MAX_VOICES || bytes < 2 * RING_BLK ||
        frame_bytes == 0 || RING_BLK % frame_bytes)
        return;
    fpb = RING_BLK / frame_bytes;
    r = s_ring[v];
    if (!r) {
        r = s_ring[v] = calloc(1, sizeof(*r));
        if (!r) return;
        r->ba = ~0u;
    }
    if (r->ba != ba || r->bytes != bytes) {   /* a new buffer on this voice */
        uint32_t keep_total = r->stale_total;
        memset(r, 0, sizeof(*r));
        r->ba = ba;
        r->bytes = bytes;
        r->nblk = bytes / RING_BLK;           /* a partial last block is skipped */
        if (r->nblk > RING_MAX_BLKS) r->nblk = RING_MAX_BLKS;
        r->cur = ~0u;
        r->lead_min = ~0u;
        r->stale_total = keep_total;
    }

    for (int i = 0; i < n; i++) {
        uint32_t b = (first + (uint32_t)i) / fpb;
        if (b != r->cur) {
            ring_close_block(r, v, fpb);
            if (b < r->cur && r->cur != ~0u)
                r->laps++;
            r->cur = b;
            r->cur_frames = 0;
            r->acc = 0x9E3779B97F4A7C15ull;
            r->acc_sound = false;
            if (b < r->nblk) {
                /* Entering b: the writer's lead, then b's memory hash. */
                uint64_t h = ring_hash_block(d, ba, b * RING_BLK, RING_BLK);
                r->entries++;
                if (r->dynamic) {
                    uint32_t k, lead = 0;
                    for (k = 1; k < r->nblk; k++) {
                        uint32_t bb = (b + k) % r->nblk;
                        if (!(r->seen[bb] & 2))
                            break;
                        if (ring_hash_block(d, ba, bb * RING_BLK, RING_BLK) == r->mem[bb])
                            break;
                        lead++;
                    }
                    lead *= fpb;
                    if (lead < r->lead_min) r->lead_min = lead;
                    r->lead_sum += lead;
                    r->lead_n++;
                }
                r->mem[b] = h;
                r->seen[b] |= 2;
            }
        }
        if (b >= r->nblk)
            continue;
        {
            uint32_t w[2];
            memcpy(w, samples[i], sizeof(w));
            r->acc = (r->acc ^ (((uint64_t)w[0] << 32) | w[1])) * 0x100000001B3ull;
            r->acc ^= r->acc >> 29;
            if (w[0] | w[1]) r->acc_sound = true;
            r->cur_frames++;
        }
    }
}

static void vp_ring_report(unsigned secs)
{
    if (s_ring_on <= 0) return;
    for (unsigned v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        struct VpRing *r = s_ring[v];
        if (!r || !r->entries || !r->dynamic) {
            if (r) r->entries = 0;
            continue;
        }
        fprintf(stderr, "[APU-RING] t=%us v=%u bytes=%u entries=%u stale=%u"
                " stale_total=%u lead_min=%u lead_avg=%llu\n",
                secs, v, r->bytes, r->entries, r->stale, r->stale_total,
                r->lead_n ? r->lead_min : 0,
                (unsigned long long)(r->lead_n ? r->lead_sum / r->lead_n : 0));
        r->entries = 0;
        r->stale = 0;
        r->lead_min = ~0u;
        r->lead_sum = 0;
        r->lead_n = 0;
    }
}

/* ============================================================
 * Sample fetching from voice buffers
 * ============================================================ */

static int voice_get_samples(MCPXAPUState *d, uint32_t v, float samples[][2],
                       int num_samples_requested)
{
    assert(v < MCPX_HW_MAX_VOICES);
    bool stereo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_STEREO) != 0;
    unsigned int channels = stereo ? 2 : 1;
    unsigned int sample_size = voice_get_mask(
        d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE);
    unsigned int container_sizes[4] = { 1, 2, 0, 4 };
    unsigned int container_size_index = voice_get_mask(
        d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE);
    unsigned int container_size = container_sizes[container_size_index];
    bool stream = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0;
    bool paused = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                                 NV_PAVS_VOICE_PAR_STATE_PAUSED) != 0;
    bool loop = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                               NV_PAVS_VOICE_CFG_FMT_LOOP) != 0;
    uint32_t ebo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_NEXT,
                                  NV_PAVS_VOICE_PAR_NEXT_EBO);
    uint32_t cbo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                                  NV_PAVS_VOICE_PAR_OFFSET_CBO);
    uint32_t lbo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CUR_PSH_SAMPLE,
                                  NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO);
    uint32_t ba = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CUR_PSL_START,
                                 NV_PAVS_VOICE_CUR_PSL_START_BA);
    unsigned int samples_per_block =
        1 + voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                           NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK);
    bool persist = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                  NV_PAVS_VOICE_CFG_FMT_PERSIST) != 0;

    int ssl_index = 0, ssl_seg = 0, page = 0, count = 0;
    int seg_len = 0, seg_cs = 0, seg_spb = 0, seg_s = 0;
    hwaddr segment_offset = 0;
    uint32_t segment_length = 0;
    size_t block_size;

    int adpcm_block_index = -1;
    uint32_t adpcm_block[36 * 2 / 4];
    int16_t adpcm_decoded[65 * 2];

    voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                   NV_PAVS_VOICE_PAR_STATE_NEW_VOICE, 0);

    if (paused) {
        if (stream) apu_irq_stats.stream_paused_frames++;
        return -1;
    }

    if (stream) {
        apu_irq_stats.stream_frames++;
        if (!persist) {
            int eacur = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                                       NV_PAVS_VOICE_PAR_STATE_EACUR);
            if (eacur < NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE) {
                stream_trace(d, (uint16_t)v, "off: not persist, eacur=%d", eacur);
                voice_off(d, (uint16_t)v);
                return -1;
            }
        }

        assert(!loop);
        ssl_index = d->vp.ssl[v].ssl_index;
        ssl_seg = d->vp.ssl[v].ssl_seg;
        page = d->vp.ssl[v].base[ssl_index] + ssl_seg;
        count = d->vp.ssl[v].count[ssl_index];

        if (count == 0) {
            if (!s_strace_dry[v]) {
                s_strace_dry[v] = 1;
                stream_trace(d, (uint16_t)v, "ran dry (SSL %d empty) persist=%d",
                             ssl_index, (int)persist);
            }
            voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                           NV_PAVS_VOICE_PAR_OFFSET_CBO, 0);
            d->vp.ssl[v].ssl_seg = 0;
            if (!persist) {
                d->vp.ssl[v].ssl_index = 0;
                voice_off(d, (uint16_t)v);
            } else {
                set_notify_status(d, v, MCPX_HW_NOTIFIER_SSLA_DONE +
                                  d->vp.ssl[v].ssl_index,
                                  NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
            }
            return -1;
        }

        hwaddr addr = d->regs[NV_PAPU_VPSSLADDR] + page * 8;
        segment_offset = ldl_le_phys(address_space_memory, addr);
        segment_length = ldl_le_phys(address_space_memory, addr + 4);
        assert(segment_offset != 0);
        assert(segment_length != 0);
        seg_len = (segment_length >> 0) & 0xffff;
        seg_cs = (segment_length >> 16) & 3;
        seg_spb = (segment_length >> 18) & 0x1f;
        seg_s = (segment_length >> 23) & 1;
        container_size_index = seg_cs;
        if (seg_cs == NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM) {
            sample_size = NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24;
        }
        assert(seg_len > 0);
        ebo = seg_len - 1;
    }

    bool adpcm =
        (container_size_index == NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM);

    if (adpcm) {
        block_size = 36;
    } else {
        block_size = container_size;
    }
    block_size *= samples_per_block;

    int sample_count = 0;
    hwaddr stat_addr = 0;
    for (; (sample_count < num_samples_requested) && (cbo <= ebo);
         sample_count++, cbo++) {
        if (sample_count == 0 && s_vstat_on != 0) {
            uint32_t lin = adpcm ? (cbo / ADPCM_SAMPLES_PER_BLOCK) * (uint32_t)block_size
                                 : cbo * (uint32_t)block_size;
            stat_addr = stream ? segment_offset + lin
                               : get_data_ptr(d->regs[NV_PAPU_VPSGEADDR],
                                              0xFFFFFFFF, ba + lin);
        }
        if (adpcm) {
            unsigned int block_index = cbo / ADPCM_SAMPLES_PER_BLOCK;
            unsigned int block_position = cbo % ADPCM_SAMPLES_PER_BLOCK;
            if (adpcm_block_index != (int)block_index) {
                uint32_t linear_addr = block_index * (uint32_t)block_size;
                if (stream) {
                    hwaddr addr = segment_offset + linear_addr;
                    memcpy(adpcm_block, mcpx_apu_phys(addr),
                           block_size);
                } else {
                    linear_addr += ba;
                    for (unsigned int word_index = 0;
                         word_index < (9 * samples_per_block); word_index++) {
                        hwaddr addr = get_data_ptr(d->regs[NV_PAPU_VPSGEADDR],
                                                   0xFFFFFFFF, linear_addr);
                        adpcm_block[word_index] =
                            ldl_le_phys(address_space_memory, addr);
                        linear_addr += 4;
                    }
                }
                {
                    /* RECOMP_APU_ADPCM_PEEK: the first block a voice decodes,
                     * byte for byte, with where it was read -- to search for
                     * in the title's wave banks. */
                    static int peek = -1, peeks;
                    if (peek < 0)
                        peek = apu_env_on("RECOMP_APU_ADPCM_PEEK");
                    if (peek > 0 && !stream && block_index == 0 && peeks < 400) {
                        const uint8_t *bb = (const uint8_t *)adpcm_block;
                        hwaddr a0 = stream ? segment_offset
                                           : get_data_ptr(d->regs[NV_PAPU_VPSGEADDR],
                                                          0xFFFFFFFF, ba);
                        peeks++;
                        fprintf(stderr, "[APU-PEEK] v=%u %s ba=%08X phys=%08X %s:",
                                (unsigned)v, stream ? "stream" : "buffer", ba,
                                (unsigned)a0, mcpx_apu_phys_class(a0));
                        for (unsigned k = 0; k < (unsigned)block_size && k < 36; k++)
                            fprintf(stderr, " %02X", bb[k]);
                        fprintf(stderr, "\n");
                    }
                }
                adpcm_decode_block(adpcm_decoded, (uint8_t *)adpcm_block,
                                   block_size, channels);
                adpcm_block_index = block_index;
            }

            samples[sample_count][0] =
                int16_to_float(adpcm_decoded[block_position * channels]);
            if (stereo) {
                samples[sample_count][1] = int16_to_float(
                    adpcm_decoded[block_position * channels + 1]);
            }
        } else {
            hwaddr addr;
            if (stream) {
                addr = segment_offset + cbo * block_size;
            } else {
                uint32_t linear_addr = ba + cbo * (uint32_t)block_size;
                addr = get_data_ptr(d->regs[NV_PAPU_VPSGEADDR], 0xFFFFFFFF,
                                    linear_addr);
            }

            for (unsigned int channel = 0; channel < channels; channel++) {
                uint32_t ival;
                float fval;
                switch (sample_size) {
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_U8:
                    ival = ldub_phys(address_space_memory, addr);
                    fval = uint8_to_float((uint8_t)(ival & 0xff));
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16:
                    ival = lduw_le_phys(address_space_memory, addr);
                    fval = int16_to_float((int16_t)(ival & 0xffff));
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24:
                    ival = ldl_le_phys(address_space_memory, addr);
                    fval = int24_to_float(ival);
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S32:
                    ival = ldl_le_phys(address_space_memory, addr);
                    fval = int32_to_float(ival);
                    break;
                default:
                    fval = 0.0f;
                    break;
                }
                samples[sample_count][channel] = fval;
                addr += container_size;
            }
        }

        if (!stereo) {
            samples[sample_count][1] = samples[sample_count][0];
        }
    }

    vp_stat_samples(v, samples, sample_count, stat_addr,
                    voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                   0xFFFFFFFF));
    if (!stream && loop && !adpcm && sample_count > 0)
        vp_ring_check(d, v, ba, ebo, (uint32_t)block_size,
                      cbo - (uint32_t)sample_count, samples, sample_count);

    if (cbo >= ebo) {
        if (stream) {
            d->vp.ssl[v].ssl_seg += 1;
            cbo = 0;
            if (d->vp.ssl[v].ssl_seg < d->vp.ssl[v].count[ssl_index]) {
                /* Move to next segment */
            } else {
                int next_index = (ssl_index + 1) % 2;
                stream_trace(d, (uint16_t)v, "SSL %d done", ssl_index);
                d->vp.ssl[v].ssl_index = next_index;
                d->vp.ssl[v].ssl_seg = 0;
                set_notify_status(d, v, MCPX_HW_NOTIFIER_SSLA_DONE + ssl_index,
                                  NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
            }
        } else {
            if (loop) {
                cbo = lbo;
            } else {
                cbo = ebo;
                voice_off(d, (uint16_t)v);
            }
        }
    }

    voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                   NV_PAVS_VOICE_PAR_OFFSET_CBO, cbo);
    return sample_count;
}

/* ============================================================
 * Voice resampling (linear interpolation in place of libsamplerate)
 *
 * libsamplerate is stubbed; voice_resample below interpolates linearly
 * between source frames at the pitch's ratio, keeping its position and
 * the last frame across calls in the voice's filter state.
 * ============================================================ */

/* The linear interpolation below leaves images of the source spectrum at
 * every multiple of its sample rate. At 44.1 -> 48 kHz they are out of
 * hearing; a voice upsampled a lot is not. Ghost Recon 2's rumble is
 * recorded at 2 kHz and stretched 24x: the images at 2, 4, 6 kHz rang as a
 * metallic knocking over helicopters, explosions and rain, on whichever bins
 * the voice was sent to. So a voice upsampled 1.5x or more (a source below
 * 32 kHz) is low-passed at 0.45 of its own sample rate -- two Butterworth
 * sections, what a band-limited resampler would have done. */
static struct {
    float rate, b0, b1, b2, a1, a2;
    float z[2][2][2];                     /* [section][channel][state] */
} s_img[MCPX_HW_MAX_VOICES];

static void image_lowpass(uint16_t v, float rate, float samples[][2], int n)
{
    int i, s, c;

    if (v >= MCPX_HW_MAX_VOICES)
        return;
    if (s_img[v].rate != rate) {
        float fc = 0.45f * 48000.0f / rate, q = 0.7071f;
        float w = 2.0f * 3.14159265f * fc / 48000.0f, cw = cosf(w);
        float al = sinf(w) / (2.0f * q), a0 = 1.0f + al;
        if (s_img[v].rate == 0.0f)
            memset(s_img[v].z, 0, sizeof s_img[v].z);
        s_img[v].rate = rate;
        s_img[v].b0 = (1.0f - cw) / 2.0f / a0;
        s_img[v].b1 = (1.0f - cw) / a0;
        s_img[v].b2 = s_img[v].b0;
        s_img[v].a1 = -2.0f * cw / a0;
        s_img[v].a2 = (1.0f - al) / a0;
    }
    for (i = 0; i < n; i++)
        for (c = 0; c < 2; c++) {
            float x = samples[i][c];
            for (s = 0; s < 2; s++) {
                float *z = s_img[v].z[s][c];
                float y = s_img[v].b0 * x + z[0];
                z[0] = s_img[v].b1 * x - s_img[v].a1 * y + z[1];
                z[1] = s_img[v].b2 * x - s_img[v].a2 * y;
                x = y;
            }
            samples[i][c] = x;
        }
}

static int voice_resample(MCPXAPUState *d, uint16_t v, float samples[][2],
                          int requested_num, float rate)
{
    /* Linear interpolation in place of libsamplerate. `rate` is the output
     * to source ratio the VP's pitch encodes (libsamplerate's src_ratio):
     * 1.088 for 44.1 kHz material at the 48 kHz frame rate, so
     * each output consumes 1/rate source samples. Fetching one source sample
     * per output, as the stub before did, played everything a semitone and a
     * half high, 8.8 % too fast -- and drained each stream buffer faster than
     * the title filled it, which is where the periodic dropouts came from.
     *
     * State per voice: the two source samples the output sits between
     * (rs_last at 0, rs_next at 1) and the output's position rs_pos in
     * [0,1). Source samples arrive from voice_get_samples in the voice's own
     * units; a shortfall (the voice ended or its stream ran dry) returns the
     * outputs that could be made, and a fetch failure with nothing made is
     * passed through as it was. */
    MCPXAPUVoiceFilter *f = &d->vp.filters[v];
    float src[NUM_SAMPLES_PER_FRAME * 4 + 2][2];
    const int max_src = (int)(sizeof(src) / sizeof(src[0]));
    float step;
    int need, got, in, out;

    if (!(rate > 0.0f)) rate = 1.0f;
    step = 1.0f / rate;

    if (!f->rs_have) {
        got = voice_get_samples(d, v, src, 2);
        /* A first fetch that gets one sample returns no output and drops
         * that sample: the next call fetches two afresh. A voice whose
         * stream starts one sample at a time loses its first sample. */
        if (got < 2) return got < 0 ? got : 0;
        f->rs_last[0] = src[0][0]; f->rs_last[1] = src[0][1];
        f->rs_next[0] = src[1][0]; f->rs_next[1] = src[1][1];
        f->rs_pos = 0.0f;
        f->rs_have = 1;
    }

    /* Source samples the next requested_num outputs will step past, exactly:
     * output k sits at rs_pos + k * step, and every whole unit that position
     * reaches before the last output is a source sample consumed. The last
     * output's own step is left pending in rs_pos for the next call, so
     * nothing fetched here goes unused -- fetching floor(rs_pos + n * step)
     * instead over-read by up to one sample a frame, which was dropped: the
     * music came out 3 % fast. A step above about 4.2 (rate below 0.24, a
     * voice pitched more than two octaves up) wants more than the 130-frame
     * buffer holds over 31 steps; make fewer outputs rather than lose
     * source, so such a voice comes out short (and late) each frame. */
    for (;;) {
        need = (int)floorf(f->rs_pos + (float)(requested_num - 1) * step);
        if (need <= max_src || requested_num <= 1) break;
        requested_num /= 2;
    }
    if (need > max_src) need = max_src;
    if (need < 0) need = 0;

    got = need > 0 ? voice_get_samples(d, v, src, need) : 0;
    if (got < 0) got = 0;

    for (in = 0, out = 0; out < requested_num; out++) {
        while (f->rs_pos >= 1.0f) {
            if (in >= got) goto done;
            f->rs_last[0] = f->rs_next[0]; f->rs_last[1] = f->rs_next[1];
            f->rs_next[0] = src[in][0];    f->rs_next[1] = src[in][1];
            in++;
            f->rs_pos -= 1.0f;
        }
        samples[out][0] = f->rs_last[0] + f->rs_pos * (f->rs_next[0] - f->rs_last[0]);
        samples[out][1] = f->rs_last[1] + f->rs_pos * (f->rs_next[1] - f->rs_last[1]);
        f->rs_pos += step;
    }
done:
    if (rate >= 1.5f && out > 0)
        image_lowpass(v, rate, samples, out);
    if (out == 0 && got == 0) {
        /* Nothing new and nothing left to interpolate: the voice has stopped
         * or its stream is empty. voice_process treats 0 as an underrun and
         * a negative as the end; the voice's active bit says which. */
        int active = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                    NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
        return active ? 0 : -1;
    }
    return out;
}

/* ============================================================
 * Voice processing (main per-voice function)
 * ============================================================ */

static void voice_process(MCPXAPUState *d,
                          float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME],
                          float sample_buf[NUM_SAMPLES_PER_FRAME][2],
                          uint16_t v, int voice_list)
{
    assert(v < MCPX_HW_MAX_VOICES);
    bool stereo = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_STEREO) != 0;
    unsigned int channels = stereo ? 2 : 1;
    bool paused = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                 NV_PAVS_VOICE_PAR_STATE_PAUSED) != 0;

    struct McpxApuDebugVoice *dbg = &g_dbg.vp.v[v];
    dbg->active = true;
    dbg->stereo = stereo;
    dbg->paused = paused;

    /* RECOMP_APU_TRACE: a stream voice the title pauses and resumes (an XMV
     * player holding its audio while the video catches up) leaves silent
     * gaps in the mix; log each change with the VP frame it happened on. */
    if (s_vstat_on > 0 && voice_is_stream(d, v)) {
        static uint8_t s_was_paused[MCPX_HW_MAX_VOICES];
        static unsigned long s_paused_at[MCPX_HW_MAX_VOICES];
        if (paused != (s_was_paused[v] != 0)) {
            fprintf(stderr, "[APU] voice %u %s at VP frame %lu%s\n", (unsigned)v,
                    paused ? "paused" : "resumed", (unsigned long)s_vp_frames,
                    paused ? "" : "");
            if (paused)
                s_paused_at[v] = (unsigned long)s_vp_frames;
            else
                fprintf(stderr, "[APU] voice %u was paused %.1f ms\n", (unsigned)v,
                        (s_vp_frames - s_paused_at[v]) * 32 * 1000.0 / 48000.0);
            s_was_paused[v] = paused;
        }
    }

    if (paused) return;

    /* Step filter envelope */
    float ef_value = voice_step_envelope(
        d, v, NV_PAVS_VOICE_CFG_ENV1, NV_PAVS_VOICE_CFG_ENVF,
        NV_PAVS_VOICE_CFG_MISC, NV_PAVS_VOICE_CFG_MISC_EF_RELEASERATE,
        NV_PAVS_VOICE_PAR_NEXT, NV_PAVS_VOICE_PAR_NEXT_EFLVL,
        NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, NV_PAVS_VOICE_PAR_STATE_EFCUR);
    if (ef_value < 0.0f) ef_value = 0.0f;
    if (ef_value > 1.0f) ef_value = 1.0f;

    int16_t p = (int16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                                         NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH);
    int8_t ps = (int8_t)voice_get_mask(d, v, NV_PAVS_VOICE_CFG_ENV0,
                                        NV_PAVS_VOICE_CFG_ENV0_EF_PITCHSCALE);
    float rate = 1.0f / powf(2.0f, (p + ps * 32 * ef_value) / 4096.0f);
    dbg->rate = rate;

    /* Step amplitude envelope */
    float ea_value = voice_step_envelope(
        d, v, NV_PAVS_VOICE_CFG_ENV0, NV_PAVS_VOICE_CFG_ENVA,
        NV_PAVS_VOICE_TAR_LFO_ENV, NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE,
        NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_EALVL,
        NV_PAVS_VOICE_CUR_ECNT_EACOUNT, NV_PAVS_VOICE_PAR_STATE_EACUR);
    if (ea_value < 0.0f) ea_value = 0.0f;
    if (ea_value > 1.0f) ea_value = 1.0f;

    float samples[NUM_SAMPLES_PER_FRAME][2];
    memset(samples, 0, sizeof(samples));

    bool multipass = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_MULTIPASS) != 0;
    dbg->multipass = multipass;

    if (!multipass) {
        uint32_t ba = voice_get_mask(d, v, NV_PAVS_VOICE_CUR_PSL_START,
                                     NV_PAVS_VOICE_CUR_PSL_START_BA);
        ba_count(ba);
        if (voice_list == 2)
            s_mp_src_ba = ba;
    } else if (v < MCPX_HW_MAX_3D_VOICES) {
        s_3d_src_ba[v] = voice_list == 2 ? s_mp_src_ba : 0;
    }

    if (multipass) {
        /* Read from multipass bin */
        int mp_bin = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_MULTIPASS_BIN);
        dbg->multipass_bin = (uint8_t)mp_bin;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            samples[i][0] = mixbins[mp_bin][i];
            samples[i][1] = mixbins[mp_bin][i];
        }
        if (s_vstat_on > 0) {
            s_mp_reads++;
            if (s_mp_writes[mp_bin] == 0) s_mp_empty++;
            if (s_mp_writes[mp_bin] > 1) s_mp_multi++;
            s_mp_writes[mp_bin] = 0;
            s_vstat[v].mpbin = mp_bin;
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
                s_vstat[v].emp += (double)samples[i][0] * samples[i][0];
        }
        bool clear_mix = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                        NV_PAVS_VOICE_CFG_FMT_CLEAR_MIX) != 0;
        if (clear_mix) {
            memset(&mixbins[mp_bin][0], 0, sizeof(mixbins[0]));
        }
    } else {
        int empty = 0;
        for (int sample_count = 0; sample_count < NUM_SAMPLES_PER_FRAME;) {
            int active = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                        NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
            if (!active) return;
            int count = voice_resample(d, v, &samples[sample_count],
                                       NUM_SAMPLES_PER_FRAME - sample_count, rate);
            if (count < 0) break;
            /* An active voice that keeps producing nothing is an underrun,
             * not a reason to spin. Asking it again at once cannot change the
             * answer, and this loop holds d->lock: Burnout 3's vehicle select
             * sat here indefinitely while the title waited in voice_lock. */
            if (count == 0 && ++empty > 4) break;
            sample_count += count;
        }
    }

    int active = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
    if (!active) return;

    /* RECOMP_APU_TRACE: a stream voice whose buffer ran dry (the title has
     * not queued the next one) is silent for as long as it stays dry. Each
     * run of 8 frames (5 ms) or more is named, with where it ended. */
    if (s_vstat_on > 0 && !multipass && voice_is_stream(d, v)) {
        static unsigned s_dry_run[MCPX_HW_MAX_VOICES];
        bool dry = true;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME && dry; i++)
            dry = samples[i][0] == 0.0f && samples[i][1] == 0.0f;
        if (dry) {
            s_dry_run[v]++;
        } else {
            if (s_dry_run[v] >= 8)
                fprintf(stderr, "[APU] voice %u was dry %u frames (%.1f ms), ended at VP frame %lu\n",
                        (unsigned)v, s_dry_run[v], s_dry_run[v] * 32 * 1000.0 / 48000.0,
                        (unsigned long)s_vp_frames);
            s_dry_run[v] = 0;
        }
    }

    /* Get volume bins */
    int bin[8];
    bin[0] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V0BIN);
    bin[1] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V1BIN);
    bin[2] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V2BIN);
    bin[3] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V3BIN);
    bin[4] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V4BIN);
    bin[5] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V5BIN);
    bin[6] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_V6BIN);
    bin[7] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_V7BIN);

    if (v < MCPX_HW_MAX_3D_VOICES) {
        bin[0] = d->vp.hrtf_submix[0];
        bin[1] = d->vp.hrtf_submix[1];
        bin[2] = d->vp.hrtf_submix[2];
        bin[3] = d->vp.hrtf_submix[3];
    }

    uint16_t vol[8];
    vol[0] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME0);
    vol[1] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME1);
    vol[2] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME2);
    vol[3] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME3);
    vol[4] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME4);
    vol[5] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME5);
    vol[6] = (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME6_B11_8) << 8);
    vol[6] |= (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME6_B7_4) << 4);
    vol[6] |= (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME6_B3_0);
    vol[7] = (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME7_B11_8) << 8);
    vol[7] |= (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME7_B7_4) << 4);
    vol[7] |= (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME7_B3_0);

    for (int i = 0; i < 8; i++) {
        dbg->bin[i] = (uint8_t)bin[i];
        dbg->vol[i] = vol[i];
    }
    if (s_vstat_on > 0) {
        s_vstat[v].ea = ea_value;
        s_vstat[v].rate = rate;
        s_vstat[v].vol0 = vol[0]; s_vstat[v].vol1 = vol[1];
        s_vstat[v].bin0 = (uint8_t)bin[0]; s_vstat[v].bin1 = (uint8_t)bin[1];
    }

    if (voice_should_mute(v)) return;

    /* Low-pass filter */
    int fmode = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_MISC,
                               NV_PAVS_VOICE_CFG_MISC_FMODE);
    bool lpf = false;
    if (v < MCPX_HW_MAX_3D_VOICES) {
        lpf = (fmode == 1);
    } else {
        lpf = stereo ? (fmode == 1) : (fmode & 1) != 0;
    }
    if (lpf) {
        for (int ch = 0; ch < 2; ch++) {
            int16_t fc = (int16_t)voice_get_mask(
                d, v, NV_PAVS_VOICE_TAR_FCA + (ch % channels) * 4,
                NV_PAVS_VOICE_TAR_FCA_FC0);
            float fc_f = clampf(powf(2, fc / 4096.0f), 0.003906f, 1.0f);
            uint16_t q = (uint16_t)voice_get_mask(
                d, v, NV_PAVS_VOICE_TAR_FCA + (ch % channels) * 4,
                NV_PAVS_VOICE_TAR_FCA_FC1);
            float q_f = clampf(q / (1.0f * 0x8000), 0.079407f, 1.0f);
            sv_filter *filter = &d->vp.filters[v].svf[ch];
            setup_svf(filter, fc_f, q_f, F_LP);
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                samples[i][ch] = run_svf(filter, samples[i][ch]);
                samples[i][ch] = fminf(fmaxf(samples[i][ch], -1.0f), 1.0f);
            }
        }
    }

    /* A 3D voice fed by a sample many voices are playing (the rain). */
    bool dense = v < MCPX_HW_MAX_3D_VOICES && multipass && s_3d_src_ba[v] &&
                 ba_voices(s_3d_src_ba[v]) >= APU_DENSE_VOICES;
    if (multipass && v < MCPX_HW_MAX_3D_VOICES)
        dense_delay(v, samples, dense);

    /* HRTF processing for 3D voices */
    if (v < MCPX_HW_MAX_3D_VOICES && apu_hrtf_mode() != APU_HRTF_OFF) {
        uint16_t hrtf_handle =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CFG_HRTF_TARGET,
                                     NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE);
        if (hrtf_handle != HRTF_NULL_HANDLE) {
            int mode = apu_hrtf_mode();
            if (mode == APU_HRTF_DENSE)
                mode = dense ? APU_HRTF_FULL : APU_HRTF_OFF;
            if (s_vstat_on > 0 && mode != APU_HRTF_OFF)
                s_vstat[v].hrtf++;
            if (mode == APU_HRTF_FULL)
                hrtf_filter_process(&d->vp.filters[v].hrtf, samples, samples);
            else if (mode == APU_HRTF_ITD)
                hrtf_filter_process_itd(&d->vp.filters[v].hrtf, samples, samples);
        }
    }

    /* Mix into bins */
    for (int b = 0; b < 8; b++) {
        float g = ea_value;
        float hr;
        if ((v < MCPX_HW_MAX_3D_VOICES) && (b < 4)) {
            hr = (float)(1 << d->vp.hrtf_headroom);
        } else {
            hr = (float)(1 << d->vp.submix_headroom[bin[b]]);
        }
        g *= attenuate(vol[b]) / hr;
        if (dense)
            g *= xbox_AudioGetAmbience();
        {
            /* Ramp from the gain this voice had on this output last frame to
             * the new one, across the frame, instead of stepping to it. The
             * VP moves a voice's current volume towards its target; taking
             * the target at once made every DirectSound 3D update (each
             * ~16 ms, new volumes for all the positioned voices) a step in
             * the gain of every 3D voice -- zipper noise at 62.5 Hz and its
             * harmonics, the metallic static over Ghost Recon 2's rain and
             * engines. A voice not mixed last frame starts at its gain. */
            float g0 = (s_gain_frame[v] + 1 == s_vp_frames) ? s_gain[v][b] : g;
            if (g > 0.0f && bin[b] >= 10)
                s_mp_writes[bin[b]]++;
            float dg = (g - g0) / (float)NUM_SAMPLES_PER_FRAME;
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                g0 += dg;
                mixbins[bin[b]][i] += g0 * samples[i][b % channels];
            }
            s_gain[v][b] = g;
        }
        if (s_vstat_on > 0 && bin[b] >= 6 && bin[b] <= 9)
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                float y = g * samples[i][b % channels];
                s_vstat[v].e3d += (double)y * y;
            }
    }

    /* VP monitor mix */
    if (d->monitor.point == MCPX_APU_DEBUG_MON_VP) {
        float g = 0.0f;
        for (int b = 0; b < 8; b++) {
            float hr = (float)(1 << d->vp.submix_headroom[bin[b]]);
            float bg = attenuate(vol[b]) / hr;
            if (bg > g) g = bg;
        }
        g *= ea_value;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            sample_buf[i][0] += g * samples[i][0];
            sample_buf[i][1] += g * samples[i][1];
        }
    }

    /* The flat mix xemu plays by default (its VP monitor, use_dsp off), for
     * RECOMP_APU_MIXDOWN=vp: each voice once, at the largest of its volumes,
     * into a stereo pair -- not summed over the bins it feeds. A voice that
     * feeds a submix (the bin the next multipass voice in the MP list reads)
     * leaves that bin out, as the submix comes in once through the multipass
     * voice. Without a GP DSP to process them, summing the 3D bins (6+8, 7+9)
     * as speakers gave Ghost Recon 2's rain and engines a loud metallic
     * static that xemu, which does this instead, does not have. */
    if (apu_vp_flat_on()) {
        int mp_skip = -1;
        float g = 0.0f;
        if (voice_list == 2) {                       /* MP list: next multipass voice */
            uint16_t n = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                                       NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
            for (int k = 0; n != 0xFFFF && k < MCPX_HW_MAX_VOICES; k++) {
                if (voice_get_mask(d, n, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_MULTIPASS)) {
                    mp_skip = (int)voice_get_mask(d, n, NV_PAVS_VOICE_CFG_FMT,
                                                  NV_PAVS_VOICE_CFG_FMT_MULTIPASS_BIN);
                    break;
                }
                n = (uint16_t)voice_get_mask(d, n, NV_PAVS_VOICE_TAR_PITCH_LINK,
                                 NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
            }
        }
        for (int b = 0; b < 8; b++) {
            float hr, bg;
            if (bin[b] == mp_skip)
                continue;
            hr = (float)(1 << d->vp.submix_headroom[bin[b]]);
            bg = attenuate(vol[b]) / hr;
            if (bg > g) g = bg;
        }
        g *= ea_value;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            g_vp_flat[i][0] += g * samples[i][0];
            g_vp_flat[i][1] += g * samples[i][1];    /* mono voices carry it in both */
        }
    }

    s_gain_frame[v] = s_vp_frames;
    (void)voice_list;
}

/* ============================================================
 * VP Frame - Process all voice lists
 *
 * Simplified single-threaded version (no worker threads initially)
 * ============================================================ */

/* RECOMP_APU_TRACE: every 60 VP frames, each active voice's play offset and
 * end offset, in samples (CBO and EBO as the VP holds them). A title that
 * times video or game events off a voice's play position follows this
 * clock; set against the flip count it shows whether audio runs fast or
 * slow relative to the frames. */
static unsigned s_pos_frames;

static void vp_trace_positions(MCPXAPUState *d)
{
    if (s_vstat_on < 0)
        vp_stat_init();
    if (s_vstat_on <= 0 || ++s_pos_frames % 60)
        return;

    for (int list = 0; list < 3; list++) {
        uint32_t h = d->regs[voice_list_regs[list].top];
        for (int i = 0; h != 0xFFFF && i < MCPX_HW_MAX_VOICES; i++) {
            uint16_t v = (uint16_t)h;
            if (voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                               NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE))
                fprintf(stderr, "[APU] voice %u pos=%u/%u\n", (unsigned)v,
                        voice_get_mask(d, v, NV_PAVS_VOICE_PAR_OFFSET,
                                       NV_PAVS_VOICE_PAR_OFFSET_CBO),
                        voice_get_mask(d, v, NV_PAVS_VOICE_PAR_NEXT,
                                       NV_PAVS_VOICE_PAR_NEXT_EBO));
            h = voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                               NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
        }
    }
}

void mcpx_apu_vp_frame(MCPXAPUState *d,
                        float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    static uint16_t idle_v[MCPX_HW_MAX_VOICES];   /* idle voices still listed */
    static int idle_list[MCPX_HW_MAX_VOICES];
    int n_idle = 0;

    memset(d->vp.sample_buf, 0, sizeof(d->vp.sample_buf));
    memset(g_vp_flat, 0, sizeof g_vp_flat);
    s_vp_frames++;
    ba_frame_start();

    {
        /* RECOMP_APU_LISTS=<frame>: the three voice lists once, in order,
         * with each voice's format, bins and multipass source -- how the
         * title pairs a submix source with the voice that reads it. */
        static long at = -2;
        if (at == -2) {
            const char *e = getenv("RECOMP_APU_LISTS");
            at = e ? atol(e) : -1;
        }
        if (at > 0 && s_vp_frames == (unsigned long)at) {
            static const char *ln[3] = { "2D", "3D", "MP" };
            for (int list = 0; list < 3; list++) {
                uint32_t h = d->regs[voice_list_regs[list].top];
                fprintf(stderr, "[APU-LIST] %s:", ln[list]);
                for (int i = 0; h != 0xFFFF && i < MCPX_HW_MAX_VOICES; i++) {
                    uint16_t v = (uint16_t)h;
                    uint32_t fmt = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFF);
                    uint32_t vb = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, 0xFFFFFFFF);
                    int act = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                             NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE) != 0;
                    fprintf(stderr, " %u%s(f%08X b%u,%u%s)", (unsigned)v, act ? "" : "-",
                            fmt, vb & 31, (vb >> 5) & 31,
                            (fmt & NV_PAVS_VOICE_CFG_FMT_MULTIPASS)
                                ? (fmt & NV_PAVS_VOICE_CFG_FMT_CLEAR_MIX ? " MPc" : " MP") : "");
                    h = voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                                       NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
                }
                fprintf(stderr, "\n");
            }
            fflush(stderr);
        }
    }

    for (int list = 0; list < 3; list++) {
        hwaddr top, current, next;
        top = voice_list_regs[list].top;
        current = voice_list_regs[list].current;
        next = voice_list_regs[list].next;

        d->regs[current] = d->regs[top];

        for (int i = 0; d->regs[current] != 0xFFFF; i++) {
            if (i >= MCPX_HW_MAX_VOICES) {
                DPRINTF("Voice list contains invalid entry!\n");
                break;
            }

            uint16_t v = (uint16_t)d->regs[current];
            d->regs[next] = voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                               NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);

            if (!voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE)) {
                if (n_idle < MCPX_HW_MAX_VOICES) {
                    idle_v[n_idle] = v;
                    idle_list[n_idle++] = list;
                }
            } else {
                /* Process voice directly (single-threaded) */
                voice_process(d, mixbins, d->vp.sample_buf, v, list);
            }
            d->regs[current] = d->regs[next];
        }
    }

    /* Idle voices still on a list: report one to the driver, which takes it
     * off (SE2FE_IDLE_VOICE traps the front end; DirectSound's ISR reads
     * FEDECPARAM and unlinks the voice).
     *
     * One a frame: a second trap in the same frame would overwrite
     * FEDECPARAM before the ISR has read the first. xemu traps for every
     * idle voice, so the last one of the frame is the one the ISR sees.
     *
     * Not always the first, though. DirectSound acts on the idle report of a
     * multi-voice client only for its last voice -- a 4-channel stream is
     * two voices, and it unlinks both when the second is reported. Always
     * reporting the first idle voice reported 216 of Ghost Recon 2's music
     * stream (216 + 217) every frame for ever; 217 never got a turn, the
     * driver never took the pair off the list, and the mission load that
     * had stopped the music waited for it with no end. Taking the idle
     * voices in turn reaches every one of them. */
    if (n_idle > 0
        && (d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE)
               != NV_PAPU_FECTL_FEMETHMODE_TRAPPED) {
        static unsigned rr;
        static int idle_logs = 200;
        int k = (int)(rr++ % (unsigned)n_idle);
        uint16_t v = idle_v[k];

        if (idle_logs > 0 && voice_is_stream(d, v)) {
            idle_logs--;
            stream_trace(d, v, "idle in list %d -> trap", idle_list[k]);
        }
        {
            /* One voice idle and trapped for a whole second: the driver is
             * not taking it off. Name it. */
            static uint16_t last_v = 0xFFFF;
            static unsigned repeats;
            if (n_idle == 1 && v == last_v) {
                if (++repeats == 1500)
                    fprintf(stderr, "[APU] voice %u idle in list %d, trapped"
                            " 1500 frames running: CFG_FMT=%08X PAR_STATE=%08X\n",
                            (unsigned)v, idle_list[k],
                            voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFF),
                            voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE, 0xFFFFFFFF));
            } else {
                last_v = v;
                repeats = 0;
            }
        }
        fe_method(d, SE2FE_IDLE_VOICE, v);
    }

    vp_trace_positions(d);
    vp_stat_report();
    if (s_ring_on > 0 && ++s_ring_frames % 1500 == 0)
        vp_ring_report(++s_ring_secs);

    /* VP monitor output */
    if (d->monitor.point == MCPX_APU_DEBUG_MON_VP) {
        int16_t isamp[NUM_SAMPLES_PER_FRAME * 2];
        src_float_to_short_array((float *)d->vp.sample_buf, isamp,
                                 NUM_SAMPLES_PER_FRAME * 2);
        int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            d->monitor.frame_buf[off + i][0] += isamp[2 * i];
            d->monitor.frame_buf[off + i][1] += isamp[2 * i + 1];
        }
        memset(d->vp.sample_buf, 0, sizeof(d->vp.sample_buf));
        memset(mixbins, 0, sizeof(float) * NUM_MIXBINS * NUM_SAMPLES_PER_FRAME);
    }
}

/* ============================================================
 * VP Init / Finalize / Reset
 * ============================================================ */

void mcpx_apu_vp_init(MCPXAPUState *d)
{
    /* Single-threaded - no worker dispatch needed */
    (void)d;
}

/* Shutdown, after the frame thread has stopped: close the voice dumps, free
 * the ring monitors and clear the statistics, so a new instance reads the
 * switches again and starts from nothing. */
void mcpx_apu_vp_finalize(MCPXAPUState *d)
{
    (void)d;
    if (s_vdump)
        fclose(s_vdump);
    s_vdump = NULL;
    for (unsigned v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        if (s_vdump_all[v])
            fclose(s_vdump_all[v]);
        s_vdump_all[v] = NULL;
        free(s_ring[v]);
        s_ring[v] = NULL;
    }
    memset(s_vdump_failed, 0, sizeof(s_vdump_failed));
    memset(s_vstat, 0, sizeof(s_vstat));
    s_vstat_on = -1;
    s_vdump_voice = -1;
    s_vstat_frames = s_vstat_secs = s_pos_frames = 0;
    s_ring_on = -1;
    s_ring_logged = 0;
    s_ring_frames = s_ring_secs = 0;
}

void mcpx_apu_vp_reset(MCPXAPUState *d)
{
    d->vp.ssl_base_page = 0;
    d->vp.hrtf_headroom = 0;
    memset(d->vp.ssl, 0, sizeof(d->vp.ssl));
    memset(d->vp.hrtf_submix, 0, sizeof(d->vp.hrtf_submix));
    memset(d->vp.submix_headroom, 0, sizeof(d->vp.submix_headroom));
    memset(d->vp.voice_locked, 0, sizeof(d->vp.voice_locked));
    for (int v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        hrtf_filter_init(&d->vp.filters[v].hrtf);
    }
}
