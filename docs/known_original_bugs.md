Bugs and odd behaviour of the original Nova, plus items to verify.
Keep entries succinct; put addresses, evidence and port behaviour at the `BUGFIX(original)`/`TODO` site in `src/`.
Corrected bugs go through `kApplyOriginalBugFixes` in `src/game/compatibility.hpp`.

## Stuff that needs verifying with an original Nova copy

Behaviour that the disassembly seems to say exist, but I don't remember:
- Ships with an inherent combat government (Polaris arachnid/scarab/raven, Federation destroyer/carrier, rebel ships) should draw attacks from governments hostile to that inherent gov, even when not directly hostile. (unverified)
- **Tech-level availability markdown stops at TechLevel 5.** `Outfit_ComputeScaledPurchasePrice` applies the `100 - 3*(stellar - item)` markdown only when both item and stellar TechLevel are `<= 5` (0x0049d65a / 0x0049d662, signed `JG`, no clamping), so no base-tech-6/7 stellar (Earth, Spacedock I-V, New England, Rebel I/II, Harbor) discounts anything, and the whole 6-7 ship/outfit band is excluded. That bound also is not the minimal way to exclude the sentinel techs (999/9999/32767). Confirmed in the decompile; the port reproduces it faithfully for now (see the `POSSIBLE-BUG(original)` note in `NovaLanded_ScaledStorePrice`).


## List of known engine issues

Per http://asw.forums.cytheraguides.com/topic/22013/comprehensive-list-of-known-bugs-in-ev-nova/22  
Per http://asw.forums.cytheraguides.com/topic/22191/a-list-of-nova-engine-eccentricities  
Per the discord.


### Proper bugs

* **NCB test expressions use a primitive flat operator chain, not precedence.** The Bible warns about this under “Test expressions” (`EV Nova Bible.html`, ~lines 293-298: “Note that since the Nova evaluator is fairly primitive, it may do unpredictable things if you give it an expression like b1 & b2 | b3 ... instead, use proper parentheses”). Ghidra `NovaExpression_EvaluateBoolean` (0x00449020) keeps one result register (`EBP`) and reloads it from the last operand (`ESI`) at every `&`/`|` (the `MOV EBP,ESI` at 0x004492c4 / 0x004492d4 inside the 0x004492c0 / 0x004492d0 operator handlers). For a flat chain of bare Boolean terms only the final operator and its two adjacent operands survive: `b1 | b2 | b3` with bits 1,0,0 evaluates false, and `b1 & b2 & b3` with bits 0,1,1 evaluates true. Parenthesised groups recurse into a fresh accumulator, so the Bible's workaround `b1 & (b2 | b3)` / `(b1 & b2) | b3` behaves as written; for an all-`&` chain use `(b1 & b2) & b3` — a single enclosing `(b1 & b2 & b3)` does not help, because the inner chain is still flat. Group and bare-token operands also update the last-operand register differently, so the reduction is not simply “last pair”. The port reproduces this faithfully (`ParseChain` in `src/game/scenario_data.cpp`; regression tests in `tests/scenario_data_test.cpp`).
* (fixed) **Weapons using another weapon as ammunition produce incorrect free-mass figures** in the ship-info dialog. 
* (fixed+reworked) **Government borders are clipped to a fixed 512×512 map area**, breaking larger custom map layouts. 
* (fixed) **Small asteroids disappear at high resolutions** unless their sprites are padded to roughly 100×100. Fixed in `SpaceflightView::WrapAsteroids`: the port derives the keep-alive window from the largest loaded asteroid frame span instead of the record's own span and wraps records in world space, so the original's despawn/ring-refill miss cannot recur. Not routed through `kApplyOriginalBugFixes` (renderer-structural divergence, no per-record `Sprite`); see the `BUGFIX(original)` note at the site. 
* (fixed) **`FragCount = 1` produces zero fragments**, despite the documented formula implying exactly one. Fixed via `kApplyOriginalBugFixes` in `Asteroid_SpawnDestructionPackage` (0x00462550): keep the original roll and raise the floor baseline to one (`BUGFIX(original)`). 
* (fixed) **One-way system links become reciprocal** — linking A→B also permits B→A. 
* (fixed) **`Mxxx` behaves like `Nxxx`** instead of positioning the player at the first stellar/system center. 
* (fixed) **`DeathDelay` 0 or 1 leaves an immortal ghost sprite** after a ship explodes. 
* (fixed) **Zero inherent shields break escape-pod ejection** — the replacement ship can immediately explode after ejecting. Fixed in `PlayerTick_TimedActionTransition`: the post-respawn armor refill uses the max-armor value.
* (fixed) **Rank discounts do not apply to outfits**, despite the rank field being documented as affecting ships and outfits. Fixed in `NovaLanded_OutfitPrice`: charge the rank price. Tech-level bug remains active.
* (fixed) **Auto-aborted missions omitted their competing-government reputation reward.** Fixed in `Mission_ResolveMisnSlot`: run the `CompGovt`/`CompReward` walk for missions flagged pay-on-auto-abort (`Flags2 0x0002`) or reversal-on-abort (`Flags 0x0040`).
* **“Attack Enemy Spobs/Stellars” AI does not work correctly**; later testing reproduced the problem, although the poster still noted some uncertainty about AI setup.  
* **Windows startup music filename is hard-coded** to `Nova Music.mp3` rather than respecting STR# 130 like the Mac version. 
* **Volume labels cannot be replaced through STR# 136**, despite older versions supporting it. 
* **Pure-white pixels in `cicn`s become transparent** regardless of the mask. 
* **Negative `DatePostInc` does not work.** 
* (confirmed, unfixed) **Beam collision detection has holes** — a beam never tests its segment against a hull. `Shot_UpdateBeamHitQueue` (0x0042f270) accepts a candidate only when its **center** is inside a per-ship forward sector (half-angle `trunc(frame_span*0.66)*10/32` deg, reach `BeamLength + ceil(trunc(frame_span*0.66)/2)`), picks the nearest accepted center, then truncates the endpoint to `distance - 0.2*frame_span`. `frame_span` is the sprite's full width (for square tiles, its longest dimension), so the sector's lateral tolerance `d*tan(cone)` is narrower than the hull at close range (the beam visibly passes through the side of the ship) and wider at long range (phantom hits whose endpoint lies in empty space); elongated hulls are sized by their length and over-hit edge-on. No occlusion or time-of-flight, so a near ship can be skipped while a farther one is hit. Reproduced faithfully by `NovaWeapon_TickBeamHitQueue` (`src/game/weapon_shots.cpp`); not routed through `kApplyOriginalBugFixes`. 
* **Finite auxiliary mission ships disappear after save/quit/reload** before the mission is completed. 
* **RLE colour runs ignore colouring, transparency and murk.** 
* (left in, seems on purpose) **The starfield ignores murk.**
* **Carried fighters ignore configured exit points** and always launch from the carrier's center. 
* **Player turn rates are quantized to multiples of 10** despite finer values being displayed; AI ships are not affected. 
* **IFF jamming creates inconsistent spob interaction** — landing can remain forbidden even though hailing treats the spob as friendly, making normal bribery unavailable. 
* **Multi-ship cargo-retrieval missions mishandle cargo** — the first target gives the full cargo amount, later targets require the same free space but yield 0 tons. 
* **Nebula image selection chooses a too-small image and scales it up** rather than scaling a slightly oversized image down. 
* (fixed) **Max-guns/max-turrets outfit bonuses do not stack.** The executable added the first ModType-45/46 value once per owning outfit definition, so N copies of one modifier gave its bonus once. Fixed in `Outfit_ClampOwnedCountToCurrentLimits` (0x004656a0) under `kApplyOriginalBugFixes`: scale by the owned count, matching the Bible's per-item "add to max" and ModType 27's explicit per-copy rule. The shipped "Sigma Mount Reinforcement" has `Max` 1, so the bug was latent in the base scenario; the faithful behavior is kept when the policy is off. 
* (fixed, ungated) **`Hxxx`/probably `Exxx` ship changes omit carried fighters**, leaving the new ship's bays empty; the initial player ship has the same problem. Mechanism: `default_weapon_secondary` is written at `ammo_or_energy_cost_code` with no mode-99 special case (C/E/H arm 0x00449370 and new-game seeding 0x00489d70), but a mode-99 carried-ship bay's `AmmoType` is the carried ship class id and every read/spend path (`Weapon_GetWeaponBurstAttempts` 0x0046f2c0, `Weapon_CanFireWeaponBank`, `Weapon_FirePlayerWeaponBank`) and `Player_ReplaceShipWithCapturedHull` (0x00423fa0) use the bay's own counter, so the newly seeded bay reads empty. 74 shipped stock loadouts are affected. The port keeps the count in the bay counter in `NovaWeapon_SeedBanksFromShipStock` and `NovaWeapon_AddShipClassStockBanks` (`BUGFIX(original)`); the original class-slot write is not reimplemented and the fix is not yet gated through `kApplyOriginalBugFixes`. 
* **Visbit-swapped systems can retain the previous system's map colour and message-buoy string.** 
* **A brief tractor-beam hit can permanently paralyse an AI ship** until it is hit by the tractor again. 
* **Map outfits can fail to reveal systems that are actually within range** because Nova does not always calculate the shortest hyperlink path. 
* **Travel-stellar objectives can complete before required special-ship cargo is collected.** 
* (fixed, ungated; unverified in original) Accepting a mission in flight can count its destination as visited just because it matches the selected planet or jump-slot number. The port now requires the player to be landed. Nested `OnAccept` mission starts still inherit the open offer window.
* **Ship trade-in value can disagree between the Extras pane and shipyard.** 
* **Point-defense beams can reacquire targets outside their normal range.** 
* (perhaps wanted?) **`Gxxx` in an outfit's `OnBuy` prevents the purchased outfit itself from being granted**; `Dxxx` in `OnSell` has the symmetric problem. 
* **Cröns contain multiple broken behaviours.** The thread treats this as a family of engine bugs rather than one isolated defect. 
* **Random mission travel/return stellars can fail when the originating system will change the following day.** 
* **Dude-specific advice STR# resources sometimes fail to load all entries.** 
* **Always-dominated spobs show a disabled-looking “Leave” button that remains clickable.** 
* **Beam decay can stack successive beam instances into an “auto-machine-gun” effect**, greatly multiplying damage. 
* **Weapon timing mixes frame-based and fixed 1/30-second timing**, making beam continuity and damage-per-second depend on machine framerate. 
* **AI cloaking has several broken transitions** (hyperspace cloak state: no cloak on jump-in or system departure; approach-uncloak only works for carried fighters; docking does nothing). 
* (discovery, fixed) **Fast-cloaking is gated on the swarm behaviour flag instead of the device's fast-fade bit**. Fixed under `kApplyOriginalBugFixes` in `NovaShip_TickCloakFadeState`. 
* (discovery, fixed) **Cloaks that drain shields can't drain to 0 and can be activated at 0 shields**. 
* (discovery, fixed) **Cloaks that don't drain fuel still require fuel to operate.** 
* **Pirate AI can make very-low-armour ships effectively invulnerable** because it refuses to destroy targets but cannot disable ships with ≤3 armour. 
* **Windows hyperspace flash does not build up like the Mac implementation, and its tunnel scalar uses the wrong formula.** The Mac hold computes `FLOAT_007354a0 = (progress - 55) * 5`, clamps it to [0,100], and requests one asynchronous 1.5 s `_FadeWhiteIn` (`0x546f`); at hold end it sets the scalar to -1 and requests `_FadeWhiteOut` (`0x54d1`). The Windows release computes `FLOAT_007354a0 = progress * 0.3 - 15` (`0x00450601`) and calls the stubbed `NoSys_NoOp_00467e60` (`0x00467e60`, a bare `RET`), so the hold shows no build-up and only a one-frame white flash lands at arrival. The hypergate transfer likewise paints white then runs a gated `_FadeWhiteOut` (`0x63c40`); the wormhole paints white but resets the starfield with no fade. The port follows the Mac behaviour unconditionally (not gated by `kApplyOriginalBugFixes`), and `hyperspace_effects` selects white vs black as in the CE colour setter (`0x00872384`) rather than disabling the effect.
* **`DispWeight` does not control mission ordering**; Mac 1.1 presents missions by increasing resource ID instead. 
* **Economy-at-Work (asteroid miners) ships can become hostile when the player requests assistance.** 
* (fixed, gated) **Crön date ranges split day/month and year ranges incorrectly**, so a range such as 1/1/1178–1/1/1179 only fires on two calendar dates rather than throughout the interval. Corrected in `CronEventDateWindowAllows` under `kApplyOriginalBugFixes` (the month/day bound applies only at the boundary year); the original is reproduced when the policy is off. 
* (fixed, gated) **`Random` 0 still fires a crön.** The per-day roll is `NovaRandom_Range(0x65)` = 0..100 inclusive tested with `<=`, so `Random` 0 activates on roll 0 (~1/101 of eligible days) and the effective odds are `(Random+1)/101`. Corrected under `kApplyOriginalBugFixes` (rolls 1..100). 
* (fixed, gated) **A zero-duration crön runs its OnEnd script twice.** After the OnStart+OnEnd pair the slot stays active with a zero holdoff, so the next daily tick runs OnEnd again before deactivating (49 of the 125 shipped cröns use `Duration` 0). Corrected under `kApplyOriginalBugFixes`. 
* (fixed, gated) **A crön's post-end wait is loaded from `PreHoldoff`, not `PostHoldoff`.** On duration expiry the engine re-arms the holdoff counter from `PreHoldoff` (0x004395d9 reads block +0x22) whenever `PostHoldoff > 0`. With `PreHoldoff == 0` the slot never deactivates and re-runs OnEnd every day; with `PreHoldoff > 0` it re-runs the full OnStart+OnEnd pair after the wait. The duration-countdown arm also leaves `duration_counter` at 0, so the post-wait arm re-activates instead of retiring the slot. Shipped `V Rare Aur/Fed Syst Change` (0x00b8/0x00b9) and `Reb/Fed Syst Change 1` (0x00c0) therefore only ever fire once per game. Fixed in `Mission_TickDailyCronEvents` under `kApplyOriginalBugFixes` (uses the Bible's `PostHoldoff`, and latches `duration_counter = -1` after OnEnd); the original is reproduced when the policy is off. 
* **Bay-launched fighters receive only 75% of normal damage** while regenerating at the normal rate. 
* **A shield-breaking hit also applies the weapon's full armour damage**, rather than only the unused portion of the hit. 
* **AI-fired weapons and their submunitions cannot damage the ship that fired them.** 
* **Negative recoil does not work for the player.** 
* **`chär` Initial Record values can fail to set the player's initial legal status correctly.** 
* **`përs` ships will not offer missions whose destination or return stellar is in the current system.** 
* (fixed) **A mission that aborts itself in `OnAccept` and starts another mission displays the second briefing twice.** Mechanism: the mission-script engine scans the shared `g_reaction_script_buffer` (DAT_007c8a10) and re-reads its length every byte (0x00449370), while the `S` opcode's activation runs the new mission's `OnAccept` payload through `Mission_RunMisnScriptPayload` (0x00448050) into that same buffer, so the outer scan continues over nested-payload bytes. Fixed in `Mission_ExecuteReactionScript`: the port passes an explicit `std::string_view`, so the outer script completes normally and the second briefing is shown once. Not gated by `kApplyOriginalBugFixes` yet and there is no path that reproduces the broken behaviour; see the `BUGFIX(original)` note at the site and the row for 0x00448050 in `decomp-progress.tsv`. 
* **AI ships can launch fighters while cloaked even when the fighter bay is not flagged to fire while cloaked.** 
* **`TravelStel 30000–30255` does not work** for selecting a random stellar belonging to a particular government/class. 
* **The fleet “random cargo” flag does nothing**; fleet-created ships have no cargo when boarded. 
* **Player escorts belonging to governments marked “don't attack player” and/or immune to player weapons can become unable to hit anything at all.** 

### Engine quirks

* (identified, reworked) Carrier escorts use weapon 1's range to decide attack vs. defend. 
* **Non-strict play gives the player +50% top speed**, with no independent switch for the bonus. 
* **Escape-pod destination/location is undocumented**, so plug-in authors cannot reliably infer where the player will reappear. 
* **Non-simultaneous weapons cannot fire more than once per rendered frame.** 
* **A `përs` absent when a pilot is created is treated as permanently dead** (a plugin added mid-run never makes the person appear). 
* **Mission persistence after failure depends on `CanAbort` rather than simply whether a fail text exists.** 
* **Weapon firing arcs are calculated from the firing ship's center rather than the weapon exit point**, allowing geometrically strange shots. 
* **`shan` flag 0x0002 unfolds ships both for hyperspace and landing**; the poster considered the landing behaviour undesirable but not necessarily broken. 
* **Zero-speed warships are automatically treated as defence platforms** and positioned around system center. 
* (identified, unfixed, seems ok) **AI ships told to scoop asteroid debris but unable to do so just shuttle between stellars.** 
* **Duplicate `përs` resources are treated as one person with increased appearance chance.** 
* **Projectile falloff also controls visual fading** — positive values fade out and negative values fade in. 
* **Mission-offering `përs` ships evaluate the player's legal status in the current system**, not their own government's relationship with the player. 
* **Landing/takeoff always advances one day**, regardless of ship type. 
* **Any amount of outfitter buying/selling costs one additional day.** 
* **Buying a ship costs four days.** 
* **Upgrading/selling escorts costs additional days based on escort count**, while hiring escorts costs no time. 
* **All accumulated landing activity time is applied only on takeoff.** 
* **Only the highest-ID buyable/sellable `jünk` appears in the Trade Center** when multiple entries qualify. 
* **`öops` adjusts from a commodity's medium price rather than the local spob price**, and legal status does not affect it. 
* **`ränk` IDs must fall between 128 and 256.** 
* **Rank flags affect the associated government and its allies**, not just the named government. 
* **Mission ships generally cannot enter through a hypergate unless following/pursuing the player.** 
* **A newly spawned mission fleet cannot be made to appear as though leaving a nav default or decloaking.** 
* **“Protect player” special ships follow the player through a hypergate rather than waiting on the far side.** 
* **Mission special ships cannot use the government-based `ShipSyst` selectors that AuxShips can use.** 
* **“Appear randomly cloaked” behaves differently on first entry versus later entries**: initially the ships hyper in; later they appear at a random cloaked location. 
* (discovery, unchanged) **AI ships will cloak when hyperspacing allows the player to jump-while-cloaked**. 
* **Aborting a mission in `OnShipDone` prevents an `OnShipDone` description from appearing.** 
* **A never-collected cargo requirement can be abused as a free-cargo-space test** for later missions. 
* **Fleet-created ships ignore generic government hail quotes** and fall back to “Greetings.” 
* **`përs` ships likewise ignore generic government hails** unless given a specific quote/disaster-info setup. 
* **System-summoned persons are already present when the player enters**, and are not spawned again until the system is re-entered. 
* **Two `përs` with exactly the same name count as one person, but adding a trailing space makes them distinct.** 
* **System-summoned fleets always arrive after the player**, via hyperspace/hypergate, rather than being present beforehand. 
* **Scan-triggered mission failure requires the scanning government to have a smuggling penalty ≥1 as well as a matching scan mask.** 
* (harcoded to avoid mod weirdness) **The combat-rating system's base unit is ship class 0's Strength** (the Shuttle, 2), read unindexed by the NPC fire-cooldown ladder (`0x00414ea8`), the afterburner roll (`0x0046b308`), and combat-odds player scaling (`0x0041343c`). The port pins 2 (`GameState::kCombatRatingBaseStrength`) so a mod editing class 0 cannot rescale the rating system. (The afterburner divides by this base, not the ship's own Strength; the port previously misread that.) 

## Datafile bugs

* **mïsn 428 (`Federation Resupply;Fed1`) AvailBits has a missing `b` prefix**: `!(b511 | b515) & !((b50 | 467) | b6666)` uses `467` instead of `b467`. Nova's tokenizer reads a bare digit run as a `#` compare-value token that is a no-op in boolean position, so the original evaluates the expression as if the `467` were absent (`!(b511 | b515) & !(b50 | b6666)`); bit 467 is never consulted. The port reproduces this (the evaluator consumes the whole digit run) rather than silently correcting it to `b467`, which would change mission availability from the original. The nearby mïsn 150 (``!(b275 | b512) & !((b511 | b515) | b6666)``) shows the same expression shape. 
* **STR# 4001 and 4002 are missing entry 80, `*Samantha`.** 
* **Ancient Vell-os Sculpture has a typo on Windows**: `"ancient Vell-s sculpture"`. 
* **Kymonth Station description uses “bought” instead of “brought.”** 
* **DESC 4135 contains malformed wording**: `"Upon seeing you a there is a sudden hush"`. 
* **DESC 4544 has incorrectly escaped/mismatched quotation markup.** 
* **DESC 9307 has missing quotation marks.** 
* **DESC 9368 says “as you make her way over.”** 
* **Stock asteroid sprites are too small to survive the high-resolution asteroid engine bug** and need padding. Not needed in the port, which sizes the asteroid keep-alive window from the largest loaded frame span rather than each record's own sprite. 
* **The Vell-os area-map data is configured so it only works every three days**; changing the cron setup makes it work daily. 
* **Flower of Spring, Summer Bloom and Winter Tempest retain beam decay values that trigger the engine's broken decay behaviour.** 
* **Auroran Cruiser and Thunderforge have garbage `moviefile` fields**, which can crash WinNova when they are selected in the shipyard. 
* **Stock multiple Ion Cannons/BRLs/TBRLs don't stack damage correctly** because their data runs into the once-per-frame weapon limitation. 
* **Vell-os abilities are lost after ejecting**, something the stock scenario could work around in its data. 
* **The stock scenario permits multiple story strings on the same pilot**, described as an exploitable scenario-data problem. 

## Datafile quirks

* **The 200mm railgun is arguably worse overall than the 150mm** (balance choice, not an engine failure). 
* **The Fission Reactor is less efficient than Solar Panels** in both mass/cost terms with no compensating benefit; another stock balance oddity. 
* **The “Set and launch trap” mission is tuned extremely aggressively**, with escorts often destroying the target almost immediately. 
* **United Shipping uses an intermediary government to confine rank benefits to itself** (stock-data workaround for rank-flag propagation). 
* **Packaging/data hygiene oddities**: redundant `Nova Ships 8`, race-movie resource forks, wrong music type/creator codes, `Nova-DF.rsrc` checksum suggestion. 
