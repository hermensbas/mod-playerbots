#include "TempArenaTeamMgr.h"

#include <algorithm>
#include <map>

#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "Battleground.h"
#include "GameTime.h"
#include "Group.h"
#include "GroupMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotOperations.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "World.h"

namespace
{
constexpr uint32 TEMP_ARENA_TEAM_STALE_MS = 15000;
constexpr uint32 TEMP_ARENA_TEAM_UPDATE_MS = 2000;
constexpr uint32 TEMP_ARENA_TEAM_OPPONENT_GRACE_MS = 20000;
constexpr uint32 TEMP_ARENA_TEAM_CANCEL_TELEPORT_DELAY_S = 5;

class TempArenaTeam final : public ArenaTeam
{
public:
    void Initialize(Player* captain, ArenaType arenaType, std::string const& teamName)
    {
        TeamId = sArenaTeamMgr->GenerateTempArenaTeamId();
        CaptainGuid = captain->GetGUID();
        Type = uint8(arenaType);
        TeamName = teamName;

        BackgroundColor = 0;
        EmblemStyle = 0;
        EmblemColor = 0;
        BorderStyle = 0;
        BorderColor = 0;
        PreviousOpponents = 0;

        Stats.WeekGames = 0;
        Stats.WeekWins = 0;
        Stats.SeasonGames = 0;
        Stats.SeasonWins = 0;
        Stats.Rank = 0;
        Stats.Rating = 1000;

        Members.clear();
    }
};

std::string BuildTempArenaTeamName(Player* leader, ArenaType arenaType)
{
    std::string prefix = leader->GetLevel() == 70 ? "PB70-" : "PB80-";
    switch (arenaType)
    {
        case ARENA_TYPE_2v2:
            prefix += "2-";
            break;
        case ARENA_TYPE_3v3:
            prefix += "3-";
            break;
        case ARENA_TYPE_5v5:
            prefix += "5-";
            break;
        default:
            prefix += "X-";
            break;
    }

    return prefix + std::to_string(leader->GetGUID().GetCounter());
}

ArenaTeam* CreateTempArenaTeam(std::vector<Player*> const& players, ArenaType arenaType, std::string const& teamName)
{
    if (players.empty())
        return nullptr;

    TempArenaTeam* team = new TempArenaTeam();
    team->Initialize(players.front(), arenaType, teamName);

    ArenaTeamStats stats = team->GetStats();
    stats.Rating = 1000;
    stats.WeekGames = 0;
    stats.WeekWins = 0;
    stats.SeasonGames = 0;
    stats.SeasonWins = 0;
    stats.Rank = 0;
    team->SetArenaTeamStats(stats);

    for (Player* player : players)
    {
        ArenaTeamMember member;
        member.Guid = player->GetGUID();
        member.Name = player->GetName();
        member.Class = player->getClass();
        member.WeekGames = 0;
        member.WeekWins = 0;
        member.SeasonGames = 0;
        member.SeasonWins = 0;
        member.PersonalRating = 1000;
        member.MatchMakerRating = 1000;
        member.MaxMMR = 1000;
        team->GetMembers().push_back(member);
    }

    return team;
}
} // namespace

TempArenaTeamMgr& TempArenaTeamMgr::instance()
{
    static TempArenaTeamMgr instance;
    return instance;
}

bool TempArenaTeamMgr::PrepareForLeader(Player* leader, BattlegroundQueueTypeId queueTypeId, BattlegroundBracketId bracketId, ArenaType arenaType)
{
    if (!leader)
        return false;

    uint32 const level = leader->GetLevel();
    if (level != 70 && level != 80)
        return false;

    if (TempArenaTeamContext* existing = GetContextByLeader(leader->GetGUID()))
    {
        if (existing->arenaType == uint8(arenaType) && existing->queueTypeId == uint32(queueTypeId) &&
            existing->bracketId == uint8(bracketId) && existing->leaderLevel == level && existing->team)
        {
            if (CanKeepContext(*existing))
            {
                Touch(*existing, getMSTime());
                return true;
            }
        }

        RemoveContext(existing->teamId, true);
    }

    if (!IsEligibleWildArenaBot(leader, level))
        return false;

    uint32 const requiredSize = uint32(arenaType);
    if (requiredSize < 2)
        return false;

    std::vector<Player*> players = CollectCandidates(leader, level, requiredSize);
    if (players.size() != requiredSize)
        return false;

    ArenaTeam* team = CreateTempArenaTeam(players, arenaType, BuildTempArenaTeamName(leader, arenaType));
    if (!team)
        return false;

    sArenaTeamMgr->AddArenaTeam(team);

    uint32 const nowMs = getMSTime();

    TempArenaTeamContext ctx;
    ctx.teamId = team->GetId();
    ctx.arenaType = uint8(arenaType);
    ctx.bracketId = uint8(bracketId);
    ctx.queueTypeId = uint32(queueTypeId);
    ctx.leaderLevel = level;
    ctx.requiredSize = requiredSize;
    ctx.createdAtMs = nowMs;
    ctx.lastTouchedMs = nowMs;
    ctx.lastOpponentSeenMs = nowMs;
    ctx.leaderGuid = leader->GetGUID();
    ctx.teamName = team->GetName();
    ctx.team = team;

    for (Player* player : players)
    {
        if (player->GetGUID() == leader->GetGUID())
            continue;

        ctx.memberGuids.push_back(player->GetGUID());
    }

    _teamIdByLeader[ctx.leaderGuid] = ctx.teamId;
    _teamIdByPlayer[ctx.leaderGuid] = ctx.teamId;
    for (ObjectGuid const& memberGuid : ctx.memberGuids)
        _teamIdByPlayer[memberGuid] = ctx.teamId;

    _contextsByTeamId[ctx.teamId] = std::move(ctx);
    return true;
}

bool TempArenaTeamMgr::EnsureGroupReady(Player* leader)
{
    if (!leader)
        return false;

    TempArenaTeamContext* ctx = GetContextByLeader(leader->GetGUID());
    if (!ctx || !ctx->team)
        return false;

    uint32 const nowMs = getMSTime();
    Touch(*ctx, nowMs);

    if (IsArenaGroupReady(*ctx))
        return true;

    if (ctx->groupFormationQueuedUntilMs > nowMs)
        return false;

    auto op = std::make_unique<ArenaGroupFormationOperation>(
        leader->GetGUID(), ctx->memberGuids, ctx->requiredSize, ctx->teamId, ctx->teamName, ctx->leaderLevel,
        ctx->queueTypeId);

    if (!PlayerbotWorldThreadProcessor::instance().QueueOperation(std::move(op)))
        return false;

    ctx->groupFormationQueuedUntilMs = nowMs + 1000;
    return false;
}

uint32 TempArenaTeamMgr::GetArenaTeamIdForPlayer(Player* player, uint8 slot) const
{
    ArenaTeam* team = GetArenaTeamForPlayer(player, slot);
    return team ? team->GetId() : 0;
}

ArenaTeam* TempArenaTeamMgr::GetArenaTeamForPlayer(Player* player, uint8 slot) const
{
    if (!player)
        return nullptr;

    TempArenaTeamContext const* ctx = GetContextByPlayer(player->GetGUID());
    if (!ctx || !ctx->team)
        return nullptr;

    if (ctx->team->GetSlot() != slot)
        return nullptr;

    if (IsPlayerActiveForContext(player, *ctx))
        return ctx->team;

    Player* leader = ObjectAccessor::FindConnectedPlayer(ctx->leaderGuid);
    if (!leader)
        return nullptr;

    Group* group = leader->GetGroup();
    if (!group || player->GetGroup() != group)
        return nullptr;

    if (!IsArenaGroupReady(*ctx))
        return nullptr;

    return ctx->team;
}

ObjectGuid TempArenaTeamMgr::GetBattlemasterGuidForLeader(Player* leader) const
{
    if (!leader)
        return ObjectGuid::Empty;

    TempArenaTeamContext const* ctx = GetContextByLeader(leader->GetGUID());
    return ctx ? ctx->battlemasterGuid : ObjectGuid::Empty;
}

void TempArenaTeamMgr::SetBattlemasterGuidForLeader(Player* leader, ObjectGuid const& battlemasterGuid)
{
    if (!leader)
        return;

    TempArenaTeamContext* ctx = GetContextByLeader(leader->GetGUID());
    if (!ctx)
        return;

    ctx->battlemasterGuid = battlemasterGuid;
}

void TempArenaTeamMgr::MarkOpposingQueueSeen(Player* leader)
{
    if (!leader)
        return;

    TempArenaTeamContext* ctx = GetContextByLeader(leader->GetGUID());
    if (!ctx)
        return;

    ctx->lastOpponentSeenMs = getMSTime();
}

bool TempArenaTeamMgr::HasRecentOpposingQueueForLeader(Player* leader) const
{
    if (!leader)
        return false;

    TempArenaTeamContext const* ctx = GetContextByLeader(leader->GetGUID());
    if (!ctx)
        return false;

    return (getMSTime() - ctx->lastOpponentSeenMs) < TEMP_ARENA_TEAM_OPPONENT_GRACE_MS;
}

bool TempArenaTeamMgr::HasTempArenaTeamForLeader(Player* leader, ArenaType arenaType) const
{
    if (!leader)
        return false;

    TempArenaTeamContext const* ctx = GetContextByLeader(leader->GetGUID());
    return ctx && ctx->team && ctx->arenaType == uint8(arenaType);
}

bool TempArenaTeamMgr::IsTempArenaTeam(ArenaTeam const* team) const
{
    return team && IsTempArenaTeamId(team->GetId());
}

bool TempArenaTeamMgr::IsTempArenaTeamId(uint32 teamId) const
{
    return _contextsByTeamId.find(teamId) != _contextsByTeamId.end();
}

bool TempArenaTeamMgr::ShouldSuppressArenaTeamInfoField(Player* player, uint8 slot) const
{
    return GetArenaTeamForPlayer(player, slot) != nullptr;
}

void TempArenaTeamMgr::ReleasePlayer(Player* player)
{
    if (!player)
        return;

    TempArenaTeamContext const* ctx = GetContextByPlayer(player->GetGUID());
    if (!ctx)
        return;

    uint32 const teamId = ctx->teamId;
    RemoveContext(teamId, true, true);
}

uint32 TempArenaTeamMgr::CountReservedTeams(BattlegroundQueueTypeId queueTypeId, BattlegroundBracketId bracketId, uint32 leaderLevel, TeamId faction) const
{
    uint32 count = 0;
    for (auto const& [teamId, ctx] : _contextsByTeamId)
    {
        (void)teamId;
        if (!ctx.team || ctx.queueTypeId != uint32(queueTypeId) || ctx.bracketId != uint8(bracketId) || ctx.leaderLevel != leaderLevel)
            continue;

        Player* leader = ObjectAccessor::FindConnectedPlayer(ctx.leaderGuid);
        if (!leader || leader->GetTeamId() != faction)
            continue;

        if (IsPlayerActiveForContext(leader, ctx))
            continue;

        bool memberActive = false;
        for (ObjectGuid const& memberGuid : ctx.memberGuids)
        {
            Player* member = ObjectAccessor::FindConnectedPlayer(memberGuid);
            if (IsPlayerActiveForContext(member, ctx))
            {
                memberActive = true;
                break;
            }
        }

        if (memberActive)
            continue;

        ++count;
    }

    return count;
}

void TempArenaTeamMgr::HandleBattlegroundEnd(Battleground* bg)
{
    if (!bg || !bg->isArena())
        return;

    uint32 const allianceTeamId = bg->GetArenaTeamIdForTeam(TEAM_ALLIANCE);
    uint32 const hordeTeamId = bg->GetArenaTeamIdForTeam(TEAM_HORDE);

    if (IsTempArenaTeamId(allianceTeamId))
        RemoveContext(allianceTeamId, true);

    if (IsTempArenaTeamId(hordeTeamId))
        RemoveContext(hordeTeamId, true);
}

void TempArenaTeamMgr::Update(uint32 /*diff*/)
{
    uint32 const nowMs = getMSTime();
    if (_nextUpdateMs && nowMs < _nextUpdateMs)
        return;

    _nextUpdateMs = nowMs + TEMP_ARENA_TEAM_UPDATE_MS;

    std::vector<uint32> toRemove;
    toRemove.reserve(_contextsByTeamId.size());

    for (auto& [teamId, ctx] : _contextsByTeamId)
    {
        bool active = false;

        if (Player* leader = ObjectAccessor::FindConnectedPlayer(ctx.leaderGuid))
            active = IsPlayerActiveForContext(leader, ctx);

        if (!active)
        {
            for (ObjectGuid const& memberGuid : ctx.memberGuids)
            {
                if (Player* member = ObjectAccessor::FindConnectedPlayer(memberGuid))
                {
                    if (IsPlayerActiveForContext(member, ctx))
                    {
                        active = true;
                        break;
                    }
                }
            }
        }

        if (!active && IsArenaGroupReady(ctx))
            active = true;

        if (active)
        {
            ctx.lastTouchedMs = nowMs;
            continue;
        }

        if ((nowMs - ctx.lastTouchedMs) >= TEMP_ARENA_TEAM_STALE_MS)
            toRemove.push_back(teamId);
    }

    for (uint32 teamId : toRemove)
        RemoveContext(teamId, true, true);
}

bool TempArenaTeamMgr::IsPlayerActiveForContext(Player* player, TempArenaTeamContext const& ctx) const
{
    if (!player)
        return false;

    BattlegroundQueueTypeId queueTypeId = BattlegroundQueueTypeId(ctx.queueTypeId);
    if (player->InBattlegroundQueueForBattlegroundQueueType(queueTypeId))
        return true;

    Battleground* bg = player->GetBattleground();
    if (!bg || !bg->isArena() || !bg->isRated())
        return false;

    if (bg->GetArenaTeamIdForTeam(TEAM_ALLIANCE) == ctx.teamId || bg->GetArenaTeamIdForTeam(TEAM_HORDE) == ctx.teamId)
        return true;

    uint8 slot = ArenaTeam::GetSlotByType(ctx.arenaType);
    return player->GetArenaTeamId(slot) == ctx.teamId;
}

bool TempArenaTeamMgr::IsEligibleWildArenaBot(Player* bot, uint32 exactLevel, uint32 ignoredTeamId) const
{
    if (!bot || !bot->GetSession() || !bot->GetSession()->IsBot())
        return false;

    if (!bot->IsInWorld() || bot->IsBeingTeleported() || bot->IsDuringRemoveFromWorld())
        return false;

    if (bot->GetLevel() != exactLevel)
        return false;

    if (bot->GetInstanceId())
        return false;

    if (bot->IsInCombat() || bot->InBattleground() || bot->InArena() || bot->InBattlegroundQueue())
        return false;

    if (!sRandomPlayerbotMgr.IsRandomBot(bot) || sRandomPlayerbotMgr.IsAddclassBot(bot))
        return false;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || botAI->IsRealPlayer() || botAI->HasRealPlayerMaster())
        return false;

    if (bot->GetGroup())
        return false;

    if (auto itr = _teamIdByPlayer.find(bot->GetGUID()); itr != _teamIdByPlayer.end() && itr->second != ignoredTeamId)
        return false;

    return true;
}

bool TempArenaTeamMgr::IsArenaGroupReady(TempArenaTeamContext const& ctx) const
{
    Player* leader = ObjectAccessor::FindConnectedPlayer(ctx.leaderGuid);
    if (!leader)
        return false;

    Group* group = leader->GetGroup();
    if (!group || !group->IsLeader(leader->GetGUID()) || group->GetMembersCount() < ctx.requiredSize)
        return false;

    uint32 onlineMemberCount = 1;
    for (ObjectGuid const& memberGuid : ctx.memberGuids)
    {
        Player* member = ObjectAccessor::FindConnectedPlayer(memberGuid);
        if (member && member->GetGroup() == group)
            ++onlineMemberCount;
    }

    return onlineMemberCount >= ctx.requiredSize;
}

bool TempArenaTeamMgr::CanKeepContext(TempArenaTeamContext const& ctx) const
{
    if (IsArenaGroupReady(ctx))
        return true;

    Player* leader = ObjectAccessor::FindConnectedPlayer(ctx.leaderGuid);
    if (!leader || leader->GetGroup())
        return false;

    if (!IsEligibleWildArenaBot(leader, ctx.leaderLevel, ctx.teamId))
        return false;

    uint32 availableMembers = 1;
    for (ObjectGuid const& memberGuid : ctx.memberGuids)
    {
        Player* member = ObjectAccessor::FindConnectedPlayer(memberGuid);
        if (!member || member->GetTeamId() != leader->GetTeamId())
            continue;

        if (IsEligibleWildArenaBot(member, ctx.leaderLevel, ctx.teamId))
            ++availableMembers;
    }

    return availableMembers >= ctx.requiredSize;
}

std::vector<Player*> TempArenaTeamMgr::CollectCandidates(Player* leader, uint32 exactLevel, uint32 requiredSize) const
{
    std::vector<Player*> players;
    players.reserve(requiredSize);
    players.push_back(leader);

    std::vector<Player*> candidates;
    PlayerBotMap allBots = sRandomPlayerbotMgr.GetAllBots();
    candidates.reserve(allBots.size());

    for (auto const& [guid, bot] : allBots)
    {
        (void)guid;
        if (!bot || bot->GetGUID() == leader->GetGUID())
            continue;

        if (bot->GetTeamId() != leader->GetTeamId())
            continue;

        if (!IsEligibleWildArenaBot(bot, exactLevel))
            continue;

        candidates.push_back(bot);
    }

    while (players.size() < requiredSize && !candidates.empty())
    {
        uint32 const index = urand(0, candidates.size() - 1);
        players.push_back(candidates[index]);
        candidates.erase(candidates.begin() + index);
    }

    return players;
}

TempArenaTeamContext* TempArenaTeamMgr::GetContextByLeader(ObjectGuid const& leaderGuid)
{
    auto itr = _teamIdByLeader.find(leaderGuid);
    if (itr == _teamIdByLeader.end())
        return nullptr;

    auto ctxItr = _contextsByTeamId.find(itr->second);
    return ctxItr != _contextsByTeamId.end() ? &ctxItr->second : nullptr;
}

TempArenaTeamContext const* TempArenaTeamMgr::GetContextByLeader(ObjectGuid const& leaderGuid) const
{
    auto itr = _teamIdByLeader.find(leaderGuid);
    if (itr == _teamIdByLeader.end())
        return nullptr;

    auto ctxItr = _contextsByTeamId.find(itr->second);
    return ctxItr != _contextsByTeamId.end() ? &ctxItr->second : nullptr;
}

TempArenaTeamContext const* TempArenaTeamMgr::GetContextByPlayer(ObjectGuid const& playerGuid) const
{
    auto itr = _teamIdByPlayer.find(playerGuid);
    if (itr == _teamIdByPlayer.end())
        return nullptr;

    auto ctxItr = _contextsByTeamId.find(itr->second);
    return ctxItr != _contextsByTeamId.end() ? &ctxItr->second : nullptr;
}

void TempArenaTeamMgr::Touch(TempArenaTeamContext& ctx, uint32 nowMs)
{
    ctx.lastTouchedMs = nowMs;
}

void TempArenaTeamMgr::RemoveContext(uint32 teamId, bool disbandGroup, bool scheduleTeleportAfterCancel)
{
    auto itr = _contextsByTeamId.find(teamId);
    if (itr == _contextsByTeamId.end())
        return;

    TempArenaTeamContext ctx = std::move(itr->second);
    _contextsByTeamId.erase(itr);

    _teamIdByLeader.erase(ctx.leaderGuid);
    _teamIdByPlayer.erase(ctx.leaderGuid);
    for (ObjectGuid const& memberGuid : ctx.memberGuids)
        _teamIdByPlayer.erase(memberGuid);

    if (disbandGroup)
        DisbandTempGroup(ctx);

    if (scheduleTeleportAfterCancel)
        ScheduleTeleportAfterCancelledArena(ctx, TEMP_ARENA_TEAM_CANCEL_TELEPORT_DELAY_S);

    if (ctx.team)
    {
        sArenaTeamMgr->RemoveArenaTeam(ctx.teamId);
        delete ctx.team;
    }
}

void TempArenaTeamMgr::DisbandTempGroup(TempArenaTeamContext const& ctx)
{
    Player* leader = ObjectAccessor::FindConnectedPlayer(ctx.leaderGuid);
    if (!leader)
        return;

    Group* group = leader->GetGroup();
    if (!group || !group->IsLeader(leader->GetGUID()))
        return;

    std::vector<ObjectGuid> expected;
    expected.reserve(ctx.memberGuids.size() + 1);
    expected.push_back(ctx.leaderGuid);
    expected.insert(expected.end(), ctx.memberGuids.begin(), ctx.memberGuids.end());

    if (group->GetMembersCount() != expected.size())
        return;

    for (GroupReference* ref = group->GetFirstMember(); ref != nullptr; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member)
            return;

        if (std::find(expected.begin(), expected.end(), member->GetGUID()) == expected.end())
            return;
    }

    group->Disband(true);
}

void TempArenaTeamMgr::ScheduleTeleportAfterCancelledArena(TempArenaTeamContext const& ctx, uint32 delaySeconds) const
{
    auto scheduleForPlayer = [delaySeconds](ObjectGuid const& guid)
    {
        Player* player = ObjectAccessor::FindConnectedPlayer(guid);
        if (!player || !player->GetSession() || !player->GetSession()->IsBot())
            return;

        if (!sRandomPlayerbotMgr.IsRandomBot(player) || sRandomPlayerbotMgr.IsAddclassBot(player))
            return;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
        if (!botAI || botAI->IsRealPlayer() || botAI->HasRealPlayerMaster())
            return;

        if (player->InBattleground() || player->InArena())
            return;

        sRandomPlayerbotMgr.ScheduleTeleport(player->GetGUID().GetCounter(), delaySeconds);
    };

    // Only scatter bots that were already staged for the queue flow.
    if (ctx.battlemasterGuid.IsEmpty() && !IsArenaGroupReady(ctx))
        return;

    scheduleForPlayer(ctx.leaderGuid);
    for (ObjectGuid const& memberGuid : ctx.memberGuids)
        scheduleForPlayer(memberGuid);
}
