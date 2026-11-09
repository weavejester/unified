#pragma once

#include "API/CNWSMessage.hpp"

#include <functional>
#include <vector>


namespace NWNXLib::Utils {

// Looking at a player message before the engine's handler reads it.
//
// There used to be a PeekMessage<T>(pMessage, offset) here that copied bytes straight
// out of m_pnReadBuffer. That only works for messages that came in over the game's own
// UDP transport. A transport plugin that serves the engine's Read* calls from somewhere
// else (NWNX_WSNetLayer, where a message is a list of typed values with no byte layout
// at all) never sets the read buffer, so a byte peek read whatever the last UDP message
// left there, and crashed on a stale pointer (niv/NWNX_WSNetLayer#3).
//
// So a peek is now done with the engine's own Read* calls, in the order and with the
// types the engine's handler reads them, inside a MessagePeek, which puts the read
// position back when it goes out of scope:
//
//     std::string sProperty; int32_t nType;
//     {
//         Utils::MessagePeek peek(pMessage);
//         sProperty = pMessage->ReadCExoString().CStr();
//         nType = pMessage->ReadINT();
//     }
//
// The types matter on a typed transport: a ReadWORD where the handler does ReadSHORT is
// a desync there. A peek may stop after any read; it need not read the whole message.
class MessagePeek
{
public:
    explicit MessagePeek(CNWSMessage *pMessage);
    ~MessagePeek();

    MessagePeek(const MessagePeek&) = delete;
    MessagePeek& operator=(const MessagePeek&) = delete;

private:
    CNWSMessage *m_pMessage;
    uint32_t m_nReadBufferPtr;
    uint32_t m_nReadFragmentsBufferPtr;
    uint32_t m_nReadFragmentsOffset;
    uint8_t m_nCurReadBit;
    uint8_t m_nLastByteBits;
    std::vector<std::function<void()>> m_Rewinds;
};

// For a plugin that serves the engine's message reads from state of its own.
// Save is called as a MessagePeek starts and returns what puts that state back as it
// ends (or an empty function when the message is not one of the plugin's). Clear is
// called by ClearReadMessage, to drop whatever is left unread.
struct MessageReadHandler
{
    std::function<std::function<void()>(CNWSMessage*)> Save;
    std::function<void(CNWSMessage*)> Clear;
};
void RegisterMessageReadHandler(MessageReadHandler handler);

}
