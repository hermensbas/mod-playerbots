/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgBaseAction.h"
#include "BroadcastHelper.h"
#include "ChatHelper.h"
#include "Creature.h"
#include "GameObject.h"
#include "GatherNodeMgr.h"
#include "GossipDef.h"
#include "GridTerrainData.h"
#include "IVMapMgr.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LootMgr.h"
#include "Map.h"
#include "ModelIgnoreFlags.h"
#include "MotionMaster.h"
#include "MoveSplineInitArgs.h"
#include "NewRpgHelpers.h"
#include "NewRpgInfo.h"
#include "QuestLevelRules.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "OutdoorPvPMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "Position.h"
#include "QuestDef.h"
#include "QuestPackets.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StatsWeightCalculator.h"
#include "Timer.h"
#include "TravelMgr.h"
#include "G3D/Vector2.h"
#include <algorithm>
#include <limits>
#include <sstream>

bool NewRpgBaseAction::MoveFarTo(WorldPosition dest)
{
    // Route the bot's A->B locomotion through the shared movement
    // orchestrator: it resolves the full route (navmesh for short
    // moves, travel-node graph for cross-map / long moves) and
    // dispatches the spline or special-movement leg.
    if (dest == WorldPosition())
        return false;
    EmitDebugMove("MoveFar", "moveto2", dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
    return MoveTo2(dest);
}

void NewRpgBaseAction::MarkUnreachable(ObjectGuid guid)
{
    lastUnreachableGO = guid;
    lastUnreachableGOMs = getMSTime();
}

bool NewRpgBaseAction::IsMarkedUnreachable(ObjectGuid guid) const
{
    return guid == lastUnreachableGO && GetMSTimeDiffToNow(lastUnreachableGOMs) < 60 * 1000;
}

bool NewRpgBaseAction::MoveWorldObjectTo(ObjectGuid guid, float distance)
{
    WorldObject* object = botAI->GetWorldObject(guid);
    if (!object)
        return false;

    Map* map = bot->GetMap();
    if (!map)
        return false;

    // 8-angle deterministic iteration around the target. For each angle,
    // validate the candidate against the navmesh and snap it to walkable
    // ground. Reject if no valid poly within 5y XY+Z or if the snap drifts
    // the Z by >10y. First angle that passes both LOS and navmesh-snap wins.
    //
    // Two passes: pass 0 prefers FLAT ground (exclude NAV_GROUND_STEEP);
    // pass 1 allows steep. An NPC standing on or beside a slope has its
    // whole 5.5y approach ring tagged steep, so a flat-only search rejects
    // every angle and the caller mills with MoveRandomNear even though the
    // NPC is a few yards away. Falling back to a steep approach lets the
    // bot actually reach it (MoveFarTo's own soft-steep path handles the
    // final walk). Flat is still tried first so bots don't climb needlessly.
    dtNavMeshQuery const* navMeshQuery =
        map->GetMapCollisionData().GetMMapData().GetNavMeshQuery();
    float const baseAngle = object->GetAngle(bot);

    // Place the approach ring INSIDE the requested range, not on its edge.
    // Landing the bot at exactly `distance` leaves it at the very limit of
    // interaction/loot range (any drift and it's out) and pushes the ring
    // onto ground farther from the object — more likely a slope edge or by
    // a wall. Pulling it ~2y inward keeps the bot comfortably in range and
    // on ground nearer where the object actually stands (so, walkable),
    // while staying clear of the object's own footprint.
    float const ringDist = std::max(distance - 2.0f, 2.5f);

    // LOS is a PREFERENCE, never a hard reject (reference parity: the
    // source only biases its angle choice by LOS and always produces a
    // destination). A giver on another floor occludes every eye-height
    // ray from below, and with deterministic angles a hard reject wedges
    // the approach forever — no destination, no MoveFarTo, no node
    // routing. Pass order: LOS+flat, LOS+steep, noLOS+flat, noLOS+steep.
    for (int pass = 0; pass < 4; ++pass)
    {
        bool const requireLos = pass < 2;
        bool const allowSteep = (pass % 2) == 1;
        uint16 const includeFlags = allowSteep ? (NAV_GROUND | NAV_GROUND_STEEP) : NAV_GROUND;
        uint16 const excludeFlags = allowSteep
            ? (NAV_WATER | NAV_MAGMA | NAV_SLIME)
            : (NAV_GROUND_STEEP | NAV_WATER | NAV_MAGMA | NAV_SLIME);

        for (float step = 0.0f; step < 2.0f * static_cast<float>(M_PI);
             step += static_cast<float>(M_PI) / 4.0f)
        {
            float const angle = baseAngle + step;
            float x = object->GetPositionX() + std::cos(angle) * ringDist;
            float y = object->GetPositionY() + std::sin(angle) * ringDist;
            float z = object->GetPositionZ();

            // LOS check at eye height (preference passes only).
            if (requireLos && !bot->IsWithinLOS(x, y, z + bot->GetCollisionHeight()))
                continue;

            // Navmesh-snap validation (cmangos ClosestCorrectPoint port).
            if (navMeshQuery)
            {
                dtQueryFilter filter;
                filter.setIncludeFlags(includeFlags);
                filter.setExcludeFlags(excludeFlags);

                float const point[VERTEX_SIZE] = { y, z, x };
                float const extents[VERTEX_SIZE] = { 5.0f, 5.0f, 5.0f };
                float closest[VERTEX_SIZE] = { 0.0f, 0.0f, 0.0f };
                dtPolyRef polyRef = INVALID_POLYREF;

                if (!dtStatusSucceed(navMeshQuery->findNearestPoly(
                        point, extents, &filter, &polyRef, closest)) ||
                    polyRef == INVALID_POLYREF)
                    continue;

                float const snappedZ = closest[1];
                if (std::fabs(snappedZ - z) > 10.0f)
                    continue;

                x = closest[2];
                y = closest[0];
                z = snappedZ;
            }

            return MoveFarTo(WorldPosition(object->GetMapId(), x, y, z));
        }
    }

    return false;
}

bool NewRpgBaseAction::MoveRandomNear(float moveStep, MovementPriority priority, WorldObject*)
{
    if (IsWaitingForLastMove(priority))
        return false;

    Map* map = bot->GetMap();
    const float x = bot->GetPositionX();
    const float y = bot->GetPositionY();
    const float z = bot->GetPositionZ();
    // Retry random samples so one bad roll doesn't lock the bot in place.
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        float distance = (0.4f + rand_norm() * 0.6f) * moveStep;
        float angle = (float)rand_norm() * 2 * static_cast<float>(M_PI);
        float dx = x + distance * cos(angle);
        float dy = y + distance * sin(angle);
        float dz = z;

        PathResult path = GeneratePath(dx, dy, dz, RELAXED_PATH_ACCEPT_MASK, /*forceDestination=*/false);

        if (!path.reachable)
            continue;

        if (!map->CanReachPositionAndGetValidCoords(bot, dx, dy, dz))
            continue;

        if (map->IsInWater(bot->GetPhaseMask(), dx, dy, dz, bot->GetCollisionHeight()))
            continue;

        // Reject samples whose straight-line passes through visual
        // obstacles (trees, models) that aren't in the navmesh. The
        // smooth-path step can otherwise interpolate a waypoint inside
        // a tree, making the bot visibly walk through it.
        if (!bot->IsWithinLOS(dx, dy, dz))
            continue;

        // Hand the validated nearby destination to the shared
        // orchestrator, which resolves the short move via navmesh.
        bool moved = MoveTo2(bot->GetMapId(), dx, dy, dz);
        if (moved)
        {
            EmitDebugMove("MoveRandomNear", "mmap", dx, dy, dz);
            return true;
        }
    }

    EmitDebugMove("MoveRandomNear", "all-fail", x, y, z);
    return false;
}

bool NewRpgBaseAction::ForceToWait(uint32 duration, MovementPriority priority)
{
    AI_VALUE(LastMovement&, "last movement")
        .Set(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetOrientation(),
             duration, priority);
    return true;
}

bool NewRpgBaseAction::TakeFlight(std::vector<uint32> const& taxiNodes, Creature* flightMaster)
{
    if (taxiNodes.size() < 2 || !flightMaster || !flightMaster->IsAlive())
        return false;

    botAI->RemoveShapeshift();
    if (bot->IsMounted())
        bot->Dismount();

    bot->GetSession()->SendLearnNewTaxiNode(flightMaster);

    if (!bot->ActivateTaxiPathTo(taxiNodes, flightMaster, 0))
    {
        LOG_DEBUG("playerbots", "[New RPG] Bot {} flight ({} nodes, {} to {}) failed",
                  bot->GetName(), taxiNodes.size(), taxiNodes.front(), taxiNodes.back());
        return false;
    }

    LOG_DEBUG("playerbots", "[New RPG] Bot {} taking flight ({} nodes, {} to {})",
              bot->GetName(), taxiNodes.size(), taxiNodes.front(), taxiNodes.back());
    EmitDebugMove("TravelPlan:flight", "taxi", flightMaster->GetPositionX(), flightMaster->GetPositionY(),
                  flightMaster->GetPositionZ());
    return true;
}

/// @TODO: Fix redundant code
/// Quest related method refer to TalkToQuestGiverAction.h
bool NewRpgBaseAction::InteractWithNpcOrGameObjectForQuest(ObjectGuid guid)
{
    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, guid);
    if (!object || !bot->CanInteractWithQuestGiver(object))
        return false;

    // Creature* creature = bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_NONE);
    // if (creature)
    // {
    //     WorldPacket packet(CMSG_GOSSIP_HELLO);
    //     packet << guid;
    //     bot->GetSession()->HandleGossipHelloOpcode(packet);
    // }

    bot->PrepareQuestMenu(guid);
    QuestMenu const& menu = bot->PlayerTalkClass->GetQuestMenu();
    if (menu.Empty())
        return true;

    for (uint8 idx = 0; idx < menu.GetMenuItemCount(); idx++)
    {
        QuestMenuItem const& item = menu.GetItem(idx);
        Quest const* quest = sObjectMgr->GetQuestTemplate(item.QuestId);
        if (!quest)
            continue;

        QuestStatus const& status = bot->GetQuestStatus(item.QuestId);
        if (status == QUEST_STATUS_NONE && bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false) &&
            NewRpgHelpers::IsQuestWorthDoing(bot, quest) && NewRpgHelpers::IsQuestCapableDoing(bot, quest))
        {
            AcceptQuest(quest, guid);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_accepted",
                    "Quest accepted %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            BroadcastHelper::BroadcastQuestAccepted(botAI, bot, quest);
            botAI->RecordQuestAccepted(quest->GetQuestId());
            LOG_DEBUG("playerbots", "[New RPG] {} accept quest {}", bot->GetName(), quest->GetQuestId());
        }
        if (status == QUEST_STATUS_COMPLETE && bot->CanRewardQuest(quest, 0, false))
        {
            TurnInQuest(quest, guid);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_rewarded",
                    "Quest rewarded %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            BroadcastHelper::BroadcastQuestTurnedIn(botAI, bot, quest);
            botAI->RecordQuestRewarded(quest->GetQuestId());
            LOG_DEBUG("playerbots", "[New RPG] {} turned in quest {}", bot->GetName(), quest->GetQuestId());
        }
    }
    return true;
}

bool NewRpgBaseAction::IsWithinInteractionDist(Object* questGiver)
{
    // This is a variant of Player::CanInteractWithQuestGiver
    // that only keep the distance check
    switch (questGiver->GetTypeId())
    {
        case TYPEID_UNIT:
        {
            ObjectGuid guid = questGiver->GetGUID();
            // unit checks
            if (!guid)
                return false;

            // exist (we need look pets also for some interaction (quest/etc)
            Creature* creature = ObjectAccessor::GetCreatureOrPetOrVehicle(*bot, guid);
            if (!creature)
                return false;

            if (!creature->IsWithinDistInMap(bot, INTERACTION_DISTANCE))
                return false;

            return true;
        }
        case TYPEID_GAMEOBJECT:
        {
            ObjectGuid guid = questGiver->GetGUID();
            if (GameObject* go = bot->GetMap()->GetGameObject(guid))
            {
                if (go->IsWithinDistInMap(bot))
                {
                    return true;
                }
            }
            return false;
        }
        // case TYPEID_PLAYER:
        //     return bot->IsAlive() && questGiver->ToPlayer()->IsAlive();
        // case TYPEID_ITEM:
        //     return bot->IsAlive();
        default:
            break;
    }
    return false;
}

bool NewRpgBaseAction::AcceptQuest(Quest const* quest, ObjectGuid guid)
{
    WorldPacket p(CMSG_QUESTGIVER_ACCEPT_QUEST);
    uint32 unk1 = 0;
    p << guid << quest->GetQuestId() << unk1;
    p.rpos(0);
    bot->GetSession()->HandleQuestgiverAcceptQuestOpcode(p);

    return true;
}

bool NewRpgBaseAction::TurnInQuest(Quest const* quest, ObjectGuid guid)
{
    uint32 questID = quest->GetQuestId();

    if (bot->GetQuestRewardStatus(questID))
    {
        return false;
    }

    if (!bot->CanRewardQuest(quest, false))
    {
        return false;
    }

    bot->PlayDistanceSound(621);

    WorldPacket p(CMSG_QUESTGIVER_CHOOSE_REWARD);
    p << guid << quest->GetQuestId();
    if (quest->GetRewChoiceItemsCount() <= 1)
    {
        p << 0;
        bot->GetSession()->HandleQuestgiverChooseRewardOpcode(p);
    }
    else
    {
        uint32 bestId = BestRewardIndex(quest);
        p << bestId;
        bot->GetSession()->HandleQuestgiverChooseRewardOpcode(p);
    }

    return true;
}

uint32 NewRpgBaseAction::BestRewardIndex(Quest const* quest)
{
    ItemIds returnIds;
    ItemUsage bestUsage = ITEM_USAGE_NONE;
    if (quest->GetRewChoiceItemsCount() <= 1)
        return 0;
    else
    {
        for (uint8 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
        {
            ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", quest->RewardChoiceItemId[i]);
            if (usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE)
                bestUsage = ITEM_USAGE_EQUIP;
            else if (usage == ITEM_USAGE_BAD_EQUIP && bestUsage != ITEM_USAGE_EQUIP)
                bestUsage = usage;
            else if (usage != ITEM_USAGE_NONE && bestUsage == ITEM_USAGE_NONE)
                bestUsage = usage;
        }
        StatsWeightCalculator calc(bot);
        uint32 best = 0;
        float bestScore = 0;
        for (uint8 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
        {
            ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", quest->RewardChoiceItemId[i]);
            if (usage == bestUsage || usage == ITEM_USAGE_REPLACE)
            {
                uint32 const itemId = quest->RewardChoiceItemId[i];
                ItemTemplate const* rewardProto = sObjectMgr->GetItemTemplate(itemId);
                uint8 rewardSlot = NULL_SLOT;
                if (rewardProto)
                    rewardSlot = botAI->FindEquipSlot(rewardProto, NULL_SLOT, true);
                float score = calc.CalculateItem(itemId, 0, rewardSlot);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = i;
                }
            }
        }
        return best;
    }
}

bool NewRpgBaseAction::OrganizeQuestLog()
{
    int32 freeSlotNum = 0;

    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            freeSlotNum++;
    }

    // it's ok if we have two more free slots
    if (freeSlotNum >= 2)
        return false;

    int32 dropped = 0;
    // remove quests that not worth doing or not capable of doing
    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        bool const worthDoing = NewRpgHelpers::IsQuestWorthDoing(bot, quest);
        bool const capableDoing = NewRpgHelpers::IsQuestCapableDoing(bot, quest);
        bool const failed = bot->GetQuestStatus(questId) == QUEST_STATUS_FAILED;
        if (!worthDoing || !capableDoing || failed)
        {
            QuestDropReason const reason = !worthDoing    ? QuestDropReason::NotWorth
                                           : !capableDoing ? QuestDropReason::NotCapable
                                                           : QuestDropReason::Failed;
            LOG_DEBUG("playerbots", "[New RPG] {} drop quest {}", bot->GetName(), questId);
            WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
            packet << (uint8)i;
            WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
            removeQuest.Read();
            bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_dropped",
                    "Quest dropped %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            botAI->RecordQuestDropped(questId, reason);
            dropped++;
        }
    }

    // drop more than 8 quests at once to avoid repeated accept and drop
    if (dropped >= 8)
        return true;

    // remove festival/class quests and quests in different zone
    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        const int64_t botZoneId = this->bot->GetZoneId();

        int32 const zoneOrSort = quest->GetZoneOrSort();
        if (zoneOrSort < 0 || (zoneOrSort > 0 && zoneOrSort != botZoneId))
        {
            QuestDropReason const reason = zoneOrSort < 0 ? QuestDropReason::ClassOrHoliday
                                                          : QuestDropReason::OtherZone;
            LOG_DEBUG("playerbots", "[New RPG] {} drop quest {}", bot->GetName(), questId);
            WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
            packet << (uint8)i;
            WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
            removeQuest.Read();
            bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_dropped",
                    "Quest dropped %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            botAI->RecordQuestDropped(questId, reason);
            dropped++;
        }
    }

    if (dropped >= 8)
        return true;

    // clear quests log
    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        LOG_DEBUG("playerbots", "[New RPG] {} drop quest {}", bot->GetName(), questId);
        WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
        packet << (uint8)i;
        WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
        removeQuest.Read();
        bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
        if (botAI->GetMaster())
            botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "new_rpg_quest_dropped",
                "Quest dropped %quest",
                {{"%quest", ChatHelper::FormatQuest(quest)}}));
        botAI->RecordQuestDropped(questId, QuestDropReason::LogFull);
    }

    return true;
}

bool NewRpgBaseAction::SearchQuestGiverAndAcceptOrReward()
{
    OrganizeQuestLog();
    ObjectGuid npcOrGo = AI_VALUE(ObjectGuid, "new rpg quest giver");
    if (!npcOrGo)
        return false;

    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, npcOrGo);
    if (!object)
        return false;

    NewRpgInfo& info = botAI->rpgInfo;
    if (info.questGiver != npcOrGo || GetMSTimeDiffToNow(info.questGiverLastTry) > questGiverRetryGap)
    {
        info.questGiver = npcOrGo;
        info.questGiverSince = getMSTime();
    }
    info.questGiverLastTry = getMSTime();

    // Unreachable, or an offer the interaction never clears: let the status action and grinding resume.
    if (GetMSTimeDiffToNow(info.questGiverSince) > questGiverGiveUpTime)
    {
        LOG_DEBUG("playerbots", "[New RPG] {} gives up on quest giver {}", bot->GetName(), npcOrGo.ToString());
        info.ignoredQuestGivers.insert(npcOrGo);
        info.questGiver.Clear();
        return false;
    }

    if (bot->CanInteractWithQuestGiver(object))
    {
        InteractWithNpcOrGameObjectForQuest(npcOrGo);
        ForceToWait(5000);
        return true;
    }
    return MoveWorldObjectTo(npcOrGo);
}

static bool BotNeedsItemForQuest(Player* bot, uint32 itemId)
{
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;
        if (bot->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
            continue;
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;
        QuestStatusData const& qs = bot->getQuestStatusMap().at(questId);
        for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        {
            if (!quest->RequiredItemCount[i])
                continue;
            if (qs.ItemCount[i] >= quest->RequiredItemCount[i])
                continue;
            if (quest->RequiredItemId[i] == itemId)
                return true;
        }
    }
    return false;
}

bool NewRpgBaseAction::TryLootQuestGO(ObjectGuid& pursuedGO, float searchRange)
{
    if (!bot->IsAlive() || bot->IsBeingTeleported() || bot->IsInFlight() ||
        bot->GetVehicle() || bot->GetTransport())
        return false;

    // valid = spawned, selectable, holds a quest item we still need.
    // INTERACT_COND is fine — ConditionMgr already gates on quest state.
    auto isValidTarget = [&](GameObject* go) -> bool
    {
        if (!go || !go->IsInWorld() || !go->isSpawned())
            return false;
        if (!(go->GetPhaseMask() & bot->GetPhaseMask()))
            return false;
        if (go->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE))
            return false;
        GameObjectTemplate const* info = go->GetGOInfo();
        if (!info)
            return false;

        // per-player quest drops via gameobject_questitem (Webwood Eggs…)
        if (GameObjectQuestItemList const* items =
                sObjectMgr->GetGameObjectQuestItemList(go->GetEntry()))
        {
            for (size_t i = 0; i < MAX_GAMEOBJECT_QUEST_ITEMS && i < items->size(); ++i)
            {
                uint32 itemId = uint32((*items)[i]);
                if (!itemId)
                    continue;
                if (BotNeedsItemForQuest(bot, itemId))
                    return true;
            }
        }

        // standard loot template (chests, fishing holes)
        if (uint32 lootId = info->GetLootId())
        {
            if (LootTemplates_Gameobject.HaveQuestLootForPlayer(lootId, bot))
                return true;
        }
        return false;
    };

    // 2.5y sits inside the 3.5y loot gate with headroom
    const float lootRange = 2.5f;

    // stick with the committed target — re-picking nearest every tick
    // causes zig-zag walks in dense spawn clusters
    if (pursuedGO)
    {
        GameObject* existing = botAI->GetGameObject(pursuedGO);
        if (existing && isValidTarget(existing) &&
            bot->GetDistance(existing) <= searchRange)
        {
            if (bot->GetDistance(existing) > lootRange)
            {
                if (MoveWorldObjectTo(existing->GetGUID(), lootRange))
                    return true;
                // Unreachable: un-commit and remember, so the picker
                // below tries a DIFFERENT spawn next tick instead of
                // re-committing this one forever.
                MarkUnreachable(pursuedGO);
                pursuedGO.Clear();
                return false;
            }
            // in range — loot strategy opens it
            return true;
        }
        pursuedGO.Clear();
    }

    GuidVector possibleGameObjects = AI_VALUE(GuidVector, "possible new rpg game objects");
    if (possibleGameObjects.empty())
        return false;

    GameObject* best = nullptr;
    float bestDist = searchRange;
    for (ObjectGuid guid : possibleGameObjects)
    {
        if (IsMarkedUnreachable(guid))
            continue;
        GameObject* go = botAI->GetGameObject(guid);
        if (!isValidTarget(go))
            continue;
        float d = bot->GetDistance(go);
        if (d >= bestDist)
            continue;
        best = go;
        bestDist = d;
    }
    if (!best)
        return false;

    // commit
    pursuedGO = best->GetGUID();

    if (bot->GetDistance(best) > lootRange)
    {
        if (MoveWorldObjectTo(best->GetGUID(), lootRange))
            return true;
        MarkUnreachable(pursuedGO);
        pursuedGO.Clear();
        return false;
    }

    // in range — consume the tick so we don't fall through to wander
    return true;
}

bool NewRpgBaseAction::TryUseQuestGO(ObjectGuid& pursuedGO, float searchRange)
{
    if (!bot->IsAlive() || bot->IsBeingTeleported() || bot->IsInFlight() ||
        bot->GetVehicle() || bot->GetTransport())
        return false;

    std::unordered_set<uint32> neededGoEntries;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;
        if (bot->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
            continue;
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;
        QuestStatusData const& qs = bot->getQuestStatusMap().at(questId);
        for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        {
            int32 entry = quest->RequiredNpcOrGo[i];
            if (entry >= 0)
                continue;
            if (qs.CreatureOrGOCount[i] >= quest->RequiredNpcOrGoCount[i])
                continue;
            neededGoEntries.insert(uint32(-entry));
        }
    }
    if (neededGoEntries.empty())
        return false;

    auto isValidTarget = [&](GameObject* go) -> bool
    {
        if (!go || !go->IsInWorld() || !go->isSpawned())
            return false;
        if (!(go->GetPhaseMask() & bot->GetPhaseMask()))
            return false;
        if (go->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE))
            return false;
        return neededGoEntries.count(go->GetEntry()) > 0;
    };

    // commitment first
    if (pursuedGO)
    {
        GameObject* existing = botAI->GetGameObject(pursuedGO);
        if (existing && isValidTarget(existing) &&
            bot->GetDistance(existing) <= searchRange)
        {
            if (bot->GetDistance(existing) > INTERACTION_DISTANCE)
                return MoveWorldObjectTo(existing->GetGUID(), INTERACTION_DISTANCE);
            existing->Use(bot);
            ForceToWait(2000);
            pursuedGO.Clear();
            return true;
        }
        pursuedGO.Clear();
    }

    GuidVector possibleGameObjects = AI_VALUE(GuidVector, "possible new rpg game objects");
    GameObject* best = nullptr;
    float bestDist = searchRange;
    for (ObjectGuid guid : possibleGameObjects)
    {
        GameObject* go = botAI->GetGameObject(guid);
        if (!isValidTarget(go))
            continue;
        float d = bot->GetDistance(go);
        if (d >= bestDist)
            continue;
        best = go;
        bestDist = d;
    }
    if (!best)
        return false;

    pursuedGO = best->GetGUID();

    if (bot->GetDistance(best) > INTERACTION_DISTANCE)
        return MoveWorldObjectTo(best->GetGUID(), INTERACTION_DISTANCE);

    best->Use(bot);
    ForceToWait(2000);
    pursuedGO.Clear();
    return true;
}

bool NewRpgBaseAction::TryUseQuestItem(ObjectGuid& pursuedGO, ObjectGuid& pursuedTarget, float searchRange)
{
    if (!bot->IsAlive() || bot->IsBeingTeleported() || bot->IsInFlight() ||
        bot->GetVehicle() || bot->GetTransport())
        return false;

    std::unordered_set<uint32> candidateItemEntries;
    // src items (the quest gave the bot a single item to use); branch C
    // (self/area cast) is only safe to fire on these — auto-firing every
    // ItemDrop on self can burn kill-credit sentinels and trigger
    // unintended scripted side effects.
    std::unordered_set<uint32> srcItemEntries;
    std::unordered_set<uint32> neededCreatureEntries;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;
        if (bot->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
            continue;
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;
        if (uint32 src = quest->GetSrcItemId())
        {
            candidateItemEntries.insert(src);
            srcItemEntries.insert(src);
        }
        // handed out by the quest (brands, flares, nets, standards)
        for (int i = 0; i < QUEST_SOURCE_ITEM_IDS_COUNT; ++i)
        {
            if (uint32 drop = quest->ItemDrop[i])
                candidateItemEntries.insert(drop);
        }
        QuestStatusData const& qs = bot->getQuestStatusMap().at(questId);
        for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        {
            int32 entry = quest->RequiredNpcOrGo[i];
            if (entry <= 0)
                continue;
            if (qs.CreatureOrGOCount[i] >= quest->RequiredNpcOrGoCount[i])
                continue;
            neededCreatureEntries.insert(uint32(entry));
        }
    }
    if (candidateItemEntries.empty())
        return false;

    for (uint32 itemEntry : candidateItemEntries)
    {
        Item* item = bot->GetItemByEntry(itemEntry);
        if (!item)
            continue;
        ItemTemplate const* proto = item->GetTemplate();
        if (!proto)
            continue;
        uint32 useSpellId = 0;
        for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        {
            if (proto->Spells[i].SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                continue;
            if (proto->Spells[i].SpellId <= 0)
                continue;
            useSpellId = proto->Spells[i].SpellId;
            break;
        }
        if (!useSpellId)
            continue;
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(useSpellId);
        if (!spellInfo)
            continue;

        // A: spell needs a focus GO (moonwell / lectern / anvil)
        if (uint32 focusId = spellInfo->RequiresSpellFocus)
        {
            auto focusRadius = [](GameObject* go) -> float
            {
                GameObjectTemplate const* info = go->GetGOInfo();
                // half radius so we end up inside, not on the rim
                return std::max<float>(1.0f, float(info->spellFocus.dist) * 0.5f);
            };
            auto isValidFocus = [&](GameObject* go) -> bool
            {
                if (!go || !go->IsInWorld() || !go->isSpawned())
                    return false;
                if (!(go->GetPhaseMask() & bot->GetPhaseMask()))
                    return false;
                if (go->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE))
                    return false;
                GameObjectTemplate const* info = go->GetGOInfo();
                if (!info || info->type != GAMEOBJECT_TYPE_SPELL_FOCUS)
                    return false;
                return info->spellFocus.focusId == focusId;
            };

            // commitment first
            if (pursuedGO)
            {
                GameObject* existing = botAI->GetGameObject(pursuedGO);
                if (existing && isValidFocus(existing) &&
                    bot->GetDistance(existing) <= searchRange)
                {
                    float radius = focusRadius(existing);
                    if (bot->GetDistance(existing) > radius)
                        return MoveWorldObjectTo(existing->GetGUID(), radius);
                    SpellCastTargets targets;
                    bot->CastItemUseSpell(item, targets, 1, 0);
                    ForceToWait(2000);
                    pursuedGO.Clear();
                    return true;
                }
                pursuedGO.Clear();
            }

            GuidVector possibleGameObjects = AI_VALUE(GuidVector, "possible new rpg game objects");
            GameObject* best = nullptr;
            float bestDist = searchRange;
            float bestRadius = INTERACTION_DISTANCE;
            for (ObjectGuid guid : possibleGameObjects)
            {
                GameObject* go = botAI->GetGameObject(guid);
                if (!isValidFocus(go))
                    continue;
                float d = bot->GetDistance(go);
                if (d >= bestDist)
                    continue;
                best = go;
                bestDist = d;
                bestRadius = focusRadius(go);
            }
            if (best)
            {
                pursuedGO = best->GetGUID();
                if (bot->GetDistance(best) > bestRadius)
                    return MoveWorldObjectTo(best->GetGUID(), bestRadius);
                SpellCastTargets targets;
                bot->CastItemUseSpell(item, targets, 1, 0);
                ForceToWait(2000);
                pursuedGO.Clear();
                return true;
            }
            continue;
        }

        // B: spell needs a unit target — walk to the required creature
        if (spellInfo->NeedsExplicitUnitTarget() && !neededCreatureEntries.empty())
        {
            auto isValidCreature = [&](Creature* c) -> bool
            {
                if (!c || !c->IsInWorld() || !c->IsAlive())
                    return false;
                if (!(c->GetPhaseMask() & bot->GetPhaseMask()))
                    return false;
                return neededCreatureEntries.count(c->GetEntry()) > 0;
            };

            // commitment first
            if (pursuedTarget)
            {
                Creature* existing = botAI->GetCreature(pursuedTarget);
                if (existing && isValidCreature(existing) &&
                    bot->GetDistance(existing) <= searchRange)
                {
                    if (bot->GetDistance(existing) > INTERACTION_DISTANCE)
                        return MoveWorldObjectTo(existing->GetGUID(), INTERACTION_DISTANCE);
                    SpellCastTargets targets;
                    targets.SetUnitTarget(existing);
                    bot->CastItemUseSpell(item, targets, 1, 0);
                    ForceToWait(2000);
                    pursuedTarget.Clear();
                    return true;
                }
                pursuedTarget.Clear();
            }

            GuidVector possibleTargets = AI_VALUE(GuidVector, "possible new rpg targets");
            Creature* best = nullptr;
            float bestDist = searchRange;
            for (ObjectGuid guid : possibleTargets)
            {
                Creature* c = botAI->GetCreature(guid);
                if (!isValidCreature(c))
                    continue;
                float d = bot->GetDistance(c);
                if (d >= bestDist)
                    continue;
                best = c;
                bestDist = d;
            }
            if (best)
            {
                pursuedTarget = best->GetGUID();
                if (bot->GetDistance(best) > INTERACTION_DISTANCE)
                    return MoveWorldObjectTo(best->GetGUID(), INTERACTION_DISTANCE);
                SpellCastTargets targets;
                targets.SetUnitTarget(best);
                bot->CastItemUseSpell(item, targets, 1, 0);
                ForceToWait(2000);
                pursuedTarget.Clear();
                return true;
            }
            continue;
        }

        // C: self / area — fire at bot's position. Restrict to GetSrcItemId
        // items (the single item the quest hands the bot for self-use, e.g.
        // a potion). ItemDrop entries can be kill-credit sentinels or
        // scripted items that should never be auto-used on self.
        if (!srcItemEntries.count(itemEntry))
            continue;
        SpellCastTargets targets;
        if (spellInfo->IsTargetingArea())
            targets.SetDst(*bot);
        else
            targets.SetUnitTarget(bot);
        bot->CastItemUseSpell(item, targets, 1, 0);
        ForceToWait(2000);
        return true;
    }

    return false;
}

bool NewRpgBaseAction::HasNearbyQuestMob(float range)
{
    return NearestQuestMob(range) != nullptr;
}

Creature* NewRpgBaseAction::NearestQuestMob(float range)
{
    // kill objectives + mobs that drop required quest items
    std::unordered_set<uint32> neededCreatureEntries;
    std::unordered_set<uint32> neededItemIds;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;
        if (bot->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
            continue;
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;
        QuestStatusData const& qs = bot->getQuestStatusMap().at(questId);
        for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        {
            int32 entry = quest->RequiredNpcOrGo[i];
            if (entry <= 0)
                continue;
            if (qs.CreatureOrGOCount[i] >= quest->RequiredNpcOrGoCount[i])
                continue;
            neededCreatureEntries.insert(uint32(entry));
        }
        for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        {
            if (!quest->RequiredItemCount[i])
                continue;
            if (qs.ItemCount[i] >= quest->RequiredItemCount[i])
                continue;
            if (quest->RequiredItemId[i])
                neededItemIds.insert(quest->RequiredItemId[i]);
        }
    }
    if (neededCreatureEntries.empty() && neededItemIds.empty())
        return nullptr;

    GuidVector possibleTargets = AI_VALUE(GuidVector, "possible targets");
    Creature* nearest = nullptr;
    float nearestDist = 0.0f;
    for (ObjectGuid guid : possibleTargets)
    {
        Creature* c = botAI->GetCreature(guid);
        if (!c || !c->IsInWorld() || !c->IsAlive())
            continue;
        if (!(c->GetPhaseMask() & bot->GetPhaseMask()))
            continue;
        float const dist = bot->GetDistance(c);
        if (dist > range)
            continue;

        bool relevant = false;

        // direct kill objective
        if (neededCreatureEntries.count(c->GetEntry()))
            relevant = true;

        // drops a required quest item — HaveQuestLootForPlayer
        // already filters by what this player still needs
        if (!relevant && !neededItemIds.empty())
        {
            CreatureTemplate const* tmpl = c->GetCreatureTemplate();
            if (tmpl && tmpl->lootid &&
                LootTemplates_Creature.HaveQuestLootForPlayer(tmpl->lootid, bot))
            {
                relevant = true;
            }
        }

        if (!relevant)
            continue;

        if (!nearest || dist < nearestDist)
        {
            nearestDist = dist;
            nearest = c;
        }
    }
    return nearest;
}

static std::vector<float> GenerateRandomWeights(int n)
{
    std::vector<float> weights(n);
    float sum = 0.0;

    for (int i = 0; i < n; ++i)
    {
        weights[i] = rand_norm();
        sum += weights[i];
    }
    for (int i = 0; i < n; ++i)
    {
        weights[i] /= sum;
    }
    return weights;
}

bool NewRpgBaseAction::GetQuestPOIPosAndObjectiveIdx(uint32 questId, std::vector<POIInfo>& poiInfo, bool toComplete)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
        return false;

    QuestPOIVector const* poiVector = sObjectMgr->GetQuestPOIVector(questId);
    if (!poiVector)
    {
        return false;
    }

    QuestStatusData const& q_status = bot->getQuestStatusMap().at(questId);

    if (toComplete && q_status.Status == QUEST_STATUS_COMPLETE)
    {
        for (QuestPOI const& qPoi : *poiVector)
        {
            if (qPoi.MapId != bot->GetMapId())
                continue;

            // not the poi pos to reward quest
            if (qPoi.ObjectiveIndex != -1)
                continue;

            if (qPoi.points.size() == 0)
                continue;

            float dx = 0, dy = 0;
            std::vector<float> weights = GenerateRandomWeights(qPoi.points.size());
            for (size_t i = 0; i < qPoi.points.size(); i++)
            {
                QuestPOIPoint const& point = qPoi.points[i];
                dx += point.x * weights[i];
                dy += point.y * weights[i];
            }

            if (bot->GetDistance2d(dx, dy) >= 1500.0f)
                continue;

            float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

            if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
                continue;

            if (bot->GetZoneId() != bot->GetMap()->GetZoneId(bot->GetPhaseMask(), dx, dy, dz))
                continue;

            poiInfo.push_back({{dx, dy}, qPoi.ObjectiveIndex});
        }

        if (poiInfo.empty())
            return false;

        return true;
    }

    if (q_status.Status != QUEST_STATUS_INCOMPLETE)
        return false;

    // Get incomplete quest objective index
    std::vector<int32> incompleteObjectiveIdx;
    for (int i = 0; i < QUEST_OBJECTIVES_COUNT; i++)
    {
        int32 npcOrGo = quest->RequiredNpcOrGo[i];
        if (!npcOrGo)
            continue;

        if (q_status.CreatureOrGOCount[i] < quest->RequiredNpcOrGoCount[i])
            incompleteObjectiveIdx.push_back(i);
    }
    for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; i++)
    {
        uint32 itemId = quest->RequiredItemId[i];
        if (!itemId)
            continue;

        if (q_status.ItemCount[i] < quest->RequiredItemCount[i])
            incompleteObjectiveIdx.push_back(QUEST_OBJECTIVES_COUNT + i);
    }

    // Get POIs to go
    for (QuestPOI const& qPoi : *poiVector)
    {
        if (qPoi.MapId != bot->GetMapId())
            continue;

        bool inComplete = false;
        for (uint32 objective : incompleteObjectiveIdx)
        {
            if (qPoi.ObjectiveIndex == static_cast<int32>(objective))
            {
                inComplete = true;
                break;
            }
        }
        if (!inComplete)
            continue;
        if (qPoi.points.size() == 0)
            continue;
        float dx = 0, dy = 0;
        std::vector<float> weights = GenerateRandomWeights(qPoi.points.size());
        for (size_t i = 0; i < qPoi.points.size(); i++)
        {
            QuestPOIPoint const& point = qPoi.points[i];
            dx += point.x * weights[i];
            dy += point.y * weights[i];
        }

        if (bot->GetDistance2d(dx, dy) >= 1500.0f)
            continue;

        float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

        if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
            continue;

        if (bot->GetZoneId() != bot->GetMap()->GetZoneId(bot->GetPhaseMask(), dx, dy, dz))
            continue;

        poiInfo.push_back({{dx, dy}, qPoi.ObjectiveIndex});
    }

    if (poiInfo.size() == 0)
    {
        // LOG_DEBUG("playerbots", "[New rpg] {}: No available poi can be found for quest {}", bot->GetName(), questId);
        return false;
    }

    return true;
}

// Resolve the height for a 2D quest-POI destination. quest_poi data has
// no Z column; probing from MAX_HEIGHT lands on the TOPMOST surface, so
// an objective inside a cave gets a destination on the hill ABOVE it —
// every move then steers the bot out of the cave toward the roof. When
// the objective's actual spawns (its creature/GO entries, or for item
// objectives the creatures/GOs that yield the item) exist near the POI,
// use the closest spawn's real Z; otherwise keep the surface probe.
float NewRpgBaseAction::ResolveQuestPOIDestZ(Quest const* quest, int32 objectiveIdx,
                                             float dx, float dy, float surfaceZ)
{
    if (!quest || objectiveIdx < 0)
        return surfaceZ;

    std::vector<uint32> creatureEntries;
    std::vector<uint32> goEntries;

    if (objectiveIdx < QUEST_OBJECTIVES_COUNT)
    {
        int32 const entry = quest->RequiredNpcOrGo[objectiveIdx];
        if (entry > 0)
            creatureEntries.push_back(entry);
        else if (entry < 0)
            goEntries.push_back(-entry);
    }
    else if (objectiveIdx < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
    {
        uint32 const itemId = quest->RequiredItemId[objectiveIdx - QUEST_OBJECTIVES_COUNT];
        if (itemId)
        {
            for (auto const& [entry, items] : *sObjectMgr->GetGameObjectQuestItemMap())
                if (std::find(items.begin(), items.end(), itemId) != items.end())
                    goEntries.push_back(entry);

            for (auto const& [entry, items] : *sObjectMgr->GetCreatureQuestItemMap())
                if (std::find(items.begin(), items.end(), itemId) != items.end())
                    creatureEntries.push_back(entry);
        }
    }

    return NearestQuestSpawnZ(creatureEntries, goEntries, dx, dy, surfaceZ);
}

// Turn-in variant: the destination is the quest ender, which can sit
// well above the terrain (treehouse platforms, towers). Resolve the
// height from the ender's actual spawn instead of the surface probe.
float NewRpgBaseAction::ResolveQuestTurnInDestZ(uint32 questId, float dx, float dy, float surfaceZ)
{
    std::vector<uint32> creatureEntries;
    std::vector<uint32> goEntries;

    for (auto const& [entry, qId] : *sObjectMgr->GetCreatureQuestInvolvedRelationMap())
        if (qId == questId)
            creatureEntries.push_back(entry);

    for (auto const& [entry, qId] : *sObjectMgr->GetGOQuestInvolvedRelationMap())
        if (qId == questId)
            goEntries.push_back(entry);

    if (creatureEntries.empty() && goEntries.empty())
        return surfaceZ;

    return NearestQuestSpawnZ(creatureEntries, goEntries, dx, dy, surfaceZ);
}

// The nearest matching spawn in 2D decides the height. 100y absorbs
// POI-polygon inaccuracy while staying local to the objective.
float NewRpgBaseAction::NearestQuestSpawnZ(std::vector<uint32> const& creatureEntries,
                                           std::vector<uint32> const& goEntries,
                                           float dx, float dy, float surfaceZ)
{
    if (creatureEntries.empty() && goEntries.empty())
        return surfaceZ;

    float constexpr maxDistSq = 100.0f * 100.0f;
    float bestSq = maxDistSq;
    float bestZ = surfaceZ;
    uint32 const mapId = bot->GetMapId();

    if (!creatureEntries.empty())
    {
        for (auto const& itr : sObjectMgr->GetAllCreatureData())
        {
            CreatureData const& data = itr.second;
            if (data.mapid != mapId)
                continue;
            if (std::find(creatureEntries.begin(), creatureEntries.end(), data.id) == creatureEntries.end())
                continue;
            float const dsq = (data.posX - dx) * (data.posX - dx) + (data.posY - dy) * (data.posY - dy);
            if (dsq < bestSq)
            {
                bestSq = dsq;
                bestZ = data.posZ;
            }
        }
    }

    if (!goEntries.empty())
    {
        for (auto const& itr : sObjectMgr->GetAllGOData())
        {
            GameObjectData const& data = itr.second;
            if (data.mapid != mapId)
                continue;
            if (std::find(goEntries.begin(), goEntries.end(), data.id) == goEntries.end())
                continue;
            float const dsq = (data.posX - dx) * (data.posX - dx) + (data.posY - dy) * (data.posY - dy);
            if (dsq < bestSq)
            {
                bestSq = dsq;
                bestZ = data.posZ;
            }
        }
    }

    return bestZ;
}

WorldPosition NewRpgBaseAction::SelectRandomGrindPos(Player* bot)
{
    std::vector<WorldLocation> const& locs = sTravelMgr.GetLocsPerLevelCache(bot->GetLevel());
    float hiRange = 500.0f;
    float loRange = 2500.0f;
    if (bot->GetLevel() < 5)
    {
        hiRange /= 3;
        loRange /= 3;
    }
    std::vector<WorldLocation> lo_prepared_locs, hi_prepared_locs;

    bool inCity = false;
    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId()))
    {
        if (zone->flags & AREA_FLAG_CAPITAL)
            inCity = true;
    }

    for (auto& loc : locs)
    {
        if (bot->GetMapId() != loc.GetMapId())
            continue;

        if (bot->GetExactDist(loc) > 2500.0f)
            continue;

        if (!inCity && bot->GetMap()->GetZoneId(bot->GetPhaseMask(), loc.GetPositionX(), loc.GetPositionY(),
                                                loc.GetPositionZ()) != bot->GetZoneId())
            continue;

        if (bot->GetExactDist(loc) < hiRange)
        {
            hi_prepared_locs.push_back(loc);
        }

        if (bot->GetExactDist(loc) < loRange)
        {
            lo_prepared_locs.push_back(loc);
        }
    }
    WorldPosition dest{};
    if (urand(1, 100) <= 50 && !hi_prepared_locs.empty())
    {
        uint32 idx = urand(0, hi_prepared_locs.size() - 1);
        dest = hi_prepared_locs[idx];
    }
    else if (!lo_prepared_locs.empty())
    {
        uint32 idx = urand(0, lo_prepared_locs.size() - 1);
        dest = lo_prepared_locs[idx];
    }

    if (!dest.IsValid())
        return dest;

    LOG_DEBUG("playerbots", "[New RPG] Bot {} select random grind pos Map:{} X:{} Y:{} Z:{} ({}+{} available in {})",
              bot->GetName(), dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
              hi_prepared_locs.size(), lo_prepared_locs.size() - hi_prepared_locs.size(), locs.size());
    return dest;
}

WorldPosition NewRpgBaseAction::SelectRandomCampPos(Player* bot)
{
    const std::vector<WorldLocation> locs = sTravelMgr.GetTravelHubs(bot);

    bool inCity = false;

    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId()))
    {
        if (zone->flags & AREA_FLAG_CAPITAL)
            inCity = true;
    }

    std::vector<WorldLocation> prepared_locs;
    for (auto& loc : locs)
    {
        if (bot->GetMapId() != loc.GetMapId())
            continue;

        float range = bot->GetLevel() <= 5 ? 500.0f : 2500.0f;
        if (bot->GetExactDist(loc) > range)
            continue;

        if (bot->GetExactDist(loc) < 50.0f)
            continue;

        if (!inCity && bot->GetMap()->GetZoneId(bot->GetPhaseMask(), loc.GetPositionX(), loc.GetPositionY(),
                                                loc.GetPositionZ()) != bot->GetZoneId())
            continue;

        prepared_locs.push_back(loc);
    }
    WorldPosition dest{};
    if (!prepared_locs.empty())
    {
        uint32 idx = urand(0, prepared_locs.size() - 1);
        dest = prepared_locs[idx];
    }

    if (!dest.IsValid())
        return dest;

    LOG_DEBUG("playerbots", "[New RPG] Bot {} select random inn keeper pos Map:{} X:{} Y:{} Z:{} ({} available in {})",
              bot->GetName(), dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
              prepared_locs.size(), locs.size());
    return dest;
}

bool NewRpgBaseAction::SelectRandomFlightTaxiNode(uint32& flightMasterEntry, WorldPosition& flightMasterPos, std::vector<uint32>& path)
{
    TravelMgr::FlightMasterInfo const* info = sTravelMgr.GetNearestFlightMasterInfo(bot);
    if (!info)
        return false;

    std::vector<std::vector<uint32>> availablePaths = sTravelMgr.GetOptimalFlightDestinations(bot);
    if (availablePaths.empty())
        return false;

    flightMasterEntry = info->templateEntry;
    flightMasterPos = info->pos;
    path = availablePaths[urand(0, availablePaths.size() - 1)];
    LOG_DEBUG("playerbots", "[New RPG] Bot {} select random flight taxi node from:{} (node {}) to:{} ({} available)",
              bot->GetName(), flightMasterEntry, path[0], path[path.size() - 1], availablePaths.size());
    return true;
}

bool NewRpgBaseAction::RandomChangeStatus(std::vector<NewRpgStatus> candidateStatus)
{
    std::vector<NewRpgStatus> availableStatus;
    uint32 probSum = 0;
    for (NewRpgStatus status : candidateStatus)
    {
        if (sPlayerbotAIConfig.RpgStatusProbWeight[status] == 0)
            continue;

        if (CheckRpgStatusAvailable(status))
        {
            availableStatus.push_back(status);
            probSum += sPlayerbotAIConfig.RpgStatusProbWeight[status];
        }
    }
    if (availableStatus.empty() || probSum == 0)
    {
        botAI->rpgInfo.ChangeToRest();
        bot->SetStandState(UNIT_STAND_STATE_SIT);
        return true;
    }
    NewRpgStatus chosenStatus = RPG_STATUS_END;

    // Quest focus: a bot with a doable quest in the log does the quest.
    // The weighted lottery below otherwise sends it grinding or
    // wandering into other zones more often than not even with open
    // quests; the other statuses come back once the log is empty,
    // finished, or every quest went low-priority.
    if (sPlayerbotAIConfig.AutoDoQuests &&
        std::find(availableStatus.begin(), availableStatus.end(), RPG_DO_QUEST) != availableStatus.end())
    {
        chosenStatus = RPG_DO_QUEST;
    }
    else
    {
        uint32 rand = urand(1, probSum);
        uint32 accumulate = 0;
        for (NewRpgStatus status : availableStatus)
        {
            accumulate += sPlayerbotAIConfig.RpgStatusProbWeight[status];
            if (accumulate >= rand)
            {
                chosenStatus = status;
                break;
            }
        }
    }

    switch (chosenStatus)
    {
        case RPG_WANDER_RANDOM:
        {
            botAI->rpgInfo.ChangeToWanderRandom();
            return true;
        }
        case RPG_WANDER_NPC:
        {
            botAI->rpgInfo.ChangeToWanderNpc();
            return true;
        }
        case RPG_GO_GRIND:
        {
            WorldPosition pos = SelectRandomGrindPos(bot);
            if (pos != WorldPosition())
            {
                botAI->rpgInfo.ChangeToGoGrind(pos);
                return true;
            }
            return false;
        }
        case RPG_GO_CAMP:
        {
            WorldPosition pos = SelectRandomCampPos(bot);
            if (pos != WorldPosition())
            {
                botAI->rpgInfo.ChangeToGoCamp(pos);
                return true;
            }
            return false;
        }
        case RPG_DO_QUEST:
        {
            std::vector<uint32> availableQuests;
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 questId = bot->GetQuestSlotQuestId(slot);
                if (botAI->lowPriorityQuest.find(questId) != botAI->lowPriorityQuest.end())
                    continue;

                std::vector<POIInfo> poiInfo;
                if (GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
                {
                    availableQuests.push_back(questId);
                }
            }
            if (availableQuests.size())
            {
                uint32 questId = availableQuests[urand(0, availableQuests.size() - 1)];
                Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                if (quest)
                {
                    botAI->rpgInfo.ChangeToDoQuest(questId, quest);
                    return true;
                }
            }
            return false;
        }
        case RPG_TRAVEL_FLIGHT:
        {
            uint32 flightMasterEntry = 0;
            WorldPosition flightMasterPos;
            std::vector<uint32> path;
            if (SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path))
            {
                botAI->rpgInfo.ChangeToTravelFlight(flightMasterEntry, flightMasterPos, path);
                return true;
            }
            return false;
        }
        case RPG_DO_GATHER:
        {
            botAI->rpgInfo.ChangeToDoGather();
            return true;
        }
        case RPG_IDLE:
        {
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        case RPG_REST:
        {
            botAI->rpgInfo.ChangeToRest();
            bot->SetStandState(UNIT_STAND_STATE_SIT);
            return true;
        }
        case RPG_OUTDOOR_PVP:
        {
            botAI->rpgInfo.ChangeToOutdoorPvp();
            return true;
        }
        default:
        {
            botAI->rpgInfo.ChangeToRest();
            bot->SetStandState(UNIT_STAND_STATE_SIT);
            return true;
        }
    }
    return false;
}

bool NewRpgBaseAction::CheckRpgStatusAvailable(NewRpgStatus status)
{
    switch (status)
    {
        case RPG_IDLE:
        case RPG_REST:
            return true;
        case RPG_WANDER_RANDOM:
        {
            Unit* target = AI_VALUE(Unit*, "grind target");
            return target != nullptr;
        }
        case RPG_GO_GRIND:
        {
            WorldPosition pos = SelectRandomGrindPos(bot);
            return pos != WorldPosition();
        }
        case RPG_GO_CAMP:
        {
            WorldPosition pos = SelectRandomCampPos(bot);
            return pos != WorldPosition();
        }
        case RPG_WANDER_NPC:
        {
            GuidVector possibleTargets = AI_VALUE(GuidVector, "possible new rpg targets");
            return possibleTargets.size() >= 3;
        }
        case RPG_DO_QUEST:
        {
            std::vector<uint32> availableQuests;
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 questId = bot->GetQuestSlotQuestId(slot);
                if (botAI->lowPriorityQuest.find(questId) != botAI->lowPriorityQuest.end())
                    continue;

                std::vector<POIInfo> poiInfo;
                if (GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
                {
                    return true;
                }
            }
            return false;
        }
        case RPG_DO_GATHER:
        {
            if (!botAI->HasSkill(SKILL_HERBALISM) && !botAI->HasSkill(SKILL_MINING))
                return false;

            // No point farming with (nearly) full bags - the loot pipeline
            // can't store the harvest (see StoreLootAction).
            if (AI_VALUE(uint8, "bag space") > 80)
                return false;

            return sGatherNodeMgr.HasUsableNodes(bot);
        }
        case RPG_TRAVEL_FLIGHT:
        {
            uint32 flightMasterEntry = 0;
            WorldPosition flightMasterPos;
            std::vector<uint32> path;
            return SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path);
        }
        case RPG_OUTDOOR_PVP:
        {
            if (!bot->IsPvP())
                return false;
            uint32 zoneId = bot->GetZoneId();
            if (zoneId == AREA_NAGRAND)
                return false;

            OutdoorPvP* outdoorPvP = sOutdoorPvPMgr->GetOutdoorPvPToZoneId(zoneId);
            return outdoorPvP != nullptr;
        }
        default:
            return false;
    }
    return false;
}
