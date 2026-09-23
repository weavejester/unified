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
| `NWNX_RISENHOLM_RUNSCRIPT_REMOVE_ON_DESTROY` | true/false | Runs the ON_REMOVED script of every `EffectRunScript` on a non-player creature when the creature is destroyed. The engine frees a destroyed object's effects without their removal handlers, so without this any cleanup done in ON_REMOVED silently never happens for DestroyObject, corpse decay, or the spawn sweep. The script runs just before deletion with a valid OBJECT_SELF; anything it queues on the creature itself is lost with it. Player characters are skipped, because their effects persist in the .bic and the engine does not run ON_APPLIED when reloading them. |

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

### Disconnect crash guard

Not an export: a hook on `CServerExoAppInternal::RemovePCFromWorld`. The engine (8193.37) looks up
the master of the creature a leaving player is driving and uses the result without a null check
whenever any other client is on the character-select screen, so a player who disconnects while
possessing a creature whose master no longer exists takes the whole server down (production,
2026-09-22). The hook clears such a dangling `m_oidMaster` first, which is the fallback the engine
itself takes a few instructions later, and logs a warning naming the player and the creature.

### Character save guard

Not an export: a hook on `CNWSPlayer::SaveServerCharacter`. The engine (8193.37) saves whichever
creature the client is driving, swapping in the master only for a possessed familiar or a DM
possession, and never checks that the result is a player character. A player left driving a
creature that has stopped being their familiar therefore gets that NPC written over their `.bic`
(production, 2026-09-23: a dead Intruder replaced tyrese's character, and its missing
`LvlStatList` then crashed the server on every later save of it). The hook resolves the save target
the same way and refuses, with a warning naming the player and the creature, to write anything
that is not a player character. The player keeps their last good file.

### Read-only inventories of other creatures

Not a switch: hooks on `CNWSMessage::HandlePlayerToServerInventoryMessage`, `...InputMessage`,
`...GroupInputMessage`, and `...StoreMessage`. A player's other-inventory panel (the one
`NWNX_Player_OpenInventory` opens on another creature, and the one a henchman's pack shows in) is
trusted by the engine for any owner: it will equip onto the owner, move the owner's items into any
repository including the player's own, split and merge their stacks, and sell them to a store for
the player's gold, with no DM check and no check that the owner is the player's associate. The
client can also point the panel at any object id itself (GuiInventory minor 1 is unchecked).

While a non-DM's panel shows anyone other than their own body, the creature they drive, or an
associate of either, a message on those handlers that names the owner or one of the owner's items
(bags included) is consumed before the engine reads it, and for inventory messages the matching
cancel is sent so the item drops back. Input and group input only refuse the owner's items, never
the owner, so attacking, casting on, or healing them still works, and using a container (opening a
bag in the panel) is let through. Store messages are refused outright while such a panel is open.
Viewing is unaffected. The module relies on this for `/search`.

An earlier version faked the panel closed for the length of each handler instead. It did not work:
with the owner blanked, `Unequip` queues the move on the searcher's own creature with the target's
item, and nothing on that path checks whose the item is (found in game, 2026-09-23).

`NWNX_Risenholm_GetOtherInventoryOwner(oPlayer)` returns whose inventory that panel is showing, or
`OBJECT_INVALID` when it is closed.

### Items concealed from one viewer

A hook on `CNWSMessage::WriteRepositoryUpdate`, plus three exports. The module's `/hideitem` lets a
character hide items from a `/search`, and a searcher who loses the roll-off for one must not be
sent it. Everything in the other-inventory panel -- the backpack (update list 1) and any bag
opened inside it (list 0) -- reaches the client through that function, which walks the
repository's item list and diffs it against what that viewer's panel was last sent. For the length
of one call for a viewer with registrations, the concealed items' list nodes are unlinked, so the
diff never adds them (and deletes any an earlier update sent), then relinked in reverse order: the
same nodes, with their own links untouched, so the list comes back exactly as it was.

Only lists 0 and 1, and only a repository belonging to the registered owner or to a bag that owner
carries. Barter (list 2) is never filtered, so nothing can be slipped into a trade unseen.

| Export | |
| --- | --- |
| `NWNX_Risenholm_ConcealItemFromViewer(oViewer, oOwner, oItem)` | Hide `oItem` from `oViewer`'s view of `oOwner`. A different owner replaces the previous registration. |
| `NWNX_Risenholm_ClearConcealedItems(oViewer)` | Drop everything registered for `oViewer`. |
| `NWNX_Risenholm_GetIsItemConcealedFrom(oViewer, oItem)` | Whether that registration exists. The module probes one before opening a search, so a build without concealment refuses the search rather than showing hidden items. |

### "[Hidden]" under a hidden item's name, for its holder only

Hooks on `CNWSMessage::AddActiveItemPropertiesToMessage` (every item as a panel lists it),
`SendServerToPlayerUpdateItemName` (SetName and `NWNX_Player_UpdateItemName`), and
`SendServerToPlayerExamineGui_ItemData`, the three places an item's name reaches a client. When the
viewer is the character carrying the item (bags included), the item's `SEARCH_HIDDEN_BY` local holds
that character's UUID, and it is not equipped, the name is sent with a second line reading
`[Hidden]`, the same shape as a Powered item's tooltip. The item's own name (`CNWSItem::m_sName`) is
swapped only for the length of the call, so everyone else -- a searcher, a barter partner -- gets the
real name. Unidentified items show their base name on the client whatever is sent, so they do not
show the line.
