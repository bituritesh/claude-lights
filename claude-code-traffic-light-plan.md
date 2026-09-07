# Claude Code Traffic Light — Project Plan

**Codename:** `signal`
**Owner:** Ritesh
**Target:** Desk-mounted RGB status light reflecting live Claude Code session state
**Estimated effort:** 6–10 hours across 3 sessions
**Revision:** v2 — hook API verified against docs, Mac-side scripting eliminated

---

## 1. Objective

A physical light on the desk that answers one question without switching windows: *does Claude Code need me right now?*

| State | Colour | Behaviour | Meaning | Clears when |
|---|---|---|---|---|
| Working | Amber `(255, 70, 0)` | Comet running along the stick with a fading tail, dark pixels between | Claude is executing, no action needed | `Stop` or `StopFailure` arrives |
| Blocked | Red `(255, 0, 0)` | Three flashes, then **solid** | One session awaiting permission or input | You answer the prompt |
| Blocked, multiple | Red `(255, 0, 0)` | Three flashes, then **N pulses, pause, repeat** — count the pulses | That many sessions are waiting on you | Count drops; pattern restarts at the new number |
| **Ignored** | **Rainbow** | **Full-strip rainbow spinning, fast** | **You have left Claude blocked for 15+ seconds. Go look at it** | **You answer the prompt** |
| Done | Green `(0, 200, 0)` | Three flashes, then solid | Turn complete | Automatically after 8s → Idle |
| API error | Purple `(160, 0, 200)` | Three flashes, then solid | Turn died on rate limit or overload | Next event, or session expiry |
| Idle | Off | Dark | No active session | A hook event arrives |
| Disconnected | Blue `(0, 40, 120)` | Slow breathing pulse, ~3s cycle | ESP lost WiFi, or no Mac heartbeat for 60s | WiFi returns and heartbeat resumes |

### The rainbow means you

It is the only state that is about **your** behaviour rather than Claude's. Every
other colour reports what the machine is doing; the rainbow reports that you have
stopped responding to it.

It fires 15 seconds (`ESCALATE_MS`) after entering blocked, and keeps going until
you answer. It is deliberately unpleasant. If it is going off too eagerly, raise
`ESCALATE_MS` rather than removing it — the whole point of the project is not
missing a blocked session.

Two things to know when you see it:

- **The blocked count disappears once disco starts.** Escalation and information
  can't share eight pixels. `curl http://signal.local/health` still lists every
  session and its state.
- **A rainbow is not always disco.** A smooth, even, deliberate-looking sweep is
  this state. Random pixels flashing wrong colours in no pattern is data
  corruption on the wire — fit the 330Ω resistor, and the 1N4007 if that doesn't
  clear it. See §11.12.

### Why animation, not just colour

Motion matters more than colour here. A static amber and a static green are easy
to confuse at a glance across a desk; a running comet versus a steady glow is not.
Test A-09 is the check that matters: from 2m, can you tell working from done by
motion alone, ignoring colour entirely?

The disconnected state is non-negotiable. Without it, a dropped connection leaves stale green on the light and it silently lies to you.

---

## 2. Architecture

### Key constraint

Claude Code runs locally. There is no Anthropic-side API that reports live session state to a third-party device. State is **pushed from the machine running Claude Code**, not pulled from the cloud. The ESP never authenticates to Anthropic.

### Phase 1 — LAN push (build this)

```
Claude Code hook (type: "http")  ──POST──►  ESP32-C3 HTTP server  ──►  WS2812 stick
```

Claude Code has a native `type: "http"` hook handler. It POSTs the event JSON directly to a URL with `Content-Type: application/json`. **There is no shell script, no curl, no jq, and nothing to install on the Mac.** The ESP32 is the hook endpoint.

This also settles the REST-vs-WebSocket question: the transport is fixed by Claude Code's hook API, and it is HTTP POST.

### Phase 2 — Cloud relay (optional, later)

```
Claude Code hook ──POST──► relay ──MQTT──► ESP32 subscribe ──► LED
```

Only worth building if you want the light visible from a different network. This is where a captive-portal login page becomes relevant — for **WiFi provisioning and device pairing**, not an Anthropic account. Use WiFiManager. Prefer MQTT over raw WebSockets: retained messages mean a reconnecting ESP immediately receives last-known state instead of sitting blank.

---

## 3. Bill of Materials

| Part | Qty | Source | ~₹ |
|---|---|---|---|
| ESP32-C3 SuperMini (USB-C) | 1 | Robocraze / SP Road | 250–400 |
| WS2812 8-bit RGB stick (53×10×2mm) | 1 | Robocraze `TIFEC0168` | 34 |
| 330Ω resistor ¼W | 1 | — | 2 |
| 220µF or 470µF electrolytic (10V+) | 1–2 | Cap kit or SP Road | 5 |
| 1N4007 diode (level-shift insurance) | 2 | — | 4 |
| Female-female Dupont jumpers | 1 set | — | 60 |
| Pin headers | 1 strip | — | 10 |
| Ping-pong ball / frosted acrylic | 1 | Any sports shop | 10 |
| USB-C **data** cable | 1 | — | have one |

Optional: breadboard (₹70), 5V 1A wall charger (₹200) to untether from the Mac, enclosure.

**Total: roughly ₹400–600.**

---

## 4. Wiring

```
ESP32-C3                    WS2812 8-bit stick
--------                    ------------------
5V  ──[1N4007]──────────►   VCC     (diode only if flicker appears)
GND ────────────────────►   GND
GPIO2 ──[330Ω]──────────►   DIN

470µF cap across VCC/GND at the stick, observe polarity
```

- Data resistor as close to the stick as possible
- Cap across the stick's power pins, not the ESP's
- Do not power 8 pixels from the 3.3V pin
- Solder pads on this stick lift easily; use headers, don't stress the joints

---

## 5. Event mapping (verified against docs)

| Hook event | Matcher | Action on ESP |
|---|---|---|
| `SessionStart` | `startup\|resume\|clear` | Register `session_id`, claim a pixel |
| `UserPromptSubmit` | *(no matcher support)* | Amber |
| `PostToolUse` | *(matches tool name)* | Amber — clears a resolved block |
| `Notification` | `permission_prompt\|idle_prompt\|agent_needs_input` | Red |
| `Stop` | *(no matcher support)* | Green |
| `StopFailure` | *(matches error type)* | Purple |
| `SessionEnd` | `clear\|resume\|logout\|prompt_input_exit\|other` | Deregister session |

Every event delivers `session_id`, `cwd`, and `hook_event_name` in the POST body as standard common fields, so the ESP can key its session table directly off the payload. `Notification` additionally carries `notification_type`, `message`, and `title`.

`SessionEnd` gives clean deregistration — better than relying on expiry alone. Expiry is still needed for crashed sessions that never fire it.

### Why `PostToolUse` is not optional

**Answering a permission prompt fires no hook.** `UserPromptSubmit` fires on
prompt submission, not on approving a dialog. So once `Notification` sets a
session to blocked, nothing clears it until `Stop` lands at the end of the turn —
the light keeps escalating while Claude is happily working.

`PostToolUse` is the first event that arrives after you approve. It sets the
session back to working.

This does **not** strobe the light, which was the original reason for excluding
it. Setting a session to `WORKING` when it is already `WORKING` is a no-op: the
animation clock only restarts on a state *change*. The refreshed `lastSeen` is a
bonus — it keeps long tasks well clear of `SESSION_TTL_MS`.

**Residual gap:** `PostToolUse` fires when the tool *finishes*. Approve a
long-running tool and the rainbow continues until it completes. `PreToolUse`
doesn't help — it fires before the permission prompt, so `Notification`
overwrites it. If this bites, check whether a `Notification` subtype fires on
prompt resolution; watch Serial Monitor while approving to see what actually
arrives.

Still deliberately **not** hooked: `PreToolUse`, `SubagentStart`/`SubagentStop`.

### Hook configuration

Lives in `~/.claude/settings.json` (global — all projects). **Now uses
`type: "command"`, not `type: "http"`.** Full config and the forwarding script
are in `signal_mac_setup.md`.

Three reasons command hooks won:

- **`async: true` is command-hooks-only.** HTTP hooks cannot background
  themselves, so they block the turn until they resolve. With async an
  unreachable ESP costs nothing at all — this retires the whole `timeout: 2`
  concern documented below.
- **Host fallback.** A script can try mDNS, then a cached IP, then re-resolve. A
  hardcoded URL in `settings.json` gets one attempt and no recovery.
- **Token independence.** The script reads `~/.config/signal/token`, so it works
  whether Claude Code launched from a terminal or the VS Code GUI. A GUI launch
  does not source `~/.zshrc`, which makes `$SIGNAL_TOKEN` empty and every hook
  401 — while your terminal tests keep passing.

The script forwards the stdin event JSON unchanged; the ESP parses `session_id`
and `hook_event_name` itself. No `jq`, nothing to keep in sync.

The HTTP config below is kept for reference — the ESP's response contract rules
still apply either way.

```json
{
  "hooks": {
    "UserPromptSubmit": [
      { "hooks": [ {
          "type": "http",
          "url": "http://signal.local/state",
          "timeout": 2,
          "headers": { "X-Signal-Token": "$SIGNAL_TOKEN" },
          "allowedEnvVars": ["SIGNAL_TOKEN"]
      } ] }
    ],
    "Notification": [
      { "matcher": "permission_prompt|idle_prompt|agent_needs_input",
        "hooks": [ { "type": "http", "url": "http://signal.local/state", "timeout": 2 } ] }
    ],
    "Stop": [
      { "hooks": [ { "type": "http", "url": "http://signal.local/state", "timeout": 2 } ] }
    ]
  }
}
```

`SessionStart`, `SessionEnd`, and `StopFailure` follow the same shape — same URL
for all six. The firmware reads `hook_event_name` from the POST body, so nothing
needs encoding in the path or query string.

Full config, token setup, and heartbeat agent: **`signal_mac_setup.md`**.

### Three contract rules the ESP must obey

**1. Always respond 204, or 200 with a completely empty body.**
A 2xx with a plain-text body such as `OK` is treated as a non-blocking error and puts a `hook error` notice in your transcript on *every single turn*. Empty body, or a valid JSON object. Nothing else.

**2. Set `timeout` explicitly on every hook. Use 2 seconds.**
The default is 600s for HTTP hooks (30s on `UserPromptSubmit`). HTTP hooks cannot be `async` — that flag is command-hooks-only — so they block the turn until they resolve. A powered-off ESP means the TCP SYN goes unanswered and you wait out the full timeout before Claude Code moves on. `timeout: 2` bounds the worst case.

**3. Connection failure is already safe.**
Per the docs, a connection failure or non-2xx from an HTTP hook is a non-blocking error and execution continues. Claude Code handles the dead-device case natively — you do not need the `|| true` guard that a curl-based design would require. Rule 2 is what still needs your attention.

Optionally add `allowedHttpHookUrls` in settings to allowlist only `signal.local`, so no other hook config can POST somewhere unexpected. (Only relevant to the HTTP variant.)

### Host resolution — three layers

DHCP has already moved the device twice during this build (`.16` → `.10`), so the
script resolves in order and gives up silently rather than ever blocking:

1. **`signal.local`** via mDNS — follows the device across leases
2. **Cached IP** at `~/.config/signal/host` — covers mDNS being filtered or slow
3. **Re-resolve via `ping` and re-cache** — recovers when both are stale

`ping` is the most reliable `.local` lookup on macOS; `dscacheutil` does not
handle mDNS names consistently.

**The real fix is a DHCP reservation** on the router, binding the ESP's MAC to a
fixed address. Then all three layers become belt-and-braces. A firmware static IP
also works but breaks when the device moves networks.

---

## 6. Execution

Two tracks that progress in parallel — hardware bring-up doesn't block firmware logic, which is testable on the bench with `curl` standing in for Claude Code.

### Track A — Hardware

**A1. Bench bring-up.** Flash blink. Confirm USB-C enumeration and that the cable carries data. Confirm pin mapping against your board's silkscreen; SuperMini clones vary.

**A2. First light.** Adafruit_NeoPixel, cycle R/G/B on all 8. If red renders green, swap `NEO_GRB` → `NEO_RGB`.

**A3. Power characterisation.** Measure current at brightness 255 full white vs 180.

**A4. Diffusion and enclosure.** Ping-pong ball or frosted acrylic. Tune colour values by eye *after* diffusion — raw values look completely different through plastic.

### Track B — Firmware

**B1. WiFi + HTTP server + mDNS.** Serve `GET /health`. mDNS so hooks target `signal.local` rather than a DHCP lease that will change on you.

**B2. State machine.** Session table, priority resolution, expiry, disconnected fallback. Testable standalone.

**B3. `POST /state` endpoint.** Parse `session_id` and `hook_event_name` from the body. Shared-secret header check. **Respond 204 empty.**

**B4. Hook wiring.** Drop the JSON above into `~/.claude/settings.json`. Verify with `/hooks` in Claude Code — it opens a read-only browser showing every configured hook, its source file, and its URL.

**B5. Heartbeat.** ESP watches `WiFi.status()` for its own disconnection. A launchd agent on the Mac POSTs to `/heartbeat` every 15s to cover Mac sleep — launchd doesn't run while asleep, which is exactly the signal wanted. Session entries expire after 60s of silence.

### Code

| File | Covers |
|---|---|
| `signal_m1_bench.ino` | M1 — hardware verification and colour tuning, no WiFi |
| `signal_firmware.ino` | M2–M4 — WiFi, mDNS, `/state`, session table, priority, expiry, disconnected |
| `signal_mac_setup.md` | Token, `~/.claude/settings.json` hooks, launchd heartbeat, verification curls |

Firmware needs **Adafruit NeoPixel** and **ArduinoJson v7** from Library Manager.

---

## 7. Concurrency and animation

`session_id` arrives on every event, so the ESP keys an 8-slot table off it.

**Aggregate mode — implemented.** All sessions collapse to one display by
priority: any blocked → blocked, else any API error, else any working, else done,
else idle. Blocked count is encoded as pulses.

**Per-pixel mode — still open (decision 3).** Each session claims one of the 8
pixels. Not built; would replace the count pulses entirely.

`SessionEnd` releases a slot immediately. Expiry catches sessions that die
without firing it.

### Blocked, in three phases

| Phase | Window | Display |
|---|---|---|
| Intro | 0 – 1.2s | Three red flashes — something changed |
| Count | 1.2s – 15s | N pulses, pause, repeat, if more than one session is blocked. A single blocked session holds solid |
| Escalate | 15s+ | Rainbow spin across all pixels until answered |

The animation clock restarts when the blocked **count** changes, not just the
state — otherwise a second session blocking mid-pattern leaves you counting a
half-finished cycle and reading the wrong number.

Once disco starts the count is no longer visible. Escalation and information
cannot share the same eight pixels; `/health` still lists every session.

### Timing constants

| Constant | Value | What it controls |
|---|---|---|
| `BLINK_ON_MS` / `BLINK_OFF_MS` | 200 / 200 | Flash rate of the three-flash intro |
| `BLINK_COUNT` | 3 | Flashes in the intro |
| `CHASE_STEP_MS` | 70 | Comet speed. Lower is faster |
| `ESCALATE_MS` | 15000 | Blocked this long → disco |
| `DONE_HOLD_MS` | 8000 | Green holds this long, then idle |
| `SESSION_TTL_MS` | 900000 | Fallback expiry for everything else |
| `RENDER_INTERVAL` | 20 | ~50fps |

### The TTL trap

`SESSION_TTL_MS` and `DONE_HOLD_MS` are deliberately different, and the reason is
easy to forget.

**No event fires while Claude is working.** `UserPromptSubmit` fires once at the
start, then nothing until `Stop`. So a short global TTL would expire the session
mid-task and drop the light to idle while Claude is still running.

But a long TTL applied to a *finished* session means green sits there for the
full duration before clearing.

Hence per-state expiry: `ST_DONE` clears after 8s, everything else gets 15
minutes. Never collapse these back into one value.

`render()` is throttled to ~50fps for a related reason — `strip.show()` masks
interrupts for roughly 240µs per call, and rendering flat-out disturbs WiFi.

---

## 8. Test cases

### 8.1 Hardware

| ID | Case | Pass criteria |
|---|---|---|
| H-01 | Board enumerates over USB-C | Serial port appears; blink sketch flashes |
| H-02 | All 8 pixels addressable | Each lights individually, none dead |
| H-03 | Colour order correct | `setPixelColor(0,255,0,0)` renders red, not green |
| H-04 | Current draw at brightness 180 | < 400mA total measured |
| H-05 | Sustained thermal | 30 min amber, nothing above warm |
| H-06 | Data integrity without diode | No flicker over 10 min of colour changes |
| H-07 | Diffused legibility | All states distinguishable at 2m in daylight |

### 8.2 Firmware unit

| ID | Case | Pass criteria |
|---|---|---|
| F-01 | Priority: red + amber | Resolves red |
| F-02 | Priority: purple + amber | Resolves purple |
| F-03 | Priority: all green | Resolves green |
| F-04 | Empty session table | Resolves idle (off) |
| F-05 | Malformed JSON body | No crash, state unchanged |
| F-06 | Unknown `hook_event_name` | Ignored cleanly, no crash |
| F-07 | Session table overflow (>8) | Oldest evicted cleanly, no overrun |
| F-08 | Expiry timer | Entry silent 60s is removed |
| F-09 | Duplicate `session_id` | Updates existing entry, no duplicate row |
| F-10 | `SessionEnd` releases pixel | Pixel freed, reusable by next session |

### 8.3 Network and hook contract

| ID | Case | Pass criteria |
|---|---|---|
| N-01 | Cold boot WiFi connect | Associates within 10s |
| N-02 | Router reboot | Auto-reconnects without power cycle |
| N-03 | mDNS resolution | `signal.local` resolves from Mac |
| N-04 | Missing / wrong auth token | Rejected, state unchanged |
| N-05 | Endpoint latency | POST → visible colour change < 200ms |
| N-06 | **Response body is empty** | **No `hook error` notice in transcript after 20 turns** |
| N-07 | Response status | 204, or 200 with zero-length body |
| N-08 | `/hooks` menu inspection | All six hooks listed, sourced from User Settings |

### 8.3b Animation

| ID | Case | Pass criteria |
|---|---|---|
| A-01 | Working state | Comet runs continuously, tail visible, never static |
| A-02 | Blocked, one session | Three flashes, then solid red |
| A-03 | Blocked, three sessions | Three flashes, then repeating three-pulse count |
| A-04 | Second session blocks mid-count | Pattern restarts cleanly at the new count |
| A-05 | Blocked held 15s | Disco starts, continues until answered |
| A-06 | Done | Three green flashes, solid, off after ~8s |
| A-07 | Long task (>5 min) | Stays on the comet, never drops to idle mid-task |
| A-08 | Disconnected then reconnect | Blue pulse, then the current state's intro plays from the start |
| A-09 | Legibility at 2m | Working and done distinguishable by motion alone, colour ignored |

### 8.4 Integration

| ID | Case | Pass criteria |
|---|---|---|
| I-01 | Submit prompt | Amber within 1s |
| I-02 | Claude requests permission | Red within 1s |
| I-03 | Turn completes | Green within 1s |
| I-04 | Rate-limit / overload error | Purple, not green |
| I-05 | Two concurrent sessions, one blocked | Red (aggregate) or correct per-pixel |
| I-06 | Three concurrent, all working | Amber, count correct |
| I-07 | Rapid state flapping | No queue backup, final state correct |
| I-08 | Long task (>10 min) | Stays amber, no premature green |
| I-09 | Heavy tool use inside one turn | No strobing — confirms PreToolUse stayed unhooked |

### 8.5 Failure and resilience

| ID | Case | Pass criteria |
|---|---|---|
| R-01 | **ESP powered off, hook fires** | **Turn proceeds; delay ≤ 2s per hook, no error, no hang** |
| R-02 | ESP unplugged mid-session | Claude Code continues normally |
| R-03 | WiFi drops mid-task | Light falls to disconnected blue within 60s |
| R-04 | ESP reboots mid-task | Recovers correct state on next hook or heartbeat |
| R-05 | Session killed with `kill -9` | Entry expires, pixel released |
| R-06 | Mac sleeps | Disconnected state, not stale green |
| R-07 | Wrong `timeout` value regression | With `timeout` omitted, confirm the delay is unacceptable — proves the setting matters |

**R-01 and R-07 are the ones that matter.** Everything else is cosmetic. Claude Code tolerates a dead endpoint natively, but only *after* the timeout expires — and the default is 600 seconds. Test R-01 by unplugging the ESP and running a normal session end to end before you trust the setup.

**N-06 is the sneakiest.** A cheerful `server.send(200, "text/plain", "OK")` will pollute your transcript with a hook error on every turn. It will work, and it will annoy you daily until you find it.

---

## 9. Milestones

| # | Deliverable | Gate |
|---|---|---|
| M1 | LED shows all states from hardcoded sketch | H-01 → H-04 |
| M2 | ESP accepts POST, changes colour, returns 204 empty | N-01 → N-07 |
| M3 | Single Claude Code session drives the light | I-01 → I-04, R-01 |
| M4 | Concurrency, session lifecycle, expiry | F-01 → F-10, I-05 → I-09 |
| M5 | Resilience hardened, enclosure built | R-02 → R-07, H-07 |

M3 is where it becomes genuinely useful. Pull R-01 forward into M3 rather than leaving it to M5 — it's the one failure that costs you real work.

---

## 10. Resolved decisions

| # | Question | Answer |
|---|---|---|
| 1 | Hook event names | Verified. `SessionStart`, `UserPromptSubmit`, `Notification`, `Stop`, `StopFailure`, `SessionEnd`. See §5 |
| 2 | Hook config scope | Global — `~/.claude/settings.json`, applies to all projects |
| 3 | Aggregate vs per-pixel | Aggregate, with blocked-count pulses. Per-pixel still open |
| 4 | Wall charger vs Mac-tethered | Mac-tethered is fine |

### Consequences of the §5 findings

- **The Mac-side script is gone entirely.** No curl, no jq, no launchd for the state push. Native `type: "http"` hooks do it. The heartbeat is the only Mac-side process left, and it exists purely to detect sleep
- **`|| true` and `--max-time` are obsolete.** Both were workarounds for a curl design. `timeout: 2` replaces them
- **The auth token is native.** `headers` plus `allowedEnvVars: ["SIGNAL_TOKEN"]` handles it, no shell interpolation
- **`SessionEnd` beats pure expiry** for clean pixel release
- **`/hooks`** gives you a built-in inspector — use it before debugging anything else

---

## 11. Command reference

Everything needed to bring the device up, test it, and debug it. Assumes
`SIGNAL_TOKEN` is exported and matches `AUTH_TOKEN` in the firmware.

### 11.1 Finding the serial port

The port number changes whenever you plug into a different USB socket. Check it
whenever an upload fails with "port does not exist".

```bash
# the ESP's serial device
ls /dev/cu.usbmodem*
# -> /dev/cu.usbmodem1101   (the number WILL change between USB ports)

# more detail, including vendor and bus
system_profiler SPUSBDataType | grep -A 8 -i "esp\|serial"

# watch for it appearing as you plug in
ls /dev/cu.* > /tmp/before.txt   # unplug, run this
ls /dev/cu.* > /tmp/after.txt    # plug in, run this
diff /tmp/before.txt /tmp/after.txt
```

Use `cu.` not `tty.` — the `tty.` variant blocks waiting on a carrier signal the
ESP never asserts.

In Arduino IDE: Tools → Port, pick the `cu.usbmodemXXXX` that matches. The status
bar shows the currently selected one and flags `[not connected]` when it's stale.

If upload still fails: hold **BOOT**, tap **RST**, release BOOT, then upload. The
C3 sometimes needs manual bootloader entry, especially while running firmware is
busy with WiFi.

### 11.2 Token setup

```bash
# check whether it's set in this shell
echo "[$SIGNAL_TOKEN]"      # brackets expose stray spaces or quote characters

# set for this session
export SIGNAL_TOKEN="k3Jm9pQx7Zt2Vw5B"

# make permanent
echo 'export SIGNAL_TOKEN="k3Jm9pQx7Zt2Vw5B"' >> ~/.zshrc
source ~/.zshrc
```

Must match `AUTH_TOKEN` in the firmware exactly. An unset variable sends an empty
header and every request 401s — with an empty body, so it looks like success
unless you pass `-i`.

### Use an alphanumeric token. This is not cosmetic.

```cpp
const char* AUTH_TOKEN = "k3Jm9pQx7Zt2Vw5B";   // letters and digits only
```

A token containing backslashes cannot survive the round trip. In C++, `\s` and
`\K` are invalid escape sequences and the compiler strips the backslash, so the
ESP holds a *different string* than the one you typed. Bash inside double quotes
keeps the backslash, so curl sends it. Permanent 401, and nothing in the logs
explains why. `#`, braces, and `?]?:` sequences carry their own hazards.

16 random alphanumerics is ~95 bits of entropy — far past sufficient for "nothing
else on my WiFi should be able to change my desk light". Punctuation buys nothing
and costs an evening.

### Verify the token end to end

```bash
export SIGNAL_TOKEN="k3Jm9pQx7Zt2Vw5B"
curl -si -X POST http://signal.local/state \
  -H "X-Signal-Token: $SIGNAL_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"session_id":"t1","hook_event_name":"UserPromptSubmit"}' | head -3
```

Want `HTTP/1.1 204` and the light going amber. A 401 here means the shell string
and the firmware string differ — check for backslashes before anything else.

If this stays stubborn, set `AUTH_TOKEN = ""` in the firmware and reflash. The
check is skipped entirely. Prove the rest of the chain works, then decide whether
you want the token back at all.

### 11.3 Device health

```bash
# no token required — always works if the device is up
curl --max-time 5 http://signal.local/health

# if mDNS fails, use the raw IP from the serial boot log
curl --max-time 5 http://192.168.1.10/health
```

**Always pass `--max-time`.** Hitting an IP with nothing on it does not fail fast:
the SYN goes unanswered, your Mac retries with backoff, and curl sits there for
~75 seconds before giving up. That is the same silent-failure behaviour that makes
`timeout: 2` mandatory in the hook config — worth experiencing once, deliberately,
so the hook setting stops feeling like paranoia.

Expected output:

```
signal
ip: 192.168.1.10
rssi: -57
uptime_s: 423
heartbeat_age_s: none
disconnected: no
sessions:
```

`rssi` worse than -75 dBm means weak signal. `sessions:` lists live entries with
state and age — the first place to look when a colour is wrong.

**Note:** the IP will change over time as DHCP hands out new leases. That is why
everything targets `signal.local` rather than a fixed address.

### 11.4 State tests

Run in order, watching the light.

```bash
# amber — one session working
curl -X POST http://signal.local/state \
  -H "X-Signal-Token: $SIGNAL_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"session_id":"test-1","hook_event_name":"UserPromptSubmit"}'

# red — second session blocked, blocked outranks working (test F-01)
curl -X POST http://signal.local/state \
  -H "X-Signal-Token: $SIGNAL_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"session_id":"test-2","hook_event_name":"Notification"}'

# back to amber — test-2 done, test-1 still working
curl -X POST http://signal.local/state \
  -H "X-Signal-Token: $SIGNAL_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"session_id":"test-2","hook_event_name":"Stop"}'

# purple — API error state (test I-04)
curl -X POST http://signal.local/state \
  -H "X-Signal-Token: $SIGNAL_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"session_id":"test-1","hook_event_name":"StopFailure"}'

# green — everything finished
curl -X POST http://signal.local/state \
  -H "X-Signal-Token: $SIGNAL_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"session_id":"test-1","hook_event_name":"Stop"}'

# off — explicit deregistration (test F-10)
curl -X POST http://signal.local/state \
  -H "X-Signal-Token: $SIGNAL_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"session_id":"test-1","hook_event_name":"SessionEnd"}'
```

### 11.5 Response contract (test N-06, N-07)

The one that keeps hook error notices out of the Claude Code transcript.

```bash
curl -si -X POST http://signal.local/state \
  -H "X-Signal-Token: $SIGNAL_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"session_id":"test-1","hook_event_name":"Stop"}' | head -5
```

Want: `HTTP/1.1 204`, `Content-Length: 0`, nothing after the headers.

Anything with a body — even `OK` — puts a hook error in the transcript on every
single turn.

### 11.6 Auth rejection (test N-04)

```bash
# no token at all
curl -si -X POST http://signal.local/state \
  -H "Content-Type: application/json" -d '{}' | head -1
# -> HTTP/1.1 401 Unauthorized

# wrong token
curl -si -X POST http://signal.local/state \
  -H "X-Signal-Token: wrong" \
  -H "Content-Type: application/json" -d '{}' | head -1
# -> HTTP/1.1 401 Unauthorized
```

401 also returns an empty body, so without `-i` a rejected request is
indistinguishable from a successful one. Always use `-i` when debugging.

### 11.7 Heartbeat

```bash
# send one manually
curl -si -X POST http://signal.local/heartbeat \
  -H "X-Signal-Token: $SIGNAL_TOKEN" | head -3
# -> HTTP/1.1 204

# confirm it registered
curl -s http://signal.local/health | grep heartbeat
# -> heartbeat_age_s: 3     (was "none" before the first ping)

# watch it climb, then trip the disconnected state at 60s (test R-06)
while true; do curl -s http://signal.local/health | grep -E "heartbeat|disconnected"; sleep 5; done
```

Once the launchd agent is loaded, `heartbeat_age_s` should never exceed 15.

```bash
# agent control
launchctl load   ~/Library/LaunchAgents/com.ritesh.signal.heartbeat.plist
launchctl unload ~/Library/LaunchAgents/com.ritesh.signal.heartbeat.plist
launchctl list | grep signal

# agent errors
cat /tmp/signal-heartbeat.err

# simulate sleep: stop the pings and watch the light fall to blue after 60s
launchctl unload ~/Library/LaunchAgents/com.ritesh.signal.heartbeat.plist
```

### 11.8 Malformed input (test F-05, F-06)

None of these should crash the device or change the light. `/health` must still
answer afterwards.

```bash
# invalid JSON
curl -si -X POST http://signal.local/state \
  -H "X-Signal-Token: $SIGNAL_TOKEN" -d 'not json at all' | head -1

# valid JSON, missing fields
curl -si -X POST http://signal.local/state \
  -H "X-Signal-Token: $SIGNAL_TOKEN" -d '{}' | head -1

# unknown event name
curl -si -X POST http://signal.local/state \
  -H "X-Signal-Token: $SIGNAL_TOKEN" \
  -d '{"session_id":"x","hook_event_name":"NotARealEvent"}' | head -1

# still alive?
curl http://signal.local/health
```

### 11.9 Session table overflow (test F-07)

Nine sessions into eight slots. The oldest should be evicted, nothing should
crash.

```bash
for i in $(seq 1 9); do
  curl -s -X POST http://signal.local/state \
    -H "X-Signal-Token: $SIGNAL_TOKEN" \
    -H "Content-Type: application/json" \
    -d "{\"session_id\":\"overflow-$i\",\"hook_event_name\":\"UserPromptSubmit\"}"
done
curl -s http://signal.local/health
# -> exactly 8 sessions listed, overflow-1 gone
```

### 11.10 Expiry (test F-08)

```bash
curl -s -X POST http://signal.local/state \
  -H "X-Signal-Token: $SIGNAL_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"session_id":"expire-me","hook_event_name":"UserPromptSubmit"}'

sleep 65
curl -s http://signal.local/health
# -> session gone, light off
```

### 11.11 R-01 — the one that matters

Unplug the ESP entirely. Run a normal Claude Code session end to end.

Expected: each hook adds at most ~2 seconds, no error, no hang, work proceeds
normally. If it hangs longer, `timeout` is missing or misspelled somewhere in
`~/.claude/settings.json`.

Do not skip this. A desk toy that stalls your actual work is worse than no desk
toy.

### 11.12 Symptom lookup

| Symptom | Cause |
|---|---|
| 401 on every POST | `$SIGNAL_TOKEN` unset, or mismatch with firmware `AUTH_TOKEN` |
| 401 with a token that looks identical | Backslashes. C++ strips them, bash keeps them — the two strings differ |
| curl hangs ~75s then fails | Nothing at that IP. Device moved or is off. Use `--max-time` |
| `/health` works, `/state` does nothing | Almost always the token. Re-run with `-i` |
| `signal.local` won't resolve | mDNS didn't start. Try the raw IP from the serial boot log |
| Upload: "port does not exist" | Port number changed. Re-check `ls /dev/cu.usbmodem*` |
| Upload fails while firmware runs | Hold BOOT, tap RST, release BOOT, upload |
| Serial monitor silent after boot | Normal — firmware only prints on hook events |
| Serial monitor silent always | USB CDC On Boot is Disabled. Enable and reflash |
| Nothing lights, `/health` fine | Wiring. Check DIN end, not DOUT |
| `hook error` in transcript every turn | ESP returning a body. Must be 204 empty |
| Light stuck on last colour | Heartbeat not running, or Mac asleep with no agent loaded |
| Light drops to idle mid-task | `SESSION_TTL_MS` too short. No event fires while Claude works |
| Green holds far too long | `DONE_HOLD_MS` raised, or per-state expiry collapsed into one TTL |
| Comet stutters, WiFi flaky | `RENDER_INTERVAL` too low. `show()` masks interrupts |
| Smooth rainbow spinning | Not a fault — blocked 15s+, escalated. Go answer Claude |
| Hooks work in terminal, not in VS Code | Relying on `$SIGNAL_TOKEN`. GUI launches don't source `~/.zshrc` — use the token file |
| Intermittent after an IP change | Stale `~/.config/signal/host`. Delete it, or set a DHCP reservation |
| `command not found` in transcript | Path in `settings.json` wrong, or script not `chmod +x` |
| Rainbow continues after you answer the prompt | `PostToolUse` hook missing from `settings.json`. Nothing else clears a resolved block before `Stop` |
| Random wrong-colour pixels, no pattern | Data corruption. Fit the 330Ω resistor, then the 1N4007 |
| Unlit pixels faintly glowing during chase | Floor value in `renderChase` above 0.0f |
| Colours wrong (red shows green) | Change `NEO_GRB` to `NEO_RGB` in the firmware |
| Works then stops after minutes | WiFi power save. Confirm `WiFi.setSleep(false)` |
