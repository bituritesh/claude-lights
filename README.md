# claude-lights

A physical status light for Claude Code sessions. Amber while Claude works, red when it needs you, green when it's done — and a full-strip rainbow if you leave it blocked for too long.

Built so you can context-switch away from a long-running task without missing the moment Claude stops and waits for a permission prompt.

![status](https://img.shields.io/badge/status-working-brightgreen)

---

## What it does

| State | Colour | Behaviour | Meaning |
|---|---|---|---|
| Working | Amber | Comet running along the strip | Claude is executing, no action needed |
| Blocked | Red | Three flashes, then solid | Awaiting permission or input |
| Blocked (multiple) | Red | Three flashes, then N pulses, pause, repeat | That many sessions are waiting on you |
| **Ignored** | **Rainbow** | **Fast full-strip spin** | **Blocked for 15+ seconds. Go look at it** |
| Done | Green | Three flashes, then solid, off after 8s | Turn complete |
| API error | Purple | Three flashes, then solid | Turn died on rate limit or overload |
| Idle | Off | Dark | No active session |
| Disconnected | Blue | Slow breathing pulse | Lost WiFi, or no heartbeat from the Mac |

Motion carries more information than colour here. A static amber and a static green are easy to confuse across a desk; a running comet versus a steady glow is not.

Handles concurrent sessions — states collapse by priority (blocked beats working beats done), and the number of blocked sessions is encoded as pulse counts.

---

## How it works

Claude Code runs locally, and there's no Anthropic-side API that reports live session state to a device. So state is **pushed from the machine**, not pulled from the cloud.

```
Claude Code hook  ──curl──►  ESP32-C3 HTTP server  ──►  WS2812 strip
   (on your Mac)               (same WiFi)
```

Claude Code's `command` hooks fire on lifecycle events and forward the event JSON straight to the ESP. The device parses `session_id` and `hook_event_name` itself, so there's no `jq` dependency and nothing to keep in sync.

| Hook event | Result |
|---|---|
| `SessionStart` | Register session |
| `UserPromptSubmit` | Amber |
| `PostToolUse` | Amber — **this is what clears a resolved permission prompt** |
| `Notification` | Red |
| `Stop` | Green |
| `StopFailure` | Purple |
| `SessionEnd` | Deregister |

All hooks use `async: true`, so an unreachable device costs nothing — the hook backgrounds itself and your turn proceeds regardless.

---

## Parts

Roughly ₹500 / $6 total.

| Part | Qty |
|---|---|
| ESP32-C3 SuperMini (USB-C onboard) | 1 |
| WS2812B 8-bit RGB stick | 1 |
| 330Ω resistor | 1 |
| 220–470µF electrolytic capacitor (10V+) | 1 |
| 1N4007 diode (only if you see flicker) | 1 |
| Jumper wires, pin headers | — |
| Ping-pong ball or frosted acrylic (diffuser) | 1 |

A bare pixel is an unpleasant point source. The diffuser costs nothing and makes the difference between a gadget and something you can actually read at a glance.

---

## Wiring

```
ESP32-C3                    WS2812B-8 (IN end)
--------                    ------------------
5V   ──────────────────►    VCC
G    ──────────────────►    GND
GPIO2 ──[330Ω]─────────►    IN

220–470µF across VCC/GND at the strip — stripe leg to GND
```

- Wire to the **IN** end, not OUT. Data flows one direction only, and wiring to OUT gives you silence with no other symptom.
- If you see random wrong-coloured pixels, add a 1N4007 in series on the 5V line, banded end toward the strip. That drops VCC to ~4.4V so the ESP's 3.3V data signal registers as a valid high.

---

## Setup

### 1. Firmware

Requires **Adafruit NeoPixel** and **ArduinoJson v7** from Library Manager.

Edit the config block at the top of `signal_firmware.ino`:

```cpp
const char* WIFI_SSID  = "<your 2.4G wifi name>";   // the C3 has no 5GHz radio
const char* WIFI_PASS  = "<your wifi password>";
const char* MDNS_NAME  = "signal";                  // -> http://signal.local
const char* AUTH_TOKEN = "<letters and digits only>";
```

**The token must be alphanumeric.** A backslash is an invalid C++ escape sequence — the compiler strips it while bash keeps it, so the two strings silently differ and every request 401s with nothing in the logs to explain it. Set it to `""` to disable auth entirely.

Board: **ESP32C3 Dev Module**. Set **USB CDC On Boot → Enabled** or Serial prints nothing.

Flash, then check the serial output at 115200 for the assigned IP:

```
wifi....
ip: 192.168.1.x
mdns: http://signal.local
ready
```

### 2. Verify

```bash
export SIGNAL_TOKEN="<same token as the firmware>"

curl --max-time 5 http://signal.local/health

curl -si -X POST http://signal.local/state \
  -H "X-Signal-Token: $SIGNAL_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"session_id":"t1","hook_event_name":"UserPromptSubmit"}' | head -3
```

Want `HTTP/1.1 204` and an amber comet. Always use `-i` when debugging — a 401 returns an empty body too, so without it a rejected request looks exactly like a successful one.

### 3. Hooks

Merge `claudeHookSettings.json` into `~/.claude/settings.json` (global, all projects). Don't overwrite the file if it already has content.

Each hook tries a hardcoded IP first, then falls back to `signal.local`:

```
curl ... http://192.168.1.16/state || curl ... http://signal.local/state || true
```

**Edit that IP to match your device.** IP first is faster when it's correct; mDNS catches DHCP moving the device.

Add to `~/.zshrc`:

```bash
export SIGNAL_TOKEN="<your token>"
```

Restart your Claude Code session — hook config is read at session start — then run `/hooks` to confirm they're all registered.

### 4. Optional: pin the IP

Give the ESP a **DHCP reservation** on your router, binding its MAC to a fixed address. The fallback chain is recovery; the reservation stops the problem happening.

---

## Endpoints

| Endpoint | Purpose |
|---|---|
| `POST /state` | Hook target. Returns 204 with an empty body |
| `POST /heartbeat` | Optional liveness ping from the Mac |
| `GET /health` | Status dump — IP, RSSI, uptime, live session table |

`/health` needs no token and is the first place to look when something's wrong.

**The 204-with-empty-body contract matters.** A 2xx carrying any body — even `OK` — puts a hook error notice in your Claude Code transcript on every single turn.

---

## Files

| File | Contents |
|---|---|
| `signal_firmware.ino` | ESP32 firmware — WiFi, mDNS, HTTP server, session table, animations |
| `claudeHookSettings.json` | Hook config to merge into `~/.claude/settings.json` |
| `signal_mac_setup.md` | Mac-side setup, heartbeat agent, verification, troubleshooting |
| `claude-code-traffic-light-plan.md` | Full design notes, test matrix, and command reference |

---

## Notes for anyone forking this

**Don't commit your WiFi credentials.** Move them to a `secrets.h` that's gitignored:

```cpp
// secrets.h  — add to .gitignore
#define WIFI_SSID_VAL  "..."
#define WIFI_PASS_VAL  "..."
#define AUTH_TOKEN_VAL "..."
```

Git keeps history, so a password committed once and removed later is still readable via `git log -p`. If it happens, rotate the credential — rewriting history doesn't undo publication.

**Two firmware constants are deliberately different and shouldn't be merged.** `SESSION_TTL_MS` is 15 minutes, `DONE_HOLD_MS` is 8 seconds. No event fires while Claude works, so a short global TTL would expire a session mid-task and drop the light to idle. A long TTL on a *finished* session leaves green sitting there. Hence per-state expiry.

**`RENDER_INTERVAL` is throttled to ~50fps on purpose.** `strip.show()` masks interrupts for roughly 240µs per call, and rendering flat-out disturbs WiFi.

---

## Troubleshooting

| Symptom | Cause |
|---|---|
| 401 on every POST | Token mismatch. Check for backslashes first |
| Hooks work in terminal, not VS Code | GUI launches don't source `~/.zshrc`, so `$SIGNAL_TOKEN` is empty |
| Light stays red through a whole turn | `PostToolUse` not hooked — nothing clears a resolved permission prompt |
| Smooth rainbow spinning | Not a fault. Blocked 15s+, escalated. Go answer Claude |
| Random wrong-coloured pixels | Data corruption. Fit the 330Ω resistor, then the 1N4007 |
| Light drops to idle mid-task | `SESSION_TTL_MS` too short |
| curl hangs ~75s then fails | Nothing at that IP. Use `--max-time` |
| Serial monitor silent always | USB CDC On Boot is Disabled |
| Red renders as green | Change `NEO_GRB` to `NEO_RGB` |
| WiFi never connects | ESP32-C3 is 2.4GHz only. Check you're not on a 5GHz SSID |

Fuller reference in `claude-code-traffic-light-plan.md` §11.
