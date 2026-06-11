#!/usr/bin/env python3
"""Phase 1 reproduction harness — now a re-export of tests/lib/harness.py
(Phase 3a consolidation). The phase-1 repros keep importing from here."""

import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)

from tests.lib.harness import wait_ready, emulator, connect  # noqa: E402,F401
from tests.lib.recontrol_client import ReControlClient  # noqa: E402,F401
