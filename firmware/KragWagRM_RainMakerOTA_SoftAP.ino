/*
 * ============================================================
 *  KragWag ï¿½ JVA Z13 WiFi Notification Board
 *  ESP32-C3-WROOM-02U  |  Arduino + ESP RainMaker
 *  Firmware v2.0.1-softap
 * ============================================================
 *
 *  HARDWARE SUMMARY  (unchanged from v1.x)
 *  ---------------------------------------------------------
 *  GPIO0   IO0 output       Gate / Garage 1  (active-LOW, 10k pullup R19)
 *  GPIO1   ADC input        12V supply monitor (ADC1_CH1, via R15/R21/R20 divider)
 *  GPIO3   IO3 output       Gate / Garage 2  (active-LOW, 10k pullup R18)
 *  GPIO4   Green LED        Status indicator  (active-HIGH, PWM)
 *  GPIO5   SIREN input      Z13 siren detect  (active-LOW, 10k pullup R6)
 *  GPIO6   IO6 output       Gate / Garage 3  (active-LOW, 10k pullup R17)
 *  GPIO7   White LED        WiFi/cloud status (active-HIGH, PWM)
 *  GPIO9   BOOT button      Hold 5 s to factory-reset (WiFi + cloud pairing)
 *  GPIO10  IN1 output       Z13 arm/disarm    (active-HIGH ? R4 ? U13 opto ? short IN1)
 *  Red LED (PWR)            Always on ï¿½ connected to 3.3V via R8, no GPIO needed
 *
 *  LED PATTERNS
 *  ---------------------------------------------------------
 *  White (WiFi):   Fast blink = provisioning/connecting  |  Slow blink = no cloud  |  Solid = online
 *  Green (Status): Fast blink = ALARM  |  Double pulse = ARMED  |  Slow blink = DISARMED or transitioning
 *  Both LEDs respect the brightness setting ï¿½ except during OTA which always uses full brightness.
 *
 *  FIRST-TIME SETUP  (Wi-Fi + cloud provisioning)
 *  ---------------------------------------------------------
 *  1. Install the ESP RainMaker app on your phone (iOS / Android ï¿½ search "ESP RainMaker")
 *  2. Power the board ï¿½ white LED fast-blinks, temporary WiFi AP appears as "PROV_KragWag"
 *  3. Open the RainMaker app ? Add Device ? scan the QR code printed to Serial
 *     OR manually: service name "PROV_KragWag", proof-of-possession "kragwag1"
 *  4. Follow app prompts to enter your WiFi credentials
 *  5. Board connects ï¿½ white LED goes solid, KragWag appears in the app
 *
 *  TO FACTORY-RESET  (wipe WiFi credentials + cloud pairing)
 *  ---------------------------------------------------------
 *  Hold BOOT button (GPIO9) for 5 seconds ? full factory reset ? reboots into provisioning mode.
 *  WiFi credentials and RainMaker cloud pairing are cleared. Your saved settings
 *  (brightness, thresholds, pulse durations, etc.) are preserved ï¿½ use "Restore
 *  Defaults" in the app to reset those separately.
 *
 *  RAINMAKER PARAMETERS
 *  ---------------------------------------------------------
 *  -- Main --
 *  "Arm / Disarm"      Toggle      Arm or disarm the energiser
 *  "Siren Active"      Indicator   True when siren input is detected
 *  "Gate 1"            Button      Pulse Gate/Garage output 1
 *  "Gate 2"            Button      Pulse Gate/Garage output 2
 *  "Gate 3"            Button      Pulse Gate/Garage output 3
 *  "Supply Voltage"    Value       12V rail reading in volts
 *  "Armed Status"      Text        "Disarmed" / "Pending..." / "Armed"
 *  "Firmware Build"    Value       Build integer (incremented each OTA release)
 *  -- Settings --
 *  "LED Brightness"    Slider 0ï¿½255
 *  "Low Volt Alert"    Slider 8.0ï¿½14.0 V
 *  "Notify Siren"      Toggle
 *  "Notify Gate"       Toggle
 *  "Gate Pulse ms"     Slider 100ï¿½2000
 *  "Arm Pulse ms"      Slider 100ï¿½2000
 *  "Restore Defaults"  Button
 *
 *  PUSH ALERTS  (enable in the RainMaker app under Alerts)
 *  ---------------------------------------------------------
 *  Siren triggered, Low supply voltage, Gate/garage triggered, OTA status
 *
 *  OTA UPDATE WORKFLOW
 *  ---------------------------------------------------------
 *  OTA is handled through ESP RainMaker. Build/export the firmware .bin and
 *  deploy it via the ESP RainMaker OTA workflow/dashboard. Local ArduinoOTA and
 *  direct GitHub self-update have intentionally been removed to fit 4 MB boards.
 *
 *  REQUIRED BOARD SUPPORT / LIBRARIES
 *  ---------------------------------------------------------
 *  - ESP32 Arduino board package 3.x recommended (includes RainMaker)
 *    Install via: Boards Manager ? "esp32" by Espressif Systems
 *  No separate Blynk or WiFiManager install needed.
 *
 *  NOTE ON LEDC (PWM)
 *  ---------------------------------------------------------
 *  This firmware uses the ESP32 Arduino 3.x LEDC API (ledcAttach / ledcWrite).
 *  If you are on board package 2.x, replace:
 *    ledcAttach(pin, freq, bits)  ?  ledcSetup(ch, freq, bits); ledcAttachPin(pin, ch);
 *    ledcWrite(pin, duty)         ?  ledcWrite(ch, duty);
 *  using channel 0 for the green LED and channel 1 for the white LED.
 * ============================================================
 */

// -- Firmware version ----------------------------------------------------------
#define FIRMWARE_VERSION  "2.0.0"
#define FIRMWARE_BUILD    46       // Increment each time you publish an OTA build.

// -- GitHub OTA ----------------------------------------------------------------
// Both URLs use raw.githubusercontent.com ï¿½ served directly without redirects.
#define OTA_VERSION_URL  "https://raw.githubusercontent.com/ScriptPilotX/kragwag/main/version.txt"
#define OTA_FIRMWARE_URL "https://raw.githubusercontent.com/ScriptPilotX/kragwag/main/kragwag.bin"

// -- RainMaker provisioning ----------------------------------------------------
// SoftAP service/AP name the phone sees during initial setup; POP is the pairing code.
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

// -- Pin definitions -----------------------------------------------------------
#define PIN_IO0         0
#define PIN_ADC_12V     1
#define PIN_IO3         3
#define PIN_LED_GREEN   4   // PWM via LEDC
#define PIN_SIREN       5
#define PIN_IO6         6
#define PIN_LED_WHITE   7   // PWM via LEDC
#define PIN_BOOT_BTN    9
#define PIN_IN1         10

// -- RainMaker parameter name constants ---------------------------------------
// Using #defines avoids typos in updateAndReportParam() and strcmp() calls.
// Main
#define PN_ARM          "Arm / Disarm"
#define PN_SIREN_ACTIVE "Siren Active"
#define PN_GATE1        "Gate 1"
#define PN_GATE2        "Gate 2"
#define PN_GATE3        "Gate 3"
#define PN_VOLTAGE      "Supply Voltage"
#define PN_ARMED_STATUS "Armed Status"
#define PN_FW_BUILD     "Firmware Build"
#define PN_OTA_CHECK    "Check OTA"
#define PN_OTA_STATUS   "OTA Status"
#define PN_WIFI_SIGNAL  "WiFi Signal"
#define PN_WIFI_LIVE    "WiFi Live"
#define PN_LOCAL_IP     "Local IP"
// Settings
// Settings
#define PN_LED_BRIGHT   "LED Brightness"
#define PN_VOLT_THRESH  "Low Volt Alert"
#define PN_FCM_TOKEN    "FCM Token"       // set by app; used to push notifications
#define PN_WORKER_URL   "Worker URL"      // Cloudflare Worker endpoint
#define PN_WORKER_SEC   "Worker Secret"   // shared secret
#define PN_NOTIF_SIREN  "Notify Siren"
#define PN_NOTIF_GATE1  "Notify Gate 1"
#define PN_NOTIF_GATE2  "Notify Gate 2"
#define PN_NOTIF_GATE3  "Notify Gate 3"
#define PN_NOTIF_OTA    "Notify OTA"
#define PN_GATE_PULSE   "Gate Pulse ms"
#define PN_ARM_PULSE    "Arm Pulse ms"
#define PN_RESTORE      "Restore Defaults"
#define PN_PANIC        "Panic Alarm"        // latches Gate 3 (IO6) as manual siren trigger
#define PN_PANIC_ACTIVE "Panic Active"      // read-only: true while panic latch is held

// -- Default settings values ---------------------------------------------------
#define DEFAULT_LED_BRIGHTNESS  255
#define DEFAULT_VOLT_THRESHOLD  10.0f
#define DEFAULT_NOTIF_SIREN     true
#define DEFAULT_NOTIF_OTA       true
#define DEFAULT_NOTIF_GATE      true
#define DEFAULT_GATE_PULSE_MS   500
#define DEFAULT_ARM_PULSE_MS    600

// -- Timing --------------------------------------------------------------------
#define SIREN_CHECK_MS     200
#define VOLTAGE_CHECK_MS   60000
#define HEARTBEAT_MS       300000  // 5 min -- Worker cron alerts if missing >10 min
#define WORKER_HEARTBEAT_URL_SUFFIX "/heartbeat"
#define ADC_SAMPLES        8
#define ARM_CONFIRM_MS     3000
#define WIFI_RESET_HOLD_MS 5000

// -- Hardware constants --------------------------------------------------------
#define ADC_DIVIDER_RATIO  (50.0f / 11.0f)   // (R15+R21+R20) / (R21+R20)
#define ADC_CAL_FACTOR     (11.92f / 12.92f)  // measured vs reported: scale to actual voltage
#define LED_PWM_FREQ       5000               // Hz
#define LED_PWM_BITS       8                  // 0ï¿½255

// -- NVS -----------------------------------------------------------------------
#define NVS_NAMESPACE      "kragwag"
#define NVS_KEY_LED_BRIGHT "led_bright"
#define NVS_KEY_VOLT_THR   "volt_thr"
#define NVS_KEY_FCM_TOKEN  "fcm_token"
#define NVS_KEY_WORKER_URL "worker_url"
#define NVS_KEY_WORKER_SEC "worker_sec"
#define NVS_KEY_NTFY_SIREN "ntfy_siren"
#define NVS_KEY_NTFY_GATE1 "ntfy_gate1"
#define NVS_KEY_NTFY_GATE2 "ntfy_gate2"
#define NVS_KEY_NTFY_GATE3 "ntfy_gate3"
#define NVS_KEY_NTFY_OTA   "ntfy_ota"
#define NVS_KEY_GATE_PULSE "gate_pulse"
#define NVS_KEY_ARM_PULSE  "arm_pulse"
#define NVS_KEY_ARM        "armed"

// -- LED pattern types ---------------------------------------------------------
enum LedPattern { LED_OFF, LED_ON, LED_SLOW_BLINK, LED_FAST_BLINK, LED_DOUBLE_PULSE };

// -- Runtime settings (loaded from NVS on boot, updated via app) --------------
uint8_t  settingLedBrightness = DEFAULT_LED_BRIGHTNESS;
float    settingVoltThreshold = DEFAULT_VOLT_THRESHOLD;
bool     settingNotifySiren   = DEFAULT_NOTIF_SIREN;
bool     settingNotifyGate1   = DEFAULT_NOTIF_GATE;
bool     settingNotifyGate2   = DEFAULT_NOTIF_GATE;
bool     settingNotifyGate3   = DEFAULT_NOTIF_GATE;
bool     settingNotifyOTA     = DEFAULT_NOTIF_OTA;
bool     panicActive          = false;              // true while IO6 is latched LOW for panic
uint16_t settingGatePulseMs   = DEFAULT_GATE_PULSE_MS;
uint16_t settingArmPulseMs    = DEFAULT_ARM_PULSE_MS;

// -- Runtime state -------------------------------------------------------------
bool systemArmed    = false;
bool sirenActive    = false;
bool sirenWasActive = false;
bool lowVoltAlerted = false;
String   fcmToken   = "fA-RcinsTw-EVV76I-K9Dc:APA91bEzWBvRtokcf91rF5aLQ9TF-3apE4nt3fq3E8_BOm06VJKPvDljI2_Z-P-N2hJp-rzUaKDudzL5EdyGYVYA6ySfTeOi0Xe-R2VW1qHsxNeGmuriETQ";  // set via RainMaker or hardcoded
String   workerUrl  = "https://kragwag-notif.fabian-kandlinger.workers.dev/notify";
String   workerSec  = "KragWag2026Secure";
bool cloudConnected = false;

bool          armTransitioning  = false;
bool          pendingArmedState = false;
unsigned long armPendingStart   = 0;

bool io0Pulsing = false;  unsigned long io0PulseStart = 0;
bool io3Pulsing = false;  unsigned long io3PulseStart = 0;
bool io6Pulsing = false;  unsigned long io6PulseStart = 0;
bool in1Pulsing = false;  unsigned long in1PulseStart = 0;

bool          otaCheckRequested = false;
bool          wifiLiveMode      = false;
unsigned long lastWifiCheck      = 0;
#define WIFI_NORMAL_MS  60000
#define WIFI_LIVE_MS    2000
unsigned long bootBtnPressStart = 0;
bool          bootBtnHeld       = false;

unsigned long lastSirenCheck   = 0;
unsigned long lastVoltageCheck = 0;
unsigned long lastHeartbeat    = 0;

bool          syncNeeded = false;   // set by WiFi-got-IP event; cleared by loop()
bool          provDeinitNeeded = false; // set by WiFi-got-IP event; cleared by loop()
unsigned long syncAt     = 0;       // millis() target before calling syncStateToCloud()

// -- LED state -----------------------------------------------------------------
struct LedState { bool output; uint8_t phase; unsigned long lastChange; };
LedState greenLed = {false, 0, 0};
LedState whiteLed = {false, 0, 0};

// -- Global objects ------------------------------------------------------------
Preferences   prefs;
WebServer     localServer(8080);
bool          localServerStarted = false;
static Device kragwagDev("KragWag", "esp.device.other", NULL);
// Node is declared locally in setup() ï¿½ it is just a handle wrapper and does not
// need to outlive setup() once addDevice() has registered it with the framework.

// -----------------------------------------------------------------------------
//  LED HELPERS  (PWM-aware)
// -----------------------------------------------------------------------------
inline void ledOn (uint8_t pin) { ledcWrite(pin, settingLedBrightness); }
inline void ledOff(uint8_t pin) { ledcWrite(pin, 0); }

// -----------------------------------------------------------------------------
//  LED PATTERN ENGINE  (non-blocking ï¿½ unchanged from v1.x)
// -----------------------------------------------------------------------------
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
//  NVS ï¿½ SETTINGS PERSISTENCE  (unchanged from v1.x)
// -----------------------------------------------------------------------------
void loadSettingsFromNVS() {
  prefs.begin(NVS_NAMESPACE, true);
  settingLedBrightness = prefs.getUChar( NVS_KEY_LED_BRIGHT, DEFAULT_LED_BRIGHTNESS);
  settingVoltThreshold = prefs.getFloat( NVS_KEY_VOLT_THR,   DEFAULT_VOLT_THRESHOLD);
  fcmToken  = prefs.getString(NVS_KEY_FCM_TOKEN,  fcmToken.c_str());
  workerUrl = prefs.getString(NVS_KEY_WORKER_URL, workerUrl.c_str());
  workerSec = prefs.getString(NVS_KEY_WORKER_SEC, workerSec.c_str());
  settingNotifySiren   = prefs.getBool(  NVS_KEY_NTFY_SIREN,  DEFAULT_NOTIF_SIREN);
  settingNotifyGate1   = prefs.getBool(  NVS_KEY_NTFY_GATE1,  DEFAULT_NOTIF_GATE);
  settingNotifyGate2   = prefs.getBool(  NVS_KEY_NTFY_GATE2,  DEFAULT_NOTIF_GATE);
  settingNotifyGate3   = prefs.getBool(  NVS_KEY_NTFY_GATE3,  DEFAULT_NOTIF_GATE);
  settingNotifyOTA     = prefs.getBool(  NVS_KEY_NTFY_OTA,    DEFAULT_NOTIF_OTA);
  settingGatePulseMs   = prefs.getUShort(NVS_KEY_GATE_PULSE,  DEFAULT_GATE_PULSE_MS);
  settingArmPulseMs    = prefs.getUShort(NVS_KEY_ARM_PULSE,   DEFAULT_ARM_PULSE_MS);
  systemArmed          = prefs.getBool(  NVS_KEY_ARM,         false);
  prefs.end();
}

void saveSettingsToNVS() {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putUChar( NVS_KEY_LED_BRIGHT, settingLedBrightness);
  prefs.putFloat( NVS_KEY_VOLT_THR,   settingVoltThreshold);
  prefs.putString(NVS_KEY_FCM_TOKEN,  fcmToken);
  prefs.putString(NVS_KEY_WORKER_URL, workerUrl);
  prefs.putString(NVS_KEY_WORKER_SEC, workerSec);
  prefs.putBool(  NVS_KEY_NTFY_SIREN,  settingNotifySiren);
  prefs.putBool(  NVS_KEY_NTFY_GATE1,  settingNotifyGate1);
  prefs.putBool(  NVS_KEY_NTFY_GATE2,  settingNotifyGate2);
  prefs.putBool(  NVS_KEY_NTFY_GATE3,  settingNotifyGate3);
  prefs.putBool(  NVS_KEY_NTFY_OTA,    settingNotifyOTA);
  prefs.putUShort(NVS_KEY_GATE_PULSE,  settingGatePulseMs);
  prefs.putUShort(NVS_KEY_ARM_PULSE,   settingArmPulseMs);
  prefs.end();
}

// -----------------------------------------------------------------------------
//  STATE SYNC  ï¿½ push all current state to the RainMaker cloud / app.
//  Called each time the MQTT connection is established.
// -----------------------------------------------------------------------------
void syncStateToCloud() {
  const char *armedStr = armTransitioning ? "Pending..." : (systemArmed ? "Armed" : "Disarmed");
  kragwagDev.updateAndReportParam(PN_ARM,          systemArmed);
  kragwagDev.updateAndReportParam(PN_SIREN_ACTIVE, sirenActive);
  kragwagDev.updateAndReportParam(PN_ARMED_STATUS, armedStr);
  kragwagDev.updateAndReportParam(PN_FW_BUILD,     FIRMWARE_BUILD);
  kragwagDev.updateAndReportParam(PN_LED_BRIGHT,   (int)settingLedBrightness);
  kragwagDev.updateAndReportParam(PN_VOLT_THRESH,  settingVoltThreshold);
  kragwagDev.updateAndReportParam(PN_NOTIF_SIREN,  settingNotifySiren);
  kragwagDev.updateAndReportParam(PN_NOTIF_GATE1,  settingNotifyGate1);
  kragwagDev.updateAndReportParam(PN_NOTIF_GATE2,  settingNotifyGate2);
  kragwagDev.updateAndReportParam(PN_NOTIF_GATE3,  settingNotifyGate3);
  kragwagDev.updateAndReportParam(PN_NOTIF_OTA,    settingNotifyOTA);
  kragwagDev.updateAndReportParam(PN_GATE_PULSE,   (int)settingGatePulseMs);
  kragwagDev.updateAndReportParam(PN_ARM_PULSE,    (int)settingArmPulseMs);
  kragwagDev.updateAndReportParam(PN_WIFI_SIGNAL, (int)WiFi.RSSI());
  kragwagDev.updateAndReportParam(PN_WIFI_LIVE,   wifiLiveMode);
  kragwagDev.updateAndReportParam(PN_LOCAL_IP,    WiFi.localIP().toString().c_str());
  // Read and push voltage immediately so the app shows a value on connect.
  { float v = read12V(); kragwagDev.updateAndReportParam(PN_VOLTAGE, v); }
}

// -----------------------------------------------------------------------------
//  RESTORE DEFAULTS
// -----------------------------------------------------------------------------
void restoreDefaultSettings() {
  settingLedBrightness = DEFAULT_LED_BRIGHTNESS;
  settingVoltThreshold = DEFAULT_VOLT_THRESHOLD;
  settingNotifySiren   = DEFAULT_NOTIF_SIREN;
  settingNotifyGate1   = DEFAULT_NOTIF_GATE;
  settingNotifyGate2   = DEFAULT_NOTIF_GATE;
  settingNotifyGate3   = DEFAULT_NOTIF_GATE;
  settingNotifyOTA     = DEFAULT_NOTIF_OTA;
  settingGatePulseMs   = DEFAULT_GATE_PULSE_MS;
  settingArmPulseMs    = DEFAULT_ARM_PULSE_MS;
  saveSettingsToNVS();
  // Push defaults to the app so sliders/toggles snap back immediately.
  kragwagDev.updateAndReportParam(PN_LED_BRIGHT,  (int)settingLedBrightness);
  kragwagDev.updateAndReportParam(PN_VOLT_THRESH, settingVoltThreshold);
  kragwagDev.updateAndReportParam(PN_NOTIF_SIREN, settingNotifySiren);
  kragwagDev.updateAndReportParam(PN_NOTIF_GATE1, settingNotifyGate1);
  kragwagDev.updateAndReportParam(PN_NOTIF_GATE2, settingNotifyGate2);
  kragwagDev.updateAndReportParam(PN_NOTIF_GATE3, settingNotifyGate3);
  kragwagDev.updateAndReportParam(PN_NOTIF_OTA,   settingNotifyOTA);
  kragwagDev.updateAndReportParam(PN_GATE_PULSE,  (int)settingGatePulseMs);
  kragwagDev.updateAndReportParam(PN_ARM_PULSE,   (int)settingArmPulseMs);
}

// -----------------------------------------------------------------------------
//  PULSE HELPERS  (unchanged from v1.x)
// -----------------------------------------------------------------------------
void pulseIn1() {
  if (!in1Pulsing) {
    digitalWrite(PIN_IN1, HIGH);
    in1Pulsing = true;  in1PulseStart = millis();
  }
}

void pulseIO(uint8_t pin, bool &pulsing, unsigned long &startTime) {
  if (!pulsing) {
    digitalWrite(pin, LOW);
    pulsing = true;  startTime = millis();
  }
}

// -----------------------------------------------------------------------------
//  ADC ï¿½ 12V SUPPLY MONITOR  (unchanged from v1.x)
// -----------------------------------------------------------------------------
// --- FCM Push Notification ----------------------------------------------------

// One-shot heartbeat task — spawned by sendHeartbeat() every HEARTBEAT_MS.
// Dies after a single POST, exactly like sendFCMTask. This avoids having a
// persistent task consuming 8 KB of stack from boot, which interfered with
// the RainMaker MQTT TLS handshake and caused a 60-second reboot loop.
void heartbeatTask(void* pv) {
  // Guard: skip if heap is below the safe threshold for running a concurrent
  // HTTPS TLS session alongside the existing MQTT TLS session.
  // mbedTLS on ESP32-C3 (Arduino 3.x) uses fixed 16 KB I/O buffers per session
  // (CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=16384, ASYMMETRIC_CONTENT_LEN not set),
  // so each TLS context needs ~50 KB.  The threshold below ensures ≥ 50 KB of
  // headroom remains for MQTT to keep its session alive while HTTPS runs.
  uint32_t freeHeap = ESP.getFreeHeap();
  Serial.printf("[HB] heap=%u\n", freeHeap);
  if (freeHeap < 30000) {
    Serial.printf("[HB] Skipping heartbeat — heap too low (%u bytes)\n", freeHeap);
    vTaskDelete(NULL);
    return;
  }

  {
    // All String and SSL objects are inside this scope so their destructors run
    // before vTaskDelete(NULL). Without this, vTaskDelete skips C++ stack
    // unwinding, leaking the heap buffers of any String declared outside.
    String nodeId = WiFi.macAddress();
    nodeId.replace(":", "");
    nodeId.toUpperCase();

    // Derive heartbeat URL: replace /notify suffix with /heartbeat
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

// Calls the Cloudflare Worker proxy which forwards to Firebase FCM.
// Runs in a non-blocking one-shot task so it does not stall loop().
void sendFCMTask(void* pv) {
  {
    // All String and SSL objects inside this scope so destructors run before
    // vTaskDelete(NULL). vTaskDelete skips C++ stack unwinding, so any String
    // declared outside would leak its heap buffer on every FCM send.
    String* args  = (String*)pv;
    String url    = args[0];
    String secret = args[1];
    String nodeId = args[2];
    String title  = args[3];
    String body   = args[4];
    delete[] args;   // free before SSL alloc so heap is clean

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
  }  // all Strings + client + http destructors called here - heap fully released
  vTaskDelete(NULL);
}

void sendFCM(const char* title, const char* body) {
  if (workerUrl.length() == 0) return;
  // Derive node ID from MAC (e.g. "9C139E5C6668") - guaranteed to match RainMaker node
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  mac.toUpperCase();
  String nodeId = mac;
  String* args = new String[5]{ workerUrl, workerSec, nodeId,
                                 String(title), String(body) };
  xTaskCreate(sendFCMTask, "fcm", 8192, args, 1, NULL);
}
float read12V() {
  long sum = 0;
  for (int i = 0; i < ADC_SAMPLES; i++) {
    sum += analogRead(PIN_ADC_12V);
    delayMicroseconds(100);
  }
  return (sum / (float)ADC_SAMPLES) / 4095.0f * 3.3f * ADC_DIVIDER_RATIO * ADC_CAL_FACTOR;
}

// -----------------------------------------------------------------------------
//  GITHUB OTA  ï¿½ runs in its own FreeRTOS task (16 KB stack).
//  Uses ONE persistent WiFiClientSecure for BOTH the version check and the
//  binary download.  Opening a second SSL context after the first fails on
//  ESP32-C3 with "connection refused" ï¿½ reusing the same client avoids this.
// -----------------------------------------------------------------------------
inline void setOtaStatus(const char *msg) {
  kragwagDev.updateAndReportParam(PN_OTA_STATUS, msg);
}

void runOTA() {
  if (ESP.getFreeHeap() < 70000) {
    Serial.printf("[OTA] Heap too low (%u bytes), restarting\n", ESP.getFreeHeap());
    setOtaStatus((String("Restarting (heap=") + ESP.getFreeHeap() + ")").c_str());
    if (settingNotifyOTA)
      esp_rmaker_raise_alert((String("OTA: heap=") + ESP.getFreeHeap() + " - restarting").c_str());
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
  }
  WiFiClientSecure client;
  client.setInsecure();

  // Step 1: version check
  setOtaStatus("Checking for updates...");
  HTTPClient http;
  http.begin(client, OTA_VERSION_URL);
  http.addHeader("User-Agent", "KragWag-OTA/" FIRMWARE_VERSION);
  int code = http.GET();

  if (code != HTTP_CODE_OK) {
    String errMsg = String("Update failed (HTTP ") + code + ")";
    setOtaStatus(errMsg.c_str());
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
  int remoteBuild = body.toInt();

  if (remoteBuild <= FIRMWARE_BUILD) {
    http.end();
    setOtaStatus((String("Already on latest build (") + FIRMWARE_BUILD + ")").c_str());
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

  // Step 2: firmware download on the SAME SSL connection
  http.begin(client, OTA_FIRMWARE_URL);
  http.addHeader("User-Agent", "KragWag-OTA/" FIRMWARE_VERSION);
  int dlCode = http.GET();

  if (dlCode != HTTP_CODE_OK) {
    setOtaStatus((String("Download failed (HTTP ") + dlCode + ")").c_str());
    if (settingNotifyOTA)
      esp_rmaker_raise_alert(
          (String("OTA: Download failed HTTP ") + dlCode
           + " heap=" + ESP.getFreeHeap()).c_str());
    http.end();
    return;
  }

  int contentLen = http.getSize();
  if (contentLen <= 0) {
    if (settingNotifyOTA)
      esp_rmaker_raise_alert("OTA: No Content-Length in response");
    http.end();
    return;
  }

  setOtaStatus("Installing...");
  if (!Update.begin(contentLen, U_FLASH)) {
    setOtaStatus("Install failed  - try again");
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
    if (settingNotifyOTA)
      esp_rmaker_raise_alert(
          (String("OTA: Write failed err=") + Update.getError()).c_str());
    return;
  }

  if (settingNotifyOTA)
    esp_rmaker_raise_alert("OTA: Complete ï¿½ restarting");
  vTaskDelay(pdMS_TO_TICKS(1000));
  esp_restart();

}

void checkRemoteOTA() {
  runOTA();
}

// -----------------------------------------------------------------------------
//  RAINMAKER ï¿½ WRITE CALLBACK
//  The RainMaker framework calls this whenever the app changes a parameter.
//  Replaces all BLYNK_WRITE() handlers from v1.x.
// -----------------------------------------------------------------------------
static void rmWriteCallback(Device *device, Param *param,
                            const param_val_t val, void *privData,
                            write_ctx_t *ctx)
{
  const char *name = param->getParamName();

  // -- Main controls ----------------------------------------------------------

  if (strcmp(name, PN_ARM) == 0) {
    // Toggle arm/disarm ï¿½ same 3-second confirm window as v1.x.
    bool wantArmed = val.val.b;
    if (wantArmed != systemArmed && !armTransitioning) {
      pendingArmedState = wantArmed;
      armTransitioning  = true;
      armPendingStart   = millis();
      pulseIn1();
      kragwagDev.updateAndReportParam(PN_ARMED_STATUS, "Pending...");
    }
    // PN_ARM echo is deferred; the arm confirmation timeout sends the final state.

  } else if (strcmp(name, PN_GATE1) == 0) {
    if (val.val.b) {
      pulseIO(PIN_IO0, io0Pulsing, io0PulseStart);
      if (settingNotifyGate1) {
        esp_rmaker_raise_alert("Gate 1 triggered");
        sendFCM("[Gate 1] KragWag", "Gate 1 triggered");
      }
      kragwagDev.updateAndReportParam(PN_GATE1, false);
    }

  } else if (strcmp(name, PN_GATE2) == 0) {
    if (val.val.b) {
      pulseIO(PIN_IO3, io3Pulsing, io3PulseStart);
      if (settingNotifyGate2) {
        esp_rmaker_raise_alert("Gate 2 triggered");
        sendFCM("[Gate 2] KragWag", "Gate 2 triggered");
      }
      kragwagDev.updateAndReportParam(PN_GATE2, false);
    }

  } else if (strcmp(name, PN_GATE3) == 0) {
    if (val.val.b) {
      if (!panicActive) {                          // block Gate 3 while panic latch is held
        pulseIO(PIN_IO6, io6Pulsing, io6PulseStart);
        if (settingNotifyGate3) {
          esp_rmaker_raise_alert("Gate 3 triggered");
          sendFCM("[Gate 3] KragWag", "Gate 3 triggered");
        }
      }
      kragwagDev.updateAndReportParam(PN_GATE3, false);
    }

  // -- Settings ---------------------------------------------------------------

  } else if (strcmp(name, PN_OTA_CHECK) == 0) {
    if (val.val.b) {
      otaCheckRequested = true;
      kragwagDev.updateAndReportParam(PN_OTA_CHECK, false);  // reset push button
    }

  } else if (strcmp(name, PN_NOTIF_OTA) == 0) {
    settingNotifyOTA = val.val.b;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putBool(NVS_KEY_NTFY_OTA, settingNotifyOTA);
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
    lowVoltAlerted = false;   // reset so the alert can fire at the new threshold
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putFloat(NVS_KEY_VOLT_THR, settingVoltThreshold);
    prefs.end();
    param->updateAndReport(val);

  } else if (strcmp(name, PN_NOTIF_SIREN) == 0) {
    settingNotifySiren = val.val.b;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putBool(NVS_KEY_NTFY_SIREN, settingNotifySiren);
    prefs.end();
    param->updateAndReport(val);

  } else if (strcmp(name, PN_NOTIF_GATE1) == 0) {
    settingNotifyGate1 = val.val.b;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putBool(NVS_KEY_NTFY_GATE1, settingNotifyGate1);
    prefs.end();
    param->updateAndReport(val);

  } else if (strcmp(name, PN_NOTIF_GATE2) == 0) {
    settingNotifyGate2 = val.val.b;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putBool(NVS_KEY_NTFY_GATE2, settingNotifyGate2);
    prefs.end();
    param->updateAndReport(val);

  } else if (strcmp(name, PN_NOTIF_GATE3) == 0) {
    settingNotifyGate3 = val.val.b;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putBool(NVS_KEY_NTFY_GATE3, settingNotifyGate3);
    prefs.end();
    param->updateAndReport(val);

  } else if (strcmp(name, PN_GATE_PULSE) == 0) {
    int v = val.val.i;
    if (v < 100 || v > 2000) return;
    settingGatePulseMs = (uint16_t)v;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUShort(NVS_KEY_GATE_PULSE, settingGatePulseMs);
    prefs.end();
    param->updateAndReport(val);

  } else if (strcmp(name, PN_ARM_PULSE) == 0) {
    int v = val.val.i;
    if (v < 100 || v > 2000) return;
    settingArmPulseMs = (uint16_t)v;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUShort(NVS_KEY_ARM_PULSE, settingArmPulseMs);
    prefs.end();
    param->updateAndReport(val);

  } else if (strcmp(name, PN_WIFI_LIVE) == 0) {
    wifiLiveMode = val.val.b;
    lastWifiCheck = 0;  // force immediate update on toggle
    param->updateAndReport(val);

  } else if (strcmp(name, PN_PANIC) == 0) {
    if (val.val.b) {
      // Toggle IO6 latch: activate keeps IO6 LOW (siren on); second press releases
      io6Pulsing = false;  // cancel any in-progress gate pulse on this pin
      panicActive = !panicActive;
      digitalWrite(PIN_IO6, panicActive ? LOW : HIGH);
      kragwagDev.updateAndReportParam(PN_PANIC_ACTIVE, panicActive);
      kragwagDev.updateAndReportParam(PN_PANIC, false);  // reset button
      if (settingNotifyGate3) {
        if (panicActive) {
          esp_rmaker_raise_alert("PANIC: Manual alarm ACTIVATED!");
          sendFCM("[PANIC] KragWag", "Panic alarm activated - siren latched ON");
        } else {
          esp_rmaker_raise_alert("PANIC: Manual alarm deactivated");
          sendFCM("[PANIC] KragWag", "Panic alarm deactivated - siren released");
        }
      }
    }
  } else if (strcmp(name, PN_RESTORE) == 0) {
    if (val.val.b) {
      restoreDefaultSettings();
      kragwagDev.updateAndReportParam(PN_RESTORE, false);  // reset push button
    }
  }
}

// -----------------------------------------------------------------------------
//  SYSTEM / PROVISIONING EVENT HANDLER
//  Called from a FreeRTOS task ï¿½ keep it short, only set flags.
//  Handles both provisioning lifecycle events and ongoing WiFi state changes.
// -----------------------------------------------------------------------------
void sysProvEvent(arduino_event_t *sys_event) {
  switch (sys_event->event_id) {

    case ARDUINO_EVENT_PROV_START:
      // SoftAP provisioning has started ï¿½ print the QR code to Serial so the
      // user can scan it with the ESP RainMaker app.
      Serial.printf("\nProvisioning started ï¿½ name: \"%s\", PoP: \"%s\"\n",
                    PROV_SERVICE_NAME, PROV_POP);
      WiFiProv.printQR(PROV_SERVICE_NAME, PROV_POP, "ble");
      break;

    case ARDUINO_EVENT_PROV_CRED_SUCCESS:
      // WiFi credentials accepted. The provisioning manager/handler will clean up
      // provisioning resources as configured.
      Serial.println("Provisioning credentials accepted");
      break;

    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      // WiFi STA is connected. Signal loop() to tear down the provisioning
      // manager (WiFiProv.endProvision) and SoftAP. This frees ~180 KB that
      // the provisioning stack holds, making heap available for SSL/OTA.
      // We use a flag rather than calling directly here because event handlers
      // run in a WiFi task context where prov_mgr_deinit is not safe to call.
      provDeinitNeeded = true;
      cloudConnected   = true;
      syncNeeded       = true;
      syncAt           = millis() + 3000;
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      cloudConnected = false;
      localServerStarted = false;   // re-init local HTTP server on next reconnect
      break;

    default: break;
  }
}

// -----------------------------------------------------------------------------
//  TIMER TASKS  (called from loop() via millis() counters)
//  Replaces BlynkTimer from v1.x.
// -----------------------------------------------------------------------------
void checkSiren() {
  sirenActive = (digitalRead(PIN_SIREN) == LOW);
  if (sirenActive != sirenWasActive) {
    kragwagDev.updateAndReportParam(PN_SIREN_ACTIVE, sirenActive);
    if (sirenActive && settingNotifySiren) {
      esp_rmaker_raise_alert("ALARM: Siren triggered!");
      sendFCM("[ALARM] KragWag", "Siren triggered!");
    }
    sirenWasActive = sirenActive;
  }
}

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
//  SETUP
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  // -- Pin modes --
  pinMode(PIN_IN1,      OUTPUT);
  pinMode(PIN_IO0,      OUTPUT);
  pinMode(PIN_IO3,      OUTPUT);
  pinMode(PIN_IO6,      OUTPUT);
  pinMode(PIN_SIREN,    INPUT);
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);

  // -- Safe idle states for digital outputs --
  digitalWrite(PIN_IN1, LOW);
  digitalWrite(PIN_IO0, HIGH);
  digitalWrite(PIN_IO3, HIGH);
  digitalWrite(PIN_IO6, HIGH);

  // -- ADC --
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  // -- PWM for LEDs (ESP32 Arduino 3.x API) --
  // See NOTE ON LEDC in the header if you are on board package 2.x.
  ledcAttach(PIN_LED_GREEN, LED_PWM_FREQ, LED_PWM_BITS);
  ledcAttach(PIN_LED_WHITE, LED_PWM_FREQ, LED_PWM_BITS);
  ledOff(PIN_LED_GREEN);
  ledOff(PIN_LED_WHITE);

  // -- Load settings from NVS --
  // RainMaker provisioning hasn't run yet so NVS is safe to read.
  loadSettingsFromNVS();

  // -- RainMaker ï¿½ build the device parameter model ---------------------------
  //
  //  Each block creates one Param, sets its UI hint and optional bounds,
  //  adds it to the device, then lets the Param go out of scope.
  //  The device retains its own internal copy of each param.
  //  We access params later via kragwagDev.updateAndReportParam(name, val).

  // -- Main params --
  {
    Param p(PN_ARM, "esp.param.power", value(false),
            PROP_FLAG_READ | PROP_FLAG_WRITE);
    p.addUIType("esp.ui.toggle");
    kragwagDev.addParam(p);
  }
  {
    // Read-only indicator; shown as a toggle (greyed out) in the app.
    Param p(PN_SIREN_ACTIVE, "esp.param.power", value(false), PROP_FLAG_READ);
    p.addUIType("esp.ui.toggle");
    kragwagDev.addParam(p);
  }
  {
    Param p(PN_GATE1, "esp.param.power", value(false),
            PROP_FLAG_READ | PROP_FLAG_WRITE);
    p.addUIType("esp.ui.push-btn-big");
    kragwagDev.addParam(p);
  }
  {
    Param p(PN_GATE2, "esp.param.power", value(false),
            PROP_FLAG_READ | PROP_FLAG_WRITE);
    p.addUIType("esp.ui.push-btn-big");
    kragwagDev.addParam(p);
  }
  {
    Param p(PN_GATE3, "esp.param.power", value(false),
            PROP_FLAG_READ | PROP_FLAG_WRITE);
    p.addUIType("esp.ui.push-btn-big");
    kragwagDev.addParam(p);
  }
  {
    // Supply voltage ï¿½ float, read-only, no special UI (shown as a number).
    Param p(PN_VOLTAGE, "esp.param.temperature", value(0.0f), PROP_FLAG_READ);
    kragwagDev.addParam(p);
  }
  {
    // Human-readable armed state string: "Disarmed" / "Pending..." / "Armed".
    Param p(PN_ARMED_STATUS, "esp.param.mode", value("Disarmed"), PROP_FLAG_READ);
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
    // Read-only OTA status string  - updated by firmware during OTA flow.
    Param p(PN_OTA_STATUS, "esp.param.mode", value("Tap Check OTA to update"), PROP_FLAG_READ);
    kragwagDev.addParam(p);
  }
  {
    // WiFi signal strength in dBm  - updated every 60 s (or 2 s in live mode).
    Param p(PN_WIFI_SIGNAL, "esp.param.mode", value(0), PROP_FLAG_READ);
    kragwagDev.addParam(p);
  }
  {
    // Live mode toggle  - app enables this for fast 2-s RSSI updates during site survey.
    Param p(PN_WIFI_LIVE, "esp.param.power", value(false), PROP_FLAG_READ | PROP_FLAG_WRITE);
    p.addUIType("esp.ui.toggle");
    kragwagDev.addParam(p);
  }
  {
    // Local IP  - reported on WiFi connect so the app can poll /rssi directly.
    Param p(PN_LOCAL_IP, "esp.param.mode", value("--"), PROP_FLAG_READ);
    kragwagDev.addParam(p);
  }

  // -- Settings params (initialised from NVS so the app sees current values) --
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
    Param p(PN_NOTIF_SIREN, "esp.param.power",
            value(settingNotifySiren), PROP_FLAG_READ | PROP_FLAG_WRITE);
    p.addUIType("esp.ui.toggle");
    kragwagDev.addParam(p);
  }
  {
    Param p(PN_NOTIF_GATE1, "esp.param.power",
            value(settingNotifyGate1), PROP_FLAG_READ | PROP_FLAG_WRITE);
    p.addUIType("esp.ui.toggle");
    kragwagDev.addParam(p);
  }
  {
    Param p(PN_NOTIF_GATE2, "esp.param.power",
            value(settingNotifyGate2), PROP_FLAG_READ | PROP_FLAG_WRITE);
    p.addUIType("esp.ui.toggle");
    kragwagDev.addParam(p);
  }
  {
    Param p(PN_NOTIF_GATE3, "esp.param.power",
            value(settingNotifyGate3), PROP_FLAG_READ | PROP_FLAG_WRITE);
    p.addUIType("esp.ui.toggle");
    kragwagDev.addParam(p);
  }
  // FCM / Worker params (read-only — values are set by firmware, not editable from app)
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
    Param p(PN_GATE_PULSE, "esp.param.mode",
            value((int)settingGatePulseMs), PROP_FLAG_READ | PROP_FLAG_WRITE);
    p.addUIType("esp.ui.slider");
    p.addBounds(value(100), value(2000), value(50));
    kragwagDev.addParam(p);
  }
  {
    Param p(PN_ARM_PULSE, "esp.param.mode",
            value((int)settingArmPulseMs), PROP_FLAG_READ | PROP_FLAG_WRITE);
    p.addUIType("esp.ui.slider");
    p.addBounds(value(100), value(2000), value(50));
    kragwagDev.addParam(p);
  }
  {
    Param p(PN_RESTORE, "esp.param.power", value(false),
            PROP_FLAG_READ | PROP_FLAG_WRITE);
    p.addUIType("esp.ui.push-btn-big");
    kragwagDev.addParam(p);
  }
  {
    // Panic Alarm button -- toggles IO6 latch (siren ON/OFF)
    Param p(PN_PANIC, "esp.param.power", value(false),
            PROP_FLAG_READ | PROP_FLAG_WRITE);
    p.addUIType("esp.ui.push-btn-big");
    kragwagDev.addParam(p);
  }
  {
    // Panic Active -- read-only, true while IO6 is latched LOW
    Param p(PN_PANIC_ACTIVE, "esp.param.power", value(false), PROP_FLAG_READ);
    kragwagDev.addParam(p);
  }

  kragwagDev.addCb(rmWriteCallback);
  // Make "Arm / Disarm" the primary (large) control shown at the top of the device card.
  kragwagDev.assignPrimaryParam(kragwagDev.getParamByName(PN_ARM));

  // -- RainMaker ï¿½ init node and start --------------------------------------
  Node rmNode = RMaker.initNode("KragWag", "KragWag Energiser Monitor");
  rmNode.addDevice(kragwagDev);

  RMaker.enableSchedule();  // allows timed schedules set from the app
  RMaker.start();

  // Register local HTTP route once at boot — NOT in the WiFi reconnect path.
  // Moving this out of loop() prevents a handler entry from being appended to
  // WebServer's internal linked list on every WiFi reconnect (heap leak fix).
  localServer.on("/rssi", HTTP_GET, []() {
    String json = String("{\"rssi\":") + WiFi.RSSI() + "}";
    localServer.sendHeader("Access-Control-Allow-Origin", "*");
    localServer.send(200, "application/json", json);
  });

  // -- WiFi + SoftAP provisioning --------------------------------------------
  // Register the event handler BEFORE beginProvision() so we never miss
  // ARDUINO_EVENT_PROV_START (which is where the QR code gets printed).
  // The same handler also tracks WiFi connected/disconnected for the LED and
  // the deferred cloud state sync.
  //
  // First boot: device creates a temporary WiFi AP named PROV_SERVICE_NAME; the app walks through WiFi
  // credential entry. Credentials persist in NVS ï¿½ subsequent boots
  // auto-connect and ARDUINO_EVENT_PROV_START never fires (no QR printed).
  //
  // SoftAP provisioning avoids the BLE/BTDM startup path on ESP32-C3.
  WiFi.onEvent(sysProvEvent);
  WiFiProv.beginProvision(
      NETWORK_PROV_SCHEME_SOFTAP,
      NETWORK_PROV_SCHEME_HANDLER_NONE,
      NETWORK_PROV_SECURITY_1,
      PROV_POP,
      PROV_SERVICE_NAME);

  // NOTE: heartbeatTask is NOT created here.
  // It is created in loop() after WiFiProv.endProvision() frees ~180 KB of
  // provisioning heap, so that the MQTT TLS handshake is never starved of memory.
}

// -----------------------------------------------------------------------------
//  MAIN LOOP
// -----------------------------------------------------------------------------
void loop() {
  unsigned long now = millis();

  // -- Provisioning teardown (deferred from WiFi-got-IP event) -------------
  // Calling WiFiProv.endProvision() inside the WiFi event handler is unsafe;
  // do it here in the main loop instead. This frees the provisioning manager
  // and SoftAP interface, reclaiming ~180 KB of heap for SSL/OTA use.
  if (provDeinitNeeded) {
    provDeinitNeeded = false;
    WiFiProv.endProvision();
    WiFi.softAPdisconnect(true);
  }

  // -- BOOT button ï¿½ hold 5 s ? full factory reset --------------------------
  // Clears WiFi credentials AND RainMaker cloud pairing (device-user association).
  // Board reboots into provisioning mode. Settings stored in our NVS namespace
  // are NOT cleared here (preserved in case the reset was accidental); the user
  // can tap "Restore Defaults" in the app to reset settings separately.
  if (digitalRead(PIN_BOOT_BTN) == LOW) {
    if (!bootBtnHeld) {
      bootBtnHeld       = true;
      bootBtnPressStart = now;
    } else if (now - bootBtnPressStart >= WIFI_RESET_HOLD_MS) {
      // RMakerFactoryReset(seconds): waits, then clears WiFi credentials and
      // RainMaker cloud pairing before restarting into provisioning mode.
      RMakerFactoryReset(2);
    }
  } else {
    bootBtnHeld = false;
  }

  // -- Arm/disarm confirmation timeout --------------------------------------
  if (armTransitioning && (now - armPendingStart >= ARM_CONFIRM_MS)) {
    systemArmed      = pendingArmedState;
    armTransitioning = false;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putBool(NVS_KEY_ARM, systemArmed);           // persist across restarts
    prefs.end();
    kragwagDev.updateAndReportParam(PN_ARM,          systemArmed);
    kragwagDev.updateAndReportParam(PN_ARMED_STATUS, systemArmed ? "Armed" : "Disarmed");
  }

  // -- IN1 pulse timeout (arm/disarm pulse) ---------------------------------
  if (in1Pulsing && (now - in1PulseStart >= settingArmPulseMs)) {
    digitalWrite(PIN_IN1, LOW);
    in1Pulsing = false;
  }

  // -- IO pulse timeouts (gate/garage pulses) --------------------------------
  if (io0Pulsing && (now - io0PulseStart >= settingGatePulseMs)) {
    digitalWrite(PIN_IO0, HIGH);
    io0Pulsing = false;
  }
  if (io3Pulsing && (now - io3PulseStart >= settingGatePulseMs)) {
    digitalWrite(PIN_IO3, HIGH);
    io3Pulsing = false;
  }
  if (io6Pulsing && (now - io6PulseStart >= settingGatePulseMs)) {
    digitalWrite(PIN_IO6, HIGH);
    io6Pulsing = false;
  }

  // -- Deferred cloud sync (triggered by WiFi-got-IP event) ----------------
  // Waits 3 s after WiFi connects to give the RainMaker MQTT time to come up,
  // then pushes current state so the app shows live values immediately.
  if (syncNeeded && (now - syncAt < 0x80000000UL) && now >= syncAt) {
    syncNeeded = false;
    syncStateToCloud();
  }

  // -- Siren check ï¿½ every 200 ms -------------------------------------------
  if (now - lastSirenCheck >= SIREN_CHECK_MS) {
    lastSirenCheck = now;
    checkSiren();
  }

  // -- Voltage check ï¿½ every 60 s -------------------------------------------
  if (now - lastVoltageCheck >= VOLTAGE_CHECK_MS) {
    lastVoltageCheck = now;
    checkVoltage();
  }

  // -- Heartbeat ï¿½ every 5 min -----------------------------------------------
  if (now - lastHeartbeat >= HEARTBEAT_MS) {
    lastHeartbeat = now;
    sendHeartbeat();
  }

  // -- Remote OTA check (deferred ï¿½ triggered by "Check OTA" button) ---------
  // Local HTTP server -- handle client requests every loop
  if (WiFi.isConnected() && !localServerStarted) {
    // Route is registered once in setup() — only begin() here on each reconnect.
    localServer.begin();
    localServerStarted = true;
    kragwagDev.updateAndReportParam(PN_LOCAL_IP, WiFi.localIP().toString().c_str());
  }
  if (localServerStarted) localServer.handleClient();

  // WiFi signal strength update
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

  LedPattern greenPattern;
  if (sirenActive)           greenPattern = LED_FAST_BLINK;
  else if (armTransitioning) greenPattern = LED_SLOW_BLINK;
  else if (systemArmed)      greenPattern = LED_DOUBLE_PULSE;
  else                       greenPattern = LED_SLOW_BLINK;

  runLed(PIN_LED_WHITE, whitePattern, whiteLed);
  runLed(PIN_LED_GREEN, greenPattern, greenLed);
}
