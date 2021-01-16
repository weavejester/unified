#include "Risenholm.hpp"

#include "API/CNWSCreature.hpp"
#include "API/CNWSCreatureStats.hpp"
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

#undef REGISTER

    GetServices()->m_hooks->RequestExclusiveHook<Functions::_ZN12CNWSCreature13GetFlatFootedEv>(&GetFlatFootedHook);
}

int32_t Risenholm::GetFlatFootedHook(CNWSCreature *pCreature)
{
    auto *pCreatureStats = pCreature->m_pStats;
    auto *pScriptVarTable = Utils::GetScriptVarTable(pCreature);

    CExoString sVarName = "FLAT_FOOTED_STATE";
    int32_t nFlatFootedState = pScriptVarTable->GetInt(sVarName);

    if (nFlatFootedState == 1)// Always FlatFooted
        return true;
    else if (nFlatFootedState == 2)// Never FlatFooted
        return false;

    if (pCreatureStats->HasFeat(226/*Uncanny Reflex*/))
        return false;

    auto IsAIState = [&](uint16_t nAIState) -> bool {
        return ((pCreature->m_nAIState & nAIState) == nAIState);
    };

    if (pCreature->GetBlind() || pCreature->m_nState == 6/*Stunned*/ ||
        (!IsAIState(0x0002/*Arms*/) && !IsAIState(0x0004/*Legs*/)) ||
        (pCreature->m_nAnimation == Constants::Animation::KnockdownFront || pCreature->m_nAnimation == Constants::Animation::KnockdownButt))
        return true;

    return false;
}

}
