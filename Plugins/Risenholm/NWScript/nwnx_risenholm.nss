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

/// @}

void NWNX_Risenholm_SetPCLikeStatus(object oSourcePC, object oTargetPC, int bNewAttitude, int bSetReciprocal=TRUE)
{
    string sFunc = "SetPCLikeStatus";

    NWNX_PushArgumentInt(NWNX_Risenholm, sFunc, bSetReciprocal);
    NWNX_PushArgumentInt(NWNX_Risenholm, sFunc, bNewAttitude);
    NWNX_PushArgumentObject(NWNX_Risenholm, sFunc, oTargetPC);
    NWNX_PushArgumentObject(NWNX_Risenholm, sFunc, oSourcePC);

    NWNX_CallFunction(NWNX_Risenholm, sFunc);
}

void NWNX_Risenholm_ForceUpdateMageArmorStats(object oCreature)
{
    string sFunc = "ForceUpdateMageArmorStats";
    NWNX_PushArgumentObject(NWNX_Risenholm, sFunc, oCreature);
    NWNX_CallFunction(NWNX_Risenholm, sFunc);
}
