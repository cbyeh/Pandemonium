# The Tower as a roguelike: design for approval

Scope approved by the user (2026-11-01): a branching map each circle (4-6 fights a path), Vampire Survivors-style slots (3 skill slots, 5 boon slots, boons levelling inside their slot), boon families with duos as evolutions, rival pairs and "+" upgrades, and elite affixes. Not in scope: rerolls, removing boons, relics found mid-run. Nothing here is built yet. Numbers are starting points for the balance pass (stage 5).

## 1. Why every deep run ends the same
- A run drafts once per floor. All 24 boons together have **34 stacks**. By floor 35 a player has had 34 drafts, so a deep run can own everything. The screenshot from 2026-11-01 showed exactly that.
- Early picks don't close any doors, so they don't shape the run.
- The Bonus fights in a circle always come in the same order; only the shuffle changes.

The fix has four parts:
- **Slots:** a build holds 3 skills and 5 boons, no more. Every run leaves most of the pool behind, so what you keep is the run's identity (section 6).
- **A pool too big to see:** 41 boons in six families, 5 skills, 8 duos and 13 "+" upgrades. A run slots 5 of the 41 boons.
- **Choices that close doors:** rival pairs, and a full slot bar.
- **A route you choose:** the map.

## 2. The map (one per circle)
A circle becomes a **7-row map** with 3 lanes, climbed bottom to top. Each node links to the node above it and to the diagonal neighbours, as in Slay the Spire. Rows 1 and 7 are a single node.

| Row | Nodes |
|---|---|
| 1 | **Guardian**, always (unchanged) |
| 2-5 | Rolled per lane: Fight 45%, Elite 15%, Event 20%, Shop 12%, Rest 8%. Rules: every lane has at least 2 Fights in rows 2-5; there's at least one Shop in rows 3-4 on some lane; no Elite in row 2; no two Shops or Rests in a row on one path |
| 6 | **Rest** on every lane: get ready for the circle's boss |
| 7 | **Guardian Unbound**, always (unchanged) |

- **Fights per circle:** 4-6, chosen by the path (Guardian, 2-4 middle fights, Unbound).
- **A Fight or Elite** is a Bonus boss. Each one is a boss not yet met this circle, while any are left. A 4-fight middle repeats one.
- **"Floor" counts fights,** as now. The HUD's Floor N, the best-floor record and Embers per floor keep their meaning. The circle counter (and its Ascension label) goes up per map.
- **Drafts:** after every Fight and Elite, as now. Events, Shops and Rests give no draft.
- **Fights a path:** 4-6 a circle, set by the route (the user's call): a greedy route earns more drafts and gold for more damage taken.
- **When the map shows:** after the draft, and before the circle's first fight. It's a full-screen menu drawn over the darkened hall, like the draft. It holds no 3D scene and loads nothing.

### Drawing it (320x240)
- **Nodes:** 7 rows, 26 px apart, bottom to top, in 3 lanes at x = 100 / 160 / 220. Each node is a 16x16 icon:
  - Fight is a sword, Elite is a horned helm, Event is a "?", Shop is a coin, Rest is a flame, and the Bosses are a crown.
  - Each icon is one I4 sprite, 128 B, about 1 KB for all of them.
- **Links:** 1 px lines (`rdpq_fill_rectangle` steps), the same way the menus already draw their frames.
- **Highlighting:** visited nodes are dimmed, reachable nodes are full colour, and the cursor node gets the menu's selection box.
- **The bottom line** names the node under the cursor:
  - an Elite shows its affix and reward, e.g. "Elite: Swift Shackled Sun · rare draft, +50% gold";
  - a Fight names its boss once you're beside it.
- **The top line** shows Floor, Ascension, Night and gold.

### Controls (the same in every layout)
- **Left/Right**, on the D-pad or the stick, moves between the reachable nodes in the next row.
- **A** enters the node.
- **Up** does the same as A, as Up/Down already do in the draft.
- **Start** opens the pause menu, whose Stats pages also show gold.
- **B** does nothing, so no one skips a node by accident.
- The C buttons and Z aren't used, so the Skills-on-C layout changes nothing.

## 3. Gold and the shop
Gold is for this run only and is lost when the run ends. Embers work as now.

| Source | Gold |
|---|---|
| Fight | 25 + 5 x circle |
| Elite | x1.5 |
| Guardian / Unbound | 25 + 5 x circle each |
| Skip in a draft | +15 (and +5 Embers, as now) |
| Events | see section 4 |

A circle earns about 150-220 gold, so one shop visit buys 1-2 things.

| Shop item | Price | Notes |
|---|---|---|
| A boon: choose 1 of 3 | Common 60, Rare 110, Legendary 180 | 3 cards rolled like a draft (family weighting included); you buy one |
| A "+" upgrade of a maxed boon you hold | 150 | only if one is unlocked |
| A level on a slotted boon | 70 | below its max |
| Release a slotted boon | 20 | the boon and its levels are gone, the slot opens |
| Potion | 45 | up to the run's max |
| Mend: heal 35% max health | 50 | |
| A pact card (blessing + curse) | 0 | one on the shelf; the curse is the price |

- Prices are the same every circle, because gold grows with the circle.
- One visit is one menu: Left/Right moves along the shelf, A buys, B leaves.

## 4. Events (rolled per circle, no repeat in a run)
Each event is one screen of text with 2-3 choices, the last always **Leave**.

| Event | Choices |
|---|---|
| **The Bleeding Altar** | Give 15% max health (for the run): a random Rare boon. / Leave. |
| **A Knight's Grave** | Take the 60 gold left on it. / Pray: heal 40%. |
| **Mirror of Shadows** | One random slotted boon gets +1 level (its "+" if maxed), and you take a random curse. / Leave. |
| **Merchant of Ash** | 1 potion for 70 gold. / 2 potions for a Rare boon. / Leave. |
| **Moonlit Pool** | A skill you don't hold, into a free skill slot or in place of one (its levels lost). / +1 level on a slotted skill. / Leave. |
| **The Chained Prisoner** | Free it: the next Fight becomes an Elite, and its draft is all Legendary. / Leave. |
| **Gambler's Bones** | Pay 50 gold: half the time 120 back, half the time nothing (run seed). / Leave. |
| **Ember Brazier** | Bank 30 x circle Embers now. / A blessing without a curse (half a pact's stack). |

## 5. Rest shrines
Choose one:
- heal 40% of max health;
- **Temper**: +1 level on a slotted boon or skill below its max, or its "+" if one is unlocked;
- +2 potions (up to the max).

Also, free with any choice: **Release** one slotted boon (it and its levels are gone; the slot opens for the next draft).

Row 6 is always a Rest, so going into Unbound is a real choice between healing and power.

## 6. Slots (Vampire Survivors style)
A build is **3 skill slots and 5 boon slots**. Pacts sit outside the slots.

### Skill slots (active: the D-pad)
- D-up, D-right and D-down each hold one skill (D-left stays the potion). The Tower offers 5 skills, so a run holds at most 3.
- A skill card with all 3 slots full names the slot it replaces ("Replaces: Halfmoon Slash", its levels lost). Skip keeps what you have.
- Skills level 1-3: Lv 1 learns it, Lv 2 +25% damage, Lv 3 is its "+" form (Moon Blade Dance+ etc. as now).
- The +8% boss health per skill offered stays (it counts skill cards offered, as now).

| Skill | Status | What it is | Cost |
|---|---|---|---|
| Moon Blade Dance | as now | jump, rain X-waves | none |
| Halfmoon Slash | as now | rooted charge, 180-degree slash | none |
| Crescent Cleave | as now | the cleave beam | none |
| **Storm Call** | New | after a 0.4 s raise of the sword, Storm Blade's pillar falls on the boss (its hit, sound and FX) | small: the Storm Blade hit + `skill_fx_spawn_pillar`, an existing cast pose |
| **Twin Crescent** | New | two Crescent Cleave beams in a narrow V, 60% damage each | small-medium: the cleave beam spawned twice at an angle; check two beams' draw cost in Ares |

### Boon slots (passive)
- 5 slots. A new boon takes a free slot at Lv 1. With 5 slots full, drafts offer only **levels of slotted boons**, their "+" (the top level), duos, skill cards and pacts.
- Levels are the old stacks, extended (the Lv column in section 7). Each level adds the old per-stack value unless the table says otherwise.
- **Swapping only at a Shop (20 gold) or a Rest (free):** release a slotted boon, its levels gone, the slot open for the next draft. Never in a draft, so filling the bar is a commitment.
- **Rivals** still lock out: a rival can't be offered while its pair is slotted, and stays locked for the run once its pair is taken.

### Duos (evolutions)
- Offered once **two maxed boons from different families** are slotted (like a Vampire Survivors weapon and its passive).
- Taking a duo **merges both into one slot**: both keep working at their max and the duo's effect is added; the other slot opens. One rule, one icon on the bar, and a duo is a reward (a free slot) rather than a cost.
- A merged duo is released only as a whole (all three gone).

### Pacts
Outside the slots, as now: they stack without end and fill drafts once a build is complete, so every late draft is still a choice.

### Why runs no longer converge
| | Picks |
|---|---|
| A complete build: 5 boons to their max (about 3.5 levels each), 2-3 "+", 1-2 duos, 3 skills to Lv 3 | about 30 |
| Drafts by floor 35 (7 circles, 4-6 fights each) | about 33 (plus Rests and Shops) |

- A deep run completes one build, but it holds 5 of 41 boons and 3 of 5 skills: two deep runs share a boon or two, not the whole list.
- Late drafts are levels, "+", duos and pacts; when a build is complete, pacts and a **+40 gold** card fill the rest.
- Slots fill by about floor 6-8, so what to slot is the first two circles' big decision.

## 7. Boon families
Every boon belongs to one of six families.
- **The card** shows the family as a coloured tag: the family name over the boon name, coloured like the pause menu's fonts (gold, red, and so on). No new art.
- **Weighting:** a family you hold 2+ boons from comes up twice as often, so runs lean into a style without being locked into it. This is Hades' god focus.

The tables below list existing boons first, then new ones (**New**). New effects only hook into events the game already has:
- a sword hit and the combo's 3rd hit;
- a roll and a perfect dodge;
- a stagger starting and ending;
- a skill cast;
- drinking a potion;
- taking a blow;
- a floor starting;
- health thresholds.

Any visual reuses existing effects: the Storm pillar, the skill waves, the red warning burst (only for player effects that land a hit), and hit flashes.

### Blood: healing and risk
| Boon | Tier | Lv | Effect |
|---|---|---|---|
| Bloodthirst | Rare | 2 | sword hits heal 12% a level of their base damage (not skills or lightning) |
| Rally | Rare | 2 | hitting back wins back the whole blow, each hit by its base damage (4 a sword hit); Lv 2 holds it 1.5 s (1 s) |
| Blood Pact | Legendary | 1 | +35% damage, potions -25% |
| Last Stand | Rare | 2 | +40% damage below 30% health |
| **Hemorrhage** (New) | Common | 3 | the combo's 3rd hit deals +4 bleed over 3 s (damage only, no heal) |
| **Sanguine Flask** (New) | Rare | 1 | drinking a potion gives +25% damage for 6 s |
| **Red Thirst** (New) | Common | 3 | +10% damage per 25% of health missing |

### Storm: hit-count procs
| Boon | Tier | Lv | Effect |
|---|---|---|---|
| Storm Blade | Legendary | 1 | every 4th hit calls lightning (+12) |
| **Static** (New) | Common | 4 | Storm Blade's lightning +4 a level; without Storm Blade, every 8th hit calls a weak bolt (+6) |
| **Thunderclap** (New) | Rare | 1 | the combo's 3rd hit calls lightning (+8) |
| **Conductor** (New) | Rare | 1 | lightning fills the stagger meter like a sword hit |
| **Overcharge** (New) | Legendary | 1 | each stagger: the next 5 hits all call lightning |

### Moon: skills (the skills themselves are in the skill slots, section 6)
| Boon | Tier | Lv | Effect |
|---|---|---|---|
| Moon Mastery | Rare | 3 | cooldowns -20% a level |
| **Lunar Tide** (New) | Common | 3 | sword hits cut skill cooldowns by 0.3 s |
| **Eclipse** (New) | Rare | 1 | a skill that hits a staggered boss deals +50% |
| **Waxing Moon** (New) | Rare | 1 | each skill cast gives +15% sword damage for 4 s |

### Iron: defence and stagger
| Boon | Tier | Lv | Effect |
|---|---|---|---|
| Iron Skin | Common | 3 | -15% damage taken a level (Lv 3: -10%) |
| Vigor | Common | 5 | +25 max health |
| Unbroken | Legendary | 1 | survive one killing blow a floor |
| Crushing Blows | Rare | 1 | staggers last 50% longer |
| Executioner | Legendary | 1 | staggered bosses take +60% |
| **Heavy Hand** (New) | Common | 3 | the combo's 3rd hit fills the stagger meter 20% more |
| **Stalwart** (New) | Rare | 1 | blows over 20% of max health deal 25% less |

### Shadow: dodging
| Boon | Tier | Lv | Effect |
|---|---|---|---|
| Phantom Step | Rare | 1 | easier perfect dodges (0.28 s window; a moving roll's invincibility ends at 0.32 s), each heals 2% |
| Riposte | Rare | 1 | +50% damage for 2.5 s after a perfect dodge |
| Fleetfoot | Rare | 1 | rolls cost 40% less stamina |
| Second Wind | Common | 3 | +40% stamina regen |
| **Shadow Edge** (New) | Common | 4 | +8% damage for 2 s after any roll |
| **Evasion** (New) | Rare | 1 | a perfect dodge refunds the roll's stamina |
| **Vanish** (New) | Legendary | 1 | 3 perfect dodges in a fight: the next blow that lands deals 0 (once a floor) |

### Hearth: potions and steel (the generalists)
| Boon | Tier | Lv | Effect |
|---|---|---|---|
| Ember Edge | Common | 5 | +15% damage a level (Lv 4-5: +10%) |
| Deep Flask | Common | 2 | +3 potions |
| Kindled Flask | Common | 2 | potions heal 25% more |
| **Quick Draught** (New) | Rare | 1 | drinking a potion is 40% faster |
| **Fortified Draught** (New) | Common | 1 | a potion gives -25% damage taken for 5 s |
| **Prospector** (New) | Common | 3 | +20% gold |

**Totals:** 41 boons for the boon slots (the Learn and skill-"+" cards moved to the skill slots), about 100 levels, plus 5 skills (15 levels). A run slots 5.

Healing check: no new boon heals; Hemorrhage and the rest deal damage. The existing healing sources keep the just-shipped values (Bloodthirst 12% of base a level, Rally from base damage, Phantom Step 2%), each about 15-25% of a floor. Rally Lv 2 is the one healing level added: check it in stage 6.

## 8. Duo boons (Legendary evolutions: two maxed boons from different families, merged into one slot)
| Duo | Families | Effect |
|---|---|---|
| Crimson Storm | Blood + Storm | lightning feeds Bloodthirst like a sword hit (no Bloodthirst: lightning +25%) |
| Thunderstep | Storm + Shadow | a perfect dodge calls lightning on the boss |
| Blood Moon Rite | Blood + Moon | a skill cast costs 3% health and deals +40% |
| Iron Tide | Iron + Moon | a staggered boss resets skill cooldowns once per stagger |
| Shade's Mercy | Shadow + Blood | Riposte's window also gives Rally back at 60% |
| Anvil of Storms | Iron + Storm | lightning on a staggered boss deals double |
| Waning Light | Moon + Shadow | a perfect dodge cuts skill cooldowns by 2 s |
| Steel Hearth | Hearth + Iron | each potion drunk adds +5 max health for the floor (up to +25) |

## 9. Rival pairs (taking one removes the other from the run)
| Pair | The fork |
|---|---|
| Riposte vs Iron Skin | punish dodges, or tank blows |
| Blood Pact vs Kindled Flask | damage now, or healing later |
| Storm Blade vs Executioner | steady procs, or stagger bursts |
| Last Stand vs Vigor | live low, or live big |
| Bloodthirst vs Moon Mastery | sword build, or skill build |
| Vanish vs Unbroken | earn your safety net, or be handed one |

A card names its rival: "Locks out: Iron Skin". The Stats > Build page lists locked boons, greyed out.

## 10. "+" upgrades (Rare; the top level, offered once the boon is at its max)
| + | Effect |
|---|---|
| Ember Edge+ | the combo's 3rd hit +25% |
| Iron Skin+ | the first blow each floor deals half |
| Storm Blade+ | every 3rd hit |
| Executioner+ | +90% |
| Crushing Blows+ | a stagger also stops the boss's stagger drain for 3 s after it rises |
| Riposte+ | 4 s |
| Unbroken+ | when it fires, heal 20% |
| Bloodthirst+ | 16% a level |
| Rally+ | each hit counts 6 (its base x1.5) |
| Phantom Step+ | 0.31 s window (still inside the roll's invincibility) |
| Static+ / Thunderclap+ | +50% lightning |
| Moon Mastery+ | -25% a level |
| Second Wind+ | stamina doesn't drain while you drink |

## 11. Elite affixes (Bonus bosses only)
**Only the three Bonus bosses get affixes,** so the Guardian and Unbound stay pure.

**How players know:** an elite is told by the map node, the boss's name bar ("Swift Shackled Sun") and the boss's body, timing and sound. There is nothing glowing on the boss and no rings or floor warnings. One affix per elite in circles 1-3, two from circle 4 on.

| Affix | Effect | How the player can tell |
|---|---|---|
| **Swift** | Moves and recoveries 12% faster; strike timing unchanged | quicker footwork; its intro sound plays higher |
| **Ironclad** | +40% health, stagger bar +30% | the name; the stagger bar is drawn longer (HUD) |
| **Enraged** | Below 50%: blows +25%, breathers -30% | the half-health war cry or break it already has, held longer |
| **Relentless** | Stagger meter drains 2x; stays down 2.5 s (not 3.5) | the name; the HUD stagger bar drains visibly faster |
| **Vampiric** | Heals 3% of the damage it deals | its health bar refills (HUD) |
| **Smouldering** | Its slams leave Shadowborne's floor fire (2 s, 6 dmg/s) | the fire is the hazard itself, drawn where it hurts (reuses Shadowborne's patches; perf check) |

**Rewards:**
- the draft is all Rare or better;
- gold x1.5 and Embers x1.5;
- in circle 4+ with two affixes, one of the cards is a duo when you're eligible.

## 12. How the rest fits
- **Circles and scaling:** unchanged per fight: health +30% a circle (40% from circle 6), damage +20%, the baseline per circle cleared, and nights per circle. An Elite is about x1.4 health, so it costs about 1.6x a fight.
- **Potions:** one per Fight, but only while below the run's max. The rest of the healing comes from Rests, the shop's Mend and events.
- **Embers:** per fight as now. Elites x1.5. +25 for a cleared circle. Shops and events don't give Embers, except the Ember Brazier.
- **Shrine:** unchanged; the upgrades and relics keep working. A possible later Shrine item could unlock a seventh family or more events (open question 6).
- **Pacts:** outside the slots, unchanged. They fill drafts once a build is complete, one sits on every shop's shelf, and they're part of Mirror of Shadows.
- **Skills:** a skill card stays first in each draft while a skill slot is empty or a slotted skill is below Lv 3. The +8% boss health per skill offered stays.
- **Shrine relic "Sage"** (teaches a skill): fills a skill slot at Lv 1.
- **Draft card:** the family tag over the name, then a slot line: "New: slot 4 of 5", "Lv 2 → 3", "+ (top level)", "Duo: merges Storm Blade + Riposte", or for skills "Replaces: Halfmoon Slash"; the rival line under it. Text and rectangles only.
- **Stats menu:** Stats (as now, plus gold and pacts) and **Build**, which replaces the Boons page: the 3 skill slots (D-pad direction, name, Lv), the 5 boon slots (family colour, name, "Lv 3/5"; a duo shows both names), then locked rivals greyed out. About 10 lines: fits the panel.

## 13. Balance case (the agreed metric)
- **Fight cost:** a floor still costs about a Guardian phase. Per-fight scaling is untouched; only the number of fights per circle varies (4-6).
- **A circle's cost:** circle 2 at 21% of resources a floor:
  - 5 fights: 105% of the 352 resources (about 370 HP).
  - 4 fights: 84%.
  - 6 fights, one of them Elite: about 140%.
- **Healing a circle** (circle 2, 220 max health):
  - potions from fights, up to max: 4-6 × about 100 HP, not banked past max;
  - Rest in row 6: 88 HP;
  - shop Mend: 77 HP for 50 gold;
  - the healing boons: about 15-25% of a floor each; with 5 slots a build carries one or two of them, not all.
- **What that means:** a careful path (4 fights plus a Shop and an Event) ends a circle close to even. A greedy path (6 fights, an Elite) is about 35% short unless the build carries it. Elites are the risk you take for rare drafts.
- **To measure in stage 5:**
  - `HLE_RUN_CLIMB` along three set paths (safe, normal, greedy), logging health in and out per circle;
  - `HLE_TEST_DUMMY` on each affixed Bonus boss for its cost.
- **Target:** a normal path is about even through circle 4. Greedy paths win more Embers and better boons but end runs around circle 5 without a strong build.

## 14. Save format
- **No EEPROM change needed.** A run lives only in RAM (as now), and power-off ends it. Keep that: saving a run mid-map would need about 50 bytes per save slot (8 build slots, map, gold, rng), and a save slot has only 24-27 spare.
- Only open question 6 (Shrine unlocks) would add saved bits, a few of the spare bytes, without breaking old saves.
- The best floor, Embers, Shrine and relics are stored as now.

## 15. Memory and ROM
- **Heap:**
  - map state: 21 nodes × 2 bytes;
  - run state: about 70 more bytes (gold, 3 + 5 slots with levels, duo merges, rivals, "+" bits, affixes);
  - all definitions are const in ROM;
  - the map, shop and event screens are drawn as text and rectangles over the paused hall, with no new 3D;
  - the 6 node icons are I4 16x16, about 1 KB, loaded with the draft UI and freed with it.
  - That's about 2 KB at the low point (400-500 KB must stay free).
- **ROM:**
  - card and event text: about 6 KB;
  - code: about 15-20 KB;
  - icons: 1 KB.
- **ROM size risk:** HLE slack was about 12.9 KB at a short path, so the release ROMs probably grow one 16 KiB step. The rule that sizes read exactly 20.6 / 12.6 MB may need dropping, or audio trimmed to pay for it: that's the user's call.
- **Frame time:** none in fights. Elite affixes are table lookups; Smouldering reuses Shadowborne's fire patches, so measure that fight in Ares at 4 MB.

## 16. Build order (each stage on branch tower-rogue, with a pre-release for the user to play)
| Stage | What | Test aids |
|---|---|---|
| 1. Slots and families | 3 skill slots and 5 boon slots with levels; stacks become levels; 41 boons in families (20 new); family weighting; the card's slot line; Stats > Build | `HLE_TEST_TOWER` gains slot, level, full-bar and weighting checks (self-check); `HLE_DRAFT_GALLERY` for card text fit; `HLE_TEST_TOWER_STATS` shows a full Build page; `HLE_LOG_BOONFX` logs each new effect firing in a fight |
| 2. Rivals, "+", duos | rival lockout in `offerable()`; "+" as top levels; 8 duos merging two slots | self-check: a duo offered only with two maxed boons of different families, merging them and opening a slot; `HLE_TOWER_GIVE` for each duo in a fight |
| 3. The map | generation rules, the map screen, Fight/Elite/Rest nodes (Temper, Release), floor/circle bookkeeping | `HLE_TEST_TOWER_MAP`: 1,000 seeded maps checked against the rules (self-check); a scripted path through a circle with shots of the map |
| 4. Gold, shop, events | economy, shop menu (levels, Release), 8 events | `HLE_TEST_TOWER_SHOP`, `HLE_EVENT_FORCE=n` with shots of each screen and logged results |
| 5. New skills | Storm Call, Twin Crescent; skill cards that replace a slotted skill | `HLE_SKILL_FORCE` with videos per skill; `[hit]` logs proving each lands where it's drawn; Ares frame time for Twin Crescent |
| 6. Elite affixes + balance | 6 affixes on the Bonus bosses; the cost measurements in section 13 | `HLE_ELITE_FORCE=affix` + `HLE_TEST_DUMMY`; `HLE_RUN_CLIMB` with `HLE_RUN_PATH=safe/normal/greedy`; Ares at 4 MB for heap and frame time |

Each stage closes with:
- the existing `HLE_TEST_TOWER`, `HLE_TEST_TOWER_RUN`, `HLE_TEST_TOWER_UI`, `HLE_TEST_TOWER_STATS` and `HLE_TEST_CYCLE` runs;
- a Delta-core run, which halts on NaN compares.

## 17. Open questions for the user
Answered: fights a path stay 4-6; families have no cap (weighting only); slots are 3 skills + 5 boons.

1. **Potions:** keep one per fight up to the max (as above), cut it to one per circle and let Rests carry the healing, or leave it as today?
2. **Gold at death:** lost (as above), or a share turned into Embers?
3. **Smouldering:** OK to reuse Shadowborne's floor fire on other bosses, or keep affixes to stats only?
4. **Node icons:** six new 16x16 icons, or letters (F, E, ?, $, R) to save the art?
5. **The new boons and skills:** any of the 20 new boons, or Storm Call / Twin Crescent, to cut or change? Are the levels right (Commons 3-5, Rares 1-3)? Should Hemorrhage's bleed be shown, e.g. by the boss's hit flash?
6. **The Shrine:** should Embers unlock part of the new content (families, duos, events), so it is earned over runs, or should everything be in from the start?
7. **Run seed:** show a seed at the end of a run so it can be shared and replayed (it's in ROADMAP's ideas; the map makes it more interesting)?
8. **ROM size:** if the release ROMs grow past 20.6 / 12.6 MB, accept it or trim elsewhere?
9. **Swapping:** Release free at a Rest and 20 gold at a Shop (as above), or Shops only?
10. **Skill replacement:** may a skill card in a draft replace a slotted skill (as above), or only at Rests?
