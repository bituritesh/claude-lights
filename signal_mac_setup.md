# signal — Mac setup

Everything that goes on the Mac. Uses `type: "command"` hooks throughout, which
buys three things over HTTP hooks:

- **`async: true`** — command hooks can background themselves. HTTP hooks cannot,
  so they block the turn until they resolve. With async, a dead ESP cannot stall
  your work at all, no matter what the timeout says.
- **Host fallback** — a script can try mDNS, then a cached IP, then re-resolve.
  A hardcoded URL in `settings.json` gets one shot.
- **Token independence** — the script reads the token from a file, so it works
  whether Claude Code was launched from a terminal or from the VS Code GUI.

---

## 1. Token

The firmware's `AUTH_TOKEN` must match. Letters and digits only — a backslash is
an invalid C++ escape sequence, so the compiler strips it while bash keeps it,
and the two strings silently differ.

```bash
mkdir -p ~/.config/signal
echo -n "k3Jm9pQx7Zt2Vw5B" > ~/.config/signal/token
chmod 600 ~/.config/signal/token
```

**Use the file, not an environment variable.** When you launch Claude Code from
the VS Code GUI it does not source `~/.zshrc`, so `$SIGNAL_TOKEN` is empty and
every hook 401s while your terminal tests pass. The file works either way.

Keep the shell export too, for manual curl testing:

```bash
echo 'export SIGNAL_TOKEN="k3Jm9pQx7Zt2Vw5B"' >> ~/.zshrc
source ~/.zshrc
```

---

## 2. Host resolution and fallback

Three layers, tried in order:

1. **`signal.local`** via mDNS — follows the device across DHCP leases
2. **Cached IP** at `~/.config/signal/host` — covers mDNS failing or being slow
3. **Re-resolve and re-cache** — recovers when both of the above are stale

Layer 1 handles the common case. Layers 2 and 3 exist because mDNS is not
reliable on every network: some routers filter multicast, and macOS caches
negative lookups.

### The real fix, if you want one

Give the ESP a **DHCP reservation** on your router — bind its MAC to a fixed IP.
Then the address never moves and all three layers become belt-and-braces. Find
the MAC in the serial boot log or your router's client list.

A static IP configured in firmware also works, but breaks if you move the device
to another network.

---

## 3. Hook script

Save as `~/.local/bin/signal-post.sh`, then `chmod +x`.

One script serves every hook. It reads the event JSON from stdin and forwards it
whole — the ESP extracts `session_id` and `hook_event_name` itself, so there is
no `jq` dependency and nothing to keep in sync.

```bash
#!/bin/bash
# signal-post.sh [endpoint]   endpoint defaults to "state"
# Reads the hook event JSON on stdin and forwards it to the ESP.
# Always exits 0 — a status light must never break the thing it reports on.

ENDPOINT="${1:-state}"
CONF=~/.config/signal
CACHE="$CONF/host"
TOKEN=$(cat "$CONF/token" 2>/dev/null || echo "$SIGNAL_TOKEN")

BODY=""
if [ "$ENDPOINT" = "state" ]; then
  BODY=$(cat)
fi

post() {
  curl --max-time 2 -sf -o /dev/null \
    -X POST "http://$1/$ENDPOINT" \
    -H "X-Signal-Token: $TOKEN" \
    -H "Content-Type: application/json" \
    -d "$BODY"
}

# 1. mDNS
if post "signal.local"; then exit 0; fi

# 2. last known good IP
if [ -s "$CACHE" ] && post "$(cat "$CACHE")"; then exit 0; fi

# 3. re-resolve, cache, retry. ping is the most reliable mDNS lookup on macOS —
#    dscacheutil does not handle .local consistently.
IP=$(ping -c1 -t1 signal.local 2>/dev/null \
     | sed -n 's/.*(\([0-9.]*\)).*/\1/p' | head -1)
if [ -n "$IP" ] && post "$IP"; then
  echo "$IP" > "$CACHE"
  exit 0
fi

exit 0
```

`-sf` makes curl return non-zero on HTTP errors, so a 401 falls through to the
next layer instead of being mistaken for success. The final `exit 0` is
unconditional: the device being unreachable is not your problem mid-task.

---

## 4. Hook config

Goes in `~/.claude/settings.json` — global, applies to every project. Merge the
`hooks` key into the existing file rather than replacing it.

**Replace `/Users/ron` with your actual home path.** Claude Code does not
reliably expand `$HOME` or `~` inside a hook command.

```json
{
  "hooks": {
    "SessionStart": [
      { "matcher": "startup|resume|clear",
        "hooks": [ { "type": "command", "command": "/Users/ron/.local/bin/signal-post.sh", "timeout": 3, "async": true } ] }
    ],
    "UserPromptSubmit": [
      { "hooks": [ { "type": "command", "command": "/Users/ron/.local/bin/signal-post.sh", "timeout": 3, "async": true } ] }
    ],
    "PostToolUse": [
      { "hooks": [ { "type": "command", "command": "/Users/ron/.local/bin/signal-post.sh", "timeout": 3, "async": true } ] }
    ],
    "Notification": [
      { "matcher": "permission_prompt|idle_prompt|agent_needs_input",
        "hooks": [ { "type": "command", "command": "/Users/ron/.local/bin/signal-post.sh", "timeout": 3, "async": true } ] }
    ],
    "Stop": [
      { "hooks": [ { "type": "command", "command": "/Users/ron/.local/bin/signal-post.sh", "timeout": 3, "async": true } ] }
    ],
    "StopFailure": [
      { "hooks": [ { "type": "command", "command": "/Users/ron/.local/bin/signal-post.sh", "timeout": 3, "async": true } ] }
    ],
    "SessionEnd": [
      { "hooks": [ { "type": "command", "command": "/Users/ron/.local/bin/signal-post.sh", "timeout": 3, "async": true } ] }
    ]
  }
}
```

Seven identical entries. The script needs no arguments because the event name
travels inside the JSON on stdin.

### Why PostToolUse is here

It is what **clears a block**. Answering a permission prompt fires no hook of its
own, so `Notification` sets the session red and nothing sets it back — the light
stays red, escalates to disco, and sits there for the rest of the turn while
Claude works away happily.

`PostToolUse` fires as soon as the approved tool runs, returning the light to the
comet. Firing on every tool call is harmless: the state is idempotent, the
animation clock only restarts on a change, and the refreshed timestamp keeps long
tasks clear of the session TTL.

`PreToolUse` is deliberately absent. It fires *before* the permission prompt, so
`Notification` overwrites it a moment later — all it adds is a brief comet flash
before the red.

**Residual gap:** `PostToolUse` fires when the tool *finishes*. Approve a
long-running tool and the rainbow keeps going until it completes. Watch Serial
Monitor while approving to see what actually arrives if this bothers you.

Verify everything with `/hooks` inside Claude Code — a read-only inspector
listing each hook and its source file.

---

## 5. Heartbeat agent

The ESP detects its own WiFi loss but cannot detect the Mac sleeping. Without a
heartbeat, a sleeping Mac leaves the last colour showing indefinitely.

`launchd` does not run agents while asleep, which is exactly what is wanted: sleep
stops the pings, the ESP notices the silence, the light falls to disconnected blue.

The same script handles it — pass `heartbeat` as the endpoint.

`~/Library/LaunchAgents/com.ritesh.signal.heartbeat.plist`:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.ritesh.signal.heartbeat</string>

    <key>ProgramArguments</key>
    <array>
        <string>/Users/ron/.local/bin/signal-post.sh</string>
        <string>heartbeat</string>
    </array>

    <key>StartInterval</key>
    <integer>15</integer>

    <key>RunAtLoad</key>
    <true/>

    <key>StandardErrorPath</key>
    <string>/tmp/signal-heartbeat.err</string>
</dict>
</plist>
```

No `EnvironmentVariables` block needed — the script reads the token from
`~/.config/signal/token`, which launchd has no trouble with.

### Is this step optional?

**Yes.** Skip it while you get the hooks working. Its only job is detecting your
Mac going to sleep so the light fades to blue instead of holding a stale colour.
Every other state works without it, and `heartbeat_age_s: none` means the ESP
will not cry wolf over a heartbeat that never existed.

Come back to it once the hooks are proven.

### Managing the agent

The plist above is a **file** you create. The commands below are typed into
Terminal to control it — they are not saved anywhere.

| Command | What it does |
|---|---|
| `launchctl load ~/Library/LaunchAgents/com.ritesh.signal.heartbeat.plist` | Registers the agent with macOS and starts it. Run once after creating the plist; it survives reboots |
| `launchctl unload ~/Library/LaunchAgents/com.ritesh.signal.heartbeat.plist` | Stops it. Use while debugging, or to deliberately trigger the disconnected state |
| `launchctl list \| grep signal` | Checks whether it is registered. Output means loaded, no output means not |
| `cat /tmp/signal-heartbeat.err` | The error log named in the plist's `StandardErrorPath`. First place to look if pings are not landing |

After editing the plist you must **unload then load** — launchd caches the old
definition and will otherwise keep running it.

On newer macOS, `launchctl load` prints a deprecation notice suggesting
`bootstrap`. It still works; ignore it.

### Confirm it is actually running

```bash
launchctl load ~/Library/LaunchAgents/com.ritesh.signal.heartbeat.plist
sleep 20
curl -s http://signal.local/health | grep heartbeat
# -> heartbeat_age_s: 5     (anything under 15 is healthy)
```

Still `none` after 20 seconds means the agent is not firing. Check
`/tmp/signal-heartbeat.err`, then confirm the path in `ProgramArguments` matches
where you actually saved the script.

---

## 6. Verification, in order

```bash
# device reachable, mDNS resolving
curl --max-time 5 http://signal.local/health

# script works standalone — light should go amber
echo '{"session_id":"test-1","hook_event_name":"UserPromptSubmit"}' \
  | ~/.local/bin/signal-post.sh

# blocked — three red flashes, then solid
echo '{"session_id":"test-2","hook_event_name":"Notification"}' \
  | ~/.local/bin/signal-post.sh

# cleared by a tool call — back to the comet
echo '{"session_id":"test-2","hook_event_name":"PostToolUse"}' \
  | ~/.local/bin/signal-post.sh

# heartbeat
~/.local/bin/signal-post.sh heartbeat
curl -s http://signal.local/health | grep heartbeat

# fallback layer: break mDNS and confirm the cached IP still works
echo "192.168.1.10" > ~/.config/signal/host
echo '{"session_id":"t","hook_event_name":"Stop"}' | ~/.local/bin/signal-post.sh
```

Then restart your Claude Code session — hook config is read at session start, so
an already-running session holds the old config — run `/hooks` to confirm all
seven are listed, and work normally with Serial Monitor open.

---

## 7. Test R-01 before you trust any of this

Unplug the ESP entirely. Run a normal Claude Code session end to end.

Expected with `async: true`: **no delay at all.** The hook backgrounds itself and
your turn proceeds regardless. This is the main reason to prefer command hooks.

If you do see a stall, `async` is missing from one of the seven entries.

---

## Troubleshooting

**Light never changes during real sessions but the script works manually** —
token. Check `~/.config/signal/token` matches the firmware's `AUTH_TOKEN`
exactly, with no trailing newline (`echo -n` above avoids one).

**Hooks not firing at all** — session was running when you edited
`settings.json`. Restart it, then confirm with `/hooks`.

**`command not found` in the transcript** — the path in `settings.json` is
wrong, or the script is not executable. `chmod +x ~/.local/bin/signal-post.sh`.

**Light stays red through a whole turn** — `PostToolUse` missing from the config.
That is the event that clears a resolved permission prompt.

**Works from terminal, not from VS Code** — you are relying on `$SIGNAL_TOKEN`.
GUI launches do not source `~/.zshrc`. Use the token file.

**Everything intermittent after an IP change** — stale cache. `rm
~/.config/signal/host` and let it re-resolve, or set a DHCP reservation.
