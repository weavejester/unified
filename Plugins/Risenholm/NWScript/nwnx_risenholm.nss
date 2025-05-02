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

/// @brief Reload 2DA resources for players. Intended to be used after NWNX_Player_SetResManOverride.
/// @param oPlayer the player who needs their rules reloaded
void NWNX_Risenholm_ReloadRules(object oPlayer);

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

void NWNX_Risenholm_ReloadRules(object oPlayer)
{
    string sFunc = "ReloadRules";

    NWNXPushObject(oPlayer);
    NWNXCall(NWNX_Risenholm, sFunc);    
}