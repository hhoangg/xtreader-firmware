"""
Delta-sync scenario for the CP_TEST_CONSOLE serial harness.

Drives CMD:MANIFESTSYNC (src/main.cpp, CP_TEST_CONSOLE build) twice in a row against the paired sync
account, through the exact code path a real periodic sync uses: sync_manifest::sync() -- see
src/sync/SyncManifest.h/.cpp. The first call establishes (or reuses) a locally-synced index with a
valid header/watermark; the second call must then read that header back and switch to a `since` delta
fetch instead of re-listing the whole library -- see sync()'s own doc comment for exactly when each
mode is chosen. This is the one behaviour host tests cannot cover (they exercise the merge/removal
logic directly, never the on-SD header round trip through real HalStorage I/O), so it is worth driving
for real here.

Requires the device to already be paired (see test_pairing.py) -- this scenario does not pair it. It
brings WiFi up itself, so no navigation is needed beforehand; it can be run from whatever screen the
device is on.

Run via:  python3 scripts/device_test.py [--port ...] scripts/device_tests/test_manifest_delta.py
Or standalone:
  python3 scripts/device_tests/test_manifest_delta.py [--port ...]
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from device_test import DeviceTestConsole, DeviceTestError  # noqa: E402

COMMAND_TIMEOUT_S = 180.0


def _sync_once(device: DeviceTestConsole, label: str) -> dict:
    print(f"[test_manifest_delta] issuing CMD:MANIFESTSYNC ({label})")
    result = device.send_command("MANIFESTSYNC", timeout=COMMAND_TIMEOUT_S)
    if not result.ok:
        raise DeviceTestError(f"CMD:MANIFESTSYNC ({label}) was rejected: {result.error} (raw: {result.raw_lines})")

    event = result.first_event()
    if not event.get("wifiConnected"):
        raise DeviceTestError(f"WiFi did not come up ({label}): {event.get('wifiError')!r}")
    if event.get("error") == "not_paired":
        raise DeviceTestError(
            "Device is not paired to a sync account -- run test_pairing.py (or pair manually) first"
        )
    if not event.get("ok"):
        raise DeviceTestError(f"CMD:MANIFESTSYNC ({label}) failed: error={event.get('error')!r} (full event: {event})")

    print(
        f"[test_manifest_delta] {label}: deltaSync={event.get('deltaSync')} "
        f"entries={event.get('entriesWritten')} pages={event.get('pagesFetched')} "
        f"totalCount={event.get('totalCount')}"
    )
    return event


def run(device: DeviceTestConsole) -> None:
    first = _sync_once(device, "first")
    second = _sync_once(device, "second")

    # The first sync may itself have been a delta (a prior run/session already left a valid, current-
    # format index on SD) -- only the *second* call is guaranteed to see one, since the first call's
    # own success is exactly what creates/refreshes the header the second call reads back.
    if not second.get("deltaSync"):
        raise DeviceTestError(
            "Second CMD:MANIFESTSYNC did not use a delta fetch (deltaSync=false) -- the on-SD index "
            "header from the first sync was not read back correctly. See src/sync/SyncManifest.cpp's "
            "sync()/openIndexForScan()."
        )
    print("[test_manifest_delta] second sync correctly used a `since` delta fetch")


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
        print(f"FAIL test_manifest_delta: {e}", file=sys.stderr)
        return 1
    finally:
        device.close()

    print("PASS test_manifest_delta")
    return 0


if __name__ == "__main__":
    sys.exit(main())
