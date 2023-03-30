#include "nwnx"

void NWNX_OpenAI_ChatAsync(json messages, string id="", string model="gpt-3.5-turbo", float randomness=0.8, int maxTokens=300);

void NWNX_OpenAI_ChatAsync(json messages, string id, string model, float randomness, int maxTokens)
{
    NWNX_PushArgumentInt(maxTokens);
    NWNX_PushArgumentFloat(randomness);
    NWNX_PushArgumentString(model);
    NWNX_PushArgumentString(id);
    NWNX_PushArgumentJson(messages);
    NWNX_CallFunction("NWNX_OpenAI", "ChatAsync");
}
