/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "BattleGroundJoinAction.h"

#include <unordered_set>
#include <algorithm>
#include <limits>
#include <map>

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

        Creature* selected = bestSameMap ? bestSameMap : (bestPreferredMap ? bestPreferredMap : fallback);
        if (!selected)
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

        if (selected)
            sTempArenaTeamMgr.SetBattlemasterGuidForLeader(bot, selected->GetGUID());

        return selected;
    }

    static bool EnsureArenaQueueGroupAtBattlemaster(Player* leader, Creature* battlemaster)
    {
        if (!leader || !battlemaster)
            return false;

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

        bool needsCapitalTeleport = leader->GetMapId() != battlemaster->GetMapId();

        for (Player* player : players)
        {
            if (!player || !player->IsInWorld() || player->IsBeingTeleported() || player->IsDuringRemoveFromWorld())
                return false;
        }

        if (needsCapitalTeleport)
        {
            WorldLocation capitalLoc;
            if (!sRandomPlayerbotMgr.GetArenaCapitalBankLocation(leader->GetLevel(), capitalLoc))
                return false;

            if (leader->GetMapId() != capitalLoc.GetMapId() ||
                leader->GetDistance2d(capitalLoc.GetPositionX(), capitalLoc.GetPositionY()) > 15.0f)
            {
                leader->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
                leader->TeleportTo(capitalLoc.GetMapId(), capitalLoc.GetPositionX(), capitalLoc.GetPositionY(),
                                   capitalLoc.GetPositionZ(), 0.0f);
            }

            sTempArenaTeamMgr.HoldQueuePositionForLeader(leader, capitalLoc.GetMapId(), capitalLoc.GetPositionX(),
                                                         capitalLoc.GetPositionY(), capitalLoc.GetPositionZ());
            return false;
        }

        sTempArenaTeamMgr.HoldQueuePositionForLeader(leader, leader->GetMapId(), leader->GetPositionX(),
                                                     leader->GetPositionY(), leader->GetPositionZ());
        return true;
    }

} // namespace

bool BGJoinAction::Execute(Event event)
{
    uint32 queueType = AI_VALUE(uint32, "bg type");
    if (!queueType)  // force join to fill bg
    {
        if (bgList.empty())
            return false;

        std::vector<uint32> const& queueChoices = ratedList.empty() ? bgList : ratedList;
        BattlegroundQueueTypeId queueTypeId =
            BattlegroundQueueTypeId(queueChoices[urand(0, queueChoices.size() - 1)]);
        BattlegroundTypeId bgTypeId = BattlegroundMgr::BGTemplateId(queueTypeId);
        BattlegroundBracketId bracketId;
        bool isArena = false;
        bool isRated = false;

        Battleground* bg = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
        if (!bg)
            return false;

        uint32 mapId = bg->GetMapId();
        PvPDifficultyEntry const* pvpDiff = GetBattlegroundBracketByLevel(mapId, bot->GetLevel());
        if (!pvpDiff)
            return false;

        bracketId = pvpDiff->GetBracketId();

        if (ArenaType type = ArenaType(BattlegroundMgr::BGArenaType(queueTypeId)))
        {
            isArena = true;

            std::vector<uint32>::iterator i = find(ratedList.begin(), ratedList.end(), queueTypeId);
            if (i != ratedList.end())
                isRated = true;

            if (isRated && !gatherArenaTeam(type))
                return false;

            botAI->GetAiObjectContext()->GetValue<uint32>("arena type")->Set(isRated);
        }

        // set bg type and bm guid
        // botAI->GetAiObjectContext()->GetValue<ObjectGuid>("bg master")->Set(bmGUID);
        botAI->GetAiObjectContext()->GetValue<uint32>("bg type")->Set(queueTypeId);
        queueType = queueTypeId;
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

        PlayerbotAI* currentBotAI = GET_PLAYERBOT_AI(bot);
        bool const isWildRandomBot = currentBotAI && !currentBotAI->HasRealPlayerMaster() &&
                                     !sRandomPlayerbotMgr.IsAddclassBot(bot) && bot->GetSession() &&
                                     bot->GetSession()->IsBot() &&
                                     sPlayerbotAIConfig.IsInRandomAccountList(bot->GetSession()->GetAccountId());

        if (isWildRandomBot && sTempArenaTeamMgr.ShouldAutoQueueLeader(bot, queueTypeId, bracketId, type))
        {
            sRandomPlayerbotMgr.BattlegroundData[queueTypeId][bracketId].ratedArenaBotCount += TeamSize;
            ratedList.push_back(queueTypeId);
            return true;
        }

        // Check if bots should join Rated Arena (Only captains can queue)
        uint32 ratedArenaBotCount = sRandomPlayerbotMgr.BattlegroundData[queueTypeId][bracketId].ratedArenaBotCount;
        uint32 ratedArenaPlayerCount =
            sRandomPlayerbotMgr.BattlegroundData[queueTypeId][bracketId].ratedArenaPlayerCount;
        uint32 ratedArenaInstanceCount =
            sRandomPlayerbotMgr.BattlegroundData[queueTypeId][bracketId].ratedArenaInstanceCount;
        uint32 activeRatedArenaQueue =
            sRandomPlayerbotMgr.BattlegroundData[queueTypeId][bracketId].activeRatedArenaQueue;

        bool isRated = (ratedArenaBotCount + ratedArenaPlayerCount) <
                       (BracketSize * (activeRatedArenaQueue + ratedArenaInstanceCount));

        if (isRated)
        {
            if (sArenaTeamMgr->GetArenaTeamByCaptain(bot->GetGUID(), type))
            {
                sRandomPlayerbotMgr.BattlegroundData[queueTypeId][bracketId].ratedArenaBotCount += TeamSize;
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
    ArenaTeam* tempArenaTeam = nullptr;

    std::string _bgType;

    // check if arena
    ArenaType arenaType = ArenaType(BattlegroundMgr::BGArenaType(queueTypeId));
    if (arenaType != ARENA_TYPE_NONE)
        isArena = true;

    // This breaks groups as refresh includes a remove from group function call.
    // refresh food/regs
    // sRandomPlayerbotMgr.Refresh(bot);

    bool joinAsGroup = bot->GetGroup() && bot->GetGroup()->GetLeaderGUID() == bot->GetGUID();

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
        isRated = botAI->GetAiObjectContext()->GetValue<uint32>("arena type")->Get();

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

    if (isArena && isRated)
    {
        tempArenaTeam = sTempArenaTeamMgr.GetArenaTeamForPlayer(bot, arenaslot);
        if (tempArenaTeam)
        {
            preparedArenaBattlemaster = FindArenaBattlemasterForBot(bot, bgTypeId);
            if (!preparedArenaBattlemaster)
            {
                sTempArenaTeamMgr.ResetGroupForLeader(bot);
                botAI->GetAiObjectContext()->GetValue<uint32>("bg type")->Set(0);
                return false;
            }

            if (!EnsureArenaQueueGroupAtBattlemaster(bot, preparedArenaBattlemaster))
            {
                sTempArenaTeamMgr.ResetGroupForLeader(bot);
                return false;
            }
        }
    }

    Unit* unit = nullptr;
    if (preparedArenaBattlemaster)
        unit = preparedArenaBattlemaster;
    else
        unit = botAI->GetUnit(sRandomPlayerbotMgr.GetBattleMasterGUID(bot, bgTypeId));

    if (!unit && isArena)
    {
        if (tempArenaTeam)
            sTempArenaTeamMgr.ResetGroupForLeader(bot);

        botAI->GetAiObjectContext()->GetValue<uint32>("bg type")->Set(0);
        LOG_DEBUG("playerbots", "Bot {} could not find Battlemaster to join", bot->GetGUID().ToString().c_str());
        return false;
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

        WorldPacket arena_packet(CMSG_BATTLEMASTER_JOIN_ARENA, 20);
        arena_packet << unit->GetGUID() << arenaslot << asGroup << uint8(isRated);
        session->HandleBattlemasterJoinArena(arena_packet);
    }

    return true;
}

// Not sure if this has ever worked, but it should be similar to BGJoinAction::shouldJoinBg
bool FreeBGJoinAction::shouldJoinBg(BattlegroundQueueTypeId queueTypeId, BattlegroundBracketId bracketId)
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

        PlayerbotAI* currentBotAI = GET_PLAYERBOT_AI(bot);
        bool const isWildRandomBot = currentBotAI && !currentBotAI->HasRealPlayerMaster() &&
                                     !sRandomPlayerbotMgr.IsAddclassBot(bot) && bot->GetSession() &&
                                     bot->GetSession()->IsBot() &&
                                     sPlayerbotAIConfig.IsInRandomAccountList(bot->GetSession()->GetAccountId());

        if (isWildRandomBot && sTempArenaTeamMgr.ShouldAutoQueueLeader(bot, queueTypeId, bracketId, type))
        {
            sRandomPlayerbotMgr.BattlegroundData[queueTypeId][bracketId].ratedArenaBotCount += TeamSize;
            ratedList.push_back(queueTypeId);
            return true;
        }

        // Check if bots should join Rated Arena (Only captains can queue)
        uint32 ratedArenaBotCount = sRandomPlayerbotMgr.BattlegroundData[queueTypeId][bracketId].ratedArenaBotCount;
        uint32 ratedArenaPlayerCount =
            sRandomPlayerbotMgr.BattlegroundData[queueTypeId][bracketId].ratedArenaPlayerCount;
        uint32 ratedArenaInstanceCount =
            sRandomPlayerbotMgr.BattlegroundData[queueTypeId][bracketId].ratedArenaInstanceCount;
        uint32 activeRatedArenaQueue =
            sRandomPlayerbotMgr.BattlegroundData[queueTypeId][bracketId].activeRatedArenaQueue;

        bool isRated = (ratedArenaBotCount + ratedArenaPlayerCount) <
                       (BracketSize * (activeRatedArenaQueue + ratedArenaInstanceCount));

        if (isRated)
        {
            if (sArenaTeamMgr->GetArenaTeamByCaptain(bot->GetGUID(), type))
            {
                sRandomPlayerbotMgr.BattlegroundData[queueTypeId][bracketId].ratedArenaBotCount += TeamSize;
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

    if (isArena && sTempArenaTeamMgr.HasContextForPlayer(bot))
        sTempArenaTeamMgr.ReleasePlayer(bot);

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

    bool isTempRatedArena = false;
    if (isArena && isRated)
        isTempRatedArena = sTempArenaTeamMgr.GetArenaTeamForPlayer(bot, ArenaTeam::GetSlotByType(arenaType)) != nullptr;

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

        if (Time2 > timer && isArena && !isTempRatedArena)  // disabled for BG
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
        if (isTempRatedArena)
            sTempArenaTeamMgr.OnQueueInvite(bot);

        LOG_INFO("playerbots", "Bot {} {}:{} <{}>: Received BG status IN_PROGRESS for {} {}",
                 bot->GetGUID().ToString().c_str(), bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(),
                 bot->GetName(), isArena ? "Arena" : "BG", _bgType);
        return false;
    }

    if (statusid == STATUS_WAIT_JOIN)  // bot may join
    {
        if (isTempRatedArena)
            sTempArenaTeamMgr.OnQueueInvite(bot);

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
