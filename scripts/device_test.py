#!/usr/bin/env python3
"""
Non-interactive host harness for the CrossPoint firmware's serial test
console (src/main.cpp, CP_TEST_CONSOLE build). Drives the device over USB
serial -- press/hold buttons, read machine-readable [TEST] lines, capture
screenshots -- and asserts on the results.

This is deliberately a *library* (DeviceTestConsole + a couple of assert
helpers) plus a thin CLI runner, so scenarios under scripts/device_tests/
can `from device_test import DeviceTestConsole, DeviceTestError` and drive
the device directly. Run standalone:

    python3 scripts/device_test.py [--port /dev/cu.usbmodem1101] [scenario.py ...]

With no scenario arguments, every scripts/device_tests/test_*.py is run.
Exits 0 only if every scenario's run(device) completes without raising.

Protocol reused from scripts/debugging_monitor.py and src/main.cpp:
  - A command is sent as "CMD:<name> <args>\n".
  - The firmware answers with "CMDACK:<cmd>" (handled) or
    "CMDERR:unknown:<cmd>" (unhandled / compiled out).
  - CMD:HEAP / CMD:FBHASH / CMD:ACTIVITY each print one
    "[TEST] {...json...}\n" line before their ack.
  - CMD:SCREENSHOT frames the raw framebuffer between
    "SCREENSHOT_START:<size>" and "SCREENSHOT_END", ack follows after.
    The framebuffer is 800x480 1-bit, landscape; debugging_monitor.py
    decodes it with PIL and rotates 270 degrees to portrait -- reused here.
"""

from __future__ import annotations

import argparse
import glob
import importlib.util
import json
import platform
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("Missing dependency: pip3 install pyserial", file=sys.stderr)
    sys.exit(1)

try:
    from PIL import Image
except ImportError:
    Image = None  # Screenshot capture falls back to raw bytes.

DEFAULT_BAUDRATE = 115200
DEFAULT_ACK_TIMEOUT_S = 5.0
FRAMEBUFFER_WIDTH = 800
FRAMEBUFFER_HEIGHT = 480

# Must match MappedInputManager::Button as exposed by CMD:PRESS / CMD:HOLD
# in src/main.cpp's parseTestButtonName().
#
# NAVNEXT/NAVPREV move a list selection (Settings, its submenus, file
# browsers, ...); PAGEBACK/PAGEFORWARD turn reader pages. UP/DOWN/LEFT/RIGHT
# are the raw physical buttons -- on real hardware those *resolve into*
# NavNext/NavPrevious/PageBack/PageForward through the board's own input
# mapping, but injecting them here bypasses that resolution entirely, so
# CMD:PRESS DOWN (etc.) does not move a list selection. Use NAVNEXT/NAVPREV
# for that.
VALID_BUTTONS = ("BACK", "CONFIRM", "LEFT", "RIGHT", "UP", "DOWN", "POWER", "NAVNEXT", "NAVPREV", "PAGEBACK",
                 "PAGEFORWARD")


class DeviceTestError(Exception):
    """Raised when the device console fails to respond, rejects a command,
    or an assertion helper's expectation isn't met."""


@dataclass
class CommandResult:
    cmd: str
    ok: bool
    error: str | None
    test_events: list[dict] = field(default_factory=list)
    raw_lines: list[str] = field(default_factory=list)

    def first_event(self) -> dict:
        if not self.test_events:
            raise DeviceTestError(f"CMD:{self.cmd} produced no [TEST] line to inspect")
        return self.test_events[0]


# Espressif Systems' USB vendor ID -- what the X4's native USB Serial/JTOG
# enumerates as, regardless of platform or which /dev/cu.usbmodemNNNN /
# /dev/ttyACMN node it lands on this time (that suffix changes across
# reboots and replugs, so matching on it is not reliable).
ESPRESSIF_USB_VID = 0x303A


def get_auto_detected_ports() -> list[str]:
    """Prefers ports whose USB VID identifies them as an Espressif device --
    robust to the device node name changing on replug/reboot and to other
    USB-serial adapters being plugged in at the same time. Falls back to a
    path-glob guess only if pyserial couldn't read VID/PID at all (some
    platform/driver combos don't expose it)."""
    ports = list(list_ports.comports())
    espressif = sorted(p.device for p in ports if p.vid == ESPRESSIF_USB_VID)
    if espressif:
        return espressif

    system = platform.system()
    if system == "Darwin":
        return sorted(glob.glob("/dev/cu.usbmodem*"))
    if system == "Linux":
        return sorted(glob.glob("/dev/ttyACM*"))
    return [p.device for p in ports]


def resolve_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    ports = get_auto_detected_ports()
    if len(ports) == 1:
        return ports[0]
    if not ports:
        raise DeviceTestError("No serial port found; pass --port explicitly")
    raise DeviceTestError(f"Multiple serial ports found: {ports}; pass --port explicitly")


class DeviceTestConsole:
    """Opens the device's serial test console and drives it. Reopens the
    port transparently on I/O errors -- it disappears and re-enumerates on
    every reboot, so a single failed read/write must not be fatal."""

    def __init__(self, port: str | None = None, baud: int = DEFAULT_BAUDRATE,
                 ack_timeout: float = DEFAULT_ACK_TIMEOUT_S, reconnect_timeout: float = 30.0):
        self.port = resolve_port(port)
        self.baud = baud
        self.ack_timeout = ack_timeout
        self.reconnect_timeout = reconnect_timeout
        self._ser: serial.Serial | None = None
        self.open()

    def open(self) -> None:
        # Unlike debugging_monitor.py's target boards (a USB-UART bridge
        # chip wired to EN/GPIO0, where DTR/RTS toggling on open is the
        # classic auto-reset trick), the X4 exposes the ESP32-C3's native
        # USB Serial/JTAG peripheral directly. There, explicitly asserting
        # *or* deasserting DTR/RTS can reset the chip or wedge the CDC
        # endpoint mid-transfer -- confirmed against real hardware: a plain
        # open() that never touches those lines works reliably, so this
        # deliberately does not set ser.dtr / ser.rts at all.
        self._ser = serial.Serial(self.port, self.baud, timeout=0.2)

    def close(self) -> None:
        if self._ser is not None:
            try:
                self._ser.close()
            except (OSError, serial.SerialException):
                pass
            self._ser = None

    def _reopen(self, deadline: float) -> None:
        self.close()
        while time.monotonic() < deadline:
            try:
                self.open()
                return
            except (OSError, serial.SerialException):
                time.sleep(0.25)
        raise DeviceTestError(f"Device did not re-enumerate on {self.port} in time")

    def _readline(self, deadline: float) -> str:
        """Blocking readline with an overall deadline, decoded and stripped.
        Empty string means "no data yet" (pyserial read timeout), not EOF."""
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise DeviceTestError("Timed out waiting for serial data")
            self._ser.timeout = min(0.2, max(0.01, remaining))
            raw = self._ser.readline()
            if raw:
                return raw.decode("utf-8", errors="replace").strip()

    def _read_exact(self, size: int, deadline: float) -> bytes:
        buf = bytearray()
        while len(buf) < size:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise DeviceTestError(f"Timed out reading {size} bytes (got {len(buf)})")
            self._ser.timeout = min(0.2, max(0.01, remaining))
            chunk = self._ser.read(size - len(buf))
            if chunk:
                buf.extend(chunk)
        return bytes(buf)

    def send_command(self, cmd: str, timeout: float | None = None) -> CommandResult:
        """Sends CMD:<cmd>, waits for its CMDACK/CMDERR, and collects any
        [TEST] {...} lines printed while waiting. Reopens the port and
        retries the send if the device drops off mid-wait (e.g. it rebooted
        because of the very command being tested)."""
        timeout = self.ack_timeout if timeout is None else timeout
        deadline = time.monotonic() + timeout
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            try:
                self._ser.reset_input_buffer()
                self._ser.write(f"CMD:{cmd}\n".encode())
                return self._await_ack(cmd, deadline)
            except (OSError, serial.SerialException) as e:
                last_error = e
                self._reopen(min(deadline, time.monotonic() + self.reconnect_timeout))
        raise DeviceTestError(f"CMD:{cmd} never acked within {timeout}s (last I/O error: {last_error})")

    def _await_ack(self, cmd: str, deadline: float) -> CommandResult:
        events: list[dict] = []
        raw_lines: list[str] = []
        while True:
            line = self._readline(deadline)
            raw_lines.append(line)
            if line.startswith("[TEST] "):
                payload = line[len("[TEST] "):]
                try:
                    events.append(json.loads(payload))
                except json.JSONDecodeError as e:
                    raise DeviceTestError(f"Malformed [TEST] line for CMD:{cmd}: {payload!r} ({e})") from e
                continue
            if line == f"CMDACK:{cmd}":
                return CommandResult(cmd=cmd, ok=True, error=None, test_events=events, raw_lines=raw_lines)
            if line.startswith(f"CMDERR:") and line.endswith(f":{cmd}"):
                return CommandResult(cmd=cmd, ok=False, error=line, test_events=events, raw_lines=raw_lines)
            # Routine log noise (LOG_* output) interleaves with our ack on a
            # shared serial line; anything that isn't our ack/err is ignored.

    def capture_screenshot(self, path: str = "screenshot.bmp", timeout: float | None = None) -> tuple[bytes, "Image.Image | None"]:
        """Issues CMD:SCREENSHOT and decodes the raw framebuffer exactly like
        debugging_monitor.py's serial_worker(): Image.frombytes('1', (800,
        480), data) rotated 270 degrees (raw data is landscape). Returns the
        raw pre-rotation bytes (what CMD:FBHASH hashes) and the decoded
        image, or None for the image if PIL isn't installed."""
        timeout = self.ack_timeout if timeout is None else timeout
        deadline = time.monotonic() + timeout
        self._ser.reset_input_buffer()
        self._ser.write(b"CMD:SCREENSHOT\n")

        line = self._readline(deadline)
        while not line.startswith("SCREENSHOT_START:") and not line.startswith("CMDERR:"):
            line = self._readline(deadline)
        if line.startswith("CMDERR:"):
            raise DeviceTestError(f"CMD:SCREENSHOT rejected: {line}")

        size = int(line.split(":", 1)[1])
        data = self._read_exact(size, deadline)

        end_line = self._readline(deadline)
        if end_line != "SCREENSHOT_END":
            raise DeviceTestError(f"Expected SCREENSHOT_END, got {end_line!r}")
        ack_line = self._readline(deadline)
        if ack_line != "CMDACK:SCREENSHOT":
            raise DeviceTestError(f"Expected CMDACK:SCREENSHOT, got {ack_line!r}")

        image = None
        if Image is not None:
            image = Image.frombytes("1", (FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT), data).transpose(Image.ROTATE_270)
            image.save(path)
        else:
            with open(path, "wb") as f:
                f.write(data)
        return data, image

    # --- Convenience wrappers over the CP_TEST_CONSOLE commands ---------

    def press(self, button: str, timeout: float | None = None) -> CommandResult:
        button = button.upper()
        if button not in VALID_BUTTONS:
            raise DeviceTestError(f"Unknown button {button!r}; expected one of {VALID_BUTTONS}")
        return self.send_command(f"PRESS {button}", timeout)

    def hold(self, button: str, ms: int, timeout: float | None = None) -> CommandResult:
        button = button.upper()
        if button not in VALID_BUTTONS:
            raise DeviceTestError(f"Unknown button {button!r}; expected one of {VALID_BUTTONS}")
        # The hold unfolds asynchronously on the device once queued (see
        # MappedInputManager::injectPress); give the ack wait enough room to
        # cover the hold itself plus the normal command round-trip.
        return self.send_command(f"HOLD {button} {ms}", timeout or (self.ack_timeout + ms / 1000.0))

    def heap(self, timeout: float | None = None) -> dict:
        return self.send_command("HEAP", timeout).first_event()

    def fbhash(self, timeout: float | None = None) -> str:
        return self.send_command("FBHASH", timeout).first_event()["fbhash"]

    def activity(self, timeout: float | None = None) -> str:
        return self.send_command("ACTIVITY", timeout).first_event()["activity"]

    def selected(self, timeout: float | None = None) -> dict:
        """CMD:SELECTED: the label of whatever row/icon is currently
        highlighted -- {"activity", "supported", "selected", "index",
        "count"}. Lets a caller drive menu navigation (NAVNEXT until the
        label matches, then CONFIRM) by reading real UI content instead of
        counting rows or probing with CONFIRM; see
        scripts/device_tests/test_pairing.py's select_by_label() for the
        pattern and Activity::getSelectedRowInfo() for what's implemented."""
        return self.send_command("SELECTED", timeout).first_event()


# --- Assertion helpers -----------------------------------------------------

def assert_equal(actual, expected, message: str = "") -> None:
    if actual != expected:
        raise DeviceTestError(f"{message or 'assertion failed'}: expected {expected!r}, got {actual!r}")


def assert_changed(before, after, message: str = "") -> None:
    if before == after:
        raise DeviceTestError(f"{message or 'expected a change'}: value stayed {before!r}")


# --- CLI: discover and run scenarios under scripts/device_tests/ ----------

def _load_scenario(path: Path):
    spec = importlib.util.spec_from_file_location(path.stem, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def _discover_scenarios() -> list[Path]:
    scenarios_dir = Path(__file__).resolve().parent / "device_tests"
    return sorted(scenarios_dir.glob("test_*.py"))


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Drive the CrossPoint firmware's serial test console and assert on it.")
    parser.add_argument("--port", default=None, help="Serial port (default: auto-detect)")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUDRATE)
    parser.add_argument("--ack-timeout", type=float, default=DEFAULT_ACK_TIMEOUT_S)
    parser.add_argument("scenarios", nargs="*", help="Scenario .py files (default: scripts/device_tests/test_*.py)")
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()

    scenario_paths = [Path(s).resolve() for s in args.scenarios] if args.scenarios else _discover_scenarios()
    if not scenario_paths:
        print("No scenarios found.", file=sys.stderr)
        return 1

    try:
        device = DeviceTestConsole(port=args.port, baud=args.baud, ack_timeout=args.ack_timeout)
    except (DeviceTestError, serial.SerialException) as e:
        print(f"Failed to open device: {e}", file=sys.stderr)
        return 1

    failures = 0
    try:
        for path in scenario_paths:
            name = path.stem
            try:
                module = _load_scenario(path)
                if not hasattr(module, "run"):
                    raise DeviceTestError(f"{path} has no run(device) function")
                module.run(device)
            except Exception as e:  # pylint: disable=broad-exception-caught
                failures += 1
                print(f"FAIL {name}: {e}")
            else:
                print(f"PASS {name}")
    finally:
        device.close()

    total = len(scenario_paths)
    print(f"{total - failures}/{total} scenarios passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
