# External probe harness (EVN_PROBE=1)

The probe is an opt-in, mostly-externalised control surface over the running
game: a small HTTP server on `127.0.0.1` that can pause/step the game, inject
input, read game state, capture screenshots and tail the game log. Intended
uses: debugging during decomp work, scripted testing, and later a TAS/RL-style
step environment.

Start with `EVN_PROBE=1 build/release/src/evnova` (port override:
`EVN_PROBE_PORT`, default **8190**). Without the env var nothing is started
and the binary behaves exactly as before. The implementation lives in
`src/probe_server.{hpp,cpp}` (transport + control plane),
`src/game/probe_state.cpp` (state reader) and hooks inside
`src/sdl_platform.{hpp,cpp}`.

## Threading / safety model

- The server thread **never** touches SDL or GameState. It parses requests,
  enqueues work and owns a small mailbox of results.
- Everything else runs on the main thread at the two choke points every game
  loop already shares:
  - the **pump** — `SdlPlatform::PollTextEvent` / `PollFlightInput` /
    `PollCommandEvent` run queued jobs, push injected key events and enforce
    the pause latch (blocking *here* is what pauses the whole game, because
    every modal loop and the flight loop poll input);
  - the **frame boundary** — `SdlPlatform::Present()` (which every loop must
    call instead of `SDL_RenderPresent`) captures a pending screenshot *while
    the frame is still the active render target* — reading after
    `SDL_RenderPresent` is undefined under GPU backends — and counts down
    step frames.
- Game state is **read-only** through the probe; the only mutation channels
  are injected inputs (plus the explicit `quit` command). This keeps the
  harness honest w.r.t. original behaviour and valid as a future RL/TAS env.

## Endpoints

| Endpoint | Purpose |
| --- | --- |
| `GET /probe/ping` | liveness check → `ok` |
| `GET /probe/state?query=Q` | JSON snapshot of the live GameState (`main thread`) |
| `GET /probe/ui` | published layout of the active modal: `{"window":...,"rects":{"accept":[x,y,w,h],…}}` |
| `GET /probe/ui?at=x,y` | hit-test oracle: name of the published element under that window point |
| `GET /probe/screenshot` | next frame as `image/bmp` (last frame if paused) |
| `GET /probe/logs?since=SEQ` | tail the in-game log ring (`{"tail":N,"lines":[...]}`) |
| `POST /probe/command` | Execution commands plus semantic `land_at`, `jump_to`, `destroy_ship`, `trade`, and `cancel_automation` |
| `GET /probe/automation` | Current optional flight-automation goal, phase, target, and failure detail |
| `POST /probe/key` | `{"key":"I"}` tap; `{"key":"I","down":true\|false}` hold/release (modal-loop channel, synthetic `SDL_Event`s) |
| `POST /probe/click` | `{"element":"accept"}` clicks the named rect published by the active modal; `{"x":553,"y":508}` clicks raw window points (motion + button-down pair) |
| `POST /probe/hold` | `{"keys":["W","SPACE"],"down":true}` virtual held keys merged into `PollFlightInput` (flight channel — `SDL_GetKeyboardState` cannot see injected events) |

Unknown queries/paths return `400`/`404` with a hint. Requests that need the
main thread time out with `504` if it never pumps (e.g. blocked in a native
modal). While paused, state/log reads still work (the pump services them);
screenshots return the last captured frame.

### Scenario replays

Semantic probe workflows live in `tests/scenarios/` as versioned TOML files
and run through the standard-library runner:

```sh
python3 tools/scenario_runner.py tests/scenarios/tutorial_first_leg.toml
```

Steps use observable waits plus input-only `click`, `key`, `hold`, and
`command` actions; an explicit `quit` step stops the game. `key` taps inject
an SDL event (the modal-loop channel), while `hold` sets/releases virtual held
keys (the flight channel) and is required for held flight commands such as the
missions panel (key `I`), which read `PollFlightInput` state rather than SDL
events. `validate_state` performs
immediate exact (`expect`) and full-regex
(`matches`) assertions on dotted probe paths; numeric path components index
arrays. `screenshot` writes BMP checkpoints below `build/scenario-results/`.
On failure the runner records UI, summary, travel, mission, automation and log
state plus a screenshot. Comments are ordinary TOML comments, and unknown
actions or fields fail instead of being silently ignored. Set
`quit_on_finish = true` at the document root to stop the probed game after
either success or failure. A `repeat` step (with `count` and a nested
`[[steps.steps]]` array-of-tables) re-runs a block in place, keeping long
repeated routes compact; log and diagnostic step labels become dotted
(`7.3.2`) inside nested iterations.

Path components may use `*` to fan out over a list: `ui.items.*.label` holds
when any row's label equals the value. A step may carry `trigger` (a table
that must match before the step runs) and `trigger_not` (a table that must not
match); either failing skips the step, so a route can gate optional actions on
runtime state (e.g. only hire a class when it is currently offered and the
fleet does not already have one). `click` accepts either `element` (a
published rect name) or `where` (a table matched against the active modal's
`items[]` fields, e.g. `where = { label = "Terrapin" }`), resolving the first
matching row. A `repeat` may add `until = { ... }` to stop early once the
condition holds (checked after each iteration). The `ships` observation root
exposes `/probe/state?query=ships` plus synthetic `ships.escorts` (class
display names of the player's attached, non-mission behavior-6 escorts) and
`ships.escort_count`, so scenarios can test fleet composition without scanning
arrays in TOML. The escort predicate mirrors `Ship_CanPlayerHaveMoreEscorts`
(0x00468920): `ai_behavior_code == 6`, `mission_fleet_slot == -1` and
`squad_leader_ship_slot == 0`. The last test is what excludes NPC escorts
following other ships, which otherwise share the behavior code and mission
slot and inflate the count. `decline_mission` is a semantic step for the Spaceport/Bar
AvailLoc offer passes: it clicks `decline` on any `mission_offer` and `done`
on the `text_reader` refuse dialog that follows, returning once no such modal
has appeared for `settle_ms` (default 300, raise it to cover the Bar's delayed
recheck timer).

### Callable scenario fragments

Any scenario can be invoked as a reusable, parameterized function from another
scenario with a `call` step, which keeps shared workflows (new-pilot bootstrap,
a travel leg, a trade circuit) in one file instead of copied into every route:

```toml
[[steps]]
action = "call"
scenario = "fragments/travel_leg"   # relative to the caller; .toml optional
args = { destination = "Sol", timeout_ms = 180000 }
```

The callee's `[[steps]]` run in place; paths resolve relative to the calling
file, falling back to the root scenario's directory for nested fragments (the
`.toml` suffix is optional), `quit_on_finish` is ignored on callees, and a
callee's own `[defaults] timeout_ms` applies to its steps. Callees declare parameters as an
array of tables; a parameter without `default` is required and `args` may only
name declared parameters (typos fail):

```toml
[[params]]
name = "destination"

[[params]]
name = "timeout_ms"
default = 180000

[[steps]]
action = "command"
cmd = "jump_to"
target = "{{ destination }}"
timeout_ms = "{{ timeout_ms }}"
```

`{{ name }}` placeholders substitute throughout the callee's step tree,
including table keys such as `ui.items.{{ index }}.label`. A value that is
exactly one placeholder keeps the parameter's own type, so numeric/boolean
`expect` values survive; embedded placeholders are stringified. Undefined
placeholders, unknown or missing arguments, recursive calls and call depth past
32 all fail rather than silently doing nothing.

Flight automation is input-only: `{"cmd":"land_at","target":"Earth"}` and
`{"cmd":"jump_to","target":"Sol"}` install a controller that emits the same
edge/held `FlightInput` commands as a pilot. A pure-decimal `target` for
`land_at` is a stellar resource id (e.g. `201` = `0xC9` = Kiniké), which is the
reliable way to name a stellar whose raw MacRoman resource name cannot
round-trip through the probe's UTF-8 JSON. An optional positive `timeout_ms`
is measured in gameplay time. `cancel_automation` stops emission; status is
read from `/probe/automation`. Accepting a new `land_at`/`jump_to` request
immediately moves the published `phase` off any previous `complete` (to
`select`), so a `wait` on the finished goal cannot match the stale state and
race ahead before the flight loop starts the new one.

`{"cmd":"destroy_ship","target":"Raider"}` matches a current-system ship by
class display name (falling back to its instance `ship_name`, case-insensitive).
A pure-decimal `target` and an optional `ship_id` (matching either a live ship
slot or its instance id) are identifier fallbacks. `allow_missing = true`
completes instead of failing when no matching ship remains, for scenarios whose
target another ship may destroy first. It cycles the player's
primary ship target with the backquote hotkey until it lands on a match, then
leads and fires that target eagerly whenever it is within weapon reach: the
nose tracks the ready primary bank's predicted intercept (via
`Ship_AimWeaponPredictive` 0x0043b740) so a crossing target cannot outrun the
projectile, and re-tracking every frame keeps weapon pushback from breaking
the lock. It completes
when the locked target is destroyed and fails fast when no ship in the system
matches (unless `allow_missing` is set).

`{"cmd":"trade","commodity":2,"side":"buy"}` exercises the docked trade
center without touching game state: it synthesizes a click on the published
`trade.row.<commodity>` rect and then one on the `buy`/`sell` button, so the
modal's own handler runs the transaction (the click quantity, up to 10 tons).
An optional `"tons":N` (1..32000) makes that transaction exact, and
`"max":true` trades the whole affordable/held amount (the shift quantity
prompt's default). The modal consumes the queued quantity inside the same
handler, so no game state is written outside the UI path; the command waits
for that consumption before returning (`504` if the modal never applies it),
which keeps consecutive `tons`/`max` trades ordered even when the resulting
cargo count is credit-limited and not known in advance. It returns `409` if
the trade center has not published those elements yet.

### UI layout registry (click by intent)

Every reconstructed modal publishes its named control rects (in its own
hit-test coordinate space) via `SdlPlatform::PublishProbeUi` — offer window,
text reader, mission BBS, mission-info, shipyard/outfitter stores,
shipyard-info, and every `UiWindow` setup dialog generically
(`ui_dialog_ditl_<id>`, buttons named by title: `ok`, `cancel`, ...). Common
names: `window`, `accept`/`decline`, `done`, `take`, `leave`, `buy`,
`sell_or_info`, `previous`, `next`, `abort`, `list`, `description`,
`scroll_up`, `scroll_down`. The Mission BBS additionally publishes one
`mission.<template_id>` rect per available row (the same zero-based id as
`missions.missions.N.template_id`), so a scenario can select a specific row
instead of relying on the default first entry. The trade center similarly
publishes `trade.row.<n>` for its 8 commodity rows and lists them in the
`items` array with `label` and `price` (so a scenario can assert
`ui.items.0.price` as well as click the row). The shipyard, hire-escort and
outfitter stores publish one `store.slot.<n>` rect per filled cell on the
current page as label/price items whose label is the item's scenario short
name, so a harness can page (`previous`/`next`) until the wanted ship or outfit
appears, click its cell, and assert `ui.items.<n>.selected` afterwards. The
store windows are named `outfitter`, `shipyard` (purchase) and
`shipyard_hire` (the Bar's Hire Escort action); the third action button is
`buy` / `sell_or_info` (Sell for the outfitter, Info for the shipyard).
Every `items` entry carries `"selected": true|false`, the list's current
highlight, so the harness can verify a trade row or store cell selection
without a screenshot. `GET /probe/ui` returns the
active modal's rects
**plus `window_size` and `playfield`** — the current window point size and
the 640x480 canvas rect — so a harness can convert backing-store screenshots
(the BMPs are physical pixels = window points x display density) to window
points exactly instead of guessing the density. `POST /probe/click
{"element":…}` resolves the rect center and clicks it (`409` with a hint if
the element isn't published). New modals should publish their controls too —
it is 3–5 lines next to the layout struct plus one `ProbeUiAutoClear` guard,
and doubles as documentation of the dialog's controls.

### State queries

`summary` (default), `player`, `missions`, `ships` (active NPCs in the
current system), `cargo` (credits, `capacity` = the player hull's own holds,
`fleet_capacity` = hull + eligible escort freighters' holds, the 6 commodity
bins and any non-zero junk), `travel`, `system`. Floats are rounded to 2 decimals; ids
are the reimplementation's zero-based/rebased ids unless the field name says
otherwise. Extend `ProbeState_Snapshot` as subsystems are reconstructed —
prefer small typed queries over one giant dump.

### Log tailing

`NovaLog::Write` double-writes every console line into a bounded ring
(1000 lines, sequence-numbered). `GET /probe/logs` returns everything since a
sequence number, so a harness can `tail` continuously: fetch once, then pass
the returned `tail` value as the next `since`. All JSON string output is
valid UTF-8: game strings are stored as MacRoman, so the serializers decode
any string that is not already valid UTF-8 (`game::NovaText_EncodeUtf8`),
which is what lets a strict `json.loads` read responses containing names such
as `Kiniké` or `Xtreem™ Rocket-Boards`.

### Mission-script / control-bit tracing

Mission scripts and Nova control-bit set strings are silent in the original
(`Mission_ExecuteMisnScriptEngine` 0x00449370 has no logging), so an opt-in
trace records what ran and why. Enable it at startup with `EVN_MISSION_TRACE`
(`1`/`on`/`commands` for one line per executed command and control-bit write,
`full` to also dump each raw script before it executes), or toggle it live:

```sh
curl -s -X POST -d '{"cmd":"mission_trace","enabled":true}' localhost:8190/probe/command
curl -s -X POST -d '{"cmd":"mission_trace","enabled":true,"mode":"full"}' \
  localhost:8190/probe/command
curl -s -X POST -d '{"cmd":"mission_trace","enabled":false}' localhost:8190/probe/command
```

Traced lines go through `NovaLog` and are therefore tailed from
`/probe/logs`; they are prefixed `mission-trace` and carry the call-site
reason (`OnAccept`, `cron OnStart`, `stellar OnDestroy`, `nebula OnExplore`,
...) plus the mission slot and script offset. Malformed commands are logged
(`WARN`) regardless; unmodelled opcodes are logged (`TODO(decomp)`) and every
executed command and control-bit write is recorded only while tracing is on.

## Example session

```sh
EVN_PROBE=1 build/release/src/evnova &
curl -s localhost:8190/probe/state | python3 -m json.tool
curl -s -X POST -d '{"cmd":"step","frames":60}' localhost:8190/probe/command
curl -s -X POST -d '{"cmd":"accelerate","enabled":true,"speed_multiplier":10,"suppress_audio":true}' localhost:8190/probe/command
curl -s localhost:8190/probe/screenshot -o frame.bmp
curl -s -X POST -d '{"key":"I"}' localhost:8190/probe/key   # open missions
curl -s "localhost:8190/probe/logs?since=0" | jq .
curl -s -X POST -d '{"cmd":"quit"}' localhost:8190/probe/command
```

## Not yet (deliberate)

- Deterministic seed control.
- PNG encoding (BMP keeps the dependency set at zero).
- Write-access to game state (intentionally excluded; see the safety model).

### Accelerated execution

The probe can scale gameplay time while the process is running. This is
deliberately a probe-command-only setting; ordinary startup, preferences, and
command-line execution remain unchanged:

```sh
curl -s -X POST -d '{"cmd":"accelerate","enabled":true,"speed_multiplier":10}' \
  localhost:8190/probe/command
```

`enabled` defaults to `true` and `speed_multiplier` defaults to 1. A value of
10 advances gameplay time at ten times wall-clock speed regardless of the
unlocked render rate. Accelerated frames disable renderer VSync and skip the
normal frame yield; modal loops and spaceflight use the same pacing hook. Send
`{"cmd":"accelerate","enabled":false}` to restore ordinary gameplay-clock
speed, pacing, and VSync without discontinuity in the gameplay clock.
Presentation, resource loading, and render-side effects remain active; this
is not a renderer-free headless mode.

Set `"suppress_audio":true` on the command to stop physical SFX and music
output while accelerated. Suppression happens in the audio backends. SFX
still create logical voices timed by the gameplay clock, so hyperspace's
warp-up completion gate and no-stack sound checks retain their normal
simulation-time behavior. Disabling acceleration restores audio output.
