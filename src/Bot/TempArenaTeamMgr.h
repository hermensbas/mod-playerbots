#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "ArenaTeam.h"
#include "BattlegroundMgr.h"
#include "ObjectGuid.h"

class Battleground;
class Player;

struct TempArenaTeamContext
{
    uint32 teamId = 0;
    uint8 arenaType = 0;
    uint8 bracketId = 0;
    uint32 queueTypeId = 0;
    uint32 leaderLevel = 0;
    uint32 requiredSize = 0;
    uint32 createdAtMs = 0;
    uint32 lastTouchedMs = 0;
    uint32 groupFormationQueuedUntilMs = 0;

    ObjectGuid leaderGuid = ObjectGuid::Empty;
    ObjectGuid battlemasterGuid = ObjectGuid::Empty;
    std::vector<ObjectGuid> memberGuids;
    std::string teamName;
    ArenaTeam* team = nullptr;
};

class TempArenaTeamMgr
{
public:
    static TempArenaTeamMgr& instance();

    bool PrepareForLeader(Player* leader, BattlegroundQueueTypeId queueTypeId, BattlegroundBracketId bracketId, ArenaType arenaType);
    bool EnsureGroupReady(Player* leader);

    uint32 GetArenaTeamIdForPlayer(Player* player, uint8 slot) const;
    ArenaTeam* GetArenaTeamForPlayer(Player* player, uint8 slot) const;
    ObjectGuid GetBattlemasterGuidForLeader(Player* leader) const;
    void SetBattlemasterGuidForLeader(Player* leader, ObjectGuid const& battlemasterGuid);
    bool HasTempArenaTeamForLeader(Player* leader, ArenaType arenaType) const;
    bool IsTempArenaTeam(ArenaTeam const* team) const;
    bool IsTempArenaTeamId(uint32 teamId) const;
    bool ShouldSuppressArenaTeamInfoField(Player* player, uint8 slot) const;
    void ReleasePlayer(Player* player);

    uint32 CountReservedTeams(BattlegroundQueueTypeId queueTypeId, BattlegroundBracketId bracketId, uint32 leaderLevel, TeamId faction) const;
    void HandleBattlegroundEnd(Battleground* bg);
    void Update(uint32 diff);

private:
    TempArenaTeamMgr() = default;

    bool IsPlayerActiveForContext(Player* player, TempArenaTeamContext const& ctx) const;
    bool IsEligibleWildArenaBot(Player* bot, uint32 exactLevel, uint32 ignoredTeamId = 0) const;
    bool IsArenaGroupReady(TempArenaTeamContext const& ctx) const;
    bool CanKeepContext(TempArenaTeamContext const& ctx) const;
    std::vector<Player*> CollectCandidates(Player* leader, uint32 exactLevel, uint32 requiredSize) const;

    TempArenaTeamContext* GetContextByLeader(ObjectGuid const& leaderGuid);
    TempArenaTeamContext const* GetContextByLeader(ObjectGuid const& leaderGuid) const;
    TempArenaTeamContext const* GetContextByPlayer(ObjectGuid const& playerGuid) const;
    void Touch(TempArenaTeamContext& ctx, uint32 nowMs);
    void RemoveContext(uint32 teamId, bool disbandGroup, bool scheduleTeleportAfterCancel = false);
    void DisbandTempGroup(TempArenaTeamContext const& ctx);
    void ScheduleTeleportAfterCancelledArena(TempArenaTeamContext const& ctx, uint32 delaySeconds) const;

    std::unordered_map<uint32, TempArenaTeamContext> _contextsByTeamId;
    std::unordered_map<ObjectGuid, uint32> _teamIdByLeader;
    std::unordered_map<ObjectGuid, uint32> _teamIdByPlayer;
    uint32 _nextUpdateMs = 0;
};

#define sTempArenaTeamMgr TempArenaTeamMgr::instance()
