#include "nwnx.hpp"

#include "API/CAppManager.hpp"
#include "API/CNWSCreature.hpp"
#include "API/CNWSCreatureStats.hpp"
#include "API/CServerExoApp.hpp"
#include "API/CNWSCombatRound.hpp"
#include "API/CNWRules.hpp"
#include "API/CNWCCMessageData.hpp"
#include "API/CNWSPlaceable.hpp"
#include "API/CNWSItem.hpp"
#include "API/CItemRepository.hpp"
#include "API/CPathfindInformation.hpp"
#include "API/CNWSArea.hpp"
#include <cmath>


using namespace NWNXLib;
using namespace NWNXLib::API;
using ArgumentStack = Events::ArgumentStack;


namespace Risenholm
{

static Hooks::Hook s_GetFlatFootedHook = Hooks::HookFunction(Functions::_ZN12CNWSCreature13GetFlatFootedEv,
    (void*)+[](CNWSCreature *pCreature) -> int32_t
    {
        auto *pCreatureStats = pCreature->m_pStats;
        auto *pScriptVarTable = Utils::GetScriptVarTable(pCreature);

        static CExoString sVarName = "FLAT_FOOTED_STATE";
        int32_t nFlatFootedState = pScriptVarTable->GetInt(sVarName);

        if (nFlatFootedState == 1)// Always FlatFooted
            return true;
        else if (nFlatFootedState == 2)// Never FlatFooted
            return false;

        if (pCreatureStats->HasFeat(Constants::Feat::UncannyReflex))
            return false;

        auto IsAIState = [&](uint16_t nAIState) -> bool {
            return ((pCreature->m_nAIState & nAIState) == nAIState);
        };

        if (pCreature->GetBlind() || pCreature->m_nState == 6/*Stunned*/ ||
            (!IsAIState(0x0002/*Arms*/) && !IsAIState(0x0004/*Legs*/)) ||
            (pCreature->m_nAnimation == Constants::Animation::KnockdownFront || pCreature->m_nAnimation == Constants::Animation::KnockdownButt))
            return true;

        return false;
    }, Hooks::Order::Final);

static void ResolvePlaceableSneakAndDeathAttack(CNWSCreature *pThis, CNWSPlaceable *pPlaceable)
{
    static const float SNEAK_ATTACK_DISTANCE =
            std::pow(Globals::Rules()->GetRulesetFloatEntry("MAX_RANGED_SNEAK_ATTACK_DISTANCE", 10.0f), 2);

    if (!pPlaceable)
        return;

    CNWSCombatAttackData* pAttackData = pThis->m_pcCombatRound->GetAttack(pThis->m_pcCombatRound->m_nCurrentAttack);

    if (pAttackData->m_nAttackType == Constants::Feat::WhirlwindAttack || pAttackData->m_nAttackType == Constants::Feat::ImprovedWhirlwind)
        return;

    const uint16_t sneakAttackFeats[] =
    {
        Constants::Feat::SneakAttack,
        Constants::Feat::SneakAttack2,
        Constants::Feat::SneakAttack3,
        Constants::Feat::SneakAttack4,
        Constants::Feat::SneakAttack5,
        Constants::Feat::SneakAttack6,
        Constants::Feat::SneakAttack7,
        Constants::Feat::SneakAttack8,
        Constants::Feat::SneakAttack9,
        Constants::Feat::SneakAttack10,
        Constants::Feat::SneakAttack11,
        Constants::Feat::SneakAttack12,
        Constants::Feat::SneakAttack13,
        Constants::Feat::SneakAttack14,
        Constants::Feat::SneakAttack15,
        Constants::Feat::SneakAttack16,
        Constants::Feat::SneakAttack17,
        Constants::Feat::SneakAttack18,
        Constants::Feat::SneakAttack19,
        Constants::Feat::SneakAttack20,
        Constants::Feat::BlackguardSneakAttack1d6,
        Constants::Feat::BlackguardSneakAttack2d6,
        Constants::Feat::BlackguardSneakAttack3d6,
        Constants::Feat::BlackguardSneakAttack4d6,
        Constants::Feat::BlackguardSneakAttack5d6,
        Constants::Feat::BlackguardSneakAttack6d6,
        Constants::Feat::BlackguardSneakAttack7d6,
        Constants::Feat::BlackguardSneakAttack8d6,
        Constants::Feat::BlackguardSneakAttack9d6,
        Constants::Feat::BlackguardSneakAttack10d6,
        Constants::Feat::BlackguardSneakAttack11d6,
        Constants::Feat::BlackguardSneakAttack12d6,
        Constants::Feat::BlackguardSneakAttack13d6,
        Constants::Feat::BlackguardSneakAttack14d6,
        Constants::Feat::BlackguardSneakAttack15d6,
        Constants::Feat::EpicImprovedSneakAttack1,
        Constants::Feat::EpicImprovedSneakAttack2,
        Constants::Feat::EpicImprovedSneakAttack3,
        Constants::Feat::EpicImprovedSneakAttack4,
        Constants::Feat::EpicImprovedSneakAttack5,
        Constants::Feat::EpicImprovedSneakAttack6,
        Constants::Feat::EpicImprovedSneakAttack7,
        Constants::Feat::EpicImprovedSneakAttack8,
        Constants::Feat::EpicImprovedSneakAttack9,
        Constants::Feat::EpicImprovedSneakAttack10
    };

    bool hasSneakAttack = false;
    for (auto sneakAttackFeat : sneakAttackFeats)
    {
        if (pThis->m_pStats->HasFeat(sneakAttackFeat))
        {
            hasSneakAttack = true;
            break;
        }
    }

    const uint16_t deathAttackFeats[] =
    {
        Constants::Feat::PrestigeDeathAttack1,
        Constants::Feat::PrestigeDeathAttack2,
        Constants::Feat::PrestigeDeathAttack3,
        Constants::Feat::PrestigeDeathAttack4,
        Constants::Feat::PrestigeDeathAttack5,
        Constants::Feat::PrestigeDeathAttack6,
        Constants::Feat::PrestigeDeathAttack7,
        Constants::Feat::PrestigeDeathAttack8,
        Constants::Feat::PrestigeDeathAttack9,
        Constants::Feat::PrestigeDeathAttack10,
        Constants::Feat::PrestigeDeathAttack11,
        Constants::Feat::PrestigeDeathAttack12,
        Constants::Feat::PrestigeDeathAttack13,
        Constants::Feat::PrestigeDeathAttack14,
        Constants::Feat::PrestigeDeathAttack15,
        Constants::Feat::PrestigeDeathAttack16,
        Constants::Feat::PrestigeDeathAttack17,
        Constants::Feat::PrestigeDeathAttack18,
        Constants::Feat::PrestigeDeathAttack19,
        Constants::Feat::PrestigeDeathAttack20
    };

    bool hasDeathAttack = false;
    for (auto deathAttackFeat : deathAttackFeats)
    {
        if (pThis->m_pStats->HasFeat(deathAttackFeat))
        {
            hasDeathAttack = true;
            break;
        }
    }

    if (!hasSneakAttack && !hasDeathAttack)
        return;

    if (pAttackData->m_bRangedAttack)
    {
        Vector v = pThis->m_vPosition;
        v.x -= pPlaceable->m_vPosition.x;
        v.y -= pPlaceable->m_vPosition.y;
        v.z -= pPlaceable->m_vPosition.z;
        float fDistance = v.x * v.x + v.y * v.y + v.z * v.z;
        if (fDistance >= SNEAK_ATTACK_DISTANCE)
            return;
    }

    pAttackData->m_bSneakAttack = hasSneakAttack;
    pAttackData->m_bDeathAttack = hasDeathAttack;
}
static Hooks::Hook s_ResolveAttackRollHook = Hooks::HookFunction(Functions::_ZN12CNWSCreature17ResolveAttackRollEP10CNWSObject,
    (void*)+[](CNWSCreature *pThis, CNWSObject *pTarget) -> void
    {
        if (!pTarget)
            return;

        CNWSCombatRound *pCombatRound = pThis->m_pcCombatRound;
        CNWSCombatAttackData *pAttackData = pCombatRound->GetAttack(pCombatRound->m_nCurrentAttack);
        int32_t nAttackRoll = Globals::Rules()->RollDice(1, 20);

        // DEBUG
        if (Globals::EnableCombatDebugging())
        {
            pAttackData->m_sAttackDebugText.Format("%s Attack Roll: %d", pThis->m_pStats->GetFullName().CStr(), nAttackRoll);
        }
        // /////

        CNWSCreature *pCreature = Utils::AsNWSCreature(pTarget);
        int32_t nAttackRollModifier, nArmorClass;

        if (pCreature)
        {
            nAttackRollModifier = pThis->m_pStats->GetAttackModifierVersus(pCreature);
            nArmorClass = pCreature->m_pStats->GetArmorClassVersus(pThis);
        }
        else
        {
            nAttackRollModifier = pThis->m_pStats->GetAttackModifierVersus();
            nArmorClass = 0;
        }

        // DEBUG
        if (Globals::EnableCombatDebugging())
        {
            CExoString sCurrent = pAttackData->m_sAttackDebugText;
            CExoString sAdd;

            sAdd.Format(" Versus AC %d", nArmorClass);
            pAttackData->m_sAttackDebugText = sCurrent + sAdd;
        }
        // /////

        if (pCreature)
        {
            pThis->ResolveSneakAttack(pCreature);
            pThis->ResolveDeathAttack(pCreature);
        }
        // RISENHOLM MODIFICATION: Sneak/Death Attack Placeables
        else if (auto *pPlaceable = Utils::AsNWSPlaceable(pTarget))
        {
            static CExoString sVarName = "SNEAK_ATTACK_IMMUNE";
            if (!Utils::GetScriptVarTable(pTarget)->GetInt(sVarName))
                ResolvePlaceableSneakAndDeathAttack(pThis, pPlaceable);
        }
        // END RISENHOLM MODIFICATION

        if (pAttackData->m_bCoupDeGrace)
        {
            pAttackData->m_nToHitRoll = 20;
            pAttackData->m_nToHitMod = nAttackRollModifier;
            pAttackData->m_nAttackResult = 7;/*Automatic Hit*/
            return;
        }

        pAttackData->m_nToHitRoll = nAttackRoll;
        pAttackData->m_nToHitMod = nAttackRollModifier;

        if (pThis->ResolveDefensiveEffects(pTarget, (nAttackRoll + nAttackRollModifier >= nArmorClass)))
            return;

        // Parry Check
        if (pCreature)
        {
            if (nAttackRoll != 20)
            {
                if (pCreature->m_nCombatMode == Constants::CombatMode::Parry &&
                    pCreature->m_pcCombatRound->m_nParryActions > 0 &&
                    !pCreature->m_pcCombatRound->m_bRoundPaused &&
                    pCreature->m_nState != 6/*Stunned*/ &&
                    !pAttackData->m_bRangedAttack &&
                    !pCreature->GetRangeWeaponEquipped())
                {
                    static int32_t nParryRiposteDifference = Globals::Rules()->GetRulesetIntEntry("PARRY_RIPOSTE_DIFFERENCE", 10);
                    int32_t nParryRoll = Globals::Rules()->RollDice(1, 20) +
                            pCreature->m_pStats->GetSkillRank(Constants::Skill::Parry, Utils::AsNWSObject(pThis));

                    if (nParryRoll >= nAttackRoll + nAttackRollModifier)
                    {
                        if (nParryRoll - nParryRiposteDifference >= nAttackRoll + nAttackRollModifier)
                        {
                            pCreature->m_pcCombatRound->AddParryAttack(pThis->m_idSelf);
                        }

                        pAttackData->m_nAttackResult = 2;/*Parried*/
                        pCreature->m_pcCombatRound->m_nParryActions--;
                        return;
                    }

                    pCreature->m_pcCombatRound->AddParryIndex();
                    pCreature->m_pcCombatRound->m_nParryActions--;
                }
            }
        }

        if (nAttackRoll != 1)
        {
            if ((nAttackRoll + nAttackRollModifier >= nArmorClass) || nAttackRoll == 20)
            {
                if (nAttackRoll >= pThis->m_pStats->GetCriticalHitRoll(pCombatRound->GetOffHandAttack()))
                {
                    int32_t nCriticalHitRoll = Globals::Rules()->RollDice(1, 20);

                    pAttackData->m_bCriticalThreat = true;
                    pAttackData->m_nThreatRoll = nCriticalHitRoll;

                    if (nCriticalHitRoll + nAttackRollModifier >= nArmorClass)
                    {
                        if (pCreature)
                        {
                            if (Globals::AppManager()->m_pServerExoApp->GetDifficultyOption(0/*No Critical Hits On PCs*/))
                            {
                                if (pCreature->m_bPlayerCharacter && !pThis->m_bPlayerCharacter)
                                {
                                    pAttackData->m_nAttackResult = 1;/*Successful Hit*/
                                    return;
                                }
                            }

                            if (pCreature->m_pStats->GetEffectImmunity(Constants::ImmunityType::CriticalHit, pThis))
                            {
                                auto *pData = new CNWCCMessageData;
                                pData->SetObjectID(0, pCreature->m_idSelf);
                                pData->SetInteger(0, 126/*Critical Hit Immunity Feedback*/);

                                pAttackData->m_alstPendingFeedback.Add(pData);
                                pAttackData->m_nAttackResult = 1;/*Successful Hit*/
                                return;
                            }
                        }

                        pAttackData->m_nAttackResult = 3;/*Critical Hit*/
                        return;
                    }
                }

                pAttackData->m_nAttackResult = 1;/*Successful Hit*/
                return;
            }
        }

        pAttackData->m_nAttackResult = 4;/*Miss*/

        if (nAttackRoll == 1)
            pAttackData->m_nMissedBy = 1;
        else
            pAttackData->m_nMissedBy = std::abs(nAttackRoll + nAttackRollModifier - nArmorClass);

    }, Hooks::Order::Final);


/*
static Hooks::Hook s_GetWeightHook = Hooks::HookFunction(Functions::_ZN8CNWSItem9GetWeightEv,
    (void*)+[](CNWSItem *pThis) -> int32_t
    {
        int32_t nWeight;

        if (auto *pItemRepository = pThis->m_pItemRepository)
            nWeight = pThis->m_nWeight + pItemRepository->CalculateContentsWeight();
        else if (pThis->m_nStackSize > 1)
            nWeight = pThis->m_nWeight * pThis->m_nStackSize;
        else
            nWeight = pThis->m_nWeight;

        if (auto *pCreature = Utils::AsNWSCreature(Utils::GetGameObject(pThis->m_oidPossessor)))
        {
            if (pCreature->m_pStats->HasFeat(12345))
                nWeight *= 0.5f;
        }

        return nWeight;
    }, Hooks::Order::Final);
static Hooks::Hook s_AcquireItemHook = Hooks::HookFunction(Functions::_ZN12CNWSCreature11AcquireItemEPP8CNWSItemjjhhii,
    (void*)+[](CNWSCreature* thisPtr, CNWSItem **ppItem, ObjectID oidPossessor, ObjectID oidTargetRepository,
            uint8_t x, uint8_t y, int32_t bOriginatingFromScript, int32_t bDisplayFeedback) -> int32_t
    {
        auto retVal = s_AcquireItemHook->CallOriginal<int32_t>(thisPtr, ppItem, oidPossessor, oidTargetRepository,
                                                                   x, y, bOriginatingFromScript, bDisplayFeedback);

        if (thisPtr->m_pStats->HasFeat(12345))
            thisPtr->UpdateEncumbranceState();

        return retVal;
    }, Hooks::Order::Earliest);
*/


static Hooks::Hook s_DoCombatStepHook = Hooks::HookFunction(Functions::_ZN12CNWSCreature12DoCombatStepEhij,
    (void*)+[](CNWSCreature *pThis, uint8_t nStepType, int32_t nAnimationTime, ObjectID oidTarget) -> void
    {
        static CExoString sVarName = "DISABLE_COMBAT_SHUFFLE";
        if (!Utils::GetScriptVarTable(pThis)->GetInt(sVarName))
        {
            s_DoCombatStepHook->CallOriginal<void>(pThis, nStepType, nAnimationTime, oidTarget);
            return;
        }

        CNWSObject *pTarget = Utils::AsNWSObject(Utils::GetGameObject(oidTarget));
        CNWSCombatRound *pCombatRound = pThis->m_pcCombatRound;

        auto IsAIState = [&](uint16_t nAIState) -> bool { return ((pThis->m_nAIState & nAIState) == nAIState); };
        if (!pTarget || !pThis->GetArea() || !IsAIState(0x0004/*Legs*/))
        {
            pCombatRound->m_bRoundPaused = false;
            pCombatRound->SetPauseTimer(0);
            pThis->SetAnimation(Constants::Animation::Ready);
            return;
        }

        auto Normalize = [](const Vector& v, float fMagnitude) -> Vector
        {
            if (fMagnitude < 0.000000001)
                return Vector{1.0f, 0.0f, 0.0f};
            else
                return Vector{v.x / fMagnitude, v.y / fMagnitude, v.z /fMagnitude};
        };

        Vector vThis = pThis->m_vPosition;
        Vector vTarget = pTarget->m_vPosition;
        auto vPosition = Vector{vThis.x - vTarget.x, vThis.y - vTarget.y, vThis.z - vTarget.z};
        auto fDeltaRange = (float)sqrt((vPosition.x * vPosition.x) + (vPosition.y * vPosition.y) + (vPosition.z * vPosition.z));
        Vector vOrientation = Normalize(vPosition, fDeltaRange);

        int32_t nAnimation;
        float fDesiredAttackRange;
        if (nStepType == 0 || nStepType == 1)
        {
            nAnimation = Constants::Animation::CombatStepDummy;
            int32_t bRangedWeapon = pThis->GetRangeWeaponEquipped();

            if (bRangedWeapon)
            {
                auto *pCreature = Utils::AsNWSCreature(pTarget);
                if (pCreature && !pCreature->GetRangeWeaponEquipped())
                    fDesiredAttackRange = pCreature->MaxAttackRange(pThis->m_idSelf) + 2 * 0.25f;
                else
                    fDesiredAttackRange = pThis->DesiredAttackRange(oidTarget, true);
            }
            else
                fDesiredAttackRange = pThis->DesiredAttackRange(oidTarget);

            if (fDeltaRange < (fDesiredAttackRange - 0.25f))
                nAnimation = Constants::Animation::CombatStepBack;
            else if (bRangedWeapon)
            {
                pCombatRound->m_bRoundPaused = false;
                pCombatRound->SetPauseTimer(0);
                pThis->SetAnimation(Constants::Animation::Ready);
                return;
            }
            else if (fDeltaRange > (fDesiredAttackRange + 0.25f))
                nAnimation = Constants::Animation::CombatStepFront;
            else
            {
                // RISENHOLM MODIFICATION: Disable Left/Right Combat Step
                pCombatRound->m_bRoundPaused = false;
                pCombatRound->SetPauseTimer(0);
                pThis->SetAnimation(Constants::Animation::Ready);
                return;
                // END RISENHOLM MODIFICATION
            }
        }
        else if (nStepType == 2)
            nAnimation = Constants::Animation::CombatStepFront;
        else if (nStepType == 3)
            nAnimation = Constants::Animation::CombatStepBack;
        else if (nStepType == 4)
            nAnimation = Constants::Animation::CombatStepLeft;
        else if (nStepType == 5)
            nAnimation = Constants::Animation::CombatStepRight;
        else
        {
            pCombatRound->m_bRoundPaused = false;
            pCombatRound->SetPauseTimer(0);
            pThis->SetAnimation(Constants::Animation::Ready);
            return;
        }

        auto VectorAdd = [](const Vector& v1, const Vector& v2) -> Vector
        {
            return Vector{v1.x + v2.x, v1.y + v2.y, v1.z + v2.z};
        };

        auto VectorMultiply = [](const Vector& v1, float f) -> Vector
        {
            return Vector{v1.x * f, v1.y * f, v1.z * f};
        };

        float fRadianOrientation, fRadianTheta;
        Vector vDesiredPosition{};
        if (nAnimation == Constants::Animation::CombatStepLeft || nAnimation == Constants::Animation::CombatStepRight)
        {
            fRadianOrientation = (float)atan2(vOrientation.y, vOrientation.x);
            fRadianTheta = (float)asin((0.8f / 2) / fDeltaRange) * 2.0f;

            if (nAnimation == Constants::Animation::CombatStepLeft)
            {
                fRadianTheta = fRadianOrientation - fRadianTheta;
                if (fRadianTheta < 0)
                    fRadianTheta = (2 * M_PI) + fRadianTheta;
            }
            else
            {
                fRadianTheta = fRadianOrientation + fRadianTheta;
                if (fRadianTheta > (2 * M_PI))
                    fRadianTheta = (2 * M_PI) - fRadianTheta;
            }

            vDesiredPosition.x = (float)cos(fRadianTheta);
            vDesiredPosition.y = (float)sin(fRadianTheta);

            vDesiredPosition = VectorAdd(vTarget, VectorMultiply(vDesiredPosition, fDeltaRange));
        }
        else if (nAnimation == Constants::Animation::CombatStepFront || nAnimation == Constants::Animation::CombatStepBack)
        {
            float fTemp = 0.8f;

            if (nStepType == 0 || nStepType == 1)
            {
                if (nAnimation == Constants::Animation::CombatStepFront)
                {
                    if ((fDeltaRange - fDesiredAttackRange) > 0.8f)
                        fTemp = 0.8f;
                    else
                        fTemp = fDeltaRange - fDesiredAttackRange;
                }
                else
                {
                    if ((fDesiredAttackRange - fDeltaRange) > 0.8f)
                        fTemp = 0.8f;
                    else
                        fTemp = fDesiredAttackRange - fDeltaRange;
                }
            }

            Vector vStepDirection = nAnimation == Constants::Animation::CombatStepFront ?
                    Vector{-vOrientation.x, -vOrientation.y, -vOrientation.z} :
                    vOrientation;

            vDesiredPosition = VectorAdd(vThis, VectorMultiply(vStepDirection, fTemp));
        }

        float fPersonalSpace = pThis->m_pcPathfindInformation->m_fCreaturePersonalSpace;
        float fCreatureHeight = pThis->m_pcPathfindInformation->m_fHeight;
        pThis->GetArea()->m_pSearchInfo = pThis->m_pcPathfindInformation;
        ObjectID oidObjectMovingTo = pThis->m_pcPathfindInformation->m_oidMovingTo;

        pThis->m_pcPathfindInformation->m_oidMovingTo = Constants::OBJECT_INVALID;
        bool bDirectLine = pThis->GetArea()->TestDirectLine(vThis.x, vThis.y, vDesiredPosition.x, vDesiredPosition.y, fPersonalSpace, fCreatureHeight, false) == 1;
        pThis->m_pcPathfindInformation->m_oidMovingTo = oidObjectMovingTo;

        if (vDesiredPosition.x < 0.0f ||
            vDesiredPosition.x > pThis->GetArea()->m_nWidth * 10.0f ||
            vDesiredPosition.y < 0.0f ||
            vDesiredPosition.y > pThis->GetArea()->m_nHeight * 10.0f)
        {
            pCombatRound->m_bRoundPaused = false;
            pCombatRound->SetPauseTimer(0);
            pThis->SetAnimation(Constants::Animation::Ready);
            return;
        }

        if (bDirectLine)
        {
            pThis->UpdateSubareasOnMoveTo(vThis, vDesiredPosition, false, nullptr);
            pThis->SetPosition(vDesiredPosition);
        }
        else
        {
            pCombatRound->m_bRoundPaused = false;
            pCombatRound->SetPauseTimer(0);
            pThis->SetAnimation(Constants::Animation::Ready);
            return;
        }
    }, Hooks::Order::Latest);


NWNX_EXPORT ArgumentStack SetPCLikeStatus(ArgumentStack&& args)
{
    auto sourceOID      = args.extract<ObjectID>();
    auto targetOID      = args.extract<ObjectID>();
    auto bNewAttitude   = args.extract<int32_t>();
    auto bSetReciprocal = args.extract<int32_t>();

    if (auto *pSource = Globals::AppManager()->m_pServerExoApp->GetCreatureByGameObjectID(sourceOID))
    {
        pSource->SetPVPPlayerLikesMe(targetOID, bNewAttitude, bSetReciprocal);
    }

    return {};
}

}