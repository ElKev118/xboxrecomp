"""A dense switch can have more than 256 cases.

Ghost Recon 2's message handler (sub_0024A2B0) bounds its index with
`cmp eax, 0x128` and dispatches through a 297-slot table. Both the lifter
and the translator stopped reading tables at 256 slots, so the last 41 arms
had no label: selecting one became an indirect tail call to an address with
no generated body, the runtime could not resolve it, and the handler
returned without running the case -- a crash a few seconds into a mission.
"""
import struct
from types import SimpleNamespace

import pytest

from . import config
from .lifter import Lifter, MAX_JUMP_TABLE_ENTRIES

BASE = 0x10000
SLOTS = 297
FUNC_SIZE = 0x1000
TABLE = BASE + FUNC_SIZE         # MSVC places the table after the code


@pytest.fixture(autouse=True)
def layout(monkeypatch):
    monkeypatch.setattr(config, "_SECTIONS", [
        config.Section(".text", BASE, 0x2000, 0, 0x2000, True),
    ])


def _lifter():
    data = bytearray(b"\xcc" * 0x2000)
    for i in range(SLOTS):
        struct.pack_into("<I", data, TABLE - BASE + 4 * i, BASE + 8 * i)
    lifter = Lifter()
    lifter.xbe_data = bytes(data)
    lifter.func_start, lifter.func_end = BASE, BASE + FUNC_SIZE
    return lifter


def test_cap_covers_a_297_case_switch():
    assert MAX_JUMP_TABLE_ENTRIES >= SLOTS


def test_every_slot_of_a_long_table_is_an_arm():
    op = SimpleNamespace(type="mem", mem_disp=TABLE, mem_index="eax",
                         mem_base=None)
    arms = _lifter()._analyze_switch_table([op])
    assert len(arms) == SLOTS
    assert arms[-1] == BASE + 8 * (SLOTS - 1)
