"""
Force-delete (server side) scenario for the CP_TEST_CONSOLE serial harness.

Drives, in order: CMD:MANIFESTFIRST (picks a real book id off the device's
already-synced local manifest index) and CMD:SERVERDELETE <id>
(src/main.cpp, CP_TEST_CONSOLE build), which runs the exact code path
FileBrowserActivity's "Delete everywhere" option uses: DELETE /library/:id
via src/sync/BookServerDelete.h, Bearer auth from the paired
SyncCredentialStore token.

*** DESTRUCTIVE AND IRREVERSIBLE ***
This soft-deletes the book on the server for the whole account -- every
device paired to it loses the book, and a later CMD:MANIFESTSYNC will stop
listing it (or report it as a tombstone via `since`). It does NOT touch
anything on this device's SD card (CMD:SERVERDELETE only calls the server
half; FileBrowserActivity's own local-delete-then-server-delete ordering is
exercised on-device through the UI, not by this script).

Point this at a disposable test book uploaded specifically for this
purpose, never at a real book in the account's library. Requires --confirm
to actually run, precisely so this can't be fired by accident as part of a
larger test sweep.

Requires the device to already be paired (see test_pairing.py) and to have
a synced manifest with at least one book (run test_manifest.py first) --
this scenario does not pair or sync on its own, and always operates on
whatever CMD:MANIFESTFIRST reports as the first (sorted-by-path) book, so
make sure that's the disposable test book before running this.

Run via:
  python3 scripts/device_tests/test_server_delete.py --confirm [--port ...]
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from device_test import DeviceTestConsole, DeviceTestError  # noqa: E402

COMMAND_TIMEOUT_S = 60.0


def run(device: DeviceTestConsole) -> None:
    print("[test_server_delete] issuing CMD:MANIFESTFIRST")
    lookup = device.send_command("MANIFESTFIRST", timeout=COMMAND_TIMEOUT_S)
    if not lookup.ok:
        raise DeviceTestError(f"CMD:MANIFESTFIRST was rejected: {lookup.error} (raw: {lookup.raw_lines})")

    first = lookup.first_event()
    if not first.get("found"):
        raise DeviceTestError(
            "Local manifest index is empty -- run test_manifest.py (or CMD:MANIFESTSYNC) against a "
            "library with at least one (disposable, test-only) book first"
        )
    book_id = first["id"]
    book_path = first.get("path")
    print(f"[test_server_delete] about to permanently delete id={book_id!r} path={book_path!r} from the server")

    result = device.send_command(f"SERVERDELETE {book_id}", timeout=COMMAND_TIMEOUT_S)
    if not result.ok:
        raise DeviceTestError(f"CMD:SERVERDELETE was rejected: {result.error} (raw: {result.raw_lines})")

    event = result.first_event()
    print(f"[test_server_delete] result: {event}")

    if not event.get("wifiConnected"):
        raise DeviceTestError(f"WiFi did not come up: {event.get('wifiError')!r}")
    if event.get("error") == "not_paired":
        raise DeviceTestError(
            "Device is not paired to a sync account -- run test_pairing.py (or pair manually) first"
        )
    if not event.get("ok"):
        raise DeviceTestError(
            f"CMD:SERVERDELETE failed: error={event.get('error')!r} httpStatus={event.get('httpStatus')!r}"
        )
    print(f"[test_server_delete] deleted {book_path!r} (id={book_id}) from the server")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", default=None, help="Serial port (default: auto-detect)")
    parser.add_argument(
        "--confirm",
        action="store_true",
        help="Required. Acknowledges this permanently deletes a book from the paired account's server library.",
    )
    args = parser.parse_args()

    if not args.confirm:
        print(
            "Refusing to run without --confirm: this permanently deletes the first book in the device's "
            "synced manifest from the server, for the whole account. Point it at a disposable test book "
            "and pass --confirm to proceed.",
            file=sys.stderr,
        )
        return 1

    try:
        device = DeviceTestConsole(port=args.port)
    except (DeviceTestError, OSError) as e:
        print(f"Failed to open device: {e}", file=sys.stderr)
        return 1

    try:
        run(device)
    except DeviceTestError as e:
        print(f"FAIL test_server_delete: {e}", file=sys.stderr)
        return 1
    finally:
        device.close()

    print("PASS test_server_delete")
    return 0


if __name__ == "__main__":
    sys.exit(main())
