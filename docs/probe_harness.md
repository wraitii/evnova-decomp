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
| `POST /probe/command` | `{"cmd":"pause"\|"resume"\|"step"\|"quit", "frames":N}` |
| `POST /probe/key` | `{"key":"I"}` tap; `{"key":"I","down":true\|false}` hold/release (modal-loop channel, synthetic `SDL_Event`s) |
| `POST /probe/click` | `{"element":"accept"}` clicks the named rect published by the active modal; `{"x":553,"y":508}` clicks raw window points (motion + button-down pair) |
| `POST /probe/hold` | `{"keys":["W","SPACE"],"down":true}` virtual held keys merged into `PollFlightInput` (flight channel — `SDL_GetKeyboardState` cannot see injected events) |

Unknown queries/paths return `400`/`404` with a hint. Requests that need the
main thread time out with `504` if it never pumps (e.g. blocked in a native
modal). While paused, state/log reads still work (the pump services them);
screenshots return the last captured frame.

### UI layout registry (click by intent)

Every reconstructed modal publishes its named control rects (in its own
hit-test coordinate space) via `SdlPlatform::PublishProbeUi` — offer window,
text reader, mission BBS, mission-info, shipyard/outfitter stores,
shipyard-info, and every `UiWindow` setup dialog generically
(`ui_dialog_ditl_<id>`, buttons named by title: `ok`, `cancel`, ...). Common
names: `window`, `accept`/`decline`, `done`, `take`, `leave`, `buy`,
`sell_or_info`, `previous`, `next`, `abort`, `list`, `description`,
`scroll_up`, `scroll_down`. `GET /probe/ui` returns the active modal's rects
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
current system), `travel`, `system`. Floats are rounded to 2 decimals; ids
are the reimplementation's zero-based/rebased ids unless the field name says
otherwise. Extend `ProbeState_Snapshot` as subsystems are reconstructed —
prefer small typed queries over one giant dump.

### Log tailing

`NovaLog::Write` double-writes every console line into a bounded ring
(1000 lines, sequence-numbered). `GET /probe/logs` returns everything since a
sequence number, so a harness can `tail` continuously: fetch once, then pass
the returned `tail` value as the next `since`.

## Example session

```sh
EVN_PROBE=1 build/release/src/evnova &
curl -s localhost:8190/probe/state | python3 -m json.tool
curl -s -X POST -d '{"cmd":"step","frames":60}' localhost:8190/probe/command
curl -s localhost:8190/probe/screenshot -o frame.bmp
curl -s -X POST -d '{"key":"I"}' localhost:8190/probe/key   # open missions
curl -s "localhost:8190/probe/logs?since=0" | jq .
curl -s -X POST -d '{"cmd":"quit"}' localhost:8190/probe/command
```

## Not yet (deliberate)

- Deterministic virtual clock / seed control (phase B of the design; needs
  the `SDL_GetTicks()` sweep through `SdlPlatform::ticks_ms`).
- PNG encoding (BMP keeps the dependency set at zero).
- Write-access to game state (intentionally excluded; see the safety model).
