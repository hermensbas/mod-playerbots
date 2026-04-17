#include "TempArenaTeamMgr.h"

#include <algorithm>
#include <memory>
#include <unordered_set>

#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "CharacterCache.h"
#include "GameTime.h"
#include "Group.h"
#include "GroupMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotOperations.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "PositionValue.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "World.h"

namespace
{
constexpr uint32 TEMP_ARENA_TEAM_STALE_MS = 15000;
constexpr uint32 TEMP_ARENA_TEAM_UPDATE_MS = 2000;
constexpr uint32 TEMP_ARENA_BUILD_RETRY_MS = 5000;
constexpr uint32 TEMP_ARENA_FORMATION_RETRY_MS = 1000;
constexpr uint32 TEMP_ARENA_POST_RELEASE_TELEPORT_DELAY = 5;
constexpr uint32 TEMP_ARENA_TEAM_RETIRE_DELAY_MS = 30000;

class TempArenaTeam final : public ArenaTeam
{
public:
    void Initialize(std::vector<Player*> const& players, ArenaType arenaType, std::string const& teamName)
    {
        ASSERT(!players.empty());

        TeamId = sArenaTeamMgr->GenerateTempArenaTeamId();
        CaptainGuid = players.front()->GetGUID();
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
            Members.push_back(member);
        }
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

ArenaTeam* CreateTempArenaTeam(std::vector<Player*> const& players, ArenaType arenaType)
{
    if (players.empty())
        return nullptr;

    TempArenaTeam* team = new TempArenaTeam();
    team->Initialize(players, arenaType, BuildTempArenaTeamName(players.front(), arenaType));
    return team;
}

uint16 ClampArenaRating(int32 rating)
{
    if (rating < 0)
        return 0;
    if (rating > 2600)
        return 2600;
    return uint16(rating);
}

void ApplyArenaTeamRatingInMemory(ArenaTeam* team, uint16 rating)
{
    if (!team)
        return;

    ArenaTeamStats stats = team->GetStats();
    stats.Rating = rating;
    team->SetArenaTeamStats(stats);

    for (auto& m : team->GetMembers())
    {
        m.PersonalRating = rating;
        m.MatchMakerRating = rating;
        m.MaxMMR = rating;
    }

    team->NotifyStatsChanged();
}

uint32 GetQueuedRealPlayersMatchmakerTarget(BattlegroundQueueTypeId queueTypeId, BattlegroundBracketId bracketId,
                                            ArenaType arenaType)
{
    BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);

    uint64 sum = 0;
    uint32 count = 0;
    std::unordered_set<uint32> seenArenaTeams;

    for (auto const& qp : bgQueue.m_QueuedPlayers)
    {
        ObjectGuid guid = qp.first;
        Player* player = ObjectAccessor::FindConnectedPlayer(guid);
        if (!player || !player->GetSession() || player->GetSession()->IsBot())
            continue;

        GroupQueueInfo ginfo;
        if (!bgQueue.GetPlayerGroupInfoData(guid, &ginfo))
            continue;

        if (!ginfo.IsRated || ginfo.BracketId != uint8(bracketId) || ginfo.ArenaType != uint8(arenaType))
            continue;

        if (ginfo.ArenaTeamId && !seenArenaTeams.insert(ginfo.ArenaTeamId).second)
            continue;

        uint32 mmr = ginfo.ArenaMatchmakerRating ? ginfo.ArenaMatchmakerRating : ginfo.ArenaTeamRating;
        if (!mmr)
            continue;

        sum += mmr;
        ++count;
    }

    return count ? uint32(sum / count) : 0;
}

uint16 GetStableArenaTargetRating(uint32 teamId, uint32 target)
{
    int32 offset = 0;
    switch (teamId % 3)
    {
        case 0:
            offset = -100;
            break;
        case 1:
            offset = 0;
            break;
        default:
            offset = 100;
            break;
    }

    return ClampArenaRating(int32(target) + offset);
}

void ApplyStay(Player* player, uint32 mapId, float x, float y, float z)
{
    if (!player)
        return;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
    if (!botAI)
        return;

    PositionMap& posMap = botAI->GetAiObjectContext()->GetValue<PositionMap&>("position")->Get();
    PositionInfo stayPos = posMap["stay"];
    stayPos.Set(x, y, z, mapId);
    posMap["stay"] = stayPos;

    botAI->ChangeStrategy("+stay", BOT_STATE_NON_COMBAT);
    botAI->ChangeStrategy("+stay", BOT_STATE_COMBAT);
}

void ClearStay(Player* player)
{
    if (!player)
        return;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
    if (!botAI)
        return;

    PositionMap& posMap = botAI->GetAiObjectContext()->GetValue<PositionMap&>("position")->Get();
    PositionInfo stayPos = posMap["stay"];
    stayPos.Reset();
    posMap["stay"] = stayPos;

    botAI->ChangeStrategy("-stay", BOT_STATE_NON_COMBAT);
    botAI->ChangeStrategy("-stay", BOT_STATE_COMBAT);
}
}

TempArenaTeamMgr& TempArenaTeamMgr::instance()
{
    static TempArenaTeamMgr instance;
    return instance;
}

bool TempArenaTeamMgr::ShouldAutoQueueLeader(Player* leader, BattlegroundQueueTypeId queueTypeId,
                                             BattlegroundBracketId bracketId, ArenaType arenaType) const
{
    if (!leader)
        return false;

    auto slotItr = _standbySlotsByQueueType.find(uint32(queueTypeId));
    if (slotItr == _standbySlotsByQueueType.end())
        return false;

    TempArenaStandbySlot const& slot = slotItr->second;
    if (!slot.teamId || slot.bracketId != uint8(bracketId) || slot.arenaType != uint8(arenaType))
        return false;

    TempArenaTeamContext const* ctx = GetContextByLeader(leader->GetGUID());
    if (!ctx || ctx->teamId != slot.teamId || ctx->detachedFromStandby)
        return false;

    return true;
}

bool TempArenaTeamMgr::HasTempArenaTeamForLeader(Player* leader, ArenaType arenaType) const
{
    if (!leader)
        return false;

    TempArenaTeamContext const* ctx = GetContextByLeader(leader->GetGUID());
    return ctx && ctx->team && ctx->arenaType == uint8(arenaType);
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
        leader->GetGUID(), ctx->memberGuids, ctx->requiredSize, ctx->teamId, ctx->teamName);

    if (!PlayerbotWorldThreadProcessor::instance().QueueOperation(std::move(op)))
        return false;

    ctx->groupFormationQueuedUntilMs = nowMs + TEMP_ARENA_FORMATION_RETRY_MS;
    return false;
}

bool TempArenaTeamMgr::HasContextForPlayer(Player* player) const
{
    return player && GetContextByPlayer(player->GetGUID()) != nullptr;
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

uint32 TempArenaTeamMgr::GetArenaPersonalRatingForPlayer(Player* player, uint8 slot) const
{
    ArenaTeam* team = GetArenaTeamForPlayer(player, slot);
    if (!team || !player)
        return 0;

    if (ArenaTeamMember* member = team->GetMember(player->GetGUID()))
        return member->PersonalRating;

    return team->GetRating();
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

void TempArenaTeamMgr::HoldQueuePositionForLeader(Player* leader, uint32 mapId, float x, float y, float z)
{
    if (!leader)
        return;

    TempArenaTeamContext* ctx = GetContextByLeader(leader->GetGUID());
    if (!ctx)
        return;

    ApplyQueueHold(*ctx, mapId, x, y, z);
}

void TempArenaTeamMgr::ResetGroupForLeader(Player* leader)
{
    if (!leader)
        return;

    TempArenaTeamContext* ctx = GetContextByLeader(leader->GetGUID());
    if (!ctx)
        return;

    ReleaseQueueHold(*ctx);
    DisbandTempGroup(*ctx);
    ctx->groupFormationQueuedUntilMs = 0;
    ctx->battlemasterGuid.Clear();
    Touch(*ctx, getMSTime());
}

bool TempArenaTeamMgr::IsTempArenaTeam(ArenaTeam const* team) const
{
    return team && IsTempArenaTeamId(team->GetId());
}

bool TempArenaTeamMgr::IsTempArenaTeamId(uint32 teamId) const
{
    return _contextsByTeamId.find(teamId) != _contextsByTeamId.end() ||
           _retiredTeamsById.find(teamId) != _retiredTeamsById.end();
}

bool TempArenaTeamMgr::ShouldSuppressArenaTeamInfoField(Player* player, uint8 slot) const
{
    return GetArenaTeamForPlayer(player, slot) != nullptr;
}

void TempArenaTeamMgr::OnQueueInvite(Player* player)
{
    if (!player)
        return;

    TempArenaTeamContext* ctx = GetContextByPlayer(player->GetGUID());
    if (!ctx)
        return;

    if (TempArenaStandbySlot* slot = GetStandbySlotForTeamId(ctx->teamId))
        slot->teamId = 0;

    ctx->detachedFromStandby = true;
    ReleaseQueueHold(*ctx);
    Touch(*ctx, getMSTime());
}

void TempArenaTeamMgr::ReleasePlayer(Player* player)
{
    if (!player)
        return;

    TempArenaTeamContext const* ctx = GetContextByPlayer(player->GetGUID());
    if (!ctx)
        return;

    RemoveContext(ctx->teamId, true, true);
}

void TempArenaTeamMgr::OnPlayerRemovedFromBattleground(Player* player, Battleground* bg)
{
    if (!player || !bg || !bg->isArena())
        return;

    TempArenaTeamContext* ctx = GetContextByPlayer(player->GetGUID());
    if (!ctx)
        return;

    ReleaseQueueHold(*ctx);
    Touch(*ctx, getMSTime());

    if (!IsAnyContextPlayerActive(*ctx))
        RemoveContext(ctx->teamId, true, true);
}

void TempArenaTeamMgr::HandleBattlegroundEnd(Battleground* bg)
{
    if (!bg || !bg->isArena())
        return;

    uint32 const allianceTeamId = bg->GetArenaTeamIdForTeam(TEAM_ALLIANCE);
    uint32 const hordeTeamId = bg->GetArenaTeamIdForTeam(TEAM_HORDE);
    uint32 const nowMs = getMSTime();

    if (TempArenaTeamContext* ctx = _contextsByTeamId.count(allianceTeamId) ? &_contextsByTeamId[allianceTeamId] : nullptr)
    {
        ReleaseQueueHold(*ctx);
        Touch(*ctx, nowMs);
    }

    if (TempArenaTeamContext* ctx = _contextsByTeamId.count(hordeTeamId) ? &_contextsByTeamId[hordeTeamId] : nullptr)
    {
        ReleaseQueueHold(*ctx);
        Touch(*ctx, nowMs);
    }
}

void TempArenaTeamMgr::Update(uint32 /*diff*/)
{
    uint32 const nowMs = getMSTime();
    CleanupRetiredArenaTeams(nowMs);

    if (!sPlayerbotAIConfig.randomBotJoinBG)
        return;

    if (_nextUpdateMs && nowMs < _nextUpdateMs)
        return;

    _nextUpdateMs = nowMs + TEMP_ARENA_TEAM_UPDATE_MS;

    EnsureStandbySlotsInitialized();

    std::vector<uint32> toRemove;
    toRemove.reserve(_contextsByTeamId.size());

    for (auto& [teamId, ctx] : _contextsByTeamId)
    {
        bool active = IsAnyContextPlayerActive(ctx);

        if (TempArenaStandbySlot* slot = GetStandbySlotForTeamId(teamId))
        {
            if (ctx.detachedFromStandby)
            {
                slot->teamId = 0;
            }
            else
            {
                Player* leader = ObjectAccessor::FindConnectedPlayer(ctx.leaderGuid);
                if (!leader || !ctx.team)
                {
                    toRemove.push_back(teamId);
                    continue;
                }

                if (!active && !IsArenaGroupReady(ctx) &&
                    !IsEligibleWildArenaBot(leader, ctx.leaderLevel, leader->GetTeamId(), ctx.teamId))
                {
                    toRemove.push_back(teamId);
                    continue;
                }

                if (!active && !IsArenaGroupReady(ctx) && !IsStandbyRosterStillBuildable(ctx))
                {
                    toRemove.push_back(teamId);
                    continue;
                }

                if (!active)
                    EnsureGroupReady(leader);
            }
        }

        if (active || (!ctx.detachedFromStandby && GetStandbySlotForTeamId(teamId)))
        {
            ctx.lastTouchedMs = nowMs;
            continue;
        }

        if ((nowMs - ctx.lastTouchedMs) >= TEMP_ARENA_TEAM_STALE_MS)
            toRemove.push_back(teamId);
    }

    for (uint32 teamId : toRemove)
        RemoveContext(teamId, true, true);

    for (auto& [queueTypeId, slot] : _standbySlotsByQueueType)
    {
        (void)queueTypeId;
        EnsureStandbySlot(slot, nowMs);
    }
}

void TempArenaTeamMgr::EnsureStandbySlotsInitialized()
{
    if (!_standbySlotsByQueueType.empty())
        return;

    if (sPlayerbotAIConfig.randomBotAutoJoinArenaBracket >= MAX_BATTLEGROUND_BRACKETS)
        return;

    auto addSlot = [&](BattlegroundQueueTypeId queueTypeId, ArenaType arenaType)
    {
        TempArenaStandbySlot slot;
        slot.arenaType = uint8(arenaType);
        slot.bracketId = uint8(sPlayerbotAIConfig.randomBotAutoJoinArenaBracket);
        slot.queueTypeId = uint32(queueTypeId);
        slot.nextPreferredTeam = urand(0, 1) == 0 ? TEAM_ALLIANCE : TEAM_HORDE;
        _standbySlotsByQueueType[slot.queueTypeId] = slot;
    };

    addSlot(BATTLEGROUND_QUEUE_2v2, ARENA_TYPE_2v2);
    addSlot(BATTLEGROUND_QUEUE_3v3, ARENA_TYPE_3v3);
    addSlot(BATTLEGROUND_QUEUE_5v5, ARENA_TYPE_5v5);
}

void TempArenaTeamMgr::EnsureStandbySlot(TempArenaStandbySlot& slot, uint32 nowMs)
{
    if (slot.teamId)
    {
        auto ctxItr = _contextsByTeamId.find(slot.teamId);
        if (ctxItr == _contextsByTeamId.end() || ctxItr->second.detachedFromStandby)
            slot.teamId = 0;
    }

    if (slot.teamId)
        return;

    if ((nowMs - slot.lastBuildAttemptMs) < TEMP_ARENA_BUILD_RETRY_MS)
        return;

    slot.lastBuildAttemptMs = nowMs;
    if (TryBuildStandbySlot(slot, slot.nextPreferredTeam, nowMs))
    {
        slot.nextPreferredTeam = slot.nextPreferredTeam == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
        return;
    }

    TeamId fallbackTeam = slot.nextPreferredTeam == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
    if (TryBuildStandbySlot(slot, fallbackTeam, nowMs))
        slot.nextPreferredTeam = fallbackTeam == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
}

bool TempArenaTeamMgr::TryBuildStandbySlot(TempArenaStandbySlot& slot, TeamId preferredTeam, uint32 nowMs)
{
    uint32 const requiredSize = uint32(slot.arenaType);
    uint32 exactLevel = 0;

    BattlegroundTypeId bgTypeId = BattlegroundMgr::BGTemplateId(BattlegroundQueueTypeId(slot.queueTypeId));
    Battleground* bg = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
    if (!bg)
        return false;

    for (uint32 level : { 80u, 70u })
    {
        PvPDifficultyEntry const* pvpDiff = GetBattlegroundBracketByLevel(bg->GetMapId(), level);
        if (pvpDiff && pvpDiff->GetBracketId() == slot.bracketId)
        {
            exactLevel = level;
            break;
        }
    }

    if (!exactLevel)
        return false;

    std::vector<Player*> players = CollectCandidates(exactLevel, preferredTeam, requiredSize);
    if (players.size() != requiredSize)
        return false;

    ArenaTeam* team = CreateTempArenaTeam(players, ArenaType(slot.arenaType));
    if (!team)
        return false;

    sArenaTeamMgr->AddArenaTeam(team);

    TempArenaTeamContext ctx;
    ctx.teamId = team->GetId();
    ctx.arenaType = slot.arenaType;
    ctx.bracketId = slot.bracketId;
    ctx.queueTypeId = slot.queueTypeId;
    ctx.leaderLevel = exactLevel;
    ctx.requiredSize = requiredSize;
    ctx.createdAtMs = nowMs;
    ctx.lastTouchedMs = nowMs;
    ctx.leaderGuid = players.front()->GetGUID();
    ctx.teamName = team->GetName();
    ctx.team = team;

    for (size_t i = 1; i < players.size(); ++i)
        ctx.memberGuids.push_back(players[i]->GetGUID());

    _teamIdByLeader[ctx.leaderGuid] = ctx.teamId;
    _teamIdByPlayer[ctx.leaderGuid] = ctx.teamId;
    for (ObjectGuid const& memberGuid : ctx.memberGuids)
        _teamIdByPlayer[memberGuid] = ctx.teamId;

    _contextsByTeamId[ctx.teamId] = std::move(ctx);
    slot.teamId = team->GetId();
    return true;
}

bool TempArenaTeamMgr::IsEligibleWildArenaBot(Player* bot, uint32 exactLevel, TeamId requiredTeam,
                                              uint32 ignoredTeamId) const
{
    if (!bot || !bot->GetSession() || !bot->GetSession()->IsBot())
        return false;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || botAI->IsRealPlayer() || botAI->HasRealPlayerMaster())
        return false;

    if (!sPlayerbotAIConfig.IsInRandomAccountList(bot->GetSession()->GetAccountId()))
        return false;

    if (sRandomPlayerbotMgr.IsAddclassBot(bot))
        return false;

    if (!bot->IsInWorld() || bot->IsBeingTeleported() || bot->IsDuringRemoveFromWorld())
        return false;

    if (bot->GetLevel() != exactLevel || bot->GetTeamId() != requiredTeam)
        return false;

    if (bot->GetInstanceId() || bot->IsInCombat() || bot->InBattleground() || bot->InArena() ||
        bot->InBattlegroundQueue())
        return false;

    if (bot->GetGroup() && !CanTakeBotFromCurrentGroup(bot))
        return false;

    if (auto itr = _teamIdByPlayer.find(bot->GetGUID()); itr != _teamIdByPlayer.end() && itr->second != ignoredTeamId)
        return false;

    return true;
}

bool TempArenaTeamMgr::IsStandbyRosterStillBuildable(TempArenaTeamContext const& ctx) const
{
    auto isBuildableMember = [&](Player* player, TeamId requiredTeam)
    {
        if (!player || !player->GetSession() || !player->GetSession()->IsBot())
            return false;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
        if (!botAI || botAI->IsRealPlayer() || botAI->HasRealPlayerMaster())
            return false;

        if (!sPlayerbotAIConfig.IsInRandomAccountList(player->GetSession()->GetAccountId()))
            return false;

        if (sRandomPlayerbotMgr.IsAddclassBot(player))
            return false;

        if (!player->IsInWorld() || player->IsDuringRemoveFromWorld())
            return false;

        if (player->GetLevel() != ctx.leaderLevel || player->GetTeamId() != requiredTeam)
            return false;

        if (player->GetInstanceId() || player->IsInCombat() || player->InBattleground() || player->InArena() ||
            player->InBattlegroundQueue())
            return false;

        if (player->GetGroup() && !CanTakeBotFromCurrentGroup(player))
            return false;

        if (auto itr = _teamIdByPlayer.find(player->GetGUID()); itr != _teamIdByPlayer.end() && itr->second != ctx.teamId)
            return false;

        return true;
    };

    Player* leader = ObjectAccessor::FindConnectedPlayer(ctx.leaderGuid);
    if (!leader || !ctx.team)
        return false;

    TeamId requiredTeam = leader->GetTeamId();
    if (!isBuildableMember(leader, requiredTeam))
        return false;

    uint32 availableMembers = 1;
    for (ObjectGuid const& memberGuid : ctx.memberGuids)
    {
        Player* member = ObjectAccessor::FindConnectedPlayer(memberGuid);
        if (!member || !isBuildableMember(member, requiredTeam))
            return false;

        ++availableMembers;
    }

    return availableMembers >= ctx.requiredSize;
}

bool TempArenaTeamMgr::CanTakeBotFromCurrentGroup(Player* bot) const
{
    Group* group = bot ? bot->GetGroup() : nullptr;
    if (!group)
        return true;

    if (group->isLFGGroup())
        return false;

    for (Group::MemberSlot const& memberSlot : group->GetMemberSlots())
    {
        if (!sPlayerbotAIConfig.IsInRandomAccountList(sCharacterCache->GetCharacterAccountIdByGuid(memberSlot.guid)))
            return false;

        Player* member = ObjectAccessor::FindConnectedPlayer(memberSlot.guid);
        if (!member)
            continue;

        WorldSession* session = member->GetSession();
        if (!session || !session->IsBot())
            return false;

        PlayerbotAI* memberBotAI = GET_PLAYERBOT_AI(member);
        if (!memberBotAI || memberBotAI->IsRealPlayer() || memberBotAI->HasRealPlayerMaster())
            return false;
    }

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
        if (member && member->GetGroup() == group && member->GetMapId() == leader->GetMapId())
            ++onlineMemberCount;
    }

    return onlineMemberCount >= ctx.requiredSize;
}

bool TempArenaTeamMgr::IsPlayerActiveForContext(Player* player, TempArenaTeamContext const& ctx) const
{
    if (!player)
        return false;

    BattlegroundQueueTypeId queueTypeId = BattlegroundQueueTypeId(ctx.queueTypeId);
    if (player->InBattlegroundQueueForBattlegroundQueueType(queueTypeId))
    {
        BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);
        GroupQueueInfo ginfo;
        if (!bgQueue.GetPlayerGroupInfoData(player->GetGUID(), &ginfo))
            return false;

        return ginfo.IsRated && ginfo.ArenaType == ctx.arenaType && ginfo.ArenaTeamId == ctx.teamId;
    }

    Battleground* bg = player->GetBattleground();
    if (!bg || !bg->isArena() || !bg->isRated())
        return false;

    return bg->GetArenaTeamIdForTeam(TEAM_ALLIANCE) == ctx.teamId ||
           bg->GetArenaTeamIdForTeam(TEAM_HORDE) == ctx.teamId;
}

bool TempArenaTeamMgr::IsAnyContextPlayerActive(TempArenaTeamContext const& ctx) const
{
    if (Player* leader = ObjectAccessor::FindConnectedPlayer(ctx.leaderGuid))
    {
        if (IsPlayerActiveForContext(leader, ctx))
            return true;
    }

    for (ObjectGuid const& memberGuid : ctx.memberGuids)
    {
        if (Player* member = ObjectAccessor::FindConnectedPlayer(memberGuid))
        {
            if (IsPlayerActiveForContext(member, ctx))
                return true;
        }
    }

    return false;
}

std::vector<Player*> TempArenaTeamMgr::CollectCandidates(uint32 exactLevel, TeamId teamId, uint32 requiredSize) const
{
    std::vector<Player*> candidates;
    PlayerBotMap allBots = sRandomPlayerbotMgr.GetAllBots();
    candidates.reserve(allBots.size());

    for (auto const& [guid, bot] : allBots)
    {
        (void)guid;
        if (!IsEligibleWildArenaBot(bot, exactLevel, teamId))
            continue;

        candidates.push_back(bot);
    }

    std::vector<Player*> players;
    players.reserve(requiredSize);

    while (players.size() < requiredSize && !candidates.empty())
    {
        uint32 const index = urand(0, candidates.size() - 1);
        players.push_back(candidates[index]);
        candidates.erase(candidates.begin() + index);
    }

    return players;
}

void TempArenaTeamMgr::OnRatedArenaGroupQueued(GroupQueueInfo* ginfo, BattlegroundBracketId bracketId, Player* leader)
{
    if (!ginfo || !ginfo->IsRated || !ginfo->ArenaType)
        return;

    BattlegroundQueueTypeId const queueTypeId = BattlegroundMgr::BGQueueTypeId(ginfo->BgTypeId, ginfo->ArenaType);
    BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);
    ArenaType const arenaType = ArenaType(ginfo->ArenaType);

    uint32 target = 0;
    if (leader && leader->GetSession() && !leader->GetSession()->IsBot())
        target = ginfo->ArenaMatchmakerRating ? ginfo->ArenaMatchmakerRating : ginfo->ArenaTeamRating;

    if (!target)
        target = GetQueuedRealPlayersMatchmakerTarget(queueTypeId, bracketId, arenaType);

    if (!target)
        return;

    for (auto& [teamId, ctx] : _contextsByTeamId)
    {
        (void)teamId;

        if (!ctx.team)
            continue;

        if (ctx.queueTypeId != uint32(queueTypeId) || ctx.bracketId != uint8(bracketId) || ctx.arenaType != uint8(arenaType))
            continue;

        GroupQueueInfo* queuedGroup = nullptr;

        if (!ginfo->IsInvitedToBGInstanceGUID && ginfo->ArenaType == ctx.arenaType && ginfo->ArenaTeamId == ctx.teamId)
            queuedGroup = ginfo;

        auto tryResolveQueuedGroup = [&](ObjectGuid const& guid)
        {
            if (queuedGroup)
                return;

            auto itr = bgQueue.m_QueuedPlayers.find(guid);
            if (itr == bgQueue.m_QueuedPlayers.end() || !itr->second)
                return;

            GroupQueueInfo* queuedInfo = itr->second;
            if (!queuedInfo->IsRated || queuedInfo->ArenaType != ctx.arenaType || queuedInfo->ArenaTeamId != ctx.teamId)
                return;

            if (queuedInfo->IsInvitedToBGInstanceGUID)
                return;

            queuedGroup = queuedInfo;
        };

        tryResolveQueuedGroup(ctx.leaderGuid);
        for (ObjectGuid const& memberGuid : ctx.memberGuids)
            tryResolveQueuedGroup(memberGuid);

        if (!queuedGroup)
            continue;

        uint16 desired = GetStableArenaTargetRating(ctx.teamId, target);
        if (ctx.team->GetRating() == desired &&
            queuedGroup->ArenaTeamRating == desired &&
            queuedGroup->ArenaMatchmakerRating == desired)
        {
            continue;
        }

        ApplyArenaTeamRatingInMemory(ctx.team, desired);
        queuedGroup->ArenaTeamRating = desired;
        queuedGroup->ArenaMatchmakerRating = desired;
    }
}

TempArenaStandbySlot* TempArenaTeamMgr::GetStandbySlotForQueueType(uint32 queueTypeId)
{
    auto itr = _standbySlotsByQueueType.find(queueTypeId);
    return itr != _standbySlotsByQueueType.end() ? &itr->second : nullptr;
}

TempArenaStandbySlot* TempArenaTeamMgr::GetStandbySlotForTeamId(uint32 teamId)
{
    for (auto& [queueTypeId, slot] : _standbySlotsByQueueType)
    {
        (void)queueTypeId;
        if (slot.teamId == teamId)
            return &slot;
    }

    return nullptr;
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

TempArenaTeamContext* TempArenaTeamMgr::GetContextByPlayer(ObjectGuid const& playerGuid)
{
    auto itr = _teamIdByPlayer.find(playerGuid);
    if (itr == _teamIdByPlayer.end())
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

void TempArenaTeamMgr::RemoveContext(uint32 teamId, bool disbandGroup, bool scheduleTeleport)
{
    auto itr = _contextsByTeamId.find(teamId);
    if (itr == _contextsByTeamId.end())
        return;

    TempArenaTeamContext ctx = std::move(itr->second);
    _contextsByTeamId.erase(itr);

    if (TempArenaStandbySlot* slot = GetStandbySlotForTeamId(teamId))
        slot->teamId = 0;

    _teamIdByLeader.erase(ctx.leaderGuid);
    _teamIdByPlayer.erase(ctx.leaderGuid);
    for (ObjectGuid const& memberGuid : ctx.memberGuids)
        _teamIdByPlayer.erase(memberGuid);

    ReleaseQueueHold(ctx);

    if (disbandGroup)
        DisbandTempGroup(ctx);

    if (scheduleTeleport)
        ScheduleWorldReturn(ctx);

    if (ctx.team)
        RetireArenaTeam(ctx, getMSTime());
}

void TempArenaTeamMgr::RetireArenaTeam(TempArenaTeamContext const& ctx, uint32 nowMs)
{
    if (!ctx.team)
        return;

    RetiredTempArenaTeam retired;
    retired.teamId = ctx.teamId;
    retired.arenaType = ctx.arenaType;
    retired.queueTypeId = ctx.queueTypeId;
    retired.removeAfterMs = nowMs + TEMP_ARENA_TEAM_RETIRE_DELAY_MS;
    retired.team = ctx.team;
    retired.playerGuids.reserve(ctx.memberGuids.size() + 1);
    retired.playerGuids.push_back(ctx.leaderGuid);
    retired.playerGuids.insert(retired.playerGuids.end(), ctx.memberGuids.begin(), ctx.memberGuids.end());

    _retiredTeamsById[retired.teamId] = std::move(retired);
}

void TempArenaTeamMgr::CleanupRetiredArenaTeams(uint32 nowMs)
{
    std::vector<uint32> toRemove;
    toRemove.reserve(_retiredTeamsById.size());

    for (auto const& [teamId, retired] : _retiredTeamsById)
    {
        if (nowMs < retired.removeAfterMs)
            continue;

        if (IsRetiredTeamStillReferenced(retired))
            continue;

        toRemove.push_back(teamId);
    }

    for (uint32 teamId : toRemove)
    {
        auto itr = _retiredTeamsById.find(teamId);
        if (itr == _retiredTeamsById.end())
            continue;

        if (itr->second.team)
        {
            sArenaTeamMgr->RemoveArenaTeam(teamId);
            delete itr->second.team;
        }

        _retiredTeamsById.erase(itr);
    }
}

bool TempArenaTeamMgr::IsRetiredTeamStillReferenced(RetiredTempArenaTeam const& retired) const
{
    BattlegroundQueueTypeId queueTypeId = BattlegroundQueueTypeId(retired.queueTypeId);
    BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);

    for (ObjectGuid const& playerGuid : retired.playerGuids)
    {
        Player* player = ObjectAccessor::FindConnectedPlayer(playerGuid);
        if (!player)
            continue;

        if (player->InBattlegroundQueueForBattlegroundQueueType(queueTypeId))
        {
            GroupQueueInfo ginfo;
            if (bgQueue.GetPlayerGroupInfoData(playerGuid, &ginfo) &&
                ginfo.IsRated && ginfo.ArenaType == retired.arenaType && ginfo.ArenaTeamId == retired.teamId)
            {
                return true;
            }
        }

        Battleground* bg = player->GetBattleground();
        if (!bg || !bg->isArena() || !bg->isRated())
            continue;

        if (bg->GetArenaTeamIdForTeam(TEAM_ALLIANCE) == retired.teamId ||
            bg->GetArenaTeamIdForTeam(TEAM_HORDE) == retired.teamId)
        {
            return true;
        }
    }

    return false;
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

void TempArenaTeamMgr::ScheduleWorldReturn(TempArenaTeamContext const& ctx)
{
    auto scheduleIfSafe = [](Player* player)
    {
        if (!player || !player->GetSession() || !player->GetSession()->IsBot())
            return;

        if (player->IsBeingTeleported() || player->IsDuringRemoveFromWorld())
            return;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
        if (!botAI || botAI->IsRealPlayer() || botAI->HasRealPlayerMaster())
            return;

        if (!sPlayerbotAIConfig.IsInRandomAccountList(player->GetSession()->GetAccountId()))
            return;

        if (sRandomPlayerbotMgr.IsAddclassBot(player))
            return;

        sRandomPlayerbotMgr.ScheduleTeleport(player->GetGUID().GetCounter(), TEMP_ARENA_POST_RELEASE_TELEPORT_DELAY);
    };

    scheduleIfSafe(ObjectAccessor::FindConnectedPlayer(ctx.leaderGuid));
    for (ObjectGuid const& memberGuid : ctx.memberGuids)
        scheduleIfSafe(ObjectAccessor::FindConnectedPlayer(memberGuid));
}

void TempArenaTeamMgr::ApplyQueueHold(TempArenaTeamContext& ctx, uint32 mapId, float x, float y, float z)
{
    ApplyStay(ObjectAccessor::FindConnectedPlayer(ctx.leaderGuid), mapId, x, y, z);
    for (ObjectGuid const& memberGuid : ctx.memberGuids)
        ApplyStay(ObjectAccessor::FindConnectedPlayer(memberGuid), mapId, x, y, z);

    ctx.queueHoldActive = true;
}

void TempArenaTeamMgr::ReleaseQueueHold(TempArenaTeamContext& ctx)
{
    if (!ctx.queueHoldActive)
        return;

    ClearStay(ObjectAccessor::FindConnectedPlayer(ctx.leaderGuid));
    for (ObjectGuid const& memberGuid : ctx.memberGuids)
        ClearStay(ObjectAccessor::FindConnectedPlayer(memberGuid));

    ctx.queueHoldActive = false;
}
