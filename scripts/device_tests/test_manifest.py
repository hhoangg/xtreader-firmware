"""
Library-manifest sync scenario for the CP_TEST_CONSOLE serial harness.

Drives CMD:MANIFESTSYNC (src/main.cpp, CP_TEST_CONSOLE build), which runs
the exact code path a real sync will use: sync_manifest::sync() brings
GET /library/manifest down page by page through HttpDownloader (Bearer
auth from the paired SyncCredentialStore token), streams each page through
ManifestStreamParser, and writes /.crosspoint/remote.idx on the SD card --
see src/sync/SyncManifest.h/.cpp for the implementation this exercises and
docs/API.md (in the crosspoint-sync repo) for the wire format.

Requires the device to already be paired (see test_pairing.py) -- this
scenario does not pair it. It brings WiFi up itself (same saved-network
auto-connect CMD:HTTPGET/CMD:MANIFESTSYNC both use), so no navigation is
needed beforehand; it can be run from whatever screen the device is on.

Reports the single [TEST] JSON line CMD:MANIFESTSYNC prints: whether WiFi
came up, whether the sync succeeded, how many pages/entries were fetched,
the server's totalCount, and three heap samples (before the request, after
the first page's TLS handshake, after the last page) -- the numbers the
task brief asks a human to look at to decide whether a sync session needs a
reboot afterwards (does the largest allocatable block move?).

Run via:  python3 scripts/device_test.py [--port ...] scripts/device_tests/test_manifest.py
Or standalone:
  python3 scripts/device_tests/test_manifest.py [--port ...]
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# Run standalone (python3 scripts/device_tests/test_manifest.py), sys.path[0] is
# this file's own directory, not scripts/ where device_test.py lives -- add it,
# matching test_https.py's own standalone-run handling.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from device_test import DeviceTestConsole, DeviceTestError  # noqa: E402

# Generous wall-clock budget: WiFi bring-up (up to ~7s per saved network,
# see src/main.cpp's TEST_WIFI_PER_NETWORK_TIMEOUT_MS) plus however many
# manifest pages a real library needs, each its own TLS handshake.
COMMAND_TIMEOUT_S = 180.0


def run(device: DeviceTestConsole) -> None:
    print("[test_manifest] issuing CMD:MANIFESTSYNC")
    result = device.send_command("MANIFESTSYNC", timeout=COMMAND_TIMEOUT_S)
    if not result.ok:
        raise DeviceTestError(f"CMD:MANIFESTSYNC was rejected: {result.error} (raw: {result.raw_lines})")

    event = result.first_event()
    print(f"[test_manifest] result: {json.dumps(event, indent=2)}")

    if not event.get("wifiConnected"):
        raise DeviceTestError(f"WiFi did not come up: {event.get('wifiError')!r}")
    print(f"[test_manifest] joined SSID {event.get('ssid')!r}")

    if event.get("error") == "not_paired":
        raise DeviceTestError(
            "Device is not paired to a sync account -- run test_pairing.py (or pair manually) first"
        )

    heap_before = event.get("heapBeforeFree")
    heap_tls = event.get("heapTlsFree")
    heap_after = event.get("heapAfterFree")
    print(
        f"[test_manifest] free heap: before={heap_before} "
        f"after-first-handshake={heap_tls} after-last-page={heap_after}"
    )
    max_before = event.get("heapBeforeMaxAlloc")
    max_tls = event.get("heapTlsMaxAlloc")
    max_after = event.get("heapAfterMaxAlloc")
    print(f"[test_manifest] largest allocatable block: before={max_before} after-handshake={max_tls} after={max_after}")
    if isinstance(max_before, int) and isinstance(max_after, int):
        moved = max_after - max_before
        print(
            f"[test_manifest] largest-block delta over the whole sync: {moved:+d} bytes "
            f"({'looks fragmented -- worth a closer look' if moved < 0 else 'did not shrink'})"
        )

    if not event.get("ok"):
        raise DeviceTestError(f"CMD:MANIFESTSYNC failed: error={event.get('error')!r} (full event: {event})")

    print(
        f"[test_manifest] synced {event.get('entriesWritten')} entries across {event.get('pagesFetched')} page(s) "
        f"(server totalCount={event.get('totalCount')}, deltaSync={event.get('deltaSync')})"
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
        print(f"FAIL test_manifest: {e}", file=sys.stderr)
        return 1
    finally:
        device.close()

    print("PASS test_manifest")
    return 0


if __name__ == "__main__":
    sys.exit(main())
