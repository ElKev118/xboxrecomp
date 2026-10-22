"""Test-wide settings for the recompiler.

Most tests assert on the exact C the lifter writes, and they were written for
guest registers as globals. RECOMP_LOCAL_REGS output (the default for real
generation) wraps calls and returns, which changes that text without changing
what it does. So the suite runs in global-register mode unless
RECOMP_TEST_LOCAL_REGS=1 is set, which is how the compile-and-run tests are
pointed at local-register output (test_local_regs.py covers its shape).
"""
import os

import pytest

from . import translator


@pytest.fixture(autouse=True)
def _register_mode(monkeypatch):
    local = os.environ.get("RECOMP_TEST_LOCAL_REGS") == "1"
    monkeypatch.setattr(translator, "LOCAL_REGS", local)
    yield
