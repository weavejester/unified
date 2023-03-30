#include "API/CNWSModule.hpp"
#include "nwnx.hpp"

#include "External/json/json.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "External/httplib.h"

#include <future>

using namespace NWNXLib;

void DoRequest(const json& payload, const std::string& id)
{
    httplib::Client client("https://api.openai.com");
    client.enable_server_certificate_verification(false);
    client.set_read_timeout(std::chrono::seconds(60));
    client.set_write_timeout(std::chrono::seconds(60));

    // See https://platform.openai.com/docs/api-reference/completions/create
    httplib::Headers header = {
        { "Authorization", "Bearer " + Config::Get<std::string>("TOKEN", "SET_ENV_CONFIG") }
    };

    std::string payload_s = payload.dump();

    LOG_INFO("Making request %s", payload_s.c_str());
    httplib::Result result = client.Post("/v1/chat/completions", header, payload_s, "application/json");

    if (result)
    {
        LOG_INFO("Got result %s", result->body.c_str());

        json bodyAsJson = json::parse(result->body);
        const std::string text = bodyAsJson["choices"][0]["message"]["content"];

        Tasks::QueueOnMainThread([=]()
            {
                auto moduleOid = NWNXLib::Utils::ObjectIDToString(Utils::GetModule()->m_idSelf);
                MessageBus::Broadcast("NWNX_EVENT_PUSH_EVENT_DATA", { "RESPONSE", text });
                MessageBus::Broadcast("NWNX_EVENT_PUSH_EVENT_DATA", { "REQUEST_ID", id });
                MessageBus::Broadcast("NWNX_EVENT_SIGNAL_EVENT", { "NWNX_ON_OPENAI_RESPONSE", moduleOid });
            });
    }
    else
    {
        LOG_ERROR("Failed to make request due to: '%s'", httplib::to_string(result.error()).c_str());
    }
}

NWNX_EXPORT ArgumentStack ChatAsync(ArgumentStack&& args)
{
    const auto payload = args.extract<JsonEngineStructure>();
    const auto id = args.extract<std::string>();
    std::async(std::launch::async, &DoRequest, payload.m_shared->m_json, id);
    return {};
}
