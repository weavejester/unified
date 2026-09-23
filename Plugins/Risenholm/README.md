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

### CreateAfterimage

`NWNX_Risenholm_PrepareAfterimage(oCreature)` serialises a creature once for a batch of clones and `NWNX_Risenholm_ReleaseAfterimage()` drops that snapshot; `NWNX_Risenholm_CreateAfterimage(oCreature, lLocation, nFaction, fAnimationSpeed)` clones a creature natively for the afterimage attacks: object state (effects, action queue, combat state) and equipment are copied, the backpack and local variables are not, and the clone comes back plot, unusable, unlootable, non-PC, at full hit points, in engine faction `nFaction`, tagged with the `IS_SET_PIECE` and `IS_VFX` locals, with `VFX_DUR_INVISIBILITY`, a permanent 100% miss chance, a permanent cutscene ghost, and animation speed `fAnimationSpeed` already applied. Replaces the `ObjectToJson`/`JsonToObject` path, which serialised the whole inventory only to discard it.
### SetAlwaysWalk

`NWNX_Risenholm_SetAlwaysWalk(oCreature, bWalk)` replaces `NWNX_Player_SetAlwaysWalk`, which is
broken. Both write `CNWSCreature::m_bForcedWalk`, and so does the engine, from
`CNWSEffectListHandler::OnApplyLimitMovementSpeed` / `OnRemoveLimitMovementSpeed` on effect
true-type 59 -- the one effect it uses to express stealth mode, Slow and encumbrance alike. Turning
the override off therefore has to check whether one of those still holds the flag, and upstream's
check is a `std::bsearch` whose comparator dereferences the list's `CGameEffect*` slots as if they
were effects, so it has never matched. The symptom was a player toggling Force Walk off while
sneaking and being able to run while hidden.

Use this, not the Player version, and do not "fix" `Plugins/Player` instead -- that is upstream's
file and the next `canon` merge would take the fix with it.

State is per-session, matching the engine flag; the module keeps the player's preference in the PC
local `IS_FORCE_WALK_ON` and re-pushes it from `pw_mod_enter.nss` on login.

### RefreshPlayerListEntry

`NWNX_Risenholm_RefreshPlayerListEntry(oCreature)` re-sends the player-list entry of the player
driving or owning `oCreature` to every client. Each client matches a chat line to a player through
the object id in that entry, and the engine only sends it when a player enters the module, so once
a player possesses something every line they send carries an object id no client recognises. The
clickable reply portrait is then never built, and a tell from a Scar Intruder cannot be answered by
clicking it. The client updates the existing row in place, and the name shown does not change.
`pw_inc_invader.nss` calls it straight after possessing the Intruder and `pw_mod_unpossesa.nss`
after every unpossession, which points the entry back at the PC body.
