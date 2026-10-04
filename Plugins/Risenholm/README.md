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
| `NWNX_RISENHOLM_STACK_ITEM_ABILITIES` | true/false | Makes every ability bonus on one item count, and every ability penalty. The engine counts only the largest of an item's effects on each ability, so +5 and +1 Strength on one item gave +5; with this on it gives +6. Spell effects keep the stock rule (only the largest of each spell's counts), and the totals are still capped by `SetAbilityBonusLimit` and `SetAbilityPenaltyLimit`. Replaces the Ability branch of `CNWSCreature::GetTotalEffectBonus`; every other bonus type is untouched. |
| `NWNX_RISENHOLM_LOG_NUI` | true/false | Logs every NUI window the server sends a client (`NUI create:`) and every group layout it replaces (`NUI layout:`), with the character, account, window id, token, and calling script. A window with malformed JSON fails only on the client, as an error box reading `json.exception.type_error...`, and the server logs nothing, so these lines are the way to tell which window a player hit. Hooks `CNWSMessage::SendServerToPlayerNui_Create` and `_SetLayout` by symbol. |
| `NWNX_RISENHOLM_RPC_PORT` | integer | Port for the RPC listener (see below). Unset or 0 (default): no listener. |
| `NWNX_RISENHOLM_RPC_SECRET` | string | Shared secret every RPC request must carry, at least 16 characters (`openssl rand -hex 32`). Required: with a port and no secret, or a short one, the listener refuses to start. |
| `NWNX_RISENHOLM_RPC_BIND` | address | Address the RPC listener binds. Default `127.0.0.1`. In Docker set `0.0.0.0` so another container can reach it, and do not publish the port. |
| `NWNX_RISENHOLM_RPC_SCRIPT_PREFIX` | string | What every script the RPC listener may run is named with. Default `pw_rpc_`. Must not be empty. |

Each switch is logged at plugin load. Measured together on the dev module (2026-09-21, idle): objects visited per frame 53,835 -> 7,579, `AIMasterUpdateState` 367 -> 240 ms per second. Call `NWNX_Risenholm_TrimAILists()` once from OnModuleLoad to sweep objects created by routes the hooks do not see.

## Local Variables

| Variable Name | VariableType | ObjectType | Values |
| -----------| ------------- | ------------- | ------ |
| `FLAT_FOOTED_STATE` | int | Creature | 1 = Always FlatFooted, 2 = Never FlatFooted |
| `SNEAK_ATTACK_IMMUNE` | int | Placeable | 1 = Immune to SneakAttacks |
| `DISABLE_COMBAT_SHUFFLE` | int | Creature | 1 = Disable Combat Shuffle |
| `LOADOUT_DORMANT` | int | Item | 1 = Dormant: none of its properties reach its wearer (see below) |
| `NO_BUDGE` | int | Creature | 1 = Never moved by the engine's door and placeable budge (see below) |

### Dormant equipment

Hooks on `CNWSItemPropertyHandler::OnItemPropertyApplied` and `OnItemPropertyRemoved`, at
`Order::Early` so they sit outside NWNX_Events' hooks on the same functions. While an item carries
the `LOADOUT_DORMANT` local, both do nothing for its permanent properties: those never reach the
wearer, whether on equip, at login, or when one is added to it while worn, and skipping the removal
in step keeps Bonus Spell Slot and Unlimited Ammunition balanced. Temporary properties (an oil,
Magic Vestment, anything cast on the item) apply as normal. Because the Events hooks are skipped too,
`NWNX_ON_ITEMPROPERTY_EFFECT_*` does not fire for a dormant item's permanent properties.
`AddItemCastSpellActions` and `UseItemInstant` refuse a worn dormant item's Cast Spell properties.

The engine runs the module's OnUnequip script before `CNWSItem::RemoveItemProperties`
(`CNWSCreature::UnequipItem`, 8193.37), so the module must not clear the local from inside
OnUnequip, or the removal would take off properties that were never applied.

The item itself is untouched, so anything reading its properties still sees them all. The module
decides what is dormant: see `pw_inc_loadout.nss`, which sets the local before the engine applies
anything and clears it only after the engine's unequip.

`NWNX_Risenholm_ApplyItemProperties(oCreature, oItem)` wakes an item that is already worn, once its
local has been cleared: the per-property loop of `CNWSItem::ApplyItemProperties`, over permanent
properties only (the temporary ones were never held back, so applying them again would stack them),
then `ComputeArmourClass` and `UpdateCombatInformation`, the steps `CNWSCreature::EquipItem` takes
around putting an item in its slot.

### Equipped weight

A hook on `CNWSCreature::UpdateEncumbranceState` first rebuilds `m_nEquippedWeight` from the
equipment slots (`ComputeTotalEquippedWeight`). The engine keeps that field as a running total,
added to by `EquipItem` and subtracted from by `UnequipItem`, and never recomputes it, so a worn
item whose weight changes leaves the total wrong by the difference. The module changes worn
items' weight routinely: Magic Vestment's weight reduction and the mirrored Base Item Weight
Reduction on an off-hand both go on from OnPlayerEquipItem, which the engine queues to run after
the weight was added, and come off from OnPlayerUnEquipItem, whose `RemoveItemProperty` is
queued to run after the weight was subtracted. Each swap leaked the difference, and a character's
weight crept up (8193.37, 2026-09-29). The engine calls `UpdateEncumbranceState` after every
equip, unequip, pack change, and weight-property change, and it is the only writer of the total
the client is sent, so rebuilding there keeps the figure right and heals a leak already carried.

### Thrown weapons

Darts, shuriken, and throwing axes are never used up, and they fire every attack of the round.
The plugin replaces `CNWSCreature::ResolveAmmunition`, which only ever consumes arrows, bolts, and
bullets, and hooks `CNWSCreature::GetAmmunitionAvailable` to answer with the attacks asked for
whenever the main hand holds one of the three thrown base items. Without the second hook the engine
answers with the equipped stack size, and a weapon that does not stack fires one attack per attack
action, three a round. The Unlimited Ammunition property would do the same, but the engine refuses
it on a thrown weapon (`itemprops.2da` row 61 has `****` in the `2_Thrown` column, and
`CNWSEffectListHandler::OnApplyItemProperty` drops what that table forbids), so no property is
involved. Launchers keep the engine's own answer.

### Item uses

A hook on `CNWSCreature::AIActionItemCastSpell` makes an item use cast once and keeps a charged
item from being used up. The engine casts, and consumes, on every call of that action that finds
the creature's `m_bLastSpellCast` clear once the conjure time is up, and checks the uses left only
before it. `NWNX_Creature_AddCastSpellActions` clears that flag for an instant cast added to the
front of the queue, and an instant cast that fails never sets it again, so an item use still in
its tail fired a second time: a free cast, a second consumption, and for a charged item left at 0
charges the item destroyed (`NWNX_TWEAKS_PRESERVE_DEPLETED_ITEMS` only covers a call entered with
1 to 5 charges). A ring was lost this way on production (8193.37, 2026-10-01). The hook remembers
a use that has cast and sets the flag again if that use comes round with it clear, and holds the
item plot for any call that uses a charges-per-use property. Read from the disassembly and
reproduced on the dev server with an NPC: an instant cast queued in front of a ring use, at a
target destroyed before it ran, cast the ring twice and destroyed it without the hook, and once,
ring kept, with it.

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

### RefreshBodyParts

`NWNX_Risenholm_RefreshBodyParts(oCreature)` reloads a creature's body parts on every client that
tracks it, without a visible swap, and the plugin calls the same thing itself whenever a
texture-replacing VFX (visualeffects.2da `ProgFX_Duration` pointing at a progfx.2da row of Type 1:
Stoneskin, Greater Stoneskin, Petrify, the jewel and bone skins, ShadowSkin, IceSkin) is removed
from a part-based creature.

The bug it covers is in the client. When such an effect ends, `CNWCAnimBaseParts::RestoreTexture`
re-tints every part with ONE stored set of ten palette colours, the set the last part loaded wrote,
so armour coloured per part (`ITEM_APPR_TYPE_ARMOR_COLOR` indices 6-119, the tailor's per-part
mode) comes back wearing one part's colours everywhere, usually the whole-piece defaults.
Whole-piece colours are the same on every part and survive. Re-sending the appearance does not
help: the client only reloads a part whose variation differs from the one it has, which is also
why a player's own re-equip (parts go naked, then come back) does. So the hook on
`CNWSMessage::WriteGameObjUpdate_UpdateObject` appends two extra appearance blocks to the first
update after the removal, the one already carrying the VFX delete: the chest item gone and every
part as 0, then the chest item back and the real values, which is what a real re-equip sends over
two ticks (the chest-delete branch wipes the client's per-part colours and the chest-add branch
re-reads them from the item; parts alone re-tint only some of them). The client processes a whole
message before it renders, so nothing flickers.

### UseItemInstant

`NWNX_Risenholm_UseItemInstant(oCreature, oItem, oTarget)` uses a single-use Cast Spell property
at once, with no action, animation, or conjure time: the item counterpart of
`NWNX_Creature_AddCastSpellActions`' `bInstant`, which `AddItemCastSpellActions` has no equivalent
of. It performs the completion half of `CNWSCreature::AIActionItemCastSpell` (8193.37, 0x499f60)
directly: the same creature fields, then `SpellCastAndImpact` with the item id, so
`NWNX_ON_CAST_SPELL` sees `ITEM_OBJECT_ID` and the spell script sees `GetSpellCastItem` and the
item's `iprp_spells` `CasterLvl`. Consumption follows afterwards, as the engine's does: one off the
stack, or for the last one the property spent and the item destroyed 500ms after the projectile time
(kept if plot). A free use that refunds from inside the cast event therefore nets out.

Refuses while a spell or item cast is at the head of the action queue, since both keep their state
in the fields this writes. The impact is queued and reads the item caster level off the creature
when it lands, so several uses need spacing a tick apart -- the module's QuickUse Autocast
(`pw_inc_quickuse.nss`) chains them 0.1s apart.

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

### Unpossess refusal guard

Not an export: a hook on `CNWSCreature::UnpossessFamiliar`. The engine (8193.37) refuses to
unpossess, changing nothing, when the possessor or its familiar has no area, e.g. while the
possessed creature is mid-jump and its client loads the destination. `NWNX_Player`'s own hook on the
same function cuts the familiar link afterwards regardless, which left a player driving a creature
that was no longer their familiar and could never be unpossessed (production, 2026-09-27). The hook
applies the engine's own test first and, when it would refuse, returns before the engine and
`NWNX_Player` run, keeping the link so a later unpossess can succeed. It is here rather than in
`Plugins/Player` so that an upstream merge cannot lose it.

### TURD crossover guard

Not an export: a hook on `CNWSPlayer::DropTURD`. The engine (8193.37) makes a TURD for a leaving
player only when the creature has an area, or a desired area that still exists, and a fresh login
has neither until the client asks for the module, which is after OnClientEnter. So a player booted
in OnClientEnter (a ban, say), or who drops in that window, leaves no TURD. NWNXLib's own
`POS.cpp` hook on the same function copies the leaver's NWNX_Object variables onto the head of the
TURD list regardless, which is then the TURD of whoever left last, and `EatTURD` hands them to that
character at their next login (production, 2026-09-28: a banned player's retries landed her
variables on two other characters, and the module's PC_UUID check locked one of them out until a
restart). The hook applies the engine's own test first and, when no TURD will be made, hides the
list head from the POS hook for the duration of the call, logging a warning naming the player. It
is here rather than in `NWNXLib/POS.cpp` so that an upstream merge cannot lose it.

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

### SetDamageBonusLimit

`NWNX_Risenholm_SetDamageBonusLimit(nLimit)` sets the damage bonus limit for the running module past
the 255 that the NWScript `SetDamageBonusLimit` allows. The engine clamps the physical damage bonus
from effects, summed across all of them, to this limit in `CNWSCreatureStats::GetDamageRoll`
(`GetTotalEffectBonus` with `bElementalDamage` off returns `min(increases, limit) - min(decreases,
limit)`); elemental types go through `ResolveElementalDamage` and are never clamped. The script
command runs its value through the `DamageBonusLimit` server setting's 0-255 constraint; this calls
`CServerExoApp::SetDamageBonusLimit` with `isModuleOverride` directly, which writes the same override
without it. `GetDamageRoll` carries the total in a short, so keep it under 32767. `pw_mod_load.nss`
calls it, because the threat system's Elite Solo creatures carry physical bonuses near 300.

### Item bonus spell slots only for reachable spell levels

The engine gates ability bonus slots but not item ones. `GetSpellGainWithBonus` returns 0 for
any spell level whose column in the class's spell gain table is `****`, so a high WIS never opens
a level the class cannot cast yet. A Bonus Spell Slot item property instead bumps a per-class
counter that four consumers add flat on top of that gated value with no check
(`UpdateNumberMemorizedSpellSlots`, `ModifyNumberBonusSpells`, `AdjustSpellUsesPerDay`,
`ResetSpellsPerDayLeft`), so a level 3 cleric wearing a Cleric 4 slot item got a level 4 slot,
prepared a spell in it, and cast it (8193.37, 2026-09-30). Hooks on those four clamp the slot
count, and the spontaneous casters' per-day counts, to 0 for any level above 0 whose
`GetSpellGainWithBonus` is 0. That test is exact for this module because every spell gain table
it ships has a positive base count in every reachable column. The item counter is left alone, so
the slot appears on its own at the level-up that makes the level reachable. Spells prepared in a
slot that goes away are dropped with it. The client shows whatever count the server sends, so it
needs nothing.

### Budge exemption

`CNWSArea::BudgeCreatures` runs on every door open and close (`CNWSDoor::SetOpenState`) and
whenever a placeable is added to an area (`CNWSPlaceable::AddToArea`). Any creature within 5m of
the object's bounding box whose own position fails `TestSafeLocationPoint` is moved with
`SetPosition` to a `ComputeSafeLocation` result up to 10m from the object. That is meant for a
creature caught in a closing doorway, but it also catches a creature the module has stood on
unwalkable ground on purpose: a prisoner in the Stocks stands on the placeable's origin, inside
its walkmesh, and was thrown out each time the door beside the stocks was used (8193.37,
2026-09-30).

A hook on `BudgeCreatures` leaves a creature carrying the `NO_BUDGE` local where it is. The
engine's loop has no test to skip a creature by, so for the length of the call each exempt
creature inside the box has `m_vPosition.y` set far outside it, then put back. It is a write to
the field, not `SetPosition`, so no trigger, area of effect, or client sees it, and the area's
object list is sorted by X. Every other creature is budged as before. The hook's scan uses the
engine's own start index and stop, so it adds one pass of the same length as the original's.

The module sets the local for as long as it holds a creature in place and must clear it on
release: see `SeatInStocks` and `ReleaseFromStocks` in `pw_inc_stocks.nss`.

### No size limits on equipping

The engine refuses a weapon more than one size category above the creature ("You are too small to
equip that weapon") or more than two below it, and refuses a tower shield to a Tiny or Small
creature. Risenholm does not use size categories, so hooks on `CNWSCreature::CanEquipWeapon`,
`CanEquipShield`, and `CanUseItem` lift all three. The first and last move `m_nCreatureSize` for
the length of the call just far enough to pass, then put it back. `CanEquipShield` checks the
right-hand weapon against the creature's size after its tower shield test, so there the shield is
passed off as a large shield for the call and the size is left alone.

An oversized weapon is passed as two-handed, so it takes both hands. Outside the hooks the engine
sees the real size difference, which for a Small creature holding a Large weapon is 2: the off
hand stays blocked and gets no attacks, but the engine's 1.5x Strength damage, which tests for a
difference of exactly 1, does not apply. On the module side, `GetWieldsTwoHandedMeleeWeapon` in
`pw_inc_itemslot.nss` counts it as two-handed.

### Player characters are always Medium

`CNWSCreature::UpdateAppearanceDependantInfo` sets the creature's size from the `SIZECATEGORY`
of its appearance. It runs at every login, from `SetCreatureAppearanceType`, and from `Polymorph`
and `UnPolymorph`, so a PC given a Large appearance became a Large creature, a greatsword was
one-handed to them, and they could dual-wield a pair. A hook puts every player character
(`CNWSCreatureStats::m_bIsPC`) back to Medium after each call. This covers everything size
affects, not only weapons: the size modifiers to AC, attack, and Hide, the knockdown size checks,
unarmed damage, and `GetCreatureSize`. A creature a player only possesses keeps its own size.
`NWNX_Creature_SetSize` still works on a PC until its next appearance change.

### A split stack keeps its local variables

When a player splits a stack, `CNWSItem::SplitItem` makes the new stack with
`CopyItem(this, FALSE)`, and that `FALSE` means no local variables are copied. The module keeps
item behaviour in locals, so a meal split off a stack lost `IS_MEAL` and `BEFORE_USE_SCRIPT` and
was eaten as a snack with no cooldown, and a split snack lost `FOOD_SCRIPT`. A hook copies the
original's locals onto the new stack before it reaches the inventory. The half left behind is the
original object and always kept them.

### RPC listener

A small HTTP listener inside the server process, so that something outside the game (Wellkeeper)
can ask it a question and get the answer back in the same request, instead of writing a row to
MySQL for a script to find on its next poll. Off unless `NWNX_RISENHOLM_RPC_PORT` is set.

    POST /rpc/<name>
    Authorization: Bearer <NWNX_RISENHOLM_RPC_SECRET>

The path names the script: `/rpc/teleport` runs `pw_rpc_teleport`. The module keeps one script for
each thing the outside world may ask of it, all named with `NWNX_RISENHOLM_RPC_SCRIPT_PREFIX`
(`pw_rpc_` by default), and the listener puts `<name>` after that prefix and runs nothing else. So
holding the secret is leave to call the scripts the module wrote to be called this way, not to run
any script it has. A name is lower-case letters, digits, and underscores, short enough for the
whole to be a 16-character resref.

The request's body is handed to that script on the main thread at the top
of the next server frame: the listener's worker thread queues it with `Tasks::QueueOnMainThread`,
which Core drains in its `MainLoop` hook, and blocks until the script has run. The script runs with
the module as `OBJECT_SELF`, reads the body with `NWNX_Risenholm_GetRpcRequest()`, and answers with
`NWNX_Risenholm_SetRpcResponse(sJson)`, which is sent back as `application/json`. Measured on the
dev server (2026-10-02), a round trip is 3 to 8 ms.

| Status | Meaning |
| :----: | ------- |
| 200 | The script's answer. |
| 401 | Wrong or missing secret. |
| 404 | No script of that name, or a name that could not be one. |
| 500 | The script ran and set no answer. |
| 503 | No module is loaded yet. |
| 504 | The main thread did not reach the request within 5 seconds. |

A request that times out is marked abandoned, and is not run when the main thread does reach it:
an action nobody is waiting for any more must not land late. The mark is checked as the request's
turn comes, so a script already running when the wait ends still finishes.

The worker threads never touch the engine; they parse HTTP, queue the request, and wait. There are
four of them, and keep-alive is off so that an idle connection cannot hold one.

#### Security

It is plain HTTP with a shared secret, meant for a loopback or a private container network on one
host. Nothing is encrypted, the secret included, so **never publish the port** and never run it
across a network you do not control. It binds `127.0.0.1` unless `NWNX_RISENHOLM_RPC_BIND` says
otherwise.

- The secret is compared in constant time, and checked in a pre-routing handler, which runs once the
  headers are in and **before any of the body is read**. A sender without it costs the server its
  headers and nothing more.
- A request is at most 100 header lines of 8 KiB and a 64 KiB body, chunked or not.
- A rejected request is logged with its sender's address, at most once every ten seconds, with a
  count of the ones in between.
- The secret is never logged.

These were found by attacking the first version on the dev server (2026-10-02). It checked the
secret in the route handler, after the library had read the body, and it used cpp-httplib 0.16.2,
which took a 64 MiB chunked body from an unauthenticated sender straight past the payload limit and
200,000 header lines likewise, the process growing to match. `External/httplib.h` is 0.57.1 for
that reason; do not swap it back for the 0.16.2 copies in `Plugins/HTTPClient` or `Plugins/WebHook`.
The same attack now changes the process's memory by nothing.

Two things it does not defend against, both by design:

- **Anything that can connect can deny the listener.** A connection that says nothing holds a worker
  until its two-second read timeout, and there are four. At most 32 connections wait behind them;
  more are closed at once. This costs the game nothing, since no worker reaches the main thread
  before its request has authenticated, but RPC callers time out while it lasts.
- **Anything with the secret can spend main-thread time**, since every request runs a script. Give
  the secret only to the service that needs it.

Send the body as ASCII, with anything else `\u`-escaped, and answer with `JsonDump`, which is
ASCII too. Checked on 8193.37 (2026-10-02): an escaped `ë` reaches the script as the one byte
0xEB and goes back out as `ë`, but the same character sent as raw UTF-8 is read a byte at a
time and arrives as two characters. Anything outside Latin-1, a curly quote included, reaches the
script as `?`.
