@page risenholm Readme
@ingroup risenholm

## Environment Variables

| Variable Name | Value | Notes |
| ------------- | :----: | ----- |
| `NWNX_RISENHOLM_TRIM_AI_STATIC_PLACEABLES` | true/false | Keeps static placeables out of the engine's per-frame AI update lists. They can never queue an action or run a script; they are put back if an effect is applied to one. |
| `NWNX_RISENHOLM_TRIM_AI_IDLE_PLACEABLES` | true/false | Same for non-static placeables with no heartbeat script that are not die-when-empty and have no inventory (the test `CNWSPlaceable::AIUpdate` itself uses to do nothing). Put back when an effect or action lands. A heartbeat assigned later with `SetEventScript` will not run until then. |
| `NWNX_RISENHOLM_TRIM_AI_ITEMS` | true/false | Keeps items with no applied effects out of the AI update lists from creation; put back when an effect is applied. |
| `NWNX_RISENHOLM_TRIM_AI_IDLE_DOORS` | true/false | Same idle test for doors (heartbeat slot empty, no actions, no effects). Opening, closing, locking and bashing all put a door back. |
| `NWNX_RISENHOLM_TRIM_AI_IDLE_TRIGGERS` | true/false | Triggers with no heartbeat script and no actions leave the lists. Enter and exit detection is unaffected; it is done by the moving creature. |
| `NWNX_RISENHOLM_ITEM_AI_DIVISOR` | integer | 0 (default) leaves item updates alone. N runs `CNWSItem::AIUpdate` (an effect-list walk for items carrying effects) every Nth frame, staggered by object id; an expiring effect on an item runs late by at most N frames. 10 is a reasonable value. |
| `NWNX_RISENHOLM_IDLE_CREATURE_AI_DIVISOR` | integer | 0 (default) leaves creature AI alone. N runs `CNWSCreature::AIUpdate` only every Nth frame, staggered by object id, for creatures that are at VERY_LOW, not in combat, with no queued actions, in an area with no players. Timers catch up on the next visit; effects and heartbeats in empty areas run late by at most N frames. 20 is a reasonable value. |

Each switch is logged at plugin load. Measured together on the dev module (2026-09-21, idle): objects visited per frame 53,835 -> 7,579, `AIMasterUpdateState` 367 -> 240 ms per second. Call `NWNX_Risenholm_TrimAILists()` once from OnModuleLoad to sweep objects created by routes the hooks do not see.

## Local Variables

| Variable Name | VariableType | ObjectType | Values |
| -----------| ------------- | ------------- | ------ |
| `FLAT_FOOTED_STATE` | int | Creature | 1 = Always FlatFooted, 2 = Never FlatFooted |
| `SNEAK_ATTACK_IMMUNE` | int | Placeable | 1 = Immune to SneakAttacks |
| `DISABLE_COMBAT_SHUFFLE` | int | Creature | 1 = Disable Combat Shuffle |
