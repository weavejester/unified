#include "Events.hpp"
#include "API/CNWSPlayer.hpp"

namespace Events {

using namespace NWNXLib;
using namespace NWNXLib::API;
using namespace NWNXLib::API::Constants;

static NWNXLib::Hooks::Hook s_HandlePlayerToServerCheatMessageHook;

static int32_t HandlePlayerToServerCheatMessageHook(CNWSMessage*, CNWSPlayer*, uint8_t);

void DebugEvents() __attribute__((constructor));
void DebugEvents()
{
    InitOnFirstSubscribe("NWNX_ON_DEBUG_.*", []() {
        s_HandlePlayerToServerCheatMessageHook = Hooks::HookFunction(
                &CNWSMessage::HandlePlayerToServerCheatMessage,
                &HandlePlayerToServerCheatMessageHook, Hooks::Order::Early);
    });
}

int32_t HandlePlayerToServerCheatMessageHook(CNWSMessage *thisPtr, CNWSPlayer *pPlayer, uint8_t nMinor)
{
    int32_t retVal;

    switch (nMinor)
    {
        case Constants::MessageCheatMinor::RunScript:
        {
            std::string scriptName;
            OBJECT_ID oidTarget = Constants::OBJECT_INVALID;
            {
                Utils::MessagePeek peek(thisPtr);
                scriptName = thisPtr->ReadCExoString().CStr();
                if (pPlayer->SatisfiesBuild(8193, 14, 0))
                    oidTarget = thisPtr->ReadOBJECTIDServer();
            }
            if (oidTarget == Constants::OBJECT_INVALID)
                oidTarget = pPlayer->m_oidNWSObject;

            auto PushAndSignalEvent = [&](const std::string& ev) -> bool {
                PushEventData("SCRIPT_NAME", scriptName);
                PushEventData("TARGET", Utils::ObjectIDToString(oidTarget & 0x7FFFFFFF));
                return SignalEvent(ev, pPlayer->m_oidNWSObject);
            };

            if (PushAndSignalEvent("NWNX_ON_DEBUG_RUN_SCRIPT_BEFORE"))
                retVal = s_HandlePlayerToServerCheatMessageHook->CallOriginal<int32_t>(thisPtr, pPlayer, nMinor);
            else
            {
                Utils::ClearReadMessage();
                retVal = false;
            }

            PushAndSignalEvent("NWNX_ON_DEBUG_RUN_SCRIPT_AFTER");

            break;
        }

        case Constants::MessageCheatMinor::RunScriptChunk:
        {
            std::string scriptChunk;
            OBJECT_ID oidTarget;
            bool bWrapIntoMain;
            {
                Utils::MessagePeek peek(thisPtr);
                scriptChunk = thisPtr->ReadCExoString().CStr();
                oidTarget = thisPtr->ReadOBJECTIDServer();
                bWrapIntoMain = thisPtr->ReadBOOL();
            }
            if (oidTarget == Constants::OBJECT_INVALID)
                oidTarget = pPlayer->m_oidNWSObject;

            auto PushAndSignalEvent = [&](const std::string& ev) -> bool {
                PushEventData("SCRIPT_CHUNK", scriptChunk);
                PushEventData("TARGET", Utils::ObjectIDToString(oidTarget  & 0x7FFFFFFF));
                PushEventData("WRAP_INTO_MAIN", std::to_string(bWrapIntoMain));
                return SignalEvent(ev, pPlayer->m_oidNWSObject);
            };

            if (PushAndSignalEvent("NWNX_ON_DEBUG_RUN_SCRIPT_CHUNK_BEFORE"))
                retVal = s_HandlePlayerToServerCheatMessageHook->CallOriginal<int32_t>(thisPtr, pPlayer, nMinor);
            else
            {
                Utils::ClearReadMessage();
                retVal = false;
            }

            PushAndSignalEvent("NWNX_ON_DEBUG_RUN_SCRIPT_CHUNK_AFTER");

            break;
        }

        case Constants::MessageCheatMinor::PlayVisualEffect:
        {
            std::string target, visualEffect, duration, x, y, z;
            {
                Utils::MessagePeek peek(thisPtr);
                target = Utils::ObjectIDToString(thisPtr->ReadOBJECTIDServer() & 0x7FFFFFFF);
                visualEffect = std::to_string(thisPtr->ReadWORD());
                duration = std::to_string(thisPtr->ReadFLOAT());
                x = std::to_string(thisPtr->ReadFLOAT());
                y = std::to_string(thisPtr->ReadFLOAT());
                z = std::to_string(thisPtr->ReadFLOAT());
            }

            auto PushAndSignalEvent = [&](const std::string& ev) -> bool {
                PushEventData("TARGET_OBJECT_ID", target);
                PushEventData("VISUAL_EFFECT", visualEffect);
                PushEventData("DURATION", duration);
                PushEventData("TARGET_POSITION_X", x);
                PushEventData("TARGET_POSITION_Y", y);
                PushEventData("TARGET_POSITION_Z", z);
                return SignalEvent(ev, pPlayer->m_oidNWSObject);
            };

            if (PushAndSignalEvent("NWNX_ON_DEBUG_PLAY_VISUAL_EFFECT_BEFORE"))
                retVal = s_HandlePlayerToServerCheatMessageHook->CallOriginal<int32_t>(thisPtr, pPlayer, nMinor);
            else
            {
                Utils::ClearReadMessage();
                retVal = false;
            }
            PushAndSignalEvent("NWNX_ON_DEBUG_PLAY_VISUAL_EFFECT_AFTER");

            break;
        }

        default:
            retVal = s_HandlePlayerToServerCheatMessageHook->CallOriginal<int32_t>(thisPtr, pPlayer, nMinor);
    }

    return retVal;
}

}
