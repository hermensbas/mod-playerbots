#ifndef _PLAYERBOT_PVEARENA_COMPAT_H
#define _PLAYERBOT_PVEARENA_COMPAT_H

class Player;

#if __has_include("PveArena.h")
#include "PveArena.h"
#define PLAYERBOTS_HAS_PVEARENA 1
#else
#define PLAYERBOTS_HAS_PVEARENA 0
#endif

namespace playerbots
{
inline bool IsInPveArena(Player* player)
{
#if PLAYERBOTS_HAS_PVEARENA
    return player && pvearena::PveArenaMgr::Instance().IsPlayerInRun(player);
#else
    (void)player;
    return false;
#endif
}
} // namespace playerbots

#endif
