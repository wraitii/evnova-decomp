# Mission dialogs — scoping handoff

Self-contained brief for a fresh session picking up fidelity work on EV Nova's
mission windows. Ground truth is the Ghidra DB
(`./tools/ghidra_api`, `docs/ghidra_api.md`); coverage lives in
`decomp-progress.tsv`. Read `docs/mission_reimplementation_overview.md` first —
it has the mïsn/MisnActive data model this work consumes.

## Scope: which functions count as "mission dialogs"

Three modals plus their shared plumbing:

| Modal | Port entry point | Ghidra (original) |
|---|---|---|
| Landed Mission BBS | `RunMissionBbsWindow` — `src/game/docked_mission_dialog.cpp:489` | `0x0043C470` run, `0x00440C90` poll, `0x00441620` draw, `0x004A1130`/`0x004A1290` buttons |
| Mission-ship / offer window | `NovaMission_RunOfferWindow` — `docked_mission_dialog.cpp:819` | `0x00442510` run, `0x00447170` poll, `0x00447680` draw, `0x004A1520`/`0x004A1820` buttons |
| In-flight mission computer | `NovaMission_RunMissionInfoWindow` — `docked_mission_dialog.cpp:1324` | `0x00446150` run, `0x00446770` poll, `0x00446E00` draw, `0x00445DC0` list rebuild, `0x004A13C0` hit-test |
| Acceptance readers (shared) | `NovaUi_RunTextReaderDialog` — `src/game/selection_text_dialog.cpp` | `0x004982A0` reader, `0x004A2AC0` draw |
| Player Info (opened from BBS/offer action 9) | `NovaPlayerInfo_RunWindow` — `src/game/player_info_window.cpp` | `0x00499C10` |
| Starmap (opened from BBS/offer action 6, mission-computer action) | `NovaStarmap_RunWindow` — `src/game/starmap.cpp` | `0x004A3AA0` |

Out of scope but adjacent: mission list evaluation (`Mission_EvaluateMissionLists`
`0x0043CF00`), eligibility (`0x00441B40`), activation
(`Mission_ActivateMissionAtSlot` `0x0043F100`) — all ported in `src/game/mission.cpp`
and working; the dialogs sit on top.

## Current tracker state (`decomp-progress.tsv`)

```
0x0043C470  72%  NovaUi_RunMissionBbsWindow
0x00440C90  60%  NovaUi_PollMissionBbsWindow
0x00441620  55%  NovaUi_DrawMissionBbsWindow
0x00442510  55%  NovaUi_RunMissionShipInteractionWindow   (offer run)
0x00447170  55%  NovaUi_PollMissionShipInteractionWindow   (offer input)
0x00447680  55%  NovaUi_DrawMissionShipInteractionWindow   (offer draw)
0x00445DC0  60%  NovaUi_RebuildMissionComputerList
0x00446150  85%  NovaUi_RunMissionComputerWindow
0x00446770  50%  NovaUi_PollMissionComputerWindow
0x00446E00  60%  NovaUi_DrawMissionComputerWindow
0x00448A30  55%  NovaUi_DrawListRowCallback
0x00499C10  75%  NovaUi_RunPlayerSpecialInteractionWindow     (Player Info)
0x004A1130  70%  NovaUi_HitTestMissionBbsActionButtons
0x004A1290  80%  NovaUi_DrawMissionBbsActionButtons
```

Several low percentages are input/paint approximations (flat rows vs the
original native list control), not missing game logic.

## Where this stands

The gameplay sub-actions are ported. The BBS polls actions 6/9/10 and the
offer polls 4/5/7 through the shared rebindable command slots (map 9, Player
Info 0x19, mission computer 0x28), with the flags-0x100 destination preselect
and the `ai_secondary_target_slot`/`travel_transfer_mode` save+restore shared
via `RunNestedMissionStarmap`. The BBS leaves when a mission script has staged
`pending_overlay_message` (0x00440c90 action 7). `NovaMission_RunMissionInfoWindow`
and `NovaPlayerInfo_RunWindow` build on a `render_background` callback instead
of `SpaceflightView &`/`HudRenderer &`, so they open from docked context too;
`SdlAudio &` is threaded through the docked dispatch so the mission computer
plays its cues.

The shared reader/offer custom-art path (T4) landed: both the text reader and
the offer window honor the dësc `dialog_variant`, so the ~68 shipped missions
with variant briefings render their art and the 10 shipped offers in the
`mission_id + 4000` range use the 0x3fc layout. The native list control (T3)
and shared row painter (T7) landed in commit 6987616, the offer/reader scroll
arrows (T5) are done, and a successful BBS accept now exits the window
(0x0043c470) instead of rebuilding the list.

## Verified original behavior (decompile evidence)

### BBS poll → action codes (`0x00440C90`, top of function)

- **Pending overlay force-exit**: `if (g_pending_overlay_message[0] != 0) { action = 1; param_3 = 7; }` — a mission-script `Q` staged a message, so the BBS leaves instead of drawing it. Ported (action 7).
- **action 6** — starmap, requested when map command (`g_nova_control_bits._44_2_`) is active. BBS caller (`0x0043C470` ~line 151) preselects the selected mission's destination when MisnDef flags 0x100 is set (travel_stellar_id, else return_stellar_id), saves/restores `ai_secondary_target_slot` + `travel_transfer_mode`, then refreshes the travel overlay.
- **action 9** — player special window, requested on player-special command (`_76_2_`).
- **action 10** — mission computer, requested on mission-computer command (`_106_2_`). BBS caller only opens it when ≥1 active mission lacks flags 0x400 (the visible filter).
- **action 7** — leave.
- **action 1** — accept (`Mission_ActivateMissionAtSlot`); on success the run
  loop's exit flag is set, so the BBS closes after the acceptance UI; on
  failure the description is reloaded and the window stays open.
- Arrow/Tab keys (Mac keycodes 9/10/11) walk the native list selection.

### Offer poll → action codes (`0x00442510` tail)

- **action 1** — accept.
- **action 2** — decline: optional aux text (`payload +0x58`), then
  `Mission_ExecuteReactionScript(payload +0x25a)`. Ported (see
  `docked_mission_dialog.cpp` decline arm).
- **action 4** — starmap with the flags-0x100 destination preselect and
  `ai_secondary_target_slot`/`travel_transfer_mode` save/restore.
- **action 5** — player special window.
- **action 7** — mission computer (only when a visible active mission exists).
- **action 9/10** — text-view scroll up/down, gated on
  `g_selection_editor_maxed` / `g_selection_editor_active`.
- **variant ≥ 0x80** (`g_selection_dialog_variant`): swap DLOG `0x3f8` →
  `0x3fc` and PICT `0x214a` → `0x2150`, with variant-specific entry-rect
  derivation (`0x00442510` lines ~87..167).
- **status-string panel** (`g_selection_dialog_status_str`): when set (and no
  control bit / flag blocks it) the original plays the status movie
  (`Ui_PlayMovieFileModal` 0x0049db00, the dësc trailing pstring). QuickTime is
  a platform replacement; recorded as a `qt` skip in `decomp-skipped.tsv`.
- **`DAT_00773ee9 == 0 && offer text empty`** → auto-accept with no window.
  Already ported.

### Mission computer (`0x00446150`)

- Creates `UiWindow_CreateFromDialogResource(0x3f4, 0x00446E00, 0x00446770)` —
  the "SpecialInteraction" helpers are this window's draw/poll, and
  `0x00445DC0` rebuilds its active-mission list. These Ghidra names were
  misnomers; they are renamed (see task T8).
- Play open cue `g_nova_control_bits._152_4_` and close cue `_156_4_` via
  `NovaAudio_QueueCenteredSound`; the port plays transition cues directly.
- Starmap action preselects the flags-0x100 destination and saves/restores
  `ai_secondary_target_slot` + `travel_transfer_mode` **only when
  `g_travel_destination_window != 0`** (i.e. the nested starmap actually
  opened). Then `NovaUi_DrawCargoMissionStatusPanel()` + redraw.
- Abort: reputation `-5 * comp_reward_delta` over every system owned by
  `comp_govt_id`, `Mission_ClearMisnSlotAssignments(slot, 1)`, rebuild list,
  close if no active missions remain.

## Tasks

### T0 — Decouple the mission-computer / Player-Info windows from flight
Implemented: both windows take a `render_background` callback instead of
`SpaceflightView &` + `HudRenderer &`, and `SdlAudio &` is threaded through the
docked dispatch. Call sites pass a flight-drawing lambda in flight and the
docked menu callback from the BBS/offer.

### T1 — Offer/BBS sub-window actions
Implemented: actions 6/9/10 (BBS) and 4/5/7 (offer) open the starmap, Player
Info, and mission computer, with the visible-active-mission gate for the
mission computer.

### T2 — BBS pending-overlay force-exit
Implemented.

### T6 — Mission-computer nested-map state restore
Implemented: `RunNestedMissionStarmap` saves/restores
`ai_secondary_target_slot` + `travel_transfer_mode` and plots the returned
destination. The original also refreshes the cargo status panel after the map;
that remains open.

### T8 — Ghidra renames + tracker/comment hygiene
Implemented: `NovaUi_PollMissionComputerWindow`,
`NovaUi_DrawMissionComputerWindow`, `NovaUi_RebuildMissionComputerList`.

### T3 — Native list control (BBS + mission computer)
**Implemented** (commit 6987616) via the shared clean-room `NovaListControl`
(`src/game/nova_list_control.cpp`, Ghidra `NovaList_Create` 0x004d1a60,
`NovaList_GetRowRect` 0x004d1f50, `NovaList_HitTestPoint` 0x004d1db0,
`NovaList_ScrollByRows` 0x004d1fb0, `NovaList_ScrollSelectionIntoView`
0x004d1d40, `NovaList_Draw` 0x004d2010). Both windows own a scroll offset,
clamp it, map clicks through it, move Tab/Up/Down and j/k and scroll the
selection into view, and draw a native scrollbar (frame, trough, chevrons,
proportional thumb): the BBS uses its DITL `0x3ee` UiPanel entry-3 strip, the
mission computer the `0xf`-narrowed right edge from `0x00445dc0`. Thumb
dragging is not reproduced (`TODO(decomp)`); offers past the view are now
reachable.

**Original.** Both mission lists are the game's reusable Mac list widget
(`DAT_00774aec`), not a hand-drawn column: it owns the scroll offset and native
scrollbar, keeps the selected row index (`g_selected_misn_slot_index` for the
BBS, `DAT_0077430a` for the mission computer), and exposes the helpers above;
Tab / Up / Down walk the list and auto-scroll it.

### T4 — Reader / offer custom art (`dialog_variant`)
**Implemented.** The desc's trailing u16 field
(`NovaStellarDescription::dialog_variant`) is a **PICT resource id** — the bar
(`0x0047c8e0`) and shipyard/outfitter dialogs
already load it directly as custom art (`Resource_LoadPictAsImage(variant)`).
When it is `>= 0x80`:
- the **text reader** (used for mission briefings, success/failure, etc.) runs
  in DLOG `0xbbc` with backdrop PICT `0x214f`, and its draw callback
  `NovaUi_DrawSelectionDialogContent` (0x00499870) loads the variant value as a
  PICT and blits it into DITL entry 2;
- the **offer window** runs in DLOG `0x3fc` with backdrop PICT `0x2150`, and its
  draw `NovaUi_DrawMissionShipInteractionWindow` (0x00447680) loads the variant
  value as a PICT and blits it into DITL entry 8.

This is not plug-in-only. In the shipped data, 68 mission definitions have at
least one variant text resource (briefings alone use ~25 distinct PICTs in the
`0x138b`–`0x18a9` range), and 10 shipped offers (`mission_id + 4000`) carry a
variant PICT — e.g. missions 192, 233, 345, 468, 485, 557, 581, 591, 602, 620.
The port threads the field: `NovaUi_RunTextReaderDialog` takes a
`dialog_variant` parameter (>= 0x80 runs DLOG 0xbbc + backdrop PICT 0x214f and
blits the variant PICT into DITL entry 2, with the auto-size arm disabled) and
`NovaMission_RunOfferWindow` switches to DLOG 0x3fc + backdrop PICT 0x2150 and
blits the variant PICT into DITL entry 8. The variant is carried from the
dësc at every desc-based caller: mission briefings and the pickup/LoadCarg
dialog (via `LoadMissionText`), the offer-decline follow-up, the success and
failure debriefs (through `MissionDebriefSink`), About/Acknowledgements, and
the intro epilogue.

Separately, the desc's trailing `status` pstring is a QuickTime filename the
original plays at open/exit through `Ui_PlayMovieFileModal` (0x0049db00), gated
on `g_pref_quicktime_movies`. The port has no movie player — QuickTime is one of
the intended platform replacements — so it is recorded in `decomp-skipped.tsv`
as a `qt` skip (along with `Ui_LoadDescAndPlayMovie` 0x0049e3c0).

### T5 — Text-scroll arrows (hold-to-repeat, page keys)
**Implemented.** The shared `NovaTextScrollView`/`NovaTextScrollHold`
(`src/game/selection_text_dialog.*`) now back **both** the offer window
(`0x00447170`) and the generic text reader (`0x00499440`) — the two callbacks
are the same machine:
- Keyboard: normalized Up/Down scroll ±10px (offer actions 9/10, reader
actions 5/6); Home/End jump to an end; PageUp/PageDown move `0xfa` (250)px.
The old offer DIK `0xc8`/`0xd0` codes never matched a `TextInput` (normalized
Up/Down are `0x61`/`0x66`).
- Every scroll action is gated on the live `g_selection_editor_maxed`
(can-scroll-up) / `g_selection_editor_active` (can-scroll-down) latches, which
the port computes as `scroll_offset > 0` / `< max_scroll`.
- Mouse: a press on an arrow emits the first 1px step and, while the button
stays down, repeats 1px per 60Hz tick. `SdlPlatform::PrimaryMouseDown()` reads
SDL's real button state, so probe-injected clicks (press-only) stay discrete.
- The arrows are still drawn disabled from the live latches (unchanged).

- The arrows are still drawn disabled from the live latches (unchanged).
- Keyboard auto-repeat: the original's polls route Mac autoKey (event type 5)
  through the same handler as key-down (type 3) — `0x00499440`/`0x00447170`
  both test `type == 3 || type == 5` — so a held scroll key repeats its
  discrete ±10px/jump step at the OS repeat rate. `SdlPlatform::PollTextEvent`
  now surfaces SDL `event.key.repeat` events (previously dropped) and tags
  them `TextInput::repeat`; the scroll handlers consume them unchanged. The
  `maxed`/`active` gates still auto-stop at the ends.

Remaining gap: the smooth `NovaUi_ScrollSelectionText` `param_5 == 0` path is
unused by both callers.

### T7 — Shared list-row painter
**Implemented** (commit 6987616): `NovaUi_DrawListRow`
(`src/game/nova_list_control.cpp`, Ghidra `NovaUi_DrawListRowCallback`
0x00448a30) is the single per-row painter the native list widget invokes: it
fills from the c.lr palette (`list_background`/`list_hilite`/`list_text`),
draws the label with the shared screen font, and applies the row clip. The BBS
(`DrawMissionBbsContents`) and the mission computer both call it; the failed
`0xa5` marker is part of the row string, as in the original.

## Testing / probe

- **Probe UI names** already published: `mission_bbs`
  (`window`,`list`,`take`,`decline`,`description`, plus `mission.<template_id>`
  rows), `mission_offer` (`window`,`accept`,`decline`,`text`,`scroll_up`,
  `scroll_down`), `mission_info` (`window`,`abort`,`done`,`list`,`description`).
  See `docs/probe_harness.md` §"UI layout registry".
- **Scenario smoke test**: `tests/scenarios/tutorial_007_abort.toml` already
  drives BBS accept, `mission_offer`, and `mission_info` abort end-to-end (see
  its `mission_info` waits around line 678). Extend it (or add a focused
  scenario) to cover the wired sub-actions: docked BBS → open mission computer
  → abort, and offer → starmap preselect.
- **Unit tests**: `tests/mission_test.cpp` covers list evaluation, activation,
  wildcards, and offer/decline context; `tests/docked_dialog_test.cpp` covers
  DLOG/DITL layouts; `tests/selection_text_dialog_test.cpp` covers the
  `TextScrollKey` mapping, line/page/jump scrolling and gating, and the
  hold-to-repeat tick math. `tests/desc_probe.cpp` pins the DLOG `0x3fc` and
  `0xbbc` variant-art ordinals (offer entry 8 = item 7, reader entry 2 = item 1).
- Validate with `cmake --build build/release` and
  `ctest --test-dir build/release --output-on-failure`; comment-only changes
  need no build. Probe/scenario runs need host approval — ask before using the
  probe.

## Constraints / conventions

- All modals must present through `SdlPlatform::Present()` and poll the shared
  input channels (probe compatibility).
- Load DLOG/DITL layout from resources; never invent geometry. Native local
  placement + inverse input mapping is the accepted divergence (see
  `docs/dlog_ditl_dialog_format.md` §7.1).
- Preserve original quirks; mark intentional divergences and gate original-bug
  fixes through the compatibility policy.
- Cite the Ghidra address above the port function; keep `decomp-progress.tsv`
  rows current in the same change.

## Recommended sequencing

All planned tasks (T0–T8) are implemented. Remaining mission-dialog gaps are
recorded in `decomp-progress.tsv`: the offer mission-ship/hail branches, exact
list-row/preview rect derivation, thumb dragging, and the platform movie skip
(`Ui_PlayMovieFileModal`).
