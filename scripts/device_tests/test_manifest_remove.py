"""
Local index removal scenario for the CP_TEST_CONSOLE serial harness.

Drives CMD:MANIFESTFIRST, CMD:MANIFESTHASID <id>, and CMD:MANIFESTREMOVE <id> (src/main.cpp,
CP_TEST_CONSOLE build) against the device's already-synced local manifest index
(/.crosspoint/remote.idx). CMD:MANIFESTREMOVE drives sync_manifest::removeFromIndex() -- the same
primitive (ManifestIndexMerge, see lib/SyncManifest/ManifestIndexMerge.h) FileBrowserActivity's
"Delete everywhere" flow calls once a server-side delete has already succeeded, so the row does not
keep showing as "On server" for a book that no longer exists anywhere. This scenario exercises just
that local bookkeeping directly, with no network call and nothing touched on the paired account's
server -- unlike test_server_delete.py, no --confirm is required.

Mutates the local index (the removed book's row is gone until a later full/delta CMD:MANIFESTSYNC
naturally restores it, since the server was never told about this): safe to run against a real synced
library, and safe to re-run.

Requires the device to already have a synced manifest with at least one book (run test_manifest.py
first) -- this scenario does not sync on its own.

Run via:  python3 scripts/device_test.py [--port ...] scripts/device_tests/test_manifest_remove.py
Or standalone:
  python3 scripts/device_tests/test_manifest_remove.py [--port ...]
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from device_test import DeviceTestConsole, DeviceTestError  # noqa: E402

COMMAND_TIMEOUT_S = 60.0


def run(device: DeviceTestConsole) -> None:
    print("[test_manifest_remove] issuing CMD:MANIFESTFIRST")
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
    book_path = first.get("path")
    print(f"[test_manifest_remove] target: id={book_id!r} path={book_path!r}")

    has_before = device.send_command(f"MANIFESTHASID {book_id}", timeout=COMMAND_TIMEOUT_S)
    if not has_before.ok or not has_before.first_event().get("found"):
        raise DeviceTestError(f"CMD:MANIFESTHASID reported the target id as absent before removal: {has_before}")

    print(f"[test_manifest_remove] issuing CMD:MANIFESTREMOVE {book_id}")
    removed = device.send_command(f"MANIFESTREMOVE {book_id}", timeout=COMMAND_TIMEOUT_S)
    if not removed.ok:
        raise DeviceTestError(f"CMD:MANIFESTREMOVE was rejected: {removed.error} (raw: {removed.raw_lines})")
    if not removed.first_event().get("ok"):
        raise DeviceTestError(f"CMD:MANIFESTREMOVE reported failure: {removed.first_event()}")

    has_after = device.send_command(f"MANIFESTHASID {book_id}", timeout=COMMAND_TIMEOUT_S)
    if not has_after.ok:
        raise DeviceTestError(f"CMD:MANIFESTHASID was rejected after removal: {has_after.error}")
    if has_after.first_event().get("found"):
        raise DeviceTestError(f"id={book_id} is still present in the index after CMD:MANIFESTREMOVE")
    print(f"[test_manifest_remove] confirmed id={book_id} is gone from the index")

    # Sanity check the rest of the index survived intact and in order: the new first entry (if any)
    # must sort at or after the removed entry's old path -- never before it, which would mean the
    # removal corrupted ordering rather than just dropping one row.
    new_first = device.send_command("MANIFESTFIRST", timeout=COMMAND_TIMEOUT_S).first_event()
    if new_first.get("found"):
        new_path = new_first.get("path")
        if book_path is not None and new_path is not None and new_path < book_path:
            raise DeviceTestError(
                f"Index ordering broke after removal: new first path {new_path!r} sorts before the "
                f"removed entry's path {book_path!r}"
            )
        print(f"[test_manifest_remove] index still sorted; new first entry: {new_first}")
    else:
        print("[test_manifest_remove] index is now empty (the removed book was the only entry)")


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
        print(f"FAIL test_manifest_remove: {e}", file=sys.stderr)
        return 1
    finally:
        device.close()

    print("PASS test_manifest_remove")
    return 0


if __name__ == "__main__":
    sys.exit(main())
