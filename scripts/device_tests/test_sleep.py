"""
Deep-sleep scenario for the CP_TEST_CONSOLE serial harness.

CMD:HOLD POWER <ms> cannot exercise the sleep path: the power-button hold
trigger in main.cpp's loop() reads HalGPIO directly rather than
MappedInputManager, so a console-injected hold is invisible to it (and a
device with its sleep timeout set to "never" has no auto-sleep fallback
either). CMD:SLEEP instead calls enterDeepSleep() directly from the command
handler, so this is the only harness path that reaches
SleepActivity::onEnter() with real activity/book state -- one of the
most-executed and, prior to this scenario, completely untested paths in the
firmware.

A successful run ends with the device OFF: enterDeepSleep() cuts power via
powerManager.startDeepSleep(), and the USB port disappears along with it.
That is the expected, PASSING outcome -- this scenario must not (and does
not) treat "the port went away" as a failure. The reverse is the failure: if
the port is still enumerated well after the ack, sleep did not actually
happen.

Bringing the device back requires a HUMAN to physically press the power
button; this scenario does not attempt that and does not block waiting for
it. It only gives the port a short grace window to reappear on its own
(which would be unexpected -- worth a note, not an assertion) before
finishing cleanly.

Run via:  python3 scripts/device_test.py [--port ...] scripts/device_tests/test_sleep.py
This is deliberately NOT meant to run as part of the no-args scenario
discovery in scripts/device_test.py: it leaves the device powered off, and
"test_sleep" < "test_smoke" alphabetically means a bare discovery run would
hit this one first and strand every scenario after it. Run it standalone,
and last.
"""

from __future__ import annotations

import time

from device_test import DeviceTestConsole, DeviceTestError, get_auto_detected_ports

# How long to wait for the port to actually vanish after CMD:SLEEP's ack.
# The ack is sent before enterDeepSleep() cuts power, so the port is expected
# to still be enumerated for a brief moment; this just bounds that wait.
PORT_GONE_TIMEOUT_S = 15.0

# After confirming the device is off, how long to watch for the port to
# reappear on its own before giving up and printing the human-intervention
# notice. Deep sleep here only ends via the power button, so this is not
# expected to fire -- it exists to skip cleanly instead of hanging if it
# doesn't, not to wait out a human response.
RECONNECT_GRACE_S = 5.0


def _port_present(port: str) -> bool:
    return port in get_auto_detected_ports()


def run(device: DeviceTestConsole) -> None:
    port = device.port

    activity_before = device.activity()
    fbhash_before = device.fbhash()
    print(f"[test_sleep] before CMD:SLEEP: activity={activity_before!r} fbhash={fbhash_before!r}")

    result = device.send_command("SLEEP")
    if not result.ok:
        raise DeviceTestError(f"CMD:SLEEP was rejected: {result.error}")

    marker = result.first_event()
    print(f"[test_sleep] last known-good marker before sleep: {marker}")
    print(f"[test_sleep] last lines from the device around the ack: {result.raw_lines}")

    # The real assertion: confirm the port actually goes away. That is what a
    # successful power-cut looks like, not a harness failure.
    deadline = time.monotonic() + PORT_GONE_TIMEOUT_S
    while time.monotonic() < deadline and _port_present(port):
        time.sleep(0.25)

    device.close()  # our handle is stale either way once the port is gone

    if _port_present(port):
        raise DeviceTestError(
            f"{port} is still enumerated {PORT_GONE_TIMEOUT_S:.0f}s after CMD:SLEEP's ack; "
            "the device does not appear to have cut power"
        )

    print(f"[test_sleep] {port} disappeared -- device powered off as expected (PASS)")

    # Give it a short window in case it reappears on its own (unexpected, but
    # worth a note); otherwise skip cleanly and tell the human what to do.
    grace_deadline = time.monotonic() + RECONNECT_GRACE_S
    while time.monotonic() < grace_deadline:
        if _port_present(port):
            print(f"[test_sleep] note: {port} re-enumerated during the grace window; "
                  "unexpected for a deep sleep that only wakes on the power button, "
                  "but not asserted on by this scenario")
            return
        time.sleep(0.5)

    print(f"[test_sleep] device is OFF and did not reappear on its own (expected). "
          f"A HUMAN must press the POWER button on the device to bring it back "
          f"before running any further scenarios against {port}.")
