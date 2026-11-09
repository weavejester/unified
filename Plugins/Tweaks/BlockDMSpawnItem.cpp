#include "nwnx.hpp"


namespace Tweaks {

using namespace NWNXLib;
using namespace NWNXLib::API;


void BlockDMSpawnItem() __attribute__((constructor));
void BlockDMSpawnItem()
{
    if (!Config::Get<bool>("BLOCK_DM_SPAWNITEM", false))
        return;

    LOG_INFO("Blocking the 'dm_spawnitem' console command.");

    static Hooks::Hook s_HandlePlayerToServerGameObjectUpdateHook =
            Hooks::HookFunction(&CNWSMessage::HandlePlayerToServerGameObjectUpdate,
            +[](CNWSMessage *pMessage, CNWSPlayer *pPlayer, uint8_t nMinor) -> int32_t
            {
                // Minor 1 starts with a CHAR naming the console command ('A' is 65).
                // Other minors are left unread, so a typed transport is not desynced.
                uint8_t nMsgType = 0;
                if (nMinor == 1)
                {
                    Utils::MessagePeek peek(pMessage);
                    nMsgType = static_cast<uint8_t>(pMessage->ReadCHAR());
                }

                LOG_DEBUG("BlockDMSpawnItem: nMinor: %i, nMsgType: %i", nMinor, nMsgType);

                // TODO: add these as constants sometime
                if (nMinor == 1 && nMsgType == 65) // dm_spawnitem
                    return false;
                else
                    return s_HandlePlayerToServerGameObjectUpdateHook->CallOriginal<int32_t>(pMessage, pPlayer, nMinor);
            }, Hooks::Order::Late);
}

}
