"""
Smoke test for the CP_TEST_CONSOLE serial harness.

Boots (or reconnects to an already-booted device), reads the current
activity, presses BACK, and asserts the device visibly reacted: either the
active activity's name changed, or -- for a screen that only opens an
overlay/menu on Back -- the framebuffer hash changed. Either signal is
enough; a live device should always redraw *something* in response to a
button. BACK is used because it is meaningful in every activity (exits the
reader to Home, closes a submenu, etc.), unlike a direction button whose
effect depends on the current screen and button-layout settings.

Run via:  python3 scripts/device_test.py [--port ...] scripts/device_tests/test_smoke.py
Or let scripts/device_test.py discover it automatically (no scenario args).
"""

from __future__ import annotations

import time

from device_test import DeviceTestConsole, DeviceTestError

BOOT_TIMEOUT_S = 30.0
RENDER_SETTLE_S = 1.0


def wait_for_boot(device: DeviceTestConsole, timeout: float = BOOT_TIMEOUT_S) -> str:
    """Poll CMD:ACTIVITY until the console answers. Covers a cold boot (the
    device may still be initializing display/SD when the harness attaches)
    as well as a reconnect after the device reset for some other reason."""
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            return device.activity(timeout=2.0)
        except DeviceTestError as e:
            last_error = e
            time.sleep(0.5)
    raise DeviceTestError(f"Device never answered CMD:ACTIVITY within {timeout}s ({last_error})")


def run(device: DeviceTestConsole) -> None:
    activity_before = wait_for_boot(device)
    fbhash_before = device.fbhash()

    device.press("BACK")
    time.sleep(RENDER_SETTLE_S)  # let the render task finish redrawing

    activity_after = device.activity()
    fbhash_after = device.fbhash()

    if activity_after == activity_before and fbhash_after == fbhash_before:
        raise DeviceTestError(
            f"CMD:PRESS BACK had no visible effect: activity stayed {activity_before!r}, "
            f"fbhash stayed {fbhash_before!r}"
        )
