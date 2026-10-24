#ifndef MOD_PLAYERBOT_BOT_ACTIVITY_REGISTRY_H
#define MOD_PLAYERBOT_BOT_ACTIVITY_REGISTRY_H

// Compatibility shim. The canonical bus is BotActivityMgr in mod-playerbots.
// This module is DISABLED. If it is turned on, it must still share the hub.
// See mod-playerbots/docs/TAKEOVER.md.

#include "BotActivityMgr.h"

namespace BotActivity
{
    inline void Reserve(uint32 guidLow)
    {
        BotActivityMgr::TryReserve(guidLow, BotActivityMgr::Owner::External);
    }

    inline void Release(uint32 guidLow)
    {
        BotActivityMgr::Release(guidLow, BotActivityMgr::Owner::External);
    }

    inline bool IsReserved(uint32 guidLow)
    {
        return BotActivityMgr::IsReserved(guidLow);
    }
}

#endif
