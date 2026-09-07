/*
 * signal — firmware (M2 + M3 + M4)
 * ESP32-C3 SuperMini + WS2812 8-bit stick
 *
 * Libraries required (Library Manager):
 *   Adafruit NeoPixel
 *   ArduinoJson   (v7.x)
 *
 * Endpoints:
 *   POST /state      hook target. Body is Claude Code's event JSON. Returns 204, empty.
 *   POST /heartbeat  from the Mac launchd agent. Returns 204, empty.
 *   GET  /health     human-readable status dump. Not a hook target.
 *
 * The 204-with-empty-body contract on /state is load-bearing. A 2xx with a
 * text body puts a hook error notice in the Claude Code transcript every turn.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>

// ---------------- config ----------------
const char* WIFI_SSID = "<your 2.4G wifi name>";   // 2.4GHz band — the C3 has no 5GHz radio
const char* WIFI_PASS = "<your wifi password>";
const char* MDNS_NAME = "signal";              // -> http://signal.local
// Letters and digits ONLY. A backslash in this string is an invalid C++ escape
// sequence — the compiler strips it, so the ESP holds a different string than the
// one you typed, while bash sends the backslash intact. Result: permanent 401
// with nothing in the logs to explain it. Set to "" to disable the check.
const char* AUTH_TOKEN = "k3Jm9pQx7Zt2Vw5B";   // must match $SIGNAL_TOKEN on the Mac

#define LED_PIN     2
#define LED_COUNT   8
#define BRIGHTNESS  180

const uint32_t SESSION_TTL_MS   = 900000;  // 15 min. Must exceed your longest task:
                                           // no event fires while Claude works, so a
                                           // short TTL would expire mid-task.
const uint32_t DONE_HOLD_MS     = 8000;    // how long green shows before going idle
const uint32_t HEARTBEAT_TTL_MS = 60000;   // no Mac ping for this long -> disconnected

// --- animation timing ---
const uint32_t BLINK_ON_MS      = 200;     // blink-thrice intro on both blocked and done
const uint32_t BLINK_OFF_MS     = 200;
const uint8_t  BLINK_COUNT      = 3;
const uint32_t BLINK_TOTAL_MS   = BLINK_COUNT * (BLINK_ON_MS + BLINK_OFF_MS);
const uint32_t CHASE_STEP_MS    = 70;      // lower = faster running light
const uint32_t ESCALATE_MS      = 15000;   // blocked this long -> disco
const uint32_t RENDER_INTERVAL  = 20;      // ~50fps. show() masks interrupts, so
                                           // rendering flat-out would disturb WiFi.
const uint32_t WIFI_CONNECT_MS  = 20000;  // how long one attempt waits
const uint32_t WIFI_RETRY_MS    = 5000;   // gap between attempts

// ---------------- colours ----------------
struct Rgb { uint8_t r, g, b; };

const Rgb C_WORKING      = {255,  70,   0};
const Rgb C_BLOCKED      = {255,   0,   0};
const Rgb C_DONE         = {  0, 200,   0};
const Rgb C_API_ERROR    = {160,   0, 200};
const Rgb C_DISCONNECTED = {  0,  40, 120};
const Rgb C_OFF          = {  0,   0,   0};

// ---------------- state ----------------
enum State : uint8_t { ST_IDLE = 0, ST_DONE, ST_WORKING, ST_API_ERROR, ST_BLOCKED };
// Order matters: higher enum value wins during priority resolution.

struct Session {
  char     id[40];
  State    state;
  uint32_t lastSeen;
  bool     used;
};

Session sessions[LED_COUNT];
uint32_t lastHeartbeat = 0;

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
WebServer server(80);

// ---------------- session table ----------------
int findSession(const char* id) {
  for (int i = 0; i < LED_COUNT; i++)
    if (sessions[i].used && strcmp(sessions[i].id, id) == 0) return i;
  return -1;
}

int claimSlot(const char* id) {
  for (int i = 0; i < LED_COUNT; i++) {
    if (!sessions[i].used) {
      strncpy(sessions[i].id, id, sizeof(sessions[i].id) - 1);
      sessions[i].id[sizeof(sessions[i].id) - 1] = '\0';
      sessions[i].used = true;
      sessions[i].state = ST_IDLE;
      sessions[i].lastSeen = millis();
      return i;
    }
  }
  // Table full: evict the stalest entry rather than dropping the new session.
  int oldest = 0;
  for (int i = 1; i < LED_COUNT; i++)
    if (sessions[i].lastSeen < sessions[oldest].lastSeen) oldest = i;
  sessions[oldest].used = false;
  return claimSlot(id);
}

void releaseSession(const char* id) {
  int i = findSession(id);
  if (i >= 0) sessions[i].used = false;
}

void touchSession(const char* id, State s) {
  int i = findSession(id);
  if (i < 0) i = claimSlot(id);
  sessions[i].state = s;
  sessions[i].lastSeen = millis();
}

void expireSessions() {
  uint32_t now = millis();
  for (int i = 0; i < LED_COUNT; i++) {
    if (!sessions[i].used) continue;
    uint32_t age = now - sessions[i].lastSeen;

    // A finished session clears fast so the light returns to idle promptly.
    // Everything else gets the long TTL — it only exists to catch sessions
    // that died without firing SessionEnd.
    uint32_t ttl = (sessions[i].state == ST_DONE) ? DONE_HOLD_MS : SESSION_TTL_MS;
    if (age > ttl) sessions[i].used = false;
  }
}

// ---------------- rendering ----------------
bool isDisconnected() {
  if (WiFi.status() != WL_CONNECTED) return true;
  if (lastHeartbeat == 0) return false;   // no heartbeat configured yet, don't cry wolf
  return millis() - lastHeartbeat > HEARTBEAT_TTL_MS;
}

State resolveState(int* blockedCount) {
  State winner = ST_IDLE;
  *blockedCount = 0;
  for (int i = 0; i < LED_COUNT; i++) {
    if (!sessions[i].used) continue;
    if (sessions[i].state > winner) winner = sessions[i].state;
    if (sessions[i].state == ST_BLOCKED) (*blockedCount)++;
  }
  return winner;
}

Rgb colorFor(State s) {
  switch (s) {
    case ST_WORKING:   return C_WORKING;
    case ST_BLOCKED:   return C_BLOCKED;
    case ST_DONE:      return C_DONE;
    case ST_API_ERROR: return C_API_ERROR;
    default:           return C_OFF;
  }
}

Rgb scale(Rgb c, float f) {
  if (f < 0) f = 0;
  if (f > 1) f = 1;
  return { (uint8_t)(c.r * f), (uint8_t)(c.g * f), (uint8_t)(c.b * f) };
}

void fill(Rgb c) {
  for (int i = 0; i < LED_COUNT; i++) strip.setPixelColor(i, c.r, c.g, c.b);
  strip.show();
}

// Blink the whole stick N times, then hold. Used as the intro on both the
// blocked and done states so a transition is impossible to miss.
void renderBlink(uint32_t age, Rgb c) {
  uint32_t period = BLINK_ON_MS + BLINK_OFF_MS;
  bool on = (age % period) < BLINK_ON_MS;
  fill(on ? c : C_OFF);
}

// A comet running along the stick with a fading tail — reads as "in progress"
// at a glance, unlike a static colour.
void renderChase(uint32_t age, Rgb c) {
  int head = (age / CHASE_STEP_MS) % LED_COUNT;
  for (int i = 0; i < LED_COUNT; i++) {
    int behind = (head - i + LED_COUNT) % LED_COUNT;
    float f;
    switch (behind) {
      case 0:  f = 1.00f; break;
      case 1:  f = 0.45f; break;
      case 2:  f = 0.18f; break;
      case 3:  f = 0.07f; break;
      default: f = 0.0f;  break;   // fully dark — the comet runs through black
    }
    Rgb p = scale(c, f);
    strip.setPixelColor(i, p.r, p.g, p.b);
  }
  strip.show();
}

// How many sessions are blocked, encoded as pulses: N flashes, pause, repeat.
// Runs AFTER the three-flash intro has finished, so the two don't collide —
// the intro says "something changed", this says "how many are waiting".
// A single blocked session holds solid instead, since one pulse forever is
// worse to look at than a steady light.
void renderBlockedCount(uint32_t t, int count, Rgb c) {
  const uint32_t ON = 250, OFF = 250, GAP = 1200;
  uint32_t cycle = count * (ON + OFF) + GAP;
  uint32_t p = t % cycle;
  bool on = (p < count * (ON + OFF)) && ((p % (ON + OFF)) < ON);
  fill(on ? c : C_OFF);
}

// Escalation for a block you have not answered. Deliberately obnoxious.
void renderDisco(uint32_t age) {
  for (int i = 0; i < LED_COUNT; i++) {
    uint16_t hue = (uint16_t)((age * 45) + (i * 8192));
    strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(hue, 255, 255)));
  }
  strip.show();
}

// Slow breathing blue. Not a status colour — it means "my data is stale".
void renderDisconnected() {
  float phase = (millis() % 3000) / 3000.0f;
  float lvl = 0.20f + 0.60f * (0.5f - 0.5f * cos(phase * 2 * PI));
  fill(scale(C_DISCONNECTED, lvl));
}

State    lastState  = ST_IDLE;
uint32_t stateSince = 0;
bool     wasDisconnected = false;

void render() {
  if (isDisconnected()) {
    renderDisconnected();
    wasDisconnected = true;
    return;
  }

  // Coming back from disconnected restarts whatever animation is current,
  // so you get the blink intro rather than joining it mid-cycle.
  if (wasDisconnected) {
    wasDisconnected = false;
    stateSince = millis();
  }

  int blocked = 0;
  State s = resolveState(&blocked);

  // Restart the animation clock on a state change, and also when the blocked
  // count changes — otherwise a second session joining mid-pulse would leave
  // you counting a half-finished pattern.
  static int lastBlocked = 0;
  if (s != lastState || (s == ST_BLOCKED && blocked != lastBlocked)) {
    lastState   = s;
    lastBlocked = blocked;
    stateSince  = millis();
  }
  uint32_t age = millis() - stateSince;

  switch (s) {
    case ST_WORKING:
      renderChase(age, C_WORKING);
      break;

    case ST_BLOCKED:
      // Three phases, in order: attention intro, then how-many, then escalate.
      if (age < BLINK_TOTAL_MS) {
        renderBlink(age, C_BLOCKED);
      } else if (age >= ESCALATE_MS) {
        renderDisco(age);
      } else if (blocked > 1) {
        renderBlockedCount(age - BLINK_TOTAL_MS, blocked, C_BLOCKED);
      } else {
        fill(C_BLOCKED);
      }
      break;

    case ST_DONE:
      if (age < BLINK_TOTAL_MS)      renderBlink(age, C_DONE);
      else                           fill(C_DONE);
      break;

    case ST_API_ERROR:
      if (age < BLINK_TOTAL_MS)      renderBlink(age, C_API_ERROR);
      else                           fill(C_API_ERROR);
      break;

    default:
      fill(C_OFF);
      break;
  }
}

// ---------------- HTTP ----------------
bool authorized() {
  if (strlen(AUTH_TOKEN) == 0) return true;
  return server.header("X-Signal-Token") == AUTH_TOKEN;
}

// Every hook reply goes through here. Empty body, always.
void reply204() { server.send(204, "", ""); }

// Recover the raw request body regardless of Content-Type.
//
// ESP32's WebServer only populates arg("plain") when the Content-Type is NOT
// application/x-www-form-urlencoded. With form encoding it parses the body into
// key=value pairs instead — and a JSON body has no '=', so the entire JSON
// string ends up as argName(0) with an empty value.
//
// Claude Code always sends application/json, so this only matters for hand-rolled
// curl tests. But a request that silently does nothing is the worst thing to
// debug, so handle both shapes.
String requestBody() {
  String body = server.arg("plain");
  if (body.length() > 0) return body;

  for (int i = 0; i < server.args(); i++) {
    String name = server.argName(i);
    if (name.startsWith("{")) {
      // Form parser split the JSON on '='. Stitch it back together.
      String v = server.arg(i);
      return v.length() ? name + "=" + v : name;
    }
  }
  return "";
}

void handleState() {
  if (!authorized()) {
    Serial.println("REJECTED  401 - token mismatch");
    server.send(401, "", "");
    return;
  }

  String body = requestBody();
  if (body.length() == 0) {
    Serial.println("IGNORED   empty body");
    reply204();
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    // Never surface a parse failure to Claude Code — a non-2xx or a response
    // body would put a hook error in the transcript. Log it here instead.
    Serial.printf("IGNORED   bad JSON (%s): %.60s\n", err.c_str(), body.c_str());
    reply204();
    return;
  }

  const char* sid   = doc["session_id"]  | "";
  const char* event = doc["hook_event_name"] | "";
  if (strlen(sid) == 0 || strlen(event) == 0) {
    Serial.printf("IGNORED   missing fields: %.60s\n", body.c_str());
    reply204();
    return;
  }

  if      (!strcmp(event, "SessionStart"))     touchSession(sid, ST_IDLE);
  else if (!strcmp(event, "UserPromptSubmit")) touchSession(sid, ST_WORKING);
  // PostToolUse is what CLEARS a block. Answering a permission prompt fires no
  // hook of its own, so without this the session stays BLOCKED — and keeps
  // escalating to disco — for the rest of the turn, until Stop finally arrives.
  //
  // Safe to fire on every tool call: touchSession with an unchanged state is
  // idempotent, the animation clock only restarts on a state change, and the
  // refreshed lastSeen keeps long tasks clear of the TTL.
  //
  // PreToolUse is deliberately NOT handled — it fires before the permission
  // prompt, so Notification immediately overwrites it. All it would add is a
  // brief comet flash before the red.
  else if (!strcmp(event, "PostToolUse")) {
    // Do NOT resurrect a finished turn. Command hooks run with async: true, so
    // they fire in parallel with no ordering guarantee — on a short task
    // PostToolUse can land AFTER Stop and overwrite DONE with WORKING, leaving
    // the session stuck amber until the TTL expires.
    //
    // ST_DONE is terminal for the turn. Only UserPromptSubmit or SessionStart
    // (i.e. a genuinely new turn) may move a session out of it.
    int i = findSession(sid);
    if (i >= 0 && sessions[i].state == ST_DONE) {
      Serial.printf("IGNORED   PostToolUse after Stop %.8s\n", sid);
      reply204();
      return;
    }
    touchSession(sid, ST_WORKING);
  }
  else if (!strcmp(event, "Notification"))     touchSession(sid, ST_BLOCKED);
  else if (!strcmp(event, "Stop"))             touchSession(sid, ST_DONE);
  else if (!strcmp(event, "StopFailure"))      touchSession(sid, ST_API_ERROR);
  else if (!strcmp(event, "SessionEnd"))       releaseSession(sid);
  else {
    Serial.printf("IGNORED   unknown event '%s'\n", event);
    reply204();
    return;
  }

  Serial.printf("%-18s %.8s\n", event, sid);
  reply204();
}

void handleHeartbeat() {
  if (!authorized()) { server.send(401, "", ""); return; }
  lastHeartbeat = millis();
  reply204();
}

void handleHealth() {
  String out = "signal\n";
  out += "ip: " + WiFi.localIP().toString() + "\n";
  out += "rssi: " + String(WiFi.RSSI()) + "\n";
  out += "uptime_s: " + String(millis() / 1000) + "\n";
  // Note: don't mix an unsigned expression and -1 in one ternary here — the -1
  // gets promoted to unsigned and prints as 4294967295.
  out += "heartbeat_age_s: ";
  out += lastHeartbeat ? String((millis() - lastHeartbeat) / 1000) : String("none");
  out += "\n";
  out += "disconnected: " + String(isDisconnected() ? "yes" : "no") + "\n";
  out += "sessions:\n";
  const char* names[] = {"idle", "done", "working", "api_error", "blocked"};
  for (int i = 0; i < LED_COUNT; i++) {
    if (!sessions[i].used) continue;
    out += "  [" + String(i) + "] " + String(sessions[i].id).substring(0, 8)
         + " " + names[sessions[i].state]
         + " age=" + String((millis() - sessions[i].lastSeen) / 1000) + "s\n";
  }
  server.send(200, "text/plain", out);
}

void connectWifi() {
  // Tear down any in-flight attempt first. Calling begin() while the radio is
  // already connecting throws "sta is connecting, cannot set config" and the
  // new attempt is silently discarded.
  WiFi.disconnect(true);
  delay(100);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                 // sleep adds latency and drops mDNS replies
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("wifi");
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_MS) {
    render();                           // keep the light alive while connecting
    delay(200);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("ip: %s\n", WiFi.localIP().toString().c_str());
    if (MDNS.begin(MDNS_NAME)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("mdns: http://%s.local\n", MDNS_NAME);
    }
  } else {
    // status 1 = NO_SSID_AVAIL (wrong name, or a 5GHz-only network)
    // status 4 = CONNECT_FAILED (wrong password, usually)
    // status 6 = DISCONNECTED  (association rejected or out of range)
    Serial.printf("wifi failed, status=%d, will retry\n", WiFi.status());
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  strip.begin();
  strip.setBrightness(BRIGHTNESS);   // set once; animations scale RGB directly
  strip.clear();
  strip.show();

  for (int i = 0; i < LED_COUNT; i++) sessions[i].used = false;

  connectWifi();

  // WebServer only exposes headers you ask it to collect.
  const char* headers[] = { "X-Signal-Token" };
  server.collectHeaders(headers, 1);

  server.on("/state",     HTTP_POST, handleState);
  server.on("/heartbeat", HTTP_POST, handleHeartbeat);
  server.on("/health",    HTTP_GET,  handleHealth);
  server.onNotFound([]() { server.send(404, "", ""); });
  server.begin();

  Serial.println("ready");
}

uint32_t lastWifiCheck = 0;

uint32_t lastRender = 0;

void loop() {
  server.handleClient();          // polled tightly so hooks land with no lag
  expireSessions();

  if (millis() - lastRender >= RENDER_INTERVAL) {
    lastRender = millis();
    render();
  }

  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiCheck > WIFI_RETRY_MS) {
    lastWifiCheck = millis();
    connectWifi();
  }

  delay(2);
}
