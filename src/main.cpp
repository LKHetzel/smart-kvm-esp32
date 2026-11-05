#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Missing include/secrets.h - copy include/secrets.example.h to include/secrets.h and fill it in."
#endif

// ---------------------------
// User configuration
// ---------------------------
// WIFI_SSID, WIFI_PASSWORD, API_TOKEN, UI_USERNAME and UI_PASSWORD live in include/secrets.h.
static const char* HOSTNAME = "kvm-controller";

// Bump on each release; shown in the web pages, /status and the serial log.
static const char* FW_VERSION = "1.1.0";
static const char* FW_BUILD = __DATE__ " " __TIME__;

// ESP32 UART2 pins. These go to the TRRS terminal adapter.
// ESP32 GPIO17 (TX) -> KVM_RX (TRRS Ring 2)
// ESP32 GPIO16 (RX) <- KVM_TX (TRRS Ring 1)
// ESP32 GND         -> KVM GND (TRRS Sleeve)
// TRRS Tip (3.3V) is intentionally left unconnected.
static constexpr int KVM_TX_PIN = 17;
static constexpr int KVM_RX_PIN = 16;

// Default baud rate; can be changed from the Settings page (saved in NVS).
static constexpr uint32_t DEFAULT_KVM_BAUD = 9600; // confirmed on the HDK403-P23 (the 2-PC manual says 38400)
static constexpr uint32_t SUPPORTED_BAUDS[] = {9600, 19200, 38400, 57600, 115200};

static constexpr uint16_t HTTP_PORT = 80;
// Minimum time between a switch and whatever is sent next. Rapid switching has hung the KVM's
// serial port until it was power-cycled, so every transmission is paced (see waitForKvmIdle).
static constexpr uint32_t COMMAND_GAP_MS = 250;
static constexpr uint32_t KVM_IDLE_MAX_MS = 500; // give up waiting for a quiet line after this long

// How long to wait for a reply after a query or raw send.
static constexpr uint32_t REPLY_WAIT_MS = 120;
// Unsolicited bytes from the KVM are logged as one entry once the line is quiet this long.
static constexpr uint32_t RX_GAP_MS = 20;
static constexpr size_t RAW_MAX_BYTES = 64;

// The controller name, PC names and presets are edited on the Settings page and saved in NVS.
static constexpr size_t NAME_MAX_LEN = 24; // bytes
static constexpr size_t TITLE_MAX_LEN = 32; // bytes
static const char* DEFAULT_TITLE = "TESmart KVM";
static constexpr size_t MAX_PRESETS = 8;

static constexpr uint8_t NUM_PCS = 4;

// Companion feedback: state is pushed to Companion custom variables when it changes, plus a
// periodic full resend in case Companion restarted. Requests are short so a missing Companion
// can't stall the web server for long.
static constexpr uint16_t DEFAULT_COMPANION_PORT = 8000;
static constexpr uint32_t COMPANION_TIMEOUT_MS = 500;
static constexpr uint32_t COMPANION_RETRY_MS = 10000;
static constexpr uint32_t COMPANION_RESYNC_MS = 60000;
static constexpr size_t COMPANION_PREFIX_MAX_LEN = 16;

// Reboot if Wi-Fi can't connect at boot within this time.
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 30000;

// Web assets from web/, embedded by board_build.embed_txtfiles in platformio.ini.
extern const char INDEX_HTML[] asm("_binary_web_index_html_start");
extern const char SETTINGS_HTML[] asm("_binary_web_settings_html_start");
extern const char APP_CSS[] asm("_binary_web_app_css_start");
extern const char APP_JS[] asm("_binary_web_app_js_start");

WebServer server(HTTP_PORT);
Preferences prefs;
HardwareSerial KvmSerial(2);

struct RoutingState {
  uint8_t output1 = 0; // 0 unknown, 1-4 PC number
  uint8_t output2 = 0;
  uint8_t output3 = 0;
};

RoutingState state;
static RoutingState kvmRoutes; // last routing the KVM reported, used to check that switches took effect
static uint8_t kmFocus = 0; // PC with the keyboard and mouse: 0 unknown, 1-4 PC number. Read from the KVM.

// Stored as a raw NVS blob: changing this layout means changing the "presets2" key too.
struct Preset {
  char name[NAME_MAX_LEN + 1];
  uint8_t outputs[3]; // per output: 0 leave unchanged, 1-4 PC number
  uint8_t focus;      // keyboard and mouse: 0 leave unchanged, 1-4 PC number
};

// Layout of the older "presets" key, read once to migrate it.
struct PresetV1 {
  char name[NAME_MAX_LEN + 1];
  uint8_t outputs[3];
};

static Preset presets[MAX_PRESETS];
static size_t presetCount = 0;
static String pcNames[NUM_PCS];
static String deviceTitle; // shown as the page heading and browser tab title

struct CompanionConfig {
  String host; // blank = disabled
  uint16_t port = DEFAULT_COMPANION_PORT;
  String prefix = "kvm";
};

static CompanionConfig companion;
static constexpr size_t COMPANION_VAR_COUNT = 5;
static const char* COMPANION_VARS[COMPANION_VAR_COUNT] = {"out1", "out2", "out3", "preset", "focus"};
static int companionPushed[COMPANION_VAR_COUNT] = {-1, -1, -1, -1, -1}; // last values Companion accepted; -1 forces a push
static uint32_t companionRetryAt = 0;
static uint32_t companionSyncedAt = 0;
static String companionResult = "Not configured";
static uint32_t kvmBaud = DEFAULT_KVM_BAUD;
static bool wifiWasConnected = false;

// TESmart commands: AA BB <type> <arg1> <arg2> <checksum>, where checksum is the low byte of the
// sum of the earlier bytes. The 2-PC model's manual shows EE in place of the checksum, but the
// HDK403-P23 ignores those frames and reports in the checksum form itself.
// Replies and reports: AA BB <type> <length> <data...> <checksum>.
static constexpr uint8_t KVM_SWITCH = 0x03;        // arg1 output 1-3 (0 = all), arg2 PC (0-based). Verified.
// The manual's buzzer toggle (04 02 xx) does nothing on the HDK403-P23, with or without checksum.
// Queries are sent with arg1 0x00, arg2 0xFF.
static constexpr uint8_t KVM_FOCUS_REPORT = 0x82;  // data: PC with the keyboard and mouse (0-based). Also sent unprompted when it moves. Verified.
static constexpr uint8_t KVM_ROUTE_REPORT = 0x83;  // data: <output 1-3> <PC (0-based)> pairs. Verified.
static constexpr size_t KVM_FRAME_LEN = 6;
static constexpr size_t KVM_RX_MAX_FRAME = 24;
// A route reply this soon after we send a switch may predate it, so it's ignored.
static constexpr uint32_t KVM_SETTLE_MS = 1000;
static constexpr uint32_t KVM_POLL_MS = 5000; // one query per tick, alternating focus and routing
static uint32_t lastSwitchTxMs = 0;
static uint32_t lastPollMs = 0;
static uint8_t pollAwaiting = 0; // type of the background query in flight; its reply stays out of the log

// ---------------------------
// Auth
// ---------------------------
static bool authRequired() {
  return API_TOKEN[0] != '\0' && strcmp(API_TOKEN, "CHANGE_ME_TOKEN") != 0;
}

// Compare without early exit so response timing doesn't leak how much of the token matched.
static bool constantTimeEquals(const String& a, const String& b) {
  if (a.length() != b.length()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < a.length(); i++) diff |= a[i] ^ b[i];
  return diff == 0;
}

static bool authorized() {
  if (!authRequired()) return true;
  if (!server.hasHeader("Authorization")) return false;
  return constantTimeEquals(server.header("Authorization"), String("Bearer ") + API_TOKEN);
}

static bool requireAuth() {
  if (authorized()) return true;
  server.send(401, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
  return false;
}

// Built-in pages and /ui/*: HTTP Basic Auth, which the browser prompts for once and remembers.
static bool uiLoggedIn() {
  return UI_PASSWORD[0] == '\0' || server.authenticate(UI_USERNAME, UI_PASSWORD);
}

// POSTs must also carry X-Requested-With. Browsers won't add a custom header cross-origin
// without a CORS preflight (which we don't answer), so other sites can't ride the saved login.
static bool uiRequestAllowed() {
  return server.method() != HTTP_POST || server.hasHeader("X-Requested-With");
}

static bool requireUiAuth() {
  if (!uiLoggedIn()) {
    server.requestAuthentication(BASIC_AUTH, "TESmart KVM");
    return false;
  }
  if (!uiRequestAllowed()) {
    server.send(403, "application/json", "{\"ok\":false,\"error\":\"missing X-Requested-With\"}");
    return false;
  }
  return true;
}

// ---------------------------
// State
// ---------------------------
static void persistState() {
  prefs.putUChar("o1", state.output1);
  prefs.putUChar("o2", state.output2);
  prefs.putUChar("o3", state.output3);
}

static void loadState() {
  state.output1 = prefs.getUChar("o1", 0);
  state.output2 = prefs.getUChar("o2", 0);
  state.output3 = prefs.getUChar("o3", 0);
}

static void savePresets() {
  prefs.putUChar("presetCount", presetCount);
  if (presetCount) prefs.putBytes("presets2", presets, presetCount * sizeof(Preset));
  if (prefs.isKey("presets")) prefs.remove("presets");
}

// Reads presetCount presets, converting the older layout if that's what is stored.
static bool loadPresets() {
  if (prefs.isKey("presets2")) {
    return prefs.getBytes("presets2", presets, presetCount * sizeof(Preset)) == presetCount * sizeof(Preset);
  }
  PresetV1 old[MAX_PRESETS];
  if (prefs.getBytes("presets", old, presetCount * sizeof(PresetV1)) != presetCount * sizeof(PresetV1)) return false;
  for (size_t i = 0; i < presetCount; i++) {
    memcpy(&presets[i], &old[i], sizeof(PresetV1));
    presets[i].focus = 0;
  }
  return true;
}

static void loadConfig() {
  deviceTitle = prefs.getString("title", DEFAULT_TITLE);
  for (uint8_t i = 0; i < NUM_PCS; i++) {
    String n = String(i + 1);
    pcNames[i] = prefs.getString(("pc" + n + "name").c_str(), "PC" + n);
  }

  companion.host = prefs.getString("cmpHost", "");
  companion.port = prefs.getUShort("cmpPort", DEFAULT_COMPANION_PORT);
  companion.prefix = prefs.getString("cmpPrefix", "kvm");

  if (prefs.isKey("presetCount")) {
    presetCount = prefs.getUChar("presetCount", 0);
    if (presetCount <= MAX_PRESETS && (presetCount == 0 || loadPresets())) return;
  }
  // First boot (or unreadable data): no presets until they're added on the Settings page.
  presetCount = 0;
}

// Drops control characters, trims, and truncates to maxLen bytes without splitting a UTF-8 character.
static String cleanName(const String& in, size_t maxLen = NAME_MAX_LEN) {
  String out;
  for (char c : in) if ((uint8_t)c >= 0x20 && c != 0x7F) out += c;
  out.trim();
  if (out.length() > maxLen) {
    size_t n = maxLen;
    while (n > 0 && ((uint8_t)out[n] & 0xC0) == 0x80) n--;
    out = out.substring(0, n);
  }
  return out;
}

static String jsonString(const String& s) {
  String out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + "\"";
}

static bool supportedBaud(uint32_t baud) {
  for (uint32_t b : SUPPORTED_BAUDS) if (b == baud) return true;
  return false;
}

// ---------------------------
// KVM serial and log
// ---------------------------
struct LogEntry {
  uint32_t id;
  uint32_t ms;
  bool tx;
  String hex;
};

static constexpr size_t LOG_SIZE = 40;
static LogEntry serialLog[LOG_SIZE];
static uint32_t nextLogId = 1;
static String pendingRx; // unsolicited bytes not yet logged
static uint32_t lastRxMs = 0;

static void appendHex(String& hex, uint8_t b) {
  static const char digits[] = "0123456789ABCDEF";
  if (hex.length()) hex += ' ';
  hex += digits[b >> 4];
  hex += digits[b & 0x0F];
}

static String toHex(const uint8_t* data, size_t len) {
  String hex;
  for (size_t i = 0; i < len; i++) appendHex(hex, data[i]);
  return hex;
}

static void logSerial(bool tx, const String& hex) {
  LogEntry& e = serialLog[nextLogId % LOG_SIZE];
  e.id = nextLogId++;
  e.ms = millis();
  e.tx = tx;
  e.hex = hex;
}

static void flushPendingRx() {
  if (!pendingRx.length()) return;
  logSerial(false, pendingRx);
  pendingRx = "";
}

static uint8_t kvmChecksum(const uint8_t* data, size_t len) {
  uint8_t sum = 0;
  for (size_t i = 0; i < len; i++) sum += data[i];
  return sum;
}

// Updates state from a frame the KVM sent.
static void handleKvmFrame(const uint8_t* f) {
  const uint8_t type = f[2];
  const uint8_t len = f[3];
  const uint8_t* data = f + 4;
  if (type == KVM_FOCUS_REPORT && len == 1 && data[0] < NUM_PCS) {
    kmFocus = data[0] + 1;
  } else if (type == KVM_ROUTE_REPORT && len % 2 == 0) {
    for (uint8_t i = 0; i < len; i += 2) {
      uint8_t pc = data[i + 1];
      if (pc >= NUM_PCS) return;
      if (data[i] == 1) kvmRoutes.output1 = pc + 1;
      else if (data[i] == 2) kvmRoutes.output2 = pc + 1;
      else if (data[i] == 3) kvmRoutes.output3 = pc + 1;
      else return;
    }
    if (millis() - lastSwitchTxMs < KVM_SETTLE_MS) return;
    if (!kvmRoutes.output1 || !kvmRoutes.output2 || !kvmRoutes.output3) return;
    if (memcmp(&kvmRoutes, &state, sizeof(state)) == 0) return;
    state = kvmRoutes;
    persistState();
    Serial.printf("KVM reports outputs on PC%u, PC%u, PC%u\n", state.output1, state.output2, state.output3);
  }
}

// Assembles received bytes into frames and handles each valid one. Returns the type of the
// frame this byte completed, or 0.
static uint8_t onKvmByte(uint8_t b) {
  static uint8_t frame[KVM_RX_MAX_FRAME];
  static size_t n = 0;
  if (n == 0 && b != 0xAA) return 0;
  if (n == 1 && b != 0xBB) {
    n = b == 0xAA ? 1 : 0;
    return 0;
  }
  if (n == 3 && b > KVM_RX_MAX_FRAME - 5) {
    n = 0;
    return 0;
  }
  frame[n++] = b;
  if (n < 5 || n < 5u + frame[3]) return 0;
  n = 0;
  if (b != kvmChecksum(frame, 4 + frame[3])) return 0;
  handleKvmFrame(frame);
  return frame[2];
}

// Collects bytes the KVM sends on its own, logging each burst once the line goes quiet.
static void pollSerial() {
  while (KvmSerial.available()) {
    uint8_t b = KvmSerial.read();
    appendHex(pendingRx, b);
    uint8_t type = onKvmByte(b);
    if (type && type == pollAwaiting) {
      pendingRx = "";
      pollAwaiting = 0;
    }
    lastRxMs = millis();
  }
  if (pendingRx.length() && millis() - lastRxMs > RX_GAP_MS) flushPendingRx();
}

// Holds off until the KVM has finished sending and the last switch has had time to take effect.
static void waitForKvmIdle() {
  uint32_t start = millis();
  while (millis() - start < KVM_IDLE_MAX_MS) {
    pollSerial();
    uint32_t now = millis();
    bool replyDue = pollAwaiting && now - lastPollMs < REPLY_WAIT_MS;
    if (!replyDue && now - lastSwitchTxMs >= COMMAND_GAP_MS && now - lastRxMs >= RX_GAP_MS) return;
    delay(1);
  }
}

static void sendCommand(const uint8_t* data, size_t len) {
  waitForKvmIdle();
  // Log anything already received first so the log stays in order.
  flushPendingRx();
  pollAwaiting = 0;
  KvmSerial.write(data, len);
  KvmSerial.flush();
  if (len > 2 && data[2] == KVM_SWITCH) lastSwitchTxMs = millis();
  logSerial(true, toHex(data, len));
}

// Sends a command and returns whatever the KVM replies within REPLY_WAIT_MS (also logged).
static String sendAndReadReply(const uint8_t* data, size_t len) {
  sendCommand(data, len);
  delay(REPLY_WAIT_MS);
  String hex;
  while (KvmSerial.available()) {
    uint8_t b = KvmSerial.read();
    appendHex(hex, b);
    onKvmByte(b);
  }
  if (hex.length()) logSerial(false, hex);
  return hex;
}

static void sendKvm(uint8_t type, uint8_t arg1, uint8_t arg2) {
  uint8_t f[KVM_FRAME_LEN] = {0xAA, 0xBB, type, arg1, arg2, 0};
  f[5] = kvmChecksum(f, 5);
  sendCommand(f, sizeof(f));
}

// Sends a query and waits for the reply, which updates the state. Returns the reply as hex.
static String queryKvm(uint8_t type) {
  uint8_t f[KVM_FRAME_LEN] = {0xAA, 0xBB, type, 0x00, 0xFF, 0};
  f[5] = kvmChecksum(f, 5);
  return sendAndReadReply(f, sizeof(f));
}

// Keeps the state in step with changes made at the KVM. Sent without logging: the log would
// otherwise fill up with these. A reply that arrives later is handled by pollSerial().
static void pollKvmState() {
  static bool routeNext = false;
  uint32_t now = millis();
  if (now - lastPollMs < KVM_POLL_MS || now - lastSwitchTxMs < KVM_SETTLE_MS ||
      now - lastRxMs < RX_GAP_MS || pendingRx.length()) return;
  lastPollMs = now;
  uint8_t type = routeNext ? KVM_ROUTE_REPORT : KVM_FOCUS_REPORT;
  routeNext = !routeNext;
  uint8_t f[KVM_FRAME_LEN] = {0xAA, 0xBB, type, 0x00, 0xFF, 0};
  f[5] = kvmChecksum(f, 5);
  pollAwaiting = type;
  KvmSerial.write(f, sizeof(f));
}

static bool routeOutput(uint8_t output, uint8_t pc) {
  if (output < 1 || output > 3 || pc < 1 || pc > NUM_PCS) return false;

  sendKvm(KVM_SWITCH, output, pc - 1);

  if (output == 1) state.output1 = pc;
  if (output == 2) state.output2 = pc;
  if (output == 3) state.output3 = pc;
  persistState();
  return true;
}

static bool waitForFocus(uint8_t pc) {
  for (int i = 0; i < 5; i++) {
    queryKvm(KVM_FOCUS_REPORT);
    if (kmFocus == pc) return true;
    delay(COMMAND_GAP_MS);
  }
  return false;
}

// The KVM can drop a switch that arrives while the keyboard and mouse are moving, so read the
// routing back and resend whatever didn't take. Passes if the KVM doesn't answer, as there's
// nothing to check against.
static bool confirmRouting() {
  const uint8_t want[3] = {state.output1, state.output2, state.output3};
  for (int i = 0; i < 4; i++) {
    kvmRoutes = RoutingState();
    queryKvm(KVM_ROUTE_REPORT);
    const uint8_t got[3] = {kvmRoutes.output1, kvmRoutes.output2, kvmRoutes.output3};
    if (!got[0] || !got[1] || !got[2]) return true;
    bool match = true;
    for (uint8_t o = 0; o < 3; o++) {
      if (got[o] == want[o]) continue;
      match = false;
      sendKvm(KVM_SWITCH, o + 1, want[o] - 1);
    }
    if (match) return true;
  }
  return false;
}

// No command to set the keyboard and mouse is known. The KVM moves them itself when the PC they're
// on leaves every monitor, and they stay put when that PC comes back. So: briefly switch the
// monitors showing the current PC to the target, wait for the move, then put them back. If that
// doesn't land, switch every monitor to the target instead. Returns an error message, or "" on success.
static String moveFocus(uint8_t pc) {
  queryKvm(KVM_ROUTE_REPORT);
  queryKvm(KVM_FOCUS_REPORT);
  if (kmFocus == pc) return "";

  const uint8_t current[3] = {state.output1, state.output2, state.output3};
  if (!current[0] || !current[1] || !current[2]) return "routing is not known yet";
  if (current[0] != pc && current[1] != pc && current[2] != pc) {
    return "PC" + String(pc) + " is not on any output, and the keyboard and mouse can only move to a PC on screen";
  }

  const uint8_t from = kmFocus;
  for (uint8_t o = 0; o < 3; o++) {
    if (current[o] != from) continue;
    sendKvm(KVM_SWITCH, o + 1, pc - 1);
  }
  bool moved = waitForFocus(pc);
  if (!moved) {
    sendKvm(KVM_SWITCH, 0x00, pc - 1);
    moved = waitForFocus(pc);
  }
  // Everything the moves above changed gets put back here.
  if (!confirmRouting()) return "the outputs did not return to their PCs";
  if (!moved || !waitForFocus(pc)) return "the keyboard and mouse did not move to PC" + String(pc);
  return "";
}

// 1-based number of the first preset whose outputs and keyboard/mouse all match the current state, or 0.
static int activePreset() {
  const uint8_t current[3] = {state.output1, state.output2, state.output3};
  for (size_t i = 0; i < presetCount; i++) {
    bool match = true;
    for (int o = 0; o < 3; o++) {
      if (presets[i].outputs[o] && presets[i].outputs[o] != current[o]) match = false;
    }
    if (presets[i].focus && presets[i].focus != kmFocus) match = false;
    if (match) return i + 1;
  }
  return 0;
}

// Returns an error message, or "" on success.
static String applyPreset(const Preset& p) {
  bool routed = false;
  for (uint8_t o = 0; o < 3; o++) {
    if (!p.outputs[o]) continue;
    routeOutput(o + 1, p.outputs[o]);
    routed = true;
  }
  if (routed && !confirmRouting()) return "some outputs did not switch";
  if (!p.focus) return "";
  return moveFocus(p.focus);
}

// ---------------------------
// Companion feedback
// ---------------------------
// Minimal HTTP/1.1 POST with an empty body. HTTPClient would do, but it pulls in TLS (~165 KB of flash).
// Returns the HTTP status, or -1 can't connect, -2 no reply in time, -3 unreadable reply.
static int companionPost(const String& path) {
  WiFiClient client;
  if (!client.connect(companion.host.c_str(), companion.port, COMPANION_TIMEOUT_MS)) return -1;
  client.print("POST " + path + " HTTP/1.1\r\nHost: " + companion.host + ":" + String(companion.port) +
               "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");

  // Only the status line ("HTTP/1.1 200 OK") is needed.
  String line;
  uint32_t start = millis();
  while (millis() - start < COMPANION_TIMEOUT_MS) {
    while (client.available()) {
      char c = client.read();
      if (c == '\n') {
        client.stop();
        int space = line.indexOf(' ');
        return space > 0 ? line.substring(space + 1).toInt() : -3;
      }
      if (line.length() < 64) line += c;
    }
    if (!client.connected()) break;
    delay(1);
  }
  client.stop();
  return -2;
}

static bool pushCompanionVariable(const char* name, int value) {
  String var = companion.prefix + "_" + name;
  int code = companionPost("/api/custom-variable/" + var + "/value?value=" + String(value));
  if (code >= 200 && code < 300) return true;
  if (code == -1) companionResult = "Can't connect to Companion at " + companion.host + ":" + String(companion.port);
  else if (code == -2) companionResult = "Companion didn't reply";
  else if (code == -3) companionResult = "Unreadable reply from Companion";
  else if (code == 404) companionResult = "Companion has no custom variable \"" + var + "\". Create it under Variables.";
  else companionResult = "Companion returned HTTP " + String(code) + " for " + var;
  return false;
}

// Pushes changed values (or all of them when full is set). Returns false on the first failure.
static bool syncCompanion(bool full) {
  int values[COMPANION_VAR_COUNT] = {state.output1, state.output2, state.output3, activePreset(), kmFocus};
  for (size_t i = 0; i < COMPANION_VAR_COUNT; i++) {
    if (!full && values[i] == companionPushed[i]) continue;
    if (!pushCompanionVariable(COMPANION_VARS[i], values[i])) return false;
    companionPushed[i] = values[i];
  }
  companionResult = "OK, last sent at " + String(millis() / 1000) + "s uptime";
  return true;
}

static void companionLoop() {
  if (!companion.host.length() || WiFi.status() != WL_CONNECTED) return;
  uint32_t now = millis();
  if ((int32_t)(now - companionRetryAt) < 0) return;

  bool full = companionSyncedAt == 0 || now - companionSyncedAt > COMPANION_RESYNC_MS;
  int values[COMPANION_VAR_COUNT] = {state.output1, state.output2, state.output3, activePreset(), kmFocus};
  bool changed = false;
  for (size_t i = 0; i < COMPANION_VAR_COUNT; i++) changed |= values[i] != companionPushed[i];
  if (!full && !changed) return;

  if (syncCompanion(full)) {
    if (full) companionSyncedAt = now ? now : 1;
  } else {
    companionRetryAt = now + COMPANION_RETRY_MS;
  }
}

// Forces every variable to be resent on the next loop.
static void resetCompanionSync() {
  for (int& v : companionPushed) v = -1;
  companionRetryAt = millis();
  companionSyncedAt = 0;
  companionResult = companion.host.length() ? "Waiting to send" : "Not configured";
}

// Host: letters, digits, dots, dashes. Prefix: letters, digits, underscores, dashes.
static bool validCompanionHost(const String& h) {
  if (h.length() > 64) return false;
  for (char c : h) if (!isalnum((unsigned char)c) && c != '.' && c != '-') return false;
  return true;
}

static bool validCompanionPrefix(const String& p) {
  if (!p.length() || p.length() > COMPANION_PREFIX_MAX_LEN) return false;
  for (char c : p) if (!isalnum((unsigned char)c) && c != '_' && c != '-') return false;
  return true;
}

// Reads companionHost/companionPort/companionPrefix form fields. Returns an error message, or "" if valid.
static String companionFromArgs(CompanionConfig& out) {
  out.host = server.arg("companionHost");
  out.host.trim();
  if (!validCompanionHost(out.host)) return "Companion host must be an IP address or hostname";
  String port = server.arg("companionPort");
  long p = port.length() ? port.toInt() : DEFAULT_COMPANION_PORT;
  if (p < 1 || p > 65535) return "Companion port must be 1-65535";
  out.port = p;
  String prefix = server.arg("companionPrefix");
  out.prefix = prefix.length() ? prefix : "kvm";
  if (!validCompanionPrefix(out.prefix)) {
    return "variable prefix must be 1-" + String(COMPANION_PREFIX_MAX_LEN) + " letters, digits, _ or -";
  }
  return "";
}

static void saveCompanion(const CompanionConfig& c) {
  companion = c;
  prefs.putString("cmpHost", companion.host);
  prefs.putUShort("cmpPort", companion.port);
  prefs.putString("cmpPrefix", companion.prefix);
  resetCompanionSync();
}

// ---------------------------
// HTTP handlers
// ---------------------------
static String stateJson() {
  String json = "{";
  json += "\"version\":\"" + String(FW_VERSION) + "\",";
  json += "\"build\":\"" + String(FW_BUILD) + "\",";
  json += "\"uptime\":" + String(millis() / 1000) + ",";
  json += "\"title\":" + jsonString(deviceTitle) + ",";
  json += "\"hostname\":\"" + String(HOSTNAME) + "\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"pcs\":[";
  for (uint8_t i = 0; i < NUM_PCS; i++) json += (i ? "," : "") + jsonString(pcNames[i]);
  json += "],";
  json += "\"presets\":[";
  for (size_t i = 0; i < presetCount; i++) {
    const Preset& p = presets[i];
    if (i) json += ',';
    json += "{\"name\":" + jsonString(p.name) + ",\"outputs\":[" + String(p.outputs[0]) + "," +
            String(p.outputs[1]) + "," + String(p.outputs[2]) + "],\"focus\":" + String(p.focus) + "}";
  }
  json += "],";
  json += "\"activePreset\":" + String(activePreset()) + ",";
  json += "\"focus\":" + String(kmFocus) + ",";
  json += "\"outputs\":{";
  json += "\"1\":" + String(state.output1) + ",";
  json += "\"2\":" + String(state.output2) + ",";
  json += "\"3\":" + String(state.output3);
  json += "}}";
  return json;
}

static void sendJsonOk(const char* action) {
  String json = String("{\"ok\":true,\"action\":\"") + action + "\",\"state\":" + stateJson() + "}";
  server.send(200, "application/json", json);
}

static void sendJsonError(int code, const String& error) {
  server.send(code, "application/json", "{\"ok\":false,\"error\":\"" + error + "\"}");
}

static void handleStatus() {
  server.send(200, "application/json", "{\"ok\":true,\"state\":" + stateJson() + "}");
}

// path is the URI with the /api or /ui prefix removed: /preset/<1-n>
static void handlePresetApply(const char* path) {
  int n = 0;
  int consumed = 0;
  if (sscanf(path, "/preset/%d%n", &n, &consumed) != 1 || path[consumed] != '\0' ||
      n < 1 || n > (int)presetCount) {
    sendJsonError(404, "no such preset");
    return;
  }
  String error = applyPreset(presets[n - 1]);
  if (error.length()) {
    sendJsonError(409, "outputs switched, but " + error);
    return;
  }
  sendJsonOk(("preset-" + String(n)).c_str());
}

// Saves the controller name from form field title. Blank resets to the default.
static void saveTitleFromArgs() {
  String title = cleanName(server.arg("title"), TITLE_MAX_LEN);
  deviceTitle = title.length() ? title : DEFAULT_TITLE;
  prefs.putString("title", deviceTitle);
}

// Saves PC names from form fields pc1..pc4. Blank resets to the default name.
static void savePcNamesFromArgs() {
  for (uint8_t i = 0; i < NUM_PCS; i++) {
    String n = String(i + 1);
    String name = cleanName(server.arg("pc" + n));
    pcNames[i] = name.length() ? name : "PC" + n;
    prefs.putString(("pc" + n + "name").c_str(), pcNames[i]);
  }
}

// Reads a preset from form fields <prefix>name, <prefix>o1..o3 and <prefix>focus. Returns an error message, or "" if valid.
static String presetFromArgs(const String& prefix, Preset& out) {
  String name = cleanName(server.arg(prefix + "name"));
  if (!name.length()) return "preset needs a name";
  bool any = false;
  for (int o = 0; o < 3; o++) {
    int v = server.arg(prefix + "o" + String(o + 1)).toInt();
    if (v < 0 || v > NUM_PCS) return "outputs must be 0 (unchanged) or a PC number 1-4";
    out.outputs[o] = v;
    any |= v != 0;
  }
  int focus = server.arg(prefix + "focus").toInt();
  if (focus < 0 || focus > NUM_PCS) return "keyboard and mouse must be 0 (unchanged) or a PC number 1-4";
  if (focus && out.outputs[0] && out.outputs[1] && out.outputs[2] && out.outputs[0] != focus &&
      out.outputs[1] != focus && out.outputs[2] != focus) {
    return "keyboard and mouse must be on a PC this preset puts on an output";
  }
  out.focus = focus;
  if (!any && !focus) return "preset must route at least one output or the keyboard and mouse";
  strlcpy(out.name, name.c_str(), sizeof(out.name));
  return "";
}

static void applyBaud(uint32_t baud) {
  kvmBaud = baud;
  KvmSerial.updateBaudRate(kvmBaud);
  prefs.putUInt("baud", kvmBaud);
}

// POST title=<name>. Blank resets to the default.
static void handleSaveTitle() {
  saveTitleFromArgs();
  sendJsonOk("save-title");
}

// POST pc1=<name>...pc4=<name>. Blank resets to the default name.
static void handleSavePcNames() {
  savePcNamesFromArgs();
  sendJsonOk("save-pc-names");
}

// POST index=<1-n, or 0 for new>&name=...&o1=&o2=&o3=&focus= (0 unchanged, 1-4 PC number)
static void handleSavePreset() {
  int index = server.arg("index").toInt();
  Preset p;
  String error = presetFromArgs("", p);
  if (error.length()) {
    sendJsonError(400, error);
    return;
  }
  if (index < 0 || index > (int)presetCount) {
    sendJsonError(404, "no such preset");
    return;
  }
  if (index == 0) {
    if (presetCount == MAX_PRESETS) {
      sendJsonError(400, "preset limit reached (" + String(MAX_PRESETS) + ")");
      return;
    }
    index = ++presetCount;
  }
  presets[index - 1] = p;
  savePresets();
  sendJsonOk("save-preset");
}

static String companionJson() {
  return "{\"host\":" + jsonString(companion.host) + ",\"port\":" + String(companion.port) +
         ",\"prefix\":" + jsonString(companion.prefix) + "}";
}

// GET: Companion settings and the result of the last push.
static void handleCompanionStatus() {
  server.send(200, "application/json",
              "{\"ok\":true,\"companion\":" + companionJson() + ",\"result\":" + jsonString(companionResult) + "}");
}

// POST companionHost=&companionPort=&companionPrefix=. A blank host turns feedback off.
static void handleSaveCompanion() {
  CompanionConfig c;
  String error = companionFromArgs(c);
  if (error.length()) {
    sendJsonError(400, error);
    return;
  }
  saveCompanion(c);
  handleCompanionStatus();
}

// POST: send every variable now and report the result.
static void handleCompanionTest() {
  if (!companion.host.length()) {
    sendJsonError(400, "set a Companion host first");
    return;
  }
  resetCompanionSync();
  if (syncCompanion(true)) companionSyncedAt = millis();
  else companionRetryAt = millis() + COMPANION_RETRY_MS;
  handleCompanionStatus();
}

// GET: settings as a downloadable JSON file. Credentials are compiled in and never included.
static void handleExportConfig() {
  String json = "{\"type\":\"tesmart-esp32-controller-settings\",\"format\":1,";
  json += "\"firmware\":\"" + String(FW_VERSION) + "\",\"baud\":" + String(kvmBaud) + ",";
  json += "\"title\":" + jsonString(deviceTitle) + ",\"companion\":" + companionJson() + ",\"pcs\":[";
  for (uint8_t i = 0; i < NUM_PCS; i++) json += (i ? "," : "") + jsonString(pcNames[i]);
  json += "],\"presets\":[";
  for (size_t i = 0; i < presetCount; i++) {
    const Preset& p = presets[i];
    if (i) json += ',';
    json += "{\"name\":" + jsonString(p.name) + ",\"outputs\":[" + String(p.outputs[0]) + "," +
            String(p.outputs[1]) + "," + String(p.outputs[2]) + "],\"focus\":" + String(p.focus) + "}";
  }
  json += "]}";
  server.sendHeader("Content-Disposition", String("attachment; filename=\"") + HOSTNAME + "-settings.json\"");
  server.send(200, "application/json", json);
}

// POST baud=&title=&pc1..pc4=&companionHost=&companionPort=&companionPrefix=&presets=<count>&p1name=&p1o1=..p1o3=&p1focus=&p2name=...
// The page parses the exported JSON and sends it as form fields. Everything is validated before anything is saved.
static void handleImportConfig() {
  uint32_t baud = server.arg("baud").toInt();
  if (!supportedBaud(baud)) {
    sendJsonError(400, "unsupported baud rate");
    return;
  }
  int count = server.arg("presets").toInt();
  if (count < 0 || count > (int)MAX_PRESETS) {
    sendJsonError(400, "too many presets (max " + String(MAX_PRESETS) + ")");
    return;
  }
  CompanionConfig importedCompanion;
  String companionError = companionFromArgs(importedCompanion);
  if (companionError.length()) {
    sendJsonError(400, companionError);
    return;
  }
  Preset imported[MAX_PRESETS];
  for (int i = 0; i < count; i++) {
    String error = presetFromArgs("p" + String(i + 1), imported[i]);
    if (error.length()) {
      sendJsonError(400, "preset " + String(i + 1) + ": " + error);
      return;
    }
  }

  saveTitleFromArgs();
  savePcNamesFromArgs();
  memcpy(presets, imported, sizeof(Preset) * count);
  presetCount = count;
  savePresets();
  applyBaud(baud);
  saveCompanion(importedCompanion);
  sendJsonOk("import");
}

// POST: erase every saved setting (title, names, presets, baud, Companion, routing state) and restart.
// Wi-Fi, the API token and the web login are compiled in and unaffected.
static void handleFactoryReset() {
  prefs.clear();
  server.send(200, "application/json", "{\"ok\":true,\"action\":\"factory-reset\"}");
  Serial.println("Factory reset, restarting");
  delay(500);
  ESP.restart();
}

// POST index=<1-n>
static void handleDeletePreset() {
  int index = server.arg("index").toInt();
  if (index < 1 || index > (int)presetCount) {
    sendJsonError(404, "no such preset");
    return;
  }
  for (size_t i = index - 1; i + 1 < presetCount; i++) presets[i] = presets[i + 1];
  presetCount--;
  savePresets();
  sendJsonOk("delete-preset");
}

// path is the URI with the /api or /ui prefix removed: /all/pc<1-4>
static void handleAllRoute(const char* path) {
  int pc = 0;
  int consumed = 0;
  if (sscanf(path, "/all/pc%d%n", &pc, &consumed) != 1 || path[consumed] != '\0' ||
      pc < 1 || pc > NUM_PCS) {
    sendJsonError(400, "bad route");
    return;
  }
  sendKvm(KVM_SWITCH, 0x00, pc - 1);
  state = {(uint8_t)pc, (uint8_t)pc, (uint8_t)pc};
  kmFocus = pc; // every other PC just left the monitors, so the keyboard and mouse follow
  persistState();
  sendJsonOk(("all-pc" + String(pc)).c_str());
}

// path is the URI with the /api or /ui prefix removed: /output/<1-3>/pc/<1-4>
static void handleOutputRoute(const char* path) {
  int output = 0;
  int pc = 0;
  int consumed = 0;
  if (sscanf(path, "/output/%d/pc/%d%n", &output, &pc, &consumed) != 2 ||
      path[consumed] != '\0' ||
      !routeOutput((uint8_t)output, (uint8_t)pc)) {
    sendJsonError(400, "bad route");
    return;
  }
  sendJsonOk("route-output");
}

// path is the URI with the /api or /ui prefix removed: /focus/pc<1-4>
static void handleFocusRoute(const char* path) {
  int pc = 0;
  int consumed = 0;
  if (sscanf(path, "/focus/pc%d%n", &pc, &consumed) != 1 || path[consumed] != '\0' ||
      pc < 1 || pc > NUM_PCS) {
    sendJsonError(400, "bad route");
    return;
  }
  String error = moveFocus(pc);
  if (error.length()) {
    sendJsonError(409, error);
    return;
  }
  sendJsonOk(("focus-pc" + String(pc)).c_str());
}

static void handleQuery() {
  String hex = queryKvm(KVM_ROUTE_REPORT);
  String focusHex = queryKvm(KVM_FOCUS_REPORT);
  if (hex.length() && focusHex.length()) hex += ' ';
  hex += focusHex;
  server.send(200, "application/json", "{\"ok\":true,\"raw\":\"" + hex + "\"}");
}

// POST hex=<bytes>, e.g. "AA BB 03 01 00". Spaces, commas, colons and dashes are ignored.
// With checksum=1, the TESmart checksum byte is appended.
static void handleRawSend() {
  String in = server.arg("hex");
  uint8_t buf[RAW_MAX_BYTES];
  size_t len = 0;
  int nibbles = 0;
  for (char c : in) {
    if (c == ' ' || c == ',' || c == ':' || c == '-') continue;
    if (!isxdigit((unsigned char)c) || len == RAW_MAX_BYTES) {
      sendJsonError(400, "hex must be up to 64 bytes of hex digits, e.g. AA BB 03 01 00 EE");
      return;
    }
    uint8_t v = isdigit((unsigned char)c) ? c - '0' : (toupper(c) - 'A' + 10);
    if (nibbles % 2 == 0) buf[len] = v << 4;
    else buf[len++] |= v;
    nibbles++;
  }
  if (nibbles == 0 || nibbles % 2 != 0) {
    sendJsonError(400, "hex must be whole bytes, e.g. AA BB 03 01 00 EE");
    return;
  }
  if (server.arg("checksum") == "1") {
    if (len == RAW_MAX_BYTES) {
      sendJsonError(400, "no room for the checksum byte");
      return;
    }
    buf[len] = kvmChecksum(buf, len);
    len++;
  }

  String reply = sendAndReadReply(buf, len);
  server.send(200, "application/json",
              "{\"ok\":true,\"action\":\"raw\",\"sent\":\"" + toHex(buf, len) + "\",\"reply\":\"" + reply + "\"}");
}

// POST baud=<rate>. Applied immediately and saved.
static void handleSetBaud() {
  uint32_t baud = server.arg("baud").toInt();
  if (!supportedBaud(baud)) {
    sendJsonError(400, "unsupported baud rate");
    return;
  }
  applyBaud(baud);
  server.send(200, "application/json", "{\"ok\":true,\"action\":\"set-baud\",\"baud\":" + String(kvmBaud) + "}");
}

// GET ?since=<id>: log entries newer than id, oldest first.
static void handleSerialLog() {
  pollSerial();
  uint32_t since = server.arg("since").toInt();
  uint32_t first = nextLogId > LOG_SIZE ? nextLogId - LOG_SIZE : 1;
  if (since + 1 > first) first = since + 1;

  String json = "{\"ok\":true,\"baud\":" + String(kvmBaud) + ",\"entries\":[";
  for (uint32_t id = first; id < nextLogId; id++) {
    const LogEntry& e = serialLog[id % LOG_SIZE];
    if (id != first) json += ',';
    json += "{\"id\":" + String(e.id) + ",\"t\":" + String(e.ms) + ",\"dir\":\"" + (e.tx ? "TX" : "RX") +
            "\",\"hex\":\"" + e.hex + "\"}";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

// Firmware upload. Auth is checked when the upload starts so nothing is flashed without a login.
static bool updateAllowed = false;

static void handleUpdateUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    updateAllowed = uiLoggedIn() && uiRequestAllowed();
    if (updateAllowed) {
      Serial.printf("Firmware update: %s\n", up.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
    }
  } else if (!updateAllowed) {
    return;
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_END) {
    if (!Update.end(true)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
  }
}

static void handleUpdateDone() {
  if (!requireUiAuth()) return;
  if (!updateAllowed || Update.hasError() || !Update.isFinished()) {
    sendJsonError(500, String("update failed: ") + Update.errorString());
    return;
  }
  server.send(200, "application/json", "{\"ok\":true,\"action\":\"update\"}");
  Serial.println("Firmware update installed, restarting");
  delay(500);
  ESP.restart();
}

static void serveAsset(const char* contentType, const char* body) {
  if (requireUiAuth()) server.send_P(200, contentType, body);
}

// Registers handler at /api<path> (token required) and /ui<path> (UI login, for the built-in pages).
static void addRoute(const char* path, HTTPMethod method, void (*handler)()) {
  server.on(String("/api") + path, method, [handler](){ if (requireAuth()) handler(); });
  server.on(String("/ui") + path, method, [handler](){ if (requireUiAuth()) handler(); });
}

// Registers handler at /ui<path> only. Used for the Settings page's tools.
static void addUiRoute(const char* path, HTTPMethod method, void (*handler)()) {
  server.on(String("/ui") + path, method, [handler](){ if (requireUiAuth()) handler(); });
}

static void setupRoutes() {
  const char* headerKeys[] = {"Authorization", "X-Requested-With"};
  server.collectHeaders(headerKeys, 2);

  server.on("/", HTTP_GET, [](){ serveAsset("text/html", INDEX_HTML); });
  server.on("/settings", HTTP_GET, [](){ serveAsset("text/html", SETTINGS_HTML); });
  server.on("/advanced", HTTP_GET, [](){ // old address for the Settings page
    server.sendHeader("Location", "/settings");
    server.send(301);
  });
  server.on("/app.css", HTTP_GET, [](){ serveAsset("text/css", APP_CSS); });
  server.on("/app.js", HTTP_GET, [](){ serveAsset("application/javascript", APP_JS); });
  server.on("/health", HTTP_GET, [](){ server.send(200, "application/json", "{\"ok\":true}"); });

  addRoute("/status", HTTP_GET, handleStatus);
  addRoute("/query", HTTP_POST, handleQuery);

  addUiRoute("/config/title", HTTP_POST, handleSaveTitle);
  addUiRoute("/config/pcs", HTTP_POST, handleSavePcNames);
  addUiRoute("/config/preset/save", HTTP_POST, handleSavePreset);
  addUiRoute("/config/preset/delete", HTTP_POST, handleDeletePreset);
  addUiRoute("/config/companion", HTTP_POST, handleSaveCompanion);
  addUiRoute("/companion", HTTP_GET, handleCompanionStatus);
  addUiRoute("/companion/test", HTTP_POST, handleCompanionTest);
  addUiRoute("/config/export", HTTP_GET, handleExportConfig);
  addUiRoute("/config/import", HTTP_POST, handleImportConfig);
  addUiRoute("/config/reset", HTTP_POST, handleFactoryReset);
  addUiRoute("/serial/send", HTTP_POST, handleRawSend);
  addUiRoute("/serial/baud", HTTP_POST, handleSetBaud);
  addUiRoute("/serial/log", HTTP_GET, handleSerialLog);
  server.on("/ui/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);

  // Routes with path parameters (/output/<n>/pc/<n>, /all/pc<n>, /focus/pc<n>, /preset/<n>) are dispatched here.
  server.onNotFound([](){
    const String& uri = server.uri();
    bool api = uri.startsWith("/api/");
    const char* path = api ? uri.c_str() + strlen("/api")
                     : uri.startsWith("/ui/") ? uri.c_str() + strlen("/ui") : nullptr;
    if (server.method() == HTTP_POST && path) {
      bool output = strncmp(path, "/output/", 8) == 0;
      bool all = strncmp(path, "/all/", 5) == 0;
      bool focus = strncmp(path, "/focus/", 7) == 0;
      bool preset = strncmp(path, "/preset/", 8) == 0;
      if (output || all || focus || preset) {
        if (!(api ? requireAuth() : requireUiAuth())) return;
        if (output) handleOutputRoute(path);
        else if (all) handleAllRoute(path);
        else if (focus) handleFocusRoute(path);
        else handlePresetApply(path);
        return;
      }
    }
    sendJsonError(404, "not found");
  });
}

// ---------------------------
// Wi-Fi
// ---------------------------
static void startMdns() {
  MDNS.end();
  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", HTTP_PORT);
    Serial.printf("mDNS: http://%s.local/\n", HOSTNAME);
  }
}

static void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("Connecting to %s", WIFI_SSID);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println("\nWiFi connect timed out, restarting");
      ESP.restart();
    }
    delay(500);
    Serial.print('.');
  }
  Serial.printf("\nWiFi connected: %s\n", WiFi.localIP().toString().c_str());
  wifiWasConnected = true;

  startMdns();
}

// Auto-reconnect restores the link, but mDNS needs restarting to re-announce.
static void checkWifi() {
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wifiWasConnected) {
    Serial.printf("WiFi reconnected: %s\n", WiFi.localIP().toString().c_str());
    startMdns();
  } else if (!connected && wifiWasConnected) {
    Serial.println("WiFi lost, waiting for reconnect");
  }
  wifiWasConnected = connected;
}

void setup() {
  Serial.begin(115200);
  delay(250);

  prefs.begin("tesmart", false);
  loadState();
  loadConfig();
  resetCompanionSync();
  kvmBaud = prefs.getUInt("baud", DEFAULT_KVM_BAUD);
  if (!supportedBaud(kvmBaud)) kvmBaud = DEFAULT_KVM_BAUD;

  KvmSerial.begin(kvmBaud, SERIAL_8N1, KVM_RX_PIN, KVM_TX_PIN);

  connectWifi();
  setupRoutes();
  server.begin();

  Serial.printf("TESmart ESP32 controller v%s (%s) ready, KVM serial %lu baud\n",
                FW_VERSION, FW_BUILD, (unsigned long)kvmBaud);
}

void loop() {
  checkWifi();
  pollSerial();
  pollKvmState();
  server.handleClient();
  companionLoop();
  delay(2);
}
