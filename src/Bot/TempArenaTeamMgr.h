#ifndef _PLAYERBOT_TEMPARENATEAMMGR_H
#define _PLAYERBOT_TEMPARENATEAMMGR_H

#include <string>
#include <unordered_map>
#include <vector>

#include "ObjectGuid.h"
#include "SharedDefines.h"

class ArenaTeam;
class Battleground;
class Player;

enum ArenaType : uint8;
enum BattlegroundQueueTypeId : uint8;
enum BattlegroundBracketId : uint8;

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
    bool queueHoldActive = false;
    bool detachedFromStandby = false;
};

struct TempArenaStandbySlot
{
    uint8 arenaType = 0;
    uint8 bracketId = 0;
    uint32 queueTypeId = 0;
    uint32 teamId = 0;
    uint32 lastBuildAttemptMs = 0;
    TeamId nextPreferredTeam = TEAM_ALLIANCE;
};

class TempArenaTeamMgr
{
public:
    static TempArenaTeamMgr& instance();

    bool ShouldAutoQueueLeader(Player* leader, BattlegroundQueueTypeId queueTypeId, BattlegroundBracketId bracketId,
                               ArenaType arenaType) const;
    bool HasTempArenaTeamForLeader(Player* leader, ArenaType arenaType) const;
    bool EnsureGroupReady(Player* leader);

    uint32 GetArenaTeamIdForPlayer(Player* player, uint8 slot) const;
    ArenaTeam* GetArenaTeamForPlayer(Player* player, uint8 slot) const;
    ObjectGuid GetBattlemasterGuidForLeader(Player* leader) const;
    void SetBattlemasterGuidForLeader(Player* leader, ObjectGuid const& battlemasterGuid);
    void HoldQueuePositionForLeader(Player* leader, uint32 mapId, float x, float y, float z);
    void ResetGroupForLeader(Player* leader);

    bool IsTempArenaTeam(ArenaTeam const* team) const;
    bool IsTempArenaTeamId(uint32 teamId) const;
    bool ShouldSuppressArenaTeamInfoField(Player* player, uint8 slot) const;

    void OnQueueInvite(Player* player);
    void ReleasePlayer(Player* player);
    void OnPlayerRemovedFromBattleground(Player* player, Battleground* bg);
    void HandleBattlegroundEnd(Battleground* bg);
    void Update(uint32 diff);

private:
    TempArenaTeamMgr() = default;

    void EnsureStandbySlotsInitialized();
    void EnsureStandbySlot(TempArenaStandbySlot& slot, uint32 nowMs);
    bool TryBuildStandbySlot(TempArenaStandbySlot& slot, TeamId preferredTeam, uint32 nowMs);
    bool IsEligibleWildArenaBot(Player* bot, uint32 exactLevel, TeamId requiredTeam, uint32 ignoredTeamId = 0) const;
    bool CanTakeBotFromCurrentGroup(Player* bot) const;
    bool IsArenaGroupReady(TempArenaTeamContext const& ctx) const;
    bool IsPlayerActiveForContext(Player* player, TempArenaTeamContext const& ctx) const;
    bool IsAnyContextPlayerActive(TempArenaTeamContext const& ctx) const;
    std::vector<Player*> CollectCandidates(uint32 exactLevel, TeamId teamId, uint32 requiredSize) const;

    TempArenaStandbySlot* GetStandbySlotForQueueType(uint32 queueTypeId);
    TempArenaStandbySlot* GetStandbySlotForTeamId(uint32 teamId);
    TempArenaTeamContext* GetContextByLeader(ObjectGuid const& leaderGuid);
    TempArenaTeamContext const* GetContextByLeader(ObjectGuid const& leaderGuid) const;
    TempArenaTeamContext* GetContextByPlayer(ObjectGuid const& playerGuid);
    TempArenaTeamContext const* GetContextByPlayer(ObjectGuid const& playerGuid) const;

    void Touch(TempArenaTeamContext& ctx, uint32 nowMs);
    void RemoveContext(uint32 teamId, bool disbandGroup, bool scheduleTeleport);
    void DisbandTempGroup(TempArenaTeamContext const& ctx);
    void ScheduleWorldReturn(TempArenaTeamContext const& ctx);
    void ApplyQueueHold(TempArenaTeamContext& ctx, uint32 mapId, float x, float y, float z);
    void ReleaseQueueHold(TempArenaTeamContext& ctx);

private:
    uint32 _nextUpdateMs = 0;
    std::unordered_map<uint32, TempArenaStandbySlot> _standbySlotsByQueueType;
    std::unordered_map<uint32, TempArenaTeamContext> _contextsByTeamId;
    std::unordered_map<ObjectGuid, uint32> _teamIdByLeader;
    std::unordered_map<ObjectGuid, uint32> _teamIdByPlayer;
};

#define sTempArenaTeamMgr TempArenaTeamMgr::instance()

#endif
