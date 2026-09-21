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
#include "API/CNWSInventory.hpp"
#include "API/CNWBaseItemArray.hpp"
#include "API/CNWBaseItem.hpp"
#include "API/CServerAIMaster.hpp"
#include "API/CTwoDimArrays.hpp"
#include "API/CNWSAreaOfEffectObject.hpp"
#include "API/CNWSDoor.hpp"
#include "API/CGameEffect.hpp"
#include "API/CNWSTrigger.hpp"
#include "External/subprocess.hpp"
#include "API/CNWSPlayer.hpp"
#include <cmath>
#include <dlfcn.h>
#include <unordered_map>
#include "API/CExoLinkedListInternal.hpp"
#include "API/CNWSScriptVar.hpp"
#include "API/CNWSScriptVarTable.hpp"
#include "API/CServerExoAppInternal.hpp"
#include "API/CNWSFaction.hpp"
#include "API/CFactionManager.hpp"
#include <vector>


using namespace NWNXLib;
using namespace NWNXLib::API;


namespace Risenholm
{

static bool s_AddItemCastSpellGrenadeAction;

// ---------------------------------------------------------------------------
// AI update list trimming
// ---------------------------------------------------------------------------
//
// CServerAIMaster::UpdateState walks every object in every one of its five AI
// lists once per server frame and calls the object's AIUpdate. For an idle
// placeable -- no queued actions, no applied effects -- CNWSPlaceable::AIUpdate
// returns after a handful of field checks, and CNWSItem::AIUpdate does nothing
// at all unless the item carries an applied effect (both read straight from
// the 8193.37 disassembly: the placeable checks m_lQueuedActions then
// m_appliedEffects.num, the item checks only m_appliedEffects.num). The cost
// is the visit itself. With the ~45,000 placeables and the tens of thousands of
// items this module keeps in those lists, the 2026-09 dev profile put
// AIUpdatePlaceable at 68 ms per wall second with nobody online and 120 ms
// with one player, and AIUpdateItem at 21 ms -- roughly 28 ns a visit, every
// frame, forever.
//
// So: objects that cannot need the visit are kept out of the lists. Static
// placeables (34,785 of the 45,039 placed in this module) can never queue an
// action or run a script, and a non-static one with no heartbeat script that
// is not die-when-empty passes the same idle test AIUpdate itself uses, so
// both are removed as they are added to an area.
// Items are taken out as they are constructed. Both are put back the moment something
// arrives that AIUpdate would have to process: an applied effect
// (CNWSObject::ApplyEffect), or for a placeable a queued action (handled
// inside this file's existing CNWSObject::AddAction hook further down). Membership is tracked by the engine's own
// m_nAILevel: RemoveObject sets it to -1, AddObject treats anything other than
// -1 as "already listed", so -1 is exactly "in no list".
//
// TrimAILists (exported below) sweeps the lists once for anything that reached
// them by another route -- CopyArea instances, objects created before the
// hooks saw them -- and is meant to be called from OnModuleLoad.
//
// Switches, each default off, in the NWNX Optimizations style so the start-up
// script can turn them on and off one at a time:
//   NWNX_RISENHOLM_TRIM_AI_STATIC_PLACEABLES   static placeables
//   NWNX_RISENHOLM_TRIM_AI_IDLE_PLACEABLES     idle non-static placeables
//   NWNX_RISENHOLM_TRIM_AI_ITEMS               items without effects
//   NWNX_RISENHOLM_TRIM_AI_IDLE_DOORS          doors with no heartbeat script, actions, or effects
//   NWNX_RISENHOLM_TRIM_AI_IDLE_TRIGGERS       triggers with no heartbeat script or actions
// The state of each is logged once when the plugin loads.
//
// Doors share the placeable's idle test exactly (CNWSDoor::AIUpdate returns at
// once when m_sScripts[5], the heartbeat slot, is empty and there are no
// actions and no effects). Triggers have no early-out at all: every one of
// them does the world-time arithmetic and a heartbeat check every frame, though
// enter and exit detection is done by the moving creature, not here. Both from
// the 8193.37 decompilation.

struct TrimAISwitches
{
    bool bStaticPlaceables;
    bool bIdlePlaceables;
    bool bItems;
    bool bIdleDoors;
    bool bIdleTriggers;

    bool Any() const { return bStaticPlaceables || bIdlePlaceables || bItems || bIdleDoors || bIdleTriggers; }
};

static const TrimAISwitches& GetTrimAISwitches()
{
    static const TrimAISwitches s_switches = []() -> TrimAISwitches
    {
        TrimAISwitches c;
        c.bStaticPlaceables = Config::Get<bool>("TRIM_AI_STATIC_PLACEABLES", false);
        c.bIdlePlaceables   = Config::Get<bool>("TRIM_AI_IDLE_PLACEABLES", false);
        c.bItems            = Config::Get<bool>("TRIM_AI_ITEMS", false);
        c.bIdleDoors        = Config::Get<bool>("TRIM_AI_IDLE_DOORS", false);
        c.bIdleTriggers     = Config::Get<bool>("TRIM_AI_IDLE_TRIGGERS", false);

        LOG_INFO("AI update list trimming: static placeables %s, idle placeables %s, items %s, idle doors %s, idle triggers %s",
                 c.bStaticPlaceables ? "on" : "off", c.bIdlePlaceables ? "on" : "off", c.bItems ? "on" : "off",
                 c.bIdleDoors ? "on" : "off", c.bIdleTriggers ? "on" : "off");

        return c;
    }();

    return s_switches;
}

// Read (and so logged) at plugin load rather than on the first object.
static const bool s_bTrimAISwitchesLogged = (GetTrimAISwitches(), true);

static bool TrimAIListsEnabled()
{
    return GetTrimAISwitches().Any();
}

static bool GetIsIdleForAIList(CNWSObject *pObject)
{
    return pObject->m_appliedEffects.num == 0 &&
           pObject->m_lQueuedActions.m_pcExoLinkedListInternal->m_nCount == 0;
}

// m_sScripts slot of the placeable OnHeartbeat script (EVENT_SCRIPT_PLACEABLE_ON_HEARTBEAT is 9004).
static constexpr int PLACEABLE_SCRIPT_HEARTBEAT = 4;

static bool GetIsTrimmablePlaceable(CNWSObject *pObject)
{
    auto *pPlaceable = Utils::AsNWSPlaceable(pObject);

    if (!pPlaceable || !GetIsIdleForAIList(pObject)) return false;
    if (pPlaceable->m_bStaticObject) return GetTrimAISwitches().bStaticPlaceables;
    if (!GetTrimAISwitches().bIdlePlaceables) return false;

    // Non-static: the same test CNWSPlaceable::AIUpdate itself applies before
    // deciding it has nothing to do -- it returns at once when the heartbeat
    // script slot is empty, the action queue is empty, no effect is applied,
    // and m_bDieWhenEmpty is off (8193.37 disassembly, offsets 0x418, 0x100,
    // 0x150, 0x4fc). Containers are left in as well: there are 29 of them and
    // their open/close bookkeeping is not worth reasoning about for the gain.
    //
    // Known limit: a script that later assigns a heartbeat to one of these
    // with SetEventScript would not get its beats until something else puts
    // the object back (an effect or an action). Nothing in the module does
    // that today; pw_oh_* scripts only ever CLEAR placeable heartbeats.
    return pPlaceable->m_sScripts[PLACEABLE_SCRIPT_HEARTBEAT].IsEmpty() &&
           !pPlaceable->m_bDieWhenEmpty &&
           !pPlaceable->m_bHasInventory;
}

static bool GetIsTrimmableItem(CNWSObject *pObject)
{
    return GetTrimAISwitches().bItems &&
           pObject->m_nObjectType == Constants::ObjectType::Item &&
           pObject->m_appliedEffects.num == 0;
}

// Door heartbeat slot: EVENT_SCRIPT_DOOR_ON_HEARTBEAT is 10005, slot 5.
static constexpr int DOOR_SCRIPT_HEARTBEAT = 5;
// Trigger heartbeat slot: EVENT_SCRIPT_TRIGGER_ON_HEARTBEAT is 7000, slot 0.
static constexpr int TRIGGER_SCRIPT_HEARTBEAT = 0;

static bool GetIsTrimmableDoor(CNWSObject *pObject)
{
    auto *pDoor = Utils::AsNWSDoor(pObject);

    return pDoor && GetTrimAISwitches().bIdleDoors && GetIsIdleForAIList(pObject) &&
           pDoor->m_sScripts[DOOR_SCRIPT_HEARTBEAT].IsEmpty();
}

static bool GetIsTrimmableTrigger(CNWSObject *pObject)
{
    auto *pTrigger = Utils::AsNWSTrigger(pObject);

    return pTrigger && GetTrimAISwitches().bIdleTriggers && GetIsIdleForAIList(pObject) &&
           pTrigger->m_sScripts[TRIGGER_SCRIPT_HEARTBEAT].IsEmpty();
}

static bool GetIsTrimmable(CNWSObject *pObject)
{
    return GetIsTrimmablePlaceable(pObject) || GetIsTrimmableItem(pObject) ||
           GetIsTrimmableDoor(pObject) || GetIsTrimmableTrigger(pObject);
}

static void RestoreToAIList(CNWSObject *pObject)
{
    if (pObject->m_nAILevel != -1) return;

    switch (pObject->m_nObjectType)
    {
        case Constants::ObjectType::Item:
        case Constants::ObjectType::Placeable:
        case Constants::ObjectType::Door:
        case Constants::ObjectType::Trigger:
            break;
        default:
            return;
    }

    Globals::AppManager()->m_pServerExoApp->GetServerAIMaster()->AddObject(pObject, 0);
}

// Items: taken out right after construction, which is where the engine adds
// them (the CNWSItem constructor ends with an AddObject at level 0). Hooking
// CServerAIMaster::AddObject itself is not an option: its five-byte prologue is
// a compare and a short conditional jump, which the hook engine refuses to
// relocate -- funchook_prepare asserts at plugin load. The constructor has an
// ordinary prologue. NWNX does not list constructors in Functions.hpp, so the
// address comes from the exported symbol.
static Hooks::Hook s_TrimItemCtorHook = []() -> Hooks::Hook
{
    void *pCtor = dlsym(RTLD_DEFAULT, "_ZN8CNWSItemC1Ej");

    if (!pCtor)
    {
        LOG_ERROR("CNWSItem constructor symbol not found; item AI list trimming is off");
        return nullptr;
    }

    return Hooks::HookFunction(pCtor,
        (void*)+[](CNWSItem *pItem, uint32_t nObjectId) -> void
        {
            s_TrimItemCtorHook->CallOriginal<void>(pItem, nObjectId);

            CNWSObject *pObject = pItem;

            if (TrimAIListsEnabled() && pObject->m_nAILevel != -1 && GetIsTrimmableItem(pObject))
                Globals::AppManager()->m_pServerExoApp->GetServerAIMaster()->RemoveObject(pObject);
        }, Hooks::Order::Earliest);
}();

// Static placeables: out as soon as they are placed. The static flag is read
// from the GIT in LoadPlaceable, which CNWSArea::LoadPlaceables calls before
// AddToArea, so it is set by the time this runs.
static Hooks::Hook s_TrimPlaceableAddToAreaHook = Hooks::HookFunction(&CNWSPlaceable::AddToArea,
    +[](CNWSPlaceable *pPlaceable, CNWSArea *pArea, float fX, float fY, float fZ, BOOL bRunScripts) -> void
    {
        s_TrimPlaceableAddToAreaHook->CallOriginal<void>(pPlaceable, pArea, fX, fY, fZ, bRunScripts);

        if (TrimAIListsEnabled() && pPlaceable->m_nAILevel != -1 && GetIsTrimmablePlaceable(pPlaceable))
            Globals::AppManager()->m_pServerExoApp->GetServerAIMaster()->RemoveObject(pPlaceable);
    }, Hooks::Order::Earliest);

static Hooks::Hook s_TrimDoorAddToAreaHook = Hooks::HookFunction(&CNWSDoor::AddToArea,
    +[](CNWSDoor *pDoor, CNWSArea *pArea, float fX, float fY, float fZ, BOOL bRunScripts) -> void
    {
        s_TrimDoorAddToAreaHook->CallOriginal<void>(pDoor, pArea, fX, fY, fZ, bRunScripts);

        if (pDoor->m_nAILevel != -1 && GetIsTrimmableDoor(pDoor))
            Globals::AppManager()->m_pServerExoApp->GetServerAIMaster()->RemoveObject(pDoor);
    }, Hooks::Order::Earliest);

static Hooks::Hook s_TrimTriggerAddToAreaHook = Hooks::HookFunction(&CNWSTrigger::AddToArea,
    +[](CNWSTrigger *pTrigger, CNWSArea *pArea, float fX, float fY, float fZ, BOOL bRunScripts) -> void
    {
        s_TrimTriggerAddToAreaHook->CallOriginal<void>(pTrigger, pArea, fX, fY, fZ, bRunScripts);

        if (pTrigger->m_nAILevel != -1 && GetIsTrimmableTrigger(pTrigger))
            Globals::AppManager()->m_pServerExoApp->GetServerAIMaster()->RemoveObject(pTrigger);
    }, Hooks::Order::Earliest);

// Back in before anything AIUpdate would have to service.
static Hooks::Hook s_TrimApplyEffectHook = Hooks::HookFunction(&CNWSObject::ApplyEffect,
    +[](CNWSObject *pObject, CGameEffect *pEffect, BOOL bLoadingGame, BOOL bInitialApplication) -> void
    {
        if (TrimAIListsEnabled())
            RestoreToAIList(pObject);

        s_TrimApplyEffectHook->CallOriginal<void>(pObject, pEffect, bLoadingGame, bInitialApplication);
    }, Hooks::Order::Earliest);



// ---------------------------------------------------------------------------
// Idle creature AI throttling
// ---------------------------------------------------------------------------
//
// CNWSCreature::AIUpdate has no short path for a VERY_LOW creature. Every visit
// runs the world-time bookkeeping, UpdateEffectList, RunActions, a walkmesh
// ComputeHeight, ComputeAIState, UpdateTrapCheck, and the combat and
// attack-of-opportunity timers (Ghidra decompilation of 8193.37, ~nwserver-re/
// export/game/CNWSCreature.c). Only perception is spaced out for VERY_LOW, and
// that is done inside SpawnInHeartbeatPerception, not by skipping the call.
// With the placeables and items trimmed, the 1,142 creatures placed in this
// module's areas, nearly all of them standing in areas with nobody in them, are
// the largest remaining per-frame cost of the AI master.
//
// The function measures the world time elapsed since its previous visit
// (m_nLastUpdateCalendarDay / TimeOfDay) and advances every timer by that
// delta, so visiting an idle creature less often is safe for the timers: an
// effect expires and a heartbeat fires on the next visit, late by at most the
// frames skipped (about 0.3 s at a divisor of 20 and 70 fps). Nothing else is
// lost: the creature is at VERY_LOW, has no queued actions, is not in combat,
// and its area holds no player. The moment a player enters, the engine bumps
// the area's m_nPlayersInArea and raises the creature to LOW
// (CNWSArea::IncrementPlayersInArea), and both conditions stop the skip on
// the very next frame.
//
// Switch: NWNX_RISENHOLM_IDLE_CREATURE_AI_DIVISOR (integer, default 0 = off).
// A value of N visits each qualifying creature once every N frames, staggered
// by object id so the work spreads evenly across frames.

static int GetIdleCreatureAIDivisor()
{
    static const int s_nDivisor = []() -> int
    {
        int n = Config::Get<int>("IDLE_CREATURE_AI_DIVISOR", 0);
        LOG_INFO("Idle creature AI throttling: %s (divisor %d)", n > 1 ? "on" : "off", n);
        return n > 1 ? n : 0;
    }();

    return s_nDivisor;
}

static const bool s_bIdleCreatureAILogged = (GetIdleCreatureAIDivisor(), true);

// Counted in the AI master so every creature in a frame sees the same value.
static uint32_t s_nAIFrame = 0;

static Hooks::Hook s_IdleCreatureFrameHook = Hooks::HookFunction(&CServerAIMaster::UpdateState,
    +[](CServerAIMaster *pAIMaster) -> void
    {
        s_nAIFrame++;
        s_IdleCreatureFrameHook->CallOriginal<void>(pAIMaster);
    }, Hooks::Order::Earliest);

static bool GetIsIdleCreatureForAI(CNWSCreature *pCreature)
{
    if (pCreature->m_bPlayerCharacter) return false;
    if (pCreature->m_nAILevel != 0) return false;   // VERY_LOW only
    if (pCreature->m_bCombatState) return false;
    if (pCreature->m_lQueuedActions.m_pcExoLinkedListInternal->m_nCount != 0) return false;

    auto *pArea = pCreature->GetArea();
    return pArea && pArea->m_nPlayersInArea == 0;
}

static Hooks::Hook s_IdleCreatureAIUpdateHook = Hooks::HookFunction(Functions::_ZN12CNWSCreature8AIUpdateEv,
    (void*)+[](CNWSCreature *pCreature) -> void
    {
        int nDivisor = GetIdleCreatureAIDivisor();

        if (nDivisor && GetIsIdleCreatureForAI(pCreature) && (s_nAIFrame + pCreature->m_idSelf) % nDivisor != 0)
            return;

        s_IdleCreatureAIUpdateHook->CallOriginal<void>(pCreature);
    }, Hooks::Order::Earliest);

// ---------------------------------------------------------------------------
// Item AI throttling
// ---------------------------------------------------------------------------
//
// CNWSItem::AIUpdate is nothing but UpdateEffectList for an item that has
// applied effects (decompilation: it returns at once otherwise). Items with no
// effects are already kept out of the lists by TRIM_AI_ITEMS; the ~3,900 in
// this module that do carry effects were still walking their effect lists
// every frame, the largest remaining per-object cost after the creature
// throttle. Visiting them every Nth frame only makes an expiring effect on an
// item late by at most N frames. Switch: NWNX_RISENHOLM_ITEM_AI_DIVISOR
// (integer, default 0 = off).
static int GetItemAIDivisor()
{
    static const int s_nDivisor = []() -> int
    {
        int n = Config::Get<int>("ITEM_AI_DIVISOR", 0);
        LOG_INFO("Item AI throttling: %s (divisor %d)", n > 1 ? "on" : "off", n);
        return n > 1 ? n : 0;
    }();

    return s_nDivisor;
}

static const bool s_bItemAILogged = (GetItemAIDivisor(), true);

static Hooks::Hook s_ItemAIUpdateHook = Hooks::HookFunction(Functions::_ZN8CNWSItem8AIUpdateEv,
    (void*)+[](CNWSItem *pItem) -> void
    {
        int nDivisor = GetItemAIDivisor();

        if (nDivisor && (s_nAIFrame + pItem->m_idSelf) % nDivisor != 0)
            return;

        s_ItemAIUpdateHook->CallOriginal<void>(pItem);
    }, Hooks::Order::Earliest);



// ---------------------------------------------------------------------------
// Native effect-list queries
// ---------------------------------------------------------------------------
//
// pw_inc_effect walks an object's effect list from NWScript for questions the
// base game has no command for: "is there an effect with this tag", "is there
// one whose string parameter N is X", "remove every effect with this tag".
// Each step of such a walk is two VM calls (GetNextEffect plus the accessor),
// a few microseconds a piece, and a buffed character carries dozens of
// effects, so a single question costs on the order of 100 us and the attack
// and damage handlers ask several per hit. The same loop in C++ over
// m_appliedEffects is a microsecond. Semantics match the script versions:
// tags compare exactly, and removal goes through CNWSObject::RemoveEffectById
// with the ids collected first, which is what the RemoveEffect command does.

static CNWSObject *ExtractNWSObject(ArgumentStack &args)
{
    return Utils::AsNWSObject(Utils::GetGameObject(args.extract<ObjectID>()));
}

NWNX_EXPORT ArgumentStack GetHasEffectByTag(ArgumentStack&& args)
{
    auto *pObject = ExtractNWSObject(args);
    const auto sTag = args.extract<std::string>();

    if (!pObject) return 0;

    const CExoString cTag(sTag.c_str());
    auto &effects = pObject->m_appliedEffects;

    for (int32_t i = 0; i < effects.num; i++)
    {
        if (effects.element[i] && effects.element[i]->m_sCustomTag == cTag)
            return 1;
    }

    return 0;
}

NWNX_EXPORT ArgumentStack GetHasEffectWithStringParam(ArgumentStack&& args)
{
    auto *pObject = ExtractNWSObject(args);
    const auto nIndex = args.extract<int32_t>();
    const auto sValue = args.extract<std::string>();

    if (!pObject || nIndex < 0 || nIndex > 5) return 0;

    const CExoString cValue(sValue.c_str());
    auto &effects = pObject->m_appliedEffects;

    for (int32_t i = 0; i < effects.num; i++)
    {
        if (effects.element[i] && effects.element[i]->m_sParamString[nIndex] == cValue)
            return 1;
    }

    return 0;
}

NWNX_EXPORT ArgumentStack RemoveEffectsByTag(ArgumentStack&& args)
{
    auto *pObject = ExtractNWSObject(args);
    const auto sTag = args.extract<std::string>();

    if (!pObject) return 0;

    const CExoString cTag(sTag.c_str());
    std::vector<uint64_t> aIds;
    auto &effects = pObject->m_appliedEffects;

    for (int32_t i = 0; i < effects.num; i++)
    {
        if (effects.element[i] && effects.element[i]->m_sCustomTag == cTag)
            aIds.push_back(effects.element[i]->m_nID);
    }

    int32_t nRemoved = 0;

    for (uint64_t nId : aIds)
    {
        if (pObject->RemoveEffectById(nId))
            nRemoved++;
    }

    return nRemoved;
}

// One pass over every AI list, removing idle static placeables and effect-free
// items that got in by a route the hooks do not cover. Returns the number
// removed. Safe to call repeatedly; a no-op when every switch is off.
NWNX_EXPORT ArgumentStack TrimAILists(ArgumentStack&&)
{
    int32_t nRemoved = 0;

    if (!TrimAIListsEnabled())
        return nRemoved;

    auto *pAIMaster = Globals::AppManager()->m_pServerExoApp->GetServerAIMaster();

    for (int32_t nLevel = 0; nLevel <= 4; nLevel++)
    {
        // Copy first: RemoveObject edits the array being walked.
        std::vector<ObjectID> aObjects;
        auto &list = pAIMaster->m_apGameAIList[nLevel].m_aoGameObjects;
        for (int32_t i = 0; i < list.num; i++)
            aObjects.push_back(list.element[i]);

        for (ObjectID oid : aObjects)
        {
            auto *pObject = Utils::AsNWSObject(Utils::GetGameObject(oid));
            if (!pObject) continue;

            if (GetIsTrimmable(pObject))
            {
                if (pAIMaster->RemoveObject(pObject))
                    nRemoved++;
            }
        }
    }

    // The size left behind is the number the engine will visit every frame
    // from here on. Logged so a restart where trimming silently did not take
    // (seen once on 2026-09-21 and not reproduced since) shows up in the log
    // as an unexpectedly large figure instead of only as lag.
    int32_t nRemaining = 0;
    for (int32_t nLevel = 0; nLevel <= 4; nLevel++)
        nRemaining += pAIMaster->m_apGameAIList[nLevel].m_aoGameObjects.num;

    LOG_INFO("TrimAILists removed %d objects from the AI update lists; %d remain", nRemoved, nRemaining);

    return nRemoved;
}



static Hooks::Hook s_GetFlatFootedHook = Hooks::HookFunction(&CNWSCreature::GetFlatFooted,
    +[](CNWSCreature *pCreature) -> int32_t
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
            (!IsAIState(Constants::AIState::CanUseLegs) && !IsAIState(Constants::AIState::CanUseHands)) ||
            (pCreature->m_nAnimation == Constants::Animation::KnockdownFront || pCreature->m_nAnimation == Constants::Animation::KnockdownButt))
            return true;

        return false;
    }, Hooks::Order::Final);

static void ResolvePlaceableSneakAndDeathAttack(CNWSCreature *pThis, CNWSPlaceable *pPlaceable)
{
    static const float SNEAK_ATTACK_DISTANCE =
            std::pow(Globals::Rules()->GetRulesetFloatEntry(CRULES_HASHEDSTR("MAX_RANGED_SNEAK_ATTACK_DISTANCE"), 10.0f), 2);

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
static Hooks::Hook s_ResolveAttackRollHook = Hooks::HookFunction(&CNWSCreature::ResolveAttackRoll,
    +[](CNWSCreature *pThis, CNWSObject *pTarget) -> void
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
                    static int32_t nParryRiposteDifference = Globals::Rules()->GetRulesetIntEntry(CRULES_HASHEDSTR("PARRY_RIPOSTE_DIFFERENCE"), 10);
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


static Hooks::Hook s_DoCombatStepHook = Hooks::HookFunction(&CNWSCreature::DoCombatStep,
    +[](CNWSCreature *pThis, uint8_t nStepType, int32_t nAnimationTime, ObjectID oidTarget) -> void
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
        if (!pTarget || !pThis->GetArea() || !IsAIState(Constants::AIState::CanUseLegs))
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


static Hooks::Hook s_ResolveAmmunitionHook = Hooks::HookFunction(&CNWSCreature::ResolveAmmunition,
    +[](CNWSCreature *pCreature, uint32_t nTimeIndex) -> void
    {
        if (auto *pItem = pCreature->m_pInventory->GetItemInSlot(Constants::EquipmentSlot::RightHand))
        {
            if (pItem->m_nBaseItem == Constants::BaseItem::Longbow ||
                pItem->m_nBaseItem == Constants::BaseItem::Shortbow ||
                pItem->m_nBaseItem == Constants::BaseItem::HeavyCrossbow ||
                pItem->m_nBaseItem == Constants::BaseItem::LightCrossbow ||
                pItem->m_nBaseItem == Constants::BaseItem::Sling)
            {
                if (!pItem->GetPropertyByTypeExists(Constants::ItemProperty::UnlimitedAmmunition))
                {
                    auto GetAmmoItem = [&]() -> CNWSItem*
                    {
                        switch (pItem->m_nBaseItem)
                        {
                            case Constants::BaseItem::Longbow:
                            case Constants::BaseItem::Shortbow:
                                return pCreature->m_pInventory->GetItemInSlot(Constants::EquipmentSlot::Arrows);

                            case Constants::BaseItem::HeavyCrossbow:
                            case Constants::BaseItem::LightCrossbow:
                                return pCreature->m_pInventory->GetItemInSlot(Constants::EquipmentSlot::Bolts);

                            case Constants::BaseItem::Sling:
                                return pCreature->m_pInventory->GetItemInSlot(Constants::EquipmentSlot::Bullets);

                            default:
                                return nullptr;
                        }
                    };

                    if (auto *pAmmoItem = GetAmmoItem())
                    {
                        CServerAIMaster *pServerAIMaster = Globals::AppManager()->m_pServerExoApp->GetServerAIMaster();
                        pServerAIMaster->AddEventDeltaTime(0, nTimeIndex, pCreature->m_idSelf,
                                                           pAmmoItem->m_idSelf,Constants::AIMasterEvent::DecrementStackSize);
                    }
                }
            }
        }
    }, Hooks::Order::Final);


static Hooks::Hook s_ResolvePostRangedDamageHook = Hooks::HookFunction(&CNWSCreature::ResolvePostRangedDamage,
    +[](CNWSCreature *pThis, CNWSObject *pTarget) -> void
    {
        if (!pTarget)
            return;

        CNWSCombatRound *pCombatRound = pThis->m_pcCombatRound;
        CNWSCombatAttackData *pAttackData = pCombatRound->GetAttack(pCombatRound->m_nCurrentAttack);
        int32_t nTotalDamage = pAttackData->GetTotalDamage(true);

        if (!pThis->GetAttackResultHit(pAttackData))
            return;

        if (auto *pCreature = Utils::AsNWSCreature(pTarget))
        {
            if (nTotalDamage >= pCreature->m_nCurrentHitPoints || pAttackData->m_bCoupDeGrace)
            {
                if (!pCreature->m_bIsImmortal && !pCreature->m_bPlotObject)
                    pAttackData->m_bKillingBlow = true;
            }

            static int32_t nMaxRangedCoupDeGraceSquared = std::pow(Globals::Rules()->GetRulesetIntEntry(CRULES_HASHEDSTR("MAX_RANGED_COUP_DE_GRACE"), 10), 2);
            Vector v = pThis->m_vPosition;
            v.x -= pTarget->m_vPosition.x;
            v.y -= pTarget->m_vPosition.y;
            v.z -= pTarget->m_vPosition.z;
            float fMagnitudeSquared = v.x * v.x + v.y * v.y + v.z * v.z;

            if (pAttackData->m_bCoupDeGrace && fMagnitudeSquared <= nMaxRangedCoupDeGraceSquared)
            {
                auto *pEffect = new CGameEffect(true);
                pEffect->m_nType = Constants::EffectTrueType::Death;
                pEffect->m_nSubType = (pEffect->m_nSubType & ~0x7) | Constants::EffectDurationType::Instant;
                pEffect->m_oidCreator = pThis->m_idSelf;
                pEffect->SetInteger(0, false);
                pEffect->SetInteger(1, true);

                pAttackData->m_alstOnHitGameEffects.Add(pEffect);
            }

            // *****
            // NOTE: Devastating Critical stuff would go here.
            // *****

            // RISENHOLM MODIFICATION: Allow Ranged Cleave
            if (pAttackData->m_bKillingBlow &&
                pAttackData->m_nAttackType != Constants::Feat::WhirlwindAttack &&
                pAttackData->m_nAttackType != Constants::Feat::ImprovedWhirlwind &&
                pCombatRound->GetTotalAttacks() < 50)
            {
                if (pThis->m_pStats->HasFeat(Constants::Feat::GreatCleave))
                {
                    if (auto *pNewTarget = pThis->GetNewCombatTarget(pTarget->m_idSelf))
                    {
                        pCombatRound->m_oidNewAttackTarget = pNewTarget->m_idSelf;
                        pCombatRound->AddCleaveAttack(pNewTarget->m_idSelf, true);
                        pThis->m_bPassiveAttackBehaviour = true;
                    }
                }
                else if ((pThis->m_pStats->HasFeat(Constants::Feat::Cleave) && pCombatRound->m_nCleaveAttacks > 0))
                {
                    if (auto *pNewTarget = pThis->GetNewCombatTarget(pTarget->m_idSelf))
                    {
                        pCombatRound->m_oidNewAttackTarget = pNewTarget->m_idSelf;
                        pCombatRound->AddCleaveAttack(pNewTarget->m_idSelf);
                        pThis->m_bPassiveAttackBehaviour = true;
                        pCombatRound->m_nCleaveAttacks--;
                    }
                }
            }
            // END RISENHOLM MODIFICATION
        }
        else
        {
            if (nTotalDamage >= pTarget->m_nCurrentHitPoints && !pTarget->m_bPlotObject)
                pAttackData->m_bKillingBlow = true;
        }

        if (nTotalDamage <= 0)
        {
            if (!pCombatRound->m_bWeaponSucks)
            {
                if (!pThis->GetIsWeaponEffective(pTarget->m_idSelf, pAttackData->m_nWeaponAttackType == 2))
                {
                    auto *pMessageData = new CNWCCMessageData;
                    pMessageData->m_nType = 3;
                    pMessageData->SetInteger(0, 117);
                    pAttackData->m_alstPendingFeedback.Add(pMessageData);

                    pCombatRound->m_bWeaponSucks = true;
                }
            }
        }
    }, Hooks::Order::Final);


static Hooks::Hook s_AIActionCastSpellHook = Hooks::HookFunction(&CNWSCreature::AIActionCastSpell,
    +[](CNWSCreature* thisPtr, CNWSObjectActionNode *pNode) -> uint32_t
    {
        BOOL bHasted = thisPtr->m_bHasted;

        thisPtr->m_bHasted = false;
        auto retVal = s_AIActionCastSpellHook->CallOriginal<uint32_t>(thisPtr, pNode);
        thisPtr->m_bHasted = bHasted;

        return retVal;
    }, Hooks::Order::Early);

// RISENHOLM MODIFICATION: Slow no longer reduces attacks per round.
//
// This replaces an older GetTotalAttacks hook (0d69b83662, "Risenholm: Slow
// halves amount of attacks", 2021-02-19) that summed the four attack-count
// members and halved the total when m_bSlowed. That hook NEVER DID ANYTHING,
// for five years: the engine has already clamped the count by the time it runs,
// so the sum was 1, and 1/2 == 0 floored straight back to 1 by the std::max(1)
// guard directly beneath it. Deleted rather than left commented out -- vanilla
// GetTotalAttacks (disassembled at 0x6500a0) is
// max(1, onHand + offHand + additional + bonusEffect), i.e. exactly what that
// hook did minus the dead halving, so removing it changes no behaviour.
//
// The real reduction is here, in InitializeNumberOfAttacks (disassembled at
// 0x65010d). Slow does not halve or decrement -- it OVERWRITES the count:
//
//     if (creature->m_bSlowed)
//         m_nOnHandAttacks = GetRulesetIntEntry("SLOWED_ATTACKS", 1);
//
// The ruleset label was recovered by hashing every ruleset.2da entry against
// the FNV-1a constant in the disassembly; ruleset.2da ships SLOWED_ATTACKS = 1.
// That is why the spell description ("lose a single attack per round") never
// matched what players saw, and why patching GetTotalAttacks accomplished
// nothing at all.
//
// Rather than reimplement a function whose body we cannot read, hide the flag
// from the original -- the same trick the AIActionCastSpell hook above uses on
// m_bHasted. This neutralises EVERY slow-based attack reduction inside the
// function, including branches nobody has disassembled, and leaves the other
// slow penalties (-2 AB/AC/reflex, halved movement) alone since those are
// applied elsewhere. Verified in game against Loew, Father of Clay, whose 5 APR
// collapsed to 1 while slowed under the old code.
//
// Editing SLOWED_ATTACKS in ruleset.2da is NOT an alternative: it assigns a
// fixed count, so it can express "slow leaves you 2 attacks" but never "slow
// changes nothing".
static Hooks::Hook s_InitializeNumberOfAttacksHook = Hooks::HookFunction(&CNWSCombatRound::InitializeNumberOfAttacks,
    +[](CNWSCombatRound* thisPtr) -> void
    {
        auto *pCreature = thisPtr->m_pBaseCreature;

        if (!pCreature)
        {
            s_InitializeNumberOfAttacksHook->CallOriginal<void>(thisPtr);
            return;
        }

        const BOOL bSlowed = pCreature->m_bSlowed;
        pCreature->m_bSlowed = false;
        s_InitializeNumberOfAttacksHook->CallOriginal<void>(thisPtr);
        pCreature->m_bSlowed = bSlowed;
    }, Hooks::Order::Early);
// END RISENHOLM MODIFICATION


static Hooks::Hook s_GetDEXModHook = Hooks::HookFunction(&CNWSCreatureStats::GetDEXMod,
    +[](CNWSCreatureStats* thisPtr, BOOL bUseArmourPenalty) -> char
    {
        int32_t nMaxDexMod = 0;

        if (bUseArmourPenalty)
        {
            static CExoString sVarName = "MAGE_ARMOR";
            auto *pScriptVarTable = Utils::GetScriptVarTable(thisPtr->m_pBaseCreature);

            if (auto *pChestItem = thisPtr->m_pBaseCreature->m_pInventory->GetItemInSlot(Constants::EquipmentSlot::Chest))
            {
                int32_t nArmorClass = pChestItem->ComputeArmorClass();
                Globals::Rules()->m_p2DArrays->GetArmorTable()->GetINTEntry(nArmorClass, "DEXBONUS", &nMaxDexMod);

                // RISENHOLM MODIFICATION: If creature has Mage Armor, set max dex bonus limit to 5
                if (pScriptVarTable && pScriptVarTable->GetInt(sVarName))
                {
                    nMaxDexMod = std::min(5, nMaxDexMod);
                }
                // END RISENHOLM MODIFICATION
            }
            else
            {
                // RISENHOLM MODIFICATION: If creature has Mage Armor, set max dex bonus limit to 5
                if (pScriptVarTable && pScriptVarTable->GetInt(sVarName))
                    nMaxDexMod = 5;
                else
                    nMaxDexMod = 10;
                // END RISENHOLM MODIFICATION
            }
        }

        if (nMaxDexMod > 0)
        {
            // RISENHOLM MODIFICATION: Add highest mental ability mod to dex mod if creature has the Strategic Defense feat
            // Or Constitution mod with Robust Defense
            char nDexMod = thisPtr->m_nDexterityModifier;
            if (thisPtr->HasFeat(1237/*Strategic Defense*/))
                nDexMod += std::max({thisPtr->m_nWisdomModifier, thisPtr->m_nIntelligenceModifier, thisPtr->m_nCharismaModifier});
            if (thisPtr->HasFeat(1376/*Robust Defense*/))
                nDexMod += thisPtr->m_nConstitutionModifier;
            // END RISENHOLM MODIFICATION

            return std::min(nDexMod, (char)nMaxDexMod);
        }
        else
            return thisPtr->m_nDexterityModifier;
    }, Hooks::Order::Final);
static Hooks::Hook s_ItemComputeArmorClassHook = Hooks::HookFunction(&CNWSItem::ComputeArmorClass,
    +[](CNWSItem *thisPtr) -> int32_t
    {
        if (Globals::Rules()->m_pBaseItemArray->GetBaseItem(thisPtr->m_nBaseItem)->m_nModelType != 3)
        {
            switch (thisPtr->m_nBaseItem)
            {
                case Constants::BaseItem::SmallShield:
                case Constants::BaseItem::LargeShield:
                case Constants::BaseItem::TowerShield:
                    return 2;

                default:
                    return 0;
            }
        }

        float fACBonus;
        Globals::Rules()->m_p2DArrays->GetPartsChest()->GetFLOATEntry(thisPtr->m_nArmorModelPart[7], "ACBonus", &fACBonus);

        // RISENHOLM MODIFICATION: If creature has Mage Armor, return 5 if ACBonus is lower than 5
        if (auto *pCreature = Utils::AsNWSCreature(Utils::GetGameObject(thisPtr->m_oidPossessor)))
        {
            static CExoString sVarName = "MAGE_ARMOR";
            auto *pScriptVarTable = Utils::GetScriptVarTable(pCreature);
            if (pScriptVarTable && pScriptVarTable->GetInt(sVarName))
            {
                return std::max(5, (int32_t)fACBonus);
            }
        }
        // END RISENHOLM MODIFICATION

        return (int32_t)fACBonus;
    }, Hooks::Order::Final);
static Hooks::Hook s_CreatureComputeArmourClassHook = Hooks::HookFunction(&CNWSCreature::ComputeArmourClass,
    +[](CNWSCreature *thisPtr, CNWSItem *pItemToEquip, BOOL, BOOL) -> void
    {
        bool bSendFeedbackMessage = false;
        auto *pInventory = thisPtr->m_pInventory;
        auto *pArmorTable = Globals::Rules()->m_p2DArrays->GetArmorTable();
        auto *pStats = thisPtr->m_pStats;

        if (auto *pChestItem = pInventory->GetItemInSlot(Constants::EquipmentSlot::Chest))
        {
            if (pChestItem == pItemToEquip)
            {
                int32_t nACArmor = pItemToEquip->ComputeArmorClass();
                int32_t nArcaneSpellFailure = 0;
                int32_t nArmorCheckPenalty = 0;

                pArmorTable->GetINTEntry(nACArmor, "ARCANEFAILURE%", &nArcaneSpellFailure);
                pArmorTable->GetINTEntry(nACArmor, "ACCHECK", &nArmorCheckPenalty);

                // NOTE: Monk/Ranger Too High Armor AC Warning Messages would go here.

                pStats->m_nBaseArmorArcaneSpellFailure = nArcaneSpellFailure;
                pStats->m_nArmorCheckPenalty = nArmorCheckPenalty;
                pStats->m_nACArmorBase = nACArmor;

                bSendFeedbackMessage = true;
            }
        }
        else
        {
            // RISENHOLM MODIFICATION: Do Mage Armor stuff when creature is not wearing chest armor
            static CExoString sVarName = "MAGE_ARMOR";
            auto *pScriptVarTable = Utils::GetScriptVarTable(thisPtr);
            if (pScriptVarTable && pScriptVarTable->GetInt(sVarName))
            {
                int32_t nACArmor = 5;
                int32_t nArcaneSpellFailure = 0;
                int32_t nArmorCheckPenalty = 0;

                pArmorTable->GetINTEntry(nACArmor, "ARCANEFAILURE%", &nArcaneSpellFailure);
                pArmorTable->GetINTEntry(nACArmor, "ACCHECK", &nArmorCheckPenalty);

                pStats->m_nBaseArmorArcaneSpellFailure = nArcaneSpellFailure;
                pStats->m_nArmorCheckPenalty = nArmorCheckPenalty;
                pStats->m_nACArmorBase = nACArmor;
            }
            else
            {
                pStats->m_nBaseArmorArcaneSpellFailure = 0;
                pStats->m_nArmorCheckPenalty = 0;
                pStats->m_nACArmorBase = 0;
            }

            if (!pItemToEquip || pItemToEquip->m_nBaseItem == Constants::BaseItem::Armor)
                bSendFeedbackMessage = true;
            // END RISENHOLM MODIFICATION
        }

        auto IsShield = [](CNWSItem *pItem) -> bool
        {
            return pItem->m_nBaseItem == Constants::BaseItem::SmallShield ||
                   pItem->m_nBaseItem == Constants::BaseItem::LargeShield ||
                   pItem->m_nBaseItem == Constants::BaseItem::TowerShield;
        };

        if (auto *pShieldItem = pInventory->GetItemInSlot(Constants::EquipmentSlot::LeftHand))
        {
            if (pShieldItem == pItemToEquip && IsShield(pShieldItem))
            {
                CNWBaseItem *pBaseItem = Globals::Rules()->m_pBaseItemArray->GetBaseItem(pShieldItem->m_nBaseItem);

                pStats->m_nBaseShieldArcaneSpellFailure = pBaseItem->m_nArcaneSpellFailure;
                pStats->m_nShieldCheckPenalty = pBaseItem->m_nArmorCheckPenalty;
                pStats->m_nACShieldBase = pItemToEquip->ComputeArmorClass();

                bSendFeedbackMessage = true;
            }
        }
        else if (pItemToEquip && IsShield(pItemToEquip))
        {
            pStats->m_nBaseShieldArcaneSpellFailure = 0;
            pStats->m_nShieldCheckPenalty = 0;
            pStats->m_nACShieldBase = 0;

            bSendFeedbackMessage = true;
        }

        if (bSendFeedbackMessage)
        {
            int32_t nTotalArcaneSpellFailure = std::max(0, std::min(100, (char)(pStats->m_nBaseArmorArcaneSpellFailure +
                                               pStats->m_nBaseShieldArcaneSpellFailure) + pStats->m_nArcaneSpellFailure));
            int32_t nTotalArmorCheckPenalty = pStats->m_nShieldCheckPenalty + pStats->m_nArmorCheckPenalty;

            auto *pMessageData = new CNWCCMessageData;
            pMessageData->SetInteger(0, nTotalArcaneSpellFailure);
            pMessageData->SetInteger(1, nTotalArmorCheckPenalty);

            thisPtr->SendFeedbackMessage(71, pMessageData);
        }
    }, Hooks::Order::Final);

static Hooks::Hook s_AddItemCastSpellActionsHook = Hooks::HookFunction(&CNWSCreature::AddItemCastSpellActions,
    +[](CNWSCreature *thisPtr, ObjectID oidItemUsed, int32_t nActivePropertyIndex, int32_t nSubPropertyIndex,
            Vector vTargetLocation, ObjectID oidTarget, int32_t bAreaTarget, int32_t bDecrementCharges) -> int32_t
    {
        if (auto *pItem = Utils::AsNWSItem(Utils::GetGameObject(oidItemUsed)))
        {
            s_AddItemCastSpellGrenadeAction = pItem->m_nBaseItem == Constants::BaseItem::Grenade;
            auto retVal = s_AddItemCastSpellActionsHook->CallOriginal<int32_t>(thisPtr, oidItemUsed, nActivePropertyIndex,
                                                                               nSubPropertyIndex, vTargetLocation, oidTarget, bAreaTarget, bDecrementCharges);
            s_AddItemCastSpellGrenadeAction = false;

            return retVal;
        }

        return false;
    }, Hooks::Order::Early);
static Hooks::Hook s_AddActionHook = Hooks::HookFunction(&CNWSObject::AddAction,
    +[](CNWSObject *thisPtr, uint32_t nActionId, uint16_t nGroupId, uint32_t nParamType1, void *pParameter1, uint32_t nParamType2, void *pParameter2,
               uint32_t nParamType3, void *pParameter3, uint32_t nParamType4, void *pParameter4, uint32_t nParamType5, void *pParameter5, uint32_t nParamType6, void *pParameter6,
               uint32_t nParamType7, void *pParameter7, uint32_t nParamType8, void *pParameter8, uint32_t nParamType9, void *pParameter9, uint32_t nParamType10, void *pParameter10,
               uint32_t nParamType11, void *pParameter11, uint32_t nParamType12, void *pParameter12) -> void
    {
        // AI update list trimming (see the block near the top of this file):
        // a trimmed placeable that is handed an action has to be back in the
        // list for the action to ever run.
        if (TrimAIListsEnabled())
            RestoreToAIList(thisPtr);

        if (s_AddItemCastSpellGrenadeAction && nActionId == 16)
        {
            float fDuration = 0.25f;
            s_AddActionHook->CallOriginal<void>(thisPtr, 30, nGroupId, 2, (float*)&fDuration,
                                                0, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, nullptr,
                                                0, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, nullptr);
        }
        else
        {
            s_AddActionHook->CallOriginal<void>(thisPtr, nActionId, nGroupId,
                                                nParamType1, pParameter1, nParamType2, pParameter2, nParamType3, pParameter3,
                                                nParamType4, pParameter4, nParamType5, pParameter5, nParamType6, pParameter6,
                                                nParamType7, pParameter7, nParamType8, pParameter8, nParamType9, pParameter9,
                                                nParamType10, pParameter10, nParamType11, pParameter11, nParamType12, pParameter12);
        }
    }, Hooks::Order::Late);

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

NWNX_EXPORT ArgumentStack ForceUpdateMageArmorStats(ArgumentStack&& args)
{
    auto oidCreature = args.extract<ObjectID>();

    if (auto *pCreature = Globals::AppManager()->m_pServerExoApp->GetCreatureByGameObjectID(oidCreature))
    {
        auto *pChestItem = pCreature->m_pInventory->GetItemInSlot(Constants::EquipmentSlot::Chest);
        pCreature->ComputeArmourClass(pChestItem, true);
    }

    return {};
}

// A creature whose appearance id is swapped wholesale (Risenholm wildshape, dodgeroll,
// DM appearance tools) renders naked to any player who entered the area DURING the swap.
// The engine only ever sends an appearance *delta*: it diffs live state against that
// player's CLastUpdateObject, and theirs cached the animal form -- empty armour part
// slots. The equipped items never changed, so flipping the appearance id back matches on
// equipment and only the appearance id is re-sent, leaving the humanoid model wearing the
// animal's (empty) parts. Players present before the swap are unaffected because their
// cache holds the correct humanoid part data from before.
//
// Wiping the cached appearance block makes ComputeAppearanceUpdateRequired flag every
// field dirty, so the next WriteGameObjUpdate_UpdateAppearance re-sends the whole thing.
// NWNX_Item_SetItemAppearance already relies on this exact trick, but only invalidates the
// three armour item oids; a wholesale appearance swap needs the part variations, phenotype,
// and colours too, hence the full Clear().
NWNX_EXPORT ArgumentStack ForceAppearanceUpdate(ArgumentStack&& args)
{
    auto oidCreature      = args.extract<ObjectID>();
    auto bFullObjectUpdate = args.extract<int32_t>();

    if (oidCreature == Constants::OBJECT_INVALID)
        return {};

    auto *pMessage = Globals::AppManager()->m_pServerExoApp->GetNWSMessage();

    for (auto *pPlayer : Globals::AppManager()->m_pServerExoApp->GetPlayerList())
    {
        if (bFullObjectUpdate)
        {
            // Hammer: drop the cached record entirely, so the next tick takes the
            // CreateNewLastUpdateObject path and re-sends the object exactly as a client
            // walking into the area would receive it. Heavier -- it also re-sends position,
            // hit points, action queue and effect icons -- so only for cases the appearance
            // block alone does not cover.
            pMessage->DeleteLastUpdateObjectsForObject(pPlayer, oidCreature);
        }
        else if (auto *pLUO = pPlayer->GetLastUpdateObject(oidCreature))
        {
            pLUO->m_cAppearance.Clear();
            // Clear() parks m_nAppearanceType at 0, which is a real appearances.2da row --
            // a creature actually using row 0 would compare equal and get no update. Force
            // an impossible value so the appearance-type bit is always dirty.
            pLUO->m_cAppearance.m_nAppearanceType = 0xFFFF;
        }
    }

    return {};
}

NWNX_EXPORT ArgumentStack ExecuteCommand(ArgumentStack&& args)
{
    auto cmdPath = args.extract<std::string>();
    std::vector<std::string> cmdArgList;

    for (int i = 0; i < 6; i++)
    {
        auto cmdArg = args.extract<std::string>();

        if (cmdArg == "") break;

        cmdArgList.push_back(cmdArg);
    }

    std::stringstream input;

    try
    {
        subprocess::popen cmd(cmdPath, cmdArgList);
        input << cmd.stdout().rdbuf();
    }
    catch (const std::exception& err)
    {
        LOG_ERROR("Plugin 'Risenholm' failed popen. Error: %s", err.what());
    }

    return input.str();
}

NWNX_EXPORT ArgumentStack StartLevelUp(ArgumentStack&& args)
{
    if (auto *pPlayer = Utils::PopPlayer(args))
    {
        auto *pMessage = Globals::AppManager()->m_pServerExoApp->GetNWSMessage();
        pMessage->SendServerToPlayerLevelUp_Begin(pPlayer->m_nPlayerID, Utils::AsNWSCreature(pPlayer->GetGameObject()));
    }
    return {};
}

NWNX_EXPORT ArgumentStack CheckForShutdownFile(ArgumentStack&& args)
{
    std::string filePath = "/nwn/home/shutdown.txt";
    std::ifstream file(filePath);
    if (file.good()) {
        file.close();
        if (std::remove(filePath.c_str()) != 0) {
            LOG_ERROR("Error deleting file: %s", filePath);
        }
        return 1;
    }
    return 0;
}

}

NWNX_EXPORT ArgumentStack FixItemDestroySkipUseableState(ArgumentStack&& args)
{
    if (auto *pItem = Utils::PopItem(args))
    {
        pItem->RestoreUsedActiveProperties(false);
        pItem->UpdateUsedActiveProperties();
    }
    return {};
}

NWNX_EXPORT ArgumentStack AddAttackOfOpportunity(ArgumentStack&& args)
{
	if (auto *pCreature = Utils::PopCreature(args))
	{
		auto *pCombatRound = pCreature->m_pcCombatRound;
		if (pCombatRound->GetTotalAttacks() < 50)
		{
			auto oidTarget = args.extract<ObjectID>();
			if (oidTarget != Constants::OBJECT_INVALID)
				pCombatRound->AddAttackOfOpportunity(oidTarget); 
		}
	}

    return {};
} 

NWNX_EXPORT ArgumentStack ForceExamineWindow(ArgumentStack&& args)
{
    if (auto* pPlayer = Utils::PopPlayer(args))
    {
        const auto oidObject = args.extract<ObjectID>();
        if (auto* pGameObject = Utils::GetGameObject(oidObject))
        {
            auto* pMessage = Globals::AppManager()->m_pServerExoApp->GetNWSMessage();

            switch (pGameObject->m_nObjectType)
            {
                case Constants::ObjectType::Creature:
                    pMessage->SendServerToPlayerExamineGui_CreatureData(pPlayer, oidObject);
                break;

                case Constants::ObjectType::Item:
                    if (auto* pCreature = Utils::AsNWSCreature(pPlayer->GetGameObject()))
                        pCreature->UseLoreOnItem(oidObject);
                    pMessage->SendServerToPlayerExamineGui_ItemData(pPlayer, oidObject);
                break;

                case Constants::ObjectType::Placeable:
                    pMessage->SendServerToPlayerExamineGui_PlaceableData(pPlayer, oidObject);
                break;

                case Constants::ObjectType::Trigger:
                    if (auto* pCreature = Utils::AsNWSCreature(pPlayer->GetGameObject()))
                        pCreature->UseSkill(2, 102, oidObject, Vector(), pCreature->m_oidArea);
                break;

                case Constants::ObjectType::Door:
                    pMessage->SendServerToPlayerExamineGui_DoorData(pPlayer, oidObject);
                break;
            }
        }
    }

    return {};
}

static Hooks::Hook s_GetSkillRankHook = Hooks::HookFunction(&CNWSCreatureStats::GetSkillRank,
    +[](CNWSCreatureStats* pThis, uint8_t nSkill, CNWSObject* pVersus, BOOL bBaseOnly) -> char
    {
        if (nSkill == Constants::Skill::OpenLock ||
            nSkill == Constants::Skill::SetTrap ||
            nSkill == Constants::Skill::DisableTrap)
        {
            nSkill = 22; // Tinkering
        }
        return s_GetSkillRankHook->CallOriginal<BOOL>(pThis, nSkill, pVersus, bBaseOnly);
    }, Hooks::Order::VeryEarly);
	


static Hooks::Hook s_GetCanUseSkillHook = Hooks::HookFunction(&CNWSCreatureStats::GetCanUseSkill,
    +[](CNWSCreatureStats* pThis, uint8_t nSkill) -> BOOL
    {
        uint8_t nTempSkill = nSkill;
        if (nTempSkill == 100 || nTempSkill == 101 || nTempSkill == 102)
            nTempSkill = Constants::Skill::DisableTrap;
        if (nTempSkill == Constants::Skill::OpenLock ||
            nTempSkill == Constants::Skill::SetTrap ||
            nTempSkill == Constants::Skill::DisableTrap)
        {
            return true;
        }
        return s_GetCanUseSkillHook->CallOriginal<BOOL>(pThis, nSkill);
    }, Hooks::Order::VeryEarly);


// ---------------------------------------------------------------------------
// Afterimage clones
// ---------------------------------------------------------------------------
//
// Native replacement for the ObjectToJson -> GffReplace* -> JsonToObject path
// that pw_inc_attack's CreateAfterimageClonesToAttackTarget used to run for
// every Wave Crash and afterimage flurry (558 ms per run on production,
// 2026-09-21). The clone MUST carry the source's object state -- effects,
// action queue, combat state -- or it does not pick up the attack animation
// fluidly, so this serialises exactly the way NWNX's own serialiser does
// (SaveObjectState, then SaveCreature) and only takes three things out of
// the picture for the duration of the save:
//
//  * the backpack: the repository's item list is swapped for an empty one.
//    Player inventories here run to hundreds of items, and the JSON path
//    serialised all of them only to JsonObjectDel the list a moment later,
//    then paid for GFF->JSON->GFF on the rest. Equipped items live in the
//    CNWSInventory equip slots, not the repository, so the image still
//    looks like its owner;
//  * the local variables: the script replaced the VarTable wholesale, and a
//    PC's class-state locals have no business on a two-second image;
//  * the PC flags, plot, and useable bits, which the script used to patch
//    into the JSON.
//
// The GFF is then loaded straight back into a new creature (the same
// DeserializeGameObject that NWNX_Object_Deserialize uses), given full hit
// points, the requested faction, a zeroed UI discovery mask, and the two
// marker locals, and added to the area facing the way the source faces. It
// is then dressed as an afterimage: not lootable, VFX_DUR_INVISIBILITY, a
// permanent 100% miss chance, a permanent cutscene ghost (the source may or
// may not be carrying one at snapshot time, and an image must never block
// anyone), and the caller's animation speed, so the script has nothing left
// to do per clone but order the attack and the destroy.
//
// The serialisation is the expensive half and a flurry wants three clones of
// the same instant, so it is split: PrepareAfterimage serialises once into a
// one-slot cache keyed by the source, CreateAfterimage builds a clone from
// that snapshot (serialising on the spot only if the cache holds a different
// source), and ReleaseAfterimage drops it. The cache is deliberately not kept
// across calls: the image has to reflect the source's state at the moment of
// the attack, so a snapshot from an earlier Wave Crash is not good enough.
//
// Not verified here: that SaveCreature reads the backpack through
// m_pcItemRepository->m_oidItems and nothing else. Ghidra truncates that
// function, so the first in-game check after deploying this is that the
// clone has no inventory and still wears its owner's gear.

static ObjectID s_AfterimageSource = Constants::OBJECT_INVALID;
static std::vector<uint8_t> s_AfterimageData;

static std::vector<uint8_t> SerializeAfterimage(CNWSCreature *pSource)
{
    // --- detach what the image must not carry, for the duration of the save
    CExoLinkedList<OBJECT_ID> emptyItems;
    if (pSource->m_pcItemRepository)
        std::swap(pSource->m_pcItemRepository->m_oidItems.m_pcExoLinkedListInternal, emptyItems.m_pcExoLinkedListInternal);

    std::unordered_map<CExoString, CNWSScriptVar> savedVars;
    std::swap(pSource->m_ScriptVars.m_vars, savedVars);

    const BOOL bPlot = pSource->m_bPlotObject;
    const BOOL bUseable = pSource->m_bUseable;
    pSource->m_bPlotObject = true;
    pSource->m_bUseable = false;

    // bStripPCFlags: SerializeGameObject zeroes m_bPlayerCharacter and
    // m_pStats->m_bIsPC around the save, which is the JSON path's IsPC = 0.
    std::vector<uint8_t> data = Utils::SerializeGameObject(pSource, true);

    pSource->m_bPlotObject = bPlot;
    pSource->m_bUseable = bUseable;
    std::swap(pSource->m_ScriptVars.m_vars, savedVars);
    if (pSource->m_pcItemRepository)
        std::swap(pSource->m_pcItemRepository->m_oidItems.m_pcExoLinkedListInternal, emptyItems.m_pcExoLinkedListInternal);

    return data;
}

NWNX_EXPORT ArgumentStack PrepareAfterimage(ArgumentStack&& args)
{
    if (auto *pSource = Utils::PopCreature(args))
    {
        s_AfterimageData = SerializeAfterimage(pSource);
        s_AfterimageSource = s_AfterimageData.empty() ? Constants::OBJECT_INVALID : pSource->m_idSelf;
        return (int32_t)s_AfterimageData.size();
    }

    return 0;
}

NWNX_EXPORT ArgumentStack ReleaseAfterimage(ArgumentStack&&)
{
    s_AfterimageSource = Constants::OBJECT_INVALID;
    std::vector<uint8_t>().swap(s_AfterimageData);
    return {};
}

NWNX_EXPORT ArgumentStack CreateAfterimage(ArgumentStack&& args)
{
    auto *pSource = Utils::PopCreature(args);
    const auto oidArea   = args.extract<ObjectID>();
    const auto x         = args.extract<float>();
    const auto y         = args.extract<float>();
    const auto z         = args.extract<float>();
    const auto fFacing   = args.extract<float>();
    const auto nFaction  = args.extract<int32_t>();
    const auto fAnimationSpeed = args.extract<float>();

    auto *pArea = Utils::AsNWSArea(Utils::GetGameObject(oidArea));

    if (!pSource || !pArea)
        return Constants::OBJECT_INVALID;

    const bool bCached = (s_AfterimageSource == pSource->m_idSelf && !s_AfterimageData.empty());
    const std::vector<uint8_t> data = bCached ? s_AfterimageData : SerializeAfterimage(pSource);

    // --- build the clone
    auto *pClone = Utils::AsNWSCreature(Utils::DeserializeGameObject(data));

    if (!pClone)
    {
        LOG_WARNING("CreateAfterimage: could not deserialize a clone of %x (%d bytes, %s)", pSource->m_idSelf, (int)data.size(), bCached ? "cached" : "fresh");
        return Constants::OBJECT_INVALID;
    }

    pClone->m_nCurrentHitPoints = pClone->GetMaxHitPoints();
    pClone->m_nUiDiscoveryMask = 0;

    CExoString sSetPiece("IS_SET_PIECE");
    CExoString sVfx("IS_VFX");
    pClone->m_ScriptVars.SetInt(sSetPiece, 1);
    pClone->m_ScriptVars.SetInt(sVfx, 1);

    if (auto *pFaction = Globals::AppManager()->m_pServerExoApp->m_pcExoAppInternal->m_pFactionManager->GetFaction(nFaction))
        pFaction->AddMember(pClone->m_idSelf);
    else
        LOG_WARNING("CreateAfterimage: faction %d does not exist; clone keeps its source's faction", nFaction);

    Utils::AddToArea(pClone, pArea, x, y, z);

    const float fRadians = fFacing * (float)M_PI / 180.0f;
    pClone->SetOrientation(Vector{std::cos(fRadians), std::sin(fRadians), 0.0f});

    // --- dress it as an afterimage
    pClone->m_bLootable = false;

    // EffectVisualEffect(VFX_DUR_INVISIBILITY): ints are id, miss flag, and
    // a third the handler reads (OnApplyVisualEffect reads ints 0-2 and
    // float 0, the scale); subtype magical like a script-made effect.
    auto *pVfx = new CGameEffect(true);
    pVfx->m_nType = Constants::EffectTrueType::VisualEffect;
    pVfx->m_nSubType = Constants::EffectSubType::Magical | Constants::EffectDurationType::Permanent;
    pVfx->m_oidCreator = pSource->m_idSelf;
    pVfx->SetNumIntegers(3);
    pVfx->SetInteger(0, 6);   // VFX_DUR_INVISIBILITY
    pVfx->SetInteger(1, 0);
    pVfx->SetInteger(2, 0);
    pVfx->SetFloat(0, 1.0f);
    pClone->ApplyEffect(pVfx, false, true);

    // ExtraordinaryEffect(EffectMissChance(100)): int 0 is the percentage
    // (OnApplyMissChance reads only that), int 1 the MISS_CHANCE_TYPE.
    auto *pMiss = new CGameEffect(true);
    pMiss->m_nType = Constants::EffectTrueType::MissChance;
    pMiss->m_nSubType = Constants::EffectSubType::Extraordinary | Constants::EffectDurationType::Permanent;
    pMiss->m_oidCreator = pSource->m_idSelf;
    pMiss->SetNumIntegers(2);
    pMiss->SetInteger(0, 100);
    pMiss->SetInteger(1, 0);  // MISS_CHANCE_TYPE_NORMAL
    pClone->ApplyEffect(pMiss, false, true);

    // ExtraordinaryEffect(EffectCutsceneGhost()), permanent: the handler
    // reads no parameters. If the source was carrying its own temporary
    // ghost at snapshot time the clone already has a copy, and the engine
    // then drops ours as a duplicate (verified on dev 2026-09-21: the clone
    // kept only the 0.7 s copy), so any inherited ghost is removed first.
    {
        std::vector<uint64_t> aGhostIds;
        auto &effects = pClone->m_appliedEffects;

        for (int32_t i = 0; i < effects.num; i++)
        {
            if (effects.element[i] && effects.element[i]->m_nType == Constants::EffectTrueType::CutsceneGhost)
                aGhostIds.push_back(effects.element[i]->m_nID);
        }

        for (uint64_t nId : aGhostIds)
            pClone->RemoveEffectById(nId);
    }

    auto *pGhost = new CGameEffect(true);
    pGhost->m_nType = Constants::EffectTrueType::CutsceneGhost;
    pGhost->m_nSubType = Constants::EffectSubType::Extraordinary | Constants::EffectDurationType::Permanent;
    pGhost->m_oidCreator = pSource->m_idSelf;
    pClone->ApplyEffect(pGhost, false, true);

    // SetObjectVisualTransform(OBJECT_VISUAL_TRANSFORM_ANIMATION_SPEED, f) on
    // the base scope: the clone is brand new, so its first object update
    // carries whatever transform data it holds when the clients meet it.
    // 1.0 (or anything non-positive) leaves the transform untouched.
    if (fAnimationSpeed > 0.0f && fAnimationSpeed != 1.0f)
    {
        if (!pClone->m_pVisualTransformData)
            pClone->m_pVisualTransformData = new ObjectVisualTransformData();
        pClone->m_pVisualTransformData->m_scopes[0].m_animationSpeed = LerpFloat(fAnimationSpeed);
    }

    return pClone->m_idSelf;
}
