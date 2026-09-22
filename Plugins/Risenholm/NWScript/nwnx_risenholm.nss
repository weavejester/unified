/// @addtogroup risenholm Risenholm
/// @brief Custom functions for the Scars of Risenholm PW server
/// @{
/// @file nwnx_risenholm.nss

const string NWNX_Risenholm = "NWNX_Risenholm"; ///< @private

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
/// @param oCreature The creature whose appearance should be re-sent.
/// @param bFullObjectUpdate If TRUE, re-send the whole object rather than just the appearance
/// block. Heavier - only needed if an appearance-only update proves insufficient.
void NWNX_Risenholm_ForceAppearanceUpdate(object oCreature, int bFullObjectUpdate = FALSE);

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
