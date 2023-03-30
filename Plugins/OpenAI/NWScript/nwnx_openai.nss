#include "nwnx"

void NWNX_OpenAI_ChatAsync(json payload, string id="");

json NWNX_OpenAI_Payload(string message, string model="gpt-3.5-turbo", float temperature=0.8, int maxTokens=300);

void NWNX_OpenAI_ChatAsync(json payload, string id)
{
    NWNX_PushArgumentString(id);
    NWNX_PushArgumentJson(payload);
    NWNX_CallFunction("NWNX_OpenAI", "ChatAsync");
}

json NWNX_OpenAI_Payload(string message, string model, float temperature, int maxTokens)
{
    json jMessage = JsonObject();
    jMessage = JsonObjectSet(jMessage, "role", JsonString("user"));
    jMessage = JsonObjectSet(jMessage, "content", JsonString(message));

    json jMessageList = JsonArray();
    jMessageList = JsonArrayInsert(jMessageList, jMessage);

    json jPayload = JsonObject();
    jPayload = JsonObjectSet(jPayload, "model", JsonString(model));
    jPayload = JsonObjectSet(jPayload, "messages", jMessageList);
    jPayload = JsonObjectSet(jPayload, "temperature", JsonFloat(temperature));
    jPayload = JsonObjectSet(jPayload, "max_tokens", JsonFloat(maxTokens));

    return jPayload;
}
