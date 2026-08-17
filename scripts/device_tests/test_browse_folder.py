"""
Remote-only-folder browsing scenario for the CP_TEST_CONSOLE serial harness.

Regression test for the bug where a folder that exists only in the synced
remote index (never downloaded, so it has no directory on the SD card --
e.g. a folder created on the web and synced, holding a book that was never
pulled down to this device) rendered as empty when opened, instead of
listing its remote children as placeholders. See
src/activities/home/FileBrowserActivity.cpp's loadFiles(): the local
directory scan is now optional (skipped when Storage.open(basepath) fails)
rather than a precondition for the remote-index merge that follows it.

Drives CMD:BROWSEFOLDER <path> (src/main.cpp, CP_TEST_CONSOLE build), which
runs the same local-scan + file_browser_merge::FolderMerge logic loadFiles()
uses, directly against the real SD card and the real
/.crosspoint/remote.idx -- no UI navigation, no network. This is the one
part of the fix host-side tests (test/file_browser_merge) cannot cover:
real SdFat directory I/O (USE_UTF8_LONG_NAMES=1) against a folder name with
diacritics.

Requires the device to already be paired and synced with a library that has
at least one folder holding an undownloaded book -- run test_pairing.py and
test_manifest.py (or a real Sync Now) first. Does not bring WiFi up itself
(CMD:BROWSEFOLDER never touches the network); it can be run from whatever
screen the device is on.

Run via:  python3 scripts/device_test.py [--port ...] scripts/device_tests/test_browse_folder.py
Or standalone:
  python3 scripts/device_tests/test_browse_folder.py [--port ...] [--folder "/Văn học"]
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# Run standalone (python3 scripts/device_tests/test_browse_folder.py), sys.path[0] is
# this file's own directory, not scripts/ where device_test.py lives -- add it,
# matching test_manifest.py's own standalone-run handling.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from device_test import DeviceTestConsole, DeviceTestError  # noqa: E402

# Pure SD + local index read, no network -- the default ack timeout is plenty.
COMMAND_TIMEOUT_S = 15.0

# Matches the reported bug's actual folder: created on the web, synced to the
# device, never downloaded. Override with --folder for a different library.
DEFAULT_FOLDER = "/Văn học"


def run(device: DeviceTestConsole, folder: str = DEFAULT_FOLDER) -> None:
    print(f"[test_browse_folder] issuing CMD:BROWSEFOLDER {folder!r}")
    result = device.send_command(f"BROWSEFOLDER {folder}", timeout=COMMAND_TIMEOUT_S)
    if not result.ok:
        raise DeviceTestError(f"CMD:BROWSEFOLDER was rejected: {result.error} (raw: {result.raw_lines})")

    event = result.first_event()
    print(f"[test_browse_folder] result: {json.dumps(event, ensure_ascii=False, indent=2)}")

    if not event.get("scanOk"):
        raise DeviceTestError(f"Remote index scan failed for {folder!r} (full event: {event})")

    # The whole point of this scenario: a folder with no local directory
    # (hasLocalDir=false, the exact state Storage.open(basepath) leaves a
    # server-only folder in) must still report its remote children instead
    # of an empty list.
    if not event.get("hasLocalDir"):
        print(f"[test_browse_folder] {folder!r} has no local directory on SD (server-only, as expected)")

    entries = event.get("entries", [])
    if not entries:
        raise DeviceTestError(
            f"{folder!r} listed zero entries -- either the bug regressed, or this account's library "
            f"doesn't have a book under that folder (run test_manifest.py first, or pass --folder)"
        )

    placeholders = [e for e in entries if e.get("remoteId")]
    print(f"[test_browse_folder] {folder!r} lists {len(entries)} entr{'y' if len(entries) == 1 else 'ies'}: "
          f"{[e.get('name') for e in entries]}")
    print(f"[test_browse_folder] {len(placeholders)} of those are remote-only placeholders (not yet downloaded)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", default=None, help="Serial port (default: auto-detect)")
    parser.add_argument("--folder", default=DEFAULT_FOLDER, help=f"Folder path to browse (default: {DEFAULT_FOLDER!r})")
    args = parser.parse_args()

    try:
        device = DeviceTestConsole(port=args.port)
    except (DeviceTestError, OSError) as e:
        print(f"Failed to open device: {e}", file=sys.stderr)
        return 1

    try:
        run(device, args.folder)
    except DeviceTestError as e:
        print(f"FAIL test_browse_folder: {e}", file=sys.stderr)
        return 1
    finally:
        device.close()

    print("PASS test_browse_folder")
    return 0


if __name__ == "__main__":
    sys.exit(main())
