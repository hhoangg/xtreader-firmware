"""
HTTPS probe scenario for the CP_TEST_CONSOLE serial harness.

Before any sync client gets written, one question has to be answered: can
this device complete an HTTPS request to a Cloudflare-hosted origin? Two
reasons to doubt it:

  1. A TLS handshake costs tens of KB of heap, and platformio.ini documents
     that this project has already hit MP_MEM OOM in exactly this path
     (hence WOLFSSL_HAVE_SP_ECC / WOLFSSL_SP_SMALL).
  2. The device has no real-time clock. If wolfSSL validates certificate
     notBefore/notAfter, it compares against a garbage clock, and Cloudflare
     rotates certificates automatically.

This scenario drives CMD:HTTPGET <url> (src/main.cpp, CP_TEST_CONSOLE build)
and reports its single [TEST] JSON line, which carries three heap samples --
free heap plus largest allocatable block, taken before the request,
immediately after the TLS handshake, and after completion -- so a human can
see exactly how much a handshake costs on this hardware, alongside whether
WiFi came up, the HTTP status, body byte count, and any error.

Run via:  python3 scripts/device_test.py [--port ...] scripts/device_tests/test_https.py
Or standalone, to override the target URL:
  python3 scripts/device_tests/test_https.py [--port ...] [url]
  CROSSPOINT_TEST_HTTPS_URL=http://192.168.1.50:8080/health python3 scripts/device_tests/test_https.py

URL resolution order: a positional argument, then $CROSSPOINT_TEST_HTTPS_URL,
then DEFAULT_URL below (the deployed Cloudflare Worker healthcheck). Passing
a plain http:// URL to a local server lets a failure against the Cloudflare
origin be isolated to TLS rather than WiFi/network plumbing in general.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

# Run standalone (python3 scripts/device_tests/test_https.py), sys.path[0] is
# this file's own directory, not scripts/ where device_test.py lives. Add it
# so `from device_test import ...` resolves the same way it does when this
# scenario is instead loaded by scripts/device_test.py's discovery (there,
# sys.path[0] is already scripts/, from the top-level script that was run).
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from device_test import DeviceTestConsole, DeviceTestError  # noqa: E402

DEFAULT_URL = "https://crosspoint-sync.hoangxuan2402.workers.dev/healthcheck"

# Generous wall-clock budget for the whole probe: WiFi bring-up alone can take
# up to TEST_WIFI_PER_NETWORK_TIMEOUT_MS (7s in src/main.cpp) per saved
# network before the HTTP request (bounded at 20s body-read plus whatever the
# connect/TLS/header phase takes) even starts.
COMMAND_TIMEOUT_S = 90.0


def resolve_url(explicit: str | None = None) -> str:
    if explicit:
        return explicit
    return os.environ.get("CROSSPOINT_TEST_HTTPS_URL", DEFAULT_URL)


def run(device: DeviceTestConsole, url: str | None = None) -> None:
    target = resolve_url(url)
    print(f"[test_https] probing {target!r}")

    result = device.send_command(f"HTTPGET {target}", timeout=COMMAND_TIMEOUT_S)
    if not result.ok:
        raise DeviceTestError(f"CMD:HTTPGET was rejected: {result.error} (raw: {result.raw_lines})")

    event = result.first_event()
    print(f"[test_https] result: {json.dumps(event, indent=2)}")

    if not event.get("wifiConnected"):
        raise DeviceTestError(f"WiFi did not come up: {event.get('wifiError')!r}")
    print(f"[test_https] joined SSID {event.get('ssid')!r}")

    heap_before = event.get("heapBeforeFree")
    heap_tls = event.get("heapTlsFree")
    heap_after = event.get("heapAfterFree")
    print(
        f"[test_https] free heap: before={heap_before} "
        f"after-handshake={heap_tls} (sampled={event.get('midHandshakeSampled')}) "
        f"after={heap_after}"
    )
    print(
        f"[test_https] largest allocatable block: before={event.get('heapBeforeMaxAlloc')} "
        f"after-handshake={event.get('heapTlsMaxAlloc')} after={event.get('heapAfterMaxAlloc')}"
    )
    if isinstance(heap_before, int) and isinstance(heap_tls, int):
        print(f"[test_https] handshake cost ~{heap_before - heap_tls} bytes of free heap")

    if not event.get("ok"):
        raise DeviceTestError(
            f"CMD:HTTPGET failed against {target!r}: error={event.get('error')!r} "
            f"status={event.get('status')} (full event: {event})"
        )

    print(
        f"[test_https] HTTP {event.get('status')}, {event.get('bytesRead')} body bytes, "
        f"preview={event.get('bodyPreview')!r}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", default=None, help="Serial port (default: auto-detect)")
    parser.add_argument(
        "url",
        nargs="?",
        default=None,
        help="URL to probe (default: $CROSSPOINT_TEST_HTTPS_URL or the deployed healthcheck)",
    )
    args = parser.parse_args()

    try:
        device = DeviceTestConsole(port=args.port)
    except (DeviceTestError, OSError) as e:
        print(f"Failed to open device: {e}", file=sys.stderr)
        return 1

    try:
        run(device, args.url)
    except DeviceTestError as e:
        print(f"FAIL test_https: {e}", file=sys.stderr)
        return 1
    finally:
        device.close()

    print("PASS test_https")
    return 0


if __name__ == "__main__":
    sys.exit(main())
