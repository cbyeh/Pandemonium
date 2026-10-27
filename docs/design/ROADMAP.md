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

| Fight | Health | Damage a minute | Avg opening | Cost |
|---|---|---|---|---|
| Guardian | 100 | 438 | 1.38 s | 27% |
| Unbound | 100 | 583 | 1.97 s | 32% |
| Story (both) | 200 | | | 58% |
| Fallen Warrior | 180, blows x1.125 | 400 | 1.83 s | ~53% |
| Shackled Sun | 240, blows x1.55 | 880 | 2.49 s | ~52% |
| Shadowborne | 275, blows x1.75 | 984 | 2.84 s | ~60% |

The Bonus bosses climb past the story (`boss_target_max_health`, `boss_damage_scale`). It ignores how readable attacks are: a random-roll test showed the Bonus bosses' attacks are much easier to avoid than the Guardian's. `HLE_DUMMY_REAL` is a bot that fights back (strikes in openings, rolls out of attacks); it is too weak to win, so it only compares survival.

## The Tower (shipped)
A run climbs floors, one fight each:
- **A circle is five floors:** the Guardian first, the three Bonus bosses in a random order, Guardian Unbound last (a floor of its own).
- **Scaling:** circle 1 bosses have 70% of their health and deal 60% of their damage; each circle adds 30% health (40% from circle 6, so even a full set of boons eventually stalls) and 20% damage. A Bonus boss has 55% of its story health, so every floor costs about what a Guardian phase does.
- **Skills:** learned from Learn cards in the draft. Boss health rises 8% for each skill offered (taken or passed over), so skipping skills never keeps bosses weak.
- **The baseline:** each circle cleared gives the player +10% damage and 6% less damage taken, through circle 5 (at most +40% and x0.78).
- **Nights** (from circle 2, one at random a circle): Blood Moon (bosses hit 20% harder, x1.5 Embers), Iron Night (bosses +25% health, x1.3), Starfall (skills recharge 30% faster, bosses hit 10% harder, x1.2).
- **After each boss:** one potion (no health), then the draft: three cards and a Skip button (+5 Embers; Down to it, Up back).
- **Embers** per floor: (8 x circle + 3 x the floor's place in its circle) x the night's bonus; +25 for a cleared circle. Kept when the run ends.
- The pause menu's **Run** page lists the run: relic, damage and damage taken, health and potions, boons, blessings and curses.

Cost per floor with the baseline, before boons (a potion returns about 19% of the resources): 13%, 21%, 29%, 37%, 45% in circles 1-5, so runs end around circles 4-6 unless boons and pacts carry them. A 45-floor scripted climb that took every boon and a pact on each floor after (`HLE_RUN_CLIMB`), costed with the logged numbers as a share of max health (a potion returns 45%): about 33% a floor in circle 1, 72-84% in circles 2-5, and 79-90% from circle 7, so even a full set runs out of floors (before the late scaling and the baseline's cap, circles 7-9 fell to 55-61%).

### Draft
The first card is a skill card (learn a skill or upgrade one) while any are left. The other two roll a rarity (legendary 6% in circle 1, +4% a circle up to 25%; rare 30%; else common). Each card is a Pact 4% of the time, and always once the boons run out, so a draft is never empty.

| Boon | Tier | Max | Effect |
|---|---|---|---|
| Ember Edge | Common | 3 | +15% damage |
| Iron Skin | Common | 2 | -15% damage taken |
| Deep Flask | Common | 2 | +3 potions |
| Kindled Flask | Common | 2 | potions heal 25% more |
| Second Wind | Common | 2 | +40% stamina regen; the 2.5 s wait after the bar empties is cut the same way (1.8 s, then 1.4 s) |
| Vigor | Common | 3 | +25 max health |
| Fleetfoot | Rare | 1 | rolls cost 40% less stamina |
| Bloodthirst | Rare | 2 | heal 6% of damage dealt |
| Last Stand | Rare | 1 | +40% damage below 30% health |
| Phantom Step | Rare | 1 | easier perfect dodges, each heals 8% |
| Riposte | Rare | 1 | +50% damage for 2.5 s after a perfect dodge |
| Crushing Blows | Rare | 1 | the stagger bar fills 40% faster |
| Rally | Rare | 1 | hitting back wins back the health just lost |
| Unbroken | Legendary | 1 | survive one killing blow a floor |
| Executioner | Legendary | 1 | staggered bosses take +60% |
| Storm Blade | Legendary | 1 | every 4th hit calls lightning (+12) |
| Blood Pact | Legendary | 1 | +35% damage, potions -25% |
| Learn Moon Blade Dance / Halfmoon Slash / Crescent Cleave | Skill | 1 | the skill, on its button in the layout |
| Moon Blade Dance+ / Halfmoon Slash+ / Crescent Cleave+ | Skill (rare) | 1 | 2 more waves / +50% / +50% |
| Moon Mastery | Skill (rare) | 2 | skill cooldowns -20% |

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
The meter fills from the blow, not its damage (`boss_apply_hit`, `POISE_DAMAGE`), as stance does in Elden Ring: a sword hit is 1/11 of the Guardian's and Unbound's bar, 1/10 of the Fallen Warrior's and 1/14 of the Sun's and Shadowborne's (set so a player who strikes only between attacks breaks each about once or twice a fight), a combo's third hit 2.5 of those, Halfmoon Slash 30%, the Cleave its damage's worth. There is no half stagger. Damage boons don't add stagger; Crushing Blows does. It keeps 5 s untouched (a dodge or two), then drains 5% a second. A bot that swings without dodging breaks a stance every 35-40 s. A broken boss stays down 3.5 s (`BOSS_STAGGER_S`, every boss) and takes no extra damage: the opening is the reward; Executioner (+60%) is the only bonus.

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
