"""
Book-finished telemetry scenario for the CP_TEST_CONSOLE serial harness.

Drives, in order: CMD:MANIFESTFIRST (picks a real book id off the device's
already-synced local manifest index) and CMD:BOOKFINISHED <id>
(src/main.cpp, CP_TEST_CONSOLE build), which POSTs /events/book-finished
with {"bookId": id} -- the same telemetry::bookFinished() call
ReaderActivity's real end-of-book trigger makes (see
src/sync/BookFinishedNotifier.cpp), just synchronous and with an explicit
id instead of waiting for a real book to be read to the end.

Requires the device to already be paired (see test_pairing.py) and to have
a synced manifest with at least one book (run test_manifest.py, or plain
CMD:MANIFESTSYNC, first) -- this scenario does not pair or sync on its own.
It brings WiFi up itself (same saved-network auto-connect every other
scenario in this directory uses), so no navigation is needed beforehand.

This is a telemetry event, not a destructive one -- unlike test_server_delete.py,
it is safe to run repeatedly against a real account.

Run via:  python3 scripts/device_test.py [--port ...] scripts/device_tests/test_book_finished.py
Or standalone:
  python3 scripts/device_tests/test_book_finished.py [--port ...]
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

# Run standalone (python3 scripts/device_tests/test_book_finished.py), sys.path[0] is
# this file's own directory, not scripts/ where device_test.py lives -- add it,
# matching test_manifest.py's own standalone-run handling.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from device_test import DeviceTestConsole, DeviceTestError  # noqa: E402

COMMAND_TIMEOUT_S = 60.0


def run(device: DeviceTestConsole) -> None:
    print("[test_book_finished] issuing CMD:MANIFESTFIRST")
    lookup = device.send_command("MANIFESTFIRST", timeout=COMMAND_TIMEOUT_S)
    if not lookup.ok:
        raise DeviceTestError(f"CMD:MANIFESTFIRST was rejected: {lookup.error} (raw: {lookup.raw_lines})")

    first = lookup.first_event()
    if not first.get("found"):
        raise DeviceTestError(
            "Local manifest index is empty -- run test_manifest.py (or CMD:MANIFESTSYNC) against a "
            "library with at least one book first"
        )
    book_id = first["id"]
    print(f"[test_book_finished] sending book-finished event for id={book_id!r} path={first.get('path')!r}")

    result = device.send_command(f"BOOKFINISHED {book_id}", timeout=COMMAND_TIMEOUT_S)
    if not result.ok:
        raise DeviceTestError(f"CMD:BOOKFINISHED was rejected: {result.error} (raw: {result.raw_lines})")

    event = result.first_event()
    print(f"[test_book_finished] result: {event}")

    if not event.get("wifiConnected"):
        raise DeviceTestError(f"WiFi did not come up: {event.get('wifiError')!r}")
    if event.get("error") == "not_paired":
        raise DeviceTestError(
            "Device is not paired to a sync account -- run test_pairing.py (or pair manually) first"
        )
    if not event.get("ok"):
        raise DeviceTestError(
            f"CMD:BOOKFINISHED failed: error={event.get('error')!r} httpStatus={event.get('httpStatus')!r}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", default=None, help="Serial port (default: auto-detect)")
    args = parser.parse_args()

    try:
        device = DeviceTestConsole(port=args.port)
    except (DeviceTestError, OSError) as e:
        print(f"Failed to open device: {e}", file=sys.stderr)
        return 1

    try:
        run(device)
    except DeviceTestError as e:
        print(f"FAIL test_book_finished: {e}", file=sys.stderr)
        return 1
    finally:
        device.close()

    print("PASS test_book_finished")
    return 0


if __name__ == "__main__":
    sys.exit(main())
