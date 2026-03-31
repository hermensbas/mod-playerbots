/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "BattleGroundJoinAction.h"

#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <sstream>

#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "BattlegroundMgr.h"
#include "Common.h"
#include "Event.h"
#include "GroupMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "PlayerbotOperations.h"
#include "PositionValue.h"
#include "ServerFacade.h"
#include "TempArenaTeamMgr.h"
#include "UpdateTime.h"
#include "PlayerbotFactory.h"

namespace
{
    // For Random Battleground queue (RB), the template BG is typically 10v10 and does not reflect the
    // actual battleground that will be selected (10/15/40). For queue filling we want a safe upper bound
    // that matches the largest possible battleground size for the current level bracket, while keeping
    // "bots only queue with real players" behavior unchanged (that logic is controlled elsewhere).
    static uint32 GetRandomBgMaxPlayersPerTeam(Player* bot, BattlegroundBracketId bracketId)
    {
        // Cache per bracket to avoid scanning every tick.
        static uint32 sCachedSize[MAX_BATTLEGROUND_BRACKETS] = {};
        static bool sCachedValid[MAX_BATTLEGROUND_BRACKETS] = {};

        uint32 br = uint32(bracketId);
        if (br < MAX_BATTLEGROUND_BRACKETS && sCachedValid[br] && sCachedSize[br])
            return sCachedSize[br];

        uint32 level = bot ? bot->GetLevel() : 0;
        uint32 maxTeamSize = 0;

        // Scan battleground queue types (non-arena, non-RB) and pick the largest team size that is valid
        // for this bracket and accessible for the bot's level.
        for (int qt = BATTLEGROUND_QUEUE_AV; qt < MAX_BATTLEGROUND_QUEUE_TYPES; ++qt)
        {
            BattlegroundQueueTypeId qid = BattlegroundQueueTypeId(qt);

            // Ignore random battleground and all-arenas pseudo queues.
            if (qid == BATTLEGROUND_QUEUE_RB)
                continue;

            // Ignore arenas.
            if (BattlegroundMgr::BGArenaType(qid) != ARENA_TYPE_NONE)
                continue;

            BattlegroundTypeId tid = BattlegroundMgr::BGTemplateId(qid);
            if (tid == BATTLEGROUND_RB)
                continue;

            if (bot && !bot->GetBGAccessByLevel(tid))
                continue;

            Battleground* tmpl = sBattlegroundMgr->GetBattlegroundTemplate(tid);
            if (!tmpl)
                continue;

            PvPDifficultyEntry const* pvpDiff = GetBattlegroundBracketByLevel(tmpl->GetMapId(), level);
            if (!pvpDiff || pvpDiff->GetBracketId() != bracketId)
                continue;

            maxTeamSize = std::max<uint32>(maxTeamSize, tmpl->GetMaxPlayersPerTeam());
        }

        // Fallback: use RB template size (usually 10) if nothing matched.
        if (!maxTeamSize)
        {
            Battleground* rbTmpl = sBattlegroundMgr->GetBattlegroundTemplate(BATTLEGROUND_RB);
            maxTeamSize = rbTmpl ? rbTmpl->GetMaxPlayersPerTeam() : 10;
        }

        // Safety clamp (WotLK max is 40).
        if (maxTeamSize > 40)
            maxTeamSize = 40;
        if (!maxTeamSize)
            maxTeamSize = 10;

        if (br < MAX_BATTLEGROUND_BRACKETS)
        {
            sCachedSize[br] = maxTeamSize;
            sCachedValid[br] = true;
        }

        return maxTeamSize;
    }

    static uint32 GetEffectiveMaxPlayersPerTeam(Player* bot, BattlegroundTypeId bgTypeId, BattlegroundBracketId bracketId, Battleground* bgTemplate)
    {
        if (!bgTemplate)
            return 0;

        uint32 teamSize = bgTemplate->GetMaxPlayersPerTeam();

        // Random BG template size is not representative; use bracket-aware upper bound.
        if (bgTypeId == BATTLEGROUND_RB)
            teamSize = GetRandomBgMaxPlayersPerTeam(bot, bracketId);

        if (teamSize > 40)
            teamSize = 40;
        if (!teamSize)
            teamSize = 10;

        return teamSize;
    }

    static uint32 GetPreferredArenaBattlemasterMap(Player* bot)
    {
        if (!bot)
            return 0;

        if (bot->GetLevel() == 70)
            return 530;

        if (bot->GetLevel() == 80)
            return 571;

        return 0;
    }

    static bool IsArenaBattlemasterCandidateAllowed(Player* bot, Creature* battlemaster)
    {
        if (!bot || !battlemaster || !battlemaster->IsBattleMaster() || battlemaster->getDeathState() == DeathState::Dead)
            return false;

        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(battlemaster->GetZoneId());
        if (!zone)
            return false;

        if (zone->team == 4 && bot->GetTeamId() == TEAM_ALLIANCE)
            return false;

        if (zone->team == 2 && bot->GetTeamId() == TEAM_HORDE)
            return false;

        return true;
    }

    struct RatedQueueSummary
    {
        uint32 waitingAllGroupsByTeam[PVP_TEAMS_COUNT] = {};
        uint32 waitingRealGroupsByTeam[PVP_TEAMS_COUNT] = {};
        uint64 waitingRealMmrSumByTeam[PVP_TEAMS_COUNT] = {};
        uint32 waitingRealMmrCountByTeam[PVP_TEAMS_COUNT] = {};
    };

    static RatedQueueSummary GetRatedQueueSummary(
        BattlegroundQueueTypeId queueTypeId, BattlegroundBracketId bracketId, ArenaType arenaType, uint32 exactLevel)
    {
        struct Key
        {
            uint32 q = 0;
            uint8 bracket = 0;
            uint8 arena = 0;
            uint8 level = 0;

            bool operator==(Key const& o) const
            {
                return q == o.q && bracket == o.bracket && arena == o.arena && level == o.level;
            }
        };

        struct KeyHash
        {
            size_t operator()(Key const& k) const
            {
                return (size_t(k.q) * 1315423911u) ^ (size_t(k.bracket) << 8) ^ (size_t(k.arena) << 16) ^
                    (size_t(k.level) << 24);
            }
        };

        struct CacheEntry
        {
            RatedQueueSummary summary;
            uint32 tsMs = 0;
        };

        static std::unordered_map<Key, CacheEntry, KeyHash> s_cache;
        static uint32 s_lastPruneMs = 0;
        constexpr uint32 kTtlMs = 1000;
        constexpr uint32 kPruneEveryMs = 5000;
        constexpr size_t kMaxEntries = 64;

        uint32 nowMs = getMSTime();
        Key key{uint32(queueTypeId), uint8(bracketId), uint8(arenaType), uint8(exactLevel)};

        auto itCached = s_cache.find(key);
        if (itCached != s_cache.end() && (nowMs - itCached->second.tsMs) <= kTtlMs)
            return itCached->second.summary;

        BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);
        RatedQueueSummary summary;
        std::unordered_set<uint32> seenGroups;

        for (auto const& qp : bgQueue.m_QueuedPlayers)
        {
            ObjectGuid guid = qp.first;
            Player* player = ObjectAccessor::FindConnectedPlayer(guid);
            if (!player || !player->GetSession())
                continue;

            GroupQueueInfo ginfo;
            if (!bgQueue.GetPlayerGroupInfoData(guid, &ginfo))
                continue;

            if (!ginfo.IsRated)
                continue;

            if (ginfo.BracketId != uint8(bracketId))
                continue;

            if (ginfo.ArenaType != uint8(arenaType))
                continue;

            if (ginfo.IsInvitedToBGInstanceGUID)
                continue;

            Player* groupLeader = player;
            if (Group* group = player->GetGroup())
            {
                if (Player* realLeader = ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID()))
                    groupLeader = realLeader;
            }

            if (!groupLeader || groupLeader->GetLevel() != exactLevel)
                continue;

            uint32 groupKey = ginfo.ArenaTeamId ? ginfo.ArenaTeamId : groupLeader->GetGUID().GetCounter();
            if (!seenGroups.insert(groupKey).second)
                continue;

            TeamId queueSide = ginfo.teamId;
            if (queueSide >= TEAM_NEUTRAL)
                continue;

            ++summary.waitingAllGroupsByTeam[queueSide];

            if (groupLeader->GetSession() && !groupLeader->GetSession()->IsBot())
            {
                ++summary.waitingRealGroupsByTeam[queueSide];

                uint32 mmr = ginfo.ArenaMatchmakerRating ? ginfo.ArenaMatchmakerRating : ginfo.ArenaTeamRating;
                if (mmr)
                {
                    summary.waitingRealMmrSumByTeam[queueSide] += mmr;
                    ++summary.waitingRealMmrCountByTeam[queueSide];
                }
            }
        }

        s_cache[key] = CacheEntry{summary, nowMs};

        if (s_cache.size() > kMaxEntries && (nowMs - s_lastPruneMs) > kPruneEveryMs)
        {
            s_lastPruneMs = nowMs;
            for (auto it = s_cache.begin(); it != s_cache.end();)
            {
                if ((nowMs - it->second.tsMs) > kPruneEveryMs)
                    it = s_cache.erase(it);
                else
                    ++it;
            }
        }

        return summary;
    }

    static uint16 ClampArenaRating(int32 rating)
    {
        if (rating < 0)
            return 0;
        if (rating > 2600)
            return 2600;
        return uint16(rating);
    }

    static void ApplyArenaTeamRatingInMemory(ArenaTeam* team, uint16 rating)
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

    static Creature* FindArenaBattlemasterForBot(Player* bot, BattlegroundTypeId bgTypeId)
    {
        if (!bot || !BattlegroundMgr::IsArenaType(bgTypeId))
            return nullptr;

        if (ObjectGuid storedGuid = sTempArenaTeamMgr.GetBattlemasterGuidForLeader(bot); !storedGuid.IsEmpty())
        {
            Unit* unit = ObjectAccessor::GetUnit(*bot, storedGuid);
            Creature* battlemaster = unit ? unit->ToCreature() : nullptr;
            if (IsArenaBattlemasterCandidateAllowed(bot, battlemaster))
                return battlemaster;
        }

        std::vector<uint32> entries;
        std::unordered_set<uint32> allowedEntries;
        std::map<TeamId, std::map<BattlegroundTypeId, std::vector<uint32>>> cache =
            sRandomPlayerbotMgr.getBattleMastersCache();

        for (uint32 entry : cache[bot->GetTeamId()][bgTypeId])
        {
            entries.push_back(entry);
            allowedEntries.insert(entry);
        }
        for (uint32 entry : cache[TEAM_NEUTRAL][bgTypeId])
        {
            entries.push_back(entry);
            allowedEntries.insert(entry);
        }

        if (allowedEntries.empty())
            return nullptr;

        uint32 preferredMap = GetPreferredArenaBattlemasterMap(bot);
        Creature* bestPreferredMap = nullptr;
        float bestPreferredMapDist = std::numeric_limits<float>::max();
        Creature* bestSameMap = nullptr;
        float bestSameMapDist = std::numeric_limits<float>::max();
        Creature* fallback = nullptr;

        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            (void)spawnId;
            if (!allowedEntries.count(data.id1))
                continue;

            Unit* unit = PlayerbotAI::GetUnit(&data);
            Creature* battlemaster = unit ? unit->ToCreature() : nullptr;
            if (!IsArenaBattlemasterCandidateAllowed(bot, battlemaster))
                continue;

            if (preferredMap && battlemaster->GetMapId() == preferredMap)
            {
                float dist2 = ServerFacade::instance().GetDistance2d(bot, data.posX, data.posY);
                if (dist2 < bestPreferredMapDist)
                {
                    bestPreferredMapDist = dist2;
                    bestPreferredMap = battlemaster;
                }
            }
            else if (battlemaster->GetMapId() == bot->GetMapId())
            {
                float dist2 = ServerFacade::instance().GetDistance2d(bot, data.posX, data.posY);
                if (dist2 < bestSameMapDist)
                {
                    bestSameMapDist = dist2;
                    bestSameMap = battlemaster;
                }
            }
            else if (!fallback)
            {
                fallback = battlemaster;
            }
        }

        Creature* selected = nullptr;
        if (bestPreferredMap)
            selected = bestPreferredMap;
        else if (bestSameMap)
            selected = bestSameMap;
        else
        {
            for (uint32 entry : entries)
            {
                CreatureData const* data = sRandomPlayerbotMgr.GetCreatureDataByEntry(entry);
                if (!data)
                    continue;

                Unit* unit = PlayerbotAI::GetUnit(data);
                Creature* battlemaster = unit ? unit->ToCreature() : nullptr;
                if (!IsArenaBattlemasterCandidateAllowed(bot, battlemaster))
                    continue;

                selected = battlemaster;
                break;
            }
        }

        if (!selected)
            selected = fallback;

        if (selected)
        {
            sTempArenaTeamMgr.SetBattlemasterGuidForLeader(bot, selected->GetGUID());
            return selected;
        }

        return nullptr;
    }

    static bool EnsureArenaQueueGroupAtBattlemaster(Player* leader, Creature* battlemaster, std::string* waitDebug)
    {
        if (!leader || !battlemaster)
        {
            if (waitDebug)
                *waitDebug = "wait cause=invalid-input";
            return false;
        }

        float const offsetDistance = 2.0f;
        float const angle = battlemaster->GetOrientation();
        WorldLocation stagingLocation(
            battlemaster->GetMapId(),
            battlemaster->GetPositionX() + std::cos(angle) * offsetDistance,
            battlemaster->GetPositionY() + std::sin(angle) * offsetDistance,
            battlemaster->GetPositionZ(),
            battlemaster->GetOrientation());

        std::vector<Player*> players;
        if (Group* group = leader->GetGroup(); group && group->IsLeader(leader->GetGUID()))
        {
            for (GroupReference* ref = group->GetFirstMember(); ref != nullptr; ref = ref->next())
            {
                if (Player* member = ref->GetSource())
                    players.push_back(member);
            }
        }
        else
        {
            players.push_back(leader);
        }

        bool allReady = true;
        std::vector<Player*> teleportTargets;

        for (Player* player : players)
        {
            if (!player || !player->IsInWorld() || player->IsBeingTeleported() || player->IsBeingTeleportedFar() ||
                player->IsDuringRemoveFromWorld())
            {
                allReady = false;
                continue;
            }

            bool wrongMap = player->GetMapId() != stagingLocation.GetMapId();
            bool tooFar =
                !wrongMap && player->GetExactDist2d(stagingLocation.GetPositionX(), stagingLocation.GetPositionY()) > 15.0f;

            if (wrongMap || tooFar)
            {
                allReady = false;
                teleportTargets.push_back(player);
            }
        }

        if (allReady)
            return true;

        for (Player* player : teleportTargets)
        {
            if (!player)
                continue;

            player->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
            player->TeleportTo(
                stagingLocation.GetMapId(), stagingLocation.GetPositionX(), stagingLocation.GetPositionY(),
                stagingLocation.GetPositionZ(), stagingLocation.GetOrientation());
        }

        if (waitDebug)
        {
            std::ostringstream out;
            out << "wait cause=group-moving bmEntry=" << battlemaster->GetEntry()
                << " bmMap=" << battlemaster->GetMapId()
                << " leaderMap=" << leader->GetMapId()
                << " stageMap=" << stagingLocation.GetMapId()
                << " stage=near-battlemaster";
            *waitDebug = out.str();
        }

        return false;
    }


} // namespace

bool BGJoinAction::Execute(Event event)
{
    uint32 queueType = AI_VALUE(uint32, "bg type");
    if (!queueType)  // force join to fill bg
    {
        if (bgList.empty())
            return false;

        size_t startIndex = urand(0, bgList.size() - 1);
        bool foundCandidate = false;

        for (size_t offset = 0; offset < bgList.size(); ++offset)
        {
            BattlegroundQueueTypeId queueTypeId =
                (BattlegroundQueueTypeId)bgList[(startIndex + offset) % bgList.size()];
            BattlegroundTypeId bgTypeId = BattlegroundMgr::BGTemplateId(queueTypeId);
            BattlegroundBracketId bracketId;
            bool isRated = false;

            Battleground* bg = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
            if (!bg)
                continue;

            uint32 mapId = bg->GetMapId();
            PvPDifficultyEntry const* pvpDiff = GetBattlegroundBracketByLevel(mapId, bot->GetLevel());
            if (!pvpDiff)
                continue;

            bracketId = pvpDiff->GetBracketId();

            if (ArenaType type = ArenaType(BattlegroundMgr::BGArenaType(queueTypeId)))
            {
                std::vector<uint32>::iterator i = find(ratedList.begin(), ratedList.end(), queueTypeId);
                if (i != ratedList.end())
                    isRated = true;

                if (isRated)
                {
                    if (!sTempArenaTeamMgr.HasTempArenaTeamForLeader(bot, type))
                        continue;

                    if (!gatherArenaTeam(type))
                        return false;
                }

                botAI->GetAiObjectContext()->GetValue<uint32>("arena type")->Set(isRated);
            }

            botAI->GetAiObjectContext()->GetValue<uint32>("bg type")->Set(queueTypeId);
            queueType = queueTypeId;
            foundCandidate = true;
            break;
        }

        if (!foundCandidate)
            return false;
    }

    return JoinQueue(queueType);
}

bool BGJoinAction::gatherArenaTeam(ArenaType type)
{
    if (!sTempArenaTeamMgr.HasTempArenaTeamForLeader(bot, type))
        return false;

    return sTempArenaTeamMgr.EnsureGroupReady(bot);
}

bool BGJoinAction::canJoinBg(BattlegroundQueueTypeId queueTypeId, BattlegroundBracketId bracketId)
{
    // check if bot can join this bracket for the specific Battleground/Arena type
    BattlegroundTypeId bgTypeId = BattlegroundMgr::BGTemplateId(queueTypeId);

    // check if already in queue
    if (bot->InBattlegroundQueueForBattlegroundQueueType(queueTypeId))
        return false;

    // check too low/high level
    if (!bot->GetBGAccessByLevel(bgTypeId))
        return false;

    // check if the bracket exists for the bot's level for the specific Battleground/Arena type
    Battleground* bg = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
    uint32 mapId = bg->GetMapId();
    PvPDifficultyEntry const* pvpDiff = GetBattlegroundBracketByLevel(mapId, bot->GetLevel());
    if (!pvpDiff)
        return false;

    BattlegroundBracketId bracket_temp = pvpDiff->GetBracketId();

    if (bracket_temp != bracketId)
        return false;

    return true;
}

bool BGJoinAction::shouldJoinBg(BattlegroundQueueTypeId queueTypeId, BattlegroundBracketId bracketId)
{
    BattlegroundTypeId bgTypeId = BattlegroundMgr::BGTemplateId(queueTypeId);
    Battleground* bg = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
    if (!bg)
        return false;

    TeamId teamId = bot->GetTeamId();
    uint32 TeamSize = GetEffectiveMaxPlayersPerTeam(bot, bgTypeId, bracketId, bg);
    uint32 BracketSize = TeamSize * 2;

    // If the bot is in a group, only the leader can queue
    if (bot->GetGroup() && !bot->GetGroup()->IsLeader(bot->GetGUID()))
        return false;

    // Check if bots should join Arena
    ArenaType type = ArenaType(BattlegroundMgr::BGArenaType(queueTypeId));
    if (type != ARENA_TYPE_NONE)
    {
        BracketSize = (uint32)(type * 2);
        TeamSize = (uint32)type;

        PlayerbotAI* leaderAI = GET_PLAYERBOT_AI(bot);
        bool isWildLeader = sRandomPlayerbotMgr.IsRandomBot(bot) && !sRandomPlayerbotMgr.IsAddclassBot(bot) &&
            leaderAI && !leaderAI->HasRealPlayerMaster();
        uint32 level = bot->GetLevel();
        bool supportedLevel = level == 70 || level == 80;

        if (isWildLeader && supportedLevel && ratedList.empty())
        {
            RatedQueueSummary summary = GetRatedQueueSummary(queueTypeId, bracketId, type, level);
            TeamId botTeam = bot->GetTeamId();
            TeamId opposingTeam = botTeam == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
            uint32 waitingRealOpposingGroups = summary.waitingRealGroupsByTeam[opposingTeam];

            if (sTempArenaTeamMgr.HasTempArenaTeamForLeader(bot, type))
            {
                if (!waitingRealOpposingGroups)
                {
                    sTempArenaTeamMgr.ReleasePlayer(bot);
                    return false;
                }

                ratedList.push_back(queueTypeId);
                return true;
            }

            uint32 waitingCurrentSideGroups =
                summary.waitingAllGroupsByTeam[botTeam] +
                sTempArenaTeamMgr.CountReservedTeams(queueTypeId, bracketId, level, botTeam);

            if (waitingRealOpposingGroups > waitingCurrentSideGroups &&
                sTempArenaTeamMgr.PrepareForLeader(bot, queueTypeId, bracketId, type))
            {
                ratedList.push_back(queueTypeId);
                return true;
            }
        }

        // Check if bots should join Skirmish Arena
        // We have extra bots queue because same faction can vs each other but can't be in the same group.
        uint32 skirmishArenaBotCount =
            sRandomPlayerbotMgr.BattlegroundData[queueTypeId][bracketId].skirmishArenaBotCount;
        uint32 skirmishArenaPlayerCount =
            sRandomPlayerbotMgr.BattlegroundData[queueTypeId][bracketId].skirmishArenaPlayerCount;
        uint32 skirmishArenaInstanceCount =
            sRandomPlayerbotMgr.BattlegroundData[queueTypeId][bracketId].skirmishArenaInstanceCount;
        uint32 activeSkirmishArenaQueue =
            sRandomPlayerbotMgr.BattlegroundData[queueTypeId][bracketId].activeSkirmishArenaQueue;
        uint32 maxRequiredSkirmishBots = BracketSize * (activeSkirmishArenaQueue + skirmishArenaInstanceCount);
        if (maxRequiredSkirmishBots != 0)
            maxRequiredSkirmishBots = maxRequiredSkirmishBots + TeamSize;

        if ((skirmishArenaBotCount + skirmishArenaPlayerCount) < maxRequiredSkirmishBots)
        {
            return true;
        }

        return false;
    }

    // Check if bots should join Battleground
    uint32 bgAllianceBotCount = sRandomPlayerbotMgr.BattlegroundData[queueTypeId][bracketId].bgAllianceBotCount;
    uint32 bgAlliancePlayerCount = sRandomPlayerbotMgr.BattlegroundData[queueTypeId][bracketId].bgAlliancePlayerCount;
    uint32 bgHordeBotCount = sRandomPlayerbotMgr.BattlegroundData[queueTypeId][bracketId].bgHordeBotCount;
    uint32 bgHordePlayerCount = sRandomPlayerbotMgr.BattlegroundData[queueTypeId][bracketId].bgHordePlayerCount;
    uint32 activeBgQueue = sRandomPlayerbotMgr.BattlegroundData[queueTypeId][bracketId].activeBgQueue;
    uint32 bgInstanceCount = sRandomPlayerbotMgr.BattlegroundData[queueTypeId][bracketId].bgInstanceCount;

    // Wild random-bots: do not join battleground queues unless there is at least one real player queued/inside.
    // (Real players in BG still count because they remain in battleground queue.)
    if (sRandomPlayerbotMgr.IsRandomBot(bot))
    {
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (botAI && !botAI->HasRealPlayerMaster() && (bgAlliancePlayerCount + bgHordePlayerCount) == 0)
            return false;
    }

    if (teamId == TEAM_ALLIANCE)
    {
        if ((bgAllianceBotCount + bgAlliancePlayerCount) < TeamSize * (activeBgQueue + bgInstanceCount))
            return true;
    }
    else
    {
        if ((bgHordeBotCount + bgHordePlayerCount) < TeamSize * (activeBgQueue + bgInstanceCount))
            return true;
    }

    return false;
}

bool BGJoinAction::isUseful()
{
    // do not try if BG bots disabled
    if (!sPlayerbotAIConfig.randomBotJoinBG)
        return false;

    // can't queue while in BG/Arena
    if (bot->InBattleground())
        return false;

    // can't queue while in BG/Arena queue
    if (bot->InBattlegroundQueue())
        return false;

    // do not try right after login (currently not working)
    if ((time(nullptr) - bot->GetInGameTime()) < 120)
        return false;

    // check level
    if (bot->GetLevel() < 10)
        return false;

    // do not try if with player master
    if (GET_PLAYERBOT_AI(bot)->HasActivePlayerMaster())
        return false;

    // do not try if in group, if in group only leader can queue
    if (bot->GetGroup() && !bot->GetGroup()->IsLeader(bot->GetGUID()))
        return false;

    // do not try if in combat
    if (bot->IsInCombat())
        return false;

    // check Deserter debuff
    if (!bot->CanJoinToBattleground())
        return false;

    // check if has free queue slots (pointless as already making sure not in queue)
    // keeping just in case.
    if (!bot->HasFreeBattlegroundQueueId())
        return false;

    // do not try if in dungeon
    // Map* map = bot->GetMap();
    // if (map && map->Instanceable())
    //     return false;

    bgList.clear();
    ratedList.clear();

    for (int bracket = BG_BRACKET_ID_FIRST; bracket < MAX_BATTLEGROUND_BRACKETS; ++bracket)
    {
        for (int queueType = BATTLEGROUND_QUEUE_AV; queueType < MAX_BATTLEGROUND_QUEUE_TYPES; ++queueType)
        {
            BattlegroundQueueTypeId queueTypeId = BattlegroundQueueTypeId(queueType);
            BattlegroundBracketId bracketId = BattlegroundBracketId(bracket);

            if (!canJoinBg(queueTypeId, bracketId))
                continue;

            if (shouldJoinBg(queueTypeId, bracketId))
                bgList.push_back(queueTypeId);
        }
    }

    if (!bgList.empty())
        return true;

    return false;
}

bool BGJoinAction::JoinQueue(uint32 type)
{
    // ignore if player is already in BG, is logging out, or already being teleport
    if (!bot || (!bot->IsInWorld() && !bot->IsBeingTeleported()) || bot->InBattleground())
        return false;

    // get BG TypeId
    BattlegroundQueueTypeId queueTypeId = BattlegroundQueueTypeId(type);
    BattlegroundTypeId bgTypeId = BattlegroundMgr::BGTemplateId(queueTypeId);
    BattlegroundBracketId bracketId;

    Battleground* bg = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
    if (!bg)
        return false;

    uint32 mapId = bg->GetMapId();
    PvPDifficultyEntry const* pvpDiff = GetBattlegroundBracketByLevel(mapId, bot->GetLevel());
    if (!pvpDiff)
        return false;

    bracketId = pvpDiff->GetBracketId();
    uint32 TeamSize = GetEffectiveMaxPlayersPerTeam(bot, bgTypeId, bracketId, bg);
    uint32 BracketSize = TeamSize * 2;
    TeamId teamId = bot->GetTeamId();

    // check if already in queue
    if (bot->InBattlegroundQueueForBattlegroundQueueType(queueTypeId))
        return false;

    // check bg req level
    if (!bot->GetBGAccessByLevel(bgTypeId))
        return false;

    // get BG MapId
    uint32 bgTypeId_ = bgTypeId;
    uint32 instanceId = 0;  // 0 = First Available

    // bool isPremade = false; //not used, line marked for removal.
    bool isArena = false;
    bool isRated = false;
    uint8 arenaslot = 0;
    uint8 asGroup = false;
    Creature* preparedArenaBattlemaster = nullptr;

    std::string _bgType;

    // check if arena
    ArenaType arenaType = ArenaType(BattlegroundMgr::BGArenaType(queueTypeId));
    if (arenaType != ARENA_TYPE_NONE)
        isArena = true;

    // This breaks groups as refresh includes a remove from group function call.
    // refresh food/regs
    // sRandomPlayerbotMgr.Refresh(bot);

    bool joinAsGroup = bot->GetGroup() && bot->GetGroup()->GetLeaderGUID() == bot->GetGUID();

    bool requestedRated = botAI->GetAiObjectContext()->GetValue<uint32>("arena type")->Get();

    if (isArena && requestedRated)
    {
        BattlegroundQueue& ratedBgQueue = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);
        if (ratedBgQueue.IsPlayerInvitedToRatedArena(bot->GetGUID()))
        {
            botAI->GetAiObjectContext()->GetValue<uint32>("bg type")->Set(0);
            return false;
        }

        if (bot->InArena() && bot->GetBattleground() && bot->GetBattleground()->isRated())
        {
            botAI->GetAiObjectContext()->GetValue<uint32>("bg type")->Set(0);
            return false;
        }
    }

    if (isArena && requestedRated && joinAsGroup && sRandomPlayerbotMgr.IsRandomBot(bot) &&
        !sRandomPlayerbotMgr.IsAddclassBot(bot))
    {
        Creature* battlemaster = FindArenaBattlemasterForBot(bot, bgTypeId);
        if (!battlemaster)
        {
            botAI->GetAiObjectContext()->GetValue<uint32>("bg type")->Set(0);
            LOG_DEBUG("playerbots", "Bot {} could not find any arena battlemaster for rated queue",
                      bot->GetGUID().ToString().c_str());
            return false;
        }

        std::string waitDebug;
        if (!EnsureArenaQueueGroupAtBattlemaster(bot, battlemaster, &waitDebug))
        {
            LOG_DEBUG("playerbots", "Bot {} is moving temp arena group to battlemaster before rated queue",
                      bot->GetGUID().ToString().c_str());
            return false;
        }

        preparedArenaBattlemaster = battlemaster;
    }

    Unit* unit = preparedArenaBattlemaster ? static_cast<Unit*>(preparedArenaBattlemaster)
                                           : botAI->GetUnit(sRandomPlayerbotMgr.GetBattleMasterGUID(bot, bgTypeId));
    if (!unit && isArena)
    {
        botAI->GetAiObjectContext()->GetValue<uint32>("bg type")->Set(0);
        LOG_DEBUG("playerbots", "Bot {} could not find Battlemaster to join", bot->GetGUID().ToString().c_str());
        return false;
    }

    // in wotlk only arena requires battlemaster guid
    // ObjectGuid guid = isArena ? unit->GetGUID() : bot->GetGUID(); //not used, line marked for removal.

    switch (bgTypeId)
    {
        case BATTLEGROUND_AV:
            _bgType = "AV";
            break;
        case BATTLEGROUND_WS:
            _bgType = "WSG";
            break;
        case BATTLEGROUND_AB:
            _bgType = "AB";
            break;
        case BATTLEGROUND_EY:
            _bgType = "EotS";
            break;
        case BATTLEGROUND_RB:
            _bgType = "Random";
            break;
        case BATTLEGROUND_SA:
            _bgType = "SotA";
            break;
        case BATTLEGROUND_IC:
            _bgType = "IoC";
            break;
        default:
            break;
    }

    if (isArena)
    {
        isArena = true;
        BracketSize = uint32(arenaType) * 2;
        TeamSize = uint32(arenaType);
        isRated = requestedRated;

        if (joinAsGroup)
            asGroup = true;

        switch (arenaType)
        {
            case ARENA_TYPE_2v2:
                arenaslot = 0;
                _bgType = "2v2";
                break;
            case ARENA_TYPE_3v3:
                arenaslot = 1;
                _bgType = "3v3";
                break;
            case ARENA_TYPE_5v5:
                arenaslot = 2;
                _bgType = "5v5";
                break;
            default:
                break;
        }
    }

    LOG_INFO("playerbots", "Bot {} {}:{} <{}> queued {} {}", bot->GetGUID().ToString().c_str(),
             bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName().c_str(), _bgType.c_str(),
             isRated   ? "Rated Arena"
             : isArena ? "Arena"
                       : "");

    if (isArena)
    {
        if (!isRated)
        {
            sRandomPlayerbotMgr.BattlegroundData[queueTypeId][bracketId].skirmishArenaBotCount++;
        }
    }
    else if (!joinAsGroup)
    {
        if (teamId == TEAM_ALLIANCE)
            sRandomPlayerbotMgr.BattlegroundData[queueTypeId][bracketId].bgAllianceBotCount++;
        else
            sRandomPlayerbotMgr.BattlegroundData[queueTypeId][bracketId].bgHordeBotCount++;
    }
    else
    {
        if (teamId == TEAM_ALLIANCE)
            sRandomPlayerbotMgr.BattlegroundData[queueTypeId][bracketId].bgAllianceBotCount +=
                bot->GetGroup()->GetMembersCount();
        else
            sRandomPlayerbotMgr.BattlegroundData[queueTypeId][bracketId].bgHordeBotCount +=
                bot->GetGroup()->GetMembersCount();
    }

    botAI->GetAiObjectContext()->GetValue<uint32>("bg type")->Set(0);

    if (!isArena)
    {
        WorldSession* session = GetBotSession();
        WorldPacket* packet = new WorldPacket(CMSG_BATTLEMASTER_JOIN, 20);
        *packet << bot->GetGUID() << bgTypeId_ << instanceId << joinAsGroup;
        /// FIX race condition
        // bot->GetSession()->HandleBattlemasterJoinOpcode(packet);
        session->QueuePacket(packet);
    }
    else
    {
        WorldSession* session = GetBotSession();
        if (isRated && sRandomPlayerbotMgr.IsRandomBot(bot) && !sRandomPlayerbotMgr.IsAddclassBot(bot))
        {
            ArenaTeam* team = sTempArenaTeamMgr.GetArenaTeamForPlayer(bot, arenaslot);
            if (!team)
            {
                botAI->GetAiObjectContext()->GetValue<uint32>("bg type")->Set(0);
                return false;
            }

            RatedQueueSummary summary = GetRatedQueueSummary(queueTypeId, bracketId, arenaType, bot->GetLevel());
            TeamId opposingTeam = bot->GetTeamId() == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
            uint32 target = 0;
            if (summary.waitingRealMmrCountByTeam[opposingTeam] > 0)
                target = uint32(summary.waitingRealMmrSumByTeam[opposingTeam] / summary.waitingRealMmrCountByTeam[opposingTeam]);

            if (!target)
                target = 1000;

            uint16 desired = ClampArenaRating(int32(target) + irand(-100, 100));
            ApplyArenaTeamRatingInMemory(team, desired);
            LOG_DEBUG("playerbots", "Bot {} <{}>: set temp arena team #{} ({}) rating/MMR to {} (target {})",
                bot->GetGUID().ToString().c_str(), bot->GetName(), team->GetId(), team->GetName().c_str(),
                desired, target);
        }
        else if (isRated)
        {
            if (!sTempArenaTeamMgr.GetArenaTeamForPlayer(bot, arenaslot))
            {
                botAI->GetAiObjectContext()->GetValue<uint32>("bg type")->Set(0);
                return false;
            }
        }

        if (isRated && joinAsGroup && sRandomPlayerbotMgr.IsRandomBot(bot) && !sRandomPlayerbotMgr.IsAddclassBot(bot))
        {
            Group* grp = bot->GetGroup();
            if (!grp || grp->GetLeaderGUID() != bot->GetGUID())
            {
                botAI->GetAiObjectContext()->GetValue<uint32>("bg type")->Set(0);
                return false;
            }

            uint32 arenaTeamId = bot->GetArenaTeamId(arenaslot);
            ArenaTeam* arenaTeam = sArenaTeamMgr->GetArenaTeamById(arenaTeamId);
            if (!arenaTeam)
            {
                botAI->GetAiObjectContext()->GetValue<uint32>("bg type")->Set(0);
                return false;
            }

            GroupJoinBattlegroundResult joinResult =
                grp->CanJoinBattlegroundQueue(bg, queueTypeId, arenaType, arenaType, true, arenaslot);
            if (joinResult <= 0)
            {
                botAI->GetAiObjectContext()->GetValue<uint32>("bg type")->Set(0);
                return false;
            }
        }

        WorldPacket arena_packet(CMSG_BATTLEMASTER_JOIN_ARENA, 20);
        arena_packet << unit->GetGUID() << arenaslot << asGroup << uint8(isRated);
        session->HandleBattlemasterJoinArena(arena_packet);
    }

    return true;
}

// Not sure if this has ever worked, but it should be similar to BGJoinAction::shouldJoinBg
bool FreeBGJoinAction::shouldJoinBg(BattlegroundQueueTypeId queueTypeId, BattlegroundBracketId bracketId)
{
    return BGJoinAction::shouldJoinBg(queueTypeId, bracketId);
}

bool BGLeaveAction::Execute(Event event)
{
    WorldSession* session = GetBotSession();

    if (!(bot->InBattlegroundQueue() || bot->InBattleground()))
        return false;

    // botAI->ChangeStrategy("-bg", BOT_STATE_NON_COMBAT);

    if (BGStatusAction::LeaveBG(botAI))
        return true;

    // leave queue if not in BG
    BattlegroundQueueTypeId queueTypeId = bot->GetBattlegroundQueueTypeId(0);
    BattlegroundTypeId _bgTypeId = BattlegroundMgr::BGTemplateId(queueTypeId);
    uint8 type = false;
    uint16 unk = 0x1F90;
    uint8 unk2 = 0x0;
    bool isArena = false;
    bool IsRandomBot = sRandomPlayerbotMgr.IsRandomBot(bot);

    ArenaType arenaType = ArenaType(BattlegroundMgr::BGArenaType(queueTypeId));
    if (arenaType)
    {
        isArena = true;
        type = arenaType;
    }

    uint32 queueType = AI_VALUE(uint32, "bg type");
    if (!queueType)
        return false;

    LOG_INFO("playerbots", "Bot {} {}:{} <{}> leaves {} queue", bot->GetGUID().ToString().c_str(),
             bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName().c_str(),
             isArena ? "Arena" : "BG");

    WorldPacket packet(CMSG_BATTLEFIELD_PORT, 20);
    packet << type << unk2 << (uint32)_bgTypeId << unk << uint8(0);
    session->QueuePacket(new WorldPacket(packet));

    if (IsRandomBot)
        botAI->SetMaster(nullptr);

    botAI->ResetStrategies(!IsRandomBot);
    botAI->GetAiObjectContext()->GetValue<uint32>("bg type")->Set(0);
    botAI->GetAiObjectContext()->GetValue<uint32>("bg role")->Set(0);
    botAI->GetAiObjectContext()->GetValue<uint32>("arena type")->Set(0);

    return true;
}

bool BGStatusAction::LeaveBG(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    WorldSession* session = bot->GetSession();

    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;
    bool isArena = bg->isArena();
    bool isRandomBot = sRandomPlayerbotMgr.IsRandomBot(bot);


    // Snapshot before we clear master (we need it to pick the correct gear limits).
    bool hadRealMaster = false;
    if (isRandomBot)
        hadRealMaster = botAI->HasRealPlayerMaster();

    if (isRandomBot)
        botAI->SetMaster(nullptr);

    botAI->ChangeStrategy("-warsong", BOT_STATE_COMBAT);
    botAI->ChangeStrategy("-warsong", BOT_STATE_NON_COMBAT);
    botAI->ChangeStrategy("-arathi", BOT_STATE_COMBAT);
    botAI->ChangeStrategy("-arathi", BOT_STATE_NON_COMBAT);
    botAI->ChangeStrategy("-eye", BOT_STATE_COMBAT);
    botAI->ChangeStrategy("-eye", BOT_STATE_NON_COMBAT);
    botAI->ChangeStrategy("-isle", BOT_STATE_COMBAT);
    botAI->ChangeStrategy("-isle", BOT_STATE_NON_COMBAT);
    botAI->ChangeStrategy("-Battleground", BOT_STATE_COMBAT);
    botAI->ChangeStrategy("-Battleground", BOT_STATE_NON_COMBAT);
    botAI->ChangeStrategy("-arena", BOT_STATE_COMBAT);
    botAI->ChangeStrategy("-arena", BOT_STATE_NON_COMBAT);

    LOG_INFO("playerbots", "Bot {} {}:{} <{}> leaves {}", bot->GetGUID().ToString().c_str(),
             bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName(),
             isArena ? "Arena" : "BG");

    WorldPacket packet(CMSG_LEAVE_BATTLEFIELD);
    packet << uint8(0);
    packet << uint8(0);  // BattlegroundTypeId-1 ?
    packet << uint32(0);
    packet << uint16(0);

    session->HandleBattlefieldLeaveOpcode(packet);

    // Wild random-bots: never keep Deserter. It can appear due to forced leaves, bugs or edge cases.
    // Removing it here keeps bots from getting stuck unable to queue again.
    if (isRandomBot && !hadRealMaster && bot->HasAura(26013))
        bot->RemoveAurasDueToSpell(26013);

    botAI->ResetStrategies(!isRandomBot);
    botAI->GetAiObjectContext()->GetValue<uint32>("bg type")->Set(0);
    botAI->GetAiObjectContext()->GetValue<uint32>("bg role")->Set(0);
    botAI->GetAiObjectContext()->GetValue<uint32>("arena type")->Set(0);
    PositionMap& posMap = botAI->GetAiObjectContext()->GetValue<PositionMap&>("position")->Get();
    PositionInfo pos = botAI->GetAiObjectContext()->GetValue<PositionMap&>("position")->Get()["bg objective"];
    pos.Reset();
    posMap["bg objective"] = pos;


    // Random bots only: schedule PvE re-equip after leaving BG/arena.
    // Leaving a battleground/arena usually involves a map transfer, so we defer the actual re-equip
    // until the bot is back in world (handled in PlayerbotAI::UpdateAI).
    if (isRandomBot)
        botAI->SetPendingPveGearReequip(hadRealMaster);
    return true;
}

bool BGStatusAction::isUseful() { return bot->InBattlegroundQueue(); }

bool BGStatusAction::Execute(Event event)
{
    uint32 QueueSlot;
    uint32 instanceId;
    uint32 mapId;
    uint32 statusid;
    uint32 Time1;
    uint32 Time2;
    std::string _bgType;

    uint64 arenaByte;
    uint8 arenaTeam;
    uint8 isRated;
    uint64 unk0;
    uint8 minlevel;
    uint8 maxlevel;

    WorldPacket p(event.getPacket());
    statusid = 0;
    p >> QueueSlot;  // queue id (0...2) - player can be in 3 queues in time
    p >> arenaByte;
    if (arenaByte == 0)
        return false;
    p >> minlevel;
    p >> maxlevel;
    p >> instanceId;
    p >> isRated;
    p >> statusid;

    // check status
    switch (statusid)
    {
        case STATUS_WAIT_QUEUE:  // status_in_queue
            p >> Time1;          // average wait time, milliseconds
            p >> Time2;          // time in queue, updated every minute!, milliseconds
            break;
        case STATUS_WAIT_JOIN:  // status_invite
            p >> mapId;         // map id
            p >> unk0;
            p >> Time1;  // time to remove from queue, milliseconds
            break;
        case STATUS_IN_PROGRESS:  // status_in_progress
            p >> mapId;           // map id
            p >> unk0;
            p >> Time1;  // time to bg auto leave, 0 at bg start, 120000 after bg end, milliseconds
            p >> Time2;  // time from bg start, milliseconds
            p >> arenaTeam;
            break;
        default:
            LOG_ERROR("playerbots", "Unknown BG status!");
            break;
    }

    bool IsRandomBot = sRandomPlayerbotMgr.IsRandomBot(bot);
    BattlegroundQueueTypeId queueTypeId = bot->GetBattlegroundQueueTypeId(QueueSlot);
    BattlegroundTypeId _bgTypeId = BattlegroundMgr::BGTemplateId(queueTypeId);
    if (!queueTypeId)
        return false;

    BattlegroundBracketId bracketId = BG_BRACKET_ID_FIRST;

    Battleground* bg = sBattlegroundMgr->GetBattlegroundTemplate(_bgTypeId);
    if (!bg)
        return false;

    mapId = bg->GetMapId();

    PvPDifficultyEntry const* pvpDiff = GetBattlegroundBracketByLevel(mapId, bot->GetLevel());
    if (!pvpDiff)
        return false;

    bracketId = pvpDiff->GetBracketId();

    bool isArena = false;
    uint8 type = false;  // arenatype if arena
    uint16 unk = 0x1F90;
    uint8 unk2 = 0x0;
    uint8 action = 0x1;

    ArenaType arenaType = ArenaType(BattlegroundMgr::BGArenaType(queueTypeId));
    if (arenaType)
    {
        isArena = true;
        type = arenaType;
    }

    switch (_bgTypeId)
    {
        case BATTLEGROUND_AV:
            _bgType = "AV";
            break;
        case BATTLEGROUND_WS:
            _bgType = "WSG";
            break;
        case BATTLEGROUND_AB:
            _bgType = "AB";
            break;
        case BATTLEGROUND_EY:
            _bgType = "EotS";
            break;
        case BATTLEGROUND_RB:
            _bgType = "Random";
            break;
        case BATTLEGROUND_SA:
            _bgType = "SotA";
            break;
        case BATTLEGROUND_IC:
            _bgType = "IoC";
            break;
        default:
            break;
    }

    switch (arenaType)
    {
        case ARENA_TYPE_2v2:
            _bgType = "2v2";
            break;
        case ARENA_TYPE_3v3:
            _bgType = "3v3";
            break;
        case ARENA_TYPE_5v5:
            _bgType = "5v5";
            break;
        default:
            break;
    }

    //TeamId teamId = bot->GetTeamId(); //not used, line marked for removal.

    if (Time1 == TIME_TO_AUTOREMOVE)  // Battleground is over, bot needs to leave
    {
        LOG_INFO("playerbots", "Bot {} <{}> ({} {}): Received BG status TIME_TO_AUTOREMOVE for {} {}",
                 bot->GetGUID().ToString().c_str(), bot->GetName(), bot->GetLevel(),
                 bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", isArena ? "Arena" : "BG", _bgType);

        if (LeaveBG(botAI))
            return true;
    }

    if (statusid == STATUS_WAIT_QUEUE)  // bot is in queue
    {
        LOG_INFO("playerbots", "Bot {} {}:{} <{}>: Received BG status WAIT_QUEUE (wait time: {}) for {} {}",
                 bot->GetGUID().ToString().c_str(), bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(),
                 bot->GetName(), Time2, isArena ? "Arena" : "BG", _bgType);
        // temp fix for crash
        // return true;

        BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);
        GroupQueueInfo ginfo;
        if (bgQueue.GetPlayerGroupInfoData(bot->GetGUID(), &ginfo))
        {
            // Wild random-bots: do not stay queued for battlegrounds when there are no real players queued.
            // This prevents bot-only battleground instances from starting when real players leave the queue.
            if (ginfo.IsInvitedToBGInstanceGUID && !bot->InBattleground())
            {
                // BattlegroundMgr::GetBattleground() does not return battleground if bgTypeId==BATTLEGROUND_AA
                Battleground* bg = sBattlegroundMgr->GetBattleground(
                    ginfo.IsInvitedToBGInstanceGUID, _bgTypeId == BATTLEGROUND_AA ? BATTLEGROUND_TYPE_NONE : _bgTypeId);
                if (bg)
                {
                    if (isArena)
                    {
                        _bgTypeId = bg->GetBgTypeID();
                    }

                    LOG_INFO("playerbots", "Bot {} {}:{} <{}>: Force join {} {}", bot->GetGUID().ToString().c_str(),
                             bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName(),
                             isArena ? "Arena" : "BG", _bgType);
                    WorldPacket emptyPacket;
                    WorldSession* session = GetBotSession();
                    session->HandleCancelMountAuraOpcode(emptyPacket);
                    action = 0x1;

                    WorldPacket packet(CMSG_BATTLEFIELD_PORT, 20);
                    packet << type << unk2 << (uint32)_bgTypeId << unk << action;
                    session->QueuePacket(new WorldPacket(packet));

                    botAI->ResetStrategies(false);
                    if (!bot->GetBattleground())
                    {
                        // first bot to join wont have battleground and PlayerbotAI::ResetStrategies() wont set them up
                        // properly, set bg for "bg strategy check" to fix that
                        botAI->ChangeStrategy("+bg", BOT_STATE_NON_COMBAT);
                    }
                    context->GetValue<uint32>("bg role")->Set(urand(0, 9));
                    PositionMap& posMap = context->GetValue<PositionMap&>("position")->Get();
                    PositionInfo pos = context->GetValue<PositionMap&>("position")->Get()["bg objective"];
                    pos.Reset();
                    posMap["bg objective"] = pos;

                    return true;
                }
            }
        }

        Battleground* bg = sBattlegroundMgr->GetBattlegroundTemplate(_bgTypeId);
        if (!bg)
            return false;

        bool leaveQ = false;
        uint32 timer;
        if (isArena)
            timer = TIME_TO_AUTOREMOVE;
        else
        {
            uint32 teamSize = bg->GetMaxPlayersPerTeam();

            // For Random Battleground queue, use bracket-aware upper bound so bots don't leave too early
            // when a 15v15 or 40v40 battleground is selected.
            if (_bgTypeId == BATTLEGROUND_RB)
            {
                if (PvPDifficultyEntry const* pvpDiff = GetBattlegroundBracketByLevel(bg->GetMapId(), bot->GetLevel()))
                    teamSize = GetEffectiveMaxPlayersPerTeam(bot, _bgTypeId, pvpDiff->GetBracketId(), bg);
            }

            timer = TIME_TO_AUTOREMOVE + 1000 * (teamSize * 8);
        }

        if (Time2 > timer && isArena)  // disabled for BG
            leaveQ = true;

        if (leaveQ && ((bot->GetGroup() && bot->GetGroup()->IsLeader(bot->GetGUID())) ||
                       !(bot->GetGroup() || botAI->GetMaster())))
        {
            //TeamId teamId = bot->GetTeamId(); //not used, line marked for removal.
            bool realPlayers = false;
            if (isRated)
                realPlayers = sRandomPlayerbotMgr.BattlegroundData[queueTypeId][bracketId].ratedArenaPlayerCount > 0;
            else
                realPlayers =
                    sRandomPlayerbotMgr.BattlegroundData[queueTypeId][bracketId].skirmishArenaPlayerCount > 0;

            if (realPlayers)
                return false;

            LOG_INFO("playerbots", "Bot {} {}:{} <{}> waited too long and leaves queue ({} {}).",
                     bot->GetGUID().ToString().c_str(), bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(),
                     bot->GetName(), isArena ? "Arena" : "BG", _bgType);

            WorldPacket packet(CMSG_BATTLEFIELD_PORT, 20);
            action = 0;
            packet << type << unk2 << (uint32)_bgTypeId << unk << action;
            WorldSession* session = GetBotSession();
            session->QueuePacket(new WorldPacket(packet));

            botAI->ResetStrategies(!IsRandomBot);
            botAI->GetAiObjectContext()->GetValue<uint32>("bg type")->Set(0);
            botAI->GetAiObjectContext()->GetValue<uint32>("bg role")->Set(0);
            botAI->GetAiObjectContext()->GetValue<uint32>("arena type")->Set(0);

            return true;
        }
    }

    if (statusid == STATUS_IN_PROGRESS)  // placeholder for Leave BG if it takes too long
    {
        LOG_INFO("playerbots", "Bot {} {}:{} <{}>: Received BG status IN_PROGRESS for {} {}",
                 bot->GetGUID().ToString().c_str(), bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(),
                 bot->GetName(), isArena ? "Arena" : "BG", _bgType);
        return false;
    }

    if (statusid == STATUS_WAIT_JOIN)  // bot may join
    {
        LOG_INFO("playerbots", "Bot {} {}:{} <{}>: Received BG status WAIT_JOIN for {} {}",
                 bot->GetGUID().ToString().c_str(), bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(),
                 bot->GetName(), isArena ? "Arena" : "BG", _bgType);

        if (isArena)
        {
            isArena = true;
            BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);

            GroupQueueInfo ginfo;
            if (!bgQueue.GetPlayerGroupInfoData(bot->GetGUID(), &ginfo))
            {
                LOG_ERROR("playerbots", "Bot {} {}:{} <{}>: Missing QueueInfo for {} {}",
                          bot->GetGUID().ToString().c_str(), bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H",
                          bot->GetLevel(), bot->GetName(), isArena ? "Arena" : "BG", _bgType);
                return false;
            }

            if (ginfo.IsInvitedToBGInstanceGUID)
            {
                // BattlegroundMgr::GetBattleground() does not return battleground if bgTypeId==BATTLEGROUND_AA
                Battleground* bg = sBattlegroundMgr->GetBattleground(
                    ginfo.IsInvitedToBGInstanceGUID, _bgTypeId == BATTLEGROUND_AA ? BATTLEGROUND_TYPE_NONE : _bgTypeId);
                if (!bg)
                {
                    LOG_ERROR("playerbots", "Bot {} {}:{} <{}>: Missing QueueInfo for {} {}",
                              bot->GetGUID().ToString().c_str(), bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H",
                              bot->GetLevel(), bot->GetName(), isArena ? "Arena" : "BG", _bgType);
                    return false;
                }

                _bgTypeId = bg->GetBgTypeID();
            }
        }

        // Wild random-bots: avoid re-joining recently evacuated empty battleground instances.
        // This prevents "leave -> immediate re-invite -> join" loops on bot-only BGs.
                LOG_INFO("playerbots", "Bot {} {}:{} <{}> joined {} - {}", bot->GetGUID().ToString().c_str(),
                 bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName(),
                 isArena ? "Arena" : "BG", _bgType);

        WorldPacket emptyPacket;
        WorldSession* session = GetBotSession();
        session->HandleCancelMountAuraOpcode(emptyPacket);

        action = 0x1;

        WorldPacket packet(CMSG_BATTLEFIELD_PORT, 20);
        packet << type << unk2 << (uint32)_bgTypeId << unk << action;
        session->QueuePacket(new WorldPacket(packet));

        botAI->ResetStrategies(false);
        if (!bot->GetBattleground())
        {
            // first bot to join wont have battleground and PlayerbotAI::ResetStrategies() wont set them up properly,
            // set bg for "bg strategy check" to fix that
            botAI->ChangeStrategy("+bg", BOT_STATE_NON_COMBAT);
        }
        context->GetValue<uint32>("bg role")->Set(urand(0, 9));
        PositionMap& posMap = context->GetValue<PositionMap&>("position")->Get();
        PositionInfo pos = context->GetValue<PositionMap&>("position")->Get()["bg objective"];
        PositionInfo pos2 = context->GetValue<PositionMap&>("position")->Get()["bg siege"];
        pos.Reset();
        pos2.Reset();
        posMap["bg objective"] = pos;
        posMap["bg siege"] = pos2;

        return true;
    }

    return true;
}

bool BGStatusCheckAction::Execute(Event event)
{
    WorldSession* session = GetBotSession();

    if (bot->IsBeingTeleported())
        return false;

    WorldPacket packet(CMSG_BATTLEFIELD_STATUS);
    session->HandleBattlefieldStatusOpcode(packet);

    LOG_INFO("playerbots", "Bot {} <{}> ({} {}) : Checking BG invite status", bot->GetGUID().ToString().c_str(),
             bot->GetName(), bot->GetLevel(), bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H");

    return true;
}

bool BGStatusCheckAction::isUseful() { return bot->InBattlegroundQueue(); }

bool BGStrategyCheckAction::Execute(Event event)
{

    // Note: InBattleground() can be true for a short moment while GetBattleground() is still null during transfer.
    // Never treat a temporary null Battleground* as "left the battleground", or bots may lose their BG tactics and idle.
    bool inBg = bot->InBattleground();

    Battleground* bg = bot->GetBattleground();
    bool inside_bg = inBg && bg;

    // If we left BG/arena, restore normal strategies exactly once.
    if (!inBg)
    {
        if (botAI->GetLastSeenBgInstanceId() || botAI->GetLastSwapBgInstanceId())
        {
            botAI->ResetBgStrategyState();
            botAI->ResetStrategies();
            return true;
        }

        return false;
    }

    // Still entering/loading: wait until Battleground* becomes available.
    if (!inside_bg)
        return false;

    uint32 instanceId = bg->GetInstanceID();
    if (!instanceId)
        return false;

    // Apply BG/arena tactics once per BG/arena instance.
    if (botAI->GetLastSeenBgInstanceId() != instanceId)
    {
        botAI->SetLastSeenBgInstanceId(instanceId);

        // If we are already in PvP gear (e.g. quick re-invite), do not rebuild it again.
        // Just mark the swap as done for this instance.
        if (botAI->IsPvpGearActive())
            botAI->SetLastSwapBgInstanceId(instanceId);
        else
            botAI->SetLastSwapBgInstanceId(0); // allow swap in this new instance

        botAI->ResetStrategies();
        // Do not return: we may swap gear in the same tick once the bot is fully on the BG map.
    }

    // Wild random bots only.
    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    // Addclass (summoned) bots must never auto-generate/swap PvP gear.
    if (sRandomPlayerbotMgr.IsAddclassBot(bot))
        return false;

    // Wait until the bot is actually on the BG/arena map (avoid swapping during transfer).
    if (!bot->IsInWorld() || bot->IsBeingTeleported() || bot->IsDuringRemoveFromWorld() || bot->GetMapId() != bg->GetMapId())
        return false;

    if (botAI->IsPvpGearActive() && botAI->GetLastSwapBgInstanceId() == instanceId)
        return false;
    // Random bots: generate PvP gear + enchants after fully entering BG/arena.
    bool hasRealMaster = botAI->HasRealPlayerMaster();

    // Do not touch player-controlled (real master) bots here. This logic is for wild random bots only.
    if (hasRealMaster)
        return false;

    uint32 qualityLimit = sPlayerbotAIConfig.randomGearQualityLimit;
    uint32 scoreLimit = sPlayerbotAIConfig.randomGearScoreLimit;

    uint32 gs = scoreLimit == 0 ? 0 : PlayerbotFactory::CalcMixedGearScore(scoreLimit, qualityLimit);

    bool isArena = bg->isArena();
    bool isRatedArena = isArena && bg->isRated();

    // Battleground gear cap for wild random bots (level 80 only).
    // Deterministic per-bot item-level cap in [200..300] based on bot GUID low.
    // This is an extra restriction on top of the configured gear limit.
    if (bot->GetLevel() == 80 && !isArena)
    {
        uint32 x = static_cast<uint32>(bot->GetGUID().GetCounter());
        x ^= x >> 16;
        x *= 0x7feb352d;
        x ^= x >> 15;
        x *= 0x846ca68b;
        x ^= x >> 16;

        uint32 ilvlCap = 200u + (x % 101u); // 200..300

        // Convert item-level cap to the same "mixed gear score" scale used by PlayerbotFactory.
        uint32 bgGsCap = PlayerbotFactory::CalcMixedGearScore(ilvlCap, ITEM_QUALITY_EPIC);
        if (bgGsCap == 0)
            bgGsCap = 1;

        if (gs == 0 || bgGsCap < gs)
            gs = bgGsCap;
    }


    // Additional arena rating-based gear cap (level 80 only).
    // 1000 rating => ilvl 200, 2400 rating => ilvl 300 (hard cap).
    // This is an extra restriction on top of the configured gear limit.
    if (bot->GetLevel() == 80 && isArena)
    {
        uint32 rating = 0;

        // Only rated arenas have meaningful team rating. Skirmish stays at rating=0 and will fall back to 1000.
        if (isRatedArena)
        {
            uint8 arenaType = bg->GetArenaType();  // 2,3,5
            uint8 slot = ArenaTeam::GetSlotByType(arenaType);
            uint32 teamId = bot->GetArenaTeamId(slot);
            if (teamId)
            {
                if (ArenaTeam* team = sArenaTeamMgr->GetArenaTeamById(teamId))
                    rating = team->GetRating();
            }
        }

        // Treat unrated/skirmish/unknown as low rating.
        if (rating == 0)
            rating = 1000;

        float ilvlCapF = 200.0f;
        if (rating >= 2400)
            ilvlCapF = 300.0f;
        else if (rating > 1000)
            ilvlCapF = 200.0f + float(rating - 1000) * (100.0f / 1400.0f);

        uint32 ilvlCap = uint32(ilvlCapF + 0.5f);

        // Convert item-level cap to the same "mixed gear score" scale used by PlayerbotFactory.
        uint32 ratingGsCap = PlayerbotFactory::CalcMixedGearScore(ilvlCap, ITEM_QUALITY_EPIC);
        if (ratingGsCap == 0)
            ratingGsCap = 1;

        if (gs == 0 || ratingGsCap < gs)
            gs = ratingGsCap;
    }

    // Bank-first PvE stash: save the currently equipped PvE set to bank so leaving BG/arena can restore it cheaply.
    botAI->StashPveGearToBankForNextPvpSwap();

    uint8 savedLevel = bot->GetLevel();
    PlayerbotFactory factory(bot, savedLevel, qualityLimit, gs, true);

    // Force gear generation; do not touch talents/level/spells/etc.
    factory.InitEquipment(false, true);

    // Apply enchants/gems only.
    if (savedLevel >= sPlayerbotAIConfig.minEnchantingBotLevel)
        factory.ApplyEnchantAndGemsNew();

    // Remember that this bot already swapped gear for this BG/arena instance.
    botAI->OnPvpGearEquipped(instanceId);

    return false;
}
