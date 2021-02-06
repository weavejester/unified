#include "nwnx.hpp"

#include "API/CAppManager.hpp"
#include "API/CNWSCreature.hpp"
#include "API/CNWSCreatureStats.hpp"
#include "API/CServerExoApp.hpp"


using namespace NWNXLib;
using namespace NWNXLib::API;
using ArgumentStack = Events::ArgumentStack;


namespace Risenholm
{

static Hooks::Hook s_GetFlatFootedHook = Hooks::HookFunction(Functions::_ZN12CNWSCreature13GetFlatFootedEv,
    (void*)+[](CNWSCreature *pCreature) -> int32_t
    {
        auto *pCreatureStats = pCreature->m_pStats;
        auto *pScriptVarTable = Utils::GetScriptVarTable(pCreature);

        CExoString sVarName = "FLAT_FOOTED_STATE";
        int32_t nFlatFootedState = pScriptVarTable->GetInt(sVarName);

        if (nFlatFootedState == 1)// Always FlatFooted
            return true;
        else if (nFlatFootedState == 2)// Never FlatFooted
            return false;

        if (pCreatureStats->HasFeat(Constants::Feat::UncannyReflex))
            return false;

        auto IsAIState = [&](uint16_t nAIState) -> bool {
            return ((pCreature->m_nAIState & nAIState) == nAIState);
        };

        if (pCreature->GetBlind() || pCreature->m_nState == 6/*Stunned*/ ||
            (!IsAIState(0x0002/*Arms*/) && !IsAIState(0x0004/*Legs*/)) ||
            (pCreature->m_nAnimation == Constants::Animation::KnockdownFront || pCreature->m_nAnimation == Constants::Animation::KnockdownButt))
            return true;

        return false;
    }, Hooks::Order::Final);


NWNX_EXPORT ArgumentStack SetPCLikeStatus(ArgumentStack&& args)
{
    auto sourceOID      = args.extract<ObjectID>();
    auto targetOID      = args.extract<ObjectID>();
    auto bNewAttitude   = args.extract<int32_t>();
    auto bSetReciprocal = args.extract<int32_t>();

    if (auto *pSource = Globals::AppManager()->m_pServerExoApp->GetCreatureByGameObjectID(sourceOID))
    {
        pSource->SetPVPPlayerLikesMe(targetOID, bNewAttitude, bSetReciprocal);
    }

    return {};
}

}
