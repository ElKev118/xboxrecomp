"""Guest registers as C locals (RECOMP_LOCAL_REGS).

The shape tests check what _localize_regs writes; the runtime test lifts a
caller, a callee and a tail call, compiles them against the real runtime
header in local-register mode and checks the registers cross the calls.
"""
import subprocess
import tempfile
from pathlib import Path

import pytest

from tools.recomp import config, translator
from tools.recomp.c_fixture import command, compiler
from tools.recomp.translator import FunctionTranslator, _localize_regs

HEADER_DIR = Path(__file__).resolve().parents[2] / "templates" / "runtime"
BASE = 0x10000


def _body(lines, names=()):
    """_localize_regs output without the per-function declarations."""
    out = _localize_regs(["{"] + lines, set(names))
    return [l for l in out[1:] if not l.startswith(("#", "    uint32_t l_",
                                                     "    RecompXmm l_",
                                                     "    const uintptr_t l_mem"))]


def test_only_named_registers_are_copied():
    out = _localize_regs(["{", "    eax = MEM32(esp + 4); xmm1 = xmm0; /* ecx */"], set())
    assert "    uint32_t l_eax = g_eax, l_esp = g_esp;" in out
    assert "    RecompXmm l_xmm0 = g_xmm0, l_xmm1 = g_xmm1;" in out
    assert ("#define RECOMP_REGS_OUT() (g_eax = l_eax, g_esp = l_esp, "
            "g_xmm0 = l_xmm0, g_xmm1 = l_xmm1, (void)0)") in out
    assert not any("l_ecx" in l for l in out)          # named only in a comment
    assert out[-2:] == ["#undef RECOMP_REGS_OUT", "#undef RECOMP_REGS_IN"]


def test_direct_call_is_published_and_reloaded():
    body = _body(["    PUSH32(esp, 1); sub_00012345(); /* x */"])
    assert ("RECOMP_REGS_OUT(); sub_00012345(); RECOMP_REGS_IN();" in body[0]
            and body[0].endswith("/* x */"))


def test_tail_call_returns_the_callee_registers():
    body = _body(["    g_seh_ebp = ebp; sub_00012345(); return; /* tail jmp */"])
    assert "RECOMP_REGS_OUT(); sub_00012345(); return;" in body[0]
    assert "RECOMP_REGS_IN" not in body[0] and "RECOMP_RETURN" not in body[0]


def test_return_publishes():
    body = _body(["    esp += 4; return; /* ret */", "    if (x) return;"])
    assert body[0] == "    esp += 4; RECOMP_RETURN(); /* ret */"
    assert body[1] == "    if (x) RECOMP_RETURN();"


def test_named_callee_and_comments_untouched():
    body = _body(["    memset_00011111(); /* return; */"], {"memset_00011111"})
    assert body[0] == "    RECOMP_REGS_OUT(); memset_00011111(); RECOMP_REGS_IN(); /* return; */"


def test_setjmp_landing_reloads_and_longjmp_publishes():
    sj = ("    { int _sjv = setjmp(*recomp_setjmp_slot(MEM32(esp))); if (_sjv) "
          "{ ebp = g_seh_ebp; eax = (uint32_t)_sjv; } else { PUSH32(esp, 1u); "
          "sub_003B9484(); } }")
    lj = "    if (!recomp_guest_longjmp(MEM32(esp), MEM32(esp + 4))) { sub_003B9484(); }"
    body = _body([sj, lj])
    assert "if (_sjv) { RECOMP_REGS_IN(); ebp = g_seh_ebp;" in body[0]
    assert "RECOMP_REGS_OUT(); sub_003B9484(); RECOMP_REGS_IN();" in body[0]
    assert body[1].startswith("    RECOMP_REGS_OUT(); if (!recomp_guest_longjmp(")


# caller (0x10000): mov ecx,5; push 37; call 0x10100; inc eax; ret
# callee (0x10100): mov eax,[esp+4]; add eax,ecx; ret 4
# tail   (0x10200): mov ecx,1; jmp 0x10100
def _image():
    img = bytearray(0x300)
    caller = bytes.fromhex("b905000000 6a25 e8") + (0x100 - 12).to_bytes(4, "little", signed=True) \
        + bytes.fromhex("40 c3")
    img[0:len(caller)] = caller
    img[0x100:0x109] = bytes.fromhex("8b442404 01c8 c20400")
    tail = bytes.fromhex("b901000000 e9") + (0x100 - 0x20A).to_bytes(4, "little", signed=True)
    img[0x200:0x200 + len(tail)] = tail
    return bytes(img)


HARNESS = r"""
#define RECOMP_GENERATED_CODE
#define RECOMP_LOCAL_REGS
#include "recomp_types.h"
#include <stdio.h>
#include <string.h>
RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi;
RECOMP_TLS uint32_t g_ebp, g_seh_ebp;
RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3, g_xmm4, g_xmm5, g_xmm6, g_xmm7;
ptrdiff_t g_xbox_mem_offset;
static uint8_t ram[0x10000];
static void push(uint32_t v) { g_esp -= 4; memcpy(ram + g_esp, &v, 4); }
void sub_00010000(void);
void sub_00010100(void);
void sub_00010200(void);
BODIES
int main(void) {
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)ram;
    g_esp = 0x8000; g_ebx = 0x1234;
    push(0xAAAA);                      /* return address */
    sub_00010000();
    if (g_eax != 43 || g_esp != 0x8000 || g_ecx != 5 || g_ebx != 0x1234) {
        printf("call: eax=%u esp=%X ecx=%u\n", g_eax, g_esp, g_ecx); return 1;
    }
    push(100); push(0xBBBB);  /* arg, return address */
    sub_00010200();
    if (g_eax != 101 || g_esp != 0x8000) {
        printf("tail: eax=%u esp=%X\n", g_eax, g_esp); return 2;
    }
    printf("OK\n");
    return 0;
}
"""


def test_registers_cross_calls_in_local_mode(monkeypatch):
    cc = compiler()
    if not cc:
        pytest.skip("C compiler unavailable")
    monkeypatch.setattr(translator, "LOCAL_REGS", True)
    image = _image()
    config._install([config.Section(".text", BASE, len(image), 0, len(image), True)],
                    entry_point=BASE, kernel_thunk_addr=BASE, origin="local-regs-test")
    db = {BASE: {"end": BASE + 14, "size": 14},
          BASE + 0x100: {"end": BASE + 0x109, "size": 9},
          BASE + 0x200: {"end": BASE + 0x20A, "size": 10}}
    tr = FunctionTranslator(image, db)
    bodies = "\n".join(tr.translate_function(a, db[a]) for a in sorted(db))
    assert "l_mem" in bodies
    with tempfile.TemporaryDirectory(prefix="recomp-localregs-") as tmp:
        src, exe = Path(tmp) / "t.c", Path(tmp) / "t.exe"
        src.write_text(HARNESS.replace("BODIES", bodies), encoding="utf-8")
        built = subprocess.run(command(cc, [src], exe, include_dirs=[HEADER_DIR]),
                               cwd=tmp, capture_output=True, text=True)
        assert built.returncode == 0, (built.stdout + built.stderr)[-4000:] + bodies
        ran = subprocess.run([str(exe)], cwd=tmp, capture_output=True, text=True)
        assert ran.returncode == 0, ran.stdout + bodies


def test_mmx_registers_are_local():
    out = _localize_regs(["{", "    mm0 = MMX_PADDW(mm0, mm1);"], set())
    assert "    RecompMmx l_mm0 = g_mm0, l_mm1 = g_mm1;" in out
    assert any("g_mm0 = l_mm0" in l and l.startswith("#define RECOMP_REGS_OUT") for l in out)


def test_x87_stack_is_local_and_synced():
    body = ["{", "    #define fp_top() g_fp_stack[g_fp_top]",
            "    fp_top() = fp_top() * g_fp_stack[(g_fp_top + 2) & 7]; /* g_fp_top */",
            "    sub_00012345();"]
    out = _localize_regs(body, set())
    assert "    double l_fp_stack[8]; int l_fp_top = g_fp_top;" in out
    assert "    #define fp_top() l_fp_stack[l_fp_top]" in out
    assert any("l_fp_stack[(l_fp_top + 2) & 7]" in l for l in out)
    assert any(l.startswith("#define RECOMP_REGS_OUT") and "memcpy(g_fp_stack, l_fp_stack" in l
               for l in out)


def test_mmio_accessors_rewrite_reads_and_writes():
    from tools.recomp.translator import _mmio_accessors
    out = _mmio_accessors([
        "    MEM32(eax + 0x20010) = MEM32(ecx + 4) | 1; /* MEM32(x) = y */",
        "    edx = MEM8(esi) + MEM16(edi + 2);",
        "    if (MEM32(eax) == 3) goto loc_1;",
    ])
    assert out[0] == "    MMIO_W32(eax + 0x20010, MMIO_R32(ecx + 4) | 1); /* MEM32(x) = y */"
    assert out[1] == "    edx = MMIO_R8(esi) + MMIO_R16(edi + 2);"
    assert out[2] == "    if (MMIO_R32(eax) == 3) goto loc_1;"
