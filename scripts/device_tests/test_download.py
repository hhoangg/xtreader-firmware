"""
Single-book-download scenario for the CP_TEST_CONSOLE serial harness.

Drives, in order: CMD:MANIFESTFIRST (picks a real book id off the device's
already-synced local manifest index) and CMD:BOOKDOWNLOAD <id> (src/main.cpp,
CP_TEST_CONSOLE build), which runs the exact code path a real download will
use: book_downloader::download() brings the book down over
GET /library/:id/file (Bearer auth from the paired SyncCredentialStore
token) through HttpDownloader, streams it to a temp file on SD, renames it
into place, and flips the manifest index's `downloaded` flag -- see
src/sync/BookDownloader.h/.cpp for the implementation this exercises and
docs/API.md (in the crosspoint-sync repo) for the wire format.

Requires the device to already be paired (see test_pairing.py) and to have a
synced manifest with at least one book (run test_manifest.py, or plain
CMD:MANIFESTSYNC, first) -- this scenario does not pair or sync on its own.
It brings WiFi up itself (same saved-network auto-connect every other
scenario in this directory uses), so no navigation is needed beforehand.

Run via:  python3 scripts/device_test.py [--port ...] scripts/device_tests/test_download.py
Or standalone:
  python3 scripts/device_tests/test_download.py [--port ...]
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# Run standalone (python3 scripts/device_tests/test_download.py), sys.path[0] is
# this file's own directory, not scripts/ where device_test.py lives -- add it,
# matching test_manifest.py's own standalone-run handling.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from device_test import DeviceTestConsole, DeviceTestError  # noqa: E402

# A real book download over Wi-Fi is the slow part here; WiFi bring-up (up to
# ~7s per saved network) plus however long a multi-MB book takes indoors.
DOWNLOAD_TIMEOUT_S = 300.0
LOOKUP_TIMEOUT_S = 30.0


def run(device: DeviceTestConsole) -> None:
    print("[test_download] issuing CMD:MANIFESTFIRST")
    lookup = device.send_command("MANIFESTFIRST", timeout=LOOKUP_TIMEOUT_S)
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
    print(f"[test_download] downloading id={book_id!r} path={book_path!r}")

    result = device.send_command(f"BOOKDOWNLOAD {book_id}", timeout=DOWNLOAD_TIMEOUT_S)
    if not result.ok:
        raise DeviceTestError(f"CMD:BOOKDOWNLOAD was rejected: {result.error} (raw: {result.raw_lines})")

    event = result.first_event()
    print(f"[test_download] result: {json.dumps(event, indent=2)}")

    if not event.get("wifiConnected"):
        raise DeviceTestError(f"WiFi did not come up: {event.get('wifiError')!r}")

    heap_before = event.get("heapBeforeFree")
    heap_tls = event.get("heapTlsFree")
    heap_after = event.get("heapAfterFree")
    print(
        f"[test_download] free heap: before={heap_before} "
        f"after-handshake={heap_tls} after-download={heap_after}"
    )
    max_before = event.get("heapBeforeMaxAlloc")
    max_after = event.get("heapAfterMaxAlloc")
    if isinstance(max_before, int) and isinstance(max_after, int):
        moved = max_after - max_before
        print(
            f"[test_download] largest-block delta over the whole download: {moved:+d} bytes "
            f"({'looks fragmented -- worth a closer look' if moved < 0 else 'did not shrink'})"
        )

    if not event.get("ok"):
        raise DeviceTestError(
            f"CMD:BOOKDOWNLOAD failed: error={event.get('error')!r} httpStatus={event.get('httpStatus')!r} "
            f"(full event: {event})"
        )

    print(f"[test_download] downloaded {event.get('bytesDownloaded')} bytes to {event.get('destPath')!r}")


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
        print(f"FAIL test_download: {e}", file=sys.stderr)
        return 1
    finally:
        device.close()

    print("PASS test_download")
    return 0


if __name__ == "__main__":
    sys.exit(main())
