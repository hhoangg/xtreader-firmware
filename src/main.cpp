#include <Arduino.h>
#include <BoardConfig.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>
#include <SPI.h>
#include <WiFi.h>
#include <XteinkDetect.h>
#include <builtinFonts/all.h>
#if FREEINK_CAP_TOUCH
#include <esp_sntp.h>
#endif

#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "SyncCredentialStore.h"
#include "WifiCredentialStore.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/LoadingIcon.h"
#include "network/HttpDownloader.h"
#include "sync/BookDownloader.h"
#include "sync/DownloadQueue.h"
#include "sync/SyncManifest.h"
#include "sync/Telemetry.h"
#include "util/ButtonNavigator.h"
#include "util/ScreenshotUtil.h"

GfxRenderer renderer(display);
MappedInputManager mappedInputManager(gpio, renderer);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts());
static unsigned long allowSleepAt = 0;
static unsigned long lastX4ProPowerClickAt = 0;

namespace {
constexpr unsigned long X4PRO_POWER_DOUBLE_CLICK_MS = 500;
constexpr unsigned long X4PRO_POWER_CLICK_MAX_HOLD_MS = 300;
constexpr unsigned long X4PRO_RECOVERY_SETTLE_MS = 20;
constexpr unsigned long DEFAULT_RECOVERY_SETTLE_MS = 500;
}  // namespace

// A wake hold must never become an in-app power-button action.  Boot may continue
// while the button is held; swallow the one release that ends that wake gesture.
static bool wakePowerReleasePending = false;

// Fonts
EpdFont notoserif14RegularFont(&notoserif_14_regular);
EpdFont notoserif14BoldFont(&notoserif_14_bold);
EpdFont notoserif14ItalicFont(&notoserif_14_italic);
EpdFont notoserif14BoldItalicFont(&notoserif_14_bolditalic);
EpdFontFamily notoserif14FontFamily(&notoserif14RegularFont, &notoserif14BoldFont, &notoserif14ItalicFont,
                                    &notoserif14BoldItalicFont);
#ifndef OMIT_FONTS
EpdFont notoserif12RegularFont(&notoserif_12_regular);
EpdFont notoserif12BoldFont(&notoserif_12_bold);
EpdFont notoserif12ItalicFont(&notoserif_12_italic);
EpdFont notoserif12BoldItalicFont(&notoserif_12_bolditalic);
EpdFontFamily notoserif12FontFamily(&notoserif12RegularFont, &notoserif12BoldFont, &notoserif12ItalicFont,
                                    &notoserif12BoldItalicFont);
EpdFont notoserif16RegularFont(&notoserif_16_regular);
EpdFont notoserif16BoldFont(&notoserif_16_bold);
EpdFont notoserif16ItalicFont(&notoserif_16_italic);
EpdFont notoserif16BoldItalicFont(&notoserif_16_bolditalic);
EpdFontFamily notoserif16FontFamily(&notoserif16RegularFont, &notoserif16BoldFont, &notoserif16ItalicFont,
                                    &notoserif16BoldItalicFont);
EpdFont notoserif18RegularFont(&notoserif_18_regular);
EpdFont notoserif18BoldFont(&notoserif_18_bold);
EpdFont notoserif18ItalicFont(&notoserif_18_italic);
EpdFont notoserif18BoldItalicFont(&notoserif_18_bolditalic);
EpdFontFamily notoserif18FontFamily(&notoserif18RegularFont, &notoserif18BoldFont, &notoserif18ItalicFont,
                                    &notoserif18BoldItalicFont);

EpdFont notosans12RegularFont(&notosans_12_regular);
EpdFont notosans12BoldFont(&notosans_12_bold);
EpdFont notosans12ItalicFont(&notosans_12_italic);
EpdFont notosans12BoldItalicFont(&notosans_12_bolditalic);
EpdFontFamily notosans12FontFamily(&notosans12RegularFont, &notosans12BoldFont, &notosans12ItalicFont,
                                   &notosans12BoldItalicFont);
EpdFont notosans14RegularFont(&notosans_14_regular);
EpdFont notosans14BoldFont(&notosans_14_bold);
EpdFont notosans14ItalicFont(&notosans_14_italic);
EpdFont notosans14BoldItalicFont(&notosans_14_bolditalic);
EpdFontFamily notosans14FontFamily(&notosans14RegularFont, &notosans14BoldFont, &notosans14ItalicFont,
                                   &notosans14BoldItalicFont);
EpdFont notosans16RegularFont(&notosans_16_regular);
EpdFont notosans16BoldFont(&notosans_16_bold);
EpdFont notosans16ItalicFont(&notosans_16_italic);
EpdFont notosans16BoldItalicFont(&notosans_16_bolditalic);
EpdFontFamily notosans16FontFamily(&notosans16RegularFont, &notosans16BoldFont, &notosans16ItalicFont,
                                   &notosans16BoldItalicFont);
EpdFont notosans18RegularFont(&notosans_18_regular);
EpdFont notosans18BoldFont(&notosans_18_bold);
EpdFont notosans18ItalicFont(&notosans_18_italic);
EpdFont notosans18BoldItalicFont(&notosans_18_bolditalic);
EpdFontFamily notosans18FontFamily(&notosans18RegularFont, &notosans18BoldFont, &notosans18ItalicFont,
                                   &notosans18BoldItalicFont);

#endif  // OMIT_FONTS

EpdFont smallFont(&notosans_8_regular);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ui10RegularFont(&ubuntu_10_regular);
EpdFont ui10BoldFont(&ubuntu_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

EpdFont ui12RegularFont(&ubuntu_12_regular);
EpdFont ui12BoldFont(&ubuntu_12_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

// measurement of power button press duration calibration value
unsigned long t1 = 0;
unsigned long t2 = 0;

// Definitions for SilentRestart.h. RTC_NOINIT survives ESP.restart() but not power loss.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;

// How the device is coming back to life, resolved once at boot. Both resume
// flows suppress the splash and leave the panel holding its pre-boot frame; a
// plain boot shows the splash. See setup() for the resolution.
enum class BootResume : uint8_t {
  Splash,          // cold boot, flash, panic, or plain reboot
  Silent,          // heap-defrag ESP.restart() (RTC flag; lost on power loss)
  SplashlessWake,  // wake from deep sleep with the splash suppressed by the SD flag
};

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;

#if FREEINK_CAP_TOUCH
static bool finishWifiSessionWithoutRestart() {
  if (!BoardConfig::hasTouch()) return false;

  // A software reset does not cycle externally powered touch/frontlight rails.
  // Shut down the network stack in place so those peripherals retain state.
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  WiFi.mode(WIFI_OFF);
  delay(100);
  LOG_DBG("MAIN", "WiFi stopped without restart on touch device");
  return true;
}
#endif

void silentRestart() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
#if FREEINK_CAP_TOUCH
  if (finishWifiSessionWithoutRestart()) return;
#endif
  silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=home)");
  // E-ink retains the previous frame until Home's first paint lands (~2-3s).
  // Without an overlay, users don't see the reboot and fire input through to
  // Home. Select on the default selectorIndex=0 then opens the most-recent
  // book, looking like a trampoline back to the reader they just exited.
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestartToReader() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
#if FREEINK_CAP_TOUCH
  if (finishWifiSessionWithoutRestart()) return;
#endif
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=reader)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

bool handleX4ProFrontlightDoubleClick() {
  if (!BoardConfig::isX4Pro() || !gpio.wasReleased(HalGPIO::BTN_POWER)) {
    return false;
  }

  const unsigned long now = millis();
  if (gpio.getPowerButtonHeldTime() > X4PRO_POWER_CLICK_MAX_HOLD_MS) {
    lastX4ProPowerClickAt = 0;
    return false;
  }

  if (lastX4ProPowerClickAt == 0 || now - lastX4ProPowerClickAt > X4PRO_POWER_DOUBLE_CLICK_MS) {
    lastX4ProPowerClickAt = now;
    return false;
  }

  lastX4ProPowerClickAt = 0;
  const bool lightOn = !Frontlight.isOn();
  Frontlight.setOn(lightOn);
  SETTINGS.frontlightOn = lightOn ? 1 : 0;
  SETTINGS.saveToFile();
  LOG_INF("LIGHT", "Frontlight toggled %s by power-button double-click", lightOn ? "on" : "off");
  return true;
}

constexpr char SLEEP_FRAME_FILE[] = "/.crosspoint/sleep_frame.bin";

static void saveSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_FILE, file)) return;
  file.write(renderer.getFrameBuffer(), renderer.getBufferSize());
  file.close();
}

static bool loadSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) return false;
  const size_t bufferSize = display.getBufferSize();
  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  file.close();
  if (bytesRead != bufferSize) {
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }
  Storage.remove(SLEEP_FRAME_FILE);
  return true;
}

// Enter deep sleep mode
void enterDeepSleep(bool fromTimeout = false) {
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();

  const bool isQuickResumeSleep =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);
  // Every sleep mode leaves a complete retained frame on the e-ink panel. Keep
  // it visible until the first useful reader or home paint replaces it.
  APP_STATE.showBootScreen = false;

  APP_STATE.saveToFile();

  // Commit to sleeping before goToSleep() runs the outgoing activity's onExit():
  // a WiFi activity would otherwise silentRestart() here and reboot instead.
  deepSleepInProgress = true;
  activityManager.goToSleep(fromTimeout);

  if (isQuickResumeSleep) {
    saveSleepFrameBuffer();
  } else if (Storage.exists(SLEEP_FRAME_FILE)) {
    // A stale Quick Resume frame must not replace the selected sleep screen during wake.
    Storage.remove(SLEEP_FRAME_FILE);
  }

  // Tear down WiFi so the modem power domain isn't held alive across deep sleep.
  // Wake from deep sleep is effectively a chip reset, so no state needs to survive.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  halTiltSensor.deepSleep();
  display.deepSleep();
  LOG_DBG("MAIN", "Entering deep sleep");

  powerManager.startDeepSleep(gpio);
}

void setupDisplayAndFonts(bool seamless = false) {
#if !FREEINK_MCU_C3
  // C3 resolves its controller in HalGPIO::begin() before SPI claims the
  // display pins. X4 Pro skips that C3-only path, so probe here before
  // display.begin() selects and initializes its panel driver.
  static bool controllerResolved = false;
  if (!controllerResolved) {
    controllerResolved = true;
    if (freeink::applyXteinkDisplayController()) {
      LOG_DBG("MAIN", "Panel controller: UltraChip UC81xx variant detected");
    }
  }
#endif

  display.begin(seamless);
  renderer.begin();
  activityManager.begin();
  LOG_DBG("MAIN", "Display initialized");

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  renderer.insertFont(NOTOSERIF_14_FONT_ID, notoserif14FontFamily);
#ifndef OMIT_FONTS
  renderer.insertFont(NOTOSERIF_12_FONT_ID, notoserif12FontFamily);
  renderer.insertFont(NOTOSERIF_16_FONT_ID, notoserif16FontFamily);
  renderer.insertFont(NOTOSERIF_18_FONT_ID, notoserif18FontFamily);

  renderer.insertFont(NOTOSANS_12_FONT_ID, notosans12FontFamily);
  renderer.insertFont(NOTOSANS_14_FONT_ID, notosans14FontFamily);
  renderer.insertFont(NOTOSANS_16_FONT_ID, notosans16FontFamily);
  renderer.insertFont(NOTOSANS_18_FONT_ID, notosans18FontFamily);
#endif  // OMIT_FONTS
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);

  // Discover and load SD card fonts
  sdFontSystem.begin(renderer);

  LOG_DBG("MAIN", "Fonts setup");
}

// download_queue's SafetyCheck: enforces "sync from the library screen,
// never with a book open" (docs/API.md's measured heap numbers -- a TLS
// session costs ~9 KB, comfortable against the ~137 KB free on the library
// screen, risky against the ~50 KB a reading session leaves) from the one
// place that actually knows what activity is current, so the queue's worker
// task pauses itself rather than every future caller having to remember to
// check. Plain function pointer (see CLAUDE.md's "Template and std::function
// Bloat"), matching download_queue::SafetyCheck's signature.
static bool downloadQueueSafetyCheck() { return !activityManager.isReaderActivity(); }

void setup() {
  BoardConfig::holdPowerRails();

  t1 = millis();

#ifdef ENABLE_SERIAL_LOG
  // Earliest possible Serial setup. The 250 ms stall before begin() lets the
  // USB Serial/JTAG peripheral finish power-on and lets the host complete USB
  // enumeration before we touch the CDC state — otherwise cold boot races
  // and the host has to be physically replugged for logs to flow. Warm reboot
  // worked without the delay because USB was already enumerated.
  delay(250);
  Serial.begin(115200);
#if LOG_SERIAL_HAS_TX_TIMEOUT
  logSerial.setTxTimeoutMs(1);  // This is a load-bearing 1. Do not modify.
#endif
#endif

  HalSystem::begin();
  // checkPanic() clears the watchdog capture marker after a successful SD
  // dump, so retain the boot classification for the later activity route.
  const bool rebootedFromPanic = HalSystem::isRebootFromPanic();

  // Read-and-clear so a panic later in setup() doesn't loop into silent reboot.
  // Bound the target range too — RTC_NOINIT memory is uninitialized on cold boot.
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  const uint32_t snapshotTarget =
      (isSilentReboot && silentRebootTarget <= SILENT_REBOOT_TARGET_READER) ? silentRebootTarget : 0;
  silentRebootMagic = 0;
  silentRebootTarget = 0;

  gpio.begin();
  powerManager.begin();
  halTiltSensor.begin();
  halClock.begin();

  // First of two USB samples (second below, before display bring-up): the SOF
  // verdict needs two samples a frame apart, and it must be settled before the
  // first refresh — the boot paint's light-sleep slices would otherwise kill a
  // live CDC link whenever the charge-based check reads false (full battery,
  // data-only cable). See HalGPIO::pollUsbState().
  gpio.pollUsbState();

  const auto wakeupReason = gpio.getWakeupReason();

  // Latch the recovery chord before SD and settings I/O. X4 Pro uses a plain
  // digital button with 5 ms debounce; other Xteink inputs retain their legacy
  // settling window. BTN_DOWN avoids the X4 Pro's GPIO0 boot-strap pin.
  bool recoveryFirmwareMode = false;
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton) {
    const unsigned long settleMs = BoardConfig::isX4Pro() ? X4PRO_RECOVERY_SETTLE_MS : DEFAULT_RECOVERY_SETTLE_MS;
    const unsigned long settleStart = millis();
    while (millis() - settleStart < settleMs) {
      gpio.update();
      delay(10);
    }

    const uint8_t recoveryButton = BoardConfig::isX4Pro() ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP;
    if (gpio.isPressed(recoveryButton)) {
      recoveryFirmwareMode = true;
      LOG_INF("MAIN", "Recovery firmware mode (%s + POWER held at boot)", BoardConfig::isX4Pro() ? "DOWN" : "UP");
    }
  }

  // Light-sleep through the render task's e-ink BUSY wait (0.3-2 s of pure pin
  // polling) in short slices, waking exactly on the BUSY pin's completion level
  // (falls back to plain polling when WiFi/USB blocks light sleep)
  display.setBusyWaitSliceHook(
      [](int8_t busyPin, uint8_t busyLevel) { return powerManager.onEinkBusyWaitSlice(busyPin, busyLevel); });

#if FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3
  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");
#else
  LOG_INF("MAIN", "Device: %s", BoardConfig::ACTIVE.name);
#endif

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    gpio.pollUsbState();  // settle the USB verdict before the error paint (see above)
    setupDisplayAndFonts(isSilentReboot);
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }

  HalSystem::checkPanic();

  SETTINGS.loadFromFile();
  APP_STATE.loadFromFile();
  RECENT_BOOKS.loadFromFile();
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  KOREADER_STORE.loadFromFile();
  OPDS_STORE.loadFromFile();
  // NVS, not the SD card (see SyncCredentialStore.h) -- no SPI/RenderLock
  // dance needed, so it can load unconditionally at boot like the others.
  SYNC_STORE.load();
  download_queue::setSafetyCheck(&downloadQueueSafetyCheck);
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);

  // Brightness and warmth are always restored. A normal wake starts with the
  // light off unless Restore Light on Wake is enabled; silent maintenance
  // reboots preserve the live state so they do not unexpectedly go dark.
  const bool restoreLightOn = SETTINGS.frontlightOn != 0 && (SETTINGS.frontlightRestoreOnWake != 0 || isSilentReboot);
  Frontlight.begin(SETTINGS.frontlightBrightness, SETTINGS.frontlightWarmth, restoreLightOn);

  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      LOG_DBG("MAIN", "Verifying power button press duration");
      if (!gpio.verifyPowerButtonWakeup(SETTINGS.getPowerButtonDuration(),
                                        SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP)) {
        powerManager.startDeepSleep(gpio);
      }
      wakePowerReleasePending = true;
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // If USB power caused a cold boot, go back to sleep
      LOG_DBG("MAIN", "Wakeup reason: After USB Power");
#if FREEINK_DEVICE_PAPERMONO
      // There is no armable GPIO wake because the button is behind the PMIC.
      // Sleeping here would strand the device in a USB-replug boot loop.
      break;
#else
      powerManager.startDeepSleep(gpio);
      break;
#endif
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }

  // First serial output only here to avoid timing inconsistencies for power button press duration verification
  LOG_DBG("MAIN", "Starting CrossPoint version " CROSSPOINT_VERSION);

  // Resolve the single boot-presentation decision. Skipping the splash also
  // skips the panel-clearing pass and the X3 initial-full-sync arming (see
  // HalDisplay::begin), so the first paint is FAST_REFRESH (~500ms) over the
  // retained frame and input dispatches against a visible UI.
  // Only a verified deep-sleep wake may use the one-shot persisted flag.
  // Otherwise a stale flag could suppress the splash on a cold boot.
  const bool isSleepWake = wakeupReason == HalGPIO::WakeupReason::PowerButton;
  const BootResume resume = isSilentReboot                             ? BootResume::Silent
                            : isSleepWake && !APP_STATE.showBootScreen ? BootResume::SplashlessWake
                                                                       : BootResume::Splash;
  bool allowFastInitialReaderRefresh = false;

  // Second USB sample (first one right after powerManager.begin()): settles the
  // SOF host-link verdict before the first refresh can slice-sleep.
  gpio.pollUsbState();

  setupDisplayAndFonts(resume != BootResume::Splash);

  switch (resume) {
    case BootResume::Silent:
      // Splash skipped: the routing block below picks the target activity; the
      // panel keeps showing the pre-reboot popup until that first paint lands.
      break;
    case BootResume::SplashlessWake:
      // One-shot flag: re-arm the splash for the next ordinary boot. Save
      // before any painting so a hang in the blocking paint path can't strand
      // us in a splashless-with-no-frame loop on the next boot.
      APP_STATE.showBootScreen = true;
      APP_STATE.saveToFile();
      if (Storage.exists(SLEEP_FRAME_FILE) && loadSleepFrameBuffer()) {
        const bool useDifferentialRefresh = gpio.deviceIsX3();
        if (useDifferentialRefresh) {
          // begin() clears the X3 controller RAM, so restore the saved frame as
          // the baseline before replacing the moon with the loading icon.
          renderer.cleanupGrayscaleWithFrameBuffer();
        }

        const auto pageHeight = renderer.getScreenHeight();
        renderer.drawImage(LoadingIcon, 0, pageHeight - LOADINGICON_HEIGHT, LOADINGICON_WIDTH, LOADINGICON_HEIGHT);
        if (useDifferentialRefresh) {
          renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
          allowFastInitialReaderRefresh = true;
        } else {
          renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        }
      }
      break;
    case BootResume::Splash:
      activityManager.goToBoot();
      break;
  }

  // Output polarity is resolved per render by ActivityManager (night mode
  // inverts only the reading surfaces), so nothing to restore here.

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
  } else if (rebootedFromPanic) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_READER &&
             !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath);
  } else if (resume == BootResume::Silent) {
    // target == home (or reader with no open book): land on home — don't fall
    // through to the sleep-wake "resume reader" logic, which fires on stale
    // openEpubPath + lastSleepFromReader from a prior session.
    activityManager.goHome();
  } else if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
    // crashed (indicated by readerActivityLoadCount > 0)
    activityManager.goHome();
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    activityManager.goToReader(path, allowFastInitialReaderRefresh);
  }

  if (resume == BootResume::Silent) {
    // Block until the first paint physically completes. refreshDisplay()
    // waits on the panel BUSY pin so when this returns the user can see the
    // new activity. Without the wait, an edge captured by gpio.update()
    // during boot dispatches against an invisible Home and the default
    // selectorIndex=0 opens the most-recent book.
    activityManager.requestUpdateAndWait();
    // Absorb any button held at this point into currentState as a non-edge:
    // two gpio.update() calls separated by > InputManager's 5ms debounce
    // transition the held bit through lastDebounceTime into currentState
    // without setting pressedEvents, so the first loop()'s own gpio.update()
    // sees state == currentState and emits nothing.
    gpio.update();
    delay(10);
    gpio.update();
  }

  allowSleepAt = millis() + 2000;
}

// delay() counts ticks, and the tick stops while onEinkBusyWaitSlice() light-sleeps
// the chip (millis() is RTC-corrected on wake; the tick is not). A delay(10) mid-refresh
// would stretch to ~210 ms and starve button sampling. millis() stays honest.
static void delayWallClock(const unsigned long ms) {
  const unsigned long deadline = millis() + ms;
  while (static_cast<long>(millis() - deadline) < 0) {
    vTaskDelay(1);
  }
}

#ifdef CP_TEST_CONSOLE
// Maps a CMD:PRESS / CMD:HOLD button token to the logical button it injects.
// injectPress()/wasPressed()/wasReleased() key purely on the
// MappedInputManager::Button value (see MappedInputManager.cpp), so this
// works for any of them, not just the seven physical buttons on the board --
// everything else falls through to CMDERR:unknown like any other bad command.
//
// NAVNEXT/NAVPREV are load-bearing for driving any list on screen: every
// list (UiListActivity/UiTabListActivity, so Settings, its submenus, file
// browsers, ...) moves its selection on ButtonNavigator's NavNext/NavPrevious
// (src/util/ButtonNavigator.h), not on Up/Down/Left/Right directly -- those
// physical buttons only *resolve into* NavNext/NavPrevious through the
// board's own input mapping, which this console-injection path bypasses.
// Pressing CMD:PRESS DOWN or RIGHT therefore does not move a list selection
// at all; only CMD:PRESS NAVNEXT/NAVPREV does. PAGEBACK/PAGEFORWARD drive the
// reader's page turns the same way.
static bool parseTestButtonName(const String& name, MappedInputManager::Button& out) {
  if (name == "BACK") {
    out = MappedInputManager::Button::Back;
  } else if (name == "CONFIRM") {
    out = MappedInputManager::Button::Confirm;
  } else if (name == "LEFT") {
    out = MappedInputManager::Button::Left;
  } else if (name == "RIGHT") {
    out = MappedInputManager::Button::Right;
  } else if (name == "UP") {
    out = MappedInputManager::Button::Up;
  } else if (name == "DOWN") {
    out = MappedInputManager::Button::Down;
  } else if (name == "POWER") {
    out = MappedInputManager::Button::Power;
  } else if (name == "NAVNEXT") {
    out = MappedInputManager::Button::NavNext;
  } else if (name == "NAVPREV") {
    out = MappedInputManager::Button::NavPrevious;
  } else if (name == "PAGEBACK") {
    out = MappedInputManager::Button::PageBack;
  } else if (name == "PAGEFORWARD") {
    out = MappedInputManager::Button::PageForward;
  } else {
    return false;
  }
  return true;
}

// Bounded budget for CMD:HTTPGET's body-read phase, enforced from inside
// HttpDownloader's per-chunk DataCallback (see testConsoleHttpGet). This is
// the only phase we can interrupt from the outside: fetchUrl()'s connect/
// TLS-handshake/header phase runs before the first callback fires, so it is
// bounded only by HttpDownloader's own internal per-socket-op timeout
// (HTTP_TIMEOUT_MS = 60s in HttpDownloader.cpp) -- still finite, just not
// ours to shorten without changing HttpDownloader itself.
constexpr unsigned long TEST_HTTPGET_BODY_TIMEOUT_MS = 20000;
// Per-network budget while trying saved WiFi credentials, mirroring
// WifiSelectionActivity::AUTO_CONNECTION_TIMEOUT_MS (same auto-connect path,
// just driven synchronously instead of across activity loop() frames).
constexpr unsigned long TEST_WIFI_PER_NETWORK_TIMEOUT_MS = 7000;
// How much of the response body to echo back, escaped, in the [TEST] line --
// enough to eyeball a JSON healthcheck body without dumping a whole page.
constexpr size_t TEST_HTTPGET_BODY_PREVIEW_MAX = 200;

// Appends `data` as a double-quoted JSON string, escaping control characters,
// the quote/backslash, and any byte outside printable ASCII as \u00XX. This
// deliberately does NOT decode UTF-8 -- each byte gets its own \u00XX escape
// -- so it is safe over arbitrary/binary response bytes without risking a
// malformed multi-byte sequence; a JSON parser (e.g. Python's json.loads)
// still accepts it and recovers the original bytes one code point at a time.
static void appendJsonEscaped(String& out, const char* data, size_t len) {
  out += '"';
  for (size_t i = 0; i < len; i++) {
    const uint8_t c = static_cast<uint8_t>(data[i]);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20 || c >= 0x7f) {
          char esc[7];
          snprintf(esc, sizeof(esc), "\\u%04x", c);
          out += esc;
        } else {
          out += static_cast<char>(c);
        }
        break;
    }
  }
  out += '"';
}

// Bounded, synchronous saved-network auto-connect for CMD:HTTPGET. Mirrors
// WifiSelectionActivity's auto-connect order (last-connected SSID first,
// then any other saved credential) but blocks the caller instead of running
// as async activity state -- fine here since the test console handler is
// meant to run to completion before the next CMD: line is read, and every
// attempt is bounded (worst case: MAX_NETWORKS saved credentials each given
// TEST_WIFI_PER_NETWORK_TIMEOUT_MS, still finite).
static bool testConsoleConnectWifi(std::string& outSsid, std::string& outError) {
  if (WiFi.status() == WL_CONNECTED) {
    outSsid = WiFi.SSID().c_str();
    return true;
  }

  {
    // SD card access (loadFromFile) shares SPI with the display; matches
    // WifiSelectionActivity::onEnter()'s use of the same lock.
    RenderLock lock;
    WIFI_STORE.loadFromFile();
  }

  const size_t savedCount = WIFI_STORE.getCredentialCount();
  if (savedCount == 0) {
    outError = "no saved wifi credentials";
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);       // credentials are managed by WifiCredentialStore, not SDK NVS
  WiFi.disconnect(true, true);  // abort any in-progress SDK auto-connect
  delay(100);

  const auto tryCredential = [](const std::string& ssid, const std::string& password) -> bool {
    LOG_DBG("TEST", "HTTPGET: attempting saved network %s", ssid.c_str());
    WiFi.disconnect();
    delay(50);
    if (!password.empty()) {
      WiFi.begin(ssid.c_str(), password.c_str());
    } else {
      WiFi.begin(ssid.c_str());
    }
    const unsigned long deadline = millis() + TEST_WIFI_PER_NETWORK_TIMEOUT_MS;
    while (static_cast<long>(millis() - deadline) < 0) {
      const wl_status_t status = WiFi.status();
      if (status == WL_CONNECTED) return true;
      if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) break;
      delay(50);
    }
    return false;
  };

  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  bool triedLast = false;
  if (!lastSsid.empty()) {
    const auto cred = WIFI_STORE.findCredential(lastSsid);
    if (cred) {
      triedLast = true;
      if (tryCredential(cred->ssid, cred->password)) {
        outSsid = cred->ssid;
        return true;
      }
    }
  }

  for (size_t i = 0; i < savedCount; i++) {
    const auto cred = WIFI_STORE.getCredentialAt(i);
    if (!cred || (triedLast && cred->ssid == lastSsid)) continue;
    if (tryCredential(cred->ssid, cred->password)) {
      outSsid = cred->ssid;
      return true;
    }
  }

  outError = "failed to connect to any saved network";
  return false;
}

// CMD:HTTPGET <url> -- probes whether this device can complete an HTTP(S)
// request through the exact code path a real sync client would use
// (HttpDownloader: same TLS stack, same CA-bundle verification, same
// streaming reader). Brings WiFi up first if needed. Reports a single
// [TEST] JSON line; see the field-by-field comments below for its shape.
static void testConsoleHttpGet(const std::string& url) {
  const size_t heapBeforeFree = ESP.getFreeHeap();
  const size_t heapBeforeMaxAlloc = ESP.getMaxAllocHeap();
  // Sampled inside the first DataCallback invocation below -- the earliest
  // point HttpDownloader's public API exposes, which is after both the TLS
  // handshake and the response headers have been read (fetchUrl has no hook
  // in between). Defaults to the "before" sample if the body is empty and
  // the callback never fires.
  size_t heapTlsFree = heapBeforeFree;
  size_t heapTlsMaxAlloc = heapBeforeMaxAlloc;
  bool midHandshakeSampled = false;

  std::string ssid;
  std::string wifiError;
  const bool wifiConnected = testConsoleConnectWifi(ssid, wifiError);

  bool ok = false;
  int httpStatus = -1;  // stays -1 if the request never got an HTTP response at all
  size_t bytesRead = 0;
  std::string bodyPreview;
  std::string error;

  if (!wifiConnected) {
    error = "wifi";
  } else {
    bool timedOut = false;
    const unsigned long bodyDeadline = millis() + TEST_HTTPGET_BODY_TIMEOUT_MS;
    const HttpDownloader::DataCallback onData = [&](const uint8_t* data, size_t len) -> bool {
      if (!midHandshakeSampled) {
        midHandshakeSampled = true;
        heapTlsFree = ESP.getFreeHeap();
        heapTlsMaxAlloc = ESP.getMaxAllocHeap();
      }
      bytesRead += len;
      if (bodyPreview.size() < TEST_HTTPGET_BODY_PREVIEW_MAX) {
        const size_t room = TEST_HTTPGET_BODY_PREVIEW_MAX - bodyPreview.size();
        const size_t take = len < room ? len : room;
        bodyPreview.append(reinterpret_cast<const char*>(data), take);
      }
      if (static_cast<long>(millis() - bodyDeadline) > 0) {
        timedOut = true;
        return false;  // abort the transfer; never block the loop indefinitely
      }
      return true;
    };

    ok = HttpDownloader::fetchUrl(url, onData, "", "", &httpStatus);

    if (!ok) {
      if (timedOut) {
        error = "timeout";
      } else if (httpStatus >= 0 && httpStatus != 200) {
        error = "http_status";
      } else {
        error = "transport";  // connect/DNS/TLS failure -- no HTTP response at all
      }
    }
  }

  const size_t heapAfterFree = ESP.getFreeHeap();
  const size_t heapAfterMaxAlloc = ESP.getMaxAllocHeap();

  String out = "[TEST] {";
  out += "\"wifiConnected\":";
  out += (wifiConnected ? "true" : "false");
  out += ",\"ssid\":";
  appendJsonEscaped(out, ssid.data(), ssid.size());
  out += ",\"wifiError\":";
  appendJsonEscaped(out, wifiError.data(), wifiError.size());
  out += ",\"ok\":";
  out += (ok ? "true" : "false");
  out += ",\"status\":";
  out += String(httpStatus);
  out += ",\"bytesRead\":";
  out += String(static_cast<unsigned>(bytesRead));
  out += ",\"error\":";
  appendJsonEscaped(out, error.data(), error.size());
  out += ",\"heapBeforeFree\":";
  out += String(static_cast<unsigned>(heapBeforeFree));
  out += ",\"heapBeforeMaxAlloc\":";
  out += String(static_cast<unsigned>(heapBeforeMaxAlloc));
  out += ",\"heapTlsFree\":";
  out += String(static_cast<unsigned>(heapTlsFree));
  out += ",\"heapTlsMaxAlloc\":";
  out += String(static_cast<unsigned>(heapTlsMaxAlloc));
  out += ",\"midHandshakeSampled\":";
  out += (midHandshakeSampled ? "true" : "false");
  out += ",\"heapAfterFree\":";
  out += String(static_cast<unsigned>(heapAfterFree));
  out += ",\"heapAfterMaxAlloc\":";
  out += String(static_cast<unsigned>(heapAfterMaxAlloc));
  out += ",\"bodyPreview\":";
  appendJsonEscaped(out, bodyPreview.data(), bodyPreview.size());
  out += "}";
  logSerial.println(out);
}

// CMD:MANIFESTSYNC -- probes GET /library/manifest end to end against the
// paired sync account, through the exact code path a real sync would use
// (sync_manifest::sync(): HttpDownloader with a Bearer token, streamed
// through ManifestStreamParser, written to /.crosspoint/remote.idx). Brings
// WiFi up first, same as CMD:HTTPGET. Reports a single [TEST] JSON line
// carrying the three heap samples the sync brief asks for -- before the
// request, after the first page's TLS handshake, after the last page --
// plus how many pages/entries were fetched and any error code.
static void testConsoleManifestSync() {
  std::string ssid;
  std::string wifiError;
  const bool wifiConnected = testConsoleConnectWifi(ssid, wifiError);

  sync_manifest::SyncResult result;
  if (wifiConnected) {
    result = sync_manifest::sync();
  } else {
    result.error = "wifi";
  }

  String out = "[TEST] {";
  out += "\"wifiConnected\":";
  out += (wifiConnected ? "true" : "false");
  out += ",\"ssid\":";
  appendJsonEscaped(out, ssid.data(), ssid.size());
  out += ",\"wifiError\":";
  appendJsonEscaped(out, wifiError.data(), wifiError.size());
  out += ",\"ok\":";
  out += (result.ok ? "true" : "false");
  out += ",\"error\":";
  appendJsonEscaped(out, result.error.data(), result.error.size());
  out += ",\"pagesFetched\":";
  out += String(static_cast<unsigned>(result.pagesFetched));
  out += ",\"entriesWritten\":";
  out += String(static_cast<unsigned>(result.entriesWritten));
  out += ",\"totalCount\":";
  out += String(static_cast<unsigned>(result.totalCount));
  out += ",\"heapBeforeFree\":";
  out += String(static_cast<unsigned>(result.beforeRequest.freeHeap));
  out += ",\"heapBeforeMaxAlloc\":";
  out += String(static_cast<unsigned>(result.beforeRequest.maxAllocHeap));
  out += ",\"heapTlsFree\":";
  out += String(static_cast<unsigned>(result.afterHandshake.freeHeap));
  out += ",\"heapTlsMaxAlloc\":";
  out += String(static_cast<unsigned>(result.afterHandshake.maxAllocHeap));
  out += ",\"heapAfterFree\":";
  out += String(static_cast<unsigned>(result.afterLastPage.freeHeap));
  out += ",\"heapAfterMaxAlloc\":";
  out += String(static_cast<unsigned>(result.afterLastPage.maxAllocHeap));
  out += "}";
  logSerial.println(out);
}

// CMD:MANIFESTFIRST -- reports the id/path of the first entry (sorted by
// path) in the local manifest index, so a host-side device test can pick a
// real id to hand to CMD:BOOKDOWNLOAD/CMD:QUEUEADD without hardcoding one.
// Requires a prior CMD:MANIFESTSYNC (or a real sync); reports found=false,
// not an error, if the index is empty or has never been synced.
static void testConsoleManifestFirst() {
  struct FirstMatch {
    std::string id;
    std::string path;
    bool found = false;
  };
  FirstMatch match;
  const auto onMatch = [](void* ctxPtr, const ManifestIndexRecord& record) -> bool {
    auto* m = static_cast<FirstMatch*>(ctxPtr);
    m->id = record.id;
    m->path = record.path;
    m->found = true;
    return false;  // one match is enough -- stop the scan
  };
  sync_manifest::listByPrefix("", onMatch, &match);

  String out = "[TEST] {\"found\":";
  out += (match.found ? "true" : "false");
  out += ",\"id\":";
  appendJsonEscaped(out, match.id.data(), match.id.size());
  out += ",\"path\":";
  appendJsonEscaped(out, match.path.data(), match.path.size());
  out += "}";
  logSerial.println(out);
}

// CMD:BOOKDOWNLOAD <id> -- probes GET /library/:id/file end to end (the
// exact code path a real download will use: book_downloader::download(),
// HttpDownloader with a Bearer token, streamed to a temp file, renamed into
// place, the manifest index's downloaded flag flipped). Requires the id to
// already be in the local manifest index (run CMD:MANIFESTSYNC first).
// Brings WiFi up first, same as CMD:HTTPGET/CMD:MANIFESTSYNC. Reports a
// single [TEST] JSON line with the same three-point heap sampling.
static void testConsoleBookDownload(const std::string& id) {
  std::string ssid;
  std::string wifiError;
  const bool wifiConnected = testConsoleConnectWifi(ssid, wifiError);

  book_downloader::DownloadResult result;
  if (wifiConnected) {
    result = book_downloader::download(id);
  } else {
    result.error = "wifi";
  }

  String out = "[TEST] {";
  out += "\"wifiConnected\":";
  out += (wifiConnected ? "true" : "false");
  out += ",\"ssid\":";
  appendJsonEscaped(out, ssid.data(), ssid.size());
  out += ",\"ok\":";
  out += (result.ok ? "true" : "false");
  out += ",\"error\":";
  appendJsonEscaped(out, result.error.data(), result.error.size());
  out += ",\"httpStatus\":";
  out += String(result.httpStatus);
  out += ",\"bytesDownloaded\":";
  out += String(static_cast<unsigned>(result.bytesDownloaded));
  out += ",\"destPath\":";
  appendJsonEscaped(out, result.destPath.data(), result.destPath.size());
  out += ",\"heapBeforeFree\":";
  out += String(static_cast<unsigned>(result.beforeRequest.freeHeap));
  out += ",\"heapBeforeMaxAlloc\":";
  out += String(static_cast<unsigned>(result.beforeRequest.maxAllocHeap));
  out += ",\"heapTlsFree\":";
  out += String(static_cast<unsigned>(result.afterHandshake.freeHeap));
  out += ",\"heapTlsMaxAlloc\":";
  out += String(static_cast<unsigned>(result.afterHandshake.maxAllocHeap));
  out += ",\"heapAfterFree\":";
  out += String(static_cast<unsigned>(result.afterDownload.freeHeap));
  out += ",\"heapAfterMaxAlloc\":";
  out += String(static_cast<unsigned>(result.afterDownload.maxAllocHeap));
  out += "}";
  logSerial.println(out);
}

// CMD:QUEUEADD <id> -- brings WiFi up (same as above) and enqueues `id` onto
// the background download_queue, mirroring how the file browser will call
// it once wired up. Reports the enqueue outcome immediately; the actual
// download happens on the worker task and is watched via CMD:QUEUESTATUS.
static void testConsoleQueueAdd(const std::string& id) {
  std::string ssid;
  std::string wifiError;
  const bool wifiConnected = testConsoleConnectWifi(ssid, wifiError);

  const download_queue::EnqueueOutcome outcome =
      wifiConnected ? download_queue::enqueue(id) : download_queue::EnqueueOutcome::NotPaired;

  const char* outcomeStr = "unknown";
  switch (outcome) {
    case download_queue::EnqueueOutcome::Ok:
      outcomeStr = "ok";
      break;
    case download_queue::EnqueueOutcome::Full:
      outcomeStr = "full";
      break;
    case download_queue::EnqueueOutcome::AlreadyQueued:
      outcomeStr = "already_queued";
      break;
    case download_queue::EnqueueOutcome::NotFound:
      outcomeStr = "not_found";
      break;
    case download_queue::EnqueueOutcome::NotPaired:
      outcomeStr = "not_paired";
      break;
  }

  logSerial.printf("[TEST] {\"wifiConnected\":%s,\"outcome\":\"%s\"}\n", wifiConnected ? "true" : "false", outcomeStr);
}

// CMD:QUEUESTATUS -- a snapshot of the queue's current state (no network,
// no WiFi bring-up): item count, whether the worker task is running, the
// front item's progress if one is downloading, and the most recent
// finished item's outcome (a failure is visible here, not silent).
static void testConsoleQueueStatus() {
  const download_queue::Snapshot snap = download_queue::snapshot();

  String out = "[TEST] {\"count\":";
  out += String(static_cast<unsigned>(snap.count));
  out += ",\"workerRunning\":";
  out += (snap.workerRunning ? "true" : "false");
  if (snap.count > 0) {
    const download_queue::QueueItem& front = snap.items[0];
    out += ",\"frontId\":";
    appendJsonEscaped(out, front.id.data(), front.id.size());
    out += ",\"frontDownloaded\":";
    out += String(static_cast<unsigned>(front.downloadedBytes));
    out += ",\"frontTotal\":";
    out += String(static_cast<unsigned>(front.totalBytes));
  }
  out += ",\"lastResultAvailable\":";
  out += (snap.lastResult.hasResult ? "true" : "false");
  if (snap.lastResult.hasResult) {
    out += ",\"lastResultId\":";
    appendJsonEscaped(out, snap.lastResult.id.data(), snap.lastResult.id.size());
    out += ",\"lastResultOk\":";
    out += (snap.lastResult.ok ? "true" : "false");
    out += ",\"lastResultError\":";
    appendJsonEscaped(out, snap.lastResult.error.data(), snap.lastResult.error.size());
  }
  out += "}";
  logSerial.println(out);
}

// CMD:QUEUECANCEL -- empties the queue and aborts whatever is downloading.
static void testConsoleQueueCancel() {
  download_queue::cancelAll();
  logSerial.println("[TEST] {\"cancelled\":true}");
}

// CMD:HEARTBEAT -- POST /devices/heartbeat with a minimal, fixed payload
// (just the firmware version -- every other field is optional and omitted).
// Brings WiFi up first.
static void testConsoleHeartbeat() {
  std::string ssid;
  std::string wifiError;
  const bool wifiConnected = testConsoleConnectWifi(ssid, wifiError);

  telemetry::TelemetryResult result;
  if (wifiConnected) {
    result = telemetry::sendHeartbeat(telemetry::HeartbeatInfo{});
  } else {
    result.error = "wifi";
  }

  logSerial.printf("[TEST] {\"wifiConnected\":%s,\"ok\":%s,\"httpStatus\":%d,\"error\":\"%s\"}\n",
                   wifiConnected ? "true" : "false", result.ok ? "true" : "false", result.httpStatus,
                   result.error.c_str());
}

// CMD:REQUESTBOOKS -- POST /feedback/request-books. Brings WiFi up first.
static void testConsoleRequestBooks() {
  std::string ssid;
  std::string wifiError;
  const bool wifiConnected = testConsoleConnectWifi(ssid, wifiError);

  telemetry::TelemetryResult result;
  if (wifiConnected) {
    result = telemetry::requestBooks();
  } else {
    result.error = "wifi";
  }

  logSerial.printf("[TEST] {\"wifiConnected\":%s,\"ok\":%s,\"httpStatus\":%d,\"error\":\"%s\"}\n",
                   wifiConnected ? "true" : "false", result.ok ? "true" : "false", result.httpStatus,
                   result.error.c_str());
}

// CMD:BOOKFINISHED <id> -- POST /events/book-finished with {"bookId": id}.
// Brings WiFi up first. Not wired into ReaderActivity's own "book finished"
// detection by this task -- see this task's report for why.
static void testConsoleBookFinished(const std::string& id) {
  std::string ssid;
  std::string wifiError;
  const bool wifiConnected = testConsoleConnectWifi(ssid, wifiError);

  telemetry::TelemetryResult result;
  if (wifiConnected) {
    result = telemetry::bookFinished(id);
  } else {
    result.error = "wifi";
  }

  logSerial.printf("[TEST] {\"wifiConnected\":%s,\"ok\":%s,\"httpStatus\":%d,\"error\":\"%s\"}\n",
                   wifiConnected ? "true" : "false", result.ok ? "true" : "false", result.httpStatus,
                   result.error.c_str());
}
#endif

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.setSharedConfirmPowerShortPressEmitsPower(SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP);
  // mappedInputManager.update() (not a raw gpio.update()): in CP_TEST_CONSOLE
  // builds this is also the frame boundary that clears the injected-input
  // edges (see MappedInputManager::update()), so an injected CMD:PRESS reads
  // true for exactly one loop() iteration, however many times an activity
  // queries it within that iteration.
  mappedInputManager.update();
  halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.orientation, activityManager.isReaderActivity());

  renderer.setFadingFix(SETTINGS.fadingFix);

  if (Serial && millis() - lastMemPrint >= 10000) {
    LOG_INF("MEM", "Free: %d bytes, Total: %d bytes, Min Free: %d bytes, MaxAlloc: %d bytes", ESP.getFreeHeap(),
            ESP.getHeapSize(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
    lastMemPrint = millis();
  }

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings
  if (logSerial.available() > 0) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      bool handled = true;
      if (cmd == "SCREENSHOT") {
        const uint32_t bufferSize = display.getBufferSize();
        logSerial.printf("SCREENSHOT_START:%d\n", bufferSize);
        uint8_t* buf = display.getFrameBuffer();
        // The global 1ms TX timeout (see setup()) exists so routine logging
        // never blocks the loop when no host is draining it; at that
        // timeout a single write() of the ~48KB framebuffer gives up after
        // roughly one USB CDC buffer and silently drops the rest. Raise it
        // only for this dump, and only long enough per chunk to let an
        // actively-reading host keep up (USB CDC drains this in well under
        // a millisecond in practice) -- then restore the load-bearing
        // default no matter how the dump ends. Writing in bounded chunks
        // (rather than one 48KB call) means a stalled host only blocks the
        // loop for one chunk's timeout, not the whole transfer, and lets us
        // detect a short write and stop instead of trusting the whole
        // buffer made it out. buf already IS the framebuffer, so this
        // writes slices of it directly -- nothing here allocates.
#if LOG_SERIAL_HAS_TX_TIMEOUT
        constexpr uint32_t kScreenshotTxTimeoutMs = 200;
        constexpr uint32_t kScreenshotChunkSize = 1024;
        logSerial.setTxTimeoutMs(kScreenshotTxTimeoutMs);
        uint32_t sent = 0;
        while (sent < bufferSize) {
          const uint32_t remaining = bufferSize - sent;
          const uint32_t chunkLen = remaining < kScreenshotChunkSize ? remaining : kScreenshotChunkSize;
          const size_t written = logSerial.write(buf + sent, chunkLen);
          sent += written;
          if (written < chunkLen) break;  // host stalled/disappeared; stop rather than spin
        }
        logSerial.setTxTimeoutMs(1);  // restore the load-bearing default from setup()
#else
        logSerial.write(buf, bufferSize);
#endif
        logSerial.printf("SCREENSHOT_END\n");
      }
#ifdef CP_TEST_CONSOLE
      else if (cmd.startsWith("PRESS ")) {
        String name = cmd.substring(6);
        name.trim();
        MappedInputManager::Button button;
        if (parseTestButtonName(name, button)) {
          mappedInputManager.injectPress(button);
        } else {
          handled = false;
        }
      } else if (cmd.startsWith("HOLD ")) {
        String rest = cmd.substring(5);
        rest.trim();
        const int sp = rest.indexOf(' ');
        MappedInputManager::Button button;
        unsigned long holdMs = 0;
        if (sp > 0 && parseTestButtonName(rest.substring(0, sp), button)) {
          String msStr = rest.substring(sp + 1);
          msStr.trim();
          holdMs = static_cast<unsigned long>(msStr.toInt());
        }
        if (holdMs > 0) {
          mappedInputManager.injectPress(button, holdMs);
        } else {
          handled = false;
        }
      } else if (cmd == "HEAP") {
        // Machine-readable, matches the [MEM] log stats above but on demand.
        logSerial.printf("[TEST] {\"heap\":%u,\"maxAlloc\":%u,\"minFreeHeap\":%u}\n",
                         static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()),
                         static_cast<unsigned>(ESP.getMinFreeHeap()));
      } else if (cmd == "FBHASH") {
        // FNV-1a 32-bit over the raw framebuffer: cheap enough to run every
        // frame, lets the harness assert "changed" / "matches" without
        // pulling the full ~48 KB buffer over serial like CMD:SCREENSHOT does.
        const uint32_t bufferSize = display.getBufferSize();
        const uint8_t* buf = display.getFrameBuffer();
        uint32_t hash = 2166136261u;
        for (uint32_t i = 0; i < bufferSize; i++) {
          hash ^= buf[i];
          hash *= 16777619u;
        }
        logSerial.printf("[TEST] {\"fbhash\":\"%08x\",\"size\":%u}\n", hash, bufferSize);
      } else if (cmd == "ACTIVITY") {
        const Activity* activity = activityManager.getCurrentActivity();
        logSerial.printf("[TEST] {\"activity\":\"%s\"}\n", activity ? activity->getName().c_str() : "");
      } else if (cmd == "SELECTED") {
        // Reports whatever row/icon is currently highlighted, so a host
        // script can drive menu navigation by reading real UI content
        // instead of counting rows or probing with CONFIRM (which mutates
        // toggle settings and triggers real side effects like a Wi-Fi scan
        // -- see Activity::getSelectedRowInfo()'s comment for why this
        // command exists).
        const Activity* activity = activityManager.getCurrentActivity();
        std::string label;
        int index = -1;
        int count = -1;
        const bool supported = activity != nullptr && activity->getSelectedRowInfo(label, index, count);
        String out = "[TEST] {\"activity\":\"";
        out += activity ? activity->getName().c_str() : "";
        out += "\",\"supported\":";
        out += (supported ? "true" : "false");
        out += ",\"selected\":";
        appendJsonEscaped(out, label.data(), label.size());
        out += ",\"index\":";
        out += String(index);
        out += ",\"count\":";
        out += String(count);
        out += "}";
        logSerial.println(out);
      } else if (cmd.startsWith("HTTPGET ")) {
        String urlArg = cmd.substring(8);
        urlArg.trim();
        if (urlArg.startsWith("http://") || urlArg.startsWith("https://")) {
          testConsoleHttpGet(std::string(urlArg.c_str()));
        } else {
          handled = false;
        }
      } else if (cmd == "MANIFESTSYNC") {
        testConsoleManifestSync();
      } else if (cmd == "MANIFESTFIRST") {
        testConsoleManifestFirst();
      } else if (cmd.startsWith("BOOKDOWNLOAD ")) {
        String idArg = cmd.substring(13);
        idArg.trim();
        if (idArg.length() > 0) {
          testConsoleBookDownload(std::string(idArg.c_str()));
        } else {
          handled = false;
        }
      } else if (cmd.startsWith("QUEUEADD ")) {
        String idArg = cmd.substring(9);
        idArg.trim();
        if (idArg.length() > 0) {
          testConsoleQueueAdd(std::string(idArg.c_str()));
        } else {
          handled = false;
        }
      } else if (cmd == "QUEUESTATUS") {
        testConsoleQueueStatus();
      } else if (cmd == "QUEUECANCEL") {
        testConsoleQueueCancel();
      } else if (cmd == "HEARTBEAT") {
        testConsoleHeartbeat();
      } else if (cmd == "REQUESTBOOKS") {
        testConsoleRequestBooks();
      } else if (cmd.startsWith("BOOKFINISHED ")) {
        String idArg = cmd.substring(13);
        idArg.trim();
        if (idArg.length() > 0) {
          testConsoleBookFinished(std::string(idArg.c_str()));
        } else {
          handled = false;
        }
      } else if (cmd == "SLEEP") {
        // Last known-good marker for the host to compare against once the device
        // wakes back up (or to inspect if it never does). Printed before the ack,
        // same ordering as HEAP/FBHASH/ACTIVITY above, so a host reading for the
        // ack via the usual [TEST]-lines-then-CMDACK protocol still captures it.
        const Activity* activity = activityManager.getCurrentActivity();
        logSerial.printf("[TEST] {\"activity\":\"%s\",\"heap\":%u}\n", activity ? activity->getName().c_str() : "",
                         static_cast<unsigned>(ESP.getFreeHeap()));
        // enterDeepSleep() cuts power via powerManager.startDeepSleep() and never
        // returns, so the USB port vanishes with it. Ack now: sending CMDACK after
        // the call (like every other command below) would just be lost.
        logSerial.printf("CMDACK:%s\n", cmd.c_str());
        // Force the ack out before anything can tear the link down: the global TX
        // timeout is 1 ms, and startDeepSleep() calls logSerial.end(). In practice
        // enterDeepSleep() spends hundreds of ms saving state and repainting first,
        // but a silently dropped ack would look identical to a device that hung.
        logSerial.flush();
        // fromTimeout=false: a console-triggered sleep is a manual trigger, the
        // same as the power-button hold path below (`enterDeepSleep()`, no arg),
        // not the inactivity-timeout path (`enterDeepSleep(true)`). Passing false
        // keeps SETTINGS.quickResumeSleepScreen's "after timeout" branch honest --
        // it must not fire just because this was sent over serial instead of a
        // real held button.
        enterDeepSleep();
        // enterDeepSleep() calls esp_deep_sleep_start() and does not return.
        return;
      }
#endif
      else {
        handled = false;
      }
      // Raw print, not LOG_*: debugging_monitor.py keys on this ack to report
      // command success, so it must survive LOG_LEVEL=0 builds. Commands
      // compiled out of this build report unknown.
      logSerial.printf(handled ? "CMDACK:%s\n" : "CMDERR:unknown:%s\n", cmd.c_str());
    }
  }

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || gpio.wasTouchActivity() || halTiltSensor.hadActivity() ||
      activityManager.preventAutoSleep()) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }

  // Let wake continue as soon as its hold has been verified. The release can
  // arrive after setup, so consume that one input frame rather than making it
  // a page turn, refresh, or other short power-button action.
  if (wakePowerReleasePending && !gpio.isPressed(HalGPIO::BTN_POWER)) {
    wakePowerReleasePending = false;
    return;
  }

  static bool screenshotButtonsReleased = true;
  static bool screenshotComboActive = false;
  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN)) {
    screenshotComboActive = true;
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      {
        RenderLock lock;
        ScreenshotUtil::takeScreenshot(renderer);
      }
    }
    return;
  }
  if (screenshotComboActive) {
    if (gpio.isPressed(HalGPIO::BTN_POWER)) return;
    if (gpio.wasReleased(HalGPIO::BTN_POWER)) {
      screenshotButtonsReleased = true;
      screenshotComboActive = false;
      return;
    }
    screenshotButtonsReleased = true;
    screenshotComboActive = false;
  }

  // Consume the second X4 Pro power-button release so it does not also run a
  // configured short-power action after toggling the frontlight.
  if (handleX4ProFrontlightDoubleClick()) {
    return;
  }

#if FREEINK_CAP_TOUCH
  // A single X4 Pro power click becomes Confirm only after the frontlight
  // double-click window expires without a second click.
  mappedInputManager.setPowerConfirmClickFrame(false);
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PWR_CONFIRM && BoardConfig::isX4Pro() &&
      lastX4ProPowerClickAt != 0 && millis() - lastX4ProPowerClickAt > X4PRO_POWER_DOUBLE_CLICK_MS) {
    lastX4ProPowerClickAt = 0;
    mappedInputManager.setPowerConfirmClickFrame(true);
  }
#endif

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (sleepTimeoutMs > 0 && millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep(true);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // A hold that woke the device must be released before it can count as a new
  // in-app long press. Otherwise a user who keeps holding after wake would put
  // the device straight back to sleep once allowSleepAt expires.
  static bool powerReleasedSinceWake = false;
  if (!gpio.isPressed(HalGPIO::BTN_POWER)) powerReleasedSinceWake = true;

  if (powerReleasedSinceWake && millis() >= allowSleepAt && gpio.isPressed(HalGPIO::BTN_POWER) &&
      gpio.getPowerButtonHeldTime() > SETTINGS.getPowerButtonDuration()) {
    // If the screenshot combination is potentially being pressed, don't sleep
    if (gpio.isPressed(HalGPIO::BTN_DOWN)) {
      return;
    }
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

#if FREEINK_DEVICE_PAPERMONO
  // Paper Mono reports the PMIC power button as a one-tick click, so the held
  // path above cannot fire. With the default Ignore action, retain the normal
  // power-button meaning and shut down; explicit alternate bindings still win.
  if ((SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP ||
       SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::IGNORE) &&
      millis() >= allowSleepAt && mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    enterDeepSleep();
    return;
  }
#endif

  // Refresh screen when power button is short-pressed with FORCE_REFRESH setting.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH &&
      mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    LOG_DBG("MAIN", "Manual screen refresh triggered");
    if (!activityManager.handleForcedRefresh()) {
      RenderLock lock;
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  if (gpio.wasUsbStateChanged()) {
    activityManager.requestUpdate();
  }

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  const unsigned long activityDuration = millis() - activityStartTime;

  // Body complete: releases the slice hook's yield (see onEinkBusyWaitSlice).
  powerManager.noteMainLoopIteration();

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (activityManager.skipLoopDelay()) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    const unsigned long idleMs = millis() - lastActivityTime;
    if (idleMs >= HalPowerManager::IDLE_LIGHT_SLEEP_MS) {
      // Idle: light-sleep between input polls instead of busy-delaying (same poll cadence).
      // Race-to-sleep: run the brief wake windows at normal clock, not LOW_POWER_FREQ.
      // The board's sleep-floor current is paid per-millisecond regardless of CPU
      // speed, so finishing the per-wake work ~16x faster and returning to sleep
      // costs less charge than stretching the window at 10 MHz (measured at 10 MHz:
      // 8.8 mA for 4.5 ms per wake). The downclock below only serves the pre-sleep
      // 100 Hz delay-poll phase. The lightSleep()-rejected fallback delay() then
      // also runs at normal clock, but that only happens when USB (externally
      // powered), WiFi, or a render Lock (full speed wanted anyway) is active.
      powerManager.setPowerSaving(false);
      if (gpio.isDebouncePending()) {
        // A raw button-state change is mid-debounce: commitment needs a second
        // matching sample, so poll again quickly instead of sleeping a slice —
        // a tap shorter than the 50 ms cadence would otherwise land in a single
        // sample and be dropped, and every press would commit a slice late.
        delayWallClock(10);
      } else if (!powerManager.lightSleep(gpio)) {
        // Light sleep declined = a render Lock, USB, or WiFi is active — the
        // chip is at full clock anyway, so poll at 100 Hz. A 50 ms cadence
        // here dropped sub-slice power taps (a press needs two samples >=5 ms
        // apart to commit), which made short-press sleep flaky during renders
        // — exactly when a render Lock forces this fallback.
        delayWallClock(10);
      }
    } else {
      // Response window after recent input: keep 100 Hz polling for snappy interaction,
      // but downclock once rapid-input bursts have settled — renders re-raise the clock
      // via HalPowerManager::Lock, so full speed only serves loop bookkeeping here
      if (idleMs >= HalPowerManager::IDLE_DOWNCLOCK_MS) {
        powerManager.setPowerSaving(true);
      }
      delayWallClock(10);
    }
  }
}
