Purpose of this doc:
- documenting bugs of the original Nova. Both known and unknown.
- documenting weird behaviour & behaviour to verify.

## Stuff that needs verifying with an original Nova copy

Behaviour that the disassembly seems to say exist, but I don't remember:
- Inherent combat government on polaris arachnid/scarab/raven, federation destroyer/carrier, rebel ships. The player should get attacked by governements that are hostile to the inherent gov, even if they directly aren't hostile.


## List of known engine issues

Per http://asw.forums.cytheraguides.com/topic/22013/comprehensive-list-of-known-bugs-in-ev-nova/22?page=2
Per http://asw.forums.cytheraguides.com/topic/22191/a-list-of-nova-engine-eccentricities
Per the discord.


### Proper bugs

* (fixed) **Weapons using another weapon as ammunition produce incorrect free-mass figures** in the ship-info dialog. 
* (fixed+reworked) **Government borders are clipped to a fixed 512×512 map area**, breaking larger custom map layouts. 

* **Small asteroids disappear at high resolutions** unless their sprites are padded to roughly 100×100. 
* **`FragCount = 1` produces zero fragments**, despite the documented formula implying exactly one. 
* **One-way system links become reciprocal** — linking A→B also permits B→A. 
* **`Mxxx` behaves like `Nxxx`** instead of positioning the player at the first stellar/system center as documented. 
* **`DeathDelay` 0 or 1 leaves an immortal ghost sprite** after a ship explodes. 
* **Zero inherent shields break escape-pod ejection** — the replacement ship can immediately explode after ejecting. Shield outfits do not prevent it.  
* **Rank discounts do not apply to outfits**, despite the rank field being documented as affecting ships and outfits. 
* **Auto-aborted missions can omit their legal-status reward** even when configured to pay on auto-abort. 
* **“Attack Enemy Spobs/Stellars” AI does not work correctly**; later testing reproduced the problem, although the poster still noted some uncertainty about AI setup.  
* **Windows startup music filename is hard-coded** to `Nova Music.mp3` rather than respecting STR# 130 like the Mac version. 
* **Volume labels cannot be replaced through STR# 136**, despite older versions supporting it. 
* **Pure-white pixels in `cicn`s become transparent** regardless of the mask. 
* **Negative `DatePostInc` does not work.** 
* **Beam collision detection has holes** — beams can visibly pass through substantial portions of a ship. 
* **Finite auxiliary mission ships disappear after save/quit/reload** before the mission is completed. 
* **RLE colour runs ignore colouring, transparency and murk.** 
* **The starfield ignores murk.** 
* **Carried fighters ignore configured exit points** and always launch from the carrier's center. 
* **Player turn rates are quantized to multiples of 10** despite finer values being displayed; AI ships are not affected. 
* **IFF jamming creates inconsistent spob interaction** — landing can remain forbidden even though hailing treats the spob as friendly, making normal bribery unavailable. 
* **Multi-ship cargo-retrieval missions mishandle cargo** — the first target gives the full cargo amount, later targets require the same free space but yield 0 tons. 
* **Nebula image selection chooses a too-small image and scales it up** rather than scaling a slightly oversized image down. 
* **Max-guns/max-turrets outfit bonuses do not stack.** 
* **`Hxxx`/probably `Exxx` ship changes omit carried fighters**, leaving the new ship's bays empty; the initial player ship has the same problem. 
* **Visbit-swapped systems can retain the previous system's map colour and message-buoy string.** 
* **A brief tractor-beam hit can permanently paralyse an AI ship** until it is hit by the tractor again. 
* **Map outfits can fail to reveal systems that are actually within range** because Nova does not always calculate the shortest hyperlink path. 
* **Travel-stellar objectives can complete before required special-ship cargo is collected.** 
* **Ship trade-in value can disagree between the Extras pane and shipyard.** 
* **Point-defense beams can reacquire targets outside their normal range.** 
* **`Gxxx` in an outfit's `OnBuy` prevents the purchased outfit itself from being granted**; `Dxxx` in `OnSell` has the symmetric problem. 
* **Cröns contain multiple broken behaviours.** The thread treats this as a family of engine bugs rather than one isolated defect. 
* **Random mission travel/return stellars can fail when the originating system will change the following day.** 
* **Dude-specific advice STR# resources sometimes fail to load all entries.** 
* **Always-dominated spobs show a disabled-looking “Leave” button that remains clickable.** 
* **Beam decay can stack successive beam instances into an “auto-machine-gun” effect**, greatly multiplying damage. 
* **Weapon timing mixes frame-based and fixed 1/30-second timing**, making beam continuity and damage-per-second depend on machine framerate. 
* **AI cloaking has several broken transitions** — e.g. cloaking while hyperspacing but not necessarily entering cloaked, and inconsistent cloak/uncloak behaviour around system departure and docking. 
* **Pirate AI can make very-low-armour ships effectively invulnerable** because it refuses to destroy targets but cannot disable ships with ≤3 armour. 
* **Windows hyperspace flash does not build up like the Mac implementation.** 
* **`DispWeight` does not control mission ordering**; Mac 1.1 presents missions by increasing resource ID instead. 
* **Economy-at-Work ships can become hostile when the player requests assistance.** 
* **Crön date ranges split day/month and year ranges incorrectly**, so a range such as 1/1/1178–1/1/1179 only fires on two calendar dates rather than throughout the interval. 
* **Bay-launched fighters receive only 75% of normal damage** while regenerating at the normal rate. 
* **A shield-breaking hit also applies the weapon's full armour damage**, rather than only the unused portion of the hit. 
* **AI-fired weapons and their submunitions cannot damage the ship that fired them.** 
* **Negative recoil does not work for the player.** 
* **`chär` Initial Record values can fail to set the player's initial legal status correctly.** 
* **`përs` ships will not offer missions whose destination or return stellar is in the current system.** 
* **A mission that aborts itself in `OnAccept` and starts another mission displays the second briefing twice.** 
* **AI ships can launch fighters while cloaked even when the fighter bay is not flagged to fire while cloaked.** 
* **`TravelStel 30000–30255` does not work** for selecting a random stellar belonging to a particular government/class. 
* **The fleet “random cargo” flag does nothing**; fleet-created ships have no cargo when boarded. 
* **Player escorts belonging to governments marked “don't attack player” and/or immune to player weapons can become unable to hit anything at all.** 

### Engine quirks

* (identified, reworked) Nova uses the 'weapon 1' range to decide if carrier escorts attack or defend. Seems like an unintended quirk.
* **Non-strict play gives the player +50% top speed**, with no independent switch for the bonus. 
* **Escape-pod destination/location is undocumented**, so plug-in authors cannot reliably infer where the player will reappear. 
* **Non-simultaneous weapons cannot fire more than once per rendered frame.** 
* **A `përs` absent when a pilot is created is treated as permanently dead.** < I _think_ this means if you add a plugin during a run pers don't show up, which isn't per se a bug IMO, but because the game latches to 'alive' instead of 'killed'. Could be tweaked.
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
* **Aborting a mission in `OnShipDone` prevents an `OnShipDone` description from appearing.** 
* **A never-collected cargo requirement can be abused as a free-cargo-space test** for later missions. 
* **Fleet-created ships ignore generic government hail quotes** and fall back to “Greetings.” 
* **`përs` ships likewise ignore generic government hails** unless given a specific quote/disaster-info setup. 
* **System-summoned persons are already present when the player enters**, and are not spawned again until the system is re-entered. 
* **Two `përs` with exactly the same name count as one person, but adding a trailing space makes them distinct.** 
* **System-summoned fleets always arrive after the player**, via hyperspace/hypergate, rather than being present beforehand. 
* **Scan-triggered mission failure requires the scanning government to have a smuggling penalty ≥1 as well as a matching scan mask.** 

## Datafile bugs

* **STR# 4001 and 4002 are missing entry 80, `*Samantha`.** 
* **Ancient Vell-os Sculpture has a typo on Windows**: `"ancient Vell-s sculpture"`. 
* **Kymonth Station description uses “bought” instead of “brought.”** 
* **DESC 4135 contains malformed wording**: `"Upon seeing you a there is a sudden hush"`. 
* **DESC 4544 has incorrectly escaped/mismatched quotation markup.** 
* **DESC 9307 has missing quotation marks.** 
* **DESC 9368 says “as you make her way over.”** 
* **Stock asteroid sprites are too small to survive the high-resolution asteroid engine bug** and need padding. 
* **The Vell-os area-map data is configured so it only works every three days**; changing the cron setup makes it work daily. 
* **Flower of Spring, Summer Bloom and Winter Tempest retain beam decay values that trigger the engine's broken decay behaviour.** 
* **Auroran Cruiser and Thunderforge have garbage `moviefile` fields**, which can crash WinNova when they are selected in the shipyard. 
* **Stock multiple Ion Cannons/BRLs/TBRLs don't stack damage correctly** because their data runs into the once-per-frame weapon limitation. 
* **Vell-os abilities are lost after ejecting**, something the stock scenario could work around in its data. 
* **The stock scenario permits multiple story strings on the same pilot**, described as an exploitable scenario-data problem. 

## Datafile quirks

* **The 200mm railgun is arguably worse overall than the 150mm** because its increased mass damage is outweighed by reload, decay, weight and cost; this is a balance/data choice rather than an engine failure. 
* **The Fission Reactor is less efficient than Solar Panels** in both mass/cost terms with no compensating benefit; another stock balance oddity. 
* **The “Set and launch trap” mission is tuned extremely aggressively**, with escorts often destroying the target almost immediately. 
* **United Shipping uses an intermediary government resource to confine rank benefits to United Shipping instead of all allied governments** — a deliberate stock-data workaround for how rank flags propagate. 
* **`Nova Ships 8` is described as redundant; race movies carry unnecessary resource forks; music files have incorrect type/creator codes; and `Nova-DF.rsrc` was suggested for checksumming.** These are packaging/data hygiene oddities rather than runtime-engine bugs. 
