/// @addtogroup risenholm Risenholm
/// @brief Custom functions for the Scars of Risenholm PW server
/// @{
/// @file nwnx_risenholm.nss
#include "nwnx"

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

/// @}

void NWNX_Risenholm_SetPCLikeStatus(object oSourcePC, object oTargetPC, int bNewAttitude, int bSetReciprocal=TRUE)
{
    NWNX_PushArgumentInt(bSetReciprocal);
    NWNX_PushArgumentInt(bNewAttitude);
    NWNX_PushArgumentObject(oTargetPC);
    NWNX_PushArgumentObject(oSourcePC);

    NWNX_CallFunction(NWNX_Risenholm, "SetPCLikeStatus");
}

void NWNX_Risenholm_ForceUpdateMageArmorStats(object oCreature)
{
    NWNX_PushArgumentObject(oCreature);
    NWNX_CallFunction(NWNX_Risenholm, "ForceUpdateMageArmorStats");
}

string NWNX_Risenholm_ExecuteCommand(string sCmd, string sArg1="", string sArg2="", string sArg3="", string sArg4="", string sArg5="", string sArg6="")
{
    NWNX_PushArgumentString(sArg6);
    NWNX_PushArgumentString(sArg5);
    NWNX_PushArgumentString(sArg4);
    NWNX_PushArgumentString(sArg3);
    NWNX_PushArgumentString(sArg2);
    NWNX_PushArgumentString(sArg1);
    NWNX_PushArgumentString(sCmd);
    NWNX_CallFunction(NWNX_Risenholm, "ExecuteCommand");
    return NWNX_GetReturnValueString();
}

int NWNX_Risenholm_CheckForShutdownFile()
{
    NWNX_CallFunction(NWNX_Risenholm, "CheckForShutdownFile");
    return NWNX_GetReturnValueInt();
}

void NWNX_Risenholm_FixItemDestroySkipUseableState(object oItem)
{
    string sFunc = "FixItemDestroySkipUseableState";

    NWNX_PushArgumentObject(oItem);
    NWNX_CallFunction(NWNX_Item, sFunc);
}