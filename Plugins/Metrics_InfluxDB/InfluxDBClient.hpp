#pragma once

#include "Services/Metrics/MetricData.hpp"
#include <arpa/inet.h>
#include <chrono>
#include <string>

struct InfluxDBClientData
{
    int m_socket;
    sockaddr_in m_server;
    std::string m_host;
    uint16_t m_port;
    bool m_isHostLiteral;
    bool m_resolved;
    bool m_failing;
    std::chrono::steady_clock::time_point m_nextResolve;
};

namespace Metrics_InfluxDB {

class InfluxDBClient
{
public:
    InfluxDBClient(const std::string& host, uint16_t port);
    ~InfluxDBClient();

    void Send(const NWNXLib::Services::MetricData& data);

private:
    bool EnsureResolved();
    void Resolve();
    void ReportFailure(const char* reason);
    void SendSocket(const std::string message);
    InfluxDBClientData m_clientData;
};

}
