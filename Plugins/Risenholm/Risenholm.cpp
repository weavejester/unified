#include "Risenholm.hpp"

#include "Services/Hooks/Hooks.hpp"

using namespace NWNXLib;
using namespace NWNXLib::API;

static Risenholm::Risenholm* g_plugin;

NWNX_PLUGIN_ENTRY Plugin* PluginLoad(Services::ProxyServiceList* services)
{
    g_plugin = new Risenholm::Risenholm(services);
    return g_plugin;
}


namespace Risenholm
{

Risenholm::Risenholm(Services::ProxyServiceList* services)
  : Plugin(services)
{
#define REGISTER(func)              \
    GetServices()->m_events->RegisterEvent(#func, \
        [this](ArgumentStack&& args){ return func(std::move(args)); })
    // No blades, no bows: Put your NWScript Events here.
#undef REGISTER
}

}
