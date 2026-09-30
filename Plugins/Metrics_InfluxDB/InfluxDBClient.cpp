#include "nwnx.hpp"
#include "InfluxDBClient.hpp"

#include <cerrno>

#include <netdb.h>
#include <unistd.h>
#include <stdexcept>
#include <string.h>
#include <sstream>

namespace Metrics_InfluxDB {

namespace {

std::string Replace(const std::string in, const std::string search, const std::string repl)
{
    auto ret = in;

    size_t pos = 0;
    while ((pos = ret.find(search, pos)) != std::string::npos)
    {
        ret.replace(pos, search.size(), repl);
        pos += repl.size();
    }

    return ret;
}

constexpr auto ResolveInterval = std::chrono::seconds(30);

std::string Escape(const std::string inp)
{
    return Replace(Replace(inp, " ", "\\ "), ",", "\\,");
}

}

using namespace NWNXLib::Services;

InfluxDBClient::InfluxDBClient(const std::string& host, uint16_t port)
    : m_clientData()
{
    m_clientData.m_host = host;
    m_clientData.m_port = port;
    m_clientData.m_socket = -1;
    m_clientData.m_resolved = false;
    m_clientData.m_failing = false;

    m_clientData.m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (m_clientData.m_socket == -1)
    {
        throw std::runtime_error("NWNX_Metrics_InfluxDB: Could not create socket");
    }

    m_clientData.m_server.sin_family = AF_INET;
    m_clientData.m_server.sin_port = htons(m_clientData.m_port);

    m_clientData.m_isHostLiteral = inet_aton(m_clientData.m_host.c_str(), &m_clientData.m_server.sin_addr) != 0;

    // A hostname that fails to resolve is not fatal, as the metrics server may simply be down.
    // Resolution is retried when sending data.
    EnsureResolved();
}

InfluxDBClient::~InfluxDBClient()
{
    close(m_clientData.m_socket);
}

bool InfluxDBClient::EnsureResolved()
{
    // Periodically re-resolve the hostname, in case the metrics server has moved or come back up.
    auto now = std::chrono::steady_clock::now();
    if (now >= m_clientData.m_nextResolve)
    {
        m_clientData.m_nextResolve = now + ResolveInterval;
        Resolve();
    }

    return m_clientData.m_resolved;
}

void InfluxDBClient::Resolve()
{
    if (m_clientData.m_isHostLiteral)
    {
        m_clientData.m_resolved = true;
        return;
    }

    addrinfo hints, *result = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    int ret = getaddrinfo(m_clientData.m_host.c_str(), nullptr, &hints, &result);

    if (ret)
    {
        m_clientData.m_resolved = false;
        ReportFailure(gai_strerror(ret));
        return;
    }

    sockaddr_in* host_addr = reinterpret_cast<sockaddr_in*>(result->ai_addr);
    memcpy(&m_clientData.m_server.sin_addr, &host_addr->sin_addr, sizeof(in_addr));
    freeaddrinfo(result);
    m_clientData.m_resolved = true;
}

void InfluxDBClient::ReportFailure(const char* reason)
{
    if (!m_clientData.m_failing)
    {
        LOG_WARNING("Could not send metrics to '%s:%u' (%s), metrics will be dropped until it recovers.",
            m_clientData.m_host, m_clientData.m_port, reason);
        m_clientData.m_failing = true;
    }
}

void InfluxDBClient::Send(const MetricData& data)
{
    if (!EnsureResolved())
    {
        return;
    }

    std::ostringstream oss;
    oss << Escape(data.m_name);

    for (auto& tag : data.m_tags)
    {
        if (tag.second != "")
        {
            oss << "," << Escape(tag.first) << "=" << Escape(tag.second);
        }
    }

    oss << " ";

    for (size_t i = 0; i < data.m_fields.size(); ++i)
    {
        auto& field = data.m_fields[i];
        oss << Escape(field.first) << "=" << field.second
            << ((i == data.m_fields.size() - 1) ? " " : ",");
    }

    oss << data.m_timestamp.time_since_epoch().count();
    SendSocket(oss.str());
}

void InfluxDBClient::SendSocket(const std::string message)
{
    int ret = sendto(m_clientData.m_socket, message.data(), message.size(), 0,
        reinterpret_cast<sockaddr*>(&m_clientData.m_server), sizeof(m_clientData.m_server));

    if (ret == -1)
    {
        // Stop sending until the next resolve, in case the address is stale.
        m_clientData.m_resolved = false;
        ReportFailure(strerror(errno));
    }
    else if (m_clientData.m_failing)
    {
        LOG_NOTICE("Sending metrics to '%s:%u' again.", m_clientData.m_host, m_clientData.m_port);
        m_clientData.m_failing = false;
    }
}

}
