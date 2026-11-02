# The Tower as a roguelike: design for approval

Status (2026-11-01): the user's answers are in (section 17); the remaining calls were made by the game-theory check (section 14). Ready for approval to build, stage by stage.

Stage 1 built (2026-11-01, pre-release v1.1-beta.1): 5 slots with levels, families on the cards and their weighting, the slot line, Stats > Build, the Shrine's Boon Packs in `_pad[24..25]`. Calls made while building it:
- Section 7's tables hold 18 existing and 17 new boons (35), not 40 / 19 new. Hearth's two are held back: **Quick Draught** speeds up a drink the game doesn't have (a potion heals at once) and **Prospector** needs gold (stage 4); so 15 new boons in five packs, and the ladder sells five (120-280).
- The skills keep today's Learn and "+" cards (stage 5 turns them into levels): no slot, a "Skill" tag. Moon Mastery is a Moon boon now, in a slot, offered once a skill is known (as Lunar Tide, Eclipse and Waxing Moon); Conductor once something calls lightning.
- Static without Storm Blade: a bolt of 6 every 8th hit, +3 a level above 1 (so its levels do something). Overcharge's bolts are Storm Blade's (6, +3 a Static level). Red Thirst counts full quarters of health missing. Vanish's blow does nothing at all (no flinch). Bleed ticks every 0.5 s and wins back no Rally.
- The B forms' 3 bits are kept but not sold until stage 5.
- Stats keeps its page; gold isn't there until stage 4.

Stage 2 built (2026-11-02, pre-release v1.1-beta.2): rival lockout, 13 "+" cards, the 9 duos merging two slots (ROADMAP has the numbers). Calls made while building it:
- A rival stays locked for the run once its pair is taken (a later Release, stage 3, won't free it), and a draft never holds both.
- "+" cards are their own cards (Rare, no slot, the boon's family tag; "+ (top level)"). **Second Wind+ is held back:** potions are drunk at once, so there's no drink to keep stamina through. Static+ raises Storm Blade's and Static's bolts; Thunderclap+ its own (8 to 12). Unbroken+ heals 20% of max on top of the 1 HP.
- A duo merges the first maxed boon (lowest slot) of each of its families; its card lists the two ("Merges / A / + B"). Duos join the pool once both families' packs are bought (section 14b); Hearth has no pack yet, so it counts as bought (Steel Hearth needs only the Iron pack).
- Crimson Storm's healing is half of Bloodthirst's rate applied to the bolt. Thunderstep's bolts are Storm Blade's (6, +3 a Static level) with or without Storm Blade. Shade's Mercy keeps a Rally pool and opens Riposte's window on a perfect dodge without either boon; hits in it win back 60% of their base (1.6x with Rally). Blood Moon Rite never takes the last health. Iron Tide resets cooldowns when the stagger starts.
- **Skyfall is built here** (stage 2 lists all 9 duos; stage 5 mentions it with the skill builds): a skill's first hit a cast calls the pillar for 6.
- Vigor and Vitality raised only the health mid-run (max health was set once, at the run's start; main has this too): they now raise max health, as Steel Hearth needs.

Stage 3 built (2026-11-02): the map, Fight/Elite/Rest nodes, Temper and Release, floors and circles by the map. Calls made while building it:
- **No Events or Shops are rolled until stage 4** (the stage split doesn't say whether their nodes may sit on the map unused): rows 2-5 roll Fight 45, Elite 15, Rest 8 (their share of 68), and the Shop-in-rows-3-4 rule waits for Shops. With the rules' rerolls a map holds about 77% Fights, 16% Elites, 7% Rests in rows 2-5 (1,000 maps), and 1% of paths have 4 fights, 25% 5, 74% 6; stage 4's Events and Shops bring those down.
- "Every lane has at least 2 Fights" counts Fight nodes (not Elites). "No two Rests in a row on one path" also keeps row 5 free of Rests (row 6 is all Rests).
- A Fight's or Elite's boss is set when its node becomes reachable (so the bottom line can name it): a Bonus boss not met this circle while any are left, else any of the three.
- **An Elite in stage 3:** x1.25 health (section 12; Bonus bosses only), a Rare-or-better draft (Commons and Learn cards are left out; a skill card leads only if a Rare one is offerable), x1.5 Embers. No affix (stage 6), no gold (stage 4); its name bar is the boss's own. The bottom line reads "Elite: Shackled Sun, rare draft, x1.5 Embers".
- **Rest:** heal 40% of max health, **Temper** (a slotted boon's next level, the "+" of a maxed one, or a known skill's "+" card, the skills' only levels before stage 5; never a new boon, a Learn card or a duo), or **+2 potions up to the cap of 15** (section 12: fights already bank potions past the run's max, so capping at that max would grey the choice out most of the time). Then **Release** (Keep first, so A twice keeps everything): the boon, its levels and its "+" go, a duo with its two boons; a rival it locked stays locked; Vigor's max health goes with it.
- Rest controls: Up/Down (D-pad or stick) along the rows, past one that can't be chosen (Temper with nothing to raise, potions at 15), A takes it; Temper's list ends with Back. Every screen after a fight waits 0.75 s for input, as the draft.
- Embers per fight: 8 x circle + 3 x the fight's place in its circle (0 for the Guardian, up to 5 for Unbound on a 6-fight path), as before for a 5-fight path.
- The map shows after every draft and before each circle's Guardian (the run's first floor too: the Guardian waits in the hall under it). The top line has no gold until stage 4.
- Icons: one 96x16 I4 strip (768 B, `assets/ui/map/map_icons.i4.png`), loaded when the map opens and freed when a fight starts.

Scope approved by the user (2026-11-01): a branching map each circle (4-6 fights a path), Vampire Survivors-style slots (5 boon slots, boons levelling inside their slot; the three skills level outside them, with two final forms each, and no new skills), boon families with duos as evolutions, rival pairs and "+" upgrades, and elite affixes. Not in scope: rerolls, removing boons, relics found mid-run. Nothing here is built yet. Numbers are starting points for the balance pass (stage 5).

## 1. Why every deep run ends the same
- A run drafts once per floor. All 24 boons together have **34 stacks**. By floor 35 a player has had 34 drafts, so a deep run can own everything. The screenshot from 2026-11-01 showed exactly that.
- Early picks don't close any doors, so they don't shape the run.
- The Bonus fights in a circle always come in the same order; only the shuffle changes.

The fix has four parts:
- **Slots:** a build holds 5 boons, no more, and draft picks are split between them and the 3 skills' levels: a run goes deep on one skill or on the sword, never on everything (section 6).
- **A pool too big to see:** 40 boons in six families, 3 skills with 5 levels and 2 final forms each, 9 duos and 13 "+" upgrades. A run slots 5 of the 40 boons.
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
| **Moonlit Pool** | Learn a skill you don't know (if any). / +1 level on a skill you know (below Lv 4). / Leave. |
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

## 6. Slots and skill builds (Vampire Survivors style)
A build is **5 boon slots** plus **the 3 skills**, which level outside the slots. Pacts sit outside the slots too.

### The 3 skills (no new skills)
- The three skills stay the only ones: new skills would need their own animation, effects, sound, timing and review. Each skill now has a **level track** of mechanical upgrades (more crosses, more blades, a follow-up slash) built from its existing waves, beams and slash, and **two final forms** at Lv 5 (choose one, like a Hades aspect).
- Skills take no boon slot, but every level is a draft pick: maxing all three skills (15 picks) and five boons (about 20) is more than a run gets, so a run picks a lane: **a skill build** (one skill to Lv 5, Moon boons in the slots) or **a sword build** (skills left low or unlearned).
- Lv 1 is the Learn card, as now. At Lv 4, the next draft shows both final forms as two of its three cards; taking one locks the other for the run.
- The +8% boss health per skill offered stays (it counts skill cards offered, as now).

### Where the skills stand today (circle 1, before damage boons; the sword: 4 a hit, about 0.65 s, so 6.2 a second in an opening)
| Skill | A cast | Cooldown | Per minute at its cooldown | A second of the cast |
|---|---|---|---|---|
| Moon Blade Dance | 5 crosses x 0.8 = **4** (7 crosses = 5.6 with MBD+) | 15 s | 16 (22) | 1.8 (sword 6.2): a dodge, not damage |
| Halfmoon Slash | **28** (Halfmoon+: 34, +20% since the nerf) | 45 s | 37 (45) | 8.8, but rooted 2 s, and a hit breaks it |
| Crescent Cleave | 5 blades, about 8 ticks in reach x 1.25 = **10** (Cleave+: 12) | 18 s | 33 (40) | 5.9 |

Why Halfmoon+ was nerfed (0696734): at +50% it did 42 a cast, about 48% of a circle-1 boss (87 HP with three skills offered), and with Moon Mastery x2 and Starfall its 45 s fell to about 20 s, so 2-3 casts took most of a fight. That sets the ceiling below: no single cast over about a quarter of a floor's boss at the depth its Lv 5 is reached (circle 3: about 185 HP).

### Level tracks
Non-damage levels (charge time, wind-up, reach) make a skill safer to use; damage comes from more of the same projectiles, not flat multipliers.

| Lv | Moon Blade Dance | Halfmoon Slash | Crescent Cleave |
|---|---|---|---|
| 1 | Learn: 5 crosses x 0.8 (4) | Learn: 28 | Learn: 5 blades (10) |
| 2 | crosses 1.5 each (7.5) | charge 1.6 s (was 2.0) | +20% (12): today's Cleave+ |
| 3 | +2 crosses: 7 (10.5): today's MBD+ | +20% (34): today's Halfmoon+ | blades hit 5 times (was 4): about 10 ticks in reach (15) |
| 4 | cooldown 12 s (was 15) | reach +25% (85 to 106): easier to land | wind-up 0.6 s (was 0.85): safer to start |
| 5 A | **Full Moon Dance:** 10 crosses (+3), 2.0 each: **20** | **Twin Moon:** a follow-up slash 0.35 s after, the same arc at 40%: **48** | **Crescent Storm:** the cast's 5 blades, then 3 more following 0.3 s behind the middle three (8 in all), at full damage: about **24** |
| 5 B | **Shattering Dance:** 7 crosses, each fills the stagger meter like a fifth of a sword hit (1.4 hits a cast, landed in 1.75 s, so none drains away): the core of the stagger build below | **Full Moon:** a 360-degree slash, its charge can't be broken, stagger 60% (was 30%): a break about every 2 casts | **Long Cleave:** blades go 1.6x as far and hit 6 times (was 5): about 22, from range |

### A maxed skill build against a maxed sword build (base damage a minute, before the damage boons both share)
Assumes a fight where you swing in openings about 24 s a minute (148 a minute with the plain sword), and the build's slots hold Moon Mastery Lv 2 (cooldowns x0.64). Starfall nights add x0.7 on top.

| Build | Its engine | A minute | Total with the sword it still swings |
|---|---|---|---|
| Sword: Ember Edge, Storm Blade+Static, Thunderclap, Riposte | lightning on every 4th hit (+6, +3 a Static level; Storm Blade was +12, section 14) | about +95 on the sword's 148 | about **245** |
| Full Moon Dance | 20 a cast every 7.7 s, airborne 2.9 s a cast (it doubles as the dodge) | about 155 | about 155 + 80 = **235** |
| Twin Moon | 48 a cast every 29 s (Starfall: 20 s) | about 100 (Starfall 145) | about 100 + 130 = **230** (275) |
| Crescent Storm | 24 a cast every 11.5 s, 2.2 s a cast | about 125 | about 125 + 110 = **235** |
| Stagger (Shattering Dance) | about 1.7 breaks a minute (sword 1, Shattering 0.5, Heavy Hand 0.2), each 5.25 s with Executioner's +60% | breaks about +65 over a plain build's one, crosses 44 | about 148 + 65 + 44 = **255** |

- Every maxed build lands in a band of **216-264** (the five builds' mean of 240, ±10%), so a skill build is a real path, not a gimmick or the best pick. Section 14 checks the contexts each one wins in.
- The ceiling check: Twin Moon's 48 is about 26% of a circle-3 boss a cast (Halfmoon+ at +50% was 48% at circle 1). Full Moon Dance's 20 is about 11% a cast, spread over 10 crosses.
- Numbers to measure in stage 5 with `HLE_TEST_SKILL_DMG` and `HLE_SDMG_TOWER`; tune the per-cross / follow-up / second-volley shares first.

### Cost and risk of each mechanic (all reuse existing code)
| Mechanic | Implementation | Perf / memory risk |
|---|---|---|
| More crosses (MBD Lv 3, Full Moon Dance) | `mbd_waves()` already takes extra waves (Crescent Echo) | low: crosses fly 0.25 s apart and live about 1 s, so about 5 are alive at once as now (104 triangles each) |
| Cross damage, stagger | the wave's damage and `skill_fx_next_poise`, as the Cleave does | none |
| Halfmoon follow-up (Twin Moon) | replay the arc's hit and FX once more 0.35 s later | low: one more arc for 0.3 s |
| Halfmoon 360 degrees, unbreakable charge | the arc's angle, skip the break check | low |
| 3 trailing blades (Crescent Storm) | the blade loop runs 3 more steps 0.3 s later on the middle three of `spread[]` | **low-medium:** 8 blades and 8 bursts alive (was 5 and 5); with the Sun's 20 embers that's 36 of `SKILL_FX_MAX` 40, so the pool stays as it is (stage 5 logs any spawn that finds it full); measure the cast's draw time in Ares at 4 MB and on Delta's core against today's 5 blades |
| Long Cleave | the blade's speed, life and hit count | low: 5 blades alive 2.5 s |

### The stagger build (Shattering Dance at its core)
The user's pick for a build: stagger the boss often, keep it down longer, hit it harder while it's down.

| Piece | Where it comes from | What it adds |
|---|---|---|
| Shattering Dance | Moon Blade Dance Lv 5 B | a cast every 7.7 s (Moon Mastery Lv 2) puts 1.4 sword hits into the meter at once |
| Heavy Hand Lv 3 | Iron (New) | the combo's 3rd hit fills 60% more |
| Crushing Blows (+: the drain stays off 3 s after it rises) | Iron | 5.25 s down (3.5 s) |
| Executioner (+: +90%) | Iron, Legendary | +60% on a staggered boss; locks out Storm Blade |
| Moon Mastery Lv 2 | Moon | the casts that feed the meter |
| Eclipse | Moon | the dance's crosses on a downed boss +50% |
| Breaker pacts | pact cards | +15% a stack on staggers, paid with their curses |
| Iron Tide (duo, Iron + Moon) | Iron + Moon maxed | a break resets the dance: a second cast on the downed boss |

- How it plays: dance, land the combo's 3rd hits, the boss drops about every 35 s and stays down 5.25 s; you spend the down time swinging.
- Its counters: **Relentless** elites (drain 2x, down 2.5 s) and **Ironclad** (+30% bar) cut it to about 205, the bottom of the band; Swift and Smouldering don't touch it.
- Stagger bonuses **add** (Executioner +60% + Breaker +15% a stack + Eclipse on skills), not multiply, capped at +100%: with 3 Breaker pacts it stops at about 275.

### Families and skills
- **Moon** holds the skill-build boons (Moon Mastery, Lunar Tide, Eclipse, Waxing Moon): a skill build fills its slots there.
- **Storm + Moon duo, Skyfall** (new, below): a skill hit calls Storm Blade's pillar once a cast (+6, its existing hit and FX).
- **Iron's Executioner** pairs with the stagger forms (Shattering Dance, Full Moon).

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
| 5 boons to their max (about 3.5 levels each), 2-3 "+", 1-2 duos | about 22 |
| One skill to Lv 5 (a skill build) | 5 |
| All three skills to Lv 5 | 15 |
| Drafts by floor 35 (7 circles, 4-6 fights each) | about 33 (plus Rests and Shops) |

- A skill build (22 + 5, plus a second skill partly) or a sword build (22, skills low) is complete around floor 30-35, but everything (22 + 15 = 37) is more than a run gets.
- A deep run holds 5 of 40 boons and one of six skill forms: two deep runs share a boon or two, not the whole list.
- Late drafts are levels, "+", duos, skill levels and pacts; when a build is complete, pacts and a **+40 gold** card fill the rest.
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
| **Hemorrhage** (New) | Common | 3 | the combo's 3rd hit deals +4 bleed over 3 s (damage only, no heal); each bleed tick shows as the boss's existing hit flash, a quarter as bright |
| **Sanguine Flask** (New) | Rare | 1 | drinking a potion gives +25% damage for 6 s |
| **Red Thirst** (New) | Common | 3 | +10% damage per 25% of health missing |

### Storm: hit-count procs
| Boon | Tier | Lv | Effect |
|---|---|---|---|
| Storm Blade | Legendary | 1 | every 4th hit calls lightning (+6; was +12, which made it best in every build: section 14) |
| **Static** (New) | Common | 4 | Storm Blade's lightning +3 a level; without Storm Blade, every 8th hit calls a weak bolt (+6) |
| **Thunderclap** (New) | Rare | 1 | the combo's 3rd hit calls lightning (+8) |
| **Conductor** (New) | Rare | 1 | lightning fills the stagger meter like a sword hit |
| **Overcharge** (New) | Legendary | 1 | each stagger: the next 5 hits all call lightning |

### Moon: skills (the skills' own levels and forms are in section 6)
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
| **Prospector** (New) | Common | 3 | +20% gold |

**Totals:** 40 boons for the boon slots (the Learn and skill-"+" cards became skill levels), about 100 levels, plus 3 skills (5 levels each, 2 forms at Lv 5). A run slots 5.

Healing check: no new boon heals; Hemorrhage and the rest deal damage. The existing healing sources keep the just-shipped values (Bloodthirst 12% of base a level, Rally from base damage, Phantom Step 2%), each about 15-25% of a floor. Rally Lv 2 is the one healing level added: check it in stage 6.

## 8. Duo boons (Legendary evolutions: two maxed boons from different families, merged into one slot)
| Duo | Families | Effect |
|---|---|---|
| Crimson Storm | Blood + Storm | lightning feeds Bloodthirst at half a sword hit's rate (a full rate healed ~36% of a floor, over the 15-25% target); no Bloodthirst: lightning +25% |
| Thunderstep | Storm + Shadow | a perfect dodge calls lightning on the boss, twice 0.3 s apart |
| Blood Moon Rite | Blood + Moon | a skill cast costs 3% health and deals +40% |
| Iron Tide | Iron + Moon | a staggered boss resets skill cooldowns once per stagger |
| Shade's Mercy | Shadow + Blood | Riposte's window also gives Rally back at 60% |
| Anvil of Storms | Iron + Storm | lightning on a staggered boss deals double |
| Waning Light | Moon + Shadow | a perfect dodge cuts skill cooldowns by 2 s |
| Steel Hearth | Hearth + Iron | each potion drunk adds +10 max health for the floor (up to +30) and -40% damage taken for 4 s (was +5/+25 alone: below the band) |
| Skyfall | Storm + Moon | a skill that hits calls Storm Blade's pillar on the boss once a cast (+6) |

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
| **Ironclad** | +30% health, stagger bar +30% | the name; the stagger bar is drawn longer (HUD) |
| **Enraged** | Below 50%: blows +25%, breathers -30% | the half-health war cry or break it already has, held longer |
| **Relentless** | Stagger meter drains 2x; stays down 2.5 s (not 3.5) | the name; the HUD stagger bar drains visibly faster |
| **Vampiric** | Heals 8% of the damage it deals | its health bar refills (HUD) |
| **Smouldering** | Its slams leave Shadowborne's floor fire (2 s, 6 dmg/s); not rolled on Shadowborne | the fire is the hazard itself, drawn where it hurts (the shared floor-fire pool; perf check in section 15) |

**Rewards:**
- the draft is all Rare or better;
- gold x1.5 and Embers x1.5;
- in circle 4+ with two affixes, one of the cards is a duo when you're eligible.

## 12. How the rest fits
- **Circles and scaling:** unchanged per fight: health +30% a circle (40% from circle 6), damage +20%, the baseline per circle cleared, and nights per circle. An Elite is x1.25 health plus its affix: 1.5-1.7x a fight (section 14).
- **Potions:** as today (the user's call): one per fallen boss, even past the run's max, up to the cap of 15. Rests, the shop's Mend and events add to it.
- **Embers:** per fight as now. Elites x1.5. +25 for a cleared circle. Shops and events don't give Embers, except the Ember Brazier.
- **Shrine:** the upgrades and relics keep working, and it now also sells the new content (the unlock ladder, section 14b).
- **Pacts:** outside the slots, unchanged. They fill drafts once a build is complete, one sits on every shop's shelf, and they're part of Mirror of Shadows.
- **Skills:** a skill card stays first in each draft while any skill is below Lv 5 (a Learn card while one is unknown). The +8% boss health per skill offered stays.
- **Shrine relic "Sage"** (teaches a skill): that skill starts at Lv 1.
- **Draft card:** the family tag over the name, then a slot line: "New: slot 4 of 5", "Lv 2 → 3", "+ (top level)", "Duo: merges Storm Blade + Riposte"; for skills "Halfmoon Slash Lv 3 → 4" or, for a form, "Lv 5 form: Twin Moon (locks out Full Moon)"; the rival line under it. Text and rectangles only.
- **Stats menu:** Stats (as now, plus gold and pacts) and **Build**, which replaces the Boons page: the 3 skills (D-pad direction, name, "Lv 4/5" or the form's name), the 5 boon slots (family colour, name, "Lv 3/5"; a duo shows both names), then locked rivals and forms greyed out. About 10 lines: fits the panel.

## 13. Balance case (the agreed metric)
- **Fight cost:** a floor still costs about a Guardian phase. Per-fight scaling is untouched; only the number of fights per circle varies (4-6).
- **A circle's cost:** circle 2 at 21% of resources a floor:
  - 5 fights: 105% of the 352 resources (about 370 HP).
  - 4 fights: 84%.
  - 6 fights, one of them Elite: about 140%.
- **Healing a circle** (circle 2, 220 max health):
  - potions from fights: 4-6 × about 100 HP, banked up to 15 (as today);
  - Rest in row 6: 88 HP;
  - shop Mend: 77 HP for 50 gold;
  - the healing boons: about 15-25% of a floor each; with 5 slots a build carries one or two of them, not all.
- **What that means:** a careful path (4 fights plus a Shop and an Event) ends a circle close to even. A greedy path (6 fights, an Elite) is about 35% short unless the build carries it. Elites are the risk you take for rare drafts.
- **To measure in stage 5:**
  - `HLE_RUN_CLIMB` along three set paths (safe, normal, greedy), logging health in and out per circle;
  - `HLE_TEST_DUMMY` on each affixed Bonus boss for its cost.
- **Target:** a normal path is about even through circle 4. Greedy paths win more Embers and better boons but end runs around circle 5 without a strong build.

## 14. Game theory check (no option strictly better)
Method, as for the bosses: each option's value is its change in **fight cost** (boss damage a minute × 30% × time to kill, over the player's resources), estimated from the code's numbers at a reference build (circle 3, 2 slots filled). The test for each choice set: every option wins in some context, and the options' expected values sit within **±10% of the set's mean**. An option outside the band, or never best anywhere, is changed. All figures are estimates to measure in stages 5-6 (`HLE_TEST_DUMMY`, `HLE_RUN_CLIMB`, `HLE_SDMG_TOWER`).

### Builds (base damage a minute, maxed; band 216-264)
| Build | Value | Best when | Worst when |
|---|---|---|---|
| Sword (Storm) | 245 | long openings (Guardian, Unbound), Swift elites | bosses that fly or keep away (Shadowborne) |
| Full Moon Dance | 235 | fast bosses (the dance is the dodge), Smouldering | Ironclad (long fights) |
| Twin Moon | 230 (275 Starfall) | Starfall nights, burst at low boss health (Wounded Foe) | Swift and Enraged: the 2 s root gets hit |
| Crescent Storm | 235 | mid range, the Sun's hover | close-in bosses that step through the fan |
| Stagger (Shattering) | 255 | Guardian, Fallen Warrior (long downs) | Relentless, Ironclad (about 205) |

Fixed: Shattering at half a hit a cross gave about 330 (dominant: the boss down 26% of the fight); now a fifth of a hit. Crescent Storm's second 7-blade volley became 3 trailing blades (the user's design): 25 to 24.

### Skill forms (pairs)
| Skill | A | B | A wins | B wins |
|---|---|---|---|---|
| Moon Blade Dance | Full Moon Dance 235 | Shattering 255 with its Iron pieces, 190 without | no Iron boons slotted, Relentless | Executioner and Crushing Blows slotted |
| Halfmoon Slash | Twin Moon 230 | Full Moon 239 (a break every 2 casts) | Starfall, damage boons | fast bosses (its charge can't break), stagger builds |
| Crescent Cleave | Crescent Storm 235 | Long Cleave 225 | close and mid range | Shadowborne's flight, safe play at low health |

Every form is within band and best somewhere; none is taken in every build.

### Rival pairs (the pick's value in fight cost)
| Pair | Left | Right | Left wins | Right wins |
|---|---|---|---|---|
| Riposte vs Iron Skin Lv 1 | -11% | -15% | perfect-dodge builds (with Phantom Step, Shadow Edge: -18%) | everyone else |
| Blood Pact vs Kindled Flask | -14% | -12% | short fights, sword builds | potion-heavy runs (Deep Flask) |
| Storm Blade vs Executioner | -27% | -8% (stagger build: -24%) | sword builds | stagger builds |
| Last Stand vs Vigor Lv 1 | -10% (risky) | -10% | Blood builds living low | safe play |
| Bloodthirst vs Moon Mastery Lv 1 | -12% | -13% (skill builds; -4% sword) | sword builds | skill builds |
| Vanish vs Unbroken | -9% | -11% | skilled dodgers (Shadow) | anyone, deep circles |

Fixed: **Storm Blade** at +12 every 4th hit was +75% on the sword (-43%), best in every build and against its rival; now **+6** (Storm Blade+ every 3rd hit, Static +3 a level). It stays Legendary.

### Duos (a freed slot, about -12%, plus the effect; band -17% to -27%)
| Duo | Value | Best in |
|---|---|---|
| Crimson Storm | -20% | sword builds with Bloodthirst |
| Thunderstep | -18% | dodge builds |
| Blood Moon Rite | -24% | skill builds at high health |
| Iron Tide | -25% | the stagger build |
| Shade's Mercy | -19% | Blood + Shadow dodgers |
| Anvil of Storms | -22% | Storm + Iron (Conductor) |
| Waning Light | -20% | skill dodgers |
| Steel Hearth | -19% | potion runs, deep circles |
| Skyfall | -22% | skill builds with Storm |

Fixed: Crimson Storm (its healing was over the 15-25% target), Steel Hearth (was -16%, below the band) and Thunderstep (-14%, now two bolts).

### Families (a maxed 5-slot build from one family plus one other)
| Family core | Value | Best when |
|---|---|---|
| Blood | -40% | Blood Moon nights, fast kills |
| Storm | -42% | long openings |
| Moon | -42% | Starfall, ranged bosses |
| Iron | -41% | stagger, deep circles (big blows) |
| Shadow | -38% | players who perfect-dodge |
| Hearth | -36% | anyone: no condition, no counter |

Mean -40%, band -36% to -44%: all in. Hearth sits at the bottom on purpose: the safe pick that never fails.

### Map routes (a circle, an average build)
| Route | Fights | Health out of the circle | Embers a circle | Best when |
|---|---|---|---|---|
| Safe (Event, Shop, Rest) | 4 | about even | about 85 | low health, circles 4+, a build still missing pieces |
| Normal | 5 | -10% | about 105 | most circles |
| Greedy (an Elite, 6 fights) | 6 | -35% | about 140 | full health, circles 1-3, a finished build |

Expected Embers a run come out within ±10% across the three for an average player (greedy earns more a circle but ends about a circle earlier), so no route is always right.

### Elites (enter one, or go round)
An elite is the boss at x1.25 health plus its affix; target 1.5-1.7x a plain fight, for a Rare-or-better draft (about +1.3 picks) and x1.5 gold and Embers.

| Affix | Cost | Hardest for | Easiest for |
|---|---|---|---|
| Swift | x1.55 | skill builds (casts get hit) | sword builds |
| Ironclad (+30% health, bar +30%) | x1.65 | stagger builds | Storm |
| Enraged | x1.55 | low-health players | burst (Twin Moon) |
| Relentless | x1.5 (stagger build x1.7) | stagger builds | everyone else |
| Vampiric (8%, was 3%) | x1.5 | slow builds | burst builds |
| Smouldering | x1.6 | close-in sword builds | ranged skills |

Fixed: Ironclad's +40% on an x1.4 elite cost about 2x (everyone would go round); now x1.25 with +30%. Vampiric at 3% cost almost nothing (always taken); now 8%.

### Release: at a Rest, a Shop, or both (open question 9: both)
| Where | Cost | Wins when |
|---|---|---|
| Rest (free, with the Rest's choice) | the boon's levels | the row-6 Rest before Unbound: respec for the circle's boss |
| Shop (20 gold) | the levels + 20 gold (about a third of a level) | mid-circle, when a duo or a rival opens up |

Neither is always better (one is free but late, the other early but paid), and the levels lost are the real price either way, so both stay.

### The calls left to my judgement
- **Map icons:** six 16x16 icons (about 1 KB), not letters: the map reads at a glance on a CRT.
- **Skill cards in drafts:** a Learn card leads every draft until a skill is known, as today. After that, skill levels are ordinary cards, twice as likely for a skill at Lv 2+. About 11 skill cards come by floor 35: enough for one skill to Lv 5 and part of a second, and a sword build sees about one skill card in three drafts instead of one in every draft.
- **The new boons:** Fortified Draught is cut (-3%: below Iron Skin and Kindled Flask everywhere). Hemorrhage's bleed shows as the boss's existing hit flash. Storm Blade, Crimson Storm, Steel Hearth, Thunderstep, Vampiric and Ironclad changed as above.

## 14b. Unlocks (the Shrine)
In from the start: today's 24 boons (now in their families), the map with every node type, the A forms of the three skills, 4 events and 3 affixes (Swift, Ironclad, Enraged).

| Unlock | How | Price |
|---|---|---|
| A family's new boons (6 packs, 3-4 boons each, in the player's order) | Embers at the Shrine | 120, 160, 200, 240, 280, 320 (1,320) |
| A skill's B form (Shattering Dance, Full Moon, Long Cleave) | Embers | 200 each (600) |
| Duos | free: a duo joins the pool once both its families' packs are bought | - |
| Events 5-8 | best floor 10, 15, 20, 25 | - |
| Affixes Relentless, Vampiric, Smouldering | best floor 15, 20, 25 | - |

- Ember income (the code's `floor_embers`): a circle earns about 95 at circle 1, 135 at circle 2 and 175 at circle 3, including the +25 for clearing it. An early run (floor 8) brings about 150, a mid run (floor 15) about 400, a deep one about 1,100.
- The ladder (1,920) on top of today's Shrine (upgrades 950, relics, Nightfarer 450) takes about 20-30 runs at an average of 200 a run: something new about every 1-2 runs.

## 14c. Save format
- A run still lives only in RAM: power-off ends it, as now.
- Correction: a slot has **5** spare bytes (`_pad[14]` and `_pad[24..27]`), not 24-27. `_pad[16..18]` (pre-1.0 relics) could be freed after their conversion on load, for 8.
- The unlocks take **9 bits** in `_pad[24..25]`: 6 family packs and 3 B forms. Events and affixes come from the saved best floor and duos from the packs, so they need no bits. Old saves read zeros: nothing bought, Embers and best floor kept.
- The run seed is shown at the end of a run (the death screen and Stats), not saved.

## 15. Memory and ROM
- **Heap:**
  - map state: 21 nodes × 2 bytes;
  - run state: about 70 more bytes (gold, 5 slots with levels, 3 skill levels and forms, duo merges, rivals, "+" bits, affixes);
  - `SKILL_FX_MAX` stays 40 (Crescent Storm: 16 entries at most, 36 with the Sun's spiral);
  - all definitions are const in ROM;
  - the map, shop and event screens are drawn as text and rectangles over the paused hall, with no new 3D;
  - the 6 node icons are I4 16x16, about 1 KB, loaded with the draft UI and freed with it.
  - That's about 2 KB at the low point (400-500 KB must stay free).
- **ROM:**
  - card and event text: about 6 KB;
  - code: about 15-20 KB;
  - icons: 1 KB.
- **ROM size:** accepted (the user's call). About 25-30 KB more: one or two 16 KiB steps, so the release pages likely read about 20.6-20.7 / 12.6-12.7 MB.
- **Frame time:** none in fights, except where noted. Elite affixes are table lookups.
  - **Smouldering** uses the shared floor-fire pool in `skill_fx` (24 points, static) that the Fallen Warrior's fire lunge already draws. In the Fallen Warrior's scene it costs nothing new; in the Shackled Sun's it adds the fire texture if that scene doesn't hold it (up to 30 KB: check against its heap low point at 4 MB); Shadowborne doesn't roll it (it has its own fire). The patches are the attack itself, drawn where they burn: no warnings or glows.
  - Crescent Storm draws 8 blades (5 today).
  - Measure both in Ares at 4 MB and on Delta's core.

## 16. Build order (each stage on branch tower-rogue, with a pre-release for the user to play)
| Stage | What | Test aids |
|---|---|---|
| 1. Slots and families | 5 boon slots with levels; stacks become levels; 40 boons in families (19 new); family weighting; the card's slot line; Stats > Build; the Shrine's unlock ladder and its 9 save bits | `HLE_TEST_TOWER` gains slot, level, full-bar and weighting checks (self-check); `HLE_DRAFT_GALLERY` for card text fit; `HLE_TEST_TOWER_STATS` shows a full Build page; `HLE_LOG_BOONFX` logs each new effect firing in a fight |
| 2. Rivals, "+", duos | rival lockout in `offerable()`; "+" as top levels; 9 duos merging two slots | self-check: a duo offered only with two maxed boons of different families, merging them and opening a slot; `HLE_TOWER_GIVE` for each duo in a fight |
| 3. The map | generation rules, the map screen, Fight/Elite/Rest nodes (Temper, Release), floor/circle bookkeeping | `HLE_TEST_TOWER_MAP`: 1,000 seeded maps checked against the rules (self-check); a scripted path through a circle with shots of the map |
| 4. Gold, shop, events | economy, shop menu (levels, Release), 8 events | `HLE_TEST_TOWER_SHOP`, `HLE_EVENT_FORCE=n` with shots of each screen and logged results |
| 5. Skill builds | the three level tracks and six Lv 5 forms (more crosses, Twin Moon's follow-up, Crescent Storm's 3 trailing blades, the stagger forms); Skyfall; the stagger-bonus cap | `HLE_SKILL_LV=<skill>,<lv>,<form>` with videos per level; `HLE_TEST_SKILL_DMG` + `HLE_SDMG_TOWER` for damage a cast against the tables; `[hit]` logs proving every cross, slash and blade lands where it's drawn; Ares and Delta frame time for Crescent Storm |
| 6. Elite affixes + balance | 6 affixes on the Bonus bosses; the cost measurements in section 13 and the payoff tables in section 14 | `HLE_ELITE_FORCE=affix` + `HLE_TEST_DUMMY`; `HLE_RUN_CLIMB` with `HLE_RUN_PATH=safe/normal/greedy`; Ares at 4 MB for heap and frame time |

Each stage closes with:
- the existing `HLE_TEST_TOWER`, `HLE_TEST_TOWER_RUN`, `HLE_TEST_TOWER_UI`, `HLE_TEST_TOWER_STATS` and `HLE_TEST_CYCLE` runs;
- a Delta-core run, which halts on NaN compares.

## 17. Decisions
The user's answers (2026-11-01):
1. Potions: as today.
2. Gold: lost at death.
3. Smouldering: reuses Shadowborne's floor fire.
4. Map icons: icons (my call, section 14).
5. New boons: changed where the game-theory check said so (section 14).
6. Unlocks: yes, lots, through the Shrine (section 14b).
7. Run seed: shown at the end of a run.
8. Bigger ROMs: accepted.
9. Release: at both a Rest and a Shop (section 14).
10. Skill forms: Crescent Storm is the 5 blades plus 3 trailing (the user's design); Shattering Dance anchors a stagger build; skill cards per section 14.

Open questions: none. The payoff tables are estimates; stages 5-6 measure them, and any number that moves out of its band comes back to the user.
