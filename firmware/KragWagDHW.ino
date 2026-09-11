/*
 * ============================================================
 *  KragWag DHW — Domestic Hot Water Cylinder Controller
 *  ESP32-C3-WROOM-02U  |  Arduino + ESP RainMaker
 *  Firmware v3.0.0-dhw  (repurposed from the KragWag alarm/gate
 *  controller v2.0.0 Build 46 — see firmware/ history on `main`
 *  for the original energiser/gate firmware this was forked from)
 * ============================================================
 *
 *  HARDWARE SUMMARY  (repurposed from the alarm board — same PCB,
 *  no redesign, only the wiring at the screw terminals changes)
 *  ---------------------------------------------------------
 *  GPIO0   IO0   I2C SDA to ADS1115         (was: Gate 1 output; R19 10k pullup
 *                                             doubles as an I2C pull-up)
 *  GPIO1   ADC input   12V supply monitor (unchanged from v1.x/v2.x)
 *  GPIO3   IO3   I2C SCL to ADS1115         (was: Gate 2 output; R18 10k pullup
 *                                             doubles as an I2C pull-up)
 *  GPIO4   Green LED   Status indicator (active-HIGH, PWM) — now shows DHW status
 *  GPIO5   SIREN input  SPARE / unused on this build (was Z13 siren detect).
 *                       Left configured as an isolated input in case a future
 *                       revision wants a flow switch or door contact here.
 *  GPIO6   IO6   Relay 2 drive — BOTTOM element (active-LOW, 10k pullup R17,
 *                idle HIGH = relay off, matches the original gate-output wiring
 *                convention — CONFIRM against the actual relay module polarity
 *                before energising a real element)
 *  GPIO7   White LED  WiFi/cloud status (active-HIGH, PWM) — unchanged
 *  GPIO9   BOOT button  Hold 5s to factory-reset (WiFi + cloud pairing) — unchanged
 *  GPIO10  IN1   Relay 1 drive — TOP element (opto-isolated, active-HIGH via
 *                R4 -> U13 opto -> short IN1, idle LOW = relay off — this was
 *                previously PULSED for arm/disarm; it is now held continuously
 *                HIGH/LOW for as long as the relay should be on/off)
 *  Red LED (PWR)  Always on — connected to 3.3V via R8, no GPIO needed
 *
 *  *** THE ONE NON-NEGOTIABLE SAFETY RULE ***
 *  ---------------------------------------------------------
 *  This firmware enforces its own hard overtemperature cutoff and treats a
 *  missing/invalid temperature reading as "turn the relay off" — entirely
 *  independent of WiFi, the Raspberry Pi (`kragwag-hub`), RainMaker/the
 *  cloud, or the app. Those are convenience/economy layers on TOP of this
 *  firmware; none of them are ever the only thing standing between the tank
 *  and an unsafe state. See updateElementSafety() below.
 *
 *  RAINMAKER DEVICES / PARAMETERS
 *  ---------------------------------------------------------
 *  Two independent Switch-type devices for Alexa/Google mapping:
 *    "DHW Top"     -- Power (R/W toggle, primary), Temperature (RO), Current (RO),
 *                       Status (RO text: Idle / Heating / Safety Lockout ...)
 *    "DHW Bottom"  -- same shape as DHW Top
 *  Plus the original board-level device "KragWag" for infra params:
 *    Supply Voltage, Firmware Build, Check OTA, OTA Status, WiFi Signal,
 *    WiFi Live, Local IP, LED Brightness, Low Volt Alert, FCM/Worker
 *    plumbing, Notify OTA, Notify DHW Events, Restore Defaults, and the new
 *    "Hub URL" / "Hub API Key" settings the ESP32 uses to talk to
 *    kragwag-hub on the Pi (GET/POST, plain HTTP — LAN only by default).
 *
 *  FIRST-TIME SETUP / FACTORY RESET / OTA
 *  ---------------------------------------------------------
 *  Unchanged from the alarm firmware — see the original header in
 *  firmware/ on `main` for the full walkthrough. In short: ESP RainMaker
 *  app -> Add Device -> scan QR (SoftAP, POP "kragwag1"); hold BOOT 5s to
 *  factory-reset; OTA is served from this repo's kragwag.bin via
 *  raw.githubusercontent.com (unchanged mechanism, not yet wired to a
 *  DHW-specific release channel — see kragwag-release skill before using it
 *  for this branch).
 *
 *  REQUIRED BOARD SUPPORT / LIBRARIES
 *  ---------------------------------------------------------
 *  Same as the alarm firmware: ESP32 Arduino board package 3.x (includes
 *  RainMaker). No new library dependencies — the ADS1115 driver below is
 *  hand-written against Wire.h (no Adafruit_ADS1X15), and hub JSON is
 *  hand-built/parsed with String, matching this codebase's existing style.
 * ============================================================
 */

// -- Firmware version ----------------------------------------------------------
#define FIRMWARE_VERSION  "3.0.0"
#define FIRMWARE_BUILD    11        // DHW branch build counter -- independent of the
                                    // alarm firmware's build numbers on main.

// -- GitHub OTA ----------------------------------------------------------------
// The DHW board has its OWN update channel, separate from the alarm/fence
// firmware. This matters: the two build counters are independent, and the
// alarm channel's version.txt reads 46 while this firmware is on a single
// digit. Sharing a channel meant this board saw itself as dozens of builds
// behind and would happily download the 1.75 MB FENCE image and flash itself
// with it, replacing the hot water controller with an energiser controller.
//
// Separate URLs alone are not enough, because "fetch this address and flash
// whatever comes back" still trusts a typo, a swapped file, or a 404 page.
// So the manifest names the variant it is for, and this firmware refuses
// anything that is not its own. The alarm firmware is untouched and keeps
// reading its plain version.txt at the repo root.
#define OTA_VARIANT      "kragwag-dhw"
#define OTA_MANIFEST_URL "https://raw.githubusercontent.com/ScriptPilotX/kragwag/main/dhw/version.json"

// -- RainMaker provisioning ----------------------------------------------------
#define PROV_SERVICE_NAME  "PROV_KragWag"
#define PROV_POP           "kragwag1"

// -- Libraries -----------------------------------------------------------------
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include "RMaker.h"
#include "WiFiProv.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include "esp_ota_ops.h"
#include <Wire.h>
#include <math.h>

// -- Pin definitions -----------------------------------------------------------
#define PIN_IO0         0   // I2C SDA
#define PIN_ADC_12V     1
#define PIN_IO3         3   // I2C SCL
#define PIN_LED_GREEN   4   // PWM via LEDC
#define PIN_SIREN       5   // spare / unused
#define PIN_IO6         6   // Relay 2 -- Bottom element (active-LOW)
#define PIN_LED_WHITE   7   // PWM via LEDC
#define PIN_BOOT_BTN    9
#define PIN_IN1         10  // Relay 1 -- Top element (active-HIGH, opto-isolated)

// -- RainMaker parameter name constants ---------------------------------------
// Board-level device ("KragWag")
#define PN_VOLTAGE      "Supply Voltage"
#define PN_FW_BUILD     "Firmware Build"
#define PN_OTA_CHECK    "Check OTA"
#define PN_OTA_STATUS   "OTA Status"
#define PN_WIFI_SIGNAL  "WiFi Signal"
#define PN_WIFI_LIVE    "WiFi Live"
#define PN_LOCAL_IP     "Local IP"
#define PN_LED_BRIGHT   "LED Brightness"
#define PN_VOLT_THRESH  "Low Volt Alert"
#define PN_FCM_TOKEN    "FCM Token"
#define PN_WORKER_URL   "Worker URL"
#define PN_WORKER_SEC   "Worker Secret"
#define PN_NOTIF_OTA    "Notify OTA"
#define PN_NOTIF_DHW    "Notify DHW Events"
#define PN_RESTORE      "Restore Defaults"
#define PN_HUB_URL      "Hub URL"       // kragwag-hub base URL, e.g. http://192.168.99.23:8420
#define PN_HUB_KEY      "Hub API Key"   // bearer token -- set from the app, never hardcoded

// Per-element device params (same names reused on both DHW Top / DHW Bottom devices)
#define PN_EL_POWER     "Power"
#define PN_EL_TEMP      "Temperature"
#define PN_EL_CURRENT   "Current"
#define PN_EL_STATUS    "Status"

// -- Default settings values ---------------------------------------------------
#define DEFAULT_LED_BRIGHTNESS  255
#define DEFAULT_VOLT_THRESHOLD  10.0f
#define DEFAULT_NOTIF_OTA       true
#define DEFAULT_NOTIF_DHW       true
#define DEFAULT_HUB_URL         "http://192.168.99.23:8420"

// -- Timing --------------------------------------------------------------------
#define VOLTAGE_CHECK_MS   60000
#define HEARTBEAT_MS       300000
#define WORKER_HEARTBEAT_URL_SUFFIX "/heartbeat"
#define ADC_SAMPLES        8
#define WIFI_RESET_HOLD_MS 5000
#define DHW_SAFETY_CHECK_MS   2000    // how often the on-device safety loop re-evaluates
#define DHW_CURRENT_CHECK_MS  10000   // how often current is sampled (200ms blocking each time)
#define DHW_HUB_REPORT_MS     30000   // how often telemetry is POSTed to kragwag-hub
#define DHW_HUB_POLL_MS       5000    // how often pending-commands is polled
#define DHW_HUB_POLL_RETRY_MS 2000    // ...and how soon to retry one that failed
#define DHW_POLL_MIN_HEAP     24000   // below this, do not even attempt the poll

// -- Hardware constants --------------------------------------------------------
#define ADC_DIVIDER_RATIO  (50.0f / 11.0f)
#define ADC_CAL_FACTOR     (11.92f / 12.92f)
#define LED_PWM_FREQ       5000
#define LED_PWM_BITS       8

// -- DHW safety constants -------------------------------------------------------
// *** Confirm both of these against the actual cylinder/element/thermostat
// *** manufacturer limits before commissioning. These are firmware-enforced
// *** software backstops, independent of and in addition to any mechanical
// *** thermostat/TCO already built into the immersion elements.
#define DHW_SAFETY_MAX_C      65.0f   // hard ceiling -- relay forced off at/above this
#define DHW_SAFETY_RESET_C    60.0f   // hysteresis -- must drop back below this to re-arm
#define DHW_SENSOR_TIMEOUT_MS 30000UL // no valid temp reading in this long -> fail safe (off)
#define NTC_SUPPLY_VOLTS      3.3f    // assumed ADS1115/NTC divider supply -- CONFIRM against hardware

// -- Commissioning ("test") mode ------------------------------------------------
// Lets the relays be driven on the bench BEFORE any temperature sensor exists,
// so contactors and wiring can be proven. It is not a safety override, and it
// is deliberately narrow:
//
//   * It relaxes ONLY the missing-sensor case. If a sensor IS reading and the
//     tank is at or above DHW_SAFETY_MAX_C, the overtemp lockout still fires
//     and still wins. Test mode can never mask a hot cylinder.
//   * It expires on its own after DHW_COMMISSION_MS with no further input.
//   * It lives in RAM only, so a reset or power cycle clears it.
//   * On expiry both elements are commanded off, so an old "on" cannot come
//     back to life the next time test mode is entered.
//
// NEVER leave this active with an immersion element connected to a cylinder
// that is not full. Dry firing destroys the element in seconds.
#define DHW_COMMISSION_MS     (15UL * 60UL * 1000UL)   // 15 minutes, then self-cancels

// -- NVS -----------------------------------------------------------------------
#define NVS_NAMESPACE      "kragwag"
#define NVS_KEY_LED_BRIGHT "led_bright"
#define NVS_KEY_VOLT_THR   "volt_thr"
#define NVS_KEY_FCM_TOKEN  "fcm_token"
#define NVS_KEY_WORKER_URL "worker_url"
#define NVS_KEY_WORKER_SEC "worker_sec"
#define NVS_KEY_NTFY_OTA   "ntfy_ota"
#define NVS_KEY_NTFY_DHW   "ntfy_dhw"
#define NVS_KEY_HUB_URL    "hub_url"
#define NVS_KEY_HUB_KEY    "hub_key"

// -- LED pattern types ---------------------------------------------------------
enum LedPattern { LED_OFF, LED_ON, LED_SLOW_BLINK, LED_FAST_BLINK, LED_DOUBLE_PULSE };

// -- Runtime settings (loaded from NVS on boot, updated via app) --------------
uint8_t  settingLedBrightness = DEFAULT_LED_BRIGHTNESS;
float    settingVoltThreshold = DEFAULT_VOLT_THRESHOLD;
bool     settingNotifyOTA     = DEFAULT_NOTIF_OTA;
bool     settingNotifyDHW     = DEFAULT_NOTIF_DHW;

// -- Runtime state -------------------------------------------------------------
bool lowVoltAlerted = false;
String   fcmToken   = "";   // set via RainMaker app; no default token hardcoded
String   workerUrl  = "https://kragwag-notif.fabian-kandlinger.workers.dev/notify";
String   workerSec  = "";   // set via RainMaker app; no default secret hardcoded
String   hubUrl      = DEFAULT_HUB_URL;
String   hubKey      = "";  // set via RainMaker app; no default secret hardcoded
bool cloudConnected = false;

bool          otaCheckRequested = false;
bool          wifiLiveMode      = false;
unsigned long lastWifiCheck      = 0;
#define WIFI_NORMAL_MS  60000
#define WIFI_LIVE_MS    2000
unsigned long bootBtnPressStart = 0;
bool          bootBtnHeld       = false;

unsigned long lastVoltageCheck   = 0;
unsigned long lastHeartbeat      = 0;
unsigned long lastSafetyCheck    = 0;
unsigned long lastCurrentCheck   = 0;
unsigned long lastHubReport      = 0;
unsigned long lastHubPoll        = 0;
// Set by any poll that did not come back with a 200, so the next loop pass
// retries promptly instead of forfeiting the whole interval.
volatile bool hubPollFailed      = false;

bool          syncNeeded = false;
bool          provDeinitNeeded = false;
unsigned long syncAt     = 0;

// -- LED state -----------------------------------------------------------------
struct LedState { bool output; uint8_t phase; unsigned long lastChange; };
LedState greenLed = {false, 0, 0};
LedState whiteLed = {false, 0, 0};

// -- Global objects ------------------------------------------------------------
Preferences   prefs;
WebServer     localServer(8080);
bool          localServerStarted = false;

// Last HTTP status the hub returned to our telemetry POST. Written from the
// ingest task, read by the local /status page -- an int write is atomic on this
// core, so no lock is needed for a value that is only ever displayed.
//   0 = nothing sent yet, <0 = transport-level failure (see HTTPClient errors),
//   200/401/... = what the hub actually replied.
// This exists so the config page can answer "did that key work?" on the spot
// rather than sending you to the Pi's log.
volatile int  lastIngestHttp = 0;
static Device kragwagDev("KragWag", "esp.device.other", NULL);

// -----------------------------------------------------------------------------
//  DHW ELEMENT MODEL — one struct per heating element, one RainMaker Device each.
// -----------------------------------------------------------------------------
struct DhwElement {
  const char* name;            // "top" / "bottom" -- MUST match kragwag-hub's element id
  const char* displayName;     // RainMaker device name
  uint8_t relayPin;
  bool    relayActiveHigh;     // true: HIGH=on (IN1/Top). false: LOW=on (IO6/Bottom).
  uint8_t tempAdcChannel;
  uint8_t currentAdcChannel;
  bool    desiredOn;           // last commanded state (app toggle or hub command)
  bool    relayOn;             // actual relay output state, after safety gating
  bool    safetyLockout;       // true while latched off (overtemp or sensor fault)
  float   lastTempC;
  float   lastCurrentA;
  unsigned long lastValidTempMs;
  volatile int8_t pendingHubCmd;  // -1 none, 0 off, 1 on -- written by poll task, consumed in loop()
  Device* dev;
};

DhwElement elementTop    = { "top",    "DHW Top",    PIN_IN1, true,  2, 0,
                              false, false, false, -999.0f, 0.0f, 0, -1, nullptr };
DhwElement elementBottom = { "bottom", "DHW Bottom", PIN_IO6, false, 3, 1,
                              false, false, false, -999.0f, 0.0f, 0, -1, nullptr };

// -----------------------------------------------------------------------------
//  LED HELPERS  (PWM-aware, unchanged from the alarm firmware)
// -----------------------------------------------------------------------------
inline void ledOn (uint8_t pin) { ledcWrite(pin, settingLedBrightness); }
inline void ledOff(uint8_t pin) { ledcWrite(pin, 0); }

void runLed(uint8_t pin, LedPattern pattern, LedState &s) {
  unsigned long now = millis();
  switch (pattern) {
    case LED_OFF:
      ledOff(pin); s.output = false; return;
    case LED_ON:
      ledOn(pin);  s.output = true;  return;
    case LED_SLOW_BLINK:
      if (now - s.lastChange >= 500) {
        s.output = !s.output;
        s.output ? ledOn(pin) : ledOff(pin);
        s.lastChange = now;
      } break;
    case LED_FAST_BLINK:
      if (now - s.lastChange >= 80) {
        s.output = !s.output;
        s.output ? ledOn(pin) : ledOff(pin);
        s.lastChange = now;
      } break;
    case LED_DOUBLE_PULSE: {
      const uint16_t seq[]  = {200, 150, 200, 800};
      const bool     vals[] = {true, false, true, false};
      if (now - s.lastChange >= seq[s.phase]) {
        s.phase = (s.phase + 1) % 4;
        s.output = vals[s.phase];
        s.output ? ledOn(pin) : ledOff(pin);
        s.lastChange = now;
      } break;
    }
  }
}

// -----------------------------------------------------------------------------
//  ADS1115 DRIVER (raw I2C, no external library)
// -----------------------------------------------------------------------------

#define ADS1115_I2C_ADDRESS     0x48
#define ADS1115_REG_CONVERSION  0x00
#define ADS1115_REG_CONFIG      0x01
#define ADS1115_TIMEOUT_MS      50UL
#define ADS1115_LSB_VOLTS       0.000125f

/**
 * Initializes I2C for the ADS1115 at its default address.
 */
void ads1115Init() {
  Wire.begin(PIN_IO0, PIN_IO3);
}

/**
 * Reads one single-ended ADS1115 channel in single-shot mode.
 *
 * Uses the +/-4.096V PGA range and 128 samples-per-second data rate.
 * Returns zero if the channel is invalid, an I2C transaction fails, or the
 * conversion does not complete within the timeout.
 */
int16_t ads1115ReadRaw(uint8_t channel) {
  if (channel > 3) {
    return 0;
  }

  const uint16_t muxBits = (uint16_t)(0x04U + channel) << 12;
  const uint16_t config =
      0x8000U |  // OS: start a single conversion
      muxBits |  // MUX: AIN0..AIN3 relative to GND
      0x0200U |  // PGA: +/-4.096V
      0x0100U |  // MODE: single-shot
      0x0080U |  // DR: 128 SPS
      0x0003U;   // Comparator disabled

  Wire.beginTransmission(ADS1115_I2C_ADDRESS);
  Wire.write(ADS1115_REG_CONFIG);
  Wire.write((uint8_t)(config >> 8));
  Wire.write((uint8_t)(config & 0xFF));
  if (Wire.endTransmission() != 0) {
    return 0;
  }

  const uint32_t startTime = millis();

  while ((uint32_t)(millis() - startTime) < ADS1115_TIMEOUT_MS) {
    Wire.beginTransmission(ADS1115_I2C_ADDRESS);
    Wire.write(ADS1115_REG_CONFIG);
    if (Wire.endTransmission(false) != 0) {
      return 0;
    }

    if (Wire.requestFrom((uint8_t)ADS1115_I2C_ADDRESS, (uint8_t)2) != 2) {
      return 0;
    }

    const uint16_t currentConfig =
        ((uint16_t)Wire.read() << 8) |
        (uint16_t)Wire.read();

    if ((currentConfig & 0x8000U) != 0) {
      Wire.beginTransmission(ADS1115_I2C_ADDRESS);
      Wire.write(ADS1115_REG_CONVERSION);
      if (Wire.endTransmission(false) != 0) {
        return 0;
      }

      if (Wire.requestFrom((uint8_t)ADS1115_I2C_ADDRESS, (uint8_t)2) != 2) {
        return 0;
      }

      const uint16_t raw =
          ((uint16_t)Wire.read() << 8) |
          (uint16_t)Wire.read();

      return (int16_t)raw;
    }
  }

  return 0;
}

/**
 * Reads one ADS1115 channel and converts the raw value to volts.
 */
float ads1115ReadVoltage(uint8_t channel) {
  return (float)ads1115ReadRaw(channel) * ADS1115_LSB_VOLTS;
}

// -----------------------------------------------------------------------------
//  NTC THERMISTOR TEMPERATURE CONVERSION
// -----------------------------------------------------------------------------

#define NTC_R_FIXED       10000.0f  // ohms, the fixed divider resistor
#define NTC_R_NOMINAL     10000.0f  // ohms at 25C -- MUST confirm against actual NTC datasheet
#define NTC_T_NOMINAL_C   25.0f
#define NTC_BETA          3950.0f   // Beta(25/85) or Beta(25/50) -- MUST confirm against actual NTC datasheet

/**
 * Converts the midpoint voltage of a high-side fixed resistor and low-side
 * NTC divider to degrees Celsius using the simplified Beta equation.
 *
 * Returns -999.0f for invalid voltages indicating an open circuit, short
 * circuit, or invalid supply voltage -- callers treat this as "no reading"
 * and MUST fail safe (see updateElementSafety()).
 */
float ntcVoltageToCelsius(float vOut, float vSupply) {
  if (vSupply <= 0.0f || vOut <= 0.0f || vOut >= vSupply) {
    return -999.0f;
  }

  const float ntcResistance =
      NTC_R_FIXED * vOut / (vSupply - vOut);

  if (ntcResistance <= 0.0f) {
    return -999.0f;
  }

  const float nominalKelvin = NTC_T_NOMINAL_C + 273.15f;
  const float inverseKelvin =
      (1.0f / nominalKelvin) +
      (logf(ntcResistance / NTC_R_NOMINAL) / NTC_BETA);

  if (inverseKelvin <= 0.0f) {
    return -999.0f;
  }

  return (1.0f / inverseKelvin) - 273.15f;
}

// -----------------------------------------------------------------------------
//  SCT-013 AC CURRENT RMS MEASUREMENT
// -----------------------------------------------------------------------------

#define SCT_VOLTS_PER_AMP    0.05f  // 20A/1V clamp -- MUST confirm against actual clamp datasheet
#define SCT_SAMPLE_WINDOW_MS 200UL
#define SCT_MIDPOINT_ALPHA   0.01f

/**
 * Samples a DC-biased SCT-013 waveform and estimates its RMS current.
 *
 * This function is blocking for approximately 200ms and must only be called
 * from a context where that is acceptable, such as a periodic check in loop().
 * It must not be called from an interrupt. Called at most every
 * DHW_CURRENT_CHECK_MS (10s), not every safety-loop tick, so this blocking
 * window never delays the overtemp safety check by more than ~200ms.
 */
float sctReadAmps(uint8_t channel) {
  if (channel > 3) {
    return 0.0f;
  }

  uint32_t numSamples = 0;
  float midpoint = 0.0f;
  double sumOfSquares = 0.0;
  const uint32_t startTime = millis();

  while ((uint32_t)(millis() - startTime) < SCT_SAMPLE_WINDOW_MS) {
    const float sample = ads1115ReadVoltage(channel);

    if (numSamples == 0) {
      midpoint = sample;
    } else {
      midpoint += (sample - midpoint) * SCT_MIDPOINT_ALPHA;
    }

    const float centeredSample = sample - midpoint;
    sumOfSquares +=
        (double)centeredSample * (double)centeredSample;
    ++numSamples;
  }

  if (numSamples == 0 || SCT_VOLTS_PER_AMP <= 0.0f) {
    return 0.0f;
  }

  const float rmsVoltage =
      sqrtf((float)(sumOfSquares / (double)numSamples));

  return rmsVoltage / SCT_VOLTS_PER_AMP;
}

// -----------------------------------------------------------------------------
//  NVS — SETTINGS PERSISTENCE
// -----------------------------------------------------------------------------
void loadSettingsFromNVS() {
  prefs.begin(NVS_NAMESPACE, true);
  settingLedBrightness = prefs.getUChar( NVS_KEY_LED_BRIGHT, DEFAULT_LED_BRIGHTNESS);
  settingVoltThreshold = prefs.getFloat( NVS_KEY_VOLT_THR,   DEFAULT_VOLT_THRESHOLD);
  fcmToken  = prefs.getString(NVS_KEY_FCM_TOKEN,  fcmToken.c_str());
  workerUrl = prefs.getString(NVS_KEY_WORKER_URL, workerUrl.c_str());
  workerSec = prefs.getString(NVS_KEY_WORKER_SEC, workerSec.c_str());
  settingNotifyOTA     = prefs.getBool(  NVS_KEY_NTFY_OTA,    DEFAULT_NOTIF_OTA);
  settingNotifyDHW     = prefs.getBool(  NVS_KEY_NTFY_DHW,    DEFAULT_NOTIF_DHW);
  hubUrl = prefs.getString(NVS_KEY_HUB_URL, hubUrl.c_str());
  hubKey = prefs.getString(NVS_KEY_HUB_KEY, hubKey.c_str());
  prefs.end();
}

void saveSettingsToNVS() {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putUChar( NVS_KEY_LED_BRIGHT, settingLedBrightness);
  prefs.putFloat( NVS_KEY_VOLT_THR,   settingVoltThreshold);
  prefs.putString(NVS_KEY_FCM_TOKEN,  fcmToken);
  prefs.putString(NVS_KEY_WORKER_URL, workerUrl);
  prefs.putString(NVS_KEY_WORKER_SEC, workerSec);
  prefs.putBool(  NVS_KEY_NTFY_OTA,    settingNotifyOTA);
  prefs.putBool(  NVS_KEY_NTFY_DHW,    settingNotifyDHW);
  prefs.putString(NVS_KEY_HUB_URL, hubUrl);
  prefs.putString(NVS_KEY_HUB_KEY, hubKey);
  prefs.end();
}

// -----------------------------------------------------------------------------
//  STATE SYNC — push all current state to the RainMaker cloud / app.
// -----------------------------------------------------------------------------
void syncStateToCloud() {
  kragwagDev.updateAndReportParam(PN_FW_BUILD,     FIRMWARE_BUILD);
  kragwagDev.updateAndReportParam(PN_LED_BRIGHT,   (int)settingLedBrightness);
  kragwagDev.updateAndReportParam(PN_VOLT_THRESH,  settingVoltThreshold);
  kragwagDev.updateAndReportParam(PN_NOTIF_OTA,    settingNotifyOTA);
  kragwagDev.updateAndReportParam(PN_NOTIF_DHW,    settingNotifyDHW);
  kragwagDev.updateAndReportParam(PN_HUB_URL,      hubUrl.c_str());
  kragwagDev.updateAndReportParam(PN_WIFI_SIGNAL, (int)WiFi.RSSI());
  kragwagDev.updateAndReportParam(PN_WIFI_LIVE,   wifiLiveMode);
  kragwagDev.updateAndReportParam(PN_LOCAL_IP,    WiFi.localIP().toString().c_str());
  { float v = read12V(); kragwagDev.updateAndReportParam(PN_VOLTAGE, v); }

  elementTop.dev->updateAndReportParam(PN_EL_POWER, elementTop.desiredOn);
  elementBottom.dev->updateAndReportParam(PN_EL_POWER, elementBottom.desiredOn);
}

// -----------------------------------------------------------------------------
//  RESTORE DEFAULTS
// -----------------------------------------------------------------------------
void restoreDefaultSettings() {
  settingLedBrightness = DEFAULT_LED_BRIGHTNESS;
  settingVoltThreshold = DEFAULT_VOLT_THRESHOLD;
  settingNotifyOTA     = DEFAULT_NOTIF_OTA;
  settingNotifyDHW     = DEFAULT_NOTIF_DHW;
  hubUrl = DEFAULT_HUB_URL;
  saveSettingsToNVS();
  kragwagDev.updateAndReportParam(PN_LED_BRIGHT,  (int)settingLedBrightness);
  kragwagDev.updateAndReportParam(PN_VOLT_THRESH, settingVoltThreshold);
  kragwagDev.updateAndReportParam(PN_NOTIF_OTA,   settingNotifyOTA);
  kragwagDev.updateAndReportParam(PN_NOTIF_DHW,   settingNotifyDHW);
  kragwagDev.updateAndReportParam(PN_HUB_URL,     hubUrl.c_str());
}

// -----------------------------------------------------------------------------
//  ADC — 12V SUPPLY MONITOR (unchanged from the alarm firmware)
// -----------------------------------------------------------------------------
float read12V() {
  long sum = 0;
  for (int i = 0; i < ADC_SAMPLES; i++) {
    sum += analogRead(PIN_ADC_12V);
    delayMicroseconds(100);
  }
  return (sum / (float)ADC_SAMPLES) / 4095.0f * 3.3f * ADC_DIVIDER_RATIO * ADC_CAL_FACTOR;
}

// -----------------------------------------------------------------------------
//  FCM PUSH NOTIFICATION  (unchanged mechanism from the alarm firmware)
// -----------------------------------------------------------------------------
void heartbeatTask(void* pv) {
  uint32_t freeHeap = ESP.getFreeHeap();
  Serial.printf("[HB] heap=%u\n", freeHeap);
  if (freeHeap < 30000) {
    Serial.printf("[HB] Skipping heartbeat — heap too low (%u bytes)\n", freeHeap);
    vTaskDelete(NULL);
    return;
  }
  {
    String nodeId = WiFi.macAddress();
    nodeId.replace(":", "");
    nodeId.toUpperCase();

    String hbUrl = workerUrl;
    int pos = hbUrl.lastIndexOf('/');
    if (pos >= 0) hbUrl = hbUrl.substring(0, pos) + "/heartbeat";

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(10);
    HTTPClient http;
    http.setTimeout(10000);
    if (http.begin(client, hbUrl)) {
      http.addHeader("Content-Type", "application/json");
      String payload = "{\"secret\":\"" + workerSec + "\","
                       "\"nodeId\":\"" + nodeId + "\","
                       "\"freeHeap\":" + String(freeHeap) + "}";
      int code = http.POST(payload);
      Serial.printf("[HB] POST %s -> %d heap=%u\n", hbUrl.c_str(), code, ESP.getFreeHeap());
      http.end();
    }
    client.stop();
  }
  vTaskDelete(NULL);
}

void sendHeartbeat() {
  if (!WiFi.isConnected()) return;
  if (workerUrl.length() == 0) return;
  xTaskCreate(heartbeatTask, "hb", 8192, NULL, 1, NULL);
}

void sendFCMTask(void* pv) {
  {
    String* args  = (String*)pv;
    String url    = args[0];
    String secret = args[1];
    String nodeId = args[2];
    String title  = args[3];
    String body   = args[4];
    delete[] args;

    if (url.length() > 0) {
      WiFiClientSecure client;
      client.setInsecure();
      HTTPClient http;
      if (http.begin(client, url)) {
        http.addHeader("Content-Type", "application/json");
        Serial.printf("[FCM] POST to %s nodeId=%s heap=%u\n", url.c_str(), nodeId.c_str(), ESP.getFreeHeap());
        String payload = "{\"secret\":\"" + secret + "\","
                         "\"nodeId\":\""  + nodeId + "\","
                         "\"title\":\""   + title  + "\","
                         "\"body\":\""    + body   + "\"}";
        http.POST(payload);
        http.end();
      }
    }
  }
  vTaskDelete(NULL);
}

void sendFCM(const char* title, const char* body) {
  if (workerUrl.length() == 0) return;
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  mac.toUpperCase();
  String nodeId = mac;
  String* args = new String[5]{ workerUrl, workerSec, nodeId,
                                 String(title), String(body) };
  xTaskCreate(sendFCMTask, "fcm", 8192, args, 1, NULL);
}

// -----------------------------------------------------------------------------
//  KRAGWAG-HUB CLIENT (Raspberry Pi) — telemetry POST + command poll.
//  Plain HTTP (LAN by default via Hub URL, e.g. http://192.168.99.23:8420).
//  Non-blocking one-shot tasks, same pattern as sendFCMTask/heartbeatTask.
//  This is a convenience/economy layer only — see the safety rule at the
//  top of this file. Losing contact with the hub never turns an element on;
//  at worst it means the price-aware "on" decision is missed, which is
//  always safe to miss.
// -----------------------------------------------------------------------------
void ingestTask(void* pv) {
  {
    String* args = (String*)pv;
    String url = args[0];
    String key = args[1];
    String payload = args[2];
    delete[] args;

    if (url.length() > 0) {
      HTTPClient http;
      if (http.begin(url)) {
        http.addHeader("Content-Type", "application/json");
        if (key.length() > 0) http.addHeader("Authorization", "Bearer " + key);
        lastIngestHttp = http.POST(payload);
        http.end();
      }
    }
  }
  vTaskDelete(NULL);
}

// Commissioning helpers, defined further down with the safety logic.
bool commissioningActive();
void setCommissioning(bool on);

void sendIngest(DhwElement &el) {
  if (hubUrl.length() == 0 || !WiFi.isConnected()) return;
  String url = hubUrl + "/ingest";
  String payload = "{\"element\":\"" + String(el.name) + "\","
                    "\"power_w\":" + String(el.lastCurrentA * 230.0f, 1) + ","  // nominal mains V -- no AC voltage sensor on this board
                    "\"current_a\":" + String(el.lastCurrentA, 2) + ","
                    "\"temp_c\":" + String(el.lastTempC, 1) + ","
                    "\"test_mode\":" + String(commissioningActive() ? 1 : 0) + ","
                    "\"relay_state\":" + String(el.relayOn ? 1 : 0) + "}";
  String* args = new String[3]{ url, hubKey, payload };
  xTaskCreate(ingestTask, "ingest", 8192, args, 1, NULL);
}

// Heap-allocated once per poll, freed inside the task after use.
struct PollArgs { String url; String key; DhwElement* el; };

void pollCommandTask(void* pv) {
  {
    PollArgs* pa = (PollArgs*)pv;
    String url = pa->url;
    String key = pa->key;
    DhwElement* el = pa->el;
    delete pa;

    // Every exit below that is not a clean 200 leaves this false, and the loop
    // retries. Previously all of them were silent and cost a full interval.
    bool ok = false;

    if (url.length() > 0 && el != nullptr) {
      HTTPClient http;
      if (http.begin(url)) {
        if (key.length() > 0) http.addHeader("Authorization", "Bearer " + key);
        int code = http.GET();
        if (code == HTTP_CODE_OK) {
          ok = true;
          String body = http.getString();
          // Hand-rolled parse (no ArduinoJson dependency, matching this file's
          // existing style): response is
          //   {"commands":[{...,"action":"on"|"off"|"test_on"|"test_off",...}]}
          //
          // Match on the whole "action":"..." pair rather than a bare token.
          // Searching for "on" alone would also hit "test_on", which is exactly
          // the sort of accidental match that turns a heater on by mistake.
          // Starlette serialises with no spaces, so the pair is contiguous.
          // TWO independent decisions, not one. A commissioning command and an
          // on/off command can arrive in the same batch -- the hub's control
          // loop is perfectly capable of queueing an "on" a few seconds after
          // you tap Test mode. Scanning once and taking whichever tag appeared
          // last let that "on" swallow the "test_on" that was meant to make it
          // possible, so test mode silently never engaged. Seen on the bench.
          int modeAt = -1;
          bool modeOn = false;
          {
            int a = body.lastIndexOf("\"action\":\"test_on\"");
            int b = body.lastIndexOf("\"action\":\"test_off\"");
            if (a > modeAt) { modeAt = a; modeOn = true;  }
            if (b > modeAt) { modeAt = b; modeOn = false; }
          }

          // "\"action\":\"on\"" cannot match inside "\"action\":\"test_on\"" --
          // the colon has to be immediately followed by the opening quote --
          // so these two scans genuinely do not see each other's tags.
          int powerAt = -1;
          int powerCmd = -1;     // 1 on, 0 off
          {
            int a = body.lastIndexOf("\"action\":\"on\"");
            int b = body.lastIndexOf("\"action\":\"off\"");
            if (a > powerAt) { powerAt = a; powerCmd = 1; }
            if (b > powerAt) { powerAt = b; powerCmd = 0; }
          }

          // Mode first, so an "on" arriving alongside is evaluated against the
          // mode this same batch just asked for rather than the previous one.
          if (modeAt >= 0)   setCommissioning(modeOn);
          if (powerCmd >= 0) el->pendingHubCmd = powerCmd;
          // Neither found means nothing was queued -- pendingHubCmd stays -1
        }
        http.end();
      }
    }

    if (!ok) hubPollFailed = true;
  }
  vTaskDelete(NULL);
}

void pollHubCommand(DhwElement &el) {
  if (hubUrl.length() == 0 || !WiFi.isConnected()) return;

  // The heartbeat's TLS POST takes roughly 21 KB while it runs. Spawning an
  // 8 KB task plus an HTTPClient into what is left is what used to fail, so
  // decline early and retry rather than burn the attempt.
  if (ESP.getFreeHeap() < DHW_POLL_MIN_HEAP) { hubPollFailed = true; return; }

  String url = hubUrl + "/api/pending-commands?element=" + String(el.name);
  PollArgs* pa = new PollArgs{ url, hubKey, &el };
  if (xTaskCreate(pollCommandTask, "poll", 8192, pa, 1, NULL) != pdPASS) {
    delete pa;                 // this leaked on every failure before
    hubPollFailed = true;
  }
}

// -----------------------------------------------------------------------------
//  GITHUB OTA  (unchanged mechanism from the alarm firmware)
// -----------------------------------------------------------------------------
inline void setOtaStatus(const char *msg) {
  kragwagDev.updateAndReportParam(PN_OTA_STATUS, msg);
}

// Last OTA outcome in plain words, for the local config page. RainMaker is
// not a reliable way to see this on the DHW board -- param writes do not
// reach it -- so the board says it itself.
String otaLastResult = "not checked yet";

// A freshly OTA'd image boots once in PENDING_VERIFY. If nothing confirms it
// works, the bootloader reverts to the previous partition at the NEXT power
// cycle -- which is exactly what happened after the Build 6 update: it ran
// fine, then quietly went back to Build 5 when the board was next powered up.
// So the image must confirm itself, and only once it has actually proved it
// works. That proof is WiFi up and the local server serving, which is why
// this is called from the loop rather than from setup().
bool otaImageConfirmed = false;   // true once this image is marked valid
bool otaWasPendingVerify = false; // true if this boot started unconfirmed

void confirmRunningImage() {
  if (otaImageConfirmed) return;
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (running == NULL || esp_ota_get_state_partition(running, &state) != ESP_OK) {
    otaImageConfirmed = true;          // nothing to confirm on this build
    return;
  }
  if (state == ESP_OTA_IMG_PENDING_VERIFY) {
    otaWasPendingVerify = true;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
      Serial.println("[OTA] running image marked valid, rollback cancelled");
      otaLastResult = String("Build ") + FIRMWARE_BUILD + " confirmed, update is permanent";
    } else {
      Serial.println("[OTA] FAILED to mark image valid -- it will roll back");
      otaLastResult = "WARNING: could not confirm this image, it will roll back";
      return;                          // leave unconfirmed so we retry
    }
  }
  otaImageConfirmed = true;
}

// Minimal JSON field readers. The manifest is small, written by our own
// release step, and this file deliberately carries no JSON library.
String jsonStr(const String& src, const char* key) {
  String needle = String("\"") + key + "\"";
  int k = src.indexOf(needle);
  if (k < 0) return "";
  int colon = src.indexOf(':', k + needle.length());
  if (colon < 0) return "";
  int q1 = src.indexOf('"', colon + 1);
  if (q1 < 0) return "";
  int q2 = src.indexOf('"', q1 + 1);
  if (q2 < 0) return "";
  return src.substring(q1 + 1, q2);
}

int jsonInt(const String& src, const char* key) {
  String needle = String("\"") + key + "\"";
  int k = src.indexOf(needle);
  if (k < 0) return -1;
  int colon = src.indexOf(':', k + needle.length());
  if (colon < 0) return -1;
  return src.substring(colon + 1).toInt();
}

// Free heap needed before attempting an update. The alarm firmware used
// 70000, which this build never reaches -- it runs a local web server the
// alarm firmware does not have, and settles around 53 KB. A lower bar is
// defensible here because a FAILED update is not a broken board: the image
// is written to the inactive app partition and the bootloader only switches
// to it after Update.end() succeeds. Run out of memory halfway and the
// running firmware carries on untouched. So the cost of trying and failing
// is one wasted download, not a bricked controller.
#define OTA_MIN_HEAP        48000

// If heap is below even that, restarting genuinely helps -- but only if
// something then retries, otherwise the restart just looks like a crash and
// the update never happens. This flag survives the restart in NVS and makes
// the next boot check for updates while memory is at its freest.
#define NVS_KEY_OTA_BOOT    "ota_boot"
bool otaRetryAfterBoot = false;   // set during setup() from that flag

void runOTA() {
  // The web server is idle almost all the time and its buffers are worth
  // more as OTA headroom. It comes back on the next boot either way.
  if (localServerStarted) {
    localServer.stop();
    localServerStarted = false;
  }

  if (ESP.getFreeHeap() < OTA_MIN_HEAP) {
    if (otaRetryAfterBoot) {
      // Already restarted once for this. Restarting again would be a loop.
      String msg = String("Not enough memory even after a restart (")
                 + ESP.getFreeHeap() + " bytes free)";
      Serial.printf("[OTA] %s\n", msg.c_str());
      setOtaStatus(msg.c_str());
      otaLastResult = msg;
      if (settingNotifyOTA) esp_rmaker_raise_alert(("OTA: " + msg).c_str());
      return;
    }
    Serial.printf("[OTA] Heap too low (%u bytes), restarting to retry\n", ESP.getFreeHeap());
    setOtaStatus((String("Restarting to free memory (heap=") + ESP.getFreeHeap() + ")").c_str());
    otaLastResult = "restarting to free memory, will retry automatically";
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putBool(NVS_KEY_OTA_BOOT, true);
    prefs.end();
    if (settingNotifyOTA)
      esp_rmaker_raise_alert((String("OTA: heap=") + ESP.getFreeHeap() + " - restarting to retry").c_str());
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
  }
  WiFiClientSecure client;
  client.setInsecure();

  setOtaStatus("Checking for updates...");
  HTTPClient http;
  http.begin(client, OTA_MANIFEST_URL);
  http.addHeader("User-Agent", "KragWag-OTA/" FIRMWARE_VERSION " (" OTA_VARIANT ")");
  int code = http.GET();

  if (code != HTTP_CODE_OK) {
    String errMsg = String("Update check failed (HTTP ") + code + ")";
    setOtaStatus(errMsg.c_str());
    otaLastResult = errMsg;
    if (settingNotifyOTA)
      esp_rmaker_raise_alert(
          (String("OTA: Version check failed HTTP ") + code
           + " heap=" + ESP.getFreeHeap()
           + " wifi=" + WiFi.status()).c_str());
    http.end();
    return;
  }

  String body = http.getString();
  body.trim();
  http.end();

  // Hand-rolled parse, matching this file's existing style (no ArduinoJson).
  // The manifest is small and written by our own release step:
  //   {"variant":"kragwag-dhw","build":6,"url":"https://...","md5":"..."}
  String variant  = jsonStr(body, "variant");
  String imageUrl = jsonStr(body, "url");
  String imageMd5 = jsonStr(body, "md5");
  int remoteBuild = jsonInt(body, "build");

  // THE GUARD. If this is not our variant, stop -- do not flash, do not
  // "try anyway". Wrong channel, wrong file, or an HTML error page all land
  // here, and all of them are reasons to refuse rather than reasons to hope.
  if (variant != OTA_VARIANT) {
    String msg = String("Refused: manifest is for '") + variant
               + "', not '" + OTA_VARIANT + "'";
    setOtaStatus(msg.c_str());
    otaLastResult = msg;
    if (settingNotifyOTA) esp_rmaker_raise_alert(("OTA: " + msg).c_str());
    return;
  }

  if (imageUrl.length() == 0 || remoteBuild <= 0) {
    setOtaStatus("Refused: manifest is malformed");
    otaLastResult = "Refused: manifest is malformed";
    return;
  }

  if (remoteBuild <= FIRMWARE_BUILD) {
    setOtaStatus((String("Already on latest build (") + FIRMWARE_BUILD + ")").c_str());
    otaLastResult = String("Already on the latest build (") + FIRMWARE_BUILD + ")";
    if (settingNotifyOTA)
      esp_rmaker_raise_alert(
          (String("OTA: Already on latest build (") + FIRMWARE_BUILD + ")").c_str());
    return;
  }

  setOtaStatus((String("Downloading build ") + remoteBuild + "...").c_str());
  if (settingNotifyOTA)
    esp_rmaker_raise_alert(
        (String("OTA: Update available  - downloading build ") + remoteBuild + "...").c_str());
  delay(2000);

  http.begin(client, imageUrl);
  http.addHeader("User-Agent", "KragWag-OTA/" FIRMWARE_VERSION " (" OTA_VARIANT ")");
  int dlCode = http.GET();

  if (dlCode != HTTP_CODE_OK) {
    setOtaStatus((String("Download failed (HTTP ") + dlCode + ")").c_str());
    otaLastResult = String("Download failed (HTTP ") + dlCode + ")";
    if (settingNotifyOTA)
      esp_rmaker_raise_alert(
          (String("OTA: Download failed HTTP ") + dlCode
           + " heap=" + ESP.getFreeHeap()).c_str());
    http.end();
    return;
  }

  int contentLen = http.getSize();
  if (contentLen <= 0) {
    otaLastResult = "Download had no length";
    if (settingNotifyOTA)
      esp_rmaker_raise_alert("OTA: No Content-Length in response");
    http.end();
    return;
  }

  setOtaStatus("Installing...");
  // A second integrity check on top of the variant guard: if the manifest
  // carries an md5, Update rejects a truncated or corrupted download rather
  // than committing a half-written image to the other app partition.
  if (imageMd5.length() == 32) Update.setMD5(imageMd5.c_str());
  if (!Update.begin(contentLen, U_FLASH)) {
    setOtaStatus("Install failed  - try again");
    otaLastResult = String("Install could not start (err ") + Update.getError() + ")";
    if (settingNotifyOTA)
      esp_rmaker_raise_alert(
          (String("OTA: Update.begin failed err=") + Update.getError()).c_str());
    http.end();
    return;
  }

  Update.writeStream(*http.getStreamPtr());
  http.end();

  if (!Update.end() || !Update.isFinished()) {
    setOtaStatus("Install failed  - try again");
    otaLastResult = String("Install failed (err ") + Update.getError()
                  + ") -- image rejected, running firmware untouched";
    if (settingNotifyOTA)
      esp_rmaker_raise_alert(
          (String("OTA: Write failed err=") + Update.getError()).c_str());
    return;
  }

  if (settingNotifyOTA)
    esp_rmaker_raise_alert("OTA: Complete - restarting");
  vTaskDelay(pdMS_TO_TICKS(1000));
  esp_restart();
}

void checkRemoteOTA() {
  runOTA();
}

// -----------------------------------------------------------------------------
//  DHW SAFETY + CONTROL LOOP  — the safety-critical core of this firmware.
//  Independent of WiFi/Pi/cloud: reads its own sensor, enforces its own
//  hard ceiling, and only ever needs an external command to turn something
//  ON — turning OFF (via lockout, or simply not being commanded on) never
//  depends on anything external.
// -----------------------------------------------------------------------------
void applyRelay(DhwElement &el, bool on) {
  if (on == el.relayOn) return;
  el.relayOn = on;
  bool pinHigh = el.relayActiveHigh ? on : !on;
  digitalWrite(el.relayPin, pinHigh ? HIGH : LOW);
}

const char* elementStatusText(const DhwElement &el) {
  if (el.safetyLockout) {
    return (el.lastValidTempMs == 0 || (millis() - el.lastValidTempMs) >= DHW_SENSOR_TIMEOUT_MS)
               ? "Safety lockout: sensor fault"
               : "Safety lockout: overtemp";
  }
  if (commissioningActive()) {
    return el.relayOn ? "TEST MODE: energised" : "TEST MODE: ready";
  }
  return el.relayOn ? "Heating" : "Idle";
}

// Non-zero while commissioning mode is live; the value is the millis() deadline.
unsigned long commissionUntilMs = 0;

bool commissioningActive() {
  if (commissionUntilMs == 0) return false;
  // Signed comparison so this still behaves correctly across the millis() wrap.
  return (long)(commissionUntilMs - millis()) > 0;
}

unsigned long commissioningSecondsLeft() {
  if (!commissioningActive()) return 0;
  return (commissionUntilMs - millis()) / 1000UL;
}

void setCommissioning(bool on) {
  bool wasOn = commissioningActive();
  if (on) {
    commissionUntilMs = millis() + DHW_COMMISSION_MS;
    if (!wasOn) {
      Serial.println("[TEST] commissioning mode ON (15 min, relays may energise)");
      esp_rmaker_raise_alert("Test mode ON: relays can energise for 15 min");
    }
  } else {
    commissionUntilMs = 0;
    if (wasOn) {
      // Drop any standing request so re-entering test mode never resumes a
      // relay the user has since forgotten about.
      elementTop.desiredOn = false;
      elementBottom.desiredOn = false;
      Serial.println("[TEST] commissioning mode OFF");
      esp_rmaker_raise_alert("Test mode ended, elements off");
    }
  }
}

void updateElementSafety(DhwElement &el) {
  unsigned long now = millis();

  // Expire commissioning mode centrally, and force both elements off as it goes.
  static bool commissionWasActive = false;
  bool commissioning = commissioningActive();
  if (commissionWasActive && !commissioning) {
    setCommissioning(false);          // clears desiredOn on both elements
    commissioning = false;
  }
  commissionWasActive = commissioning;

  // -- Read the temperature sensor for this element ---------------------------
  float vOut = ads1115ReadVoltage(el.tempAdcChannel);
  float tempC = ntcVoltageToCelsius(vOut, NTC_SUPPLY_VOLTS);
  if (tempC > -900.0f) {
    el.lastTempC = tempC;
    el.lastValidTempMs = now;
  }
  bool sensorHealthy = (el.lastValidTempMs != 0) &&
                        ((now - el.lastValidTempMs) < DHW_SENSOR_TIMEOUT_MS);

  // -- Hard safety ceiling with hysteresis reset -------------------------------
  bool wasLockedOut = el.safetyLockout;
  if (!sensorHealthy) {
    // Fail safe on a missing reading -- UNLESS commissioning mode is explicitly
    // live. This is the only branch test mode touches. Note the ordering: the
    // overtemp branch below is a separate case and is never reachable while the
    // sensor is unhealthy, so relaxing this one cannot hide a hot tank.
    el.safetyLockout = !commissioning;
  } else if (el.lastTempC >= DHW_SAFETY_MAX_C) {
    el.safetyLockout = true;                               // overtemp -- wins regardless of test mode
  } else if (el.safetyLockout && el.lastTempC <= DHW_SAFETY_RESET_C) {
    el.safetyLockout = false;                               // back in safe range -- re-arm
  }

  if (el.safetyLockout != wasLockedOut && settingNotifyDHW) {
    String msg = String(el.displayName) + ": " + elementStatusText(el);
    esp_rmaker_raise_alert(msg.c_str());
    sendFCM((String("[DHW] ") + el.displayName).c_str(), elementStatusText(el));
  }

  // -- Merge any command received from the app (RainMaker) or the hub ---------
  // Both write only `desiredOn`; the safety gate below is what actually
  // decides the relay state, so neither source can ever force an unsafe ON.
  if (el.pendingHubCmd == 1) { el.desiredOn = true; }
  else if (el.pendingHubCmd == 0) { el.desiredOn = false; }
  el.pendingHubCmd = -1;

  bool shouldBeOn = el.desiredOn && !el.safetyLockout;
  bool wasOn = el.relayOn;
  applyRelay(el, shouldBeOn);

  if (wasOn != el.relayOn) {
    el.dev->updateAndReportParam(PN_EL_STATUS, elementStatusText(el));
  }
}

void updateElementCurrent(DhwElement &el) {
  el.lastCurrentA = sctReadAmps(el.currentAdcChannel);
}

void reportElementTelemetry(DhwElement &el) {
  el.dev->updateAndReportParam(PN_EL_TEMP, el.lastTempC);
  el.dev->updateAndReportParam(PN_EL_CURRENT, el.lastCurrentA);
  el.dev->updateAndReportParam(PN_EL_STATUS, elementStatusText(el));
  sendIngest(el);
}

// -----------------------------------------------------------------------------
//  RAINMAKER — WRITE CALLBACK
// -----------------------------------------------------------------------------
static void rmWriteCallback(Device *device, Param *param,
                            const param_val_t val, void *privData,
                            write_ctx_t *ctx)
{
  const char *name = param->getParamName();

  // -- Per-element Power toggle -----------------------------------------------
  if (device == elementTop.dev && strcmp(name, PN_EL_POWER) == 0) {
    elementTop.desiredOn = val.val.b;
    param->updateAndReport(val);
    return;
  }
  if (device == elementBottom.dev && strcmp(name, PN_EL_POWER) == 0) {
    elementBottom.desiredOn = val.val.b;
    param->updateAndReport(val);
    return;
  }

  // -- Board-level settings ----------------------------------------------------
  if (strcmp(name, PN_OTA_CHECK) == 0) {
    if (val.val.b) {
      otaCheckRequested = true;
      kragwagDev.updateAndReportParam(PN_OTA_CHECK, false);
    }

  } else if (strcmp(name, PN_NOTIF_OTA) == 0) {
    settingNotifyOTA = val.val.b;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putBool(NVS_KEY_NTFY_OTA, settingNotifyOTA);
    prefs.end();
    param->updateAndReport(val);

  } else if (strcmp(name, PN_NOTIF_DHW) == 0) {
    settingNotifyDHW = val.val.b;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putBool(NVS_KEY_NTFY_DHW, settingNotifyDHW);
    prefs.end();
    param->updateAndReport(val);

  } else if (strcmp(name, PN_LED_BRIGHT) == 0) {
    int v = val.val.i;
    if (v < 0 || v > 255) return;
    settingLedBrightness = (uint8_t)v;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUChar(NVS_KEY_LED_BRIGHT, settingLedBrightness);
    prefs.end();
    param->updateAndReport(val);

  } else if (strcmp(name, PN_VOLT_THRESH) == 0) {
    float v = val.val.f;
    if (v < 8.0f || v > 14.0f) return;
    settingVoltThreshold = v;
    lowVoltAlerted = false;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putFloat(NVS_KEY_VOLT_THR, settingVoltThreshold);
    prefs.end();
    param->updateAndReport(val);

  } else if (strcmp(name, PN_HUB_URL) == 0) {
    hubUrl = String(val.val.s);
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putString(NVS_KEY_HUB_URL, hubUrl);
    prefs.end();
    param->updateAndReport(val);

  } else if (strcmp(name, PN_HUB_KEY) == 0) {
    hubKey = String(val.val.s);
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putString(NVS_KEY_HUB_KEY, hubKey);
    prefs.end();
    param->updateAndReport(val);

  } else if (strcmp(name, PN_WIFI_LIVE) == 0) {
    wifiLiveMode = val.val.b;
    lastWifiCheck = 0;
    param->updateAndReport(val);

  } else if (strcmp(name, PN_RESTORE) == 0) {
    if (val.val.b) {
      restoreDefaultSettings();
      kragwagDev.updateAndReportParam(PN_RESTORE, false);
    }
  }
}

// -----------------------------------------------------------------------------
//  SYSTEM / PROVISIONING EVENT HANDLER  (unchanged from the alarm firmware)
// -----------------------------------------------------------------------------
void sysProvEvent(arduino_event_t *sys_event) {
  switch (sys_event->event_id) {

    case ARDUINO_EVENT_PROV_START:
      Serial.printf("\nProvisioning started - name: \"%s\", PoP: \"%s\"\n",
                    PROV_SERVICE_NAME, PROV_POP);
      WiFiProv.printQR(PROV_SERVICE_NAME, PROV_POP, "ble");
      break;

    case ARDUINO_EVENT_PROV_CRED_SUCCESS:
      Serial.println("Provisioning credentials accepted");
      break;

    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      provDeinitNeeded = true;
      cloudConnected   = true;
      syncNeeded       = true;
      syncAt           = millis() + 3000;
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      cloudConnected = false;
      localServerStarted = false;
      break;

    default: break;
  }
}

// -----------------------------------------------------------------------------
//  TIMER TASKS
// -----------------------------------------------------------------------------
void checkVoltage() {
  Serial.printf("[V] heap=%u uptime=%lus\n", ESP.getFreeHeap(), millis()/1000);
  float v = read12V();
  kragwagDev.updateAndReportParam(PN_VOLTAGE, v);
  if (v < settingVoltThreshold && !lowVoltAlerted) {
    esp_rmaker_raise_alert(
        (String("Low supply voltage: ") + String(v, 1) + "V").c_str());
    sendFCM("[WARN] KragWag", (String("Low voltage: ") + String(v, 1) + "V").c_str());
    lowVoltAlerted = true;
  } else if (v >= settingVoltThreshold) {
    lowVoltAlerted = false;
  }
}

// -----------------------------------------------------------------------------
//  Registers the four params shared by both DHW element devices, then wires
//  up the write callback and primary param. Called once per element from
//  setup() -- kept as an explicit helper (rather than a loop over an
//  initializer_list of pointers) to keep the RainMaker device/param setup
//  as close as possible to the original file's verbose-but-explicit style.
// -----------------------------------------------------------------------------
void setupElementParams(DhwElement &el) {
  Device* d = el.dev;
  {
    Param p(PN_EL_POWER, "esp.param.power", value(false),
            PROP_FLAG_READ | PROP_FLAG_WRITE);
    p.addUIType("esp.ui.toggle");
    d->addParam(p);
  }
  {
    Param p(PN_EL_TEMP, "esp.param.temperature", value(-999.0f), PROP_FLAG_READ);
    d->addParam(p);
  }
  {
    // Reused "esp.param.mode" the same way PN_FW_BUILD/PN_WIFI_SIGNAL do --
    // there's no dedicated "current" UI type in this RainMaker version.
    Param p(PN_EL_CURRENT, "esp.param.mode", value(0.0f), PROP_FLAG_READ);
    d->addParam(p);
  }
  {
    Param p(PN_EL_STATUS, "esp.param.mode", value("Idle"), PROP_FLAG_READ);
    d->addParam(p);
  }
  d->addCb(rmWriteCallback);
  d->assignPrimaryParam(d->getParamByName(PN_EL_POWER));
}

// -----------------------------------------------------------------------------
//  SETUP
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
//  LOCAL CONFIG PAGE  (http://<board-ip>:8080/)
//
//  The Hub URL and Hub API Key have to be settable without the internet. They
//  are what lets this board talk to the Pi on the same LAN, so making them
//  depend on a round trip through a cloud service is backwards -- and in
//  practice RainMaker param writes did not reach this board at all, which is
//  why this page exists.
//
//  Deliberately not authenticated: it is reachable only from the LAN, and it
//  never displays the key it holds -- only whether one is set. Anyone already
//  on this network can reach the Pi directly anyway, so a password here would
//  be theatre rather than a boundary.
// -----------------------------------------------------------------------------
static const char CONFIG_PAGE[] PROGMEM = R"KWCFG(<!doctype html>
<html lang="en-GB">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>KragWag Setup</title>
<style>
*{box-sizing:border-box}html{color-scheme:dark}body{margin:0;min-height:100vh;display:grid;place-items:center;padding:24px;background:#181a1b;color:#f4f5f5;font-family:system-ui,-apple-system,"Segoe UI",sans-serif}.card{width:100%;max-width:420px;padding:30px;border:1px solid #3b4042;border-radius:18px;background:#242729;box-shadow:0 18px 45px #0006}h1{margin:0 0 24px;font-size:1.75rem;line-height:1.2;letter-spacing:-.02em}.status{display:grid;gap:1px;margin:0 0 26px;border:1px solid #3b4042;border-radius:12px;overflow:hidden;background:#3b4042}.row{padding:12px 14px;background:#2b2f31}.label,label{display:block;margin:0 0 6px;color:#aeb6b9;font-size:.78rem;font-weight:700;letter-spacing:.06em;text-transform:uppercase}.value{display:block;color:#fff;font-size:.95rem;line-height:1.35;overflow-wrap:anywhere}label{margin:0 0 8px}.field{margin:0 0 18px}input,button{width:100%;min-height:48px;border-radius:10px;font:inherit}input{padding:11px 13px;border:1px solid #555d60;background:#191b1c;color:#fff;outline:0}input:focus{border-color:#6dc8af;box-shadow:0 0 0 3px #6dc8af33}button{margin-top:6px;border:0;background:#6dc8af;color:#10251f;font-weight:750;cursor:pointer}button:hover{background:#82d4bd}button:focus-visible{outline:3px solid #baf3e3;outline-offset:3px}button:active{transform:translateY(1px)}button.secondary{background:#2b2f31;color:#dfe4e6;border:1px solid #555d60}button.secondary:hover{background:#343a3c}
</style>
</head>
<body>
<main class="card">
<h1>KragWag Setup</h1>
<section class="status" aria-label="Current status">
<div class="row"><span class="label">Hub URL</span><span class="value">{{HUBURL}}</span></div>
<div class="row"><span class="label">API key</span><span class="value">{{KEYSTATE}}</span></div>
<div class="row"><span class="label">Last hub reply</span><span class="value">{{HUBREPLY}}</span></div>
<div class="row"><span class="label">Firmware build</span><span class="value">{{BUILD}}</span></div>
<div class="row"><span class="label">Free memory</span><span class="value">{{HEAP}}</span></div>
<div class="row"><span class="label">Last update check</span><span class="value">{{OTARESULT}}</span></div>
</section>
<form action="/config" method="post">
<div class="field"><label for="huburl">Hub URL</label><input id="huburl" name="huburl" type="text" value="{{HUBURL}}"></div>
<div class="field"><label for="hubkey">Hub API Key</label><input id="hubkey" name="hubkey" type="text" autocomplete="off" spellcheck="false"></div>
<button type="submit">Save</button>
</form>
<form action="/ota" method="post" style="margin-top:14px">
<button type="submit" class="secondary">Check for firmware updates</button>
</form>
</main>
</body>
</html>
)KWCFG";

// Human-readable form of lastIngestHttp for the status card.
String hubReplyText() {
  int c = lastIngestHttp;
  if (c == 0)   return "nothing sent yet";
  if (c == 200) return "200 accepted";
  if (c == 401) return "401 rejected - key wrong or missing";
  if (c <  0)   return String("could not reach hub (") + c + ")";
  return String(c) + " unexpected";
}

void handleConfigPage() {
  String page = FPSTR(CONFIG_PAGE);
  page.replace("{{HUBURL}}",   hubUrl.length() ? hubUrl : String("(not set)"));
  // Never render the key itself -- length alone is enough to tell a typo from
  // an empty field, without putting the secret on a screen or in a cache.
  page.replace("{{KEYSTATE}}", hubKey.length()
                                 ? String("set (") + hubKey.length() + " characters)"
                                 : String("not set"));
  page.replace("{{HUBREPLY}}", hubReplyText());
  page.replace("{{BUILD}}",    String(FIRMWARE_BUILD));
  // OTA needs roughly 70 KB free to run, so this is not idle trivia.
  page.replace("{{HEAP}}",     String(ESP.getFreeHeap() / 1024) + " KB"
                               + (ESP.getFreeHeap() < 70000 ? " (too low for an update)" : ""));
  page.replace("{{OTARESULT}}", otaLastResult);
  localServer.send(200, "text/html", page);
}

void handleOtaRequest() {
  // Run the check from the main loop rather than inside this handler: OTA
  // reboots the board on success, and doing that with an HTTP response
  // half-written leaves the browser hanging on a dead socket.
  otaCheckRequested = true;
  otaLastResult = "checking now, reload in a moment...";
  localServer.sendHeader("Location", "/");
  localServer.send(303, "text/plain", "Checking");
}

void handleConfigSave() {
  bool changed = false;

  if (localServer.hasArg("huburl")) {
    String v = localServer.arg("huburl");
    v.trim();
    if (v.length() > 0 && v != hubUrl) { hubUrl = v; changed = true; }
  }

  // An empty key field means "leave it alone", so reloading the page and
  // pressing Save does not silently wipe a working key.
  if (localServer.hasArg("hubkey")) {
    String v = localServer.arg("hubkey");
    v.trim();
    if (v.length() > 0 && v != hubKey) { hubKey = v; changed = true; }
  }

  if (changed) {
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putString(NVS_KEY_HUB_URL, hubUrl);
    prefs.putString(NVS_KEY_HUB_KEY, hubKey);
    prefs.end();
    kragwagDev.updateAndReportParam(PN_HUB_URL, hubUrl.c_str());
    lastIngestHttp = 0;                 // forget the old verdict; re-prove it
    Serial.printf("[CFG] saved via local page (url=%s, key len=%u)\n",
                  hubUrl.c_str(), (unsigned)hubKey.length());
  }

  localServer.sendHeader("Location", "/");
  localServer.send(303, "text/plain", "Saved");
}

void handleRestart() {
  localServer.sendHeader("Location", "/");
  localServer.send(303, "text/plain", "Restarting");
  delay(300);                 // let the response actually leave
  esp_restart();
}

void handleStatusJson() {
  String json = String("{\"build\":") + FIRMWARE_BUILD
              + ",\"hub_url\":\"" + hubUrl + "\""
              + ",\"key_len\":" + hubKey.length()
              + ",\"last_hub_http\":" + lastIngestHttp
              + ",\"test_mode\":" + (commissioningActive() ? 1 : 0)
              + ",\"free_heap\":" + ESP.getFreeHeap()
              + ",\"ota_variant\":\"" OTA_VARIANT "\""
              + ",\"ota_last\":\"" + otaLastResult + "\""
              + ",\"image_confirmed\":" + (otaImageConfirmed ? 1 : 0)
              + ",\"was_pending_verify\":" + (otaWasPendingVerify ? 1 : 0)
              + ",\"ip\":\"" + WiFi.localIP().toString() + "\"}";
  localServer.sendHeader("Access-Control-Allow-Origin", "*");
  localServer.send(200, "application/json", json);
}

void setup() {
  Serial.begin(115200);

  // -- Pin modes --
  pinMode(PIN_IN1,      OUTPUT);
  pinMode(PIN_IO6,      OUTPUT);
  pinMode(PIN_SIREN,    INPUT);   // spare, unused
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);

  // -- Safe idle states: BOTH relays OFF before anything else runs --
  digitalWrite(PIN_IN1, LOW);   // Top:    active-HIGH -> LOW = off
  digitalWrite(PIN_IO6, HIGH);  // Bottom: active-LOW  -> HIGH = off

  // -- ADC --
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  // -- I2C / ADS1115 --
  ads1115Init();

  // -- PWM for LEDs --
  ledcAttach(PIN_LED_GREEN, LED_PWM_FREQ, LED_PWM_BITS);
  ledcAttach(PIN_LED_WHITE, LED_PWM_FREQ, LED_PWM_BITS);
  ledOff(PIN_LED_GREEN);
  ledOff(PIN_LED_WHITE);

  // -- Load settings from NVS --
  loadSettingsFromNVS();

  // Did the last run restart itself specifically to free memory for an
  // update? Clear the flag first, so a crash loop cannot be sustained by it,
  // then arrange to check again once WiFi is up.
  prefs.begin(NVS_NAMESPACE, false);
  otaRetryAfterBoot = prefs.getBool(NVS_KEY_OTA_BOOT, false);
  if (otaRetryAfterBoot) prefs.putBool(NVS_KEY_OTA_BOOT, false);
  prefs.end();

  // -- RainMaker — board-level device --------------------------------------
  {
    Param p(PN_VOLTAGE, "esp.param.temperature", value(0.0f), PROP_FLAG_READ);
    kragwagDev.addParam(p);
  }
  {
    Param p(PN_FW_BUILD, "esp.param.mode", value(FIRMWARE_BUILD), PROP_FLAG_READ);
    kragwagDev.addParam(p);
  }
  {
    Param p(PN_OTA_CHECK, "esp.param.power", value(false),
            PROP_FLAG_READ | PROP_FLAG_WRITE);
    p.addUIType("esp.ui.push-btn-big");
    kragwagDev.addParam(p);
  }
  {
    Param p(PN_OTA_STATUS, "esp.param.mode", value("Tap Check OTA to update"), PROP_FLAG_READ);
    kragwagDev.addParam(p);
  }
  {
    Param p(PN_WIFI_SIGNAL, "esp.param.mode", value(0), PROP_FLAG_READ);
    kragwagDev.addParam(p);
  }
  {
    Param p(PN_WIFI_LIVE, "esp.param.power", value(false), PROP_FLAG_READ | PROP_FLAG_WRITE);
    p.addUIType("esp.ui.toggle");
    kragwagDev.addParam(p);
  }
  {
    Param p(PN_LOCAL_IP, "esp.param.mode", value("--"), PROP_FLAG_READ);
    kragwagDev.addParam(p);
  }
  {
    Param p(PN_LED_BRIGHT, "esp.param.brightness",
            value((int)settingLedBrightness), PROP_FLAG_READ | PROP_FLAG_WRITE);
    p.addUIType("esp.ui.slider");
    p.addBounds(value(0), value(255), value(1));
    kragwagDev.addParam(p);
  }
  {
    Param p(PN_VOLT_THRESH, "esp.param.setpoint-temperature",
            value(settingVoltThreshold), PROP_FLAG_READ | PROP_FLAG_WRITE);
    p.addUIType("esp.ui.slider");
    p.addBounds(value(8.0f), value(14.0f), value(0.1f));
    kragwagDev.addParam(p);
  }
  {
    Param p(PN_FCM_TOKEN, "esp.param.name", value(""), PROP_FLAG_READ);
    kragwagDev.addParam(p);
  }
  {
    Param p(PN_WORKER_URL, "esp.param.name", value(""), PROP_FLAG_READ);
    kragwagDev.addParam(p);
  }
  {
    Param p(PN_WORKER_SEC, "esp.param.name", value(""), PROP_FLAG_READ);
    kragwagDev.addParam(p);
  }
  {
    Param p(PN_NOTIF_OTA, "esp.param.power",
            value(settingNotifyOTA), PROP_FLAG_READ | PROP_FLAG_WRITE);
    p.addUIType("esp.ui.toggle");
    kragwagDev.addParam(p);
  }
  {
    Param p(PN_NOTIF_DHW, "esp.param.power",
            value(settingNotifyDHW), PROP_FLAG_READ | PROP_FLAG_WRITE);
    p.addUIType("esp.ui.toggle");
    kragwagDev.addParam(p);
  }
  {
    // Read/write string -- set once from the app after flashing. Never
    // hardcoded: this repo is public (raw.githubusercontent.com OTA).
    Param p(PN_HUB_URL, "esp.param.name", value(hubUrl.c_str()), PROP_FLAG_READ | PROP_FLAG_WRITE);
    kragwagDev.addParam(p);
  }
  {
    Param p(PN_HUB_KEY, "esp.param.name", value(""), PROP_FLAG_READ | PROP_FLAG_WRITE);
    kragwagDev.addParam(p);
  }
  {
    Param p(PN_RESTORE, "esp.param.power", value(false),
            PROP_FLAG_READ | PROP_FLAG_WRITE);
    p.addUIType("esp.ui.push-btn-big");
    kragwagDev.addParam(p);
  }
  kragwagDev.addCb(rmWriteCallback);

  // -- RainMaker — per-element Switch devices ------------------------------
  static Device topDevObj(elementTop.displayName, "esp.device.switch", NULL);
  static Device bottomDevObj(elementBottom.displayName, "esp.device.switch", NULL);
  elementTop.dev    = &topDevObj;
  elementBottom.dev = &bottomDevObj;

  setupElementParams(elementTop);
  setupElementParams(elementBottom);

  // -- RainMaker — init node and start --------------------------------------
  Node rmNode = RMaker.initNode("KragWag DHW", "KragWag DHW Controller");
  rmNode.addDevice(kragwagDev);
  rmNode.addDevice(*elementTop.dev);
  rmNode.addDevice(*elementBottom.dev);

  RMaker.enableSchedule();
  RMaker.start();

  localServer.on("/",       HTTP_GET,  handleConfigPage);
  localServer.on("/config", HTTP_POST, handleConfigSave);
  localServer.on("/status", HTTP_GET,  handleStatusJson);
  localServer.on("/ota",    HTTP_POST, handleOtaRequest);
  localServer.on("/restart", HTTP_POST, handleRestart);

  localServer.on("/rssi", HTTP_GET, []() {
    String json = String("{\"rssi\":") + WiFi.RSSI() + "}";
    localServer.sendHeader("Access-Control-Allow-Origin", "*");
    localServer.send(200, "application/json", json);
  });

  WiFi.onEvent(sysProvEvent);
  WiFiProv.beginProvision(
      NETWORK_PROV_SCHEME_SOFTAP,
      NETWORK_PROV_SCHEME_HANDLER_NONE,
      NETWORK_PROV_SECURITY_1,
      PROV_POP,
      PROV_SERVICE_NAME);
}

// -----------------------------------------------------------------------------
//  MAIN LOOP
// -----------------------------------------------------------------------------
void loop() {
  unsigned long now = millis();

  // -- Provisioning teardown (deferred from WiFi-got-IP event) -------------
  if (provDeinitNeeded) {
    provDeinitNeeded = false;
    WiFiProv.endProvision();
    WiFi.softAPdisconnect(true);
  }

  // -- BOOT button — hold 5s -> full factory reset --------------------------
  if (digitalRead(PIN_BOOT_BTN) == LOW) {
    if (!bootBtnHeld) {
      bootBtnHeld       = true;
      bootBtnPressStart = now;
    } else if (now - bootBtnPressStart >= WIFI_RESET_HOLD_MS) {
      RMakerFactoryReset(2);
    }
  } else {
    bootBtnHeld = false;
  }

  // -- Deferred cloud sync (triggered by WiFi-got-IP event) ----------------
  if (syncNeeded && (now - syncAt < 0x80000000UL) && now >= syncAt) {
    syncNeeded = false;
    syncStateToCloud();
  }

  // -- DHW safety loop — every DHW_SAFETY_CHECK_MS, runs regardless of WiFi -
  if (now - lastSafetyCheck >= DHW_SAFETY_CHECK_MS) {
    lastSafetyCheck = now;
    updateElementSafety(elementTop);
    updateElementSafety(elementBottom);
  }

  // -- Current sampling — every DHW_CURRENT_CHECK_MS (blocks ~200ms each) --
  if (now - lastCurrentCheck >= DHW_CURRENT_CHECK_MS) {
    lastCurrentCheck = now;
    updateElementCurrent(elementTop);
    updateElementCurrent(elementBottom);
  }

  // -- Report telemetry to kragwag-hub + RainMaker --------------------------
  if (now - lastHubReport >= DHW_HUB_REPORT_MS) {
    lastHubReport = now;
    reportElementTelemetry(elementTop);
    reportElementTelemetry(elementBottom);
  }

  // -- Poll kragwag-hub for pending commands --------------------------------
  {
    unsigned long pollWait = hubPollFailed ? DHW_HUB_POLL_RETRY_MS
                                           : DHW_HUB_POLL_MS;
    if (now - lastHubPoll >= pollWait) {
      lastHubPoll = now;
      // Cleared before polling; the tasks set it again if they fail. They
      // finish long before the shortest wait above, so this cannot race.
      hubPollFailed = false;
      pollHubCommand(elementTop);
      pollHubCommand(elementBottom);
    }
  }

  // -- Voltage check — every 60s ---------------------------------------------
  if (now - lastVoltageCheck >= VOLTAGE_CHECK_MS) {
    lastVoltageCheck = now;
    checkVoltage();
  }

  // -- Heartbeat — every 5 min -----------------------------------------------
  if (now - lastHeartbeat >= HEARTBEAT_MS) {
    lastHeartbeat = now;
    sendHeartbeat();
  }

  // -- Local HTTP server -- handle client requests every loop ---------------
  if (WiFi.isConnected() && !localServerStarted) {
    localServer.begin();
    localServerStarted = true;
    kragwagDev.updateAndReportParam(PN_LOCAL_IP, WiFi.localIP().toString().c_str());
    // WiFi is up and we are serving: this image works. Keep it.
    confirmRunningImage();
    if (otaRetryAfterBoot) otaCheckRequested = true;   // resume the interrupted update
  }
  if (localServerStarted) localServer.handleClient();

  // -- WiFi signal strength update -------------------------------------------
  {
    unsigned long wifiInterval = wifiLiveMode ? WIFI_LIVE_MS : WIFI_NORMAL_MS;
    if (now - lastWifiCheck >= wifiInterval) {
      lastWifiCheck = now;
      if (WiFi.isConnected()) {
        kragwagDev.updateAndReportParam(PN_WIFI_SIGNAL, (int)WiFi.RSSI());
      }
    }
  }

  if (otaCheckRequested) {
    otaCheckRequested = false;
    checkRemoteOTA();
  }

  // -- LED patterns ----------------------------------------------------------
  LedPattern whitePattern;
  if (!WiFi.isConnected()) whitePattern = LED_FAST_BLINK;
  else if (!cloudConnected) whitePattern = LED_SLOW_BLINK;
  else                      whitePattern = LED_ON;

  bool anyLockout  = elementTop.safetyLockout || elementBottom.safetyLockout;
  bool anyHeating  = elementTop.relayOn || elementBottom.relayOn;
  LedPattern greenPattern;
  if (anyLockout)      greenPattern = LED_FAST_BLINK;
  else if (anyHeating) greenPattern = LED_DOUBLE_PULSE;
  else                  greenPattern = LED_SLOW_BLINK;

  runLed(PIN_LED_WHITE, whitePattern, whiteLed);
  runLed(PIN_LED_GREEN, greenPattern, greenLed);
}
