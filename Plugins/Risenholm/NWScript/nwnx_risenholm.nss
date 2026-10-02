/// @addtogroup risenholm Risenholm
/// @brief Custom functions for the Scars of Risenholm PW server
/// @{
/// @file nwnx_risenholm.nss

const string NWNX_Risenholm = "NWNX_Risenholm"; ///< @private

/// @brief Signalled through the Events plugin after an item is moved within a
/// bag -- from one slot to another, or merged onto a stack there -- or carried
/// into one from its owner's inventory. Subscribe with NWNX_Events_SubscribeEvent.
/// A move within a bag fires no NWNX_ON_INVENTORY_* event, so this is the only
/// way to hear of it.
///
/// OBJECT_SELF is the bag. Event data ITEM is the item moved (StringToObject);
/// it is no longer valid if it was merged onto a stack.
const string NWNX_RISENHOLM_ON_REPOSITORY_MOVE_AFTER = "NWNX_RISENHOLM_ON_REPOSITORY_MOVE_AFTER";

/// @brief Signalled through the Events plugin when an item is dragged into a bag,
/// or from one slot of it to another, before the engine moves it. Same OBJECT_SELF
/// and ITEM as NWNX_RISENHOLM_ON_REPOSITORY_MOVE_AFTER. NWNX_Events_SkipEvent
/// refuses the move: the client is told it is cancelled and puts the item back
/// where it was dragged from, and nothing is lost. This is the safe way to keep
/// an item out of a bag; skipping NWNX_ON_INVENTORY_ADD_ITEM_BEFORE is not.
const string NWNX_RISENHOLM_ON_REPOSITORY_MOVE_BEFORE = "NWNX_RISENHOLM_ON_REPOSITORY_MOVE_BEFORE";

/// @brief Set a PC like/dislike status on the player list without changing their hostility.
/// @param oSourcePC The source PC.
/// @param oTargetPC The target PC.
/// @param bNewAttitude The new attitude, TRUE for like, FALSE for dislike.
/// @param bSetReciprocal True if the attitude change should be reciprocal
void NWNX_Risenholm_SetPCLikeStatus(object oSourcePC, object oTargetPC, int bNewAttitude, int bSetReciprocal=TRUE);

/// @brief Update Mage Armor Stats for a creature
/// @note Should be executed when setting/deleting the MAGE_ARMOR local int and when someone logs in.
/// @param oCreature The creature
void NWNX_Risenholm_ForceUpdateMageArmorStats(object oCreature);

/// @brief Force every connected client to be re-sent oCreature's full appearance.
/// @note Works around clients rendering a creature naked when its appearance id is swapped
/// back to a humanoid form (eg. leaving wildshape) if they entered the area while it was
/// transformed. Call it right after SetCreatureAppearanceType().
/// @note Safe to call in the same tick as SetCreatureTailType() or SetCreatureWingType(): every
/// field of each client's cached appearance is set to an impossible value, so nothing compares
/// equal and gets skipped. Older builds reset the cache to real values instead, which swallowed
/// a tail or wing cleared to NONE (0) in the same tick.
/// @param oCreature The creature whose appearance should be re-sent.
/// @param bFullObjectUpdate If TRUE, re-send the whole object rather than just the appearance
/// block. Heavier - only needed if an appearance-only update proves insufficient.
void NWNX_Risenholm_ForceAppearanceUpdate(object oCreature, int bFullObjectUpdate = FALSE);

/// @brief Reload oCreature's body parts on every client that sees it, with no visible swap.
/// @note Works around a client bug: when a texture-replacing VFX (Stoneskin, Petrify, the jewel
/// skins...) ends, the client re-tints every body part with ONE stored colour set, so armour with
/// per-part colours comes back wearing one part's colours everywhere. The plugin already does
/// this on its own whenever such an effect is removed; call it by hand only for a creature whose
/// parts got out of step some other way. A plain appearance re-send cannot fix it: the client
/// reloads only parts whose model changed, so this sends every part as empty and then as it is,
/// both in one message, which the client applies before it draws a frame.
/// @param oCreature The creature whose body parts should be reloaded.
void NWNX_Risenholm_RefreshBodyParts(object oCreature);

/// @brief Executes an external command in a child process and returns STDOUT as a string.
/// @note Use only when necessary, keep user-alterable data to a minimum, or ideally zero.
/// @param sCmd The path of the command to execute
/// @param sArg1 An optional 1st argument
/// @param sArg2 An optional 2nd argument
/// @param sArg3 An optional 3rd argument
/// @param sArg4 An optional 4th argument
/// @param sArg5 An optional 5th argument
/// @param sArg6 An optional 6th argument
string NWNX_Risenholm_ExecuteCommand(string sCmd, string sArg1="", string sArg2="", string sArg3="", string sArg4="", string sArg5="", string sArg6="");

/// @brief Checks for a file that indicates a shutdown should take place.
/// @note Deletes the file afterward.
/// @return True if the shutdown file was found, false otherwise
int NWNX_Risenholm_CheckForShutdownFile();

/// @brief Remove idle static placeables, idle non-static placeables, idle doors and
/// triggers, and effect-free items from the engine's
/// AI update lists, so CServerAIMaster::UpdateState stops visiting them every
/// frame. The plugin already does this for objects as they are created; this
/// sweeps anything that arrived by another route (CopyArea instances, objects
/// created before the plugin's hooks). Call once from OnModuleLoad.
/// @note No-op unless at least one NWNX_RISENHOLM_TRIM_AI_* switch is set in the
/// plugin environment; each sweeps only what its switch covers (see the plugin README).
/// @return The number of objects removed.
int NWNX_Risenholm_TrimAILists();

/// @brief TRUE if oObject carries an effect whose tag (TagEffect) is exactly sTag.
/// Native replacement for the NWScript effect-list walk in pw_inc_effect.
int NWNX_Risenholm_GetHasEffectByTag(object oObject, string sTag);

/// @brief TRUE if oObject carries an effect whose string parameter nIndex (0-5) is
/// exactly sValue, e.g. the RunScript script name of a primed effect.
int NWNX_Risenholm_GetHasEffectWithStringParam(object oObject, int nIndex, string sValue);

/// @brief Remove every effect on oObject whose tag is exactly sTag.
/// @return The number of effects removed.
int NWNX_Risenholm_RemoveEffectsByTag(object oObject, string sTag);

/// @brief TRUE if oCreature is a DM avatar the players can see, ie. one who has
/// pressed Appear on the DM client. A DM logs in unmanifested and Disappear puts
/// him back that way; while he is, the engine leaves him out of other creatures'
/// perception and out of the pathing line-of-sight test, so nothing in the world
/// reacts to where he is standing.
/// @note This is CNWSCreatureStats::m_bDMManifested read raw, so it means nothing
/// on anything but a DM avatar -- NWNX_Player_ToggleDM leaves it set on a player it
/// has toggled back out of DM. Gate on GetIsDM() first.
/// @param oCreature The DM avatar.
int NWNX_Risenholm_GetIsDMManifested(object oCreature);

/// @brief Fixes items that have become unuseable when their destruction is skipped in the NWNX_ON_ITEM_DESTROY_OBJECT_BEFORE event
/// @param oItem The item to fix
void NWNX_Risenholm_FixItemDestroySkipUseableState(object oItem);

/// @brief Use oItem's single-use Cast Spell property on oTarget at once: no
/// action, animation, or conjure time. The item counterpart of
/// NWNX_Creature_AddCastSpellActions' bInstant.
///
/// Otherwise indistinguishable from an ordinary use -- NWNX_ON_CAST_SPELL
/// fires with the item, GetSpellCastItem returns it, the caster level is its
/// iprp_spells CasterLvl, and one is consumed afterwards the way the engine
/// consumes it (so a free use that refunds from the cast event nets out).
///
/// The spell's impact is queued, and reads the ITEM caster level from
/// oCreature when it lands. Space several uses at least a tick apart (a
/// DelayCommand of 0.1 is plenty for a self-target) or they share the last
/// one's caster level.
/// @param oCreature The user; must carry oItem, loose or in a bag.
/// @param oItem The item. Charges and uses/day are not supported.
/// @param oTarget The target, in oCreature's area.
/// @return TRUE if the spell was cast. FALSE, with nothing changed, if the
/// item has no usable single-use Cast Spell property, oCreature cannot use
/// it, or oCreature is in the middle of casting a spell or using an item.
int NWNX_Risenholm_UseItemInstant(object oCreature, object oItem, object oTarget);

/// @brief Apply the properties of an item oCreature is already wearing, as equipping it would.
/// @note For waking a dormant item (pw_inc_loadout.nss): clear its LOADOUT_DORMANT local first.
/// While that local is set the plugin keeps every property of the item off its wearer, on
/// equip, at login, and when a property is added to it, and this refuses too.
/// @param oCreature The wearer.
/// @param oItem The item, equipped on oCreature.
/// @return TRUE if the properties were applied. FALSE if oItem is not worn by oCreature or is
/// still marked dormant.
int NWNX_Risenholm_ApplyItemProperties(object oCreature, object oItem);

/// @brief Perform a free attack on oTarget from oCreature
/// @param oCreature The source of the attack
/// @param oTarget The target of the attack
void NWNX_Risenholm_AddAttackOfOpportunity(object oCreature, object oTarget);

/// @brief Force the Examine window for oTarget on oPC
/// @param oPC The PC to show the Examine window to
/// @param oTarget The object for which to show the Examine window
void NWNX_Risenholm_ForceExamineWindow(object oPC, object oTarget);

/// @brief Opens the level up GUI for oPlayer.
/// @param oPlayer The player.
void NWNX_Risenholm_StartLevelUp(object oPlayer);

/// @brief Serialise oCreature once for a batch of afterimage clones. Call before a run of
/// NWNX_Risenholm_CreateAfterimage calls for the same creature, and NWNX_Risenholm_ReleaseAfterimage
/// after. One snapshot is held at a time; it is not kept across attacks, because the image must
/// reflect the creature's state at the moment of the attack.
/// @param oCreature The creature to snapshot.
/// @return The snapshot size in bytes, or 0 on failure.
int NWNX_Risenholm_PrepareAfterimage(object oCreature);

/// @brief Drop the snapshot taken by NWNX_Risenholm_PrepareAfterimage.
void NWNX_Risenholm_ReleaseAfterimage();

/// @brief Create an afterimage clone of oCreature at lLocation: a plot, unusable, non-PC, unlootable
/// copy that carries oCreature's effects, action queue, and equipment but none of its backpack or
/// local variables, with full hit points, the marker locals IS_SET_PIECE and IS_VFX, faction
/// nFaction, VFX_DUR_INVISIBILITY, a permanent 100% miss chance, a permanent cutscene ghost, and
/// animation speed fAnimationSpeed. Uses the snapshot from NWNX_Risenholm_PrepareAfterimage when it
/// is for this creature, and serialises on the spot otherwise.
/// @note Native replacement for the ObjectToJson/JsonToObject afterimage path. The caller only
/// orders the attack and the DestroyObject.
/// @param oCreature The creature to copy.
/// @param lLocation Where the clone appears; its facing is used too.
/// @param nFaction The ENGINE faction id (STANDARD_FACTION_* + 1).
/// @param fAnimationSpeed OBJECT_VISUAL_TRANSFORM_ANIMATION_SPEED for the clone; 1.0 leaves it alone.
/// @return The clone, or OBJECT_INVALID.
object NWNX_Risenholm_CreateAfterimage(object oCreature, location lLocation, int nFaction, float fAnimationSpeed = 1.0);

/// @brief Stop or allow oCreature running, as NWNX_Player_SetAlwaysWalk does.
/// @note Use this and NOT NWNX_Player_SetAlwaysWalk. Both drive the same engine flag, but the
/// Player version clears it without noticing the movement-limit effect that stealth mode, Slow
/// and encumbrance all hold it with, so turning it off while sneaking let the character run
/// while hidden. See the Forced walk section of Risenholm.cpp for the whole story.
/// @note Per-session, like the engine flag it sets: a character who logs out and back in is no
/// longer walking. pw_mod_enter re-pushes it from the PC local IS_FORCE_WALK_ON.
/// @param oCreature The creature.
/// @param bWalk TRUE to force walking, FALSE to hand the flag back to whatever else wants it.
void NWNX_Risenholm_SetAlwaysWalk(object oCreature, int bWalk = TRUE);

/// @brief Re-send the player-list entry of the player driving or owning oCreature to every client.
/// @note Call after possession changes what a player drives. Clients match a chat line to a player
/// through the object id in this entry, which the engine only sends at login, so while a player is
/// possessing something their lines get no clickable portrait and a tell from them cannot be
/// answered by clicking it. The name shown in the player list does not change.
/// @param oCreature The PC body, or the creature its player is driving.
void NWNX_Risenholm_RefreshPlayerListEntry(object oCreature);

/// @brief Whose inventory oPlayer's other-inventory panel is showing.
/// @note The panel NWNX_Player_OpenInventory opens on another creature. For a non-DM it is
/// read-only unless the owner is their own body or one of their associates: the plugin stops the
/// engine acting on it, which it otherwise would for any owner at all (see the plugin README).
/// @param oPlayer The player.
/// @return The owner, or OBJECT_INVALID if the panel is closed.
object NWNX_Risenholm_GetOtherInventoryOwner(object oPlayer);

/// @brief Keep oItem, which oOwner is carrying, out of oViewer's view of oOwner's inventory.
/// @note Covers the other-inventory panel's backpack and any bag opened inside it, never barter.
/// Registering against a different owner drops what was registered for the previous one.
/// @param oViewer The player looking.
/// @param oOwner The creature whose inventory they are looking at.
/// @param oItem The item to leave out.
void NWNX_Risenholm_ConcealItemFromViewer(object oViewer, object oOwner, object oItem);

/// @brief Drop everything registered as concealed from oViewer.
/// @param oViewer The player looking.
void NWNX_Risenholm_ClearConcealedItems(object oViewer);

/// @brief Whether oItem is registered as concealed from oViewer.
/// @param oViewer The player looking.
/// @param oItem The item.
/// @return TRUE if registered, FALSE otherwise -- including on a plugin build without concealment.
int NWNX_Risenholm_GetIsItemConcealedFrom(object oViewer, object oItem);

/// @brief Set the damage bonus limit for the running module, past the 255 SetDamageBonusLimit allows.
/// @note The engine clamps the PHYSICAL damage bonus from effects to this limit, summed across
/// every effect (CNWSCreatureStats::GetDamageRoll); elemental types are never clamped. The script
/// command only accepts 0-255, the range of the DamageBonusLimit server setting; this writes the same
/// module override the engine reads, without that range check. Lasts until the module unloads.
/// @param nLimit The new limit, 0 or more.
void NWNX_Risenholm_SetDamageBonusLimit(int nLimit);

/// @brief Copy out every level oCreature has taken, as the engine stores them, for RestoreLevelHistory.
/// @note Take this BEFORE deleveling: LevelDown deletes each level's record, and the .bic only ever
/// holds the levels a character currently has. The result is
/// {"levels": [{class, hitdie, ability, epic, skillpoints, skills[], feats[], known[10][], unknown[10][]}, ...],
///  "classes": [{class, domain1, domain2, school}, ...]}, level 1 first.
/// @param oCreature The creature.
/// @return The history, with empty arrays for an invalid creature.
json NWNX_Risenholm_GetLevelHistory(object oCreature);

/// @brief Give oCreature back the levels in jHistory above its current one, with no level-up dialog.
/// @note Each level is re-applied exactly as stored, including everything OnPlayerLevelUp did to it,
/// and OnPlayerLevelUp does NOT fire again. XP is set to nXP without feedback, and levels are restored
/// only as far as nXP pays for (and the module's level cap allows), so the client is never offered a
/// level-up for them. Refuses, changing nothing, if the levels oCreature still has are not the
/// history's own class for class, or if the history is malformed.
/// @param oCreature The creature.
/// @param jHistory A result of NWNX_Risenholm_GetLevelHistory.
/// @param nXP The creature's XP afterwards.
/// @return The number of levels restored, or -1 if refused.
int NWNX_Risenholm_RestoreLevelHistory(object oCreature, json jHistory, int nXP);

/// @brief The body of the request the RPC listener is running this script for.
/// @note The plugin's listener (NWNX_RISENHOLM_RPC_PORT) runs pw_rpc_<name> for
/// a request to /rpc/<name>, with the module as OBJECT_SELF, at the top of the
/// next server frame. See "RPC" in Risenholm.cpp and the plugin README.
/// @return The body as it was sent, or "" when no request is being handled.
string NWNX_Risenholm_GetRpcRequest();

/// @brief Set the answer to the request this script is running for. The
/// listener sends it back as the reply's body, as application/json.
/// @note A request whose script sets no answer is replied to with a 500. Does
/// nothing outside an RPC script.
/// @param sResponse The reply body. Pass JsonDump output: it is pure ASCII,
/// whatever the strings inside hold.
void NWNX_Risenholm_SetRpcResponse(string sResponse);

/// @}

void NWNX_Risenholm_SetPCLikeStatus(object oSourcePC, object oTargetPC, int bNewAttitude, int bSetReciprocal=TRUE)
{
    NWNXPushInt(bSetReciprocal);
    NWNXPushInt(bNewAttitude);
    NWNXPushObject(oTargetPC);
    NWNXPushObject(oSourcePC);

    NWNXCall(NWNX_Risenholm, "SetPCLikeStatus");
}

void NWNX_Risenholm_ForceUpdateMageArmorStats(object oCreature)
{
    NWNXPushObject(oCreature);
    NWNXCall(NWNX_Risenholm, "ForceUpdateMageArmorStats");
}

int NWNX_Risenholm_TrimAILists()
{
    NWNXCall(NWNX_Risenholm, "TrimAILists");
    return NWNXPopInt();
}

int NWNX_Risenholm_GetHasEffectByTag(object oObject, string sTag)
{
    NWNXPushString(sTag);
    NWNXPushObject(oObject);
    NWNXCall(NWNX_Risenholm, "GetHasEffectByTag");
    return NWNXPopInt();
}

int NWNX_Risenholm_GetHasEffectWithStringParam(object oObject, int nIndex, string sValue)
{
    NWNXPushString(sValue);
    NWNXPushInt(nIndex);
    NWNXPushObject(oObject);
    NWNXCall(NWNX_Risenholm, "GetHasEffectWithStringParam");
    return NWNXPopInt();
}

int NWNX_Risenholm_RemoveEffectsByTag(object oObject, string sTag)
{
    NWNXPushString(sTag);
    NWNXPushObject(oObject);
    NWNXCall(NWNX_Risenholm, "RemoveEffectsByTag");
    return NWNXPopInt();
}

int NWNX_Risenholm_GetIsDMManifested(object oCreature)
{
    NWNXPushObject(oCreature);
    NWNXCall(NWNX_Risenholm, "GetIsDMManifested");
    return NWNXPopInt();
}

void NWNX_Risenholm_ForceAppearanceUpdate(object oCreature, int bFullObjectUpdate = FALSE)
{
    NWNXPushInt(bFullObjectUpdate);
    NWNXPushObject(oCreature);
    NWNXCall(NWNX_Risenholm, "ForceAppearanceUpdate");
}

void NWNX_Risenholm_RefreshBodyParts(object oCreature)
{
    NWNXPushObject(oCreature);
    NWNXCall(NWNX_Risenholm, "RefreshBodyParts");
}

string NWNX_Risenholm_ExecuteCommand(string sCmd, string sArg1="", string sArg2="", string sArg3="", string sArg4="", string sArg5="", string sArg6="")
{
    NWNXPushString(sArg6);
    NWNXPushString(sArg5);
    NWNXPushString(sArg4);
    NWNXPushString(sArg3);
    NWNXPushString(sArg2);
    NWNXPushString(sArg1);
    NWNXPushString(sCmd);
    NWNXCall(NWNX_Risenholm, "ExecuteCommand");
    return NWNXPopString();
}

int NWNX_Risenholm_CheckForShutdownFile()
{
    NWNXCall(NWNX_Risenholm, "CheckForShutdownFile");
    return NWNXPopInt();
}

void NWNX_Risenholm_FixItemDestroySkipUseableState(object oItem)
{
    string sFunc = "FixItemDestroySkipUseableState";

    NWNXPushObject(oItem);
    NWNXCall(NWNX_Risenholm, sFunc);
}

int NWNX_Risenholm_UseItemInstant(object oCreature, object oItem, object oTarget)
{
    string sFunc = "UseItemInstant";

    NWNXPushObject(oTarget);
    NWNXPushObject(oItem);
    NWNXPushObject(oCreature);

    NWNXCall(NWNX_Risenholm, sFunc);
    return NWNXPopInt();
}

int NWNX_Risenholm_ApplyItemProperties(object oCreature, object oItem)
{
    NWNXPushObject(oItem);
    NWNXPushObject(oCreature);
    NWNXCall(NWNX_Risenholm, "ApplyItemProperties");
    return NWNXPopInt();
}

void NWNX_Risenholm_AddAttackOfOpportunity(object oCreature, object oTarget)
{
    string sFunc = "AddAttackOfOpportunity";

    NWNXPushObject(oTarget);
    NWNXPushObject(oCreature);

    NWNXCall(NWNX_Risenholm, sFunc);
}

void NWNX_Risenholm_ForceExamineWindow(object oPC, object oTarget)
{
    string sFunc = "ForceExamineWindow";

    NWNXPushObject(oTarget);
    NWNXPushObject(oPC);

    NWNXCall(NWNX_Risenholm, sFunc);
}

void NWNX_Risenholm_StartLevelUp(object oPlayer)
{
    NWNXPushObject(oPlayer);
    NWNXCall(NWNX_Risenholm, "StartLevelUp");
}

int NWNX_Risenholm_PrepareAfterimage(object oCreature)
{
    NWNXPushObject(oCreature);
    NWNXCall(NWNX_Risenholm, "PrepareAfterimage");
    return NWNXPopInt();
}

void NWNX_Risenholm_ReleaseAfterimage()
{
    NWNXCall(NWNX_Risenholm, "ReleaseAfterimage");
}

object NWNX_Risenholm_CreateAfterimage(object oCreature, location lLocation, int nFaction, float fAnimationSpeed = 1.0)
{
    vector vPosition = GetPositionFromLocation(lLocation);

    NWNXPushFloat(fAnimationSpeed);
    NWNXPushInt(nFaction);
    NWNXPushFloat(GetFacingFromLocation(lLocation));
    NWNXPushFloat(vPosition.z);
    NWNXPushFloat(vPosition.y);
    NWNXPushFloat(vPosition.x);
    NWNXPushObject(GetAreaFromLocation(lLocation));
    NWNXPushObject(oCreature);
    NWNXCall(NWNX_Risenholm, "CreateAfterimage");
    return NWNXPopObject();
}

void NWNX_Risenholm_SetAlwaysWalk(object oCreature, int bWalk = TRUE)
{
    NWNXPushInt(bWalk);
    NWNXPushObject(oCreature);
    NWNXCall(NWNX_Risenholm, "SetAlwaysWalk");
}

void NWNX_Risenholm_RefreshPlayerListEntry(object oCreature)
{
    NWNXPushObject(oCreature);
    NWNXCall(NWNX_Risenholm, "RefreshPlayerListEntry");
}

object NWNX_Risenholm_GetOtherInventoryOwner(object oPlayer)
{
    NWNXPushObject(oPlayer);
    NWNXCall(NWNX_Risenholm, "GetOtherInventoryOwner");
    return NWNXPopObject();
}

void NWNX_Risenholm_ConcealItemFromViewer(object oViewer, object oOwner, object oItem)
{
    NWNXPushObject(oItem);
    NWNXPushObject(oOwner);
    NWNXPushObject(oViewer);
    NWNXCall(NWNX_Risenholm, "ConcealItemFromViewer");
}

void NWNX_Risenholm_ClearConcealedItems(object oViewer)
{
    NWNXPushObject(oViewer);
    NWNXCall(NWNX_Risenholm, "ClearConcealedItems");
}

int NWNX_Risenholm_GetIsItemConcealedFrom(object oViewer, object oItem)
{
    NWNXPushObject(oItem);
    NWNXPushObject(oViewer);
    NWNXCall(NWNX_Risenholm, "GetIsItemConcealedFrom");
    return NWNXPopInt();
}

void NWNX_Risenholm_SetDamageBonusLimit(int nLimit)
{
    NWNXPushInt(nLimit);
    NWNXCall(NWNX_Risenholm, "SetDamageBonusLimit");
}

json NWNX_Risenholm_GetLevelHistory(object oCreature)
{
    NWNXPushObject(oCreature);
    NWNXCall(NWNX_Risenholm, "GetLevelHistory");
    return NWNXPopJson();
}

int NWNX_Risenholm_RestoreLevelHistory(object oCreature, json jHistory, int nXP)
{
    NWNXPushInt(nXP);
    NWNXPushJson(jHistory);
    NWNXPushObject(oCreature);
    NWNXCall(NWNX_Risenholm, "RestoreLevelHistory");
    return NWNXPopInt();
}

string NWNX_Risenholm_GetRpcRequest()
{
    NWNXCall(NWNX_Risenholm, "GetRpcRequest");
    return NWNXPopString();
}

void NWNX_Risenholm_SetRpcResponse(string sResponse)
{
    NWNXPushString(sResponse);
    NWNXCall(NWNX_Risenholm, "SetRpcResponse");
}
