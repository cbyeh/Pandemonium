# Pandemonium: ideas for what's next

The Tower MVP and the Shrine shipped in v0.72-fork.8. Weapons, curses, seeds, elite floors, practice mode and the King are still ideas.

Work happens on the `tower` branch. Each section lists what it is, how it fits the code we have, and rough size: **S** is under a day, **M** is a few days, **L** is a week or more.

## Constraints to design around
- **RAM: 4 MB, no Expansion Pak.** No new big models. New visuals should be procedural, like the skins and the Shackled Sun (`src/fx/proc_mesh.c`), or reuse existing assets.
- **Save space: EEPROM 4K, 3 slots.** Each `SaveData` slot has 21 spare bytes left (`_pad[7..27]`). That's enough for a currency, upgrade levels, unlocks and records without changing the save format.
- **One arena.** Every fight takes place in the Guardian hall (`src/scenes/guardian/guardian_scene.c`). Bonus fights already swap the boss in place through `BonusDef` begin/skip functions, which is the base a boss rush needs.
- **Balance tooling exists.** `HLE_TEST_DUMMY` measures a boss's damage per 30 s against a passive dummy (the Guardian deals 915 per 120 s), and `HLE_DUMMY_HALF` measures the second half. Use them to tune anything new.

## 1. The Tower: a boss-rush roguelite (M, building first)
A run climbs floors, and each floor is one fight:
- **A circle is five floors:** the Guardian first, then the Fallen Warrior, the Shackled Sun and Shadowborn in a seeded random order, and Guardian Unbound last, as a floor of its own (no transformation cutscene).
- **The climb starts well below the story fights:** in circle 1 the bosses have 70% of their health and deal 60% of their damage. Each circle adds 30% health and 20% damage, so circle 3 is at full damage and beyond.
- **A run starts like the original game:** 3 potions and the sword only. The D-pad skills are learned during the climb, from skill cards in the boon draft.

Boss health in a run starts from each boss's skills-off health:
- The Guardian and Unbound keep theirs, so floor 1 plays like the original jam game.
- The Bonus bosses start at half, since their health was sized for 7 potions and the skills.
- Each skill learned in the run adds a third, so all three learned doubles it, the same as the Skills setting does.

Between floors:
- The player heals a third and gets a potion back, up to the run's maximum.
- The player drafts **one of three Boons** (run-only perks, below).
- Each floor cleared earns **Embers**, a persistent currency.

When the player dies, the run ends. Embers are kept, and the best floor is recorded per save slot.

**Hooks:**
- A boss HP and damage scale: `boss_target_max_health` and `character_apply_damage`.
- Player-side multipliers read from a "run modifiers" struct.
- A draft screen as a scene state.
- Title menu entries **Tower** and **Shrine**.

### Boons (run-only)
| Boon | Effect |
|---|---|
| Ember Edge | +15% damage |
| Iron Skin | -15% damage taken |
| Deep Flask | +1 potion (and +1 max) |
| Kindled Flask | potions heal +15% |
| Fleetfoot | rolls cost 40% less stamina |
| Second Wind | +40% stamina regen |
| Moon Mastery | skill cooldowns -20% (offered once a skill is unlocked) |
| Bloodthirst | heal 6% of damage dealt |
| Vigor | +25 max HP |
| Last Stand | +40% damage below 30% HP |
| Unbroken | once per floor, a lethal hit leaves you at 1 HP |
| Crescent Echo | Moon Blade Dance casts 2 more waves (offered once it is unlocked) |

Boons stack. Duplicates of a stat boon add up, and unique boons leave the pool once taken.

## 2. The Shrine: permanent upgrades (S to M, building second)
Spend Embers on the title screen. Upgrades apply only inside Tower runs.

Skills aren't sold here. They're learned in the climb: one of the three boon cards after each boss is always a skill card (learn Moon Blade Dance, Halfmoon Slash or Crescent Cleave, or upgrade one learned this run). In a run, only learned skills work; the Skills setting is ignored.

| Upgrade | Levels | Per level |
|---|---|---|
| Flask Capacity | 3 | +1 potion |
| Flask Potency | 3 | +8% heal |
| Honed Blade | 3 | +8% damage |
| Vigor | 3 | +10 max HP |
| Skill Mastery | 3 | -8% skill cooldowns |

Costs rise per level (for example 30 / 60 / 100). Save layout: Embers as u16, levels at 3 bits each.

## 3. Weapons (M)
Bought at the Shrine, equipped there, and shown in hand through the bone-attached accessory system the skins already use. The Dragonslayer slab exists already.
- **Greatsword:** +45% damage, swings 15% slower, more reach. Reuses the Black Swordsman's slab.
- **Moonlight Greatsword:** a strong attack also releases a crescent wave (the skill_fx wave), and the blade glows.
- **Twin Fangs (stretch):** faster, weaker swings that build skill cooldowns.

Needs a per-weapon stats table: damage, animation speed and the hit capsule length (`SWORD_LENGTH`).

## 4. More roguelike depth (M)
- **Curses:** an optional harder Boon draft with more Embers (bosses heal when you drink, no lock-on, enemy waves are faster).
- **Seeds:** a run seed shown on the results screen, so a run can be shared or retried.
- **Elite floors:** remixed bosses, such as a Shackled Sun that starts unshackled or a twin Fallen Knight (two instances).
- **Relic rewards:** after circle 1, a relic slot with a strong unique effect, such as Moon Blade Dance on roll.

## 5. New bosses and modes (L)
- **The King** (from the ending): a throne-bound caster fight in the same hall, built procedurally.
- **Practice mode:** pick any boss and phase, with infinite potions and the hit timings shown.
- **Time attack:** a best time per boss, which `save_controller_record_boss_clear_time_ms` already records for the story fight.

## Order of work
1. Tower MVP: floors, circles, ascension scaling, Boon draft, Embers, results.
2. Shrine with the five upgrades, saved per slot.
3. Weapons: Greatsword and Moonlight Greatsword.
4. Curses, seeds and elite floors.
5. Practice mode, then the King.

## Decisions (2026-10-21)
- **Shrine upgrades apply only in the Tower.** The story and Bonus fights stay as tuned, so `tower_mod_*` returns 1.0 (or 0) whenever no run is active.
- **Runs are endless circles**: 5 floors per circle, with Ascension scaling every circle after the first. The score is the best floor reached.
- **The Guardian and Guardian Unbound are two separate floors**, with no transformation cutscene between them. Unbound closes each circle.
- **Tower starts much easier than the story mode**, then catches up by circle 3.
- **Runs start with 3 potions and basic attacks only**; skills are learned from boon cards during the climb, and boss health grows with them.
- **Shadowborn** (Bonus 3) joins the circle; it may be a little harder than the others.
