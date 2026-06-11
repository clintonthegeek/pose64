#!/usr/bin/env python3
"""Phase 1 reproduction harness — canonical copy, lives in tests/lib/.

Self-launches a pose64 instance (headless via QT_QPA_PLATFORM=offscreen) on a
dedicated port, waits until the ReControl server answers `state` with OK, yields
a connected ReControlClient, and tears the process down cleanly.

Used by every tests/phase1/repro_*.py so reproductions are standalone and
effect-based (recovery-plan rule R3).
"""

import contextlib
import os
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)

from tests.lib.recontrol_client import ReControlClient  # noqa: E402


def wait_ready(port, timeout=25):
    """Poll-connect until the ReControl server answers `state` with OK."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        c = ReControlClient(port=port, timeout=2)
        if c.connect():
            try:
                r = c.send_command("state")
            finally:
                c.disconnect()
            if r and r.startswith("OK"):
                return True
        time.sleep(0.4)
    return False


@contextlib.contextmanager
def emulator(port, psf="m515.psf", build="build", env_extra=None, capture_log=None,
             extra_args=None):
    """Launch pose64 on `port`, yield the Popen handle, guarantee teardown."""
    exe = os.path.join(REPO, build, "pose64")
    if not os.path.isfile(exe):
        raise RuntimeError(f"executable not found: {exe}")

    env = dict(os.environ)
    env.setdefault("QT_QPA_PLATFORM", "offscreen")
    if env_extra:
        env.update(env_extra)

    cmd = [exe, "-psf", os.path.join(REPO, psf), "--port", str(port)]
    if extra_args:
        cmd += list(extra_args)

    log = open(capture_log, "w") if capture_log else subprocess.DEVNULL
    proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, env=env)
    try:
        if not wait_ready(port):
            raise RuntimeError(f"server not ready on port {port}")
        yield proc
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        if capture_log:
            log.close()


def connect(port, timeout=5):
    c = ReControlClient(port=port, timeout=timeout)
    if not c.connect():
        raise RuntimeError(f"could not connect to {port}")
    return c
