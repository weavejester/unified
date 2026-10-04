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
#include "API/CNWSModule.hpp"
#include "API/CNWSInventory.hpp"
#include "API/CNWBaseItemArray.hpp"
#include "API/CNWBaseItem.hpp"
#include "API/CServerAIMaster.hpp"
#include "API/CTwoDimArrays.hpp"
#include "API/CNWSAreaOfEffectObject.hpp"
#include "API/CNWSDoor.hpp"
#include "API/CGameEffect.hpp"
#include "API/CNWSTrigger.hpp"
#include "API/CNWSEffectListHandler.hpp"
#include "API/CNWSItemPropertyHandler.hpp"
#include "API/CNWItemProperty.hpp"
#include "External/subprocess.hpp"
#include "API/CNWSPlayer.hpp"
#include <cmath>
#include <dlfcn.h>
#include <unordered_map>
#include "API/CExoLinkedListInternal.hpp"
#include "API/CNWSScriptVar.hpp"
#include "API/CNWSScriptVarTable.hpp"
#include "API/CServerExoAppInternal.hpp"
#include "API/CNetLayer.hpp"
#include "API/CNetLayerPlayerInfo.hpp"
#include "API/CNWSFaction.hpp"
#include "API/CFactionManager.hpp"
#include <vector>
#include <unordered_set>
#include "API/CNWSMessage.hpp"
#include "API/CVirtualMachine.hpp"
#include "API/CNWSPlayerInventoryGUI.hpp"
#include "API/CExoLinkedListNode.hpp"
#include "API/CNWSObjectActionNode.hpp"
#include "API/CLastUpdateObject.hpp"
#include "API/C2DA.hpp"
#include "API/CNWLevelStats.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include "External/httplib.h"
#include "API/CExoResMan.hpp"
#include "API/CResRef.hpp"
#include <atomic>
#include <future>
#include <thread>


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
// NWNX_RISENHOLM_ON_REPOSITORY_MOVE_BEFORE/_AFTER: an item dragged into or
// within a bag
//
// BEFORE is signalled when an item is dragged into an item container, or from
// one slot of it to another, before the engine does anything, with the same
// OBJECT_SELF and ITEM as AFTER below. NWNX_Events_SkipEvent refuses the move
// the way the engine refuses one itself -- it tells the client the move is
// cancelled, which puts the item back where it was dragged from, and fails
// the action -- so nothing has been taken from anywhere yet and nothing can be
// lost. This is the safe place to keep an item out of a bag. Skipping
// NWNX_ON_INVENTORY_ADD_ITEM_BEFORE is not: CNWSItem::AcquireItem removes the
// item from where it was before it calls AddItem, and if that fails it just
// returns, leaving the item in neither place. The Scroll Case uses this to
// refuse anything that is not a scroll (pw_inc_scrlcase.nss).
//
// The cancel's BOOL says which inventory panel the client restores the item
// to: FALSE the player's own, TRUE the other-inventory panel of whatever else
// it came from, such as a chest being looted. The engine's own refusals (the
// feedback 0xf6 path, 0x4a4a4c) set it the same way: TRUE when the item's
// owner is neither the mover nor nothing.
//
// Dragging an item from one slot of a bag to another is
// CNWSCreature::AIActionRepositoryMove, which calls CItemRepository::MoveItem
// and nothing else -- no AddItem, no RemoveItem -- so the Events plugin's
// NWNX_ON_INVENTORY_ADD/REMOVE_ITEM never fire for it, and neither does any
// script event. The Scroll Case window (pw_inc_scrlcase.nss) mirrors its
// case's layout slot for slot, and needs to hear of it.
//
// So after the action, if the item ended up inside an item container, this
// signals NWNX_RISENHOLM_ON_REPOSITORY_MOVE_AFTER through the Events plugin,
// for a script subscribed with NWNX_Events_SubscribeEvent:
//
//     OBJECT_SELF = the container
//     ITEM        = the item moved (event data, an object id string)
//
// The same action also carries an item from the creature's own inventory
// into a bag (through AddItem, which the Events plugin does see), so that
// signals here too. So does dropping an item onto a matching stack in the
// bag, which merges it in and destroys it -- the item is gone by the time
// this looks, but the stack it joined has grown. A move the engine refused
// leaves the item where it was, outside the container, and a move out to
// the creature's own inventory has no container, so neither signals.
//
// The action's parameters, read from the disassembly (nwserver 8193.37): 0 is
// the item, 1 the container it goes into (INVALID for the creature's own
// inventory), 2 and 3 its x and y. They are read before the original runs:
// the node belongs to the action queue.
// ---------------------------------------------------------------------------

static_assert(offsetof(CNWSObjectActionNode, m_pParameter) == 0x38, "CNWSObjectActionNode layout changed");

// CNWSObject::ACTION_FAILED, read from the binary's .rodata (0xb42038, 8193.37):
// ACTION_IN_PROGRESS is 1, ACTION_COMPLETE 2, ACTION_FAILED 3. NWNXLib does
// not declare them.
static constexpr uint32_t ACTION_FAILED = 3;

// Set by the Events plugin's NWNX_EVENT_SIGNAL_EVENT_SKIPPED broadcast, which
// it sends synchronously at the end of every signal, before the broadcast
// that signalled it returns.
static bool s_bRepositoryMoveSkipped = false;

static auto s_idRepositoryMoveSkipped = MessageBus::Subscribe("NWNX_EVENT_SIGNAL_EVENT_SKIPPED",
    [](const std::vector<std::string> &message)
    {
        if (message.size() == 2 && message[0] == "NWNX_RISENHOLM_ON_REPOSITORY_MOVE_BEFORE")
            s_bRepositoryMoveSkipped = message[1] == "1";
    });

static Hooks::Hook s_RepositoryMoveHook = Hooks::HookFunction(&CNWSCreature::AIActionRepositoryMove,
    +[](CNWSCreature *pCreature, CNWSObjectActionNode *pNode) -> uint32_t
    {
        auto oidItem      = static_cast<ObjectID>(pNode->m_pParameter[0]);
        auto oidContainer = static_cast<ObjectID>(pNode->m_pParameter[1]);

        auto *pServer = Globals::AppManager()->m_pServerExoApp;

        {
            auto *pItem      = pServer->GetItemByGameObjectID(oidItem);
            auto *pContainer = pServer->GetItemByGameObjectID(oidContainer);

            if (pItem && pContainer && pContainer->m_pItemRepository)
            {
                s_bRepositoryMoveSkipped = false;

                MessageBus::Broadcast("NWNX_EVENT_PUSH_EVENT_DATA", {"ITEM", Utils::ObjectIDToString(oidItem)});
                MessageBus::Broadcast("NWNX_EVENT_SIGNAL_EVENT", {"NWNX_RISENHOLM_ON_REPOSITORY_MOVE_BEFORE", Utils::ObjectIDToString(oidContainer)});

                if (s_bRepositoryMoveSkipped)
                {
                    // The item's top-level owner: its possessor, or the
                    // possessor of the bag it is in.
                    ObjectID oidOwner = pItem->m_oidPossessor;

                    if (auto *pOwnerItem = pServer->GetItemByGameObjectID(oidOwner))
                        oidOwner = pOwnerItem->m_oidPossessor;

                    if (auto *pPlayer = pServer->GetClientObjectByObjectId(pCreature->m_idSelf))
                    {
                        pServer->GetNWSMessage()->SendServerToPlayerInventory_RepositoryMoveCancel(pPlayer->m_nPlayerID, oidItem,
                            oidOwner != pCreature->m_idSelf && oidOwner != Constants::OBJECT_INVALID);
                    }

                    return ACTION_FAILED;
                }
            }
        }

        auto nResult = s_RepositoryMoveHook->CallOriginal<uint32_t>(pCreature, pNode);

        auto *pItem      = pServer->GetItemByGameObjectID(oidItem);
        auto *pContainer = pServer->GetItemByGameObjectID(oidContainer);

        if (pContainer && pContainer->m_pItemRepository && (!pItem || pItem->m_oidPossessor == oidContainer))
        {
            MessageBus::Broadcast("NWNX_EVENT_PUSH_EVENT_DATA", {"ITEM", Utils::ObjectIDToString(oidItem)});
            MessageBus::Broadcast("NWNX_EVENT_SIGNAL_EVENT", {"NWNX_RISENHOLM_ON_REPOSITORY_MOVE_AFTER", Utils::ObjectIDToString(oidContainer)});
        }

        return nResult;
    });


// ---------------------------------------------------------------------------
// RunScript ON_REMOVED when the host creature is destroyed
// ---------------------------------------------------------------------------
//
// An EffectRunScript's removal script is run by
// CNWSEffectListHandler::OnRemoveRunScript, synchronously and with the host as
// OBJECT_SELF, but only when the effect goes through
// CNWSObject::RemoveEffect/RemoveEffectById. Destroying the host takes neither
// path: ~CNWSObject frees each CGameEffect directly. So DestroyObject, corpse
// decay and the spawn sweep all drop every RunScript effect on a creature
// without its ON_REMOVED, and whatever the module cleans up there leaks.
// (8193.37 disassembly, 2026-09-23.)
//
// Found on production 2026-09-23. pw_rs_provoke lives on the provoked NPC,
// and its ON_REMOVED is the only thing that removes the PERMANENT beam it put
// on the provoker. When the NPC was destroyed, the player kept a beam pointing
// at nothing, visible only to them, and saved into their .bic. The same
// mechanism leaked pw_rs_retal and pw_rs_dr_custom SQL rows keyed by the dead
// creature, and pw_rs_tele entries in the area's telegraph list.
//
// This removes every RunScript effect through RemoveEffectById at the start of
// ~CNWSCreature. The creature is still whole there and still in the object
// table, so ON_REMOVED sees a valid OBJECT_SELF. Anything the script queues on
// the creature itself (DelayCommand, AssignCommand to it) dies with it; queue
// on the module instead, as pw_rs_provoke does. All 58 ON_REMOVED branches in
// the module were audited against this on 2026-09-23.
//
// Deliberately not done:
//  - Player characters. Their effects are saved to the .bic and reapplied at
//    login, and OnApplyRunScript skips the ON_APPLIED script when it is loading
//    a save (its bLoadingGame argument), so firing ON_REMOVED at logout would
//    unbalance every apply/remove pair.
//  - Inside the area and module destructors and module unload, where the
//    engine is emptying containers a script could add to. DestroyArea is NOT
//    such a case: it queues an ordinary destroy for each creature in the area
//    first, so those creatures come through the normal path and their scripts
//    do run, after DestroyArea has returned. Verified on the dev server
//    2026-09-23 with a chicken carrying a logging RunScript effect: DestroyObject
//    and DestroyArea both ran ON_REMOVED with a valid OBJECT_SELF, and nothing
//    ran with the switch off.
//  - Creatures that are not in the object table, such as a temporary one built
//    to read a character file. Module scripts must not run against those.
//  - Placeables, doors and other object types. Creatures only, for now.
//
// Switch: NWNX_RISENHOLM_RUNSCRIPT_REMOVE_ON_DESTROY (bool, default false).

static bool GetRunScriptRemoveOnDestroy()
{
    static const bool s_bOn = []() -> bool
    {
        bool b = Config::Get<bool>("RUNSCRIPT_REMOVE_ON_DESTROY", false);
        LOG_INFO("RunScript ON_REMOVED on creature destruction: %s", b ? "on" : "off");
        return b;
    }();

    return s_bOn;
}

static const bool s_bRunScriptRemoveOnDestroyLogged = (GetRunScriptRemoveOnDestroy(), true);

// Depth of the teardown contexts (area destructor, module destructor, module
// unload) in which no removal script may run. They can nest.
static int s_nRunScriptRemoveSuppressed = 0;

static void RemoveRunScriptEffectsBeforeDestroy(CNWSCreature *pCreature)
{
    if (!GetRunScriptRemoveOnDestroy() || s_nRunScriptRemoveSuppressed > 0)
        return;

    if (pCreature->m_bPlayerCharacter)
        return;

    if (Utils::GetGameObject(pCreature->m_idSelf) != static_cast<CGameObject*>(pCreature))
        return;

    // Ids first, as the RemoveEffect command does: a removal script may itself
    // remove other effects, and RemoveEffectById on an id that is already gone
    // finds nothing and does nothing. Linked effects share an id, so one call
    // takes the whole link, as a script RemoveEffect would.
    //
    // A removal script may also APPLY effects to the creature it is leaving.
    // pw_mod_aoemanage runs its AoE script on ON_REMOVED, and pw_aoem_provoke
    // used to answer by putting a fresh Provoke on the dying host, whose
    // ON_APPLIED hung a new beam on the player. A single pass never saw that
    // effect, so ~CNWSObject freed it without a handler and the beam stayed on
    // the player for good: the stuck Provoke beams on production (2026-09-29,
    // reproduced on dev). So sweep again until a pass finds nothing new, with
    // a cap against a script that re-applies on every removal.
    std::unordered_set<uint64_t> seen;
    auto &effects = pCreature->m_appliedEffects;

    for (int nPass = 0; nPass < 8; nPass++)
    {
        std::vector<uint64_t> ids;

        for (int32_t i = 0; i < effects.num; i++)
        {
            auto *pEffect = effects.element[i];
            if (pEffect && pEffect->m_nType == Constants::EffectTrueType::RunScript && !seen.count(pEffect->m_nID))
                ids.push_back(pEffect->m_nID);
        }

        if (ids.empty())
            return;

        for (uint64_t id : ids)
        {
            seen.insert(id);
            pCreature->RemoveEffectById(id);
        }
    }

    LOG_WARNING("RunScript effects kept reappearing on %x while it was destroyed; gave up after 8 passes", pCreature->m_idSelf);
}

// None of these are in NWNX's function table, so the addresses come from the
// exported symbols, as the item constructor hook above does it. All four have
// an ordinary push/mov prologue the hook engine can relocate.
static Hooks::Hook HookByName(const char *sSymbol, void *pHandler)
{
    void *pTarget = dlsym(RTLD_DEFAULT, sSymbol);

    if (!pTarget)
    {
        LOG_ERROR("%s not found; RunScript ON_REMOVED on creature destruction is off", sSymbol);
        return nullptr;
    }

    return Hooks::HookFunction(pTarget, pHandler, Hooks::Order::Earliest);
}

static Hooks::Hook s_RunScriptCreatureDtorHook;
static Hooks::Hook s_RunScriptAreaDtorHook;
static Hooks::Hook s_RunScriptModuleDtorHook;
static Hooks::Hook s_RunScriptUnloadModuleHook;

static const bool s_bRunScriptRemoveOnDestroyHooked = []() -> bool
{
    s_RunScriptCreatureDtorHook = HookByName("_ZN12CNWSCreatureD1Ev",
        (void*)+[](CNWSCreature *pCreature) -> void
        {
            RemoveRunScriptEffectsBeforeDestroy(pCreature);
            s_RunScriptCreatureDtorHook->CallOriginal<void>(pCreature);
        });

    s_RunScriptAreaDtorHook = HookByName("_ZN8CNWSAreaD1Ev",
        (void*)+[](CNWSArea *pArea) -> void
        {
            s_nRunScriptRemoveSuppressed++;
            s_RunScriptAreaDtorHook->CallOriginal<void>(pArea);
            s_nRunScriptRemoveSuppressed--;
        });

    s_RunScriptModuleDtorHook = HookByName("_ZN10CNWSModuleD1Ev",
        (void*)+[](CNWSModule *pModule) -> void
        {
            s_nRunScriptRemoveSuppressed++;
            s_RunScriptModuleDtorHook->CallOriginal<void>(pModule);
            s_nRunScriptRemoveSuppressed--;
        });

    s_RunScriptUnloadModuleHook = HookByName("_ZN21CServerExoAppInternal12UnloadModuleEv",
        (void*)+[](CServerExoAppInternal *pApp) -> int32_t
        {
            s_nRunScriptRemoveSuppressed++;
            int32_t bRet = s_RunScriptUnloadModuleHook->CallOriginal<int32_t>(pApp);
            s_nRunScriptRemoveSuppressed--;
            return bRet;
        });

    return true;
}();



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

// ---------------------------------------------------------------------------
// DM visibility
// ---------------------------------------------------------------------------
//
// A DM avatar is "manifested" when the DM client's Appear button has been
// pressed and the players can see him; Disappear unmanifests him again, and a
// DM logs in unmanifested. The engine keeps that in
// CNWSCreatureStats::m_bDMManifested and consults it wherever an unmanifested
// DM should not count as being present -- another creature's perception update
// skips him, and so does the pathing line-of-sight test in CNWSArea -- but no
// NWScript command reads it. Scripts that react to a creature being somewhere
// want the same rule: pw_aoe_ballen, for one, stopped the rolling ball dead
// when a DM nobody could see happened to be standing in its path.
//
// The raw flag, so it is only meaningful on a DM avatar. It is left set on a
// player who was toggled in and out of DM with NWNX_Player_ToggleDM (that path
// sets it true both ways), so callers gate on GetIsDM() first.
NWNX_EXPORT ArgumentStack GetIsDMManifested(ArgumentStack&& args)
{
    auto *pCreature = Utils::AsNWSCreature(Utils::GetGameObject(args.extract<ObjectID>()));

    if (!pCreature || !pCreature->m_pStats) return 0;

    return pCreature->m_pStats->m_bDMManifested ? 1 : 0;
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


// Thrown weapons get every attack of the round. ResolveRangedAttack asks
// GetAmmunitionAvailable how many of an attack action's attacks it may fire,
// and for a dart, shuriken, or throwing axe the engine answers with the
// equipped stack size. The module's throwing weapons do not stack
// (baseitems.2da Stacking 1), so each of the round's three attack actions
// fired one attack and every thrower had 3 APR. The Unlimited Ammunition
// property is the first thing the engine checks and would lift that, but it
// cannot be put on a thrown weapon: CNWSEffectListHandler::OnApplyItemProperty
// drops any property itemprops.2da forbids for the base item, and row 61's
// 2_Thrown cell is ****, so AddItemProperty of it silently did nothing, on
// equip and on a blueprint alike (8193.37, read from the disassembly
// 2026-09-29). Answering here needs no property and no 2da edit. Launchers get
// the engine's answer, and thrown weapons are never consumed either way: the
// ResolveAmmunition hook above only ever decrements arrows, bolts, and bullets.
static Hooks::Hook s_GetAmmunitionAvailableHook = Hooks::HookFunction(&CNWSCreature::GetAmmunitionAvailable,
    +[](CNWSCreature *pCreature, int32_t nNumAttacks) -> int32_t
    {
        if (auto *pItem = pCreature->m_pInventory->GetItemInSlot(Constants::EquipmentSlot::RightHand))
        {
            if (pItem->m_nBaseItem == Constants::BaseItem::Dart ||
                pItem->m_nBaseItem == Constants::BaseItem::Shuriken ||
                pItem->m_nBaseItem == Constants::BaseItem::ThrowingAxe)
            {
                return nNumAttacks;
            }
        }

        return s_GetAmmunitionAvailableHook->CallOriginal<int32_t>(pCreature, nNumAttacks);
    }, Hooks::Order::Early);


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


// ---------------------------------------------------------------------------
// Ability bonuses from one item stack
// ---------------------------------------------------------------------------
//
// Every Ability Bonus and Decrease Ability property becomes an effect of its
// own, created by the item with no spell id
// (CNWSItemPropertyHandler::ApplyAbilityBonus), and stock
// CNWSCreature::GetTotalEffectBonus then counts only the largest of each
// item's effects on an ability -- it looks the creator up with
// GetItemByGameObjectID and keeps one entry per item. So +5 and +1 Strength on
// one item gave +5, and -2 and -1 gave -2.
//
// This replaces the Ability branch, the one GetTotalSTRBonus and its five
// siblings call on every ability read (nothing caches it), with the stock
// rules as read from the 8193.37 disassembly (0x45f4f2 onwards) minus the
// per-item one:
//
//  - an effect with a spell id counts only as the largest of that spell's
//    effects on the ability, bonuses and penalties apart;
//  - every other effect counts in full, an item's now included;
//  - bonuses and penalties are totalled separately, each capped by
//    GetAbilityBonusLimit / GetAbilityPenaltyLimit, and the penalty is taken
//    from the bonus.
//
// Stock keeps at most 50 entries a side and ignores the rest; this tracks at
// most 50 spells a side the same way. Nothing comes near either.
//
// The largest-per-item rule could have been hiding a worn item's effects
// applied twice, which would now double. Checked there is no such path:
// CNWSObject::SaveEffectList skips DURATION_TYPE_EQUIPPED, so a relog does not
// bring equipped effects back from the TURD on top of the re-equip, and
// NWNX_Risenholm_ApplyItemProperties only wakes a dormant item, whose
// permanent properties were never applied (pw_inc_loadout decides dormancy
// before an item goes on, never for one already worn).
//
// Every other bonus type goes to the original untouched. NWNX_Feat and
// NWNX_Race hook this function Early and call through, so they still run.
//
// Switch: NWNX_RISENHOLM_STACK_ITEM_ABILITIES (bool, default false).

static bool GetStackItemAbilities()
{
    static const bool s_bOn = []() -> bool
    {
        bool b = Config::Get<bool>("STACK_ITEM_ABILITIES", false);
        LOG_INFO("Ability bonuses from one item stack: %s", b ? "on" : "off");
        return b;
    }();

    return s_bOn;
}

static const bool s_bStackItemAbilitiesLogged = (GetStackItemAbilities(), true);

static Hooks::Hook s_GetTotalEffectBonusHook = Hooks::HookFunction(&CNWSCreature::GetTotalEffectBonus,
    +[](CNWSCreature *thisPtr, uint8_t nEffectBonusType, CNWSObject *pObject, BOOL bElementalDamage, BOOL bForceMax,
            uint8_t nSaveType, uint8_t nSpecificType, uint8_t nSkill, uint8_t nAbilityScore, BOOL bOffHand) -> int32_t
    {
        if (nEffectBonusType != Constants::EffectBonusType::Ability || !GetStackItemAbilities() || !thisPtr->m_pStats)
        {
            return s_GetTotalEffectBonusHook->CallOriginal<int32_t>(thisPtr, nEffectBonusType, pObject, bElementalDamage,
                                                                    bForceMax, nSaveType, nSpecificType, nSkill,
                                                                    nAbilityScore, bOffHand);
        }

        constexpr int32_t MaxSpells = 50;

        struct SpellEntry
        {
            uint32_t nSpellId;
            int32_t nAmount;
        };

        // [0] bonuses, [1] penalties.
        SpellEntry spells[2][MaxSpells];
        int32_t nSpells[2] = {0, 0};
        int32_t nTotal[2] = {0, 0};

        // The engine keeps applied effects sorted by type, and m_nAbilityPtr
        // is where the ability ones begin, as stock walks them.
        auto &effects = thisPtr->m_appliedEffects;

        for (int32_t i = thisPtr->m_pStats->m_nAbilityPtr; i < effects.num; i++)
        {
            auto *pEffect = effects.element[i];

            if (pEffect->m_nType < Constants::EffectTrueType::AbilityIncrease ||
                pEffect->m_nType > Constants::EffectTrueType::AbilityDecrease)
                break;

            if (pEffect->GetInteger(0) != nAbilityScore)
                continue;

            const int nSide = pEffect->m_nType == Constants::EffectTrueType::AbilityDecrease ? 1 : 0;
            const int32_t nAmount = pEffect->GetInteger(1);

            if (pEffect->m_nSpellId == ~0u)
            {
                nTotal[nSide] += nAmount;
                continue;
            }

            int32_t j = 0;

            while (j < nSpells[nSide] && spells[nSide][j].nSpellId != pEffect->m_nSpellId)
                j++;

            if (j < nSpells[nSide])
                spells[nSide][j].nAmount = std::max(spells[nSide][j].nAmount, nAmount);
            else if (j < MaxSpells)
                spells[nSide][nSpells[nSide]++] = {pEffect->m_nSpellId, nAmount};
        }

        for (int nSide = 0; nSide < 2; nSide++)
        {
            for (int32_t j = 0; j < nSpells[nSide]; j++)
                nTotal[nSide] += spells[nSide][j].nAmount;
        }

        auto *pServerExoApp = Globals::AppManager()->m_pServerExoApp;
        const int32_t nBonus = std::min(nTotal[0], pServerExoApp->GetAbilityBonusLimit());
        const int32_t nPenalty = std::min(nTotal[1], pServerExoApp->GetAbilityPenaltyLimit());

        return nBonus - nPenalty;
    }, Hooks::Order::Final);


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

                // RISENHOLM MODIFICATION: The check penalty follows the armour actually worn, not
                // nACArmor. Under Mage Armor, ComputeArmorClass floors the armour at base AC 5
                // (s_ItemComputeArmorClassHook), and row 5's ACCHECK is -5, so a robe cost its
                // wearer a chainmail coat's penalty. The spell grants base AC 5 and caps Dex at
                // +5; it was never meant to cost skills. Read from the chest part the same way
                // the item hook does, before its floor.
                float fWornAC = 0.0f;
                Globals::Rules()->m_p2DArrays->GetPartsChest()->GetFLOATEntry(pItemToEquip->m_nArmorModelPart[7], "ACBonus", &fWornAC);
                pArmorTable->GetINTEntry((int32_t)fWornAC, "ACCHECK", &nArmorCheckPenalty);
                // END RISENHOLM MODIFICATION

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

                pArmorTable->GetINTEntry(nACArmor, "ARCANEFAILURE%", &nArcaneSpellFailure);

                // No ACCHECK read: an unarmoured Mage Armor wearer has no check penalty. Taking
                // row 5's gave every such caster -5 (see the chest-item branch above).
                pStats->m_nBaseArmorArcaneSpellFailure = nArcaneSpellFailure;
                pStats->m_nArmorCheckPenalty = 0;
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

// ---------------------------------------------------------------------------
// Dormant equipment
// ---------------------------------------------------------------------------
//
// A character's loadout is the Bound equipment they last rested in, or last
// wore at Well Water (pw_inc_loadout.nss). Anything else Bound they put on is
// DORMANT: it is worn and looks the part, but none of its own properties reach
// the wearer, so a player can change clothes or swap a helmet for a hat without
// changing their build. The module marks a dormant item with the
// LOADOUT_DORMANT local, and this is what honours the mark.
//
// Every passive property reaches the wearer through
// CNWSItemPropertyHandler::OnItemPropertyApplied -- on equip
// (CNWSItem::ApplyItemProperties, from CNWSCreature::EquipItem), at login (the
// same call with bLoadingGame set, from ReadItemsFromGff), and when a property
// is added to an item already worn (CNWSEffectListHandler::OnApplyItemProperty).
// Read from the 8193.37 decompilation. Refusing it there covers all three, and
// leaves the item itself untouched: its properties stay on it, so Examine,
// item value, the orbs, and every other reader still see a whole item.
//
// Order::Early puts this outside NWNX_Events' hook on the same function, which
// sits at Order::Latest, so a dormant item skips that too -- and with it
// pw_mod_iprpappb, the module's handler for its custom properties (Stamina,
// Dash Speed, DR Threshold, and the rest), which is what we want.
//
// Only the item's own PERMANENT properties sleep. A temporary one is something
// cast on the item while it is worn -- an oil, a weapon enhancement, Magic
// Vestment, elemental damage on a monk's gloves -- and the spell paid for it,
// so it applies whatever the item underneath is doing. The engine keeps the
// distinction in CNWItemProperty::m_nDurationType, 1 for temporary and 2 for
// permanent, the same values as the DURATION_TYPE_* constants
// (CNWSEffectListHandler::OnApplyItemProperty, 8193.37).
//
// Removal is refused in step, by the same test. A dormant item's permanent
// properties were never applied, so there is nothing to take off, and the
// Events docs warn that skipping the apply of Bonus Spell Slot or Unlimited
// Ammunition without also skipping its removal goes wrong. The engine runs the
// module's OnUnequip script BEFORE it removes the properties
// (CNWSCreature::UnequipItem), so pw_mod_unequ leaves the mark for a zero
// delay to clear, and both halves see it.
static bool GetIsItemDormant(CNWSItem *pItem)
{
    static CExoString sVarName = "LOADOUT_DORMANT";
    auto *pScriptVarTable = pItem ? Utils::GetScriptVarTable(pItem) : nullptr;
    return pScriptVarTable && pScriptVarTable->GetInt(sVarName);
}

static bool GetIsItemPropertyDormant(CNWSItem *pItem, CNWItemProperty *pItemProperty)
{
    constexpr uint8_t DurationTypeTemporary = 1;

    return (!pItemProperty || pItemProperty->m_nDurationType != DurationTypeTemporary) && GetIsItemDormant(pItem);
}

// For the Cast Spell refusals, which unlike the property hooks can meet an
// item that is not being worn. The mark is only meant to exist while it is,
// but an unequip the engine makes without running OnUnequip would leave it
// behind, and an item in a pack should never be refused for it.
static bool GetIsWornItemDormant(CNWSItem *pItem)
{
    if (!GetIsItemDormant(pItem))
        return false;

    auto *pCreature = Utils::AsNWSCreature(Utils::GetGameObject(pItem->m_oidPossessor));
    return pCreature && pCreature->m_pInventory && pCreature->m_pInventory->GetSlotFromItem(pItem);
}

// Hooked by mangled symbol, as NWNX_Events does, NOT &CNWSItemPropertyHandler::
// OnItemPropertyApplied: both functions are virtual (overrides of
// CItemPropertyApplierRemover), so the member pointer holds a vtable offset
// rather than a code address, and hooking it segfaulted the server as the
// plugin loaded (2026-09-26).
static Hooks::Hook s_DormantItemPropertyAppliedHook = Hooks::HookFunction(Functions::_ZN23CNWSItemPropertyHandler21OnItemPropertyAppliedEP8CNWSItemP15CNWItemPropertyP12CNWSCreatureji,
    +[](CNWSItemPropertyHandler *thisPtr, CNWSItem *pItem, CNWItemProperty *pItemProperty, CNWSCreature *pCreature,
            uint32_t nInventorySlot, BOOL bLoadingGame) -> int32_t
    {
        if (GetIsItemPropertyDormant(pItem, pItemProperty))
            return 0;

        return s_DormantItemPropertyAppliedHook->CallOriginal<int32_t>(thisPtr, pItem, pItemProperty, pCreature,
                                                                       nInventorySlot, bLoadingGame);
    }, Hooks::Order::Early);

static Hooks::Hook s_DormantItemPropertyRemovedHook = Hooks::HookFunction(Functions::_ZN23CNWSItemPropertyHandler21OnItemPropertyRemovedEP8CNWSItemP15CNWItemPropertyP12CNWSCreaturej,
    +[](CNWSItemPropertyHandler *thisPtr, CNWSItem *pItem, CNWItemProperty *pItemProperty, CNWSCreature *pCreature,
            uint32_t nInventorySlot) -> int32_t
    {
        if (GetIsItemPropertyDormant(pItem, pItemProperty))
            return 0;

        return s_DormantItemPropertyRemovedHook->CallOriginal<int32_t>(thisPtr, pItem, pItemProperty, pCreature,
                                                                       nInventorySlot);
    }, Hooks::Order::Early);

// Wakes an item that was dormant while worn, once the module has cleared its
// LOADOUT_DORMANT mark: at a rest, or on reaching Well Water. The same steps
// CNWSCreature::EquipItem takes around putting an item in its slot, minus the
// slot itself, which it already holds. Nothing to do for an item not worn by
// oCreature -- GetSlotFromItem answers 0 for that.
//
// Permanent properties only. CNWSItem::ApplyItemProperties would do the loop
// below over every passive property, but the temporary ones were never asleep
// -- the hooks above let them through -- so they are on the wearer already, and
// applying them again would stack a second Magic Vestment on the first.
NWNX_EXPORT ArgumentStack ApplyItemProperties(ArgumentStack&& args)
{
    constexpr uint8_t DurationTypeTemporary = 1;

    auto *pCreature = Utils::PopCreature(args);
    auto *pItem = Utils::PopItem(args);

    if (!pCreature || !pItem || GetIsItemDormant(pItem))
        return false;

    const uint32_t nSlot = pCreature->m_pInventory->GetSlotFromItem(pItem);

    if (!nSlot)
        return false;

    auto *pAIMaster = Globals::AppManager()->m_pServerExoApp->GetServerAIMaster();

    for (int32_t i = 0; i < pItem->m_lstPassiveProperties.num; i++)
    {
        auto *pItemProperty = pItem->GetPassiveProperty(i);

        if (pItemProperty && pItemProperty->m_nDurationType != DurationTypeTemporary)
            pAIMaster->OnItemPropertyApplied(pItem, pItemProperty, pCreature, nSlot, false);
    }

    pCreature->ComputeArmourClass(pItem, true, false);
    pCreature->m_pStats->UpdateCombatInformation();

    return true;
}

// ---------------------------------------------------------------------------
// Equipped weight
// ---------------------------------------------------------------------------
//
// The engine keeps a creature's equipped weight as a running total,
// CNWSCreature::m_nEquippedWeight: EquipItem adds the item's weight once it is
// in its slot, UnequipItem subtracts it once it is out, and nothing recomputes
// it. ComputeTotalWeightCarried is that total plus a fresh sum of the pack
// (CItemRepository::CalculateContentsWeight walks the pack every time), and
// UpdateEncumbranceState stores the result in m_nTotalWeightCarried, which is
// the figure CNWSMessage sends the client.
//
// So a worn item whose weight changes leaves the total wrong by the
// difference: EquipItem added one weight and UnequipItem subtracts another.
// The item's own weight stays right throughout -- CNWSItem::ComputeWeight
// rebuilds it from its base item and its Base Item Weight Reduction, Weight
// Increase, and Enhanced Container properties (types 11, 81, and 32) whenever
// one is added or removed -- but the running total is never told.
//
// The module does exactly this on every equip. OnPlayerEquipItem is queued as
// a CScriptEvent from EquipItem and runs after the weight went on; pw_mod_equ
// then adds Magic Vestment's 80% weight reduction to the armour and mirrors a
// main hand's Base Item Weight Reduction onto the off-hand. pw_mod_unequ
// removes them again, but OnPlayerUnEquipItem runs synchronously at the top of
// UnequipItem and RemoveItemProperty is queued (CServerAIMaster::
// AddEventDeltaTime), so the property is still on when UnequipItem subtracts
// the reduced weight. Every cycle leaked the difference upward -- 80% of the
// armour's weight per swap under Magic Vestment. Reported 2026-09-29 as weight
// going up on unequipping and re-equipping.
//
// Rather than chase every path that changes a worn item's weight, stop
// trusting the running total: rebuild it from the slots before every
// UpdateEncumbranceState. ComputeTotalEquippedWeight is the engine's own sum
// over the fourteen equipment slots, and MergeItem already assigns it to the
// same field. The engine calls UpdateEncumbranceState after every equip,
// unequip, pack change, and weight-property change, so the total is right
// whenever anyone looks, and a character carrying a leak from before heals on
// their next update. Fourteen slot lookups per call; it is on no per-frame
// path.
//
// Read from the 8193.37 decompilation and disassembly, 2026-09-29: EquipItem,
// UnequipItem, ComputeTotalWeightCarried, UpdateEncumbranceState,
// ExecuteCommandRemoveItemProperty, and CNWSEffectListHandler's item property
// apply and remove, which recompute the item and call UpdateEncumbranceState
// but never touch m_nEquippedWeight.
static Hooks::Hook s_UpdateEncumbranceStateHook = Hooks::HookFunction(&CNWSCreature::UpdateEncumbranceState,
    +[](CNWSCreature *pThis, BOOL bDisplayFeedback) -> void
    {
        pThis->m_nEquippedWeight = pThis->ComputeTotalEquippedWeight();

        s_UpdateEncumbranceStateHook->CallOriginal<void>(pThis, bDisplayFeedback);
    }, Hooks::Order::Early);

// ---------------------------------------------------------------------------
// Item uses fire once, and a charged item is never used up
// ---------------------------------------------------------------------------
//
// CNWSCreature::AIActionItemCastSpell (0x499f60, 8193.37) runs in two phases,
// told apart by the time since the action started, which it keeps in the
// node's parameters 7 and 8. Until the conjure time is up (500ms, 2500 for
// some base items) it checks GetUsedActivePropertyUsesLeft, fails the action
// if there are none, and clears m_bLastSpellCast. From then on, EVERY call
// that finds m_bLastSpellCast clear casts -- SpellCastAndImpact, which sets
// the flag -- and then runs the consumption switch, with no second look at
// the uses left. The action stays in the queue until its whole duration is
// up, so the flag is all that stops the cast repeating through that tail.
//
// The flag belongs to the creature, not to the action, and it gets cleared
// under a running item use: NWNX_Creature_AddCastSpellActions clears it for
// an instant cast added to the front of the queue (it has to: RunActions
// times a spell action from the object's action timer, which the item use
// already started, so the spell action skips the start that would clear it).
// The instant cast sets it again when it goes off, but AIActionCastSpell has
// a dozen exits before that, and after any of them the item use resumes with
// the flag clear. It then casts a second time for nothing and consumes a
// second time: another one off a stack, another use of the day, or, for a
// charged item now at 0 charges, a spent property on an item that is not
// plot, which the engine destroys 500ms after the projectile time.
// NWNX_TWEAKS_PRESERVE_DEPLETED_ITEMS does not catch that: it holds the item
// plot only for a call entered with 1 to 5 charges.
//
// A Prayer ring was lost this way on production (2026-10-01): 3 charges and 3
// a use, used in a fight, and gone. It had survived the same use twice before
// with the same charges and properties, which is what ruled out the item
// itself. Which instant cast cut in on that player was not established. The
// engine half above is read from the disassembly and was reproduced on the
// dev server the same day, with an NPC wearing a ring of 3 charges: an
// instant cast queued in front 1s into the use, at a target destroyed before
// it could run, had Prayer cast a second time at 0 charges and the ring gone
// within the second. An instant cast that went off did nothing of the kind,
// and nor did a use left alone.
//
// Two things here, then. A use that has cast is remembered, by its node, its
// item, and its start time, and if it comes round again with the flag clear
// the flag is set before the engine looks, so the tail runs out as it would
// have. Whatever cleared it has had its turn by then: RunActions always runs
// the head of the queue, so a cast queued in front has finished or failed
// before the item use is called again. And a use of a charges-per-use
// property (cost table values 2 to 6) holds the item plot for the call
// whatever its charges, so no path through the consumption switch can
// destroy a charged item. The module never wants one destroyed: it recharges
// them at rest and on kills. With the hook, the dev server case above casts
// once and keeps the ring.
//
// The node's parameters are read before the original runs and the node is
// not touched after it: a script run from the cast can clear the queue.
struct FiredItemCast
{
    CNWSObjectActionNode *pNode;
    ObjectID oidItem;
    intptr_t nStartDay;
    intptr_t nStartTime;
};

static std::unordered_map<ObjectID, FiredItemCast> s_FiredItemCasts;

static constexpr uint32_t ACTION_IN_PROGRESS = 1;

static Hooks::Hook s_AIActionItemCastSpellHook = Hooks::HookFunction(&CNWSCreature::AIActionItemCastSpell,
    +[](CNWSCreature *pThis, CNWSObjectActionNode *pNode) -> uint32_t
    {
        const ObjectID oidSelf = pThis->m_idSelf;
        const FiredItemCast thisCast = { pNode, (ObjectID)(uintptr_t)pNode->m_pParameter[0],
                                         pNode->m_pParameter[7], pNode->m_pParameter[8] };
        const auto nPropertyIndex = (int32_t)pNode->m_pParameter[1];

        bool bFired = false;
        auto it = s_FiredItemCasts.find(oidSelf);

        if (it != s_FiredItemCasts.end())
        {
            bFired = it->second.pNode == thisCast.pNode && it->second.oidItem == thisCast.oidItem &&
                     it->second.nStartDay == thisCast.nStartDay && it->second.nStartTime == thisCast.nStartTime;

            // A different use: the one remembered ended without this hook
            // seeing it, cleared out of the queue.
            if (!bFired)
                s_FiredItemCasts.erase(it);
        }

        if (bFired && !pThis->m_bLastSpellCast)
            pThis->m_bLastSpellCast = true;

        auto *pItem = Utils::AsNWSItem(Utils::GetGameObject(thisCast.oidItem));
        BOOL bPlot = false;
        bool bHoldPlot = false;

        if (pItem)
        {
            auto *pProperty = pItem->GetActiveProperty(nPropertyIndex);

            if (pProperty && pProperty->m_nPropertyName == Constants::ItemProperty::CastSpell &&
                pProperty->m_nCostTableValue >= 2 && pProperty->m_nCostTableValue <= 6)
            {
                bPlot = pItem->m_bPlotObject;
                pItem->m_bPlotObject = true;
                bHoldPlot = true;
            }
        }

        const BOOL bCastBefore = pThis->m_bLastSpellCast;

        auto retVal = s_AIActionItemCastSpellHook->CallOriginal<uint32_t>(pThis, pNode);

        // Still there: the engine destroys an item through a queued event.
        if (bHoldPlot)
            pItem->m_bPlotObject = bPlot;

        if (retVal != ACTION_IN_PROGRESS)
            s_FiredItemCasts.erase(oidSelf);
        // Entered with the flag clear, a call either starts the use, which
        // writes m_bLastItemCastSpell 0, or casts, which writes it 1. That
        // tells them apart where m_bLastSpellCast afterwards would not: a
        // script run from the cast can queue an instant cast of its own and
        // have the flag cleared again before the original returns.
        else if (!bFired && !bCastBefore && pThis->m_bLastItemCastSpell && (thisCast.nStartDay || thisCast.nStartTime))
            s_FiredItemCasts[oidSelf] = thisCast;

        return retVal;
    }, Hooks::Order::Early);

static Hooks::Hook s_AddItemCastSpellActionsHook = Hooks::HookFunction(&CNWSCreature::AddItemCastSpellActions,
    +[](CNWSCreature *thisPtr, ObjectID oidItemUsed, int32_t nActivePropertyIndex, int32_t nSubPropertyIndex,
            Vector vTargetLocation, ObjectID oidTarget, int32_t bAreaTarget, int32_t bDecrementCharges) -> int32_t
    {
        if (auto *pItem = Utils::AsNWSItem(Utils::GetGameObject(oidItemUsed)))
        {
            // A dormant item's Cast Spell properties are as asleep as the rest
            // of it. pw_mod_useitemb refuses the inventory's Use with a
            // message; this is the backstop for every other route here.
            if (GetIsWornItemDormant(pItem))
                return false;

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

// CNWSCreature::SetPVPPlayerLikesMe only MUTATES an entry it already finds in
// m_pPVPList -- it never inserts one -- and GetPVPPlayerLikesMe returns TRUE
// ("likes me") on a miss. The only caller of AddToPVPList in the whole binary is
// CNWSMessage::HandlePlayerToServerPVPListOperations subtype 1, which looks up
// the OTHER player named in the message, validates them, and then adds the
// SENDER's own oid to the SENDER's own list (verified in the 8193.37
// disassembly at 0x669f2f-0x669f74: every operand of the add comes from
// param_1). A creature's PvP list can therefore only ever contain itself, so
// every cross-player lookup misses and every script-driven attitude change --
// stock SetPCDislike included -- is a silent no-op.
//
// Insert the entry ourselves and the attitude sticks where GetPVPReputation
// reads it, which is what makes GetIsEnemy true and lets AI and the other
// player's associates acquire the target at all.
static void EnsurePVPListEntry(CNWSCreature *pCreature, ObjectID oidOther)
{
    if (!pCreature || !pCreature->m_pPVPList || oidOther == Constants::OBJECT_INVALID)
        return;

    for (int32_t i = 0; i < pCreature->m_pPVPList->num; i++)
    {
        if (pCreature->m_pPVPList->element[i].m_oidPC == oidOther)
            return;
    }

    pCreature->AddToPVPList(oidOther);
}

// GetClientObjectByObjectId matches only m_oidNWSObject -- the object the player
// is CONTROLLING -- so it returns null for a PC's own body while that player is
// possessing something else. That is exactly the state the invasion code calls
// SetPCLikeStatus in (pw_inc_invader.nss hands us the sleeping body), so match
// either the controlled object or the PC object.
static CNWSPlayer *FindPlayerForCreature(ObjectID oid)
{
    if (oid == Constants::OBJECT_INVALID)
        return nullptr;

    for (auto *pPlayer : Globals::AppManager()->m_pServerExoApp->GetPlayerList())
    {
        if (pPlayer && (pPlayer->m_oidNWSObject == oid || pPlayer->m_oidPCObject == oid))
            return pPlayer;
    }

    return nullptr;
}

NWNX_EXPORT ArgumentStack SetPCLikeStatus(ArgumentStack&& args)
{
    auto sourceOID      = args.extract<ObjectID>();
    auto targetOID      = args.extract<ObjectID>();
    auto bNewAttitude   = args.extract<int32_t>();
    auto bSetReciprocal = args.extract<int32_t>();

    auto *pServer = Globals::AppManager()->m_pServerExoApp;
    auto *pSource = pServer->GetCreatureByGameObjectID(sourceOID);

    if (!pSource)
        return {};

    EnsurePVPListEntry(pSource, targetOID);

    if (bSetReciprocal)
        EnsurePVPListEntry(pServer->GetCreatureByGameObjectID(targetOID), sourceOID);

    pSource->SetPVPPlayerLikesMe(targetOID, bNewAttitude, bSetReciprocal);

    // The server half above is only what the AI reads. PC-vs-PC hostility as the
    // players SEE it -- ring colour, and being able to attack without the game
    // arguing -- is client state, fed by this message; stock SetPCDislike sends
    // it in both directions and this export used not to send it at all, which is
    // why Scar Intruders rendered neutral however hostile the server thought
    // they were. The recipient is the side whose view just changed.
    auto *pMessage      = pServer->GetNWSMessage();
    auto *pSourcePlayer = FindPlayerForCreature(sourceOID);
    auto *pTargetPlayer = FindPlayerForCreature(targetOID);

    if (pMessage && pSourcePlayer && pTargetPlayer)
    {
        pMessage->SendServerToPlayerPVP_Attitude_Change(
            pSourcePlayer->m_nPlayerID, pTargetPlayer->m_nPlayerID, bNewAttitude);

        if (bSetReciprocal)
        {
            pMessage->SendServerToPlayerPVP_Attitude_Change(
                pTargetPlayer->m_nPlayerID, pSourcePlayer->m_nPlayerID, bNewAttitude);
        }
    }

    return {};
}

// Re-send the player-list entry of the player driving (or owning) a creature, so
// the object id stored in every client's copy tracks what that player is driving
// right now.
//
// A chat line carries nothing but the speaker's object id and the text. The
// client turns that id into a player by searching its OWN copy of the player
// list (CClientExoApp::GetPlayerByGameObjectID, matching CNWCPlayer+0x34), and
// builds the clickable reply portrait beside the line only on a hit; clicking it
// fills the chat bar with /tp "<player name>". That +0x34 is written from the
// object id in a SendServerToPlayerPlayerList_Add/_All entry, which is the
// player's m_oidNWSObject at the moment of sending -- and the engine sends those
// only when a player enters the module. Possession moves m_oidNWSObject onto the
// possessed creature, every line the player sends from then on is stamped with
// that creature's id, and no client can match it: no portrait, so a tell from a
// Scar Intruder cannot be answered by clicking it, which gives the invasion away
// out of character. Read from the 8193.37 server and Linux client disassembly,
// 2026-09-23.
//
// Safe at any point after the player has entered the module (NWNX_Rename warns
// against sending the list before the engine's own first Add). The client's
// handler looks the entry up by player id first and updates that row in place,
// so this never adds a second row, and nothing visible changes: an ordinary
// entry's name is the net layer's player name, which possession cannot touch,
// and the character-name block the engine builds follows m_oidMaster back to the
// PC body whenever the driven creature is not itself flagged a player character
// (m_bIsPC, which no possession path writes). Mirrors the engine's login
// broadcast, once to players and once to DMs; NWNX_Rename hooks both sends and
// applies its overrides exactly as it does at login.
NWNX_EXPORT ArgumentStack RefreshPlayerListEntry(ArgumentStack&& args)
{
    auto oidCreature = args.extract<ObjectID>();

    auto *pPlayer  = FindPlayerForCreature(oidCreature);
    auto *pMessage = Globals::AppManager()->m_pServerExoApp->GetNWSMessage();

    if (!pPlayer || !pMessage)
        return {};

    pMessage->SendServerToPlayerPlayerList_Add(Constants::PLAYERID_ALL_PLAYERS, pPlayer);
    pMessage->SendServerToPlayerPlayerList_Add(Constants::PLAYERID_ALL_GAMEMASTERS, pPlayer);

    return {};
}

// CServerExoAppInternal::RemovePCFromWorld (8193.37) segfaults when a player
// disconnects while driving a creature whose m_oidMaster no longer resolves to
// a creature, if any other client is on the character-select screen at that
// moment. Read from the disassembly after the 2026-09-22 production crash
// (RemovePCFromWorld+0xf3, reached from NWNX_Events' disconnect hook):
//
//   - if the driven creature's m_oidMaster is not OBJECT_INVALID, the function
//     stores GetCreatureByGameObjectID(m_oidMaster) WITHOUT a null check;
//   - it then walks the player list and, for every player with
//     m_bPlayModuleListingCharacters set, sends a character-list response
//     carrying that pointer's m_idSelf -- a read of NULL+8.
//
// The same lookup a few instructions later IS null-checked, and falls back to
// the driven creature itself (cmovne), and SaveServerCharacter separately
// refuses to save a possessed familiar whose master does not resolve. So
// clearing a dangling master here makes the first lookup agree with the
// engine's own fallback and changes nothing else: no character is saved that
// would not have been, and OnClientLeave sees the same object it would have.
//
// NWNX_Events hooks this at Order::Earliest, so NWNX_ON_CLIENT_DISCONNECT_BEFORE
// has already run by the time this sees the creature.
static Hooks::Hook s_RemovePCFromWorldHook = Hooks::HookFunction(&CServerExoAppInternal::RemovePCFromWorld,
    +[](CServerExoAppInternal *pThis, CNWSPlayer *pPlayer) -> void
    {
        auto *pServer   = Globals::AppManager()->m_pServerExoApp;
        auto *pCreature = pPlayer ? Utils::AsNWSCreature(Utils::GetGameObject(pPlayer->m_oidNWSObject)) : nullptr;

        if (pCreature && pCreature->m_oidMaster != Constants::OBJECT_INVALID &&
            !pServer->GetCreatureByGameObjectID(pCreature->m_oidMaster))
        {
            auto *pInfo = pServer->GetNetLayer()->GetPlayerInfo(pPlayer->m_nPlayerID);

            LOG_WARNING("RemovePCFromWorld: player '%s' is leaving while driving %x (tag '%s', associate type %d), "
                        "whose master %x no longer exists; clearing it to avoid the engine's null dereference",
                        pInfo ? pInfo->m_sPlayerName.CStr() : "?",
                        pCreature->m_idSelf, pCreature->m_sTag.CStr(),
                        pCreature->m_nAssociateType, pCreature->m_oidMaster);

            pCreature->m_oidMaster = Constants::OBJECT_INVALID;
        }

        s_RemovePCFromWorldHook->CallOriginal<void>(pThis, pPlayer);
    });

// CNWSPlayer::SaveServerCharacter (8193.37) writes whichever creature the
// client is driving (m_oidNWSObject) into the player's vault file. Only for a
// possessed familiar, or a DM possession (associate types 7 and 8), does it
// write the master instead, and it skips the save when that master does not
// resolve. Nothing checks that the creature it ends up with is a player
// character at all.
//
// So a player left driving a creature that has stopped being their possessed
// familiar gets that creature saved over their character. That happened on
// production on 2026-09-23: an Intruder really died, the module skipped the
// engine's forced unpossess (fixed in pw_mod_unpossesb since), OnApplyDeath
// removed it from its master's associates anyway, and the next save wrote the
// dead NPC into tyrese's ghost.bic. An NPC has class levels but no
// LvlStatList, so the next save of THAT -- CopyObject in the intro cutscene,
// its XP being 0 -- segfaulted in CNWSCreatureStats::SaveClassInfo, twice.
//
// Resolve the save target exactly the way the engine does and refuse to write
// anything that is not a player character. The player keeps their last good
// file; losing the progress since then is far better than losing the
// character, and a crash on every later login. FALSE is what the engine's
// callers already handle, since NWNX_Events' skip returns it too. Earliest, so
// a refused save does not raise the save events either.
//
// The engine saves only character types 3 and 4 (the byte at CNWSPlayer+0xb4)
// and returns before touching the creature otherwise; the same gate here keeps
// every other export (a DM's, for one) out of the check and out of the log.
static_assert(offsetof(CNWSPlayer, m_nCharacterType) == 0xb4);
static Hooks::Hook s_SaveServerCharacterHook = Hooks::HookFunction(&CNWSPlayer::SaveServerCharacter,
    +[](CNWSPlayer *pPlayer, int32_t bBackupPlayer) -> int32_t
    {
        auto *pServer   = Globals::AppManager()->m_pServerExoApp;
        auto *pCreature = pPlayer && (uint8_t)(pPlayer->m_nCharacterType - 3) < 2
            ? Utils::AsNWSCreature(Utils::GetGameObject(pPlayer->m_oidNWSObject))
            : nullptr;
        auto *pTarget   = pCreature;

        if (pCreature && (pCreature->GetIsPossessedFamiliar() ||
                          pCreature->m_nAssociateType == Constants::AssociateType::DMPossess ||
                          pCreature->m_nAssociateType == Constants::AssociateType::DMImpersonate))
        {
            pTarget = pServer->GetCreatureByGameObjectID(pCreature->m_oidMaster);
        }

        // A null target is a save the engine skips by itself.
        if (pTarget && !pTarget->m_bPlayerCharacter)
        {
            auto *pInfo = pServer->GetNetLayer()->GetPlayerInfo(pPlayer->m_nPlayerID);

            LOG_WARNING("SaveServerCharacter: refusing to save %x (tag '%s', associate type %d, master %x) "
                        "for player '%s': it is not a player character",
                        pTarget->m_idSelf, pTarget->m_sTag.CStr(), pTarget->m_nAssociateType,
                        pTarget->m_oidMaster, pInfo ? pInfo->m_sPlayerName.CStr() : "?");

            return false;
        }

        return s_SaveServerCharacterHook->CallOriginal<int32_t>(pPlayer, bBackupPlayer);
    }, Hooks::Order::Earliest);

// CNWSCreature::UnpossessFamiliar (8193.37) refuses, doing nothing at all but
// log "Unable to unpossess familiar. Player has no valid area.", when its
// first familiar exists but either the possessor or that familiar has no
// area -- e.g. the possessed creature is mid-jump while its client loads the
// destination. NWNX_Player's own hook on it, which PossessCreature installs,
// calls the engine and then cuts the associate link and restores the
// creature's old associate type WITHOUT checking whether the unpossess
// happened. After a refusal the player was left driving a creature that was
// no longer their familiar, which nothing could unpossess afterwards: the
// orphaned Intruder of 2026-09-27.
//
// When the engine is going to refuse, return before the hook chain, so
// neither the engine nor NWNX_Player's bookkeeping runs and the link survives
// for a later unpossess to succeed. The test is the engine's own, read from
// the 8193.37 decompile: the creature behind GetAssociateId(FAMILIAR, 1), then
// GetArea on it and on the possessor. Earliest, so this runs before
// NWNX_Player's Early hook however late that one is installed -- NWNX keeps
// hooks sorted by order, not by when they were made.
//
// This lives here rather than as a fix inside Plugins/Player, which comes from
// upstream and would lose the change at the next merge.
static Hooks::Hook s_UnpossessFamiliarHook = Hooks::HookFunction(&CNWSCreature::UnpossessFamiliar,
    +[](CNWSCreature *pPossessor) -> void
    {
        auto *pServer   = Globals::AppManager()->m_pServerExoApp;
        auto *pFamiliar = pServer->GetCreatureByGameObjectID(pPossessor->GetAssociateId(Constants::AssociateType::Familiar, 1));

        if (pFamiliar && (!pPossessor->GetArea() || !pFamiliar->GetArea()))
        {
            LOG_WARNING("UnpossessFamiliar: %x or its familiar %x has no area, so the engine would refuse; "
                        "keeping the familiar link for a later unpossess", pPossessor->m_idSelf, pFamiliar->m_idSelf);
            return;
        }

        s_UnpossessFamiliarHook->CallOriginal<void>(pPossessor);
    }, Hooks::Order::Earliest);

// CNWSPlayer::DropTURD (8193.37) makes a TURD for a leaving player only when
// the creature has an area, or failing that a desired area that still exists.
// Otherwise it deletes the TURD it just built and never adds it to the list.
// A login with no TURD to resume has neither: OnClientEnter is queued in
// LoadCharacterFinish, and m_oidDesiredArea is only set in
// InitiateModuleForPlayer, when the client's module message arrives later. So
// a player booted in OnClientEnter (a ban, a refused login), or who drops in
// that window, leaves no TURD at all. NWNXLib's POS hook on the same function runs
// after the engine and copies the leaver's NWNX_Object variables onto the TURD
// at the HEAD of the module's list, assuming it is the one just added. With no
// TURD added it is someone else's -- whoever left last -- and that character's
// variables are replaced wholesale by the leaver's. EatTURD restores them over
// the ones loaded from the .bic at that character's next login, so the next
// save writes another character's variables into their file.
//
// Production, 2026-09-28: Syclya Xao'se, banned and booted in OnClientEnter,
// twice, and Gers Fryar's and then Kitiara Vance's TURDs took her variables.
// The module's PC_UUID check (pw_inc_player) caught Kitiara holding Syclya's
// UUID and refused the login, and every refusal fed the same TURD back, so
// she was locked out until a restart. Before that check existed, the
// crossover would have been saved silently.
//
// When the engine is going to add no TURD, hide the list head from the POS
// hook for the duration of the call, so it has nothing to copy onto. The test
// is the engine's own, read from the 8193.37 disassembly: the driven creature
// (its master for a possessed familiar), then GetArea, then m_oidDesiredArea
// through GetAreaByGameObjectID. The engine does not touch the list on that
// path, so the head can go back afterwards unchanged. Earliest, so it wraps
// the POS hook (VeryEarly). A fix belongs upstream in NWNXLib/POS.cpp; this is
// here until then, because an upstream merge would lose an edit there.
static Hooks::Hook s_DropTURDHook = Hooks::HookFunction(&CNWSPlayer::DropTURD,
    +[](CNWSPlayer *pPlayer) -> void
    {
        auto *pServer   = Globals::AppManager()->m_pServerExoApp;
        auto *pObject   = pPlayer->GetGameObject();
        auto *pCreature = pObject ? pObject->AsNWSCreature() : nullptr;

        // No game object: the POS hook finds no source either, so nothing to guard.
        if (!pObject)
        {
            s_DropTURDHook->CallOriginal<void>(pPlayer);
            return;
        }

        if (pCreature && pCreature->GetIsPossessedFamiliar())
        {
            if (auto *pMaster = pServer->GetCreatureByGameObjectID(pCreature->m_oidMaster))
                pCreature = pMaster;
        }

        bool bAddsTURD = pCreature &&
            (pCreature->GetArea() ||
             (pCreature->m_oidDesiredArea != Constants::OBJECT_INVALID &&
              pServer->GetAreaByGameObjectID(pCreature->m_oidDesiredArea)));

        auto *pList = Utils::GetModule()->m_lstTURDList.m_pcExoLinkedListInternal;

        if (bAddsTURD || !pList || !pList->pHead)
        {
            s_DropTURDHook->CallOriginal<void>(pPlayer);
            return;
        }

        auto *pInfo = pServer->GetNetLayer()->GetPlayerInfo(pPlayer->m_nPlayerID);

        LOG_WARNING("DropTURD: %x ('%s', player '%s') leaves no TURD (no area, desired area %x); "
                    "hiding the TURD list from NWNX's POS copy so it cannot land on another character's",
                    pObject->m_idSelf, pCreature ? pCreature->m_pStats->GetFullName().CStr() : "?",
                    pInfo ? pInfo->m_sPlayerName.CStr() : "?",
                    pCreature ? pCreature->m_oidDesiredArea : Constants::OBJECT_INVALID);

        auto pHead = pList->pHead;
        pList->pHead = nullptr;
        s_DropTURDHook->CallOriginal<void>(pPlayer);
        pList->pHead = pHead;
    }, Hooks::Order::Earliest);

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
// Poisoning the cached appearance block makes ComputeAppearanceUpdateRequired flag every
// field dirty, so the next WriteGameObjUpdate_UpdateAppearance re-sends the whole thing.
// NWNX_Item_SetItemAppearance already relies on this exact trick, but only invalidates the
// three armour item oids; a wholesale appearance swap needs the part variations, phenotype,
// and colours too.
//
// Clear() alone is NOT enough, because it parks every field at a value a creature really
// holds, and a field that compares equal is never written. The diff is field by field,
// each with its own dirty bit (tail 0x800, wing 0x1000), and WriteGameObjUpdate_
// UpdateAppearance writes the tail and wing DWORDs only under those bits. So clearing a
// tail and calling this in the same tick -- which pw_ee_endwildshp does on every unshift --
// left the cached tail at Clear()'s 0 and the live tail at CREATURE_TAIL_TYPE_NONE, also 0.
// The tail delta that would have gone out without this call was cancelled by it, and every
// observer kept rendering the wildshape tail until an area change rebuilt their cache.
// (Read from the nwserver 8193.37 disassembly, 2026-09-27.) Every field is therefore set
// to a value no creature can hold, not just the appearance type.
//
// The offsets are the ones ComputeAppearanceUpdateRequired compares, measured from the
// CLastUpdateObject it is handed.
static_assert(offsetof(CLastUpdateObject, m_cAppearance) + offsetof(CNWSPlayerLUOAppearanceInfo, m_nAppearanceType) == 0x14);
static_assert(offsetof(CLastUpdateObject, m_cAppearance) + offsetof(CNWSPlayerLUOAppearanceInfo, m_nTailVariation) == 0x44);
static_assert(offsetof(CLastUpdateObject, m_cAppearance) + offsetof(CNWSPlayerLUOAppearanceInfo, m_nWingVariation) == 0x48);
static void PoisonAppearanceCache(CNWSPlayerLUOAppearanceInfo &cAppearance)
{
    cAppearance.Clear();

    // Item oids and weapon VFX all feed the one items bit, 0x200.
    cAppearance.m_oidLeftHandItem   = 0xFFFFFFFF;
    cAppearance.m_oidRightHandItem  = 0xFFFFFFFF;
    cAppearance.m_oidChestItem      = 0xFFFFFFFF;
    cAppearance.m_oidHeadItem       = 0xFFFFFFFF;
    cAppearance.m_oidCloakItem      = 0xFFFFFFFF;
    cAppearance.m_nRightHandItemVFX = 0xFF;
    cAppearance.m_nLeftHandItemVFX  = 0xFF;

    cAppearance.m_nAppearanceType = 0xFFFF;
    cAppearance.m_nPhenoType      = 0xFF;
    cAppearance.m_nGender         = 0xFF;
    cAppearance.m_nSkinColor      = 0xFF;
    cAppearance.m_nHairColor      = 0xFF;
    cAppearance.m_nTattooColor1   = 0xFF;
    cAppearance.m_nTattooColor2   = 0xFF;
    cAppearance.m_nHeadVariation  = 0xFFFF;
    cAppearance.m_nTailVariation  = 0xFFFFFFFF;
    cAppearance.m_nWingVariation  = 0xFFFFFFFF;

    for (auto &nPart : cAppearance.m_pPartVariation)
        nPart = 0xFFFF;
}

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
            PoisonAppearanceCache(pLUO->m_cAppearance);
        }
    }

    return {};
}

// ---------------------------------------------------------------------------
// Body parts re-tinted after a texture-replacing VFX
//
// When a duration VFX whose progfx.2da row is Type 1 (a texture replace:
// Stoneskin, Petrify, the jewel and bone skins, ShadowSkin, IceSkin...) ends,
// the client puts each body part's PLT back through
// CNWCAnimBaseParts::RestoreTexture, which re-applies every part with ONE
// stored block of ten palette colours -- the block written by the last part
// loaded (CNWCAnimBaseParts::ReplaceTexture keeps the resref per part but the
// colours in a single buffer at +0x18). Armour coloured per part
// (ITEM_APPR_TYPE_ARMOR_COLOR 6..119, the tailor's per-part mode) therefore
// comes back with one part's colours on all of them, usually the untouched
// whole-piece "defaults". Whole-piece colours are identical on every part and
// survive. Read from nwmain-linux 8193.37 and reproduced in game 2026-09-27
// with two tailor dummies in the same padded armour.
//
// Nothing the server re-sends repairs that on its own: the client only
// reloads a part whose VARIATION differs from the one it has
// (CNWCCreatureAppearance::CreateBodyParts skips the rest), so
// ForceAppearanceUpdate, its full-object variant, and an NWNX unequip+equip
// were each tried in game and changed nothing. A player's own re-equip works
// because the parts go naked and come back.
//
// So that is what this does, invisibly. On the first object update the
// player receives after the effect is removed -- the one carrying the VFX
// 'D' delta, and WriteGameObjUpdate_UpdateObject writes the appearance block
// BEFORE the 'U' block holding the VFX list -- two more appearance blocks are
// appended: the chest item gone and every part variation 0 (parts unloaded),
// then the chest item back and the real values. The client handles a whole
// message before it renders, so the parts reload with the right per-part
// colours and no frame ever shows the swap.
// Whole-piece armour gets the same treatment: two small blocks and one reload
// of the equipped parts is cheaper than telling the two apart.
// ---------------------------------------------------------------------------

static_assert(offsetof(CNWSCreature, m_cAppearance) + offsetof(CNWSCreatureAppearanceInfo, m_pPartVariation) == 0xa74);

// (player id << 32 | creature oid) -> when it was queued. Consumed by the
// update hook below; an entry the player's updates never reach (they stopped
// seeing the creature) is dropped, unused, once it is older than a few seconds.
static std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> s_PendingPartRefresh;

// Make the player's next tick carry an appearance update for the creature
// even if nothing changed: a cached part value no creature has. The client
// gets the real value, which it already has, and ignores it. Only works
// BEFORE the tick's compare; the caller stores the tick's fields into the
// cache right after the update hook, which undoes a poison set there.
static void PoisonPartCache(CLastUpdateObject *pLUO)
{
    pLUO->m_cAppearance.m_pPartVariation[0] = 0xFFFF;
}

static uint64_t PartRefreshKey(uint32_t nPlayerId, ObjectID oidCreature)
{
    return (uint64_t(nPlayerId) << 32) | oidCreature;
}

// visualeffects.2da ProgFX_Duration -> progfx.2da Type == 1.
static bool IsTextureReplaceVfx(int32_t nRow)
{
    static std::unordered_map<int32_t, bool> s_Cache;

    auto it = s_Cache.find(nRow);
    if (it != s_Cache.end())
        return it->second;

    // Through the name cache, not the fixed table slots: the dedicated server
    // never fills the progfx slot (CTwoDimArrays::Load2DArrays loads it in a
    // client-only block), so GetProgFxTable() is null here and a check built
    // on it was silently false for every row (2026-09-27).
    bool bReplace = false;
    int32_t nProgFx = 0, nType = 0;
    auto *pVfx    = Globals::Rules()->m_p2DArrays->GetCached2DA("visualeffects", true);
    auto *pProgFx = Globals::Rules()->m_p2DArrays->GetCached2DA("progfx", true);

    if (pVfx && pProgFx
        && pVfx->GetINTEntry(nRow, "ProgFX_Duration", &nProgFx)
        && pProgFx->GetINTEntry(nProgFx, "Type", &nType))
        bReplace = nType == 1;

    s_Cache[nRow] = bReplace;
    return bReplace;
}

// Whether a texture replace is on the creature right now. The effect whose
// removal queued a refresh is already out of the list by the time the update
// hook asks.
static bool HasTextureReplaceVfx(CNWSCreature *pCreature)
{
    auto &effects = pCreature->m_appliedEffects;

    for (int32_t i = 0; i < effects.num; i++)
    {
        if (effects.element[i]
            && effects.element[i]->m_nType == Constants::EffectTrueType::VisualEffect
            && IsTextureReplaceVfx(effects.element[i]->GetInteger(0)))
            return true;
    }

    return false;
}

// Queue the refresh for every player currently tracking the creature.
// bForceUpdate poisons each player's cached part list so the next tick sends
// an update even when nothing else changed; the script export needs that,
// whereas a VFX removal already brings its own 'D'.
static void QueueBodyPartRefresh(CNWSCreature *pCreature, bool bForceUpdate)
{
    int32_t nQueued = 0;
    bool bHasParts = false;
    for (uint16_t nPart : pCreature->m_cAppearance.m_pPartVariation)
        bHasParts |= nPart != 0;

    if (!bHasParts)                                  // not a part-based body
        return;

    auto tNow = std::chrono::steady_clock::now();

    for (auto *pPlayer : Globals::AppManager()->m_pServerExoApp->GetPlayerList())
    {
        auto *pLUO = pPlayer->GetLastUpdateObject(pCreature->m_idSelf);
        if (!pLUO)
            continue;

        s_PendingPartRefresh[PartRefreshKey(pPlayer->m_nPlayerID, pCreature->m_idSelf)] = tNow;
        nQueued++;

        if (bForceUpdate)
            PoisonPartCache(pLUO);
    }

    LOG_DEBUG("Body part refresh queued for creature %x on %d player(s)", pCreature->m_idSelf, nQueued);
}

static Hooks::Hook s_OnRemoveVisualEffectHook = Hooks::HookFunction(&CNWSEffectListHandler::OnRemoveVisualEffect,
    +[](CNWSEffectListHandler *pThis, CNWSObject *pObject, CGameEffect *pEffect) -> int32_t
    {
        int32_t nRet = s_OnRemoveVisualEffectHook->CallOriginal<int32_t>(pThis, pObject, pEffect);

        if (auto *pCreature = Utils::AsNWSCreature(pObject))
            if (IsTextureReplaceVfx(pEffect->GetInteger(0)))
                QueueBodyPartRefresh(pCreature, false);

        return nRet;
    }, Hooks::Order::Late);

static Hooks::Hook s_WriteGameObjUpdate_UpdateObjectHook = Hooks::HookFunction(&CNWSMessage::WriteGameObjUpdate_UpdateObject,
    +[](CNWSMessage *pThis, CNWSPlayer *pPlayer, CNWSObject *pObject, CLastUpdateObject *pLUO,
        uint32_t nObjectUpdatesRequired, uint32_t nObjectAppearanceUpdatesRequired) -> void
    {
        s_WriteGameObjUpdate_UpdateObjectHook->CallOriginal<void>(pThis, pPlayer, pObject, pLUO,
            nObjectUpdatesRequired, nObjectAppearanceUpdatesRequired);

        // Party members come through here with no LUO: nothing to refresh.
        if (!pLUO || s_PendingPartRefresh.empty())
            return;

        auto it = s_PendingPartRefresh.find(PartRefreshKey(pPlayer->m_nPlayerID, pObject->m_idSelf));
        if (it == s_PendingPartRefresh.end())
            return;

        auto *pCreature = Utils::AsNWSCreature(pObject);
        bool bFresh = std::chrono::steady_clock::now() - it->second < std::chrono::seconds(10);
        s_PendingPartRefresh.erase(it);

        if (!bFresh || !pCreature)
            return;

        // Not while another texture replace is still on the creature: the
        // reload would pull the parts out from under a replace the client has
        // live. The skin spells each flash VFX_DUR_PROT_STONESKIN for 3.0s, and
        // a hasted caster lands two spells exactly 3.0s apart, so Ironskin's
        // flash ends in the tick Stoneskin's begins. On production (2026-09-30)
        // a caster and the party member beside her both stopped answering the
        // server in that very second, twice in six minutes, and not when the
        // same two spells landed 4s apart unhasted. Not yet reproduced on a
        // client. Nothing is lost by waiting: the replace that is still up
        // queues a refresh of its own when it ends.
        if (HasTextureReplaceVfx(pCreature))
            return;

        // Written into the update carrying the removal, i.e. right behind the
        // VFX 'D' in the same message. That is the write that works: in game
        // (2026-09-27) a cycle sent only in a LATER tick, after the client had
        // swept the stopped effect, re-tinted some parts and left others,
        // however often it was repeated, whereas this one restored every part.
        LOG_DEBUG("Body part refresh written for creature %x to player %d", pObject->m_idSelf, pPlayer->m_nPlayerID);

        // Parts (0x100) and equipped items (0x200) together: the chest item
        // has to leave and come back with the parts, because the client keeps
        // the per-part colours only as long as it does. Its chest-delete
        // branch wipes them to "no override" and its chest-add branch re-reads
        // them from the item before the rebuild; a parts-only pair was tried
        // first and re-tinted just some of the parts.
        static constexpr uint32_t APPEARANCE_PARTS_AND_ITEMS = 0x300;
        auto &cAppearance = pCreature->m_cAppearance;
        uint16_t nSavedParts[19];
        memcpy(nSavedParts, cAppearance.m_pPartVariation, sizeof(nSavedParts));
        const ObjectID oidChest = cAppearance.m_oidChestItem;

        // Both blocks list only what differs from the player's cached copy, so
        // the cache has to follow each write, or the second block would be
        // empty and the client would keep the unloaded parts.
        memset(cAppearance.m_pPartVariation, 0, sizeof(nSavedParts));
        cAppearance.m_oidChestItem = Constants::OBJECT_INVALID;
        pThis->WriteGameObjUpdate_UpdateAppearance(pObject, pLUO, APPEARANCE_PARTS_AND_ITEMS, pPlayer);
        pThis->UpdateLastUpdateObjectAppearance(pObject, pLUO, APPEARANCE_PARTS_AND_ITEMS);

        memcpy(cAppearance.m_pPartVariation, nSavedParts, sizeof(nSavedParts));
        cAppearance.m_oidChestItem = oidChest;
        pThis->WriteGameObjUpdate_UpdateAppearance(pObject, pLUO, APPEARANCE_PARTS_AND_ITEMS, pPlayer);
        pThis->UpdateLastUpdateObjectAppearance(pObject, pLUO, APPEARANCE_PARTS_AND_ITEMS);
    }, Hooks::Order::Late);

NWNX_EXPORT ArgumentStack RefreshBodyParts(ArgumentStack&& args)
{
    if (auto *pCreature = Utils::AsNWSCreature(Utils::GetGameObject(args.extract<ObjectID>())))
        QueueBodyPartRefresh(pCreature, true);

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

// RISENHOLM MODIFICATION: an attack of opportunity does not change the attacker's target.
//
// AddAttackOfOpportunity queues a combat round action of type 1 (attack) aimed at
// its own target with m_bActionRetargettable = 0 (disassembled at 0x651d10), as
// AddParryAttack and AddWhirlwindAttack also do. When AIActionAttackObject
// reaches such an action and its target is not the ATTACKOBJECT node's target,
// it does
//
//     m_oidAttemptedAttackTarget = action.m_oidTarget;
//
// before swinging (nwscreatureaicombat.cpp, AIActionAttackObject case 1), and
// nothing ever sets it back: the only other writers are ChangeAttackTarget, the
// INVALID -> node target fill-in at the top of the same function, and
// RunActions clearing it when the action queue empties. The node keeps the real
// target and later swings go there, but GetAttemptedAttackTarget() reports
// the AoO victim until the creature next changes target. That is how the
// maneuvers (pw_sp_mv_*), Action Surge, Shockwave, and the NPC AI pick their
// target, so a Shield Fighting AoO sent the next Knockdown to whoever had just
// hit the shield-bearer. It also hits the engine's own movement AoOs, Parry
// ripostes, and Whirlwind Attack, which go through the same code.
//
// The node's target is mirrored in m_oidCurrentActionTarget (RunActions sets it
// from m_pParameter[0], ChangeAttackTarget alongside the node), so a call that
// moves the attempted target without moving that is the case above, and it is
// put back. The node itself is not read after the call: the swing runs the
// attack event script, which can clear the action queue under us.
//
// m_oidAttackTarget (GetAttackTarget) and the facing lock are left alone. They
// follow whatever is being swung at, the AoO victim during the AoO, and the
// next ordinary swing sets them back to the node's target.
static_assert(offsetof(CNWSObject, m_oidCurrentActionTarget) == 0x94, "CNWSObject layout changed");
static_assert(offsetof(CNWSCreature, m_oidAttemptedAttackTarget) == 0x720, "CNWSCreature layout changed");

static Hooks::Hook s_AIActionAttackObjectHook = Hooks::HookFunction(&CNWSCreature::AIActionAttackObject,
    +[](CNWSCreature *pCreature, CNWSObjectActionNode *pNode) -> uint32_t
    {
        const ObjectID oidNodeTarget  = pCreature->m_oidCurrentActionTarget;
        const ObjectID oidAttemptedIn = pCreature->m_oidAttemptedAttackTarget;

        auto retVal = s_AIActionAttackObjectHook->CallOriginal<uint32_t>(pCreature, pNode);

        if (oidNodeTarget != Constants::OBJECT_INVALID &&
            pCreature->m_oidCurrentActionTarget == oidNodeTarget &&
            pCreature->m_oidAttemptedAttackTarget != oidAttemptedIn &&
            pCreature->m_oidAttemptedAttackTarget != oidNodeTarget &&
            pCreature->m_oidAttemptedAttackTarget != Constants::OBJECT_INVALID)
        {
            pCreature->m_oidAttemptedAttackTarget = oidNodeTarget;
        }

        return retVal;
    }, Hooks::Order::Early);

// RISENHOLM MODIFICATION: use a single-use Cast Spell item at once.
//
// The item counterpart of NWNX_Creature_AddCastSpellActions's bInstant, which
// the module's QuickUse Menu uses to drink potions and read scrolls out
// of combat the way QuickCast's Autocast casts buffs: no action queued, no
// drink or read animation, no conjure time. The engine offers no such thing
// for items -- AddItemCastSpellActions has no instant flag -- so this is the
// completion half of CNWSCreature::AIActionItemCastSpell, done directly.
// Disassembled at 0x499f60 (8193.37-17); the order of each step below is the
// engine's.
//
// It deliberately goes through CNWSObject::SpellCastAndImpact with the item
// id, exactly as the engine does, rather than casting the spell some other
// way. That is what makes the use indistinguishable from an ordinary one to
// everything downstream: NWNX_ON_CAST_SPELL fires with ITEM_OBJECT_ID set (the
// module's Stability cost and free item uses live there), GetSpellCastItem
// returns the item in the spell script, and the caster level is the item's
// iprp_spells CasterLvl.
//
// Consumption also follows the engine, and runs AFTER the cast for the same
// reason it does there: a free use refunds by adding one to the stack from
// inside the cast event, and the decrement here then nets it out. The last of
// a stack is destroyed 500ms after the spell's projectile time rather than on
// the spot, because the impact script is queued, not run inline, and still
// needs GetSpellCastItem to answer.
//
// Single-use properties only. Charges and uses/day go through other branches
// of the engine's consumption switch and nothing here needs them.
NWNX_EXPORT ArgumentStack UseItemInstant(ArgumentStack&& args)
{
    auto *pCreature = Utils::PopCreature(args);
    auto *pItem = Utils::PopItem(args);
    const auto oidTarget = args.extract<ObjectID>();

    if (!pCreature || !pItem || pCreature->GetDead() || pCreature->GetIsPCDying())
        return false;

    // A dormant item's powers are asleep -- see GetIsItemDormant. This skips
    // AddItemCastSpellActions, so it needs the refusal of its own.
    if (Risenholm::GetIsWornItemDormant(pItem))
        return false;

    auto *pTarget = Utils::AsNWSObject(Utils::GetGameObject(oidTarget));
    if (!pTarget || pTarget->m_oidArea != pCreature->m_oidArea)
        return false;

    // Carried by the creature, loose or one bag down -- the same reach the
    // inventory's own Use has.
    if (pItem->m_oidPossessor != pCreature->m_idSelf)
    {
        auto *pBag = Utils::AsNWSItem(Utils::GetGameObject(pItem->m_oidPossessor));
        if (!pBag || pBag->m_oidPossessor != pCreature->m_idSelf)
            return false;
    }

    if (!pCreature->CanUseItem(pItem, false))
        return false;

    // Refuse while a spell or item cast is running. Both keep their state in
    // the very m_nLastSpell* / m_bLastSpellCast fields written below, so
    // cutting in would clobber the conjure in progress -- with m_bLastSpellCast
    // left set, its own SpellCastAndImpact would then do nothing at all. 15
    // and 17 are the ids AddCastSpellActions and AddItemCastSpellActions queue.
    // Only the head matters: a cast still waiting in the queue writes those
    // fields afresh when it starts.
    if (auto *pHead = pCreature->m_lQueuedActions.m_pcExoLinkedListInternal->pHead)
    {
        auto *pAction = static_cast<CNWSObjectActionNode*>(pHead->pObject);
        if (pAction && (pAction->m_nActionId == 15 || pAction->m_nActionId == 17))
            return false;
    }

    CNWItemProperty *pProperty = nullptr;
    for (int32_t i = 0; i < pItem->m_lstActiveProperties.num; i++)
    {
        auto *pCandidate = pItem->GetActiveProperty(i);

        if (pCandidate && pCandidate->m_nPropertyName == Constants::ItemProperty::CastSpell &&
            pCandidate->m_nCostTableValue == 1 && pCandidate->m_bUseable)
        {
            pProperty = pCandidate;
            break;
        }
    }

    if (!pProperty)
        return false;

    auto *pIPRPSpells = Globals::Rules()->m_p2DArrays->GetIPRPSpells();
    int32_t nSpellId = -1;
    int32_t nCasterLevel = 0;

    if (!pIPRPSpells->GetINTEntry(pProperty->m_nSubType, "SpellIndex", &nSpellId) || nSpellId < 0)
        return false;

    pIPRPSpells->GetINTEntry(pProperty->m_nSubType, "CasterLvl", &nCasterLevel);

    // The fields AIActionItemCastSpell fills in just before its own
    // SpellCastAndImpact call. m_bLastSpellCast in particular must be clear:
    // SpellCastAndImpact does nothing at all while it is set.
    const Vector vTarget = pTarget->m_vPosition;

    pCreature->m_oidSpellTarget = oidTarget;
    pCreature->m_vLastSpellTarget = vTarget;
    pCreature->m_oidLastSpellTarget = oidTarget;
    pCreature->m_nLastSpellId = nSpellId;
    pCreature->m_bLastSpellCast = false;
    pCreature->m_bLastSpellCastSpontaneous = false;
    pCreature->m_nLastSpellCastMetaType = 0;
    pCreature->m_nLastSpellCastFeat = 0xFFFF;
    pCreature->m_oidLastSpellCastItem = pItem->m_idSelf;
    pCreature->m_bLastItemCastSpell = true;
    pCreature->m_nLastItemCastSpellLevel = nCasterLevel;
    pItem->m_bRecalculateCost = true;
    pCreature->CalculateLastSpellProjectileTime(0);

    pCreature->SpellCastAndImpact(nSpellId, vTarget, oidTarget, 0xFF, pItem->m_idSelf, false, false, 0, false);

    // The engine's single-use consumption. A plot item's last use leaves the
    // property spent but keeps the item.
    bool bDestroy = false;

    if (pItem->m_nStackSize > 1)
        pItem->m_nStackSize--;
    else
    {
        pProperty->m_bUseable = false;
        bDestroy = !pItem->m_bPlotObject;
    }

    if (bDestroy)
    {
        Globals::AppManager()->m_pServerExoApp->GetServerAIMaster()->AddEventDeltaTime(0,
            pCreature->m_nLastSpellProjectileTime + 500, pCreature->m_idSelf, pItem->m_idSelf,
            Constants::AIMasterEvent::DestroyObject);
    }
    else
        pItem->UpdateUsedActiveProperties(false);

    return true;
}
// END RISENHOLM MODIFICATION

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


// ---------------------------------------------------------------------------
// Forced walk
// ---------------------------------------------------------------------------
//
// Stands in for NWNX_Player_SetAlwaysWalk, which is broken in a way that
// matters here. Both write the same engine flag, CNWSCreature::m_bForcedWalk,
// which is what stops a creature running. The engine only ever sets that flag
// from CNWSEffectListHandler::OnApplyLimitMovementSpeed and clears it from
// OnRemoveLimitMovementSpeed, keyed on effect true-type 59,
// EFFECT_MOVEMENT_SPEED_DECREASE -- the engine's own numbering, not the
// EFFECT_TYPE_* constant of that name in nwscript.nss.
//
// That single effect is also how the engine expresses stealth, Slow and
// encumbrance. CNWSCreature::SetStealthMode calls ComputeModifiedMovementRate,
// which sums the detect-mode, stealth and encumbrance ruleset penalties and
// applies one type-59 effect for the total. So an override that clears
// m_bForcedWalk has to first ask whether one of those is holding it.
//
// Upstream tries to, with a std::bsearch over m_appliedEffects whose comparator
// casts its arguments straight to CGameEffect*. The list holds CGameEffect*, so
// what the comparator is handed is CGameEffect**, and reading ->m_nType off
// that reads two bytes out of the pointer slot rather than out of the effect --
// for the search key and for every element. It has never matched anything. The
// symptom players saw was that toggling Force Walk off while sneaking cleared
// the stealth walk lock along with their own, letting them run while hidden.
//
// Deliberately kept here rather than fixed in Plugins/Player: that is upstream's
// file and the fix would be lost at the next canon merge. pw_sp_forcewalk.nss
// and pw_mod_enter.nss call this instead, and nothing in the module calls the
// Player version any more, so only one of the two OnRemoveLimitMovementSpeed
// hooks is ever installed.
//
// Engine behaviour above was read from the 8193.37 disassembly, 2026-09-22, not
// inferred: m_bForcedWalk (offset 0x764) is written in exactly two places, both
// in CNWSEffectListHandler, and ComputeModifiedMovementRate's tail applies
// EFFECT_MOVEMENT_SPEED_DECREASE with SetInteger(0, 1).

constexpr uint16_t RH_EFFECT_MOVEMENT_SPEED_DECREASE = 59;

// Creatures whose walk we are forcing. Deliberately not the persistent nwnx
// object variable upstream marks them with: m_bForcedWalk is per-session state,
// and a marker that outlives the session can only ever disagree with it. The
// player's *preference* is module state -- the PC local IS_FORCE_WALK_ON, which
// pw_mod_enter re-pushes on login. A set lookup is also far cheaper than
// POS::Get, which allocates object storage on a miss and copies the object's
// entire int map on a hit; that matters in a hook which fires for every
// creature in the world, not just players.
static std::unordered_set<ObjectID> s_ForcedWalk;

static bool RH_HasMovementLimitEffect(CNWSObject *pObject)
{
    auto &effects = pObject->m_appliedEffects;

    for (int32_t i = 0; i < effects.num; i++)
    {
        if (!effects.element[i]) continue;

        const uint16_t nType = effects.element[i]->m_nType;

        // The list is kept sorted ascending by type, which is what lets the
        // engine's own loop in OnRemoveLimitMovementSpeed stop early as well.
        if (nType > RH_EFFECT_MOVEMENT_SPEED_DECREASE) break;
        if (nType == RH_EFFECT_MOVEMENT_SPEED_DECREASE) return true;
    }

    return false;
}

// Without this, any other movement limit expiring takes our override with it:
// the engine recomputes the flag from the effects that remain and knows nothing
// about us. Sneaking and then unsneaking with Force Walk on was enough to do it.
static Hooks::Hook s_OnRemoveLimitMovementSpeedHook =
    Hooks::HookFunction(&CNWSEffectListHandler::OnRemoveLimitMovementSpeed,
    +[](CNWSEffectListHandler *pThis, CNWSObject *pObject, CGameEffect *pEffect) -> int32_t
    {
        auto it = s_ForcedWalk.find(pObject->m_idSelf);

        if (it != s_ForcedWalk.end())
        {
            auto *pCreature = Utils::AsNWSCreature(pObject);

            // Still the player we set it on, so hold the flag.
            if (pCreature && pCreature->m_bPlayerCharacter)
                return 1;

            // The object id has been recycled onto something else since that
            // player left. Drop the stale entry and let the engine have its way.
            s_ForcedWalk.erase(it);
        }

        return s_OnRemoveLimitMovementSpeedHook->CallOriginal<int32_t>(pThis, pObject, pEffect);
    }, Hooks::Order::Late);

NWNX_EXPORT ArgumentStack SetAlwaysWalk(ArgumentStack&& args)
{
    auto *pCreature = Utils::AsNWSCreature(Utils::GetGameObject(args.extract<ObjectID>()));
    const auto bWalk = args.extract<int32_t>();

    if (!pCreature) return {};

    if (bWalk)
    {
        s_ForcedWalk.insert(pCreature->m_idSelf);
        pCreature->m_bForcedWalk = true;
        return {};
    }

    s_ForcedWalk.erase(pCreature->m_idSelf);

    // Hand the flag back to whatever else is holding it, if anything is.
    pCreature->m_bForcedWalk = RH_HasMovementLimitEffect(pCreature);

    // Encumbrance reaches m_bForcedWalk through a type-59 effect like everything
    // else, so the scan above has normally already caught it. Kept because it is
    // the one source with a state field of its own to consult, and re-deriving
    // it costs nothing on a path that runs once per button press.
    if (!pCreature->m_bForcedWalk)
    {
        pCreature->UpdateEncumbranceState(false);
        pCreature->m_bForcedWalk = (pCreature->m_nEncumbranceState != 0);
    }

    return {};
}

// ---- Read-only inventories of other creatures ----
//
// A player's "other inventory" panel (CNWSPlayer::m_pOtherInventoryGUI) exists
// so they can manage a henchman's pack, and the engine trusts it completely:
// for any item whose possessor is that panel's owner, it carries out the
// player's request on the owner's behalf. Read from the decompilation, 8193.37:
//
//   HandlePlayerToServerInventoryMessage   equip onto the owner (minor 1), and
//                                          move/unequip out of the owner's
//                                          inventory into any repository,
//                                          the player's own included (minor 7)
//   HandlePlayerToServerInputMessage       split and merge the owner's stacks
//   HandlePlayerToServerGroupInputMessage  the same, as a group input
//   HandlePlayerToServerStoreMessage       RequestSell sells the owner's items
//                                          for the player's gold; RequestBuy
//                                          buys into the owner's inventory
//
// None of those ask whether the player is a DM, or whether the owner is an
// associate of theirs -- and nothing restricts who the owner can be. The
// client sets it itself with GuiInventory minor 1 (any object id, no checks),
// and NWNX_Player_OpenInventory sets it to anyone. So opening another PC's
// inventory to let a player LOOK, which is what /search does, also let them
// take everything that is not cursed.
//
// The fix: while a non-DM's panel shows anyone other than their own body, the
// creature they drive, or an associate of either, any message on those four
// handlers that names that owner -- or an item the owner carries, bags
// included -- is refused before the engine reads it: the message is consumed
// and, for the inventory minors, the matching cancel is sent so the client
// drops the item back where it was. Store messages are refused outright while
// such a panel is open, since selling is the one path whose layout was not
// worth guessing at and nobody needs a merchant mid-search.
//
// DISPROVED, 2026-09-23: the first version faked the panel closed for the
// length of each handler instead (open 0, owner OBJECT_INVALID), on the theory
// that every trust check would then fail the way it does with no panel open.
// Tested in game, the searcher could still rearrange the target's pack and drag
// items out of it into their own. With the owner blanked, Unequip (minor 7)
// does not refuse a stranger's item; it takes its OTHER branch and queues the
// move on the searcher's own creature, with the target's item, and nothing on
// that path checks whose the item is. Blanking rerouted the theft rather than
// stopping it, so do not go back to it.
//
// Input and group input only refuse the owner's ITEMS, never the owner
// themselves: those messages also carry the creature being attacked, cast on,
// or healed, and a search must not stop anyone doing any of that to the person
// being searched. UseObject on a container is let through too -- it is how the
// client opens a bag inside the other panel, through AIActionUseObject and
// CNWSItem::OpenInventory, and a bag's contents are part of what is on show.
// Inventory messages refuse the owner as well, since there it can only be a
// destination repository, i.e. an item being planted on them.
//
// Peeked fields: the first three DWORDs of the message, as object ids -- the
// item, then the destination or target, in every item message these handlers
// take (Equip: item, creature, slot; Unequip and RepositoryMove: item,
// repository, x, y; split: item, count). A field that is not an object id is
// vanishingly unlikely to equal one of the owner's items, and even then the
// only cost is one refused input while a search is open.
//
// Hooked at Latest so NWNX_Events' Early hooks, and the module scripts they
// run, still see every message as sent.

static bool GetIsTrustedOtherInventoryOwner(CNWSPlayer *pPlayer, ObjectID oidOwner)
{
    if (oidOwner == Constants::OBJECT_INVALID)
        return true;

    if (pPlayer->GetIsDM())
        return true;

    // Their own body while driving something else, and whatever they drive.
    if (oidOwner == pPlayer->m_oidNWSObject || oidOwner == pPlayer->m_oidPCObject)
        return true;

    auto *pOwner = Utils::AsNWSCreature(Utils::GetGameObject(oidOwner));
    if (!pOwner)
        return false;

    return pOwner->m_oidMaster != Constants::OBJECT_INVALID &&
           (pOwner->m_oidMaster == pPlayer->m_oidNWSObject || pOwner->m_oidMaster == pPlayer->m_oidPCObject);
}

// The owner of oPlayer's other-inventory panel when it is open on someone
// they have no business handling, or OBJECT_INVALID.
static ObjectID GetUntrustedOtherInventoryOwner(CNWSPlayer *pPlayer)
{
    if (!pPlayer || !pPlayer->m_pOtherInventoryGUI || !pPlayer->m_pOtherInventoryGUI->m_bGuiInventoryOpen)
        return Constants::OBJECT_INVALID;

    const ObjectID oidOwner = pPlayer->m_pOtherInventoryGUI->m_oidInventoryOwner;

    return GetIsTrustedOtherInventoryOwner(pPlayer, oidOwner) ? Constants::OBJECT_INVALID : oidOwner;
}

// Utils::PeekMessage without the bounds check it does not do. Masked the way
// NWNX_Events masks the ids it peeks.
static bool PeekMessageObjectID(CNWSMessage *pMessage, int32_t offset, ObjectID &oid)
{
    if (!pMessage || !pMessage->m_pnReadBuffer ||
        pMessage->m_nReadBufferPtr + offset + sizeof(ObjectID) > pMessage->m_nReadBufferSize)
        return false;

    oid = Utils::PeekMessage<ObjectID>(pMessage, offset) & 0x7FFFFFFF;
    return true;
}

static bool GetIsOwnerOrOwnersItem(ObjectID oid, ObjectID oidOwner, bool bOwnerItself)
{
    if (oid == oidOwner)
        return bOwnerItself;

    auto *pItem = Utils::AsNWSItem(Utils::GetGameObject(oid));
    if (!pItem)
        return false;

    ObjectID oidHolder = pItem->m_oidPossessor;
    if (auto *pBag = Utils::AsNWSItem(Utils::GetGameObject(oidHolder)))
        oidHolder = pBag->m_oidPossessor;

    return oidHolder == oidOwner;
}

static bool GetMessageTouchesOwner(CNWSMessage *pMessage, ObjectID oidOwner, bool bOwnerItself)
{
    for (int32_t offset = 0; offset <= 8; offset += 4)
    {
        ObjectID oid;
        if (PeekMessageObjectID(pMessage, offset, oid) && GetIsOwnerOrOwnersItem(oid, oidOwner, bOwnerItself))
            return true;
    }

    return false;
}

static bool GetIsContainerItem(ObjectID oid)
{
    auto *pItem = Utils::AsNWSItem(Utils::GetGameObject(oid));
    return pItem && pItem->m_pItemRepository;
}

static void RefuseOtherInventoryMessage(CNWSPlayer *pPlayer)
{
    Utils::ClearReadMessage();

    if (auto *pCreature = Utils::AsNWSCreature(Utils::GetGameObject(pPlayer->m_oidNWSObject)))
        pCreature->SendFeedbackString("You can look, but not handle anything.");
}

static Hooks::Hook s_ReadOnlyInventoryMessageHook = Hooks::HookFunction(&CNWSMessage::HandlePlayerToServerInventoryMessage,
    +[](CNWSMessage *pThis, CNWSPlayer *pPlayer, uint8_t nMinor) -> int32_t
    {
        const ObjectID oidOwner = GetUntrustedOtherInventoryOwner(pPlayer);

        if (oidOwner == Constants::OBJECT_INVALID || !GetMessageTouchesOwner(pThis, oidOwner, true))
            return s_ReadOnlyInventoryMessageHook->CallOriginal<int32_t>(pThis, pPlayer, nMinor);

        ObjectID oidItem = Constants::OBJECT_INVALID;
        PeekMessageObjectID(pThis, 0, oidItem);

        uint32_t nSlot = 0;
        if (pThis->m_nReadBufferPtr + 12 <= pThis->m_nReadBufferSize)
            nSlot = Utils::PeekMessage<uint32_t>(pThis, 8);

        RefuseOtherInventoryMessage(pPlayer);

        using namespace Constants::MessageInventoryMinor;
        switch (nMinor)
        {
            case Equip:          pThis->SendServerToPlayerInventory_EquipCancel(pPlayer->m_nPlayerID, oidItem, nSlot); break;
            case Drop:           pThis->SendServerToPlayerInventory_DropCancel(pPlayer->m_nPlayerID, oidItem);         break;
            case Pickup:         pThis->SendServerToPlayerInventory_PickupCancel(pPlayer->m_nPlayerID, oidItem);       break;
            case Unequip:        pThis->SendServerToPlayerInventory_UnequipCancel(pPlayer->m_nPlayerID, oidItem);      break;
            case RepositoryMove: pThis->SendServerToPlayerInventory_RepositoryMoveCancel(pPlayer->m_nPlayerID, oidItem); break;
            default: break;
        }

        return true;
    }, Hooks::Order::Latest);

static Hooks::Hook s_ReadOnlyInputMessageHook = Hooks::HookFunction(&CNWSMessage::HandlePlayerToServerInputMessage,
    +[](CNWSMessage *pThis, CNWSPlayer *pPlayer, uint8_t nMinor) -> int32_t
    {
        const ObjectID oidOwner = GetUntrustedOtherInventoryOwner(pPlayer);

        if (oidOwner != Constants::OBJECT_INVALID && GetMessageTouchesOwner(pThis, oidOwner, false))
        {
            ObjectID oidObject;
            const bool bOpeningBag = nMinor == Constants::MessageInputMinor::UseObject &&
                                     PeekMessageObjectID(pThis, 0, oidObject) && GetIsContainerItem(oidObject);

            if (!bOpeningBag)
            {
                RefuseOtherInventoryMessage(pPlayer);
                return true;
            }
        }

        return s_ReadOnlyInputMessageHook->CallOriginal<int32_t>(pThis, pPlayer, nMinor);
    }, Hooks::Order::Latest);

static Hooks::Hook s_ReadOnlyGroupInputMessageHook = Hooks::HookFunction(&CNWSMessage::HandlePlayerToServerGroupInputMessage,
    +[](CNWSMessage *pThis, CNWSPlayer *pPlayer, uint8_t nMinor) -> int32_t
    {
        const ObjectID oidOwner = GetUntrustedOtherInventoryOwner(pPlayer);

        if (oidOwner != Constants::OBJECT_INVALID && GetMessageTouchesOwner(pThis, oidOwner, false))
        {
            RefuseOtherInventoryMessage(pPlayer);
            return true;
        }

        return s_ReadOnlyGroupInputMessageHook->CallOriginal<int32_t>(pThis, pPlayer, nMinor);
    }, Hooks::Order::Latest);

static Hooks::Hook s_ReadOnlyStoreMessageHook = Hooks::HookFunction(&CNWSMessage::HandlePlayerToServerStoreMessage,
    +[](CNWSMessage *pThis, CNWSPlayer *pPlayer, uint8_t nMinor) -> int32_t
    {
        if (GetUntrustedOtherInventoryOwner(pPlayer) != Constants::OBJECT_INVALID)
        {
            RefuseOtherInventoryMessage(pPlayer);
            return true;
        }

        return s_ReadOnlyStoreMessageHook->CallOriginal<int32_t>(pThis, pPlayer, nMinor);
    }, Hooks::Order::Latest);

// Whose inventory oPlayer's other-inventory panel is showing, or
// OBJECT_INVALID if it is closed. The module polls this to close a /search
// view when the two separate, and to stop polling once the searcher has
// closed it themselves.
NWNX_EXPORT ArgumentStack GetOtherInventoryOwner(ArgumentStack&& args)
{
    ObjectID oidOwner = Constants::OBJECT_INVALID;

    if (auto *pPlayer = Utils::PopPlayer(args))
    {
        if (auto *pGUI = pPlayer->m_pOtherInventoryGUI; pGUI && pGUI->m_bGuiInventoryOpen)
            oidOwner = pGUI->m_oidInventoryOwner;
    }

    return oidOwner;
}

// ---- Items concealed from one viewer ----
//
// /hideitem lets a character keep an item out of a /search, and the searcher
// only sees it if they win a roll-off for it. The search view is the engine's
// own other-inventory panel, so hiding has to happen in what the engine
// streams to that one client.
//
// Every item in that panel -- the backpack (update list 1) and a bag opened
// inside it (list 0) -- reaches the client through
// CNWSMessage::WriteRepositoryUpdate. It walks the repository's item list
// from the tail with GetPrev and diffs it against a per-viewer, per-panel
// "last sent" list (CNWSPlayerLUOInventory), sending A(dd) for anything new
// and D(elete) for anything gone. So for the length of one call for one
// viewer, the concealed items' nodes are taken out of the list; the diff never
// adds them, and deletes them if an earlier update had sent them. The nodes
// are relinked in reverse order the moment the call returns -- the same node
// objects, with their own pPrev/pNext never touched, so the list comes back
// exactly as it was. Nothing else runs in between; the server is single
// threaded here.
//
// Only lists 0 and 1, and only repositories owned by the registered owner or
// by a bag that owner is carrying. The other caller is the barter window
// (list 2), and an item hidden from a search must never be hidden from
// someone about to accept it in a trade. The viewer's own inventory is not
// the owner's, so an item that somehow changes hands stays visible to its
// new holder.
//
// Registered per viewer by the module when a search opens and cleared when
// it closes; with nothing registered the hook returns straight away.

struct ConcealedItems
{
    ObjectID oidOwner = Constants::OBJECT_INVALID;
    std::unordered_set<ObjectID> items;
};

static std::unordered_map<ObjectID, ConcealedItems> s_ConcealedItems;

static const ConcealedItems *FindConcealedItems(CNWSPlayer *pPlayer)
{
    if (s_ConcealedItems.empty() || !pPlayer)
        return nullptr;

    auto it = s_ConcealedItems.find(pPlayer->m_oidNWSObject);
    if (it == s_ConcealedItems.end())
        it = s_ConcealedItems.find(pPlayer->m_oidPCObject);

    return (it == s_ConcealedItems.end() || it->second.items.empty()) ? nullptr : &it->second;
}

static bool GetIsRepositoryOfOwner(CItemRepository *pRepository, ObjectID oidOwner)
{
    if (pRepository->m_oidParent == oidOwner)
        return true;

    auto *pBag = Utils::AsNWSItem(Utils::GetGameObject(pRepository->m_oidParent));
    return pBag && pBag->m_oidPossessor == oidOwner;
}

static Hooks::Hook s_ConcealWriteRepositoryUpdateHook = Hooks::HookFunction(&CNWSMessage::WriteRepositoryUpdate,
    +[](CNWSMessage *pThis, CNWSPlayer *pPlayer, CNWSObject *pPlayerGameObject, CItemRepository *pRepository,
        CNWSPlayerLUOInventory *pLastUpdateInventory, uint8_t nLastUpdateList, char cGuiElementByte,
        uint8_t nCurrentPanel) -> void
    {
        auto CallOriginal = [&]()
        {
            s_ConcealWriteRepositoryUpdateHook->CallOriginal<void>(pThis, pPlayer, pPlayerGameObject, pRepository,
                pLastUpdateInventory, nLastUpdateList, cGuiElementByte, nCurrentPanel);
        };

        const auto *pConcealed = FindConcealedItems(pPlayer);

        if (!pConcealed || nLastUpdateList > 1 || !pRepository ||
            !GetIsRepositoryOfOwner(pRepository, pConcealed->oidOwner))
        {
            CallOriginal();
            return;
        }

        auto *pList = pRepository->m_oidItems.m_pcExoLinkedListInternal;
        std::vector<CExoLinkedListNode*> unlinked;

        for (auto *pNode = pList->pHead; pNode; )
        {
            auto *pNext = pNode->pNext;
            auto *pOid  = static_cast<ObjectID*>(pNode->pObject);

            if (pOid && pConcealed->items.count(*pOid))
            {
                if (pNode->pPrev) pNode->pPrev->pNext = pNode->pNext; else pList->pHead = pNode->pNext;
                if (pNode->pNext) pNode->pNext->pPrev = pNode->pPrev; else pList->pTail = pNode->pPrev;
                pList->m_nCount--;
                unlinked.push_back(pNode);
            }

            pNode = pNext;
        }

        CallOriginal();

        for (auto it = unlinked.rbegin(); it != unlinked.rend(); ++it)
        {
            auto *pNode = *it;
            if (pNode->pPrev) pNode->pPrev->pNext = pNode; else pList->pHead = pNode;
            if (pNode->pNext) pNode->pNext->pPrev = pNode; else pList->pTail = pNode;
            pList->m_nCount++;
        }
    }, Hooks::Order::Latest);

// Hide oItem, which oOwner is carrying, from oViewer's view of oOwner's
// inventory. Registering against a different owner drops what was registered
// for the last one.
NWNX_EXPORT ArgumentStack ConcealItemFromViewer(ArgumentStack&& args)
{
    const auto oidViewer = args.extract<ObjectID>();
    const auto oidOwner  = args.extract<ObjectID>();
    const auto oidItem   = args.extract<ObjectID>();

    if (oidViewer == Constants::OBJECT_INVALID || oidOwner == Constants::OBJECT_INVALID || oidItem == Constants::OBJECT_INVALID)
        return {};

    auto &entry = s_ConcealedItems[oidViewer];

    if (entry.oidOwner != oidOwner)
    {
        entry.oidOwner = oidOwner;
        entry.items.clear();
    }

    entry.items.insert(oidItem);

    return {};
}

NWNX_EXPORT ArgumentStack ClearConcealedItems(ArgumentStack&& args)
{
    s_ConcealedItems.erase(args.extract<ObjectID>());

    return {};
}

// Whether oItem is registered as concealed from oViewer. The module checks one
// registration with this before opening a search view, so a server on a build
// without concealment refuses the search instead of showing hidden items.
NWNX_EXPORT ArgumentStack GetIsItemConcealedFrom(ArgumentStack&& args)
{
    const auto oidViewer = args.extract<ObjectID>();
    const auto oidItem   = args.extract<ObjectID>();

    auto it = s_ConcealedItems.find(oidViewer);

    return (int32_t)(it != s_ConcealedItems.end() && it->second.items.count(oidItem) != 0);
}

// ---- "[Hidden]" under a hidden item's name, for its holder only ----
//
// The holder needs to see which of their items /hideitem has marked, and a
// searcher must never see it: a revealed item that says [Hidden] would tell
// them it was deliberately hidden. A SetName would be global, so the item's
// real name is left alone and the extra line is added per viewer instead, at
// the three places an item's name is sent to a client (read from the
// 8193.37 disassembly):
//
//   AddActiveItemPropertiesToMessage       every item as it is listed in a
//                                          panel -- own pack, search panel,
//                                          bags, barter -- copying m_sName
//                                          (CNWSItem+0x418) into the message
//   SendServerToPlayerUpdateItemName       SetName, and NWNX_Player_UpdateItemName,
//                                          which /hideitem calls after a toggle
//   SendServerToPlayerExamineGui_ItemData  the examine window
//
// For the length of one call, when the viewer is the character carrying the
// item (bags included) AND the item's SEARCH_HIDDEN_BY local holds that
// character's UUID AND it is not equipped, m_sName is swapped for the name
// with "\n[Hidden]" beneath it -- the same two-line shape as a Powered item's
// tooltip -- and put back as soon as the call returns. Equipped items are in
// plain sight whatever the mark says, so they do not get the line. Anyone else
// is sent the name exactly as it is.
//
// An unidentified item shows its base name on the client whatever name is
// sent, so it does not get the line either -- a limitation, not a leak.

static bool GetShowsHiddenLine(CNWSPlayer *pPlayer, CNWSItem *pItem)
{
    if (!pPlayer || !pItem)
        return false;

    auto *pVars = Utils::GetScriptVarTable(pItem);
    if (!pVars)
        return false;

    CExoString sVarName = "SEARCH_HIDDEN_BY";
    const CExoString sHiddenBy = pVars->GetString(sVarName);
    if (sHiddenBy.IsEmpty())
        return false;

    ObjectID oidHolder = pItem->m_oidPossessor;
    if (auto *pBag = Utils::AsNWSItem(Utils::GetGameObject(oidHolder)))
        oidHolder = pBag->m_oidPossessor;

    if (oidHolder == Constants::OBJECT_INVALID ||
        (oidHolder != pPlayer->m_oidNWSObject && oidHolder != pPlayer->m_oidPCObject))
        return false;

    auto *pHolder = Utils::AsNWSCreature(Utils::GetGameObject(oidHolder));
    if (!pHolder)
        return false;

    if (pHolder->m_pInventory && pHolder->m_pInventory->GetItemInInventory(pItem))
        return false;

    return pHolder->m_pUUID.GetOrAssignRandom() == sHiddenBy;
}

class HiddenItemNameScope
{
public:
    HiddenItemNameScope(CNWSPlayer *pPlayer, CNWSItem *pItem)
    {
        if (!GetShowsHiddenLine(pPlayer, pItem))
            return;

        const std::string sName = Utils::ExtractLocString(pItem->m_sName);
        if (sName.empty())
            return;

        m_pItem  = pItem;
        m_sSaved = pItem->m_sName;
        pItem->m_sName = Utils::CreateLocString(sName + "\n[Hidden]");
    }

    ~HiddenItemNameScope()
    {
        if (m_pItem)
            m_pItem->m_sName = m_sSaved;
    }

    HiddenItemNameScope(const HiddenItemNameScope&) = delete;
    HiddenItemNameScope& operator=(const HiddenItemNameScope&) = delete;

private:
    CNWSItem *m_pItem = nullptr;
    CExoLocString m_sSaved;
};

static Hooks::Hook s_HiddenNameActivePropertiesHook = Hooks::HookFunction(&CNWSMessage::AddActiveItemPropertiesToMessage,
    +[](CNWSMessage *pThis, CNWSPlayer *pPlayer, CNWSItem *pItem, CNWSCreature *pCreature) -> void
    {
        HiddenItemNameScope scope(pPlayer, pItem);
        s_HiddenNameActivePropertiesHook->CallOriginal<void>(pThis, pPlayer, pItem, pCreature);
    }, Hooks::Order::Latest);

static Hooks::Hook s_HiddenNameUpdateItemNameHook = Hooks::HookFunction(&CNWSMessage::SendServerToPlayerUpdateItemName,
    +[](CNWSMessage *pThis, CNWSPlayer *pPlayer, CNWSItem *pItem) -> int32_t
    {
        HiddenItemNameScope scope(pPlayer, pItem);
        return s_HiddenNameUpdateItemNameHook->CallOriginal<int32_t>(pThis, pPlayer, pItem);
    }, Hooks::Order::Latest);

static Hooks::Hook s_HiddenNameExamineItemHook = Hooks::HookFunction(&CNWSMessage::SendServerToPlayerExamineGui_ItemData,
    +[](CNWSMessage *pThis, CNWSPlayer *pPlayer, ObjectID oidItem) -> int32_t
    {
        HiddenItemNameScope scope(pPlayer, Utils::AsNWSItem(Utils::GetGameObject(oidItem)));
        return s_HiddenNameExamineItemHook->CallOriginal<int32_t>(pThis, pPlayer, oidItem);
    }, Hooks::Order::Latest);

// ---------------------------------------------------------------------------
// Damage bonus limit
// ---------------------------------------------------------------------------
//
// CNWSCreatureStats::GetDamageRoll adds the PHYSICAL damage bonus from effects
// to the weapon roll, and takes it from CNWSCreature::GetTotalEffectBonus with
// bElementalDamage off, which returns min(increases, limit) - min(decreases,
// limit) where limit is CServerExoApp::GetDamageBonusLimit() -- 100 by
// default. Elemental types go through ResolveElementalDamage instead, which
// adds each one straight onto the attack and never meets the limit (it skips
// damage indices 0-2, bludgeoning/piercing/slashing, so the physical ones are
// only ever counted in GetDamageRoll). Read from the 8193.37 disassembly.
//
// The module's threat system gives an Elite Solo creature a physical bonus of
// nearly 300, so every one of them was quietly hitting for +100.
//
// SetDamageBonusLimit the NWScript command can only go to 255: it passes the
// value through the DamageBonusLimit server setting's constraints before
// storing it. The engine setter itself takes any int, and with
// isModuleOverride writes the override that GetDamageBonusLimit returns
// whenever it is not negative. GetDamageRoll carries the total in a short, so
// anything up to 32767 survives the trip. Nothing else reads the limit.
NWNX_EXPORT ArgumentStack SetDamageBonusLimit(ArgumentStack&& args)
{
    const auto nLimit = args.extract<int32_t>();
    ASSERT_OR_THROW(nLimit >= 0);

    Globals::AppManager()->m_pServerExoApp->SetDamageBonusLimit(nLimit, true);

    return {};
}



// ---------------------------------------------------------------------------
// Level history: delevel for low-level content, relevel without the dialog
// ---------------------------------------------------------------------------
//
// Giving a PC back the XP it lost makes the client offer the level-up dialog,
// but the client never decides that for itself: it reads a flag the server
// computes as CNWSCreatureStats::CanLevelUp() when it writes the periodic
// player update (CNWCMessage::HandleServerToPlayerUpdate_PlayerInfo passes
// that BOOL straight to CGuiInGame::SetLevelUpMode). So if the levels are
// back before the next update goes out, the client never sees the flag.
//
// Each level's choices live in m_lstLevelStats, one CNWLevelStats per level:
// class, hit die, ability gain, skill rank changes, feats, and known spells
// added and removed. The engine already knows how to replay one --
// SetExperience, on an XP gain, runs LevelUp() on every entry the list holds
// past the current level -- but nothing is ever left there to replay:
// LevelDown removes the entry and deletes it, SaveClassInfo writes only
// GetLevel() entries, and ReadStatsFromGff reads only that many back. All
// read from the 8193.37 disassembly.
//
// GetLevelHistory copies the list out as JSON for the module to keep, and
// RestoreLevelHistory puts levels back from it with LevelUp(), the call
// SetExperience's replay and ValidateLevelUp both end in. The entries are
// replayed as stored, deliberately without ValidateLevelUp: the module's
// OnPlayerLevelUp rewrites each level after the engine applies it (feats by
// level, auto-levelled skill ranks, redistributed hit points, the ability
// gain revoked), so a stored entry is the finished level, not something a
// client could have submitted, and validating it would reject it. For the
// same reason no OnPlayerLevelUp fires; ValidateLevelUp is what queues it.

namespace
{

// A feat in a level's record that the creature no longer has was taken away
// after the level, and is left out. NWNX_Creature_RemoveFeat removes a feat
// from the creature's list only, never from the level that granted it, and
// the module relies on that in all 37 archetype scripts (pw_at_arch*): they
// add the archetype's feats by level but remove the placeholder
// FEAT_ARCHETYPE_SELECTION_* with plain RemoveFeat. Replaying the record
// as-is gave a Barbarian Agitator the selection feat back, and at the next
// level EnforceArchetypeSelection saw it and pushed him back down (seen
// 2026-09-29). m_lstFeats rather than HasFeat, which also counts
// m_lstBonusFeats, so a feat an item grants cannot keep a removed one here.
json LevelStatsToJson(CNWLevelStats *pLevelStats, CExoArrayList<uint16_t> &creatureFeats)
{
    json jSkills = json::array();
    for (uint16_t nSkill = 0; nSkill < Globals::Rules()->m_nNumSkills; nSkill++)
        jSkills.push_back((int32_t)pLevelStats->GetSkillRankChange(nSkill));

    json jFeats = json::array();
    for (int32_t i = 0; i < pLevelStats->m_lstFeats.num; i++)
    {
        const uint16_t nFeat = pLevelStats->m_lstFeats.element[i];
        if (creatureFeats.Contains(nFeat))
            jFeats.push_back(nFeat);
    }

    json jAdded = json::array();
    json jRemoved = json::array();
    for (int32_t nSpellLevel = 0; nSpellLevel < 10; nSpellLevel++)
    {
        json jAddedAtLevel = json::array();
        auto &added = pLevelStats->m_pAddedKnownSpellList[nSpellLevel];
        for (int32_t i = 0; i < added.num; i++)
            jAddedAtLevel.push_back(added.element[i]);
        jAdded.push_back(jAddedAtLevel);

        json jRemovedAtLevel = json::array();
        auto &removed = pLevelStats->m_pRemovedKnownSpellList[nSpellLevel];
        for (int32_t i = 0; i < removed.num; i++)
            jRemovedAtLevel.push_back(removed.element[i]);
        jRemoved.push_back(jRemovedAtLevel);
    }

    return {
        {"class",       pLevelStats->m_nClass},
        {"hitdie",      pLevelStats->m_nHitDie},
        {"ability",     pLevelStats->m_nAbilityGain},
        {"epic",        pLevelStats->m_bEpic ? 1 : 0},
        {"skillpoints", pLevelStats->m_nSkillPointsRemaining},
        {"skills",      jSkills},
        {"feats",       jFeats},
        {"known",       jAdded},
        {"unknown",     jRemoved},
    };
}

// Throws on a malformed entry; the caller owns the result.
CNWLevelStats *LevelStatsFromJson(const json &jLevel)
{
    auto pLevelStats = std::make_unique<CNWLevelStats>();

    pLevelStats->m_nClass = jLevel.at("class").get<uint8_t>();
    pLevelStats->m_nHitDie = jLevel.at("hitdie").get<uint8_t>();
    pLevelStats->m_nAbilityGain = jLevel.at("ability").get<uint8_t>();
    pLevelStats->m_bEpic = jLevel.at("epic").get<int32_t>();
    pLevelStats->m_nSkillPointsRemaining = jLevel.at("skillpoints").get<uint16_t>();

    const auto &jSkills = jLevel.at("skills");
    for (uint16_t nSkill = 0; nSkill < jSkills.size() && nSkill < Globals::Rules()->m_nNumSkills; nSkill++)
        pLevelStats->SetSkillRankChange(nSkill, (char)jSkills.at(nSkill).get<int32_t>());

    for (const auto &jFeat : jLevel.at("feats"))
        pLevelStats->AddFeat(jFeat.get<uint16_t>());

    const auto &jAdded = jLevel.at("known");
    const auto &jRemoved = jLevel.at("unknown");
    for (int32_t nSpellLevel = 0; nSpellLevel < 10; nSpellLevel++)
    {
        for (const auto &jSpell : jAdded.at(nSpellLevel))
            pLevelStats->m_pAddedKnownSpellList[nSpellLevel].Add(jSpell.get<uint32_t>());
        for (const auto &jSpell : jRemoved.at(nSpellLevel))
            pLevelStats->m_pRemovedKnownSpellList[nSpellLevel].Add(jSpell.get<uint32_t>());
    }

    return pLevelStats.release();
}

}

NWNX_EXPORT ArgumentStack GetLevelHistory(ArgumentStack&& args)
{
    json jLevels = json::array();
    json jClasses = json::array();

    if (auto *pCreature = Utils::PopCreature(args))
    {
        auto *pStats = pCreature->m_pStats;
        const int32_t nLevel = std::min<int32_t>(pStats->GetLevel(false), pStats->m_lstLevelStats.num);

        for (int32_t i = 0; i < nLevel; i++)
            jLevels.push_back(LevelStatsToJson(pStats->m_lstLevelStats.element[i], pStats->m_lstFeats));

        // LevelUp only sets domains and school when it adds a class the
        // creature does not have yet, so they have to be kept alongside the
        // levels for a class that deleveling removes entirely.
        for (uint8_t nMultiClass = 0; nMultiClass < pStats->m_nNumMultiClasses; nMultiClass++)
        {
            const auto &classInfo = pStats->m_ClassInfo[nMultiClass];
            jClasses.push_back({
                {"class",   classInfo.m_nClass},
                {"domain1", classInfo.m_nDomain[0]},
                {"domain2", classInfo.m_nDomain[1]},
                {"school",  classInfo.m_nSchool},
            });
        }
    }

    return JsonEngineStructure(json{{"levels", jLevels}, {"classes", jClasses}}, CExoString(""));
}

NWNX_EXPORT ArgumentStack RestoreLevelHistory(ArgumentStack&& args)
{
    auto *pCreature = Utils::PopCreature(args);
    const auto history = args.extract<JsonEngineStructure>();
    const auto nXP = args.extract<int32_t>();
    ASSERT_OR_THROW(nXP >= 0);

    if (!pCreature)
        return -1;

    auto *pStats = pCreature->m_pStats;
    const uint8_t nLevel = pStats->GetLevel(false);

    if (pStats->GetIsDM())
        return -1;

    // A list out of step with the level would have LevelUp replay the wrong
    // entries; a PC's never is, so refuse rather than guess.
    if (pStats->m_lstLevelStats.num != nLevel)
    {
        LOG_WARNING("RestoreLevelHistory: %x has %d level stats for level %d; refusing.",
            pCreature->m_idSelf, pStats->m_lstLevelStats.num, nLevel);
        return -1;
    }

    std::vector<std::unique_ptr<CNWLevelStats>> pending;
    std::unordered_map<uint8_t, json> classes;

    try
    {
        const auto &jHistory = history.m_shared->m_json;
        const auto &jLevels = jHistory.at("levels");

        // The levels the creature still has must be the history's own, class
        // for class, or this history belongs to someone else or to a build
        // that has since been changed by hand.
        if (jLevels.size() < nLevel)
        {
            LOG_WARNING("RestoreLevelHistory: %x is level %d but the history holds %d levels; refusing.",
                pCreature->m_idSelf, nLevel, (int32_t)jLevels.size());
            return -1;
        }

        for (uint8_t i = 0; i < nLevel; i++)
        {
            if (jLevels.at(i).at("class").get<uint8_t>() != pStats->m_lstLevelStats.element[i]->m_nClass)
            {
                LOG_WARNING("RestoreLevelHistory: %x level %d is not the history's class; refusing.",
                    pCreature->m_idSelf, i + 1);
                return -1;
            }
        }

        // Build every entry before applying any, so a malformed one leaves
        // the creature untouched.
        for (size_t i = nLevel; i < jLevels.size(); i++)
            pending.emplace_back(LevelStatsFromJson(jLevels.at(i)));

        for (const auto &jClass : jHistory.at("classes"))
            classes[jClass.at("class").get<uint8_t>()] = jClass;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("RestoreLevelHistory: malformed history for %x: %s", pCreature->m_idSelf, e.what());
        return -1;
    }

    // Written straight to the field rather than through SetExperience, which
    // would send "XP gained" and "you can level up" feedback and, on a gain,
    // run its own replay. CanLevelUp then gives the same stopping point as
    // that replay: the next level's XP, the module's level cap, and 40.
    pStats->m_nExperience = nXP;

    int32_t nApplied = 0;

    for (auto &pLevelStats : pending)
    {
        if (!pStats->CanLevelUp())
            break;

        uint8_t nDomain1 = 0, nDomain2 = 0, nSchool = 0;
        auto it = classes.find(pLevelStats->m_nClass);

        if (it != classes.end())
        {
            nDomain1 = it->second.value("domain1", 0);
            nDomain2 = it->second.value("domain2", 0);
            nSchool = it->second.value("school", 0);
        }

        // bAddStatsToList: the list takes ownership, as it does for a level
        // the client submits; LevelDown deletes it again.
        pStats->LevelUp(pLevelStats.release(), nDomain1, nDomain2, nSchool, true);
        nApplied++;
    }

    // What ValidateLevelUp does after LevelUp, plus the spell slot recount
    // LevelUpAutomatic does before it; LevelUp itself does neither.
    pStats->UpdateCombatInformation();
    pStats->UpdateNumberMemorizedSpellSlots();

    return nApplied;
}



// ---------------------------------------------------------------------------
// Item bonus spell slots only for spell levels the class can reach
// ---------------------------------------------------------------------------
//
// The engine keeps two kinds of bonus slot and gates only one of them.
// Ability bonus slots are worked out inside GetSpellGainWithBonus, which
// returns 0 whenever the class's spell gain table has **** in that column,
// so a high WIS never opens a level the class cannot cast yet. A Bonus Spell
// Slot item property instead bumps a per-class counter
// (CNWSCreatureStats_ClassInfo::m_nBonusSpellsList, via ModifyNumberBonusSpells)
// that three consumers add flat on top of the gated value, with no check at
// all (Ghidra decompilation of 8193.37, nwserver-re/export/game/
// CNWSCreatureStats.c):
//
//   UpdateNumberMemorizedSpellSlots  gain + bonus -> SetNumberMemorizedSpellSlots
//   ModifyNumberBonusSpells          the same, for the one level, on equip/unequip
//   AdjustSpellUsesPerDay            gain + bonus -> SetMaxSpellsPerDayLeft
//   ResetSpellsPerDayLeft            gain + bonus -> SpellsPerDayLeft, at rest
//
// So a level 3 cleric wearing a Cleric 4 slot item gets 0 + 1 = 1 slot at
// level 4, prepares a spell in it, and casts it. The module's own tooling
// (quickcast, metamagic restore) derives the castable ceiling from class
// level and never saw the slot, which is how it was noticed (2026-09-30).
//
// Each consumer is hooked and the result clamped to 0 for any level above 0
// whose GetSpellGainWithBonus is 0. That is the engine's own test for the
// ability slots, and it is exact for this module: every spell gain table it
// uses has a positive base count in every reachable column, so 0 means ****
// (the stock paladin and ranger tables have a 0 at class level 4, but the
// module ships its own with a 1 there). The item counter itself is left
// alone, so the slot appears by itself on the recount that follows the
// level-up that makes the level reachable, with no re-equip needed. Spells
// prepared in a slot that goes away are dropped with it.
//
// The two computing callers are hooked rather than SetNumberMemorizedSpellSlots
// beneath them: the setter is also how the slot lists are sized while a
// character loads, and clamping there would have to trust the class and
// ability fields being populated already. The client needs nothing -- its
// spellbook shows the count the server sends (CNWCCreatureStats::
// GetNumberMemorizedSpellSlots is a plain field read).

static bool IsUnreachableSpellLevel(CNWSCreatureStats *pStats, uint8_t nMultiClass, uint8_t nSpellLevel)
{
    return nSpellLevel > 0 && nSpellLevel < 10
        && nMultiClass < pStats->m_nNumMultiClasses
        && pStats->GetSpellGainWithBonus(nMultiClass, nSpellLevel) == 0;
}

static void ClearUnreachableMemorizedSlots(CNWSCreatureStats *pStats)
{
    for (uint8_t nMultiClass = 0; nMultiClass < pStats->m_nNumMultiClasses; nMultiClass++)
    {
        for (uint8_t nSpellLevel = 1; nSpellLevel < 10; nSpellLevel++)
        {
            if (pStats->GetNumberMemorizedSpellSlots(nMultiClass, nSpellLevel)
                && IsUnreachableSpellLevel(pStats, nMultiClass, nSpellLevel))
            {
                pStats->SetNumberMemorizedSpellSlots(nMultiClass, nSpellLevel, 0);
            }
        }
    }
}

static void ClearUnreachableSpellsPerDay(CNWSCreatureStats *pStats, uint8_t nMultiClass, uint8_t nSpellLevel)
{
    if (!IsUnreachableSpellLevel(pStats, nMultiClass, nSpellLevel))
        return;

    auto &classInfo = pStats->m_ClassInfo[nMultiClass];

    if (classInfo.GetMaxSpellsPerDayLeft(nSpellLevel) || classInfo.GetSpellsPerDayLeft(nSpellLevel))
    {
        classInfo.SetMaxSpellsPerDayLeft(nSpellLevel, 0);
        classInfo.SetSpellsPerDayLeft(nSpellLevel, 0);
    }
}

// Memorising classes: level-up, class and ability changes, and the plugin's
// own LevelUp replay above all recount through here.
static Hooks::Hook s_UpdateNumberMemorizedSpellSlotsHook = Hooks::HookFunction(&CNWSCreatureStats::UpdateNumberMemorizedSpellSlots,
    +[](CNWSCreatureStats *pStats) -> void
    {
        s_UpdateNumberMemorizedSpellSlotsHook->CallOriginal<void>(pStats);
        ClearUnreachableMemorizedSlots(pStats);
    }, Hooks::Order::Late);

// Memorising classes: equipping or removing the item, which includes the
// equipped items being applied as a character loads.
static Hooks::Hook s_ModifyNumberBonusSpellsHook = Hooks::HookFunction(&CNWSCreatureStats::ModifyNumberBonusSpells,
    +[](CNWSCreatureStats *pStats, uint8_t nMultiClass, uint8_t nSpellLevel, int32_t nDelta) -> void
    {
        s_ModifyNumberBonusSpellsHook->CallOriginal<void>(pStats, nMultiClass, nSpellLevel, nDelta);

        if (nMultiClass < pStats->m_nNumMultiClasses && nSpellLevel < 10
            && pStats->GetNumberMemorizedSpellSlots(nMultiClass, nSpellLevel)
            && IsUnreachableSpellLevel(pStats, nMultiClass, nSpellLevel))
        {
            pStats->SetNumberMemorizedSpellSlots(nMultiClass, nSpellLevel, 0);
        }
    }, Hooks::Order::Late);

// Spontaneous classes: the recount after an equip, unequip, or ability change.
static Hooks::Hook s_AdjustSpellUsesPerDayHook = Hooks::HookFunction(&CNWSCreatureStats::AdjustSpellUsesPerDay,
    +[](CNWSCreatureStats *pStats) -> void
    {
        s_AdjustSpellUsesPerDayHook->CallOriginal<void>(pStats);

        for (uint8_t nMultiClass = 0; nMultiClass < pStats->m_nNumMultiClasses; nMultiClass++)
            for (uint8_t nSpellLevel = 1; nSpellLevel < 10; nSpellLevel++)
                ClearUnreachableSpellsPerDay(pStats, nMultiClass, nSpellLevel);
    }, Hooks::Order::Late);

// Spontaneous classes: the reset at rest (ReadySpellLevel) and from the
// NWScript restore commands.
static Hooks::Hook s_ResetSpellsPerDayLeftHook = Hooks::HookFunction(&CNWSCreatureStats::ResetSpellsPerDayLeft,
    +[](CNWSCreatureStats *pStats, uint8_t nMultiClass, uint8_t nSpellLevel) -> void
    {
        s_ResetSpellsPerDayLeftHook->CallOriginal<void>(pStats, nMultiClass, nSpellLevel);
        ClearUnreachableSpellsPerDay(pStats, nMultiClass, nSpellLevel);
    }, Hooks::Order::Late);



// ---------------------------------------------------------------------------
// Budge exemption
// ---------------------------------------------------------------------------
//
// CNWSArea::BudgeCreatures runs whenever a door opens or closes
// (CNWSDoor::SetOpenState) and whenever a placeable is added to an area
// (CNWSPlaceable::AddToArea). It walks the creatures within 5m of the object's
// bounding box, and any whose own position fails TestSafeLocationPoint is
// moved with SetPosition to a ComputeSafeLocation result up to 10m from the
// object -- the engine's way of getting a creature out of a doorway that has
// just closed on it.
//
// The module stands some creatures on unwalkable ground on purpose. A prisoner
// in the Stocks is put on the placeable's origin, inside its own walkmesh,
// because the pose only lines up there (pw_inc_stocks.nss). To the engine that
// is a creature stuck in something, and the next door to move nearby threw
// them out: on 2026-09-30 every prisoner locked in was gone within about a
// minute, each time the tailor's door beside the stocks was used, and the
// module then read the jump as a DM moving them and ended the sentence.
//
// A creature carrying the NO_BUDGE local is left where it is. The engine's
// loop offers nothing to skip one by -- its only tests are the box and the
// safe-point check -- so each exempt creature inside the box has its Y moved
// far outside the box for the length of the call and put back after. That is
// a write to the field, not SetPosition, so nothing else sees it: no trigger
// or area-of-effect crossing, no client update, and the area's object list is
// sorted by X, which is not touched. Every other creature in the box is budged
// exactly as before.
//
// The scan here is the engine's own, with the same start index and the same
// stop, so it costs what the original loop costs and no more. That matters
// because AddToArea calls this for every placeable as an area loads.
//
// Read from the 8193.37 disassembly, 2026-10-01: BudgeCreatures and its two
// callers. The start index really is taken from vPosition.x - vBBMin.x - 5,
// odd as that looks beside the absolute box tests that follow; it is copied
// as it stands so the two loops visit the same creatures.
static_assert(offsetof(CNWSArea, m_aGameObjects) == 0x228, "CNWSArea layout changed");

static Hooks::Hook s_BudgeCreaturesHook = Hooks::HookFunction(&CNWSArea::BudgeCreatures,
    +[](CNWSArea *pThis, const Vector *pvPosition, const Vector *pvBBMin, const Vector *pvBBMax,
            ObjectID oidNewObject, BOOL bBumpToActionPoint) -> void
    {
        // The three vectors are references in the engine's signature. They are
        // taken as pointers here because CallOriginal deduces its argument
        // types, and would hand a reference on BY VALUE -- the original then
        // reads a Vector's floats as an address (crashed at module load,
        // 2026-10-01).
        const Vector &vPosition = *pvPosition;
        const Vector &vBBMin    = *pvBBMin;
        const Vector &vBBMax    = *pvBBMax;

        static CExoString sVarName = "NO_BUDGE";

        const float fMargin     = 5.0f;     // the engine's own, around the box
        const float fHideOffset = 1000.0f;  // well past any area's edge

        struct Exempt { ObjectID oidCreature; float fY; };
        std::vector<Exempt> exempt;

        auto *pServer = Globals::AppManager()->m_pServerExoApp;

        int32_t nIndex = 0;
        pThis->GetFirstObjectIndiceByX(&nIndex, vPosition.x - vBBMin.x - fMargin);
        if (nIndex < 0)
            nIndex = 0;

        for (; nIndex < pThis->m_aGameObjects.num; nIndex++)
        {
            auto *pCreature = pServer->GetCreatureByGameObjectID(pThis->m_aGameObjects.element[nIndex]);
            if (!pCreature)
                continue;

            const Vector vCreature = pCreature->m_vPosition;

            if (vCreature.x >= vBBMax.x + fMargin)
                break;

            if (vCreature.x <= vBBMin.x - fMargin ||
                vCreature.y <= vBBMin.y - fMargin ||
                vCreature.y >= vBBMax.y + fMargin)
                continue;

            if (!Utils::GetScriptVarTable(pCreature)->GetInt(sVarName))
                continue;

            exempt.push_back({pCreature->m_idSelf, vCreature.y});
            pCreature->m_vPosition.y = vBBMax.y + fMargin + fHideOffset;
        }

        s_BudgeCreaturesHook->CallOriginal<void>(pThis, pvPosition, pvBBMin, pvBBMax, oidNewObject, bBumpToActionPoint);

        // By id, not by pointer: nothing in the original should destroy a
        // creature, but a pointer held across engine code is not worth
        // trusting for the sake of one lookup.
        for (const auto &e : exempt)
        {
            if (auto *pCreature = pServer->GetCreatureByGameObjectID(e.oidCreature))
                pCreature->m_vPosition.y = e.fY;
        }
    }, Hooks::Order::Early);



// ---------------------------------------------------------------------------
// No size limits on equipping
// ---------------------------------------------------------------------------
//
// The engine refuses a weapon whose size is more than one category above the
// creature's ("You are too small to equip that weapon", feedback 0x78) or more
// than two below it (feedback 0x104), and refuses a tower shield to a Tiny or
// Small creature. Risenholm does not use size categories, so none of these
// should ever stop an equip.
//
// Three engine functions carry the tests, each against m_nCreatureSize:
// CanEquipWeapon (the base item's WeaponSize minus the creature's size must
// be in -2..1), CanEquipShield (tower shield, creature size 3 or more), and
// CanUseItem (both of the same tests, for a creature using the item).
//
// CanEquipWeapon and CanUseItem are hooked to move m_nCreatureSize for the
// length of the call just far enough to pass, then put it back. Nothing else
// reads the field during those calls, and no message goes to the client for
// it. CanEquipShield is different: after the tower shield test it checks the
// weapon in the right hand against the creature's size, and a raised size
// would let a Small creature add a shield to a weapon it holds two-handed.
// So there the shield itself is passed off as a large shield for the call
// (its base item is read by the tower test and by nothing after it), and the
// size is left alone. CanUseItem cannot take that route, since it checks the
// tower shield proficiency.
//
// Clamping to weapon size - 1 rather than to the weapon's own size means an
// oversized weapon is passed as two-handed (a difference of exactly 1), so it
// takes both hands, the same as a greatsword in a Medium creature's.
// Afterwards, outside the hook, the difference is the real one: 2 for a Small
// creature holding a Large weapon. CanEquipShield and CanEquipWeapon test a
// held weapon with "difference above 0", so the off hand stays blocked, and
// CalculateOffHandAttacks gives no off-hand attacks. The engine's 1.5x
// Strength damage (GetMeleeDamageBonus, GetDamageBonus) tests "exactly 1", so
// it does not apply in that case.
//
// Read from the 8193.37 decompilation, 2026-10-02: CanEquipWeapon,
// CanEquipShield, CanUseItem, and the other readers of m_nCreatureSize.
static int32_t GetSizeToEquip(CNWSCreature *pCreature, CNWSItem *pItem)
{
    int32_t nSize = pCreature->m_nCreatureSize;
    if (!pItem)
        return nSize;

    // CanUseItem only; CanEquipShield takes the base item route above.
    if (pItem->m_nBaseItem == Constants::BaseItem::TowerShield)
        nSize = std::max(nSize, 3); // Medium

    // WeaponSize 0 is an item that is not a weapon (shields among them),
    // which CanEquipWeapon and CanUseItem never put to the weapon test.
    if (auto *pBaseItem = Globals::Rules()->m_pBaseItemArray->GetBaseItem(pItem->m_nBaseItem))
    {
        const int32_t nWeaponSize = pBaseItem->m_nWeaponSize;
        if (nWeaponSize > 0)
            nSize = std::clamp(nSize, nWeaponSize - 1, nWeaponSize + 2);
    }

    return nSize;
}

struct ScopedSizeToEquip
{
    CNWSCreature *pCreature;
    int32_t nSaved;

    ScopedSizeToEquip(CNWSCreature *pCreature, CNWSItem *pItem)
        : pCreature(pCreature), nSaved(pCreature->m_nCreatureSize)
    {
        pCreature->m_nCreatureSize = GetSizeToEquip(pCreature, pItem);
    }

    ~ScopedSizeToEquip()
    {
        pCreature->m_nCreatureSize = nSaved;
    }
};

static Hooks::Hook s_CanEquipWeaponHook = Hooks::HookFunction(&CNWSCreature::CanEquipWeapon,
    +[](CNWSCreature *pThis, CNWSItem *pItem, uint32_t *pnEquipToSlot, BOOL bEquipping,
            BOOL bDisplayFeedback, CNWSPlayer *pFeedbackPlayer) -> uint8_t
    {
        ScopedSizeToEquip size(pThis, pItem);
        return s_CanEquipWeaponHook->CallOriginal<uint8_t>(pThis, pItem, pnEquipToSlot, bEquipping,
                                                           bDisplayFeedback, pFeedbackPlayer);
    }, Hooks::Order::Early);

static Hooks::Hook s_CanEquipShieldHook = Hooks::HookFunction(&CNWSCreature::CanEquipShield,
    +[](CNWSCreature *pThis, CNWSItem *pItem, BOOL bEquipping, BOOL bDisplayFeedback) -> uint8_t
    {
        if (!pItem || pItem->m_nBaseItem != Constants::BaseItem::TowerShield)
            return s_CanEquipShieldHook->CallOriginal<uint8_t>(pThis, pItem, bEquipping, bDisplayFeedback);

        pItem->m_nBaseItem = Constants::BaseItem::LargeShield;
        const uint8_t nResult = s_CanEquipShieldHook->CallOriginal<uint8_t>(pThis, pItem, bEquipping, bDisplayFeedback);
        pItem->m_nBaseItem = Constants::BaseItem::TowerShield;
        return nResult;
    }, Hooks::Order::Early);

static Hooks::Hook s_CanUseItemHook = Hooks::HookFunction(&CNWSCreature::CanUseItem,
    +[](CNWSCreature *pThis, CNWSItem *pItem, BOOL bIgnoreIdentifiedFlag) -> BOOL
    {
        ScopedSizeToEquip size(pThis, pItem);
        return s_CanUseItemHook->CallOriginal<BOOL>(pThis, pItem, bIgnoreIdentifiedFlag);
    }, Hooks::Order::Early);



// ---------------------------------------------------------------------------
// Player characters are always Medium
// ---------------------------------------------------------------------------
//
// CNWSCreature::UpdateAppearanceDependantInfo sets m_nCreatureSize from the
// SIZECATEGORY column of appearance.2da. It runs on every appearance change:
// at load (PostProcess, so on every login, whatever CreatureSize the .bic
// holds), from SetCreatureAppearanceType, and from Polymorph and UnPolymorph.
// A PC given a Large appearance so became a Large creature, a greatsword was
// a one-handed weapon to them, and they could dual-wield a pair.
//
// Risenholm does not use size categories, so a player character is put back
// to Medium after each of those calls. That reaches everything the engine
// reads the size for, not only weapons: the size modifiers to AC, attack,
// and Hide, the knockdown size checks, unarmed damage, and GetCreatureSize in
// scripts. pw_mod_1st_login already set new characters Medium (a 2021 fix for
// druid wildshape), but the next appearance change or login undid it.
//
// A PC is CNWSCreatureStats::m_bIsPC, read from the character file's IsPC
// field before PostProcess, which copies it to m_bPlayerCharacter after this
// has run. A creature a player only possesses keeps the size of its own
// appearance. NWNX_Creature_SetSize still works on a PC until the next
// appearance change.
//
// Read from the 8193.37 decompilation and disassembly, 2026-10-02.
static Hooks::Hook s_UpdateAppearanceDependantInfoHook = Hooks::HookFunction(&CNWSCreature::UpdateAppearanceDependantInfo,
    +[](CNWSCreature *pThis) -> void
    {
        s_UpdateAppearanceDependantInfoHook->CallOriginal<void>(pThis);

        if (pThis->m_pStats && pThis->m_pStats->m_bIsPC)
            pThis->m_nCreatureSize = 3; // CREATURE_SIZE_MEDIUM
    }, Hooks::Order::Early);



// ---------------------------------------------------------------------------
// A split stack keeps its local variables
// ---------------------------------------------------------------------------
//
// CNWSItem::SplitItem makes the split-off stack with CopyItem(this, FALSE),
// and CopyItem's second argument is the only thing that decides whether it
// calls CopyScriptVars. So the new stack came out with no locals at all, and
// the module keeps item behaviour in them: a meal split off a stack lost
// IS_MEAL and BEFORE_USE_SCRIPT, so it skipped the meal cooldown and the
// town-or-campfire check and was eaten as a snack, and a snack lost
// FOOD_SCRIPT, so eating it cleared the eater's food buff instead of setting
// one. The half left behind is the original object and always kept them.
//
// The two halves of a stack are the same thing, so the new one gets a copy of
// the original's locals. CNWSCreature::SplitItem, the player's inventory
// split, is SplitItem's only caller, and it adds the new stack to an inventory
// only after this returns, so the locals are in place before anything sees it.
//
// Read from the 8193.37 disassembly, 2026-10-04.
static Hooks::Hook s_SplitItemHook = Hooks::HookFunction(&CNWSItem::SplitItem,
    +[](CNWSItem *pThis, int32_t nNumberToSplitOff) -> CNWSItem*
    {
        auto *pNewItem = s_SplitItemHook->CallOriginal<CNWSItem*>(pThis, nNumberToSplitOff);

        if (pNewItem)
            pNewItem->CopyScriptVars(&pThis->m_ScriptVars);

        return pNewItem;
    }, Hooks::Order::Early);



// ---------------------------------------------------------------------------
// NUI window logging
// ---------------------------------------------------------------------------
//
// A window whose JSON the client cannot read -- the wrong shape in the layout
// or in a bind, e.g. an array where a colour object belongs -- is replaced on
// the client by an error box reading "[json.exception.type_error.304] cannot
// use at() with ...". The server does not validate NUI JSON, so nothing about
// it reaches the server log, and a player's report ("I got a json error while
// opening seeds") gives no way of telling which window it was: NuiCreate and
// NuiSetGroupLayout log nothing, and the engine's own NUI messages ("bind not
// found", "window does not exist") name no player. Reported 2026-09-28 on
// seed-cracking at an anvil, where the hammer swap rebuilds several windows
// at once, with no way to say which one failed.
//
// This logs every window the server sends a client, and every group layout
// it replaces, naming the character, the account, the window id and token,
// and the script that asked. Matching a report's time and character against
// these lines names the window.
//
// Hooked by symbol: SendServerToPlayerNui_Create and _SetLayout take an
// nlohmann::json, so the API headers leave them commented out. The json is
// passed through untouched as an opaque pointer. The window id is a
// std::string taken BY VALUE, which the Itanium ABI passes as a pointer to a
// caller-owned temporary; the caller also destroys it, so the pointer is
// forwarded as it came.
//
// Switch: NWNX_RISENHOLM_LOG_NUI (bool, default false).

static bool GetLogNui()
{
    static const bool s_bOn = []() -> bool
    {
        bool b = Config::Get<bool>("LOG_NUI", false);
        LOG_INFO("NUI window logging: %s", b ? "on" : "off");
        return b;
    }();

    return s_bOn;
}

static const bool s_bLogNuiLogged = (GetLogNui(), true);

// The innermost running script, or "(engine)" when none is -- a window can be
// sent from a DelayCommand closure, which runs under its originating script's
// name, but never from outside the VM.
static std::string GetNuiCallingScript()
{
    auto *pVM = Globals::VirtualMachine();

    if (pVM && pVM->m_nRecursionLevel >= 0 && pVM->m_nRecursionLevel < 8)
    {
        auto &script = pVM->m_pVirtualMachineScript[pVM->m_nRecursionLevel];

        if (!script.m_sScriptName.IsEmpty())
            return script.m_sScriptName.CStr();
    }

    return "(engine)";
}

// "Character (account)". The character is the PC, not whatever creature the
// player is driving, since NUI belongs to the client and a possessed creature's
// windows are the player's own.
static std::string GetNuiPlayerLabel(CNWSPlayer *pPlayer)
{
    std::string sAccount = pPlayer->GetPlayerName().CStr();
    auto *pCreature = Utils::AsNWSCreature(Utils::GetGameObject(pPlayer->m_oidPCObject));

    if (pCreature && pCreature->m_pStats)
        return std::string(pCreature->m_pStats->GetFullName().CStr()) + " (" + sAccount + ")";

    return "(" + sAccount + ")";
}

static Hooks::Hook s_NuiCreateHook;
static Hooks::Hook s_NuiSetLayoutHook;

static Hooks::Hook HookNuiByName(const char *sSymbol, void *pHandler)
{
    void *pTarget = dlsym(RTLD_DEFAULT, sSymbol);

    if (!pTarget)
    {
        LOG_ERROR("%s not found; NUI window logging is incomplete", sSymbol);
        return nullptr;
    }

    return Hooks::HookFunction(pTarget, pHandler, Hooks::Order::Earliest);
}

static const bool s_bLogNuiHooked = []() -> bool
{
    if (!GetLogNui())
        return false;

    s_NuiCreateHook = HookNuiByName(
        "_ZN11CNWSMessage28SendServerToPlayerNui_CreateEP10CNWSPlayeriNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERKN8nlohmann10basic_jsonISt3mapSt6vectorS7_blmdSaNS8_14adl_serializerESB_IhSaIhEEEE",
        (void*)+[](CNWSMessage *pMessage, CNWSPlayer *pPlayer, int32_t nToken, const std::string *pId, const void *pJson) -> int32_t
        {
            if (pPlayer)
            {
                LOG_INFO("NUI create: %s window '%s' token %d from %s",
                    GetNuiPlayerLabel(pPlayer).c_str(), pId ? pId->c_str() : "", nToken, GetNuiCallingScript().c_str());
            }

            return s_NuiCreateHook->CallOriginal<int32_t>(pMessage, pPlayer, nToken, pId, pJson);
        });

    s_NuiSetLayoutHook = HookNuiByName(
        "_ZN11CNWSMessage31SendServerToPlayerNui_SetLayoutEP10CNWSPlayeriRK10CExoStringRKN8nlohmann10basic_jsonISt3mapSt6vectorNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEblmdSaNS5_14adl_serializerES8_IhSaIhEEEE",
        (void*)+[](CNWSMessage *pMessage, CNWSPlayer *pPlayer, int32_t nToken, const CExoString *pElement, const void *pJson) -> int32_t
        {
            if (pPlayer)
            {
                // The token is all the engine passes; its window id is in the
                // player's NUI state, which the layout command has just looked
                // the token up in, so it is there.
                std::string sId;
                auto &windows = pPlayer->m_cNuiState.m_windows;
                auto it = windows.find(nToken);

                if (it != windows.end())
                    sId = it->second.m_id;

                LOG_INFO("NUI layout: %s window '%s' token %d element '%s' from %s",
                    GetNuiPlayerLabel(pPlayer).c_str(), sId.c_str(), nToken,
                    pElement ? pElement->CStr() : "", GetNuiCallingScript().c_str());
            }

            return s_NuiSetLayoutHook->CallOriginal<int32_t>(pMessage, pPlayer, nToken, pElement, pJson);
        });

    return true;
}();

// ---------------------------------------------------------------------------
// RPC: run a script for a request from outside the game, and send back its answer
// ---------------------------------------------------------------------------
//
// Wellkeeper (the DMs' web tool) reached the game only through MySQL: it wrote
// a row, and a script polling that table on a DelayCommand picked it up some
// seconds later. That is slow, costs a query per poll with nothing to find,
// and cannot answer -- "where is this character standing" has no row to read.
//
// This is a small HTTP listener inside the server process. A request's body is
// handed to a script on the main thread at the top of the next server frame
// (Tasks::QueueOnMainThread, drained by Core's MainLoop hook before the frame
// runs), and whatever the script passes to SetRpcResponse goes back as the
// reply. A round trip is one frame, about 16ms on an idle server, and nothing
// runs at all between requests.
//
//   POST /rpc/<name>   Authorization: Bearer <NWNX_RISENHOLM_RPC_SECRET>
//   body         anything; the module's scripts take a JSON object
//   200          the script's answer, sent as application/json
//   401          wrong or missing secret
//   404          no such script
//   500          the script ran and set no answer
//   503          no module is loaded yet
//   504          the main thread did not get to it within 5 seconds
//
// The path names the script: /rpc/teleport runs pw_rpc_teleport. The module
// keeps one script for each thing the outside world may ask of it, all under
// that prefix, and the prefix is the boundary. <name> is put after it and
// nothing else is ever run, so that holding the secret is leave to call what
// the module wrote to be called this way, not to run any script it has
// (pw_mod_load, a DM tool). A name is lower-case letters, digits, and
// underscores, and short enough for the whole to be a resref.
//
// The script runs with the module as OBJECT_SELF and reads the body with
// GetRpcRequest. Only one runs at a time, since they all run on the main
// thread, so "the current request" is a single pointer.
//
// A request that timed out is marked abandoned and is NOT run when the main
// thread finally reaches it: an action nobody is waiting on any more (a
// teleport, say) must not land half a minute late, after its sender has been
// told it failed and has maybe tried something else. The mark is checked as
// the task starts, so a script already running when the wait ends still
// finishes; its sender is told 504 regardless.
//
// The listener's worker threads never touch the engine. They parse HTTP, queue
// the task, and block on its future.
//
// Keep-alive is off (one request per connection). The pool is four threads,
// and an idle kept-alive connection would hold one of them for its whole
// timeout.
//
// What it is and is not protected against. It is plain HTTP with a shared
// secret, for a loopback or a private container network on one host; it must
// never be published. Within that:
//
// - The secret is checked BEFORE the body is read (a pre-routing handler), so
//   a sender without it gets nothing buffered on its behalf but its headers.
//   The first version checked in the route handler, by which time the library
//   had read the whole body, and an attack on the dev server on 2026-10-02
//   showed what that was worth: cpp-httplib 0.16.2 took a 64 MiB chunked body
//   from an unauthenticated sender straight past the 64 KiB payload limit, and
//   200,000 header lines likewise, the process growing to match. External/
//   httplib.h is now 0.57.1, which caps a request at 100 header lines of 8 KiB
//   and applies the payload limit to chunked bodies too.
// - A rejected request is logged with its sender's address, at most once
//   every ten seconds, so that probing shows up in the server log without
//   being able to flood it.
// - Anything that can connect can still DENY the listener: each silent
//   connection holds a worker until its two-second read timeout, and there are
//   four workers. The queue behind them is capped, so such a flood is turned
//   away rather than stored. It costs the game itself nothing, because no
//   worker touches the main thread until a request has authenticated.
// - A sender WITH the secret can spend main-thread time: every request runs a
//   script. Keep the secret to the one service that needs it.
//
// Switches (all NWNX_RISENHOLM_*):
//   RPC_PORT    port to listen on. Unset or 0: no listener at all.
//   RPC_SECRET  shared secret, at least 16 characters. Required: with a port
//               and no secret, or a short one, the listener refuses to start
//               rather than run open. Generate one: openssl rand -hex 32
//   RPC_BIND    address to bind [127.0.0.1]. In Docker this must be 0.0.0.0
//               for another container to reach it; do NOT publish the port.
//   RPC_SCRIPT_PREFIX  what every RPC script's name starts with [pw_rpc_].
//               Must not be empty, which would make every script callable.

struct RpcCall
{
    std::string sRequest;
    std::string sResponse;
    bool bAnswered = false;
    std::atomic<bool> bAbandoned { false };
    std::promise<void> done;
};

// The call whose script is running. Main thread only.
static RpcCall *s_pRpcCall = nullptr;

// Constant time in the length of the secret, so that a wrong guess's timing
// says nothing about how much of it was right.
static bool RpcSecretMatches(const std::string &sGiven, const std::string &sExpected)
{
    unsigned char nDiff = sGiven.size() == sExpected.size() ? 0 : 1;

    for (size_t i = 0; i < sExpected.size(); i++)
        nDiff |= (unsigned char)(i < sGiven.size() ? sGiven[i] : 0) ^ (unsigned char)sExpected[i];

    return nDiff == 0;
}

// Whether a string is made only of what a script's name may be. Upper case is
// left out on purpose: resrefs are lower case, and one name per script keeps
// the log readable.
static bool RpcIsResRefText(const std::string &s)
{
    for (char c : s)
    {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    }

    return true;
}

static const bool s_bRpcListening = []() -> bool
{
    int nPort = Config::Get<int>("RPC_PORT", 0);

    if (nPort <= 0)
    {
        LOG_INFO("RPC listener: off");
        return false;
    }

    std::string sSecret = Config::Get<std::string>("RPC_SECRET", "");

    // Guessing is the one attack the secret has to stand up to on its own, and
    // a short one does not. Its length is all that is ever logged of it.
    if (sSecret.size() < 16)
    {
        LOG_ERROR("RPC listener: NWNX_RISENHOLM_RPC_PORT is set but NWNX_RISENHOLM_RPC_SECRET is %s; not listening",
            sSecret.empty() ? "not" : "shorter than 16 characters");
        return false;
    }

    std::string sBind   = Config::Get<std::string>("RPC_BIND", "127.0.0.1");
    std::string sPrefix = Config::Get<std::string>("RPC_SCRIPT_PREFIX", "pw_rpc_");

    // The prefix is what keeps a request to the scripts written for it. An
    // empty one would make every script in the module callable, and one that
    // leaves no room for a name makes none.
    if (sPrefix.empty() || sPrefix.size() >= 16 || !RpcIsResRefText(sPrefix))
    {
        LOG_ERROR("RPC listener: NWNX_RISENHOLM_RPC_SCRIPT_PREFIX '%s' is not 1 to 15 of a-z, 0-9, and _; not listening", sPrefix.c_str());
        return false;
    }

    // Never freed: its thread is detached and runs until the process exits,
    // and tearing a listening server down from a static destructor at exit is
    // a crash waiting for the wrong moment.
    auto *pServer = new httplib::Server();

    // Four workers and no more, with at most 32 connections waiting for one:
    // past that a new connection is closed at once instead of queued.
    pServer->new_task_queue = [] { return new httplib::ThreadPool(4, 4, 32); };
    pServer->set_keep_alive_max_count(1);
    pServer->set_payload_max_length(64 * 1024);
    pServer->set_read_timeout(2, 0);
    pServer->set_write_timeout(5, 0);

    // Runs once the headers are in and before any of the body is read, which
    // is the point of doing it here rather than in the route.
    pServer->set_pre_routing_handler([sSecret](const httplib::Request &req, httplib::Response &res)
    {
        if (RpcSecretMatches(req.get_header_value("Authorization"), "Bearer " + sSecret))
            return httplib::Server::HandlerResponse::Unhandled;

        static std::atomic<int64_t> s_nLastLogged { 0 };
        static std::atomic<uint32_t> s_nUnlogged { 0 };

        int64_t nNow = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        int64_t nLast = s_nLastLogged.load();

        if (nNow - nLast >= 10 && s_nLastLogged.compare_exchange_strong(nLast, nNow))
        {
            LOG_WARNING("RPC listener: rejected a request from %s with a wrong or missing secret (%u more since the last such line)",
                req.remote_addr.c_str(), s_nUnlogged.exchange(0));
        }
        else
        {
            s_nUnlogged++;
        }

        res.status = 401;
        res.set_content("{\"error\":\"Unauthorised.\"}", "application/json");
        return httplib::Server::HandlerResponse::Handled;
    });

    pServer->Post("/rpc/:name", [sPrefix](const httplib::Request &req, httplib::Response &res)
    {
        // Checked here, on the worker, so that a name that could not be a
        // script never costs the main thread anything.
        auto name = req.path_params.find("name");
        std::string sScript = sPrefix + (name != req.path_params.end() ? name->second : "");

        if (sScript.size() <= sPrefix.size() || sScript.size() > 16 || !RpcIsResRefText(sScript))
        {
            res.status = 404;
            res.set_content("{\"error\":\"No such RPC script.\"}", "application/json");
            return;
        }

        auto pCall = std::make_shared<RpcCall>();
        pCall->sRequest = req.body;
        auto done = pCall->done.get_future();

        // How far the main thread got: written there, read here once `done`
        // is ready.
        enum { NoModule, NoScript, Ran };
        auto pReached = std::make_shared<int>(NoModule);

        Tasks::QueueOnMainThread([pCall, pReached, sScript]()
        {
            if (!pCall->bAbandoned.load())
            {
                if (auto *pModule = Utils::GetModule())
                {
                    // Asked rather than found out by running it: a script
                    // that is not there and one that set no answer would
                    // otherwise look the same.
                    if (Globals::ExoResMan()->Exists(CResRef(sScript.c_str()), Constants::ResRefType::NCS, nullptr))
                    {
                        *pReached = Ran;
                        s_pRpcCall = pCall.get();
                        Utils::ExecuteScript(sScript, pModule->m_idSelf);
                        s_pRpcCall = nullptr;
                    }
                    else
                    {
                        *pReached = NoScript;
                    }
                }
            }

            pCall->done.set_value();
        });

        if (done.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
        {
            pCall->bAbandoned.store(true);
            res.status = 504;
            res.set_content("{\"error\":\"The game server did not answer in time.\"}", "application/json");
            return;
        }

        if (*pReached == NoModule)
        {
            res.status = 503;
            res.set_content("{\"error\":\"The module is not loaded yet.\"}", "application/json");
            return;
        }

        if (*pReached == NoScript)
        {
            res.status = 404;
            res.set_content("{\"error\":\"No such RPC script.\"}", "application/json");
            return;
        }

        if (!pCall->bAnswered)
        {
            res.status = 500;
            res.set_content("{\"error\":\"The game's RPC script gave no answer.\"}", "application/json");
            return;
        }

        res.set_content(pCall->sResponse, "application/json");
    });

    if (!pServer->bind_to_port(sBind, nPort))
    {
        LOG_ERROR("RPC listener: could not bind %s:%d; not listening", sBind.c_str(), nPort);
        delete pServer;
        return false;
    }

    std::thread([pServer]() { pServer->listen_after_bind(); }).detach();

    LOG_INFO("RPC listener: %s:%d, scripts '%s*'", sBind.c_str(), nPort, sPrefix.c_str());
    return true;
}();

NWNX_EXPORT ArgumentStack GetRpcRequest(ArgumentStack&&)
{
    return s_pRpcCall ? s_pRpcCall->sRequest : std::string();
}

NWNX_EXPORT ArgumentStack SetRpcResponse(ArgumentStack&& args)
{
    auto sResponse = args.extract<std::string>();

    if (s_pRpcCall)
    {
        s_pRpcCall->sResponse = std::move(sResponse);
        s_pRpcCall->bAnswered = true;
    }

    return {};
}
