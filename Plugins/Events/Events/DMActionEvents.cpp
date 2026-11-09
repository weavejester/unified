#include "Events.hpp"
#include "API/CAppManager.hpp"
#include "API/CServerExoApp.hpp"
#include "API/CNWSFaction.hpp"
#include "API/CServerExoAppInternal.hpp"
#include "API/CFactionManager.hpp"
#include "API/CGameObjectArray.hpp"
#include "API/CNetLayer.hpp"
#include "API/CNetLayerPlayerInfo.hpp"
#include "API/CNWSPlayer.hpp"
#include "API/CNWSMessage.hpp"
#include <vector>

namespace Events {

using namespace NWNXLib;
using namespace NWNXLib::API;
using namespace NWNXLib::API::Constants;
using namespace NWNXLib::Platform;

static Hooks::Hook s_HandlePlayerToServerDungeonMasterMessageHook;

static int32_t HandleGiveEvent(CNWSMessage*, CNWSPlayer*, uint8_t, int32_t, const std::string&, int32_t);
static int32_t HandleGroupEvent(CNWSMessage*, CNWSPlayer*, uint8_t, int32_t, const std::string&);
static int32_t HandleSingleTargetEvent(CNWSMessage*, CNWSPlayer*, uint8_t, int32_t, const std::string&);
static int32_t HandleTeleportEvent(CNWSMessage*, CNWSPlayer*, uint8_t, int32_t, const std::string&);
static int32_t HandleDMMessageHook(CNWSMessage*, CNWSPlayer*, uint8_t, int32_t);

void DMActionEvents() __attribute__((constructor));
void DMActionEvents()
{
    InitOnFirstSubscribe("NWNX_ON_DM_.*", []() {
        s_HandlePlayerToServerDungeonMasterMessageHook = Hooks::HookFunction(
                &CNWSMessage::HandlePlayerToServerDungeonMasterMessage,
                &HandleDMMessageHook, Hooks::Order::Early);
    });
}

int32_t HandleGiveEvent(CNWSMessage *pMessage, CNWSPlayer *pPlayer, uint8_t nMinor, int32_t bGroup,
                                        const std::string &event, int32_t alignmentType = 0)
{
    int32_t retVal;
    ObjectID oidDM = pPlayer ? pPlayer->m_oidNWSObject : OBJECT_INVALID;
    // GiveXP, GiveLevel, and GiveGold read an INT and then the target. The 8193.37
    // server has no case for the alignment minors at all; they are taken to match.
    std::string amount, target;
    {
        Utils::MessagePeek peek(pMessage);
        amount = std::to_string(pMessage->ReadINT());
        target = Utils::ObjectIDToString(pMessage->ReadOBJECTIDServer() & 0x7FFFFFFF);
    }

    auto PushAndSignalGiveEvent = [&](const std::string& ev) -> bool {
        PushEventData("AMOUNT", amount);
        PushEventData("OBJECT", target);
        if (alignmentType > 0)
        {
            PushEventData("ALIGNMENT_TYPE", std::to_string(alignmentType));
        }
        return SignalEvent(ev, oidDM);
    };

    if (PushAndSignalGiveEvent(event + "_BEFORE"))
    {
        retVal = s_HandlePlayerToServerDungeonMasterMessageHook->CallOriginal<int32_t>(pMessage, pPlayer, nMinor, bGroup);
    }
    else
    {
        Utils::ClearReadMessage();
        retVal = false;
    }

    PushAndSignalGiveEvent(event + "_AFTER");

    return retVal;
}

int32_t HandleGroupEvent(CNWSMessage *pMessage, CNWSPlayer *pPlayer, uint8_t nMinor, int32_t bGroup,
                                        const std::string &event)
{
    int32_t retVal;
    ObjectID oidDM = pPlayer ? pPlayer->m_oidNWSObject : OBJECT_INVALID;
    int32_t groupSize = 1;
    std::vector<ObjectID> targets;

    // A group is a DWORD count and that many objects; otherwise one object.
    {
        Utils::MessagePeek peek(pMessage);
        if (bGroup)
            groupSize = static_cast<int32_t>(pMessage->ReadDWORD());

        targets.reserve(groupSize > 0 ? groupSize : 0);

        for (int32_t target = 0; target < groupSize; target++)
            targets.push_back(pMessage->ReadOBJECTIDServer() & 0x7FFFFFFF);
    }

    auto PushAndSignalGroupEvent = [&](const std::string& ev) -> bool {
        PushEventData("NUM_TARGETS", std::to_string(groupSize));
        for(int32_t target = 0; target < groupSize; target++)
        {
            PushEventData("TARGET_" + std::to_string(target + 1), Utils::ObjectIDToString(targets[target]));
        }
        return SignalEvent(ev, oidDM);
    };

    if (PushAndSignalGroupEvent(event + "_BEFORE"))
    {
        retVal = s_HandlePlayerToServerDungeonMasterMessageHook->CallOriginal<int32_t>(pMessage, pPlayer, nMinor, bGroup);
    }
    else
    {
        Utils::ClearReadMessage();
        retVal = false;
    }

    PushAndSignalGroupEvent(event + "_AFTER");

    return retVal;
}

int32_t HandleSingleTargetEvent(CNWSMessage *pMessage, CNWSPlayer *pPlayer, uint8_t nMinor, int32_t bGroup,
                                                const std::string &event)
{
    int32_t retVal;
    ObjectID oidDM = pPlayer ? pPlayer->m_oidNWSObject : OBJECT_INVALID;
    // Goto, Possess, Impersonate, ToggleLock, and DisableTrap all start with the object,
    // and none of them reads a group count first.
    std::string target;
    {
        Utils::MessagePeek peek(pMessage);
        target = Utils::ObjectIDToString(pMessage->ReadOBJECTIDServer() & 0x7FFFFFFF);
    }

    auto PushAndSignalSingleTargetEvent = [&](const std::string& ev) -> bool {
        PushEventData("TARGET", target);
        return SignalEvent(ev, oidDM);
    };

    if (PushAndSignalSingleTargetEvent(event + "_BEFORE"))
    {
        retVal = s_HandlePlayerToServerDungeonMasterMessageHook->CallOriginal<int32_t>(pMessage, pPlayer, nMinor, bGroup);
    }
    else
    {
        Utils::ClearReadMessage();
        retVal = false;
    }

    PushAndSignalSingleTargetEvent(event + "_AFTER");

    return retVal;
}

int32_t HandleTeleportEvent(CNWSMessage *pMessage, CNWSPlayer *pPlayer, uint8_t nMinor, int32_t bGroup,
                                            const std::string &event)
{
    int32_t retVal;
    ObjectID oidDM = pPlayer ? pPlayer->m_oidNWSObject : OBJECT_INVALID;
    int32_t groupSize = 1;
    std::vector<ObjectID> targets;
    std::string targetArea, x, y, z;

    // The area and x, y, z; GotoPointTarget then has its targets, a DWORD count first
    // only when bGroup is set.
    {
        Utils::MessagePeek peek(pMessage);
        targetArea = Utils::ObjectIDToString(pMessage->ReadOBJECTIDServer() & 0x7FFFFFFF);
        x = std::to_string(pMessage->ReadFLOAT());
        y = std::to_string(pMessage->ReadFLOAT());
        z = std::to_string(pMessage->ReadFLOAT());

        if (nMinor == MessageDungeonMasterMinor::GotoPointTarget)
        {
            if (bGroup)
                groupSize = static_cast<int32_t>(pMessage->ReadDWORD());

            targets.reserve(groupSize > 0 ? groupSize : 0);

            for (int32_t target = 0; target < groupSize; target++)
                targets.push_back(pMessage->ReadOBJECTIDServer() & 0x7FFFFFFF);
        }
    }

    auto PushAndSignalTeleportEvent = [&](const std::string& ev) -> bool {
        PushEventData("TARGET_AREA", targetArea);
        PushEventData("POS_X", x);
        PushEventData("POS_Y", y);
        PushEventData("POS_Z", z);
        if (nMinor == MessageDungeonMasterMinor::GotoPointTarget)
        {
            PushEventData("NUM_TARGETS", std::to_string(groupSize));
            for(int32_t target = 0; target < groupSize; target++)
            {
                PushEventData("TARGET_" + std::to_string(target + 1), Utils::ObjectIDToString(targets[target]));
            }
        }
        return SignalEvent(ev, oidDM);
    };

    if (PushAndSignalTeleportEvent(event + "_BEFORE"))
    {
        retVal = s_HandlePlayerToServerDungeonMasterMessageHook->CallOriginal<int32_t>(pMessage, pPlayer, nMinor, bGroup);
    }
    else
    {
        Utils::ClearReadMessage();
        retVal = false;
    }

    PushAndSignalTeleportEvent(event + "_AFTER");

    return retVal;
}

int32_t HandleDMMessageHook(CNWSMessage *thisPtr, CNWSPlayer *pPlayer, uint8_t nMinor, int32_t bGroup)
{
    int32_t retVal;
    std::string event = "NWNX_ON_DM_";
    ObjectID oidDM = pPlayer ? pPlayer->m_oidNWSObject : OBJECT_INVALID;

    auto DefaultSignalEvent = [&]() -> void {
        if (SignalEvent(event + "_BEFORE", oidDM))
        {
            retVal = s_HandlePlayerToServerDungeonMasterMessageHook->CallOriginal<int32_t>(thisPtr, pPlayer, nMinor, bGroup);
        }
        else
        {
            Utils::ClearReadMessage();
            retVal  = false;
        }
        SignalEvent(event + "_AFTER", oidDM);
    };

    switch (nMinor)
    {
        case MessageDungeonMasterMinor::SpawnCreature:
        case MessageDungeonMasterMinor::SpawnItem:
        case MessageDungeonMasterMinor::SpawnPlaceable:
        case MessageDungeonMasterMinor::SpawnWaypoint:
        case MessageDungeonMasterMinor::SpawnTrigger:
        case MessageDungeonMasterMinor::SpawnEncounter:
        case MessageDungeonMasterMinor::SpawnPortal:
        {
            event += "SPAWN_OBJECT";
            std::string object = Utils::ObjectIDToString(Globals::AppManager()->m_pServerExoApp->GetObjectArray()->m_nNextObjectArrayID[0]);
            int32_t objectType;

            switch (nMinor)
            {
                case MessageDungeonMasterMinor::SpawnCreature:
                    objectType = ObjectType::Creature;
                    break;
                case MessageDungeonMasterMinor::SpawnItem:
                    objectType = ObjectType::Item;
                    break;
                case MessageDungeonMasterMinor::SpawnPlaceable:
                    objectType = ObjectType::Placeable;
                    break;
                case MessageDungeonMasterMinor::SpawnWaypoint:
                    objectType = ObjectType::Waypoint;
                    break;
                case MessageDungeonMasterMinor::SpawnTrigger:
                    objectType = ObjectType::Trigger;
                    break;
                case MessageDungeonMasterMinor::SpawnEncounter:
                    objectType = ObjectType::Encounter;
                    break;
                case MessageDungeonMasterMinor::SpawnPortal:
                    objectType = ObjectType::Portal;
                    break;

                default:
                    break;
            }

            // The area and x, y, z, then a placeable's orientation (three more floats),
            // then the resref. A waypoint or a portal sends a CExoString there instead;
            // the byte peek this replaces read its length and first characters as a
            // resref.
            std::string area, x, y, z, resref;
            {
                Utils::MessagePeek peek(thisPtr);
                area = Utils::ObjectIDToString(thisPtr->ReadOBJECTIDServer() & 0x7FFFFFFF);
                x = std::to_string(thisPtr->ReadFLOAT());
                y = std::to_string(thisPtr->ReadFLOAT());
                z = std::to_string(thisPtr->ReadFLOAT());

                if (nMinor == MessageDungeonMasterMinor::SpawnPlaceable)
                {
                    thisPtr->ReadFLOAT();
                    thisPtr->ReadFLOAT();
                    thisPtr->ReadFLOAT();
                }

                if (nMinor == MessageDungeonMasterMinor::SpawnWaypoint ||
                    nMinor == MessageDungeonMasterMinor::SpawnPortal)
                    resref = thisPtr->ReadCExoString().CStr();
                else
                    resref = thisPtr->ReadCResRef().GetResRefStr();
            }

            auto PushAndSignal = [&](const std::string& ev) -> bool {
                PushEventData("AREA", area);
                PushEventData("OBJECT", object);
                PushEventData("OBJECT_TYPE", std::to_string(objectType));
                PushEventData("POS_X", x);
                PushEventData("POS_Y", y);
                PushEventData("POS_Z", z);
                PushEventData("RESREF", resref);
                return SignalEvent(ev, oidDM);
            };

            if (PushAndSignal(event + "_BEFORE"))
            {
                retVal = s_HandlePlayerToServerDungeonMasterMessageHook->CallOriginal<int32_t>(thisPtr, pPlayer, nMinor, bGroup);
            }
            else
            {
                Utils::ClearReadMessage();
                retVal = false;
            }

            PushAndSignal(event +"_AFTER");
            break;
        }
        case MessageDungeonMasterMinor::Difficulty:
        {
            event += "CHANGE_DIFFICULTY";

            std::string difficulty;
            {
                Utils::MessagePeek peek(thisPtr);
                difficulty = std::to_string(thisPtr->ReadINT());
            }

            auto PushAndSignal = [&](const std::string& ev) -> bool {
                PushEventData("DIFFICULTY_SETTING", difficulty);
                return SignalEvent(ev, oidDM);
            };

            if (PushAndSignal(event + "_BEFORE"))
            {
                retVal = s_HandlePlayerToServerDungeonMasterMessageHook->CallOriginal<int32_t>(thisPtr, pPlayer, nMinor, bGroup);
            }
            else
            {
                Utils::ClearReadMessage();
                retVal = false;
            }

            PushAndSignal(event +"_AFTER");
            break;
        }
        case MessageDungeonMasterMinor::ViewInventory:
        {
            event += "VIEW_INVENTORY";

            std::string openInventory, target;
            {
                Utils::MessagePeek peek(thisPtr);
                openInventory = std::to_string(thisPtr->ReadINT());
                target = Utils::ObjectIDToString(thisPtr->ReadOBJECTIDServer() & 0x7FFFFFFF);
            }

            auto PushAndSignal = [&](const std::string& ev) -> bool {
                PushEventData("OPEN_INVENTORY", openInventory);
                PushEventData("TARGET", target);
                return SignalEvent(ev, oidDM);
            };

            if (PushAndSignal(event + "_BEFORE"))
            {
                retVal = s_HandlePlayerToServerDungeonMasterMessageHook->CallOriginal<int32_t>(thisPtr, pPlayer, nMinor, bGroup);
            }
            else
            {
                Utils::ClearReadMessage();
                retVal = false;
            }

            PushAndSignal(event +"_AFTER");
            break;
        }
        case MessageDungeonMasterMinor::SpawnTrapOnObject:
        {
            event += "SPAWN_TRAP_ON_OBJECT";

            std::string area, target;
            {
                Utils::MessagePeek peek(thisPtr);
                area = Utils::ObjectIDToString(thisPtr->ReadOBJECTIDServer() & 0x7FFFFFFF);
                target = Utils::ObjectIDToString(thisPtr->ReadOBJECTIDServer() & 0x7FFFFFFF);
            }

            auto PushAndSignal = [&](const std::string& ev) -> bool {
                PushEventData("AREA", area);
                PushEventData("TARGET", target);
                return SignalEvent(ev, oidDM);
            };

            if (PushAndSignal(event + "_BEFORE"))
            {
                retVal = s_HandlePlayerToServerDungeonMasterMessageHook->CallOriginal<int32_t>(thisPtr, pPlayer, nMinor, bGroup);
            }
            else
            {
                Utils::ClearReadMessage();
                retVal = false;
            }

            PushAndSignal(event +"_AFTER");
            break;
        }
        case MessageDungeonMasterMinor::Heal:
        {
            event += "HEAL";
            retVal = HandleGroupEvent(thisPtr, pPlayer, nMinor, bGroup, event);
            break;
        }
        case MessageDungeonMasterMinor::Kill:
        {
            event += "KILL";
            retVal = HandleGroupEvent(thisPtr, pPlayer, nMinor, bGroup, event);
            break;
        }
        case MessageDungeonMasterMinor::Goto:
        {
            event += "GOTO";
            retVal = HandleSingleTargetEvent(thisPtr, pPlayer, nMinor, bGroup, event);
            break;
        }
        case MessageDungeonMasterMinor::Possess:
        {
            event += "POSSESS";
            retVal = HandleSingleTargetEvent(thisPtr, pPlayer, nMinor, bGroup, event);
            break;
        }
        case MessageDungeonMasterMinor::Impersonate:
        {
            event += "POSSESS_FULL_POWER";
            retVal = HandleSingleTargetEvent(thisPtr, pPlayer, nMinor, bGroup, event);
            break;
        }
        case MessageDungeonMasterMinor::Invulnerable:
        {
            event += "TOGGLE_INVULNERABLE";
            retVal = HandleGroupEvent(thisPtr, pPlayer, nMinor, bGroup, event);
            break;
        }
        case MessageDungeonMasterMinor::Rest:
        {
            event += "FORCE_REST";
            retVal = HandleGroupEvent(thisPtr, pPlayer, nMinor, bGroup, event);
            break;
        }
        case MessageDungeonMasterMinor::Limbo:
        {
            event += "LIMBO";
            retVal = HandleGroupEvent(thisPtr, pPlayer, nMinor, bGroup, event);
            break;
        }
        case MessageDungeonMasterMinor::ToggleAI:
        {
            event += "TOGGLE_AI";
            retVal = HandleGroupEvent(thisPtr, pPlayer, nMinor, bGroup, event);
            break;
        }
        case MessageDungeonMasterMinor::ToggleLock:
        {
            event += "TOGGLE_LOCK";
            retVal = HandleSingleTargetEvent(thisPtr, pPlayer, nMinor, bGroup, event);
            break;
        }
        case MessageDungeonMasterMinor::DisableTrap:
        {
            event += "DISABLE_TRAP";
            retVal = HandleSingleTargetEvent(thisPtr, pPlayer, nMinor, bGroup, event);
            break;
        }
        case MessageDungeonMasterMinor::Manifest:
        {
            event += "APPEAR";
            DefaultSignalEvent();
            break;
        }
        case MessageDungeonMasterMinor::Unmanifest:
        {
            event += "DISAPPEAR";
            DefaultSignalEvent();
            break;
        }
        case MessageDungeonMasterMinor::Immortal:
        {
            event += "TOGGLE_IMMORTAL";
            retVal = HandleGroupEvent(thisPtr, pPlayer, nMinor, bGroup, event);
            break;
        }
        case MessageDungeonMasterMinor::GotoPoint:
        {
            event += "JUMP_TO_POINT";
            retVal = HandleTeleportEvent(thisPtr, pPlayer, nMinor, bGroup, event);
            break;
        }
        case MessageDungeonMasterMinor::GiveXP:
        {
            event += "GIVE_XP";
            retVal = HandleGiveEvent(thisPtr, pPlayer, nMinor, bGroup, event);
            break;
        }
        case MessageDungeonMasterMinor::GiveLevel:
        {
            event += "GIVE_LEVEL";
            retVal = HandleGiveEvent(thisPtr, pPlayer, nMinor, bGroup, event);
            break;
        }
        case MessageDungeonMasterMinor::GiveGold:
        {
            event += "GIVE_GOLD";
            retVal = HandleGiveEvent(thisPtr, pPlayer, nMinor, bGroup, event);
            break;
        }
        case MessageDungeonMasterMinor::SetFaction:
        case MessageDungeonMasterMinor::SetFactionByName:
        {
            event += "SET_FACTION";

            int32_t factionid;
            std::string factionName;
            std::string target;

            if (nMinor == MessageDungeonMasterMinor::SetFaction)
            {
                {
                    Utils::MessagePeek peek(thisPtr);
                    factionid = thisPtr->ReadINT();
                    target = Utils::ObjectIDToString(thisPtr->ReadOBJECTIDServer() & 0x7FFFFFFF);
                }

                auto* pFaction = Globals::AppManager()->m_pServerExoApp->m_pcExoAppInternal->m_pFactionManager->GetFaction(factionid);
                if (pFaction)
                {
                    factionName = pFaction->m_sFactionName;
                }
            }
            else
            {
                {
                    Utils::MessagePeek peek(thisPtr);
                    target = Utils::ObjectIDToString(thisPtr->ReadOBJECTIDServer() & 0x7FFFFFFF);
                    factionName = thisPtr->ReadCExoString().CStr();
                }

                factionid = Globals::AppManager()->m_pServerExoApp->m_pcExoAppInternal->m_pFactionManager->GetFactionIdByName(factionName);
            }

            auto PushAndSignal = [&](const std::string& ev) -> bool {
                PushEventData("TARGET", target);
                PushEventData("FACTION_ID", std::to_string(factionid));
                PushEventData("FACTION_NAME", factionName);
                return SignalEvent(ev, oidDM);
            };

            if (PushAndSignal(event + "_BEFORE"))
            {
                retVal = s_HandlePlayerToServerDungeonMasterMessageHook->CallOriginal<int32_t>(thisPtr, pPlayer, nMinor, bGroup);
            }
            else
            {
                Utils::ClearReadMessage();
                retVal = false;
            }

            PushAndSignal(event +"_AFTER");
            break;
        }
        case MessageDungeonMasterMinor::GiveItem:
        {
            event += "GIVE_ITEM";

            std::string target;
            {
                Utils::MessagePeek peek(thisPtr);
                target = Utils::ObjectIDToString(thisPtr->ReadOBJECTIDServer() & 0x7FFFFFFF);
            }
            std::string item =Utils::ObjectIDToString(Globals::AppManager()->m_pServerExoApp->GetObjectArray()->m_nNextObjectArrayID[0]);

            auto PushAndSignal = [&](const std::string& ev) -> bool {
                PushEventData("TARGET", target);
                PushEventData("ITEM", item);
                return SignalEvent(ev, oidDM);
            };

            if (PushAndSignal(event + "_BEFORE"))
            {
                retVal = s_HandlePlayerToServerDungeonMasterMessageHook->CallOriginal<int32_t>(thisPtr, pPlayer, nMinor, bGroup);
            }
            else
            {
                Utils::ClearReadMessage();
                retVal = false;
            }

            PushAndSignal(event +"_AFTER");
            break;
        }
        case MessageDungeonMasterMinor::TakeItem:
        {
            event += "TAKE_ITEM";
            DefaultSignalEvent();
            break;
        }
        case MessageDungeonMasterMinor::GotoPointTarget:
        {
            event += "JUMP_TARGET_TO_POINT";
            retVal = HandleTeleportEvent(thisPtr, pPlayer, nMinor, bGroup, event);
            break;
        }
        case MessageDungeonMasterMinor::GotoPointAllPlayers:
        {
            event += "JUMP_ALL_PLAYERS_TO_POINT";
            retVal = HandleTeleportEvent(thisPtr, pPlayer, nMinor, bGroup, event);
            break;
        }
        case MessageDungeonMasterMinor::SetStat:
        {
            event += "SET_STAT";

            // The engine reads the flag BEFORE the target: INT, FLOAT, BOOL, OBJECT.
            int32_t stat;
            std::string value, target, set;
            {
                Utils::MessagePeek peek(thisPtr);
                stat = thisPtr->ReadINT();
                value = std::to_string(thisPtr->ReadFLOAT());
                set = std::to_string((bool)thisPtr->ReadBOOL());
                target = Utils::ObjectIDToString(thisPtr->ReadOBJECTIDServer() & 0x7FFFFFFF);
            }

            auto PushAndSignal = [&](const std::string& ev) -> bool {
                PushEventData("STAT", std::to_string(stat - 5));
                PushEventData("VALUE", value);
                PushEventData("TARGET", target);
                PushEventData("SET", set);
                return SignalEvent(ev, oidDM);
            };

            if (PushAndSignal(event + "_BEFORE"))
            {
                retVal = s_HandlePlayerToServerDungeonMasterMessageHook->CallOriginal<int32_t>(thisPtr, pPlayer, nMinor, bGroup);
            }
            else
            {
                Utils::ClearReadMessage();
                retVal = false;
            }

            PushAndSignal(event +"_AFTER");
            break;
        }
        case MessageDungeonMasterMinor::GetVar:
        {
            event += "GET_VARIABLE";

            uint8_t varType;
            std::string target, key;
            {
                Utils::MessagePeek peek(thisPtr);
                varType = thisPtr->ReadBYTE();
                target = Utils::ObjectIDToString(thisPtr->ReadOBJECTIDServer() & 0x7FFFFFFF);
                key = thisPtr->ReadCExoString().CStr();
            }

            auto PushAndSignal = [&](const std::string& ev) -> bool {
                PushEventData("TYPE", std::to_string(varType));
                PushEventData("TARGET", target);
                PushEventData("KEY", key);
                return SignalEvent(ev, oidDM);
            };

            if (PushAndSignal(event + "_BEFORE"))
            {
                retVal = s_HandlePlayerToServerDungeonMasterMessageHook->CallOriginal<int32_t>(thisPtr, pPlayer, nMinor, bGroup);
            }
            else
            {
                Utils::ClearReadMessage();
                retVal = false;
            }

            PushAndSignal(event +"_AFTER");
            break;
        }
        case MessageDungeonMasterMinor::SetVar:
        {
            event += "SET_VARIABLE";

            // The value's read is picked by the type byte, from a jump table in the
            // engine: 0 INT, 1 FLOAT, 2 CExoString, 3 object (4, a location, is not
            // reported here).
            uint8_t varType;
            std::string target, key, value;
            {
                Utils::MessagePeek peek(thisPtr);
                varType = thisPtr->ReadBYTE();
                target = Utils::ObjectIDToString(thisPtr->ReadOBJECTIDServer() & 0x7FFFFFFF);
                key = thisPtr->ReadCExoString().CStr();

                switch (varType)
                {
                    case 0:
                        value = std::to_string(static_cast<uint32_t>(thisPtr->ReadINT()));
                        break;
                    case 1:
                        value = std::to_string(thisPtr->ReadFLOAT());
                        break;
                    case 2:
                        value = thisPtr->ReadCExoString().CStr();
                        break;
                    case 3:
                        value = Utils::ObjectIDToString(thisPtr->ReadOBJECTIDServer());
                        break;
                }
            }

            auto PushAndSignal = [&](const std::string& ev) -> bool {
                PushEventData("TYPE", std::to_string(varType));
                PushEventData("TARGET", target);
                PushEventData("KEY", key);
                PushEventData("VALUE", value);
                return SignalEvent(ev, oidDM);
            };

            if (PushAndSignal(event + "_BEFORE"))
            {
                retVal = s_HandlePlayerToServerDungeonMasterMessageHook->CallOriginal<int32_t>(thisPtr, pPlayer, nMinor, bGroup);
            }
            else
            {
                Utils::ClearReadMessage();
                retVal = false;
            }

            PushAndSignal(event +"_AFTER");
            break;
        }
        case MessageDungeonMasterMinor::SetTime:
        {
            event += "SET_TIME";
            DefaultSignalEvent();
            break;
        }
        case MessageDungeonMasterMinor::SetDate:
        {
            event += "SET_DATE";
            DefaultSignalEvent();
            break;
        }
        case MessageDungeonMasterMinor::SetFactionReputation:
        {
            event += "SET_FACTION_REPUTATION";
            DefaultSignalEvent();
            break;
        }
        case MessageDungeonMasterMinor::GetFactionReputation:
        {
            event += "GET_FACTION_REPUTATION";
            DefaultSignalEvent();
            break;
        }
        case MessageDungeonMasterMinor::DumpLocals:
        {
            event += "DUMP_LOCALS";
            int32_t type;
            std::string target;
            {
                Utils::MessagePeek peek(thisPtr);
                type = thisPtr->ReadINT();
                target = Utils::ObjectIDToString(thisPtr->ReadOBJECTIDServer() & 0x7FFFFFFF);
            }

            auto PushAndSignalDumpLocalsEvent = [&](const std::string& ev) -> bool {
                PushEventData("TYPE", std::to_string(type));
                PushEventData("TARGET", target);
                return SignalEvent(ev, oidDM);
            };

            if (PushAndSignalDumpLocalsEvent(event + "_BEFORE"))
            {
                retVal = s_HandlePlayerToServerDungeonMasterMessageHook->CallOriginal<int32_t>(thisPtr, pPlayer, nMinor, bGroup);
            }
            else
            {
                Utils::ClearReadMessage();
                retVal = false;
            }

            PushAndSignalDumpLocalsEvent(event + "_AFTER");
            break;
        }
        case MessageDungeonMasterMinor::GiveGoodAlignment:
        case MessageDungeonMasterMinor::GiveEvilAlignment:
        case MessageDungeonMasterMinor::GiveLawfulAlignment:
        case MessageDungeonMasterMinor::GiveChaoticAlignment:
        {
            event += "GIVE_ALIGNMENT";

            int32_t alignmentType = 0;
            switch (nMinor)
            {
                case MessageDungeonMasterMinor::GiveGoodAlignment:
                    alignmentType = Alignment::Good;
                    break;
                case MessageDungeonMasterMinor::GiveEvilAlignment:
                    alignmentType = Alignment::Evil;
                    break;
                case MessageDungeonMasterMinor::GiveLawfulAlignment:
                    alignmentType = Alignment::Lawful;
                    break;
                case MessageDungeonMasterMinor::GiveChaoticAlignment:
                    alignmentType = Alignment::Chaotic;
                    break;

                default:
                    break;
            }

            retVal = HandleGiveEvent(thisPtr, pPlayer, nMinor, bGroup, event, alignmentType);
            break;
        }
        case Constants::MessageDungeonMasterMinor::Login:
        {
            event += "PLAYERDM_LOGIN";
            std::string password;
            {
                Utils::MessagePeek peek(thisPtr);
                password = thisPtr->ReadCExoString().CStr();
            }

            auto PushAndSignalPlayerDMLoginEvent = [&](const std::string& ev) -> bool {
                PushEventData("PASSWORD", password);
                return SignalEvent(ev, oidDM);
            };

            if (PushAndSignalPlayerDMLoginEvent(event + "_BEFORE"))
            {
                retVal = s_HandlePlayerToServerDungeonMasterMessageHook->CallOriginal<int32_t>(thisPtr, pPlayer, nMinor, bGroup);
            }
            else
            {
                Utils::ClearReadMessage();
                retVal = false;
            }

            PushAndSignalPlayerDMLoginEvent(event + "_AFTER");

            break;
        }
        case Constants::MessageDungeonMasterMinor::Logout:
        {
            event += "PLAYERDM_LOGOUT";
            DefaultSignalEvent();
            break;
        }
        default:
            retVal = s_HandlePlayerToServerDungeonMasterMessageHook->CallOriginal<int32_t>(thisPtr, pPlayer, nMinor, bGroup);
            break;
    }

    return retVal;
}

}
