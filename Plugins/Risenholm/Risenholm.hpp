#pragma once

#include "Plugin.hpp"
#include "Services/Events/Events.hpp"
#include "Services/Hooks/Hooks.hpp"

using ArgumentStack = NWNXLib::Services::Events::ArgumentStack;

namespace Risenholm
{

class Risenholm : public NWNXLib::Plugin
{
public:
    Risenholm(NWNXLib::Services::ProxyServiceList* services);
    virtual ~Risenholm() {}

    static int32_t GetFlatFootedHook(CNWSCreature*);
};

}
