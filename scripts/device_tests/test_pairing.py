"""
Device pairing (QR login) scenario for the CP_TEST_CONSOLE serial harness.

Drives the on-device menu from Home into Settings > System > Account Sync. If
the hub offers "Pair Device", runs the pairing flow: confirms
SyncPairingActivity is on screen, captures a screenshot of the QR code, and
reports free heap bracketed around the Wi-Fi-plus-TLS episode that fetches
the pairing code (POST /device/code) -- the same before/after-a-network-op
pattern test_https.py uses for CMD:HTTPGET, applied here to the real pairing
flow instead of a synthetic probe.

If the hub instead offers "Unlink Device", the device is already paired.
This scenario verifies and reports that (the hub rows are enough evidence)
and skips the rest cleanly -- it never unlinks a working pairing just to
force a clean run every time; see the branch in run() for why.

Two things this script cannot do, both noted where they matter below:
  - It cannot sample heap *during* the blocking POST /device/code call: the
    firmware's main loop (which also services this serial console) blocks
    for the duration of that call, exactly like every other network op in
    this codebase. CMD:HEAP sent while it's in flight just waits until it
    returns, then reports the *post*-call value. So "before" here means
    "right before the button press that starts Wi-Fi + the code request",
    not "mid-handshake" -- test_https.py's CMD:HTTPGET is the tool for a
    true mid-handshake sample against this origin.
  - It reads the userCode back via the [TEST] line SyncPairingActivity
    prints once on entering the QR screen (see SyncPairingActivity.cpp),
    not OCR on the screenshot -- this harness has no OCR.

List/menu navigation uses NAVNEXT/NAVPREV, never UP/DOWN/LEFT/RIGHT. Every
list on this firmware -- Home's icon row, Settings and its tabs, this
feature's own SyncSettingsActivity hub -- moves its selection through
ButtonNavigator, which is wired to the logical NavNext/NavPrevious buttons
(src/util/ButtonNavigator.h), not the physical direction buttons. On real
hardware, Up/Down/Left/Right *resolve into* NavNext/NavPrevious via the
board's own input mapping; CMD:PRESS injects a button directly and bypasses
that resolution, so CMD:PRESS DOWN (or RIGHT, UP, LEFT) does not move a
selection at all. NAVNEXT/NAVPREV is what actually needs to be exercised;
see main.cpp's parseTestButtonName() for the injection side of this.

The device does not start at Home -- it resumes the last-open book straight
into EpubReader on boot. navigate_to_home() below presses BACK in a loop
until CMD:ACTIVITY reports "Home" *before* anything else in this scenario is
attempted; nothing here assumes a starting screen.

Menu rows are found by reading them, not by counting or probing. Earlier
versions of this scenario tried two things that both turned out wrong:

  1. A hardcoded row index. A row's position is data-dependent, not just
     code-dependent -- Home prepends one tile per recent book, and any
     Settings category can grow or shrink the same way. That picks the
     wrong row silently on any configuration other than the one it was
     measured against.
  2. Probing: CONFIRM the current row, check CMD:ACTIVITY, back out if
     wrong, try the next row. This is worse than the bug it was meant to
     fix -- it actually toggles real settings in place (rows like "Show
     Hidden Files" have no navigation to detect and back out of) and
     triggers real side effects (CONFIRMing "Wi-Fi Networks" on the way to
     something else starts a real scan). Do not resurrect this approach.

CMD:SELECTED (see Activity::getSelectedRowInfo(), guarded by CP_TEST_CONSOLE)
reports the *label* of whatever is currently highlighted without acting on
it. select_by_label() below just reads that label and presses NAVNEXT until
it matches, then CONFIRMs exactly once -- no mutation, no side effects, no
row counting, immune to menu reordering and to configuration-dependent rows.
Implemented for HomeActivity, SettingsActivity, and SyncSettingsActivity (the
three screens this flow touches); other UiListActivity subclasses don't have
it yet.

Matching is on the English label text (e.g. "Settings", "Account Sync",
"Pair Device"), since that's what CMD:SELECTED reports back verbatim from
I18N -- this assumes the device's UI language is English. A device left in
another language will fail select_by_label() with a clear "not found, saw
X/Y/Z" error rather than picking the wrong row.

Run via:  python3 scripts/device_test.py [--port ...] scripts/device_tests/test_pairing.py
"""

from __future__ import annotations

import time

from device_test import DeviceTestConsole, DeviceTestError

BOOT_TIMEOUT_S = 30.0
RENDER_SETTLE_S = 1.0

# How many BACK presses to try, at most, to work back to the Home screen from
# wherever the device happens to be sitting when this scenario starts --
# generous, since a cold-resumed EpubReader could in principle have an
# overlay or confirm dialog on top before BACK actually reaches Home.
MAX_BACK_TO_HOME_PRESSES = 8

# Default bound for select_by_label()'s search -- comfortably more than
# Home's menu (4 base icons, plus however many recent-book cover tiles
# precede them) or the SyncSettingsActivity hub
# (3 fixed rows) will ever have.
SELECT_MAX_STEPS_DEFAULT = 12

# Settings > System currently has up to 11 rows (Time to Sleep, Show Hidden
# Files, Remove Read from Recents, Move Finished to Read, Wi-Fi Networks,
# KOReader Sync, Account Sync, Clear Reading Cache, Check for
# Updates, SD Firmware Update, Language -- see SettingsList.h /
# SettingsActivity.cpp::rebuildSettingsLists()); this only needs to be a
# generous upper bound, not exact.
SELECT_MAX_STEPS_SETTINGS_SYSTEM = 16

# SettingsActivity opens on the Display tab with the tab band focused (ring
# position 0). CONFIRM there cycles to the next category
# (Display -> Reader -> Controls -> System); three presses land on System
# with the tab band still focused. See SettingsActivity::handleButtons().
# Unlike row order within a category, category order is a fixed enum
# (SettingsActivity::categoryNames), not reordered by device configuration,
# so a fixed press count is safe here -- and CMD:SELECTED confirms it: at
# ring 0 it reports the active tab's own label, so a script could check
# "System" was actually reached, though this scenario doesn't bother since
# the category enum order is compiled in, not owner-configurable.
SETTINGS_CATEGORY_CYCLE_PRESSES = 3

PAIRING_ACTIVITY_NAME = "SyncPairing"
CODE_REQUEST_TIMEOUT_S = 45.0  # Wi-Fi auto-connect + POST /device/code
QR_EVENT_POLL_INTERVAL_S = 2.0


def wait_for_boot(device: DeviceTestConsole, timeout: float = BOOT_TIMEOUT_S) -> str:
    """Poll CMD:ACTIVITY until the console answers (mirrors test_smoke.py)."""
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            return device.activity(timeout=2.0)
        except DeviceTestError as e:
            last_error = e
            time.sleep(0.5)
    raise DeviceTestError(f"Device never answered CMD:ACTIVITY within {timeout}s ({last_error})")


def navigate_to_home(device: DeviceTestConsole) -> None:
    activity = device.activity()
    for _ in range(MAX_BACK_TO_HOME_PRESSES):
        if activity == "Home":
            return
        device.press("BACK")
        time.sleep(RENDER_SETTLE_S)
        activity = device.activity()
    if activity != "Home":
        raise DeviceTestError(
            f"Could not get back to Home within {MAX_BACK_TO_HOME_PRESSES} BACK presses "
            f"(stuck on {activity!r}) -- run from Home manually and retry"
        )


def assert_activity(device: DeviceTestConsole, expected: str, context: str) -> None:
    actual = device.activity()
    if actual != expected:
        raise DeviceTestError(f"{context}: expected activity {expected!r}, got {actual!r}")


def select_by_label(device: DeviceTestConsole, wanted_label: str, max_steps: int = SELECT_MAX_STEPS_DEFAULT) -> None:
    """Reads the currently highlighted row/icon's label via CMD:SELECTED and
    presses NAVNEXT until it matches `wanted_label`, then CONFIRMs exactly
    once. See the module docstring for why this replaced both a hardcoded
    row index and a CONFIRM-and-back-out probe.

    A selection with no text label (event["selected"] == "", e.g. one of
    Home's recent-book cover tiles) is skipped, not treated as a mismatch
    worth stopping on -- it's a legitimate state CMD:SELECTED reports, not
    an error."""
    seen: list[str] = []
    for _ in range(max_steps):
        event = device.selected()
        if not event.get("supported"):
            raise DeviceTestError(
                f"select_by_label({wanted_label!r}): activity {event.get('activity')!r} does not "
                f"implement CMD:SELECTED"
            )
        label = event.get("selected", "")
        seen.append(label)
        if label == wanted_label:
            device.press("CONFIRM")
            time.sleep(RENDER_SETTLE_S)
            return
        device.press("NAVNEXT")
        time.sleep(RENDER_SETTLE_S)
    raise DeviceTestError(
        f"select_by_label: {wanted_label!r} not found within {max_steps} steps. Labels seen: {seen}"
    )


# SyncSettingsActivity::MENU_ITEMS: Server URL, Status, then the Pair/Unlink
# action -- fixed and owned by this feature, unlike Settings > System, so a
# hardcoded row count here is not the fragility the module docstring warns
# about elsewhere.
HUB_ROW_COUNT = 3


def navigate_to_sync_hub(device: DeviceTestConsole) -> None:
    """Home -> Settings -> System -> Account Sync. Leaves the cursor on the
    hub's first row (Server URL) -- what to do next is the caller's call,
    made by reading the hub's own rows (see scan_hub_rows()) rather than
    assumed here, since whether the device already has a pairing changes
    what those rows are."""
    select_by_label(device, "Settings")
    assert_activity(device, "Settings", "Home -> Settings navigation")

    for _ in range(SETTINGS_CATEGORY_CYCLE_PRESSES):
        device.press("CONFIRM")  # ring position 0 (tab band): cycles category, not a row
        time.sleep(RENDER_SETTLE_S)
    # Now on the System tab, tab band still focused (ring 0). One NAVNEXT
    # descends into the first row -- CONFIRM at ring 0 cycles tabs instead of
    # activating a row, so select_by_label must not start until the cursor
    # is actually on a row.
    device.press("NAVNEXT")
    time.sleep(RENDER_SETTLE_S)

    select_by_label(device, "Account Sync", max_steps=SELECT_MAX_STEPS_SETTINGS_SYSTEM)
    assert_activity(device, "SyncSettings", "Settings -> Account Sync navigation")


def scan_hub_rows(device: DeviceTestConsole, count: int = HUB_ROW_COUNT) -> list[str]:
    """Reads each row's label on the Account Sync hub via CMD:SELECTED, in
    order, starting from wherever the cursor currently is (row 0 on a fresh
    entry, which is what navigate_to_sync_hub() leaves it at). Read-only --
    advances with NAVNEXT, never CONFIRMs -- so on its own this cannot pair
    or unlink anything. Leaves the cursor sitting on the last row read."""
    labels: list[str] = []
    for i in range(count):
        event = device.selected()
        if not event.get("supported"):
            raise DeviceTestError(f"scan_hub_rows: activity {event.get('activity')!r} does not implement CMD:SELECTED")
        labels.append(event.get("selected", ""))
        if i < count - 1:
            device.press("NAVNEXT")
            time.sleep(RENDER_SETTLE_S)
    return labels


def wait_for_qr_code(device: DeviceTestConsole, timeout: float = CODE_REQUEST_TIMEOUT_S) -> dict:
    """Polls CMD:ACTIVITY (a cheap no-op command) until a [TEST] line carrying
    userCode shows up in one of its responses -- SyncPairingActivity prints
    that line exactly once, on entering the QR screen (see
    SyncPairingActivity::enterQrScreen()), so it may land in the response
    window of any command sent around that time, not necessarily this one."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = device.send_command("ACTIVITY", timeout=5.0)
        for event in result.test_events:
            if "userCode" in event:
                return event
        if result.first_event().get("activity") == "SyncPairing":
            time.sleep(QR_EVENT_POLL_INTERVAL_S)
            continue
        # Left the pairing activity before the QR ever showed up (Wi-Fi or
        # the code request failed and the activity moved to its Failure
        # screen, or the flow was otherwise interrupted).
        raise DeviceTestError(
            f"Left {PAIRING_ACTIVITY_NAME!r} before the QR code appeared "
            f"(now on {result.first_event().get('activity')!r})"
        )
    raise DeviceTestError(f"No userCode [TEST] line seen within {timeout}s of entering {PAIRING_ACTIVITY_NAME!r}")


def run(device: DeviceTestConsole) -> None:
    wait_for_boot(device)
    navigate_to_home(device)

    heap_before = device.heap()
    print(f"[test_pairing] heap before Wi-Fi + code request: {heap_before}")

    navigate_to_sync_hub(device)
    print("[test_pairing] confirmed activity: SyncSettings")

    hub_rows = scan_hub_rows(device)
    print(f"[test_pairing] Account Sync hub rows: {hub_rows}")

    # scan_hub_rows() left the cursor on the last row read -- the action row,
    # third and last of the hub's fixed three. Its label tells us whether
    # this device is already paired; branch on that rather than assuming.
    action_row = hub_rows[-1] if hub_rows else ""
    if action_row == "Unlink Device":
        # Already paired. The hub rows are enough evidence -- do not unlink
        # to force a clean run: that would be the same class of mistake as
        # the toggle-flipping probe this scenario used to do, just aimed at
        # a more consequential row. Verify and skip instead.
        print(
            "[test_pairing] device is already paired (hub offers 'Unlink Device', not 'Pair Device') -- "
            "skipping the pairing flow rather than unlinking a working pairing to force a clean run"
        )
        device.press("BACK")
        time.sleep(RENDER_SETTLE_S)
        return
    if action_row != "Pair Device":
        raise DeviceTestError(f"Account Sync hub's action row is neither 'Pair Device' nor 'Unlink Device': {hub_rows}")

    device.press("CONFIRM")
    time.sleep(RENDER_SETTLE_S)
    assert_activity(device, PAIRING_ACTIVITY_NAME, "Account Sync -> Pair Device navigation")
    print(f"[test_pairing] confirmed activity: {PAIRING_ACTIVITY_NAME}")

    code_event = wait_for_qr_code(device)
    heap_after = device.heap()
    print(f"[test_pairing] pairing code issued: {code_event}")
    print(f"[test_pairing] heap after Wi-Fi + code request: {heap_after}")
    if isinstance(heap_before.get("heap"), int) and isinstance(heap_after.get("heap"), int):
        print(f"[test_pairing] free heap delta: {heap_after['heap'] - heap_before['heap']} bytes")

    user_code = code_event.get("userCode")
    if not user_code:
        raise DeviceTestError(f"userCode event had no userCode field: {code_event}")
    print(f"[test_pairing] userCode: {user_code!r} (approve at the printed verificationUri on another device)")

    _data, image = device.capture_screenshot(path="pairing_qr.bmp")
    if image is not None:
        print("[test_pairing] QR screenshot saved to pairing_qr.bmp")
    else:
        print("[test_pairing] QR screenshot saved to pairing_qr.bmp (raw bytes; install Pillow to decode)")

    # Leave the device the way this scenario found it: cancel out of the
    # pairing screen rather than leaving Wi-Fi associated and a code
    # outstanding for the rest of a test run.
    device.press("BACK")
    time.sleep(RENDER_SETTLE_S)
