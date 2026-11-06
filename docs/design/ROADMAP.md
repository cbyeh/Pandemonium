# Pandemonium: design notes and ideas for what's next

What shipped in 1.0, the numbers it is tuned to, and the ideas still open. Sizes: **S** is under a day, **M** a few days, **L** a week or more.

## Constraints to design around
- **RAM: 4 MB, no Expansion Pak.** No new big models. New visuals should be procedural, like the skins and the Shackled Sun (`src/fx/proc_mesh.c`), or reuse existing assets. Keep 400-500 KB free at the lowest point.
- **Save space: EEPROM 4K, 3 slots.** Each `SaveData` slot has spare bytes in `_pad`; 24-27 are still free (see `save_controller.c` for the map). Old saves read new bytes as zero.
- **One arena.** Every fight takes place in the Guardian hall (`src/scenes/guardian/guardian_scene.c`). Bonus fights swap the boss in place through `BonusDef` begin/skip functions.
- **The Guardian and Guardian Unbound stay as the jam game made them**: their patterns, health and damage are not tuned. Balance work goes into the Bonus bosses and the Tower.

## Balance: how difficulty is measured
`HLE_TEST_DUMMY` (with `HLE_DUMMY_BOSS` 1-5) logs, every 30 s, the damage a boss lands on a passive, invincible player and its openings (time between attacks, and how many). The cost of a fight is:
- damage per minute x 30% (the player avoids 70% of attacks) x time to kill,
- the time to kill coming from the openings: a sword hit is 4 per 0.65 s, after a 0.4 s reaction,
- as a share of the player's resources (150 health + 3 potions at 45%, about 352).

Each half measured on its own (`HLE_DUMMY_HALF` for the second, 45% of the health; the Sun's halves weigh 50/50, as it unshackles at half health), with `HLE_LOG_MOVES` (+ `HLE_LOG_HURTBOX` for Shadowborne) tagging each hit's source. A lingering hazard (the Sun's Spiral embers, Shadowborne's flames, the Fallen Warrior's Cleave blades and fire trail) counts once a cast: the passive dummy stands in it and takes every tick, a player steps out. Openings: a gap's sword damage is (gap - 0.4 s) / 0.65 s x 4.

| Fight | Health | Damage per 120 s (1st / 2nd half) | Avg opening | Cost |
|---|---|---|---|---|
| Guardian | 100 | 890 / 840 | 1.42 / 1.43 s | 26% |
| Unbound | 100 | 1095 / 1090 | 2.01 / 2.05 s | 27% |
| Fallen Warrior | 180, blows x1.125 | 880 / 915 | 2.45 / 2.60 s | 47% |
| Shackled Sun | 240, blows x1.55, health lost x0.78 | 612 / 1019 (785 / 1307 as blows) | 2.39 / 1.96 s | 62% (was 78%) |
| Shadowborne | 275, blows x1.5 | 790 / 891 | 2.54 / 1.97 s | 68% |

Measured 2026-11-01. The older estimate (Fallen Warrior ~48%, Sun ~52%, Shadowborne ~60%) counted every hazard tick and missed the Sun's second half; 2026-11-02: the Sun cost more than Shadowborne, so its blows now take 78% of their damage off the player's health (`boss_hp_loss_scale`) while knockdowns, shoves, flinches and perfect dodges still go by the full blow: the fight plays the same, its moves, timing and unshackled +20% untouched (also in the Tower). Measured over 12 minutes a half (two second-half runs, 1266 and 1348 as blows: 61-64%).

The Bonus bosses climb past the story (`boss_target_max_health`, `boss_damage_scale`). It ignores how readable attacks are: a random-roll test showed the Bonus bosses' attacks are much easier to avoid than the Guardian's. `HLE_DUMMY_REAL` is a bot that fights back (strikes in openings, rolls out of attacks); it is too weak to win, so it only compares survival.

## The Tower (shipped)
A run climbs floors, one fight each:
- **A circle is a map** (branch tower-rogue, stage 3; [TOWER_ROGUELIKE.md](TOWER_ROGUELIKE.md) section 2): 7 rows of 3 lanes, the Guardian in row 1, Guardian Unbound in row 7 (a floor of its own), a Rest on every lane of row 6, rows 2-5 rolled (Fight 45, Elite 15, Event 20, Shop 12, Rest 8), rerolled until every lane has 2+ Fights, every path 2-4 fights in those rows, a Shop sits in row 3 or 4 on some lane, no Elite in row 2 and no two Shops or Rests in a row on any path (the rules leave a map about 65% Fights, 10% Elites, 10% Events, 12% Shops, 3% Rests in rows 2-5). A path has 4-6 fights (23% / 51% / 26% of paths); each Fight or Elite is a Bonus boss not met this circle while any are left (set when its node becomes reachable).
- **Elites:** the Bonus boss at x1.25 health, a Rare-or-better draft, x1.5 gold, x2 Embers (phase 2), a second potion and a chest (a slotted boon's next level or its "+"), and its affixes (stage 6; TOWER_ROGUELIKE sections 11 and 14d): one in circles 1-3, two from circle 4 (then its last card is a duo when one is ready), Swift, Ironclad and Enraged from the start, Relentless, Vampiric and Smouldering at best floor 15, 20, 25, never Smouldering on Shadowborne. Swift: footwork, breathers and cooldowns x1.2, its sounds 12% higher. Ironclad: +30% health (x1.625 in all), stagger bar +30%. Enraged: below half blows +25%, breathers -15%. Relentless: stagger drain x2, down 2.5 s, breathers -15%. Vampiric: heals 25% of the health its blows take. Smouldering: its slams leave Shadowborne's floor fire (5 flames 3 s where a blade bites, a ring of 8 for 2 s round the Sun's landing; 6 a second). Each costs 1.5-1.7x a plain fight on the agreed metric.
- **Gold** (stage 4; this run only, lost when it ends): 25 + 5 x circle a fight (any boss; x1.5 an Elite), +15 for a Skip, (a complete build's drafts hold a minor blessing card instead, phase 2); Prospector (Hearth pack) +20% a level on every gain. About 166 a circle from fights in circles 1-2.
- **Shops:** three new-boon cards rolled as a draft (Common 60, Rare 110, Legendary 180; buy one), a level on a slotted boon 70, a maxed boon's "+" 150, the free pact card, a potion 45 (up to the run's max), Mend (heal 35%) 50, Release 20; each once a visit (potions until the max). Left/Right along the shelf, Up/Down by rows, A buys, B or Leave leaves.
- **Events** (one screen, 2-3 choices, the last Leave; no repeat in a run until all unlocked are met): the Bleeding Altar, a Knight's Grave, Mirror of Shadows, Merchant of Ash from the start; Moonlit Pool, the Chained Prisoner, Gambler's Bones and the Ember Brazier at best floor 10, 15, 20, 25.
- **Rests:** heal 40% of max health, Temper (+1 level on a slotted boon, a maxed boon's "+", or a known skill below Lv 4), or +2 potions (up to the run's max); then Release one slotted boon free (it, its levels and its "+" gone; a duo with its two; a rival it locked stays locked).
- **Scaling:** circle 1 bosses have 70% of their health and deal 60% of their damage; each circle adds 30% health (40% from circle 6, so even a full set of boons eventually stalls) and 20% damage. A Bonus boss has 55% of its story health, so every floor costs about what a Guardian phase does.
- **Skills:** learned from Learn cards in the draft, then levelled by the same card (Lv 2-4), then one of two Lv 5 forms (stage 5, below). Boss health rises 8% for each skill offered (taken or passed over), so skipping skills never keeps bosses weak.
- **The baseline:** each circle cleared gives the player +10% damage and 6% less damage taken, through circle 5 (at most +40% and x0.78).
- **Nights** (from circle 2, one at random a circle): Blood Moon (bosses hit 12% harder, x1.3 Embers), Iron Night (bosses +25% health, x1.3), Starfall (skills recharge 30% faster, bosses hit 15% harder, x1.2).
- **After each boss:** one potion (no health; at most 15 held), then the draft: three cards and a Skip button (+5 Embers; Down to it, Up back), then the map (Left/Right on the D-pad or stick, A or Up enters; it also shows before each circle's Guardian).
- **Embers** per floor: (4 + 2 x circle + the fight's place in its circle, 0-5) x the night's bonus (x2 an Elite) x the Heat dial (+5% a step); +10 for a cleared circle (phase 2 pacing: was 8 x circle + 3 x place, +25). Kept when the run ends.
- The pause menu's **Stats** pages: Stats (relic, damage and damage taken, health and potions, blessings and curses) and Build (the 3 skills, then the 5 boon slots with their levels).

Cost per floor with the baseline, before boons (a potion returns about 19% of the resources): 13%, 21%, 29%, 37%, 45% in circles 1-5, so runs end around circles 4-6 unless boons and pacts carry them. A 45-floor scripted climb that took every boon and a pact on each floor after (`HLE_RUN_CLIMB`), costed with the logged numbers as a share of max health (a potion returns 45%): about 33% a floor in circle 1, 72-84% in circles 2-5, and 79-90% from circle 7, so even a full set runs out of floors (before the late scaling and the baseline's cap, circles 7-9 fell to 55-61%).

### Draft
Until a skill is known the first card is a Learn card; after that a third of the drafts hold one skill card (a skill at Lv 2+ twice as likely as each other one), and the draft after a skill reaches Lv 4 leads with its forms. The other cards roll a rarity (legendary 6% in circle 1, +4% a circle up to 25%; rare 30%; else common). Each card is a Pact 4% of the time, and always once the boons run out, so a draft is never empty.

**Slots and families** (tower-rogue stage 1, docs/design/TOWER_ROGUELIKE.md): a run holds **5 boon slots**. A new boon takes the next free slot at Lv 1; taking it again levels it in its slot (each level adds the old per-stack value unless the table says otherwise). With the bar full, drafts offer only levels of the slotted boons, skill cards and pacts. Skill cards take no slot. Every boon has a family (the card's tag over its name); a family with 2+ boons slotted comes up twice as often (`HLE_TEST_TOWER` measures 1.93x, one held 1.08x). The new boons (**pack**) join the pool once their family's pack is bought at the Shrine (Boon Packs: the nth pack bought costs 120, 160, 200, 240, 280; 9 save bits in the slot's `_pad[24..25]`, bits 6-8 kept for the skills' second forms).

| Boon | Family | Tier | Max Lv | Effect |
|---|---|---|---|---|
| Bloodthirst | Blood | Rare | 3 | sword hits (not skills or lightning) heal 12% a level of their base damage (before damage boons); Lv 3 +8% (32%, the old Bloodthirst+) |
| Rally | Blood | Rare | 3 | hitting back wins back the health just lost, each hit an eighth of its base a level (a sword hit 0.5; Lv 2 a quarter, held 1.5 s; Lv 3 three eighths, the old Rally+) |
| Blood Pact | Blood | Legendary | 1 | +35% damage, potions -25% |
| Last Stand | Blood | Rare | 2 | +40% damage a level below 30% health |
| Hemorrhage (pack) | Blood | Common | 3 | the combo's 3rd hit bleeds 4 a level over 3 s (ticks every 0.5 s, a quarter of a hit's flash, no flinch, no healing) |
| Sanguine Flask (pack) | Blood | Rare | 1 | a potion: +25% damage for 6 s |
| Red Thirst (pack) | Blood | Common | 3 | +10% damage a level per full 25% of health missing |
| Berserker (pack, stage 6) | Blood | Legendary | 1 | below half health: a blow that doesn't knock you down doesn't flinch you or break a wind-up, each sword hit in a row +8% to +40% (a blow or 2 s ends it), no potions; Iron Skin's rival |
| Storm Blade | Storm | Legendary | 1 | every 4th hit calls lightning (+6; was +12, best in every build) |
| Static (pack) | Storm | Common | 5 | Storm Blade's bolt +1.5 a level; without it, every 8th hit a bolt of 6 (+1.5 a level above 1) (stage 6: was +3; the Storm build was +21% over the builds' mean, now +4%) |
| Thunderclap (pack) | Storm | Rare | 2 | the combo's 3rd hit calls lightning (+6; stage 6: was +8); Lv 2 +50% (9, the old Thunderclap+) |
| Conductor (pack) | Storm | Rare | 1 | lightning fills the stagger meter like a sword hit (offered once something calls lightning) |
| Overcharge (pack) | Storm | Legendary | 1 | a stagger: the next 5 hits each call Storm Blade's bolt (6, +3 a Static level) |
| Moon Mastery | Moon | Rare | 4 | skill cooldowns -20% a level to Lv 3 (x0.8 each); Lv 4 x0.42 in all (the old Moon Mastery+) |
| Lunar Tide (pack) | Moon | Common | 3 | sword hits cut skill cooldowns 0.3 s a level |
| Eclipse (pack) | Moon | Rare | 1 | a skill's hit on a staggered boss +50% |
| Waxing Moon (pack) | Moon | Rare | 1 | a skill cast: sword hits +15% for 4 s |
| Iron Skin | Iron | Common | 3 | -15% damage taken a level (Lv 3: -10%) |
| Vigor | Iron | Common | 5 | +25 max health |
| Unbroken | Iron | Legendary | 1 | survive one killing blow a floor |
| Crushing Blows | Iron | Rare | 1 | a staggered boss stays down 50% longer |
| Executioner | Iron | Legendary | 2 | staggered bosses take +60% (Lv 2 +90%, the old Executioner+) |
| Heavy Hand (pack) | Iron | Common | 3 | the combo's 3rd hit fills the stagger meter 20% more a level |
| Stalwart (pack) | Iron | Rare | 1 | blows over 20% of max health (after damage taken) deal 25% less |
| Phantom Step | Shadow | Rare | 2 | easier perfect dodges (0.28 s window; Lv 2 0.31 s, the old Phantom Step+), each heals 2% |
| Riposte | Shadow | Rare | 2 | +50% damage for 2.5 s after a perfect dodge (Lv 2 4 s, the old Riposte+) |
| Fleetfoot | Shadow | Rare | 1 | rolls cost 40% less stamina |
| Second Wind | Shadow | Common | 3 | +40% stamina regen a level; the 2.5 s wait after the bar empties is cut the same way |
| Shadow Edge (pack) | Shadow | Common | 4 | +8% damage a level for 2 s after any roll |
| Evasion (pack) | Shadow | Rare | 1 | a perfect dodge also gives back the roll's stamina |
| Vanish (pack) | Shadow | Legendary | 1 | 3 perfect dodges in a floor: the next blow that lands deals nothing (once a floor) |
| Ember Edge | Hearth | Common | 5 | +15% damage a level (Lv 4-5: +10%) |
| Deep Flask | Hearth | Common | 2 | +3 potions |
| Kindled Flask | Hearth | Common | 2 | potions heal 25% more |
| Learn Moon Blade Dance / Halfmoon Slash / Crescent Cleave | Skill | Common, then Rare | 4 | the skill on its button (no slot); the card again is its next level (below) |
| The six Lv 5 forms | Skill | Legendary | 1 | at Lv 4; one locks the other out (below); the B forms bought at the Shrine |

**Skill levels** (stages 5-6, TOWER_ROGUELIKE sections 6 and 14d; damage and stagger a cast measured with `HLE_TEST_SKILL_DMG` + `HLE_SDMG_TOWER`, the Guardian 30 away, circle 1; the meter's peak in sword hits of the Guardian's 11). Lv 1 is main's, in the Tower too (stage 6):

| Lv | Moon Blade Dance (15 s) | Halfmoon Slash (45 s) | Crescent Cleave (18 s) |
|---|---|---|---|
| 1 | 5 crosses of 0.8: 4, stagger 1 hit | 28, charge 2 s, stagger 3.6 hits | 5 blades, 4 hits each 0.3 s apart, 7 round: 12.5-13.8, 3.1-3.4 hits |
| 2 | crosses 1.5: 7.5 (the first five keep Lv 1's stagger: 1 hit) | charge 1.6 s: 28 | +20%: 15-16.5 |
| 3 | 7 crosses: 10.5 (1 hit) | +20%: 33.6 | 5 hits 0.24 s apart: 18-19.5 |
| 4 | cooldown 12 s | reach 85 to 106 | wind-up 0.6 s (was 0.85): 19.5 |
| 5 A | Full Moon Dance: 9 crosses of 1.75: 15.75 (1 hit; phase 2, was 10 of 2) | Twin Moon: the slash again 0.35 s later at 45% (phase 2, was 40%): 48.7, 5 hits | Crescent Storm: 3 more blades 0.3 s behind the middle three at 70%: 27.5-29 (stage 6: at full damage 33, +12% over the builds' mean) |
| 5 B | Shattering Dance: 7 crosses of 1.75 filling two thirds of a sword hit each: 12.2, 4.7 hits (stage 5: 1.5, a fifth: 1.4 hits; the build -22%) | Full Moon: all round, charge and slash unbroken, stagger 60%, x1.0 (phase 2, was x1.15: 38.6): 33.6 | Long Cleave: blades 1.6x as far, 6 hits, fan 60% as wide: 21 |

Stage 5 had cut the player's Cleave blades' reach to 4 (10 a cast at Lv 1) and Moon Blade Dance's Lv 2-4 to no stagger; stage 6 put both back as on main (the user's call) and rebalanced the levels and forms from that Lv 1 (TOWER_ROGUELIKE 14d).

The B forms cost 600 Embers each (phase 2: every Shrine price x3) at the Shrine (Unlocks, after the packs; bits 6-8 of the unlock bits).

Not built yet: Hearth's pack (Quick Draught needs a drinking time, which potions don't have; Prospector needs gold, stage 4), so the Shrine sells five packs.

**Rivals, "+" and duos** (tower-rogue stage 2):
- **Rivals:** taking one locks the other out for the run (never offered after, never on one draft together): Riposte / Iron Skin, Blood Pact / Kindled Flask, Storm Blade / Executioner, Last Stand / Vigor, Bloodthirst / Moon Mastery, Vanish / Unbroken, Berserker / Iron Skin (stage 6: Iron Skin locks out both). The card names the rival ("Locks out"); Build lists the locked ones, greyed.
- **"+"** (Rare, no slot, offered once its boon is at its max; Build shows "Lv 5/5+"): only the five that add an effect: Ember Edge+ finishers +25%; Iron Skin+ the first blow a floor deals half; Storm Blade+ every 3rd hit; Crushing Blows+ no stagger drain for 3 s after the boss rises; Unbroken+ heals 20% when it saves you. Second Wind+ is held back (a potion is drunk at once). 2026-11-05 (the user's call): the eight that only raised numbers are their boon's last level now, worth the same (Executioner Lv 2 +90%, Riposte Lv 2 4 s, Bloodthirst Lv 3 32%, Rally Lv 3 three eighths, Phantom Step Lv 2 0.31 s, Static Lv 5 bolts +50%, Thunderclap Lv 2 9, Moon Mastery Lv 4 x0.42); a duo needs that level.
- **Duos** (Legendary): offered once a maxed boon of each of its two families is slotted and both families' packs are bought (Hearth has none yet: open). Taking one merges those two (the first maxed of each family) into one slot, both still at their max, and opens the other slot; Build shows the duo and its two boons.

| Duo | Families | Effect |
|---|---|---|
| Crimson Storm | Blood + Storm | lightning heals half a sword hit's Bloodthirst rate of its damage (0.96 a bolt of 6 at Lv 3); no Bloodthirst: lightning +25% |
| Thunderstep | Storm + Shadow | a perfect dodge calls two of Storm Blade's bolts (6, +3 a Static level), 0.3 s apart |
| Blood Moon Rite | Blood + Moon | a skill cast costs 3% of max health (never the last 1), skill hits +40% |
| Iron Tide | Iron + Moon | a stagger resets every skill cooldown |
| Shade's Mercy | Shadow + Blood | blows fill a Rally pool (even without Rally); in Riposte's window after a perfect dodge (2.5 s, even without Riposte) hits win back 60% of their base |
| Anvil of Storms | Iron + Storm | lightning on a staggered boss x2 |
| Waning Light | Moon + Shadow | a perfect dodge cuts skill cooldowns 2 s |
| Steel Hearth | Hearth + Iron | a potion: +10 max health for the floor (to +30), x0.6 damage taken for 4 s |
| Skyfall | Storm + Moon | a skill's first hit a cast calls Storm Blade's pillar (6) |

Vigor and Vitality now raise max health when taken mid-run (before, only the health; max health was set at the run's start).

**Pacts** (a blessing and a curse, rolled for the card, stacking without end; a curse never undoes its blessing):

| Blessing | Per stack | | Curse | Per stack |
|---|---|---|---|---|
| Might | +8% damage | | Thirst | -1 potion (or the next boss's) |
| Bulwark | -6% damage taken | | Frailty | +5% damage taken (never with Bulwark) |
| Vitality | +12 max health | | Weakness | -5% damage dealt (never with Might) |
| Wounded Foe | +12% damage, boss below 50% | | Exposed | +15% damage taken 2 s after a roll |
| Breaker | +15% damage on staggers | | Impaired | 3% of hits miss |
| Last Breath | +15% damage below 30% health | | | |

A best blessing (Might, Bulwark) with a curse comes out a little ahead; the others about even, so Skip is a real choice.

### Stagger
The meter fills from the blow, not its damage (`boss_apply_hit`, `POISE_DAMAGE`), as stance does in Elden Ring: a sword hit is 1/11 of the Guardian's and Unbound's bar, 1/10 of the Fallen Warrior's and 1/14 of the Sun's and Shadowborne's (set so a player who strikes only between attacks breaks each about once or twice a fight), a combo's third hit 2.5 of those, Halfmoon Slash 30%, the Cleave its damage's worth. There is no half stagger. No boon adds stagger. It keeps 5 s untouched (a dodge or two), then drains 5% a second. A bot that swings without dodging breaks a stance every 35-40 s. A broken boss stays down 3.5 s (`BOSS_STAGGER_S`, every boss; 5.25 s with Crushing Blows) and takes no extra damage: the opening is the reward; Executioner (+60%) is the only bonus; in the Tower the stagger bonuses (Executioner +60/+90%, Breaker +15% a stack, Eclipse +50% on skills) add, up to +100%.

## The Shrine (shipped)
Embers are spent on the title screen; everything applies only in the Tower. Everything costs 2400 Embers, about 6.5 hours of climbing (7.5 with the story and Bonus bosses).

| Upgrade | Per level (max 3) | Cost |
|---|---|---|
| Flask Capacity | +1 potion | 30 / 60 / 100 |
| Flask Potency | potions heal 8% more | 30 / 60 / 100 |
| Honed Blade | +8% damage | 30 / 60 / 100 |
| Vigor | +10 max health | 30 / 60 / 100 |
| Skill Mastery | skill cooldowns -8% | 30 / 60 / 100 |
| Nightfarer | a skin | 450 |

**Relics**, 200 Embers each, one equipped for a climb (or None): Knight's Oath (+6% damage, +15 max health), Saint's Chalice (-6% damage taken, +2 potions), Moonlit Tome (start knowing a skill, +20% stamina regen), Ember Crown (+20% Embers, +6% damage), Brand of Wrath (+15% damage, -20 max health). Saves from before 1.0 kept up to three found relics: those that are one of these are owned, others refunded.

## Ideas still open
### Weapons (M)
Bought at the Shrine, equipped there, and shown in hand through the bone-attached accessory system the skins already use. The Dragonslayer slab exists already.
- **Greatsword:** +45% damage, swings 15% slower, more reach. Reuses the Black Swordsman's slab.
- **Moonlight Greatsword:** a strong attack also releases a crescent wave (the skill_fx wave), and the blade glows.
- **Twin Fangs (stretch):** faster, weaker swings that build skill cooldowns.

Needs a per-weapon stats table: damage, animation speed and the hit capsule length (`SWORD_LENGTH`). The strong (charged) attack state exists in the code but nothing triggers it yet.

### More roguelike depth (M)
- **Seeds:** a run seed shown on the results screen, so a run can be shared or retried.
- **Elite floors:** remixed bosses, such as a Shackled Sun that starts unshackled or a twin Fallen Knight.

### New bosses and modes (L)
- **The King** (from the ending): a throne-bound caster fight in the same hall, built procedurally.
- **Practice mode:** pick any boss and phase, with infinite potions and the hit timings shown.
- **Time attack:** a best time per boss, which `save_controller_record_boss_clear_time_ms` already records for the story fight.

### Sound left to improve (S)
- **The Fallen Warrior's footwork** (branch `fw-alive`): its footsteps play in its chase only; its strafing and backing off are silent until they're hooked up after main is merged in.
- **Timing:** `tools/sfx_timing.py` lines each sound up against the screen shakes in an `HLE_LOG_SFX` log. Keep every impact under ~60 ms; sounds baked into one mix (the Sun's break) don't show separately.
