/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MovementActions.h"
#include "Corpse.h"
#include "DBCStores.h"
#include "Event.h"
#include "FleeManager.h"
#include "GameObject.h"
#include "GridDefines.h"
#include "LastMovementValue.h"
#include "LootObjectStack.h"
#include "Map.h"
#include "ModelIgnoreFlags.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "MoveSplineInitArgs.h"
#include "TravelNode.h"
#include "MovementGenerator.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "PathGenerator.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Position.h"
#include "PositionValue.h"
#include "Random.h"
#include "ServerFacade.h"
#include "SharedDefines.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "Stances.h"
#include "Timer.h"
#include "Transport.h"
#include "Unit.h"
#include "Vehicle.h"
#include "WaypointMovementGenerator.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "G3D/Vector3.h"
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>

namespace
{
// Conform a teleport destination's Z to the live ground when resolvable, and
// REJECT an unresolved / out-of-bounds Z before it reaches Player::TeleportTo. A
// baked travel-node position can carry Z = VMAP_INVALID_HEIGHT_VALUE (-200000),
// which UpdateAllowedPositionZ cannot re-resolve; teleporting to it makes the
// core log "invalid coordinates". Returns false (caller skips the teleport this
// tick and re-paths) when the Z cannot be made valid.
bool SafeBotTeleport(Player* bot, WorldPosition dst, float orientation)
{
    float z = dst.GetPositionZ();
    bot->UpdateAllowedPositionZ(dst.GetPositionX(), dst.GetPositionY(), z);
    if (z <= -2000.0f || !Acore::IsValidMapCoord(dst.GetPositionX(), dst.GetPositionY(), z))
        return false;
    return bot->TeleportTo(dst.GetMapId(), dst.GetPositionX(), dst.GetPositionY(), z, orientation);
}
}  // namespace

MovementAction::MovementAction(PlayerbotAI* botAI, std::string const name) : Action(botAI, name)
{
    bot = botAI->GetBot();
}

void MovementAction::EmitDebugMove(char const* method, char const* generator, float x, float y, float z, char const* extra)
{
    if (!botAI->HasStrategy("debug move", BOT_STATE_NON_COMBAT))
        return;

    // Distance to the actual RPG GOAL (the NPC / quest POI / camp), as
    // opposed to `dis` below which is only the immediate move step. -1
    // when there is no positional goal. Without this the step distance
    // sits next to the target name and reads as "the NPC is Xy away".
    float goalDist = -1.0f;

    auto resolveName = [&](ObjectGuid guid) -> std::string
    {
        if (!guid)
            return "";
        if (WorldObject* obj = botAI->GetWorldObject(guid))
        {
            goalDist = bot->GetExactDist(obj);
            return obj->GetName();
        }
        return "";
    };

    NewRpgInfo& info = botAI->rpgInfo;
    NewRpgStatus status = info.GetStatus();
    char const* statusName =
        status == RPG_IDLE ? "idle" :
        status == RPG_GO_GRIND ? "go-grind" :
        status == RPG_GO_CAMP ? "go-camp" :
        status == RPG_WANDER_NPC ? "wander-npc" :
        status == RPG_WANDER_RANDOM ? "wander-random" :
        status == RPG_REST ? "rest" :
        status == RPG_DO_QUEST ? "do-quest" :
        status == RPG_TRAVEL_FLIGHT ? "travel-flight" :
        status == RPG_OUTDOOR_PVP ? "outdoor-pvp" : "?";

    // Resolve a human-readable target name from the RPG context. When
    // we can name the target (quest objective, wander NPC, flight
    // master, travel-node hop, etc.), it replaces the loc=(x,y,z)
    // field — names are far more useful than coordinates. When no
    // target can be named (combat moves, follow, flee, ad-hoc), we
    // fall through to loc=(x,y,z).
    std::string targetName;
    switch (status)
    {
        case RPG_DO_QUEST:
            if (auto* data = std::get_if<NewRpgInfo::DoQuest>(&info.data))
            {
                if (data->quest)
                {
                    bool turnIn = data->questId &&
                        bot->GetQuestStatus(data->questId) == QUEST_STATUS_COMPLETE;
                    if (turnIn)
                    {
                        std::ostringstream t;
                        t << "turn-in:" << data->quest->GetTitle() << "(" << data->questId << ")";
                        targetName = t.str();
                    }
                    else if (bot->getQuestStatusMap().count(data->questId))
                    {
                        // Guarded lookup: RewardQuest erases the status-map
                        // entry (and a drop leaves NONE) while rpgInfo can
                        // still hold the stale DoQuest state until the RPG
                        // action next runs — .at() here would throw.
                        Quest const* q = data->quest;
                        QuestStatusData const& qs = bot->getQuestStatusMap().at(data->questId);
                        std::string goal;
                        for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                        {
                            int32 entry = q->RequiredNpcOrGo[i];
                            if (entry != 0 && qs.CreatureOrGOCount[i] < q->RequiredNpcOrGoCount[i])
                            {
                                if (entry > 0)
                                {
                                    if (CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(entry))
                                        goal = "mob:" + ct->Name;
                                }
                                else
                                {
                                    if (GameObjectTemplate const* gt = sObjectMgr->GetGameObjectTemplate(-entry))
                                        goal = "go:" + gt->name;
                                }
                                break;
                            }
                            uint32 item = q->RequiredItemId[i];
                            if (item && bot->GetItemCount(item, true) < q->RequiredItemCount[i])
                            {
                                if (ItemTemplate const* it = sObjectMgr->GetItemTemplate(item))
                                    goal = "item:" + it->Name1;
                                break;
                            }
                        }
                        if (goal.empty())
                        {
                            std::ostringstream t;
                            t << "quest:" << q->GetTitle() << "(" << data->questId << ")";
                            goal = t.str();
                        }
                        targetName = goal;
                    }
                }
            }
            break;
        case RPG_WANDER_NPC:
            if (auto* data = std::get_if<NewRpgInfo::WanderNpc>(&info.data))
            {
                std::string n = resolveName(data->npcOrGo);
                if (!n.empty())
                    targetName = "npc:" + n;
            }
            break;
        case RPG_TRAVEL_FLIGHT:
            if (auto* data = std::get_if<NewRpgInfo::TravelFlight>(&info.data))
            {
                if (CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(data->flightMasterEntry))
                    targetName = "flightmaster:" + ct->Name;
            }
            break;
        case RPG_GO_GRIND:
            targetName = "grind-pos";
            if (auto* data = std::get_if<NewRpgInfo::GoGrind>(&info.data))
                goalDist = bot->GetExactDist(data->pos.GetPositionX(), data->pos.GetPositionY(), data->pos.GetPositionZ());
            break;
        case RPG_GO_CAMP:
            targetName = "camp-pos";
            if (auto* data = std::get_if<NewRpgInfo::GoCamp>(&info.data))
                goalDist = bot->GetExactDist(data->pos.GetPositionX(), data->pos.GetPositionY(), data->pos.GetPositionZ());
            break;
        case RPG_WANDER_RANDOM: targetName = "wander-random"; break;
        default: break;
    }

    // Quest POI distance (the DoQuest goal is a position, not the named mob).
    if (status == RPG_DO_QUEST)
        if (auto* data = std::get_if<NewRpgInfo::DoQuest>(&info.data))
            goalDist = bot->GetExactDist(data->pos.GetPositionX(), data->pos.GetPositionY(), data->pos.GetPositionZ());

    // Travel-plan override: when actively routing through the node
    // graph, prefer the next-hop node name over any RPG-level target.
    if (info.HasActiveTravelPlan())
    {
        TravelPlan const& plan = info.travelPlan;
        if (plan.stepIdx < plan.steps.GetPathRef().size())
        {
            PathNodePoint const& pnt = plan.steps.GetPathRef()[plan.stepIdx];
            if (pnt.type == PathNodeType::NODE_NODE || pnt.type == PathNodeType::NODE_PATH)
            {
                if (TravelNode* n = sTravelNodeMap.getNode(pnt.point, nullptr, 5.0f))
                    targetName = "node:" + n->getName();
            }
        }
    }

    float dis = bot->GetExactDist(x, y, z);
    std::ostringstream out;
    // getName() = the ACTION executing this move (attack anything, reach
    // melee, new rpg do quest, ...). The status/target further right is
    // only the RPG-layer GOAL — without the action name, a combat move
    // during a Fel Moss quest reads as "moving to Fel Moss" when the bot
    // is actually fighting something else entirely.
    out << "[M] | " << getName()
        << " | " << method
        << " | " << (generator && *generator ? generator : "-")
        << " | " << statusName
        << " | step " << std::fixed << std::setprecision(1) << dis << "y"
        << " | " << (targetName.empty() ? "-" : targetName.c_str());
    // Real distance to the goal, so it isn't confused with the move step.
    if (goalDist >= 0.0f)
        out << " @" << std::fixed << std::setprecision(1) << goalDist << "y";
    if (extra && *extra)
        out << " | " << extra;
    botAI->TellMasterNoFacing(out);
}

void MovementAction::CreateWp(Player* wpOwner, float x, float y, float z, float o, uint32 entry, bool important)
{
    float dist = wpOwner->GetDistance(x, y, z);
    float delay = IN_MILLISECONDS * dist / wpOwner->GetSpeed(MOVE_RUN) + sPlayerbotAIConfig.ReactDelay;

    // if (!important)
    // delay *= 0.25;

    Creature* wpCreature = wpOwner->SummonCreature(entry, x, y, z - 1, o, TEMPSUMMON_TIMED_DESPAWN, delay);
    botAI->GetBot()->AddAura(246, wpCreature);

    if (!important)
        wpCreature->SetObjectScale(0.5f);
}

bool MovementAction::JumpTo(uint32 mapId, float x, float y, float z, MovementPriority priority)
{
    UpdateMovementState();
    if (!IsMovingAllowed())
        return false;

    if (IsDuplicateMove(x, y, z))
        return false;

    if (IsWaitingForLastMove(priority))
        return false;

    float speed = bot->GetSpeed(MOVE_RUN);
    MotionMaster& mm = *bot->GetMotionMaster();
    mm.Clear();
    mm.MoveJump(x, y, z, speed, speed, 1);
    AI_VALUE(LastMovement&, "last movement").Set(mapId, x, y, z, bot->GetOrientation(), 1000, priority);
    return true;
}

bool MovementAction::MoveNear(uint32 mapId, float x, float y, float z, float distance, MovementPriority priority)
{
    float angle = GetFollowAngle();
    EmitDebugMove("MoveNear", "mmap", x, y, z);
    return MoveTo(mapId, x + cos(angle) * distance, y + sin(angle) * distance, z, false, false, false, false, priority);
}

bool MovementAction::MoveNear(WorldObject* target, float distance, MovementPriority priority)
{
    if (!target)
        return false;

    distance += target->GetCombatReach();

    float followAngle = GetFollowAngle();

    for (float angle = followAngle; angle <= followAngle + static_cast<float>(2 * M_PI);
         angle += static_cast<float>(M_PI / 4.f))
    {
        float x = target->GetPositionX() + cos(angle) * distance;
        float y = target->GetPositionY() + sin(angle) * distance;
        float z = target->GetPositionZ();
        // Clamp Z to the terrain under the offset point so we don't
        // hand PointMovementGenerator a Z that matches the target's
        // floor but not the sampled (x,y) — avoids straight-line
        // fallbacks through geometry.
        bot->UpdateAllowedPositionZ(x, y, z);

        if (!bot->IsWithinLOS(x, y, z))
            continue;

        bool moved = MoveTo(target->GetMapId(), x, y, z, false, false, false, false, priority);
        if (moved)
            return true;
    }

    return false;
}

bool MovementAction::MoveToLOS(WorldObject* target, bool ranged)
{
    if (!target)
        return false;

    float x = target->GetPositionX();
    float y = target->GetPositionY();
    float z = target->GetPositionZ();
    EmitDebugMove("MoveToLOS", "mmap", x, y, z);

    // Use standard PathGenerator to find a route.
    PathResult path = GeneratePath(x, y, z, DEFAULT_PATH_ACCEPT_MASK, false);
    if (!path.reachable)
        return false;

    if (!ranged)
        return MoveTo((Unit*)target, target->GetCombatReach());

    float dist = FLT_MAX;
    PositionInfo dest;

    if (!path.points.empty())
    {
        for (auto& point : path.points)
        {
            if (botAI->HasStrategy("debug move", BOT_STATE_NON_COMBAT))
                CreateWp(bot, point.x, point.y, point.z, 0.0, 2334);

            float distPoint = target->GetDistance(point.x, point.y, point.z);
            if (distPoint < dist && target->IsWithinLOS(point.x, point.y, point.z + bot->GetCollisionHeight()))
            {
                dist = distPoint;
                dest.Set(point.x, point.y, point.z, target->GetMapId());

                if (ranged)
                    break;
            }
        }
    }

    if (dest.isSet())
        return MoveTo(dest.mapId, dest.x, dest.y, dest.z);
    else
        botAI->TellError("All paths not in LOS");

    return false;
}

bool MovementAction::MoveTo(uint32 mapId, float x, float y, float z, bool /*idle*/, bool /*react*/, bool normal_only,
                            bool exact_waypoint, MovementPriority priority, bool lessDelay, bool backwards)
{
    UpdateMovementState();
    if (!IsMovingAllowed())
    {
        return false;
    }
    if (IsDuplicateMove(x, y, z))
    {
        return false;
    }
    if (IsWaitingForLastMove(priority))
    {
        return false;
    }

    bool generatePath = !bot->IsFlying() && !bot->isSwimming();
    bool disableMoveSplinePath =
        sPlayerbotAIConfig.DisableMoveSplinePath >= 2 ||
        (sPlayerbotAIConfig.DisableMoveSplinePath == 1 && bot->InBattleground());
    if (Vehicle* vehicle = bot->GetVehicle())
    {
        VehicleSeatEntry const* seat = vehicle->GetSeatForPassenger(bot);
        Unit* vehicleBase = vehicle->GetBase();
        generatePath = !vehicleBase || !vehicleBase->CanFly();
        if (!vehicleBase || !seat || !seat->CanControl())  // is passenger and cant move anyway
            return false;

        float distance = vehicleBase->GetExactDist(x, y, z);  // use vehicle distance, not bot
        if (distance > 0.01f)
        {
            DoMovePoint(vehicleBase, x, y, z, generatePath, backwards);
            float speed = backwards ? vehicleBase->GetSpeed(MOVE_RUN_BACK) : vehicleBase->GetSpeed(MOVE_RUN);
            float delay = IN_MILLISECONDS * (distance / speed);
            if (lessDelay)
            {
                delay -= botAI->GetReactDelay();
            }
            delay = std::max(.0f, delay);
            delay = std::min((float)sPlayerbotAIConfig.MaxWaitForMove, delay);
            AI_VALUE(LastMovement&, "last movement").Set(mapId, x, y, z, bot->GetOrientation(), delay, priority);
            return true;
        }
    }
    else if (exact_waypoint || disableMoveSplinePath || !generatePath)
    {
        float distance = bot->GetExactDist(x, y, z);
        if (distance > 0.01f)
        {
            if (bot->IsSitState())
                bot->SetStandState(UNIT_STAND_STATE_STAND);

            // bot->CastStop();

            DoMovePoint(bot, x, y, z, generatePath, backwards);
            float delay = IN_MILLISECONDS * MoveDelay(distance, backwards);
            if (lessDelay)
            {
                delay -= botAI->GetReactDelay();
            }
            delay = std::max(.0f, delay);
            delay = std::min((float)sPlayerbotAIConfig.MaxWaitForMove, delay);
            AI_VALUE(LastMovement&, "last movement").Set(mapId, x, y, z, bot->GetOrientation(), delay, priority);
            return true;
        }
    }
    else
    {
        // Direct dispatch — engine MovePoint(generatePath=true) handles
        // pathfinding. Avoid ±z probes: their "shortest path" preference
        // can pick an unreachable ledge and air-walk via NOPATH fallback.
        float distance = bot->GetExactDist(x, y, z);
        if (distance > 0.01f)
        {
            // Reachability gate: on NOPATH the engine's MovePoint falls
            // back to a straight 2-point spline, gliding the bot through
            // geometry to an unreachable target. Probe first and refuse
            // the move instead. Flying/swimming dispatches keep
            // generatePath false and are unaffected.
            if (generatePath)
            {
                PathResult probe = GeneratePath(x, y, z);
                if (!probe.reachable)
                    return false;
            }

            if (bot->IsSitState())
                bot->SetStandState(UNIT_STAND_STATE_STAND);

            DoMovePoint(bot, x, y, z, generatePath, backwards);
            float delay = IN_MILLISECONDS * MoveDelay(distance, backwards);
            if (lessDelay)
                delay -= botAI->GetReactDelay();
            delay = std::max(.0f, delay);
            delay = std::min((float)sPlayerbotAIConfig.MaxWaitForMove, delay);
            AI_VALUE(LastMovement&, "last movement")
                .Set(mapId, x, y, z, bot->GetOrientation(), delay, priority);
            return true;
        }
    }

    return false;
    //
    // // LOG_DEBUG("playerbots", "IsMovingAllowed {}", IsMovingAllowed());
    // bot->AddUnitMovementFlag()

    // bool isVehicle = false;
    // Unit* mover = bot;
    // if (Vehicle* vehicle = bot->GetVehicle())
    // {
    //     VehicleSeatEntry const* seat = vehicle->GetSeatForPassenger(bot);
    //     LOG_DEBUG("playerbots", "!seat || !seat->CanControl() {}", !seat || !seat->CanControl());
    //     if (!seat || !seat->CanControl())
    //         return false;

    //     isVehicle = true;
    //     mover = vehicle->GetBase();
    // }

    // bool detailedMove = botAI->AllowActivity(DETAILED_MOVE_ACTIVITY);
    // if (!detailedMove)
    // {
    //     time_t now = time(nullptr);
    //     if (AI_VALUE(LastMovement&, "last movement").nextTeleport > now) // We can not teleport yet. Wait.
    //     {
    //         LOG_DEBUG("playerbots", "AI_VALUE(LastMovement&, \"last movement\").nextTeleport > now");
    //         botAI->SetNextCheckDelay(
    //             (AI_VALUE(LastMovement&, "last movement").nextTeleport - now) * IN_MILLISECONDS);
    //         return true;
    //     }
    // }

    // float minDist = sPlayerbotAIConfig.TargetPosRecalcDistance; //Minium distance a bot should move.
    // float maxDist = sPlayerbotAIConfig.ReactDistance;           //Maxium distance a bot can move in one single
    // action. float originalZ = z;                                        // save original destination height to check
    // if bot needs to fly up

    // bool generatePath = !bot->IsFlying() && !bot->HasUnitMovementFlag(MOVEMENTFLAG_SWIMMING) && !bot->IsInWater() &&
    // !bot->IsUnderWater(); if (generatePath)
    // {
    //     z += CONTACT_DISTANCE;
    //     mover->UpdateAllowedPositionZ(x, y, z);
    // }

    // if (!isVehicle && !IsMovingAllowed() && bot->isDead())
    // {
    //     bot->StopMoving();
    //     LOG_DEBUG("playerbots", "!isVehicle && !IsMovingAllowed() && bot->isDead()");
    //     return false;
    // }

    // if (!isVehicle && bot->isMoving() && !IsMovingAllowed())
    // {
    //     if (!bot->HasUnitState(UNIT_STATE_IN_FLIGHT))
    //         bot->StopMoving();
    //     LOG_DEBUG("playerbots", "!isVehicle && bot->isMoving() && !IsMovingAllowed()");
    //     return false;
    // }

    // LastMovement& lastMove = *context->GetValue<LastMovement&>("last movement");

    // WorldPosition startPosition = WorldPosition(bot);             //Current location of the bot
    // WorldPosition endPosition = WorldPosition(mapId, x, y, z, 0); //The requested end location
    // WorldPosition movePosition;                                   //The actual end location

    // float totalDistance = startPosition.distance(endPosition);    //Total distance to where we want to go
    // float maxDistChange = totalDistance * 0.1;                    //Maximum change between previous destination
    // before needing a recalulation

    // if (totalDistance < minDist)
    // {
    //     if (lastMove.lastMoveShort.distance(endPosition) < maxDistChange)
    //         AI_VALUE(LastMovement&, "last movement").clear();

    //     mover->StopMoving();
    //     LOG_DEBUG("playerbots", "totalDistance < minDist");
    //     return false;
    // }

    // TravelPath movePath;

    // if (lastMove.lastMoveShort.distance(endPosition) < maxDistChange &&
    // startPosition.distance(lastMove.lastMoveShort) < maxDist) //The last short movement was to the same place we want
    // to move now.
    //     movePosition = endPosition;
    // else if (!lastMove.lastPath.empty() && lastMove.lastPath.getBack().distance(endPosition) < maxDistChange) //The
    // last long movement was to the same place we want to move now.
    // {
    //     movePath = lastMove.lastPath;
    // }
    // else
    // {
    //     movePosition = endPosition;

    //     if (startPosition.GetMapId() != endPosition.GetMapId() || totalDistance > maxDist)
    //     {
    //         if (!TravelNodeMap::instance().getNodes().empty() && !bot->InBattleground())
    //         {
    //             if (sPlayerbotAIConfig.TweakValue)
    //             {
    //                 if (lastMove.future.valid())
    //                 {
    //                     movePath = lastMove.future.get();
    //                 }
    //                 else
    //                 {
    //                     lastMove.future = std::async(&TravelNodeMap::getFullPath, startPosition, endPosition, bot);
    //                     LOG_DEBUG("playerbots", "lastMove.future = std::async(&TravelNodeMap::getFullPath,
    //                     startPosition, endPosition, bot);"); return true;
    //                 }
    //             }
    //             else
    //                 movePath = TravelNodeMap::instance().getFullPath(startPosition, endPosition, bot);

    //             if (movePath.empty())
    //             {
    //                 //We have no path. Beyond 450yd the standard PathGenerator will probably move the wrong way.
    //                 if (ServerFacade::instance().IsDistanceGreaterThan(totalDistance, maxDist * 3))
    //                 {
    //                     movePath.clear();
    //                     movePath.addPoint(endPosition);
    //                     AI_VALUE(LastMovement&, "last movement").setPath(movePath);

    //                     bot->StopMoving();
    //                     if (botAI->HasStrategy("debug move", BOT_STATE_NON_COMBAT))
    //                         botAI->TellMasterNoFacing("I have no path");
    //                     LOG_DEBUG("playerbots", "ServerFacade::instance().IsDistanceGreaterThan(totalDistance, maxDist * 3)");
    //                     return false;
    //                 }

    //                 movePosition = endPosition;
    //             }
    //         }
    //         else
    //         {
    //             //Use standard PathGenerator to find a route.
    //             movePosition = endPosition;
    //         }
    //     }
    // }

    // if (movePath.empty() && movePosition.distance(startPosition) > maxDist)
    // {
    //     //Use standard PathGenerator to find a route.
    //     PathGenerator path(mover);
    //     path.CalculatePath(movePosition.GetPositionX(), movePosition.GetPositionY(), movePosition.GetPositionZ(), false);
    //     PathType type = path.GetPathType();
    //     Movement::PointsArray const& points = path.GetPath();
    //     movePath.addPath(startPosition.fromPointsArray(points));
    // }

    // if (!movePath.empty())
    // {
    //     if (movePath.makeShortCut(startPosition, maxDist))
    //         if (botAI->HasStrategy("debug move", BOT_STATE_NON_COMBAT))
    //             botAI->TellMasterNoFacing("Found a shortcut.");

    //     if (movePath.empty())
    //     {
    //         AI_VALUE(LastMovement&, "last movement").setPath(movePath);

    //         if (botAI->HasStrategy("debug move", BOT_STATE_NON_COMBAT))
    //             botAI->TellMasterNoFacing("Too far from path. Rebuilding.");
    //         LOG_DEBUG("playerbots", "movePath.empty()");
    //         return true;
    //     }

    //     TravelNodePathType pathType;
    //     uint32 entry;
    //     movePosition = movePath.getNextPoint(startPosition, maxDist, pathType, entry);

    //     if (pathType == TravelNodePathType::portal) // && !botAI->isRealPlayer())
    //     {
    //         //Log bot movement
    //         if (sPlayerbotAIConfig.HasLog("bot_movement.csv"))
    //         {
    //             WorldPosition telePos;
    //             if (entry)
    //             {
    //                 if (AreaTriggerTeleport const* at = sObjectMgr->GetAreaTriggerTeleport(entry))
    //                     telePos = WorldPosition(at->target_mapId, at->target_X, at->target_Y, at->target_Z,
    //                     at->target_Orientation);
    //             }
    //             else
    //                 telePos = movePosition;

    //             std::ostringstream out;
    //             out << sPlayerbotAIConfig.GetTimestampStr() << "+00,";
    //             out << bot->GetName() << ",";
    //             if (telePos && telePos.GetExactDist(movePosition) > 0.001)
    //                 startPosition.printWKT({ startPosition, movePosition, telePos }, out, 1);
    //             else
    //                 startPosition.printWKT({ startPosition, movePosition }, out, 1);

    //             out << std::to_string(bot->getRace()) << ",";
    //             out << std::to_string(bot->getClass()) << ",";
    //             out << bot->GetLevel() << ",";
    //             out << (entry ? -1 : entry);

    //             sPlayerbotAIConfig.Log("bot_movement.csv", out.str().c_str());
    //         }

    //         if (entry)
    //         {
    //             AI_VALUE(LastMovement&, "last area trigger").lastAreaTrigger = entry;
    //         }
    //         else
    //         {
    //             LOG_DEBUG("playerbots", "!entry");
    //             return bot->TeleportTo(movePosition.GetMapId(), movePosition.GetPositionX(), movePosition.GetPositionY(),
    //             movePosition.GetPositionZ(), movePosition.GetOrientation(), 0);
    //         }
    //     }

    //     if (pathType == TravelNodePathType::transport && entry)
    //     {
    //         if (!bot->GetTransport())
    //         {
    //             for (auto& transport : movePosition.getTransports(entry))
    //                 if (movePosition.sqDistance2d(WorldPosition((WorldObject*)transport)) < 5 * 5)
    //                     transport->AddPassenger(bot, true);
    //         }
    //         WaitForReach(100.0f);
    //         LOG_DEBUG("playerbots", "pathType == TravelNodePathType::transport && entry");
    //         return true;
    //     }

    //     if (pathType == TravelNodePathType::flightPath && entry)
    //     {
    //         if (TaxiPathEntry const* tEntry = sTaxiPathStore.LookupEntry(entry))
    //         {
    //             Creature* unit = nullptr;

    //             if (!bot->m_taxi.IsTaximaskNodeKnown(tEntry->from))
    //             {
    //                 GuidVector npcs = AI_VALUE(GuidVector, "nearest npcs");
    //                 for (GuidVector::iterator i = npcs.begin(); i != npcs.end(); i++)
    //                 {
    //                     Creature* unit = bot->GetNPCIfCanInteractWith(*i, UNIT_NPC_FLAG_FLIGHTMASTER);
    //                     if (!unit)
    //                         continue;

    //                     bot->GetSession()->SendLearnNewTaxiNode(unit);

    //                     unit->SetFacingTo(unit->GetAngle(bot));
    //                 }
    //             }

    //             uint32 botMoney = bot->GetMoney();
    //             if (botAI->HasCheat(BotCheatMask::gold))
    //             {
    //                 bot->SetMoney(10000000);
    //             }

    //             bool goTaxi = bot->ActivateTaxiPathTo({ tEntry->from, tEntry->to }, unit, 1);

    //             if (botAI->HasCheat(BotCheatMask::gold))
    //                 bot->SetMoney(botMoney);
    //             LOG_DEBUG("playerbots", "goTaxi");
    //             return goTaxi;
    //         }
    //     }

    //     // if (pathType == TravelNodePathType::teleportSpell && entry)
    //     // {
    //     //     if (entry == 8690)
    //     //     {
    //     //         if (!bot->HasSpellCooldown(8690))
    //     //         {
    //     //             return botAI->DoSpecificAction("hearthstone", Event("move action"));
    //     //         }
    //     //         else
    //     //         {
    //     //             movePath.clear();
    //     //             AI_VALUE(LastMovement&, "last movement").setPath(movePath);
    //     //             LOG_DEBUG("playerbots", "bot->HasSpellCooldown(8690)");
    //     //             return false;
    //     //         }
    //     //     }
    //     // }

    //     //if (!isTransport && bot->GetTransport())
    //     //    bot->GetTransport()->RemovePassenger(bot);
    // }

    // AI_VALUE(LastMovement&, "last movement").setPath(movePath);

    // if (!movePosition || movePosition.GetMapId() != bot->GetMapId())
    // {
    //     movePath.clear();
    //     AI_VALUE(LastMovement&, "last movement").setPath(movePath);

    //     if (botAI->HasStrategy("debug move", BOT_STATE_NON_COMBAT))
    //         botAI->TellMasterNoFacing("No point. Rebuilding.");
    //     LOG_DEBUG("playerbots", "!movePosition || movePosition.GetMapId() != bot->GetMapId()");
    //     return false;
    // }

    // if (movePosition.distance(startPosition) > maxDist)
    // {
    //     //Use standard pathfinder to find a route.
    //     PathGenerator path(mover);
    //     path.CalculatePath(movePosition.getX(), movePosition.getY(), movePosition.getZ(), false);
    //     PathType type = path.GetPathType();
    //     Movement::PointsArray const& points = path.GetPath();
    //     movePath.addPath(startPosition.fromPointsArray(points));
    //     TravelNodePathType pathType;
    //     uint32 entry;
    //     movePosition = movePath.getNextPoint(startPosition, maxDist, pathType, entry);
    // }

    // if (movePosition == WorldPosition())
    // {
    //     movePath.clear();

    //     AI_VALUE(LastMovement&, "last movement").setPath(movePath);

    //     if (botAI->HasStrategy("debug move", BOT_STATE_NON_COMBAT))
    //         botAI->TellMasterNoFacing("No point. Rebuilding.");

    //     return false;
    // }

    // //Visual waypoints
    // if (botAI->HasStrategy("debug move", BOT_STATE_NON_COMBAT))
    // {
    //     if (!movePath.empty())
    //     {
    //         float cx = x;
    //         float cy = y;
    //         float cz = z;
    //         for (auto i : movePath.getPath())
    //         {
    //             CreateWp(bot, i.point.GetPositionX(), i.point.GetPositionY(), i.point.GetPositionZ(), 0.f, 2334);

    //             cx = i.point.GetPositionX();
    //             cy = i.point.GetPositionY();
    //             cz = i.point.GetPositionZ();
    //         }
    //     }
    //     else
    //         CreateWp(bot, movePosition.GetPositionX(), movePosition.GetPositionY(), movePosition.GetPositionZ(), 0, 2334, true);
    // }

    // //Log bot movement
    // if (sPlayerbotAIConfig.HasLog("bot_movement.csv") && lastMove.lastMoveShort.GetExactDist(movePosition) > 0.001)
    // {
    //     std::ostringstream out;
    //     out << sPlayerbotAIConfig.GetTimestampStr() << "+00,";
    //     out << bot->GetName() << ",";
    //     startPosition.printWKT({ startPosition, movePosition }, out, 1);
    //     out << std::to_string(bot->getRace()) << ",";
    //     out << std::to_string(bot->getClass()) << ",";
    //     out << bot->GetLevel();
    //     out << 0;

    //     sPlayerbotAIConfig.Log("bot_movement.csv", out.str().c_str());
    // }
    // // LOG_DEBUG("playerbots", "({}, {}) -> ({}, {})", startPosition.GetPositionX(), startPosition.GetPositionY(),
    // movePosition.GetPositionX(), movePosition.GetPositionY()); if (!react)
    //     if (totalDistance > maxDist)
    //         WaitForReach(startPosition.distance(movePosition) - 10.0f);
    //     else
    //         WaitForReach(startPosition.distance(movePosition));

    // if (!isVehicle)
    // {
    //     bot->HandleEmoteCommand(0);
    //     if (bot->IsSitState())
    //         bot->SetStandState(UNIT_STAND_STATE_STAND);

    //     bot->CastStop();

    //  /* Why do we do this?
    // if (lastMove.lastMoveShort.distance(movePosition) < minDist)
    // {
    //     bot->StopMoving();
    //     bot->GetMotionMaster()->Clear();
    // }
    // */

    // // Clean movement if not already moving the same way.
    // // if (mover->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
    // // {
    // //     mover->StopMoving();
    // //     mover->GetMotionMaster()->Clear();
    // // }
    // // else
    // // {
    // //     mover->GetMotionMaster()->GetDestination(x, y, z);
    // //     if (movePosition.distance(WorldPosition(movePosition.GetMapId(), x, y, z, 0)) > minDist)
    // //     {
    // //         mover->StopMoving();
    // //         mover->GetMotionMaster()->Clear();
    // //     }
    // // }

    // if (totalDistance > maxDist && !detailedMove && !botAI->HasPlayerNearby(&movePosition)) // Why walk if you can
    // fly?
    // {
    //     time_t now = time(nullptr);

    //     AI_VALUE(LastMovement&, "last movement").nextTeleport = now +
    //     (time_t)MoveDelay(startPosition.distance(movePosition)); LOG_DEBUG("playerbots", "totalDistance > maxDist &&
    //     !detailedMove && !botAI->HasPlayerNearby(&movePosition)"); return bot->TeleportTo(movePosition.GetMapId(),
    //     movePosition.GetPositionX(), movePosition.GetPositionY(), movePosition.GetPositionZ(), startPosition.getAngleTo(movePosition));
    // }

    // // walk if master walks and is close
    // bool masterWalking = false;
    // if (botAI->GetMaster())
    // {
    //     if (botAI->GetMaster()->m_movementInfo.HasMovementFlag(MOVEMENTFLAG_WALKING) &&
    //     ServerFacade::instance().GetDistance2d(bot, botAI->GetMaster()) < 20.0f)
    //         masterWalking = true;
    // }

    // if (masterWalking)
    //     bot->SetWalk(true);

    // bot->SendMovementFlagUpdate();
    // // LOG_DEBUG("playerbots", "normal move? {} {} {}",
    // !bot->HasAuraType(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED) && !bot->HasAuraType(SPELL_AURA_FLY),
    // //     bot->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE), bot->getStandState());
    // if (!bot->HasAuraType(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED) && !bot->HasAuraType(SPELL_AURA_FLY))
    // {
    //     bot->SetWalk(masterWalking);
    //     bot->GetMotionMaster()->MovePoint(movePosition.GetMapId(), movePosition.GetPositionX(), movePosition.GetPositionY(),
    //     movePosition.GetPositionZ(), generatePath); WaitForReach(startPosition.distance(movePosition));
    //     // LOG_DEBUG("playerbots", "Movepoint to ({}, {})", movePosition.GetPositionX(), movePosition.GetPositionY());
    // }
    // else
    // {
    //     bool needFly = false;
    //     bool needLand = false;
    //     bool isFly = bot->IsFlying();

    //     if (!isFly && originalZ > bot->GetPositionZ() && (originalZ - bot->GetPositionZ()) > 5.0f)
    //         needFly = true;

    //     if (needFly && !isFly)
    //     {
    //         WorldPacket data(SMSG_SPLINE_MOVE_SET_FLYING, 9);
    //         data << bot->GetPackGUID();
    //         bot->SendMessageToSet(&data, true);

    //         if (!bot->m_movementInfo.HasMovementFlag(MOVEMENTFLAG_FLYING))
    //             bot->m_movementInfo.AddMovementFlag(MOVEMENTFLAG_FLYING);

    //         if (!bot->m_movementInfo.HasMovementFlag(MOVEMENTFLAG_DISABLE_GRAVITY))
    //             bot->m_movementInfo.AddMovementFlag(MOVEMENTFLAG_DISABLE_GRAVITY);
    //     }

    //     if (isFly)
    //     {
    //         float ground = bot->GetPositionZ();
    //         float height = bot->GetMap()->GetWaterOrGroundLevel(bot->GetPositionX(), bot->GetPositionY(),
    //         bot->GetPositionZ(), ground); if (bot->GetPositionZ() > originalZ && (bot->GetPositionZ() - originalZ
    //         < 5.0f) && (fabs(originalZ - ground) < 5.0f))
    //             needLand = true;

    //         if (needLand)
    //         {
    //             WorldPacket data(SMSG_SPLINE_MOVE_UNSET_FLYING, 9);
    //             data << bot->GetPackGUID();
    //             bot->SendMessageToSet(&data, true);

    //             if (bot->m_movementInfo.HasMovementFlag(MOVEMENTFLAG_FLYING))
    //                 bot->m_movementInfo.RemoveMovementFlag(MOVEMENTFLAG_FLYING);

    //             if (bot->m_movementInfo.HasMovementFlag(MOVEMENTFLAG_DISABLE_GRAVITY))
    //                 bot->m_movementInfo.RemoveMovementFlag(MOVEMENTFLAG_DISABLE_GRAVITY);
    //         }
    //     }

    //     bot->GetMotionMaster()->MovePoint(movePosition.GetMapId(), Position(movePosition.GetPositionX(), movePosition.GetPositionY(),
    //     movePosition.GetPositionZ(), 0.f)); WaitForReach(startPosition.distance(movePosition)); LOG_DEBUG("playerbots",
    //     "Movepoint to ({}, {})", movePosition.GetPositionX(), movePosition.GetPositionY());
    // }

    // AI_VALUE(LastMovement&, "last movement").setShort(movePosition);

    // if (!idle)
    //     ClearIdleState();

    // LOG_DEBUG("playerbots", "return true in the end");
    // return true;
}

bool MovementAction::MoveTo(WorldObject* target, float distance, MovementPriority priority)
{
    if (!IsMovingAllowed(target))
        return false;

    float bx = bot->GetPositionX();
    float by = bot->GetPositionY();
    float bz = bot->GetPositionZ();

    float tz = target->GetPositionZ();

    float distanceToTarget = bot->GetDistance(target);
    float angle = bot->GetAngle(target);
    float needToGo = distanceToTarget - distance;

    float maxDistance = sPlayerbotAIConfig.SpellDistance;
    if (needToGo > 0 && needToGo > maxDistance)
        needToGo = maxDistance;
    else if (needToGo < 0 && needToGo < -maxDistance)
        needToGo = -maxDistance;

    float dx = cos(angle) * needToGo + bx;
    float dy = sin(angle) * needToGo + by;
    // Start from a seed Z between bot and target, then clamp to the
    // terrain under (dx,dy). Linear interpolation alone ignores hills
    // between the two units and fed PointMovementGenerator a Z that
    // could be well above/below ground, triggering straight-line
    // fallbacks through walls.
    float dz;
    if (distanceToTarget > CONTACT_DISTANCE)
        dz = bz + (tz - bz) * (needToGo / distanceToTarget);
    else
        dz = tz;
    bot->UpdateAllowedPositionZ(dx, dy, dz);
    return MoveTo(target->GetMapId(), dx, dy, dz, false, false, false, false, priority);
}

bool MovementAction::ReachCombatTo(Unit* target, float distance)
{
    if (!IsMovingAllowed(target))
        return false;

    float tx = target->GetPositionX();
    float ty = target->GetPositionY();
    float tz = target->GetPositionZ();

    float targetOrientation = target->GetOrientation();

    float deltaAngle = Position::NormalizeOrientation(targetOrientation - target->GetAngle(bot));
    if (deltaAngle > M_PI)
        deltaAngle -= 2.0f * M_PI;  // -PI..PI
    // if target is moving forward and moving far away, predict the position
    bool behind = fabs(deltaAngle) > M_PI_2;
    if (target->HasUnitMovementFlag(MOVEMENTFLAG_FORWARD) && behind)
    {
        float predictDis = std::min(3.0f, target->GetObjectSize() * 2);
        tx += cos(target->GetOrientation()) * predictDis;
        ty += sin(target->GetOrientation()) * predictDis;
        if (!target->GetMap()->CheckCollisionAndGetValidCoords(target, target->GetPositionX(), target->GetPositionY(),
                                                               target->GetPositionZ(), tx, ty, tz))
        {
            tx = target->GetPositionX();
            ty = target->GetPositionY();
            tz = target->GetPositionZ();
        }
    }
    float combatDistance = bot->GetCombatReach() + target->GetCombatReach();
    distance += combatDistance;

    if (bot->GetExactDist(tx, ty, tz) <= distance)
        return false;

    PathGenerator path(bot);
    path.CalculatePath(tx, ty, tz, false);
    PathType type = path.GetPathType();
    int typeOk = PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_SHORTCUT;
    if (!(type & typeOk))
        return false;
    float shortenTo = distance;

    // Avoid walking too far when moving towards each other
    float disToGo = bot->GetExactDist(tx, ty, tz) - distance;
    if (disToGo >= 6.0f)
        shortenTo = disToGo / 2 + distance;

    // if (bot->GetExactDist(tx, ty, tz) <= shortenTo)
    //     return false;

    path.ShortenPathUntilDist(G3D::Vector3(tx, ty, tz), shortenTo);
    G3D::Vector3 endPos = path.GetPath().back();
    bool moved = MoveTo(target->GetMapId(), endPos.x, endPos.y, endPos.z, false, false, false, false,
                        MovementPriority::MOVEMENT_COMBAT, true);
    // Only emit on a successful new commit — combat ticks call this
    // many times per second and MoveTo internally suppresses while a
    // prior spline is still playing. Emitting before the suppression
    // check produces per-tick whisper spam.
    if (moved)
        EmitDebugMove("ReachCombatTo", "mmap", endPos.x, endPos.y, endPos.z);
    return moved;
}

float MovementAction::GetFollowAngle()
{
    Player* master = GetMaster();
    Group* group = master ? master->GetGroup() : bot->GetGroup();
    if (!group)
        return 0.0f;

    // Prevent bots with orphaned raid groups from dividing by 0, which freezes the server.
    uint32 memberCount = group->GetMembersCount();
    if (memberCount <= 1)
        return 0.0f;

    uint32 index = 1;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        if (ref->GetSource() == master)
            continue;

        if (ref->GetSource() == bot)
            return 2 * M_PI / (memberCount - 1) * index;

        ++index;
    }

    return 0;
}

bool MovementAction::IsMovingAllowed(WorldObject* target)
{
    if (!target)
        return false;

    if (bot->GetMapId() != target->GetMapId())
        return false;

    // float distance = ServerFacade::instance().GetDistance2d(bot, target);
    // if (!bot->InBattleground() && distance > sPlayerbotAIConfig.ReactDistance)
    //     return false;

    return IsMovingAllowed();
}

bool MovementAction::IsDuplicateMove(float x, float y, float z)
{
    LastMovement& lastMove = *context->GetValue<LastMovement&>("last movement");

    // heuristic 5s
    if (lastMove.msTime + sPlayerbotAIConfig.MaxWaitForMove < getMSTime() ||
        lastMove.lastMoveShort.GetExactDist(x, y, z) > 0.01f)
        return false;

    return true;
}

bool MovementAction::IsWaitingForLastMove(MovementPriority priority)
{
    LastMovement& lastMove = *context->GetValue<LastMovement&>("last movement");

    if (priority > lastMove.priority)
        return false;

    // heuristic 5s
    if (lastMove.lastdelayTime + lastMove.msTime > getMSTime())
        return true;

    return false;
}

bool MovementAction::IsMovingAllowed()
{
    return botAI->CanMove();
}

bool MovementAction::Follow(Unit* target, float distance) { return Follow(target, distance, GetFollowAngle()); }

void MovementAction::UpdateMovementState()
{
    const bool isCurrentlyRestricted =  // see if the bot is currently slowed, rooted, or otherwise unable to move
        bot->HasUnitState(UNIT_STATE_LOST_CONTROL) || bot->IsRooted() || bot->isFrozen() || bot->IsPolymorphed();

    // no update movement flags while movement is current restricted.
    if (!isCurrentlyRestricted && bot->IsAlive())
    {
        // state flags
        const auto master = botAI ? botAI->GetMaster() : nullptr;
        const auto liquidState = bot->GetLiquidData().Status;
        const float gZ = bot->GetMapWaterOrGroundLevel(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        const bool onGroundZ = bot->GetPositionZ() < gZ + 1.f;
        const bool wantsSwim = liquidState == LIQUID_MAP_IN_WATER || liquidState == LIQUID_MAP_UNDER_WATER;
        const bool wantsFly = bot->HasIncreaseMountedFlightSpeedAura() || bot->HasFlyAura();
        const bool canWaterWalk = bot->HasWaterWalkAura();
        const bool isMasterFlying = master ? master->HasUnitMovementFlag(MOVEMENTFLAG_FLYING) : true;
        const bool isMasterSwimming = master ? master->HasUnitMovementFlag(MOVEMENTFLAG_SWIMMING) : true;
        const bool isFlying = bot->HasUnitMovementFlag(MOVEMENTFLAG_FLYING);
        const bool isSwimming = bot->HasUnitMovementFlag(MOVEMENTFLAG_SWIMMING);
        const bool isWaterWalking = bot->HasUnitMovementFlag(MOVEMENTFLAG_WATERWALKING);
        const bool hasGravityDisabled = bot->HasUnitMovementFlag(MOVEMENTFLAG_DISABLE_GRAVITY);
        bool movementFlagsUpdated = false;

        // handle water (fragile logic do not alter without testing every detail, animation and transition)
        if (liquidState != LIQUID_MAP_NO_WATER && !isFlying)
        {
            if (canWaterWalk && !isMasterSwimming && !isWaterWalking)
            {
                bot->SetSwim(false);
                bot->AddUnitMovementFlag(MOVEMENTFLAG_WATERWALKING);
                movementFlagsUpdated = true;
            }
            else if ((!canWaterWalk || isMasterSwimming) && isWaterWalking)
            {
                bot->RemoveUnitMovementFlag(MOVEMENTFLAG_WATERWALKING);
                if (wantsSwim)
                    bot->SetSwim(true);
                movementFlagsUpdated = true;
            }
            else if (!wantsSwim && isSwimming)
            {
                bot->SetSwim(false);
                movementFlagsUpdated = true;
            }
        }

        // reset when not around water while swimming or water walking
        if (liquidState == LIQUID_MAP_NO_WATER && (isSwimming || isWaterWalking))
        {
            bot->SetSwim(false);
            bot->RemoveUnitMovementFlag(MOVEMENTFLAG_WATERWALKING);
            movementFlagsUpdated = true;
        }

        // handle flying
        if (wantsFly && !isFlying && isMasterFlying)
        {
            bot->AddUnitMovementFlag(MOVEMENTFLAG_CAN_FLY);
            bot->AddUnitMovementFlag(MOVEMENTFLAG_DISABLE_GRAVITY);
            bot->AddUnitMovementFlag(MOVEMENTFLAG_FLYING);
            movementFlagsUpdated = true;
        }
        else if (!wantsFly && !isWaterWalking && (isFlying || hasGravityDisabled))
        {
            bot->RemoveUnitMovementFlag(MOVEMENTFLAG_CAN_FLY);
            bot->RemoveUnitMovementFlag(MOVEMENTFLAG_DISABLE_GRAVITY);
            bot->RemoveUnitMovementFlag(MOVEMENTFLAG_FLYING);
            movementFlagsUpdated = true;
        }
        else if (!isMasterFlying && isFlying && onGroundZ)
        {
            bot->RemoveUnitMovementFlag(MOVEMENTFLAG_CAN_FLY);
            bot->RemoveUnitMovementFlag(MOVEMENTFLAG_DISABLE_GRAVITY);
            bot->RemoveUnitMovementFlag(MOVEMENTFLAG_FLYING);
            movementFlagsUpdated = true;
        }

        // detect if movement/CC restrictions have been ended, refresh movement state for animations.
        if (wasMovementRestricted)
            movementFlagsUpdated = true;

        // movement flags should only be updated between state changes, if not it will break certain effects.
        if (movementFlagsUpdated)
            bot->SendMovementFlagUpdate();
    }

    // Save current state for the next check
    wasMovementRestricted = isCurrentlyRestricted;

    // Temporary speed increase in group
    // if (botAI->HasGameClientMaster())
    //     bot->SetSpeedRate(MOVE_RUN, 1.1f);
    // else
    //     bot->SetSpeedRate(MOVE_RUN, 1.0f);

    // check if target is not reachable (from Vmangos)
    // if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE && bot->CanNotReachTarget() &&
    // !bot->InBattleground())
    // {
    //     if (Unit* pTarget = ServerFacade::instance().GetChaseTarget(bot))
    //     {
    //         if (!bot->IsWithinMeleeRange(pTarget) && pTarget->IsCreature())
    //         {
    //             float angle = bot->GetAngle(pTarget);
    //             float distance = 5.0f;
    //             float x = bot->GetPositionX() + cos(angle) * distance;
    //             float y = bot->GetPositionY() + sin(angle) * distance;
    //             float z = bot->GetPositionZ();

    //             z += CONTACT_DISTANCE;
    //             bot->UpdateAllowedPositionZ(x, y, z);

    //             bot->StopMoving();
    //             bot->GetMotionMaster()->Clear();
    //             bot->NearTeleportTo(x, y, z, bot->GetOrientation());
    //             //bot->GetMotionMaster()->MovePoint(bot->GetMapId(), x, y, z, FORCED_MOVEMENT_RUN, false);
    //             return;
    //             /*if (pTarget->IsCreature() && !bot->isMoving() && bot->IsWithinDist(pTarget, 10.0f))
    //             {
    //                 // Cheating to prevent getting stuck because of bad mmaps.
    //                 bot->StopMoving();
    //                 bot->GetMotionMaster()->Clear();
    //                 bot->GetMotionMaster()->MovePoint(bot->GetMapId(), pTarget->GetPosition(), FORCED_MOVEMENT_RUN,
    //                 false); return;
    //             }*/
    //         }
    //     }
    // }

    // if ((bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE ||
    //     bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE) && bot->CanNotReachTarget()
    //     && !bot->InBattleground())
    // {
    //     if (Unit* pTarget = ServerFacade::instance().GetChaseTarget(bot))
    //     {
    //         if (pTarget != botAI->GetGroupLeader())
    //             return;

    //         if (!bot->IsWithinMeleeRange(pTarget))
    //         {
    //             if (!bot->isMoving() && bot->IsWithinDist(pTarget, 10.0f))
    //             {
    //                 // Cheating to prevent getting stuck because of bad mmaps.
    //                 float angle = bot->GetAngle(pTarget);
    //                 float distance = 5.0f;
    //                 float x = bot->GetPositionX() + cos(angle) * distance;
    //                 float y = bot->GetPositionY() + sin(angle) * distance;
    //                 float z = bot->GetPositionZ();

    //                 z += CONTACT_DISTANCE;
    //                 bot->UpdateAllowedPositionZ(x, y, z);

    //                 bot->StopMoving();
    //                 bot->GetMotionMaster()->Clear();
    //                 bot->NearTeleportTo(x, y, z, bot->GetOrientation());
    //                 //bot->GetMotionMaster()->MovePoint(bot->GetMapId(), x, y, z, FORCED_MOVEMENT_RUN, false);
    //                 return;
    //             }
    //         }
    //     }
    // }
}

bool MovementAction::Follow(Unit* target, float distance, float angle)
{
    UpdateMovementState();

    if (!target)
        return false;

    if (!bot->InBattleground() && ServerFacade::instance().IsDistanceLessOrEqualThan(ServerFacade::instance().GetDistance2d(bot, target),
                                                                           sPlayerbotAIConfig.FollowDistance))
    {
        // botAI->TellError("No need to follow");
        return false;
    }

    /*
    if (!bot->InBattleground()
        && ServerFacade::instance().IsDistanceLessOrEqualThan(ServerFacade::instance().GetDistance2d(bot, target->GetPositionX(),
    target->GetPositionY()), sPlayerbotAIConfig.SightDistance)
        && abs(bot->GetPositionZ() - target->GetPositionZ()) >= sPlayerbotAIConfig.SpellDistance &&
    botAI->HasGameClientMaster()
        && (target->GetMapId() && bot->GetMapId() != target->GetMapId()))
    {
        bot->StopMoving();
        bot->GetMotionMaster()->Clear();

        float x = bot->GetPositionX();
        float y = bot->GetPositionY();
        float z = target->GetPositionZ();
        if (target->GetMapId() && bot->GetMapId() != target->GetMapId())
        {
            if ((target->GetMap() && target->GetMap()->IsBattlegroundOrArena()) || (bot->GetMap() &&
    bot->GetMap()->IsBattlegroundOrArena())) return false;

            bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
            bot->TeleportTo(target->GetMapId(), x, y, z, bot->GetOrientation());
        }
        else
        {
            bot->Relocate(x, y, z, bot->GetOrientation());
        }

        AI_VALUE(LastMovement&, "last movement").Set(target);
        ClearIdleState();
        return true;
    }

    if (!IsMovingAllowed(target) && botAI->HasGameClientMaster())
    {
        if ((target->GetMap() && target->GetMap()->IsBattlegroundOrArena()) || (bot->GetMap() &&
    bot->GetMap()->IsBattlegroundOrArena())) return false;

        if (bot->isDead() && botAI->GetMaster()->IsAlive())
        {
            bot->ResurrectPlayer(1.0f, false);
            botAI->TellMasterNoFacing("I live, again!");
        }
        else
            botAI->TellError("I am stuck while following");

        bot->CombatStop(true);
        botAI->TellMasterNoFacing("I will there soon.");
        bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
        bot->TeleportTo(target->GetMapId(), target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(),
    target->GetOrientation()); return false;
    }
    */

    // Move to target corpse if alive.
    if (!target->IsAlive() && bot->IsAlive() && target->GetGUID().IsPlayer())
    {
        Player* pTarget = (Player*)target;

        Corpse* corpse = pTarget->GetCorpse();

        if (corpse)
        {
            WorldPosition botPos(bot);
            WorldPosition cPos(corpse);

            if (botPos.fDist(cPos) > sPlayerbotAIConfig.SpellDistance)
                return MoveTo(cPos.GetMapId(), cPos.GetPositionX(), cPos.GetPositionY(), cPos.GetPositionZ());
        }
    }

    if (ServerFacade::instance().IsDistanceGreaterOrEqualThan(ServerFacade::instance().GetDistance2d(bot, target),
                                                    sPlayerbotAIConfig.SightDistance))
    {
        EmitDebugMove("Follow", "mmap", target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());

        if (target->GetGUID().IsPlayer())
        {
            Player* pTarget = (Player*)target;

            PlayerbotAI* targetBotAI = GET_PLAYERBOT_AI(pTarget);
            if (targetBotAI)  // Try to move to where the bot is going if it is closer and in the same direction.
            {
                WorldPosition botPos(bot);
                WorldPosition tarPos(target);
                WorldPosition longMove =
                    targetBotAI->GetAiObjectContext()->GetValue<WorldPosition>("last long move")->Get();

                if (longMove)
                {
                    float lDist = botPos.fDist(longMove);
                    float tDist = botPos.fDist(tarPos);
                    float ang = botPos.getAngleBetween(tarPos, longMove);
                    if ((lDist * 1.5 < tDist && ang < static_cast<float>(M_PI) / 2) ||
                        target->HasUnitState(UNIT_STATE_IN_FLIGHT))
                    {
                        return MoveTo(longMove.GetMapId(), longMove.GetPositionX(), longMove.GetPositionY(), longMove.GetPositionZ());
                    }
                }
            }
            else
            {
                if (pTarget->HasUnitState(UNIT_STATE_IN_FLIGHT))  // Move to where the player is flying to.
                {
                    TaxiPathNodeList const& tMap =
                        static_cast<FlightPathMovementGenerator*>(pTarget->GetMotionMaster()->top())->GetPath();
                    if (!tMap.empty())
                    {
                        auto tEnd = tMap.back();
                        if (tEnd)
                            return MoveTo(tEnd->mapid, tEnd->x, tEnd->y, tEnd->z);
                    }
                }
            }
        }

        if (!target->HasUnitState(UNIT_STATE_IN_FLIGHT))
            return MoveTo(target, sPlayerbotAIConfig.FollowDistance);
    }

    if (ServerFacade::instance().IsDistanceLessOrEqualThan(ServerFacade::instance().GetDistance2d(bot, target),
                                                 sPlayerbotAIConfig.FollowDistance))
    {
        // botAI->TellError("No need to follow");
        return false;
    }

    if (target->IsFriendlyTo(bot) && bot->IsMounted() && AI_VALUE(GuidVector, "all targets").empty())
        distance += angle;

    if (!bot->InBattleground() && ServerFacade::instance().IsDistanceLessOrEqualThan(ServerFacade::instance().GetDistance2d(bot, target),
                                                                           sPlayerbotAIConfig.FollowDistance))
    {
        // botAI->TellError("No need to follow");
        return false;
    }

    bot->HandleEmoteCommand(0);

    if (bot->IsSitState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    bot->CastStop();

    // AI_VALUE(LastMovement&, "last movement").Set(target);
    ClearIdleState();

    if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
    {
        Unit* currentTarget = ServerFacade::instance().GetChaseTarget(bot);
        if (currentTarget && currentTarget->GetGUID() == target->GetGUID())
            return false;
    }

    if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
        bot->GetMotionMaster()->Clear();

    EmitDebugMove("Follow", "follow", target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
    bot->GetMotionMaster()->MoveFollow(target, distance, angle);
    return true;
}

bool MovementAction::ChaseTo(WorldObject* obj, float distance)
{
    if (!IsMovingAllowed())
    {
        return false;
    }

    if (obj)
        EmitDebugMove("ChaseTo", "chase", obj->GetPositionX(), obj->GetPositionY(), obj->GetPositionZ());

    if (Vehicle* vehicle = bot->GetVehicle())
    {
        VehicleSeatEntry const* seat = vehicle->GetSeatForPassenger(bot);
        if (!seat || !seat->CanControl())
            return false;

        // vehicle->GetMotionMaster()->Clear();
        vehicle->GetBase()->GetMotionMaster()->MoveChase((Unit*)obj, 30.0f);
        return true;
    }

    UpdateMovementState();

    if (!bot->IsStandState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    bot->CastStop();

    // bot->GetMotionMaster()->Clear();
    bot->GetMotionMaster()->MoveChase((Unit*)obj, distance);

    // TODO shouldnt this use "last movement" value?
    WaitForReach(bot->GetExactDist2d(obj) - distance);
    return true;
}

float MovementAction::MoveDelay(float distance, bool backwards)
{
    float speed;
    if (bot->isSwimming())
    {
        speed = backwards ? bot->GetSpeed(MOVE_SWIM_BACK) : bot->GetSpeed(MOVE_SWIM);
    }
    else if (bot->IsFlying())
    {
        speed = backwards ? bot->GetSpeed(MOVE_FLIGHT_BACK) : bot->GetSpeed(MOVE_FLIGHT);
    }
    else
    {
        speed = backwards ? bot->GetSpeed(MOVE_RUN_BACK) : bot->GetSpeed(MOVE_RUN);
    }
    float delay = distance / speed;
    return delay;
}

// TODO should this be removed? (or modified to use "last movement" value?)
void MovementAction::WaitForReach(float distance)
{
    float delay = IN_MILLISECONDS * MoveDelay(distance) + sPlayerbotAIConfig.ReactDelay;

    if (delay > sPlayerbotAIConfig.MaxWaitForMove)
        delay = sPlayerbotAIConfig.MaxWaitForMove;

    // Deliberately NOT clamped to globalCoolDown while a target exists
    // (the reference keeps this disabled too): a lingering grind or
    // combat target would collapse the whole movement commitment to
    // 0.5s, so every re-entry cancels the in-flight spline and
    // re-dispatches — bots visibly jitter back and forward mid-route.
    // Unit* target = *botAI->GetAiObjectContext()->GetValue<Unit*>("current target");
    // Unit* player = *botAI->GetAiObjectContext()->GetValue<Unit*>("enemy player target");
    // if ((player || target) && delay > sPlayerbotAIConfig.GlobalCoolDown)
    //     delay = sPlayerbotAIConfig.GlobalCoolDown;

    if (delay < 0)
        delay = 0;

    botAI->SetNextCheckDelay((uint32)delay);
    if (IsReaction())
        SetDuration((uint32)delay);
}

// similiar to botAI->SetNextCheckDelay() but only stops movement
void MovementAction::SetNextMovementDelay(float delayMillis)
{
    AI_VALUE(LastMovement&, "last movement")
        .Set(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetOrientation(),
             delayMillis, MovementPriority::MOVEMENT_FORCED);
}

bool MovementAction::Flee(Unit* target)
{
    Player* master = GetMaster();
    if (!target)
        target = master;

    if (!target)
        return false;

    EmitDebugMove("Flee", "flee", target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());

    if (!IsMovingAllowed())
    {
        botAI->TellError("I am stuck while fleeing");
        return false;
    }

    bool foundFlee = false;
    time_t lastFlee = AI_VALUE(LastMovement&, "last movement").lastFlee;
    time_t now = time(0);
    uint32 fleeDelay = urand(2, sPlayerbotAIConfig.ReturnDelay / 1000);

    if (lastFlee)
    {
        if ((now - lastFlee) <= fleeDelay)
        {
            return false;
        }
    }

    Unit* currentVictim = target->CanHaveThreatList() ? target->GetThreatMgr().GetCurrentVictim() : target->GetVictim();
    if (currentVictim && currentVictim == bot)  // bot is target - try to flee to tank or master
    {
        if (Group* group = bot->GetGroup())
        {
            Unit* fleeTarget = nullptr;
            float fleeDistance = sPlayerbotAIConfig.SightDistance;

            for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
            {
                Player* player = gref->GetSource();
                if (!player || player == bot || !player->IsAlive())
                    continue;

                if (PlayerbotAI::IsTank(player))
                {
                    float distanceToTank = ServerFacade::instance().GetDistance2d(bot, player);
                    if (distanceToTank < fleeDistance)
                    {
                        fleeTarget = player;
                        fleeDistance = distanceToTank;
                    }
                }
            }

            if (fleeTarget)
                foundFlee = MoveNear(fleeTarget);

            if ((!fleeTarget || !foundFlee) && master)
            {
                foundFlee = MoveNear(master);
            }
        }
    }
    else  // bot is not targeted, try to flee dps/healers
    {
        bool isHealer = PlayerbotAI::IsHeal(bot);
        bool needHealer = !isHealer && AI_VALUE2(uint8, "health", "self target") < 50;
        bool isRanged = PlayerbotAI::IsRanged(bot);

        Group* group = bot->GetGroup();
        if (group)
        {
            Unit* fleeTarget = nullptr;
            float fleeDistance = botAI->GetRange("shoot") * 1.5f;
            Unit* spareTarget = nullptr;
            float spareDistance = botAI->GetRange("shoot") * 2.0f;
            std::vector<Unit*> possibleTargets;

            for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
            {
                Player* player = gref->GetSource();
                if (!player || player == bot || !player->IsAlive())
                    continue;

                if ((isHealer && PlayerbotAI::IsHeal(player)) || needHealer)
                {
                    float distanceToHealer = ServerFacade::instance().GetDistance2d(bot, player);
                    float distanceToTarget = ServerFacade::instance().GetDistance2d(player, target);
                    if (distanceToHealer < fleeDistance &&
                        distanceToTarget > (botAI->GetRange("shoot") / 2 + sPlayerbotAIConfig.FollowDistance) &&
                        (needHealer || player->IsWithinLOSInMap(target)))
                    {
                        fleeTarget = player;
                        fleeDistance = distanceToHealer;
                        possibleTargets.push_back(fleeTarget);
                    }
                }
                else if (isRanged && PlayerbotAI::IsRanged(player))
                {
                    float distanceToRanged = ServerFacade::instance().GetDistance2d(bot, player);
                    float distanceToTarget = ServerFacade::instance().GetDistance2d(player, target);
                    if (distanceToRanged < fleeDistance &&
                        distanceToTarget > (botAI->GetRange("shoot") / 2 + sPlayerbotAIConfig.FollowDistance) &&
                        player->IsWithinLOSInMap(target))
                    {
                        fleeTarget = player;
                        fleeDistance = distanceToRanged;
                        possibleTargets.push_back(fleeTarget);
                    }
                }
                // remember any group member in case no one else found
                float distanceToFlee = ServerFacade::instance().GetDistance2d(bot, player);
                float distanceToTarget = ServerFacade::instance().GetDistance2d(player, target);
                if (distanceToFlee < spareDistance &&
                    distanceToTarget > (botAI->GetRange("shoot") / 2 + sPlayerbotAIConfig.FollowDistance) &&
                    player->IsWithinLOSInMap(target))
                {
                    spareTarget = player;
                    spareDistance = distanceToFlee;
                    possibleTargets.push_back(fleeTarget);
                }
            }

            if (!possibleTargets.empty())
                fleeTarget = possibleTargets[urand(0, possibleTargets.size() - 1)];

            if (!fleeTarget)
                fleeTarget = spareTarget;

            if (fleeTarget)
                foundFlee = MoveNear(fleeTarget);

            if ((!fleeTarget || !foundFlee) && master && master->IsAlive() && master->IsWithinLOSInMap(target))
            {
                float distanceToTarget = ServerFacade::instance().GetDistance2d(master, target);
                if (distanceToTarget > (botAI->GetRange("shoot") / 2 + sPlayerbotAIConfig.FollowDistance))
                    foundFlee = MoveNear(master);
            }
        }
    }

    if ((foundFlee || lastFlee) && bot->GetGroup())
    {
        if (!lastFlee)
            AI_VALUE(LastMovement&, "last movement").lastFlee = now;
        else
            AI_VALUE(LastMovement&, "last movement").lastFlee = 0;
    }

    FleeManager manager(bot, botAI->GetRange("flee"), bot->GetAngle(target) + M_PI);
    if (!manager.isUseful())
        return false;

    float rx, ry, rz;
    if (!manager.CalculateDestination(&rx, &ry, &rz))
    {
        botAI->TellError("Nowhere to flee");
        return false;
    }

    bool result = MoveTo(target->GetMapId(), rx, ry, rz);

    if (result)
        AI_VALUE(LastMovement&, "last movement").lastFlee = time(nullptr);

    return result;
}

void MovementAction::ClearIdleState()
{
    context->GetValue<time_t>("stay time")->Set(0);
    context->GetValue<PositionMap&>("position")->Get()["random"].Reset();
}

bool MovementAction::MoveAway(Unit* target, float distance, bool backwards)
{
    if (!target)
    {
        return false;
    }
    EmitDebugMove("MoveAway", "mmap", target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
    float init_angle = target->GetAngle(bot);
    for (float delta = 0; delta <= M_PI / 2; delta += M_PI / 8)
    {
        float angle = init_angle + delta;
        float dx = bot->GetPositionX() + cos(angle) * distance;
        float dy = bot->GetPositionY() + sin(angle) * distance;
        float dz = bot->GetPositionZ();
        bool exact = true;
        if (!bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                            bot->GetPositionZ(), dx, dy, dz))
        {
            // disable prediction if position is invalid
            dx = bot->GetPositionX() + cos(angle) * distance;
            dy = bot->GetPositionY() + sin(angle) * distance;
            dz = bot->GetPositionZ();
            exact = false;
        }
        if (MoveTo(target->GetMapId(), dx, dy, dz, false, false, true, exact, MovementPriority::MOVEMENT_COMBAT, false,
                   backwards))
        {
            return true;
        }
        if (delta == 0)
        {
            continue;
        }
        exact = true;
        angle = init_angle - delta;
        dx = bot->GetPositionX() + cos(angle) * distance;
        dy = bot->GetPositionY() + sin(angle) * distance;
        dz = bot->GetPositionZ();
        if (!bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                            bot->GetPositionZ(), dx, dy, dz))
        {
            // disable prediction if position is invalid
            dx = bot->GetPositionX() + cos(angle) * distance;
            dy = bot->GetPositionY() + sin(angle) * distance;
            dz = bot->GetPositionZ();
            exact = false;
        }
        if (MoveTo(target->GetMapId(), dx, dy, dz, false, false, true, exact, MovementPriority::MOVEMENT_COMBAT, false,
                   backwards))
        {
            return true;
        }
    }
    return false;
}

// just calculates average position of group and runs away from that position
bool MovementAction::MoveFromGroup(float distance)
{
    if (Group* group = bot->GetGroup())
    {
        uint32 mapId = bot->GetMapId();
        float closestDist = FLT_MAX;
        float x = 0.0f;
        float y = 0.0f;
        uint32 count = 0;

        for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
        {
            Player* player = gref->GetSource();
            if (!player || player == bot || !player->IsAlive() || player->GetMapId() != mapId)
                continue;
            float dist = bot->GetDistance2d(player);
            if (closestDist > dist)
                closestDist = dist;
            x += player->GetPositionX();
            y += player->GetPositionY();
            count++;
        }

        if (count && closestDist < distance)
        {
            x /= count;
            y /= count;
            // x and y are now average position of the group members
            float angle = bot->GetAngle(x, y) + M_PI;
            EmitDebugMove("MoveFromGroup", "mmap", x, y, bot->GetPositionZ());
            return Move(angle, distance - closestDist);
        }
    }
    return false;
}

bool MovementAction::Move(float angle, float distance)
{
    float x = bot->GetPositionX() + cos(angle) * distance;
    float y = bot->GetPositionY() + sin(angle) * distance;

    // TODO do we need GetMapWaterOrGroundLevel() if we're using CheckCollisionAndGetValidCoords() ?
    float z = bot->GetMapWaterOrGroundLevel(x, y, bot->GetPositionZ());
    if (z == -100000.0f || z == -200000.0f)
        z = bot->GetPositionZ();
    if (!bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                        bot->GetPositionZ(), x, y, z, false))
        return false;

    return MoveTo(bot->GetMapId(), x, y, z);
}

bool MovementAction::GetRandomPointInCone(Position const& origin, float orientation, float halfArc, float minDistance,
                                          float maxDistance, Position& out, float maxBelow, float maxAbove,
                                          uint8 attempts)
{
    constexpr float maxEndOffset = 2.0f;
    constexpr float eyeHeight = 2.0f;

    float const originX = origin.GetPositionX();
    float const originY = origin.GetPositionY();
    float const originZ = origin.GetPositionZ();
    float originGroundZ = originZ;
    bot->UpdateGroundPositionZ(originX, originY, originGroundZ);

    for (uint8 i = 0; i < attempts; ++i)
    {
        float const angle = Position::NormalizeOrientation(orientation + frand(-halfArc, halfArc));
        float const distance = frand(minDistance, maxDistance);
        float const x = originX + distance * std::cos(angle);
        float const y = originY + distance * std::sin(angle);
        float z = originZ;
        bot->UpdateGroundPositionZ(x, y, z);

        float const dz = z - originGroundZ;
        if (dz < -maxBelow || dz > maxAbove)
            continue;

        if (!bot->GetMap()->isInLineOfSight(originX, originY, originZ + eyeHeight, x, y, z + eyeHeight,
                                            bot->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS,
                                            VMAP::ModelIgnoreFlags::Nothing))
            continue;

        PathGenerator path(bot);
        path.CalculatePath(originX, originY, originZ, x, y, z, false);
        PathType const type = path.GetPathType();
        if (!(type & PATHFIND_NORMAL) ||
            (type & (PATHFIND_NOT_USING_PATH | PATHFIND_NOPATH | PATHFIND_SHORT | PATHFIND_INCOMPLETE)))
            continue;

        G3D::Vector3 const& end = path.GetActualEndPosition();
        if (std::hypot(end.x - x, end.y - y, end.z - z) > maxEndOffset)
            continue;

        out.Relocate(x, y, z);
        return true;
    }

    return false;
}

bool MovementAction::MoveInside(uint32 mapId, float x, float y, float z, float distance, MovementPriority priority)
{
    if (bot->GetDistance2d(x, y) <= distance)
    {
        return false;
    }
    EmitDebugMove("MoveInside", "mmap", x, y, z);
    return MoveNear(mapId, x, y, z, distance, priority);
}

// float MovementAction::SearchBestGroundZForPath(float x, float y, float z, bool generatePath, float range, bool
// normal_only, float step)
// {
//     if (!generatePath)
//     {
//         return z;
//     }
//     float min_length = 100000.0f;
//     float current_z = INVALID_HEIGHT;
//     float modified_z;
//     float delta;
//     for (delta = 0.0f; delta <= range / 2; delta += step) {
//         modified_z = bot->GetMapWaterOrGroundLevel(x, y, z + delta);
//         PathGenerator gen(bot);
//         gen.CalculatePath(x, y, modified_z);
//         if (gen.GetPathType() == PATHFIND_NORMAL && gen.getPathLength() < min_length)
//         {
//             min_length = gen.getPathLength();
//             current_z = modified_z;
//             if (abs(current_z - z) < 0.5f)
//             {
//                 return current_z;
//             }
//         }
//     }
//     for (delta = -step; delta >= -range / 2; delta -= step) {
//         modified_z = bot->GetMapWaterOrGroundLevel(x, y, z + delta);
//         PathGenerator gen(bot);
//         gen.CalculatePath(x, y, modified_z);
//         if (gen.GetPathType() == PATHFIND_NORMAL && gen.getPathLength() < min_length)
//         {
//             min_length = gen.getPathLength();
//             current_z = modified_z;
//             if (abs(current_z - z) < 0.5f)
//                 return current_z;
//         }
//     }
//     for (delta = range / 2 + step; delta <= range; delta += 2) {
//         modified_z = bot->GetMapWaterOrGroundLevel(x, y, z + delta);
//         PathGenerator gen(bot);
//         gen.CalculatePath(x, y, modified_z);
//         if (gen.GetPathType() == PATHFIND_NORMAL && gen.getPathLength() < min_length)
//         {
//             min_length = gen.getPathLength();
//             current_z = modified_z;
//             if (abs(current_z - z) < 0.5f)
//             {
//                 return current_z;
//             }
//         }
//     }
//     if (current_z == INVALID_HEIGHT && normal_only)
//     {
//         return INVALID_HEIGHT;
//     }
//     if (current_z == INVALID_HEIGHT && !normal_only)
//     {
//         return z;
//     }
//     return current_z;
// }

PathResult MovementAction::GeneratePath(float x, float y, float z, uint32 acceptMask, bool forceDestination)
{
    PathResult result;
    PathGenerator gen(bot);
    gen.CalculatePath(x, y, z, forceDestination);
    result.pathType = gen.GetPathType();
    result.reachable = !(result.pathType & (~acceptMask));
    result.points = gen.GetPath();
    result.actualEnd = gen.GetActualEndPosition();
    result.end = gen.GetEndPosition();
    return result;
}

void MovementAction::DoMovePoint(Unit* unit, float x, float y, float z, bool generatePath, bool backwards)
{
    if (!unit)
        return;

    MotionMaster* mm = unit->GetMotionMaster();
    if (!mm)
        return;

    // bot water collision correction
    if (unit->HasUnitMovementFlag(MOVEMENTFLAG_WATERWALKING) && unit->HasWaterWalkAura())
    {
        float gZ = unit->GetMapWaterOrGroundLevel(unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ());
        unit->UpdatePosition(unit->GetPositionX(), unit->GetPositionY(), gZ, false);
    }

    mm->Clear();
    if (backwards)
    {
        mm->MovePointBackwards(
            /*id*/ 0,
            /*coords*/ x, y, z,
            /*generatePath*/ generatePath,
            /*forceDestination*/ false);
        return;
    }
    else
    {
        mm->MovePoint(
            /*id*/ 0,
            /*coords*/ x, y, z,
            /*forcedMovement*/ FORCED_MOVEMENT_NONE,
            /*speed*/ 0.f,
            /*orientation*/ 0.f,
            /*generatePath*/ generatePath,  // true => terrain path (2d mmap); false => straight spline (3d vmap)
            /*forceDestination*/ false);
    }
}

bool FleeAction::Execute(Event /*event*/)
{
    return MoveAway(AI_VALUE(Unit*, "current target"), sPlayerbotAIConfig.FleeDistance, true);
}

bool FleeAction::isUseful()
{
    if (bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) != nullptr)
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");
    if (target && target->IsInWorld() && !bot->IsWithinMeleeRange(target))
        return false;

    return true;
}

bool FleeWithPetAction::Execute(Event /*event*/)
{
    if (bot->GetPet())
        botAI->PetFollow();

    return Flee(AI_VALUE(Unit*, "current target"));
}

bool AvoidAoeAction::isUseful()
{
    if (getMSTime() - moveInterval < uint32(lastMoveTimer))
        return false;

    GuidVector traps = AI_VALUE(GuidVector, "nearest trap with damage");
    GuidVector triggers = AI_VALUE(GuidVector, "possible triggers");
    return AI_VALUE(Aura*, "area debuff") || !traps.empty() || !triggers.empty();
}

bool AvoidAoeAction::Execute(Event /*event*/)
{
    // Case #1: Aura with dynamic object (e.g. rain of fire)
    if (AvoidAuraWithDynamicObj())
    {
        return true;
    }
    // Case #2: Trap game object with spell (e.g. lava bomb)
    if (AvoidGameObjectWithDamage())
    {
        return true;
    }
    // Case #3: Trigger npc (e.g. Lesser shadow fissure)
    if (AvoidUnitWithDamageAura())
    {
        return true;
    }
    return false;
}

bool AvoidAoeAction::AvoidAuraWithDynamicObj()
{
    Aura* aura = AI_VALUE(Aura*, "area debuff");
    if (!aura || aura->IsRemoved() || aura->IsExpired())
    {
        return false;
    }
    if (!aura->GetOwner() || !aura->GetOwner()->IsInWorld())
    {
        return false;
    }
    // Crash fix: maybe change owner due to check interval
    if (aura->GetType() != DYNOBJ_AURA_TYPE)
    {
        return false;
    }
    SpellInfo const* spellInfo = aura->GetSpellInfo();
    if (!spellInfo)
    {
        return false;
    }
    if (sPlayerbotAIConfig.AoeAvoidSpellWhitelist.find(spellInfo->Id) !=
        sPlayerbotAIConfig.AoeAvoidSpellWhitelist.end())
        return false;

    DynamicObject* dynOwner = aura->GetDynobjOwner();
    if (!dynOwner || !dynOwner->IsInWorld())
    {
        return false;
    }
    float radius = dynOwner->GetRadius();
    if (!radius || radius > sPlayerbotAIConfig.MaxAoeAvoidRadius)
        return false;
    if (bot->GetDistance(dynOwner) > radius)
    {
        return false;
    }
    std::ostringstream name;
    name << spellInfo->SpellName[LOCALE_enUS];  // << "] (aura)";
    if (FleePosition(dynOwner->GetPosition(), radius))
    {
        if (sPlayerbotAIConfig.TellWhenAvoidAoe && lastTellTimer < time(NULL) - 10)
        {
            lastTellTimer = time(NULL);
            lastMoveTimer = getMSTime();
            std::ostringstream out;
            out << "I'm avoiding " << name.str() << " (" << spellInfo->Id << ")" << " Radius " << radius << " - [Aura]";
            bot->Say(out.str(), LANG_UNIVERSAL);
        }
        return true;
    }
    return false;
}

bool AvoidAoeAction::AvoidGameObjectWithDamage()
{
    GuidVector traps = AI_VALUE(GuidVector, "nearest trap with damage");
    if (traps.empty())
    {
        return false;
    }
    for (ObjectGuid& guid : traps)
    {
        GameObject* go = botAI->GetGameObject(guid);
        if (!go || !go->IsInWorld())
        {
            continue;
        }
        if (go->GetGoType() != GAMEOBJECT_TYPE_TRAP)
        {
            continue;
        }
        GameObjectTemplate const* goInfo = go->GetGOInfo();
        if (!goInfo)
        {
            continue;
        }
        // 0 trap with no despawn after cast. 1 trap despawns after cast. 2 bomb casts on spawn.
        if (goInfo->trap.type != 0)
            continue;

        uint32 spellId = goInfo->trap.spellId;
        if (!spellId)
        {
            continue;
        }

        if (sPlayerbotAIConfig.AoeAvoidSpellWhitelist.find(spellId) !=
            sPlayerbotAIConfig.AoeAvoidSpellWhitelist.end())
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo || spellInfo->IsPositive())
        {
            continue;
        }

        float radius = (float)goInfo->trap.diameter / 2 + go->GetCombatReach();
        if (!radius || radius > sPlayerbotAIConfig.MaxAoeAvoidRadius)
            continue;

        if (bot->GetDistance(go) > radius)
        {
            continue;
        }
        std::ostringstream name;
        name << spellInfo->SpellName[LOCALE_enUS];  // << "] (object)";
        if (FleePosition(go->GetPosition(), radius))
        {
            if (sPlayerbotAIConfig.TellWhenAvoidAoe && lastTellTimer < time(NULL) - 10)
            {
                lastTellTimer = time(NULL);
                lastMoveTimer = getMSTime();
                std::ostringstream out;
                out << "I'm avoiding " << name.str() << " (" << spellInfo->Id << ")" << " Radius " << radius
                    << " - [Trap]";
                bot->Say(out.str(), LANG_UNIVERSAL);
            }
            return true;
        }
    }
    return false;
}

bool AvoidAoeAction::AvoidUnitWithDamageAura()
{
    GuidVector triggers = AI_VALUE(GuidVector, "possible triggers");
    if (triggers.empty())
    {
        return false;
    }
    for (ObjectGuid& guid : triggers)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsInWorld())
        {
            continue;
        }
        if (!unit->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
        {
            return false;
        }
        Unit::AuraEffectList const& aurasPeriodicTriggerSpell =
            unit->GetAuraEffectsByType(SPELL_AURA_PERIODIC_TRIGGER_SPELL);
        Unit::AuraEffectList const& aurasPeriodicTriggerWithValueSpell =
            unit->GetAuraEffectsByType(SPELL_AURA_PERIODIC_TRIGGER_SPELL_WITH_VALUE);
        for (Unit::AuraEffectList const& list : {aurasPeriodicTriggerSpell, aurasPeriodicTriggerWithValueSpell})
        {
            for (auto i = list.begin(); i != list.end(); ++i)
            {
                AuraEffect* aurEff = *i;
                SpellInfo const* spellInfo = aurEff->GetSpellInfo();
                if (!spellInfo)
                    continue;
                SpellInfo const* triggerSpellInfo =
                    sSpellMgr->GetSpellInfo(spellInfo->Effects[aurEff->GetEffIndex()].TriggerSpell);
                if (!triggerSpellInfo)
                    continue;
                if (sPlayerbotAIConfig.AoeAvoidSpellWhitelist.find(triggerSpellInfo->Id) !=
                    sPlayerbotAIConfig.AoeAvoidSpellWhitelist.end())
                    return false;
                for (int j = 0; j < MAX_SPELL_EFFECTS; j++)
                {
                    if (triggerSpellInfo->Effects[j].Effect == SPELL_EFFECT_SCHOOL_DAMAGE)
                    {
                        float radius = triggerSpellInfo->Effects[j].CalcRadius();
                        if (bot->GetDistance(unit) > radius)
                        {
                            break;
                        }
                        if (!radius || radius > sPlayerbotAIConfig.MaxAoeAvoidRadius)
                            continue;
                        std::ostringstream name;
                        name << triggerSpellInfo->SpellName[LOCALE_enUS];  //<< "] (unit)";
                        if (FleePosition(unit->GetPosition(), radius))
                        {
                            if (sPlayerbotAIConfig.TellWhenAvoidAoe && lastTellTimer < time(NULL) - 10)
                            {
                                lastTellTimer = time(NULL);
                                lastMoveTimer = getMSTime();
                                std::ostringstream out;
                                out << "I'm avoiding " << name.str() << " (" << triggerSpellInfo->Id << ")"
                                    << " Radius " << radius << " - [Unit Trigger]";
                                bot->Say(out.str(), LANG_UNIVERSAL);
                            }
                        }
                    }
                }
            }
        }
    }
    return false;
}

Position MovementAction::BestPositionForMeleeToFlee(Position pos, float radius)
{
    Unit* currentTarget = AI_VALUE(Unit*, "current target");
    std::vector<CheckAngle> possibleAngles;
    if (currentTarget)
    {
        // Normally, move to left or right is the best position
        bool isTanking = (!currentTarget->isFrozen()
            && !currentTarget->HasRootAura()) && (currentTarget->GetVictim() == bot);
        float angle = bot->GetAngle(currentTarget);
        float angleLeft = angle + (float)M_PI / 2;
        float angleRight = angle - (float)M_PI / 2;
        possibleAngles.push_back({angleLeft, false});
        possibleAngles.push_back({angleRight, false});
        possibleAngles.push_back({angle, true});
        if (isTanking)
        {
            possibleAngles.push_back({angle + (float)M_PI, false});
            possibleAngles.push_back({bot->GetAngle(&pos) - (float)M_PI, false});
        }
    }
    else
    {
        float angleTo = bot->GetAngle(&pos) - (float)M_PI;
        possibleAngles.push_back({angleTo, false});
    }
    float farestDis = 0.0f;
    Position bestPos;
    for (CheckAngle& checkAngle : possibleAngles)
    {
        float angle = checkAngle.angle;
        std::list<FleeInfo>& infoList = AI_VALUE(std::list<FleeInfo>&, "recently flee info");
        if (!CheckLastFlee(angle, infoList))
        {
            continue;
        }
        bool strict = checkAngle.strict;
        float fleeDis = std::min(radius + 1.0f, sPlayerbotAIConfig.FleeDistance);
        float dx = bot->GetPositionX() + cos(angle) * fleeDis;
        float dy = bot->GetPositionY() + sin(angle) * fleeDis;
        float dz = bot->GetPositionZ();
        if (!bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                            bot->GetPositionZ(), dx, dy, dz))
        {
            continue;
        }
        Position fleePos{dx, dy, dz};
        if (strict && currentTarget &&
            fleePos.GetExactDist(currentTarget) - currentTarget->GetCombatReach() >
                sPlayerbotAIConfig.TooCloseDistance &&
            bot->IsWithinMeleeRange(currentTarget))
        {
            continue;
        }
        if (pos.GetExactDist(fleePos) > farestDis)
        {
            farestDis = pos.GetExactDist(fleePos);
            bestPos = fleePos;
        }
    }
    if (farestDis > 0.0f)
    {
        return bestPos;
    }
    return Position();
}

Position MovementAction::BestPositionForRangedToFlee(Position pos, float radius)
{
    Unit* currentTarget = AI_VALUE(Unit*, "current target");
    std::vector<CheckAngle> possibleAngles;
    float angleToTarget = 0.0f;
    float angleFleeFromCenter = bot->GetAngle(&pos) - (float)M_PI;
    if (currentTarget)
    {
        // Normally, move to left or right is the best position
        angleToTarget = bot->GetAngle(currentTarget);
        float angleLeft = angleToTarget + (float)M_PI / 2;
        float angleRight = angleToTarget - (float)M_PI / 2;
        possibleAngles.push_back({angleLeft, false});
        possibleAngles.push_back({angleRight, false});
        possibleAngles.push_back({angleToTarget + (float)M_PI, true});
        possibleAngles.push_back({angleToTarget, true});
        possibleAngles.push_back({angleFleeFromCenter, true});
    }
    else
    {
        possibleAngles.push_back({angleFleeFromCenter, false});
    }
    float farestDis = 0.0f;
    Position bestPos;
    for (CheckAngle& checkAngle : possibleAngles)
    {
        float angle = checkAngle.angle;
        std::list<FleeInfo>& infoList = AI_VALUE(std::list<FleeInfo>&, "recently flee info");
        if (!CheckLastFlee(angle, infoList))
        {
            continue;
        }
        bool strict = checkAngle.strict;
        float fleeDis = std::min(radius + 1.0f, sPlayerbotAIConfig.FleeDistance);
        float dx = bot->GetPositionX() + cos(angle) * fleeDis;
        float dy = bot->GetPositionY() + sin(angle) * fleeDis;
        float dz = bot->GetPositionZ();
        if (!bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                            bot->GetPositionZ(), dx, dy, dz))
        {
            continue;
        }
        Position fleePos{dx, dy, dz};
        if (strict && currentTarget &&
            fleePos.GetExactDist(currentTarget) - currentTarget->GetCombatReach() > sPlayerbotAIConfig.SpellDistance)
        {
            continue;
        }
        if (strict && currentTarget &&
            fleePos.GetExactDist(currentTarget) - currentTarget->GetCombatReach() <
                (sPlayerbotAIConfig.TooCloseDistance))
        {
            continue;
        }

        if (pos.GetExactDist(fleePos) > farestDis)
        {
            farestDis = pos.GetExactDist(fleePos);
            bestPos = fleePos;
        }
    }
    if (farestDis > 0.0f)
    {
        return bestPos;
    }
    return Position();
}

bool MovementAction::FleePosition(Position pos, float radius, uint32 minInterval)
{
    std::list<FleeInfo>& infoList = AI_VALUE(std::list<FleeInfo>&, "recently flee info");

    if (!infoList.empty() && infoList.back().timestamp + minInterval > getMSTime())
        return false;

    Position bestPos;
    if (PlayerbotAI::IsMelee(bot))
    {
        bestPos = BestPositionForMeleeToFlee(pos, radius);
    }
    else
    {
        bestPos = BestPositionForRangedToFlee(pos, radius);
    }
    if (bestPos != Position())
    {
        EmitDebugMove("FleePosition", "mmap", bestPos.GetPositionX(), bestPos.GetPositionY(), bestPos.GetPositionZ());
        if (MoveTo(bot->GetMapId(), bestPos.GetPositionX(), bestPos.GetPositionY(), bestPos.GetPositionZ(), false,
                   false, true, false, MovementPriority::MOVEMENT_COMBAT))
        {
            uint32 curTS = getMSTime();
            while (!infoList.empty())
            {
                if (infoList.size() > 10 || infoList.front().timestamp + 5000 < curTS)
                {
                    infoList.pop_front();
                }
                else
                {
                    break;
                }
            }
            infoList.push_back({pos, radius, bot->GetAngle(&bestPos), curTS});
            return true;
        }
    }
    return false;
}

bool MovementAction::CheckLastFlee(float curAngle, std::list<FleeInfo>& infoList)
{
    uint32 curTS = getMSTime();
    curAngle = Position::NormalizeOrientation(curAngle);
    while (!infoList.empty())
    {
        if (infoList.size() > 10 || infoList.front().timestamp + 5000 < curTS)
        {
            infoList.pop_front();
        }
        else
        {
            break;
        }
    }
    for (FleeInfo& info : infoList)
    {
        // more than 5 sec
        if (info.timestamp + 5000 < curTS)
        {
            continue;
        }
        float revAngle = Position::NormalizeOrientation(info.angle + M_PI);
        // angle too close
        if (fabs(revAngle - curAngle) < M_PI / 4)
        {
            return false;
        }
    }
    return true;
}

bool CombatFormationMoveAction::isUseful()
{
    if (getMSTime() - moveInterval < uint32(lastMoveTimer))
        return false;

    if (bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) != nullptr)
        return false;

    return true;
}

bool CombatFormationMoveAction::Execute(Event /*event*/)
{
    float dis = AI_VALUE(float, "disperse distance");
    if (dis <= 0.0f || (!bot->IsInCombat() && botAI->HasStrategy("stay", BotState::BOT_STATE_NON_COMBAT)) ||
        (bot->IsInCombat() && botAI->HasStrategy("stay", BotState::BOT_STATE_COMBAT)))
        return false;
    Player* playerToLeave = NearestGroupMember(dis);
    if (playerToLeave && bot->GetExactDist(playerToLeave) < dis)
    {
        if (FleePosition(playerToLeave->GetPosition(), dis))
        {
            lastMoveTimer = getMSTime();
        }
    }
    return false;
}

Position CombatFormationMoveAction::AverageGroupPos(float dis, bool ranged, bool self)
{
    float averageX = 0, averageY = 0, averageZ = 0;
    int cnt = 0;
    Group* group = bot->GetGroup();
    if (!group)
    {
        return Position();
    }
    Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
    for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
    {
        Player* member = ObjectAccessor::FindPlayer(itr->guid);
        if (!member)
            continue;

        if (!self && member == bot)
            continue;

        if (ranged && !PlayerbotAI::IsRanged(member))
            continue;

        if (!member->IsAlive() || member->GetMapId() != bot->GetMapId() || member->IsCharmed() ||
            ServerFacade::instance().GetDistance2d(bot, member) > dis)
            continue;

        averageX += member->GetPositionX();
        averageY += member->GetPositionY();
        averageZ += member->GetPositionZ();
    }
    averageX /= cnt;
    averageY /= cnt;
    averageZ /= cnt;
    return Position(averageX, averageY, averageZ);
}

float CombatFormationMoveAction::AverageGroupAngle(Unit* from, bool ranged, bool self)
{
    Group* group = bot->GetGroup();
    if (!from || !group)
    {
        return 0.0f;
    }
    // float average = 0.0f;
    float sumX = 0.0f;
    float sumY = 0.0f;
    int cnt = 0;
    Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
    for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
    {
        Player* member = ObjectAccessor::FindPlayer(itr->guid);
        if (!member)
            continue;

        if (!self && member == bot)
            continue;

        if (ranged && !PlayerbotAI::IsRanged(member))
            continue;

        if (!member->IsAlive() || member->GetMapId() != bot->GetMapId() || member->IsCharmed() ||
            ServerFacade::instance().GetDistance2d(bot, member) > sPlayerbotAIConfig.SightDistance)
            continue;

        cnt++;
        sumX += member->GetPositionX() - from->GetPositionX();
        sumY += member->GetPositionY() - from->GetPositionY();
    }
    if (cnt == 0)
        return 0.0f;

    // unnecessary division
    // sumX /= cnt;
    // sumY /= cnt;

    return atan2(sumY, sumX);
}

Position CombatFormationMoveAction::GetNearestPosition(std::vector<Position> const& positions)
{
    Position result;
    for (Position const& pos : positions)
    {
        if (bot->GetExactDist(pos) < bot->GetExactDist(result))
            result = pos;
    }
    return result;
}

Player* CombatFormationMoveAction::NearestGroupMember(float dis)
{
    float nearestDis = 10000.0f;
    Player* result = nullptr;
    Group* group = bot->GetGroup();
    if (!group)
    {
        return result;
    }
    Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
    for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
    {
        Player* member = ObjectAccessor::FindPlayer(itr->guid);
        if (!member || !member->IsAlive() || member == bot || member->GetMapId() != bot->GetMapId() ||
            member->IsCharmed() || ServerFacade::instance().GetDistance2d(bot, member) > dis)
            continue;
        if (nearestDis > bot->GetExactDist(member))
        {
            result = member;
            nearestDis = bot->GetExactDist(member);
        }
    }
    return result;
}

bool TankFaceAction::Execute(Event /*event*/)
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;

    if (!bot->GetGroup())
        return false;

    if (!bot->IsWithinMeleeRange(target) || target->isMoving())
        return false;

    if (!AI_VALUE2(bool, "has aggro", "current target"))
        return false;

    float averageAngle = AverageGroupAngle(target, true);

    if (averageAngle == 0.0f)
        return false;

    float deltaAngle = Position::NormalizeOrientation(averageAngle - target->GetAngle(bot));
    if (deltaAngle > M_PI)
        deltaAngle -= 2.0f * M_PI;  // -PI..PI

    float tolerable = M_PI_2;

    if (fabs(deltaAngle) > tolerable)
        return false;

    float goodAngle1 = Position::NormalizeOrientation(averageAngle + M_PI * 3 / 5);
    float goodAngle2 = Position::NormalizeOrientation(averageAngle - M_PI * 3 / 5);

    // if dist < bot->GetMeleeRange(target) / 2, target will move backward
    float dist = std::max(bot->GetExactDist(target), bot->GetMeleeRange(target) / 2) - bot->GetCombatReach() -
                 target->GetCombatReach();
    std::vector<Position> availablePos;
    float x, y, z;
    target->GetNearPoint(bot, x, y, z, 0.0f, dist, goodAngle1);
    if (bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                       bot->GetPositionZ(), x, y, z))
    {
        /// @todo: movement control now is a mess, prepare to rewrite
        std::list<FleeInfo>& infoList = AI_VALUE(std::list<FleeInfo>&, "recently flee info");
        Position pos(x, y, z);
        float angle = bot->GetAngle(&pos);
        if (CheckLastFlee(angle, infoList))
        {
            availablePos.push_back(Position(x, y, z));
        }
    }
    target->GetNearPoint(bot, x, y, z, 0.0f, dist, goodAngle2);
    if (bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                       bot->GetPositionZ(), x, y, z))
    {
        std::list<FleeInfo>& infoList = AI_VALUE(std::list<FleeInfo>&, "recently flee info");
        Position pos(x, y, z);
        float angle = bot->GetAngle(&pos);
        if (CheckLastFlee(angle, infoList))
        {
            availablePos.push_back(Position(x, y, z));
        }
    }
    if (availablePos.empty())
        return false;
    Position nearest = GetNearestPosition(availablePos);
    return MoveTo(bot->GetMapId(), nearest.GetPositionX(), nearest.GetPositionY(), nearest.GetPositionZ(), false, false,
                  false, true, MovementPriority::MOVEMENT_COMBAT);
}

bool RearFlankAction::isUseful()
{
    Unit const* target = GetTarget();
    if (!target)
        return false;

    // Need to double the front angle check to account for mirrored angle.
    bool inFront = target->HasInArc(2.f * minAngle, bot);
    // Rear check does not need to double this angle as the logic is inverted
    // and we are subtracting from 2pi.
    bool inRear = !target->HasInArc((2.f * M_PI) - maxAngle, bot);

    return inFront || inRear;
}

bool RearFlankAction::Execute(Event /*event*/)
{
    Unit const* target = GetTarget();
    if (!target)
        return false;

    float angle = frand(minAngle, maxAngle);
    float baseDistance = bot->GetMeleeRange(target) * 0.5f;
    Position leftFlank = target->GetPosition();
    Position rightFlank = target->GetPosition();
    Position* destination = nullptr;
    leftFlank.RelocatePolarOffset(angle, baseDistance + distance);
    rightFlank.RelocatePolarOffset(-angle, baseDistance + distance);

    if (bot->GetExactDist2d(leftFlank) < bot->GetExactDist2d(rightFlank))
    {
        destination = &leftFlank;
    }
    else
    {
        destination = &rightFlank;
    }

    return MoveTo(bot->GetMapId(), destination->GetPositionX(), destination->GetPositionY(),
                  destination->GetPositionZ(), false, false, false, true, MovementPriority::MOVEMENT_COMBAT);
}

Unit* BossRearFlankAction::GetTarget()
{
    return AI_VALUE2(Unit*, "find target", bossName);
}

bool DisperseSetAction::Execute(Event event)
{
    std::string const text = event.getParam();
    if (text == "disable")
    {
        RESET_AI_VALUE(float, "disperse distance");
        botAI->TellMasterNoFacing("Disable disperse");
        return true;
    }
    if (text == "enable" || text == "reset")
    {
        if (PlayerbotAI::IsMelee(bot))
        {
            SET_AI_VALUE(float, "disperse distance", DEFAULT_DISPERSE_DISTANCE_MELEE);
        }
        else
        {
            SET_AI_VALUE(float, "disperse distance", DEFAULT_DISPERSE_DISTANCE_RANGED);
        }
        float dis = AI_VALUE(float, "disperse distance");
        std::ostringstream out;
        out << "Enable disperse distance " << std::setprecision(2) << dis;
        botAI->TellMasterNoFacing(out.str());
        return true;
    }
    if (text == "increase")
    {
        float dis = AI_VALUE(float, "disperse distance");
        std::ostringstream out;
        if (dis <= 0.0f)
        {
            out << "Enable disperse first";
            botAI->TellMasterNoFacing(out.str());
            return true;
        }
        dis += 1.0f;
        SET_AI_VALUE(float, "disperse distance", dis);
        out << "Increase disperse distance to " << std::setprecision(2) << dis;
        botAI->TellMasterNoFacing(out.str());
        return true;
    }
    if (text == "decrease")
    {
        float dis = AI_VALUE(float, "disperse distance");
        dis -= 1.0f;
        if (dis <= 0.0f)
        {
            dis += 1.0f;
        }
        SET_AI_VALUE(float, "disperse distance", dis);
        std::ostringstream out;
        out << "Increase disperse distance to " << std::setprecision(2) << dis;
        botAI->TellMasterNoFacing(out.str());
        return true;
    }
    if (text.starts_with("set"))
    {
        float dis = -1.0f;
        ;
        sscanf(text.c_str(), "set %f", &dis);
        std::ostringstream out;
        if (dis < 0 || dis > 100.0f)
        {
            out << "Invalid disperse distance " << std::setprecision(2) << dis;
        }
        else
        {
            SET_AI_VALUE(float, "disperse distance", dis);
            out << "Set disperse distance to " << std::setprecision(2) << dis;
        }
        botAI->TellMasterNoFacing(out.str());
        return true;
    }
    std::ostringstream out;
    out << "Usage: disperse [enable | disable | increase | decrease | set {distance}]";
    float dis = AI_VALUE(float, "disperse distance");
    if (dis > 0.0f)
    {
        out << "(Current disperse distance: " << std::setprecision(2) << dis << ")";
    }
    botAI->TellMasterNoFacing(out.str());
    return true;
}

bool RunAwayAction::Execute(Event /*event*/) { return Flee(AI_VALUE(Unit*, "group leader")); }

bool MoveToLootAction::Execute(Event /*event*/)
{
    LootObjectStack* availableLoot = AI_VALUE(LootObjectStack*, "available loot");
    if (availableLoot->IsLootPending() || bot->GetLootGUID())
    {
        lootWatchGuid.Clear();
        return false;
    }

    LootObject loot = AI_VALUE(LootObject, "loot target");
    if (!loot.IsLootPossible(bot))
    {
        lootWatchGuid.Clear();
        return false;
    }

    WorldObject* target = loot.GetWorldObject(bot);
    if (!target)
        return false;

    float const distance = bot->GetDistance(target);
    uint32 const nowMs = getMSTime();

    constexpr uint32 evalGapMs = 10000;    // longer than the slowest out-of-combat tick (1-3 s)
    constexpr uint32 noProgressMs = 5000;  // give up on a target after 5s without progress
    constexpr float reachedMargin = 2.0f;  // near-target distance jitter margin

    if (loot.guid != lootWatchGuid || nowMs - lootWatchLastEvalMs > evalGapMs)
    {
        lootWatchGuid = loot.guid;
        lootWatchDistance = distance;
        lootWatchMs = nowMs;
    }
    else if (distance < lootWatchDistance - 0.5f)
    {
        lootWatchDistance = distance;
        lootWatchMs = nowMs;
    }
    lootWatchLastEvalMs = nowMs;

    // Drop a target the bot is not getting closer to (blocked path, pulled elsewhere), so the next
    // pickup chooses a closer reachable one.
    if (distance > sPlayerbotAIConfig.ContactDistance + reachedMargin && nowMs - lootWatchMs >= noProgressMs)
    {
        availableLoot->DeferLoot(loot.guid);
        if (AI_VALUE(LootObject, "loot target").guid == loot.guid)
            context->GetValue<LootObject>("loot target")->Set(LootObject());
        lootWatchGuid.Clear();
        return false;
    }

    return MoveNear(target, sPlayerbotAIConfig.ContactDistance);
}

bool MoveOutOfEnemyContactAction::Execute(Event /*event*/)
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;

    return MoveTo(target, sPlayerbotAIConfig.ContactDistance);
}

bool MoveOutOfEnemyContactAction::isUseful() { return AI_VALUE2(bool, "inside target", "current target"); }

bool SetFacingTargetAction::Execute(Event /*event*/)
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;

    if (bot->HasUnitState(UNIT_STATE_IN_FLIGHT))
        return true;

    ServerFacade::instance().SetFacingTo(bot, target);
    botAI->SetNextCheckDelay(sPlayerbotAIConfig.ReactDelay);
    return true;
}

bool SetFacingTargetAction::isUseful() { return !AI_VALUE2(bool, "facing", "current target"); }

bool SetFacingTargetAction::isPossible()
{
    if (bot->isFrozen() || bot->IsPolymorphed() || (bot->isDead() && !bot->HasPlayerFlag(PLAYER_FLAGS_GHOST)) ||
        bot->IsBeingTeleported() || bot->HasConfuseAura() || bot->IsCharmed() || bot->HasStunAura() ||
        bot->IsInFlight() || bot->HasUnitState(UNIT_STATE_LOST_CONTROL))
        return false;

    return true;
}

bool SetBehindTargetAction::Execute(Event /*event*/)
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;

    if (target->GetVictim() == bot)
        return false;

    if (!bot->IsWithinMeleeRange(target) || target->isMoving())
        return false;

    float deltaAngle = Position::NormalizeOrientation(target->GetOrientation() - target->GetAngle(bot));
    if (deltaAngle > M_PI)
        deltaAngle -= 2.0f * M_PI;  // -PI..PI

    float tolerable = M_PI_2;

    if (fabs(deltaAngle) > tolerable)
        return false;

    float goodAngle1 = Position::NormalizeOrientation(target->GetOrientation() + M_PI * 3 / 5);
    float goodAngle2 = Position::NormalizeOrientation(target->GetOrientation() - M_PI * 3 / 5);

    float dist = std::max(bot->GetExactDist(target), bot->GetMeleeRange(target) / 2) - bot->GetCombatReach() -
                 target->GetCombatReach();
    std::vector<Position> availablePos;
    float x, y, z;
    target->GetNearPoint(bot, x, y, z, 0.0f, dist, goodAngle1);
    if (bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                       bot->GetPositionZ(), x, y, z))
    {
        /// @todo: movement control now is a mess, prepare to rewrite
        std::list<FleeInfo>& infoList = AI_VALUE(std::list<FleeInfo>&, "recently flee info");
        Position pos(x, y, z);
        float angle = bot->GetAngle(&pos);
        if (CheckLastFlee(angle, infoList))
        {
            availablePos.push_back(Position(x, y, z));
        }
    }
    target->GetNearPoint(bot, x, y, z, 0.0f, dist, goodAngle2);
    if (bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                       bot->GetPositionZ(), x, y, z))
    {
        std::list<FleeInfo>& infoList = AI_VALUE(std::list<FleeInfo>&, "recently flee info");
        Position pos(x, y, z);
        float angle = bot->GetAngle(&pos);
        if (CheckLastFlee(angle, infoList))
        {
            availablePos.push_back(Position(x, y, z));
        }
    }
    if (availablePos.empty())
        return false;
    Position nearest = GetNearestPosition(availablePos);
    return MoveTo(bot->GetMapId(), nearest.GetPositionX(), nearest.GetPositionY(), nearest.GetPositionZ(), false, false,
                  false, true, MovementPriority::MOVEMENT_COMBAT);
}

bool MoveOutOfCollisionAction::Execute(Event /*event*/)
{
    float angle = M_PI * 2000 / frand(1.f, 1000.f);
    float distance = sPlayerbotAIConfig.FollowDistance;
    return MoveTo(bot->GetMapId(), bot->GetPositionX() + cos(angle) * distance,
                  bot->GetPositionY() + sin(angle) * distance, bot->GetPositionZ());
}

bool MoveOutOfCollisionAction::isUseful()
{
    // do not avoid collision on vehicle
    if (botAI->IsInVehicle())
        return false;

    return AI_VALUE2(bool, "collision", "self target") &&
           botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest friendly players")->Get().size() < 15;
}

bool MoveRandomAction::Execute(Event /*event*/)
{
    float distance = sPlayerbotAIConfig.TooCloseDistance + urand(10, 30);

    Map* map = bot->GetMap();
    for (int i = 0; i < 3; ++i)
    {
        float x = bot->GetPositionX();
        float y = bot->GetPositionY();
        float z = bot->GetPositionZ();
        float angle = (float)rand_norm() * static_cast<float>(M_PI);
        x += urand(0, distance) * cos(angle);
        y += urand(0, distance) * sin(angle);

        if (!bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                            bot->GetPositionZ(), x, y, z))
            continue;
        if (map->IsInWater(bot->GetPhaseMask(), x, y, z, bot->GetCollisionHeight()))
            continue;

        bool moved = MoveTo(bot->GetMapId(), x, y, z, false, false, false, true);
        if (moved)
            return true;
    }

    return false;
}

bool MoveRandomAction::isUseful() { return !AI_VALUE(GuidPosition, "rpg target"); }

bool MoveInsideAction::Execute(Event /*event*/) { return MoveInside(bot->GetMapId(), x, y, bot->GetPositionZ(), distance); }

bool RotateAroundTheCenterPointAction::Execute(Event /*event*/)
{
    uint32 next_point = GetCurrWaypoint();
    if (MoveTo(bot->GetMapId(), waypoints[next_point].first, waypoints[next_point].second, bot->GetPositionZ(), false,
               false, false, false, MovementPriority::MOVEMENT_COMBAT))
    {
        call_counters += 1;
        return true;
    }
    return false;
}

bool MoveFromGroupAction::Execute(Event event)
{
    float distance = atoi(event.getParam().c_str());
    if (!distance)
        distance = 20.0f;  // flee distance from config is too small for this
    return MoveFromGroup(distance);
}

bool MoveAwayFromCreatureAction::Execute(Event /*event*/)
{
    GuidVector targets = AI_VALUE(GuidVector, "nearest npcs");

    // Find all creatures with the specified Id
    std::vector<Unit*> creatures;
    for (auto const& guid : targets)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (unit && (alive && unit->IsAlive()) && unit->GetEntry() == creatureId)
        {
            creatures.push_back(unit);
        }
    }

    if (creatures.empty())
    {
        return false;
    }

    // Search for a safe position
    const int directions = 8;
    const float increment = 3.0f;
    float bestX = bot->GetPositionX();
    float bestY = bot->GetPositionY();
    float bestZ = bot->GetPositionZ();
    float maxSafetyScore = -1.0f;

    for (int i = 0; i < directions; ++i)
    {
        float angle = (i * 2 * M_PI) / directions;
        for (float distance = increment; distance <= 30.0f; distance += increment)
        {
            float moveX = bot->GetPositionX() + distance * cos(angle);
            float moveY = bot->GetPositionY() + distance * sin(angle);
            float moveZ = bot->GetPositionZ();

            // Check creature distance constraints
            bool isSafeFromCreatures = true;
            float minCreatureDist = std::numeric_limits<float>::max();
            for (Unit* creature : creatures)
            {
                float dist = creature->GetExactDist2d(moveX, moveY);
                if (dist < range)
                {
                    isSafeFromCreatures = false;
                    break;
                }
                if (dist < minCreatureDist)
                {
                    minCreatureDist = dist;
                }
            }

            if (isSafeFromCreatures && bot->IsWithinLOS(moveX, moveY, moveZ))
            {
                // A simple safety score: the minimum distance to any creature. Higher is better.
                if (minCreatureDist > maxSafetyScore)
                {
                    maxSafetyScore = minCreatureDist;
                    bestX = moveX;
                    bestY = moveY;
                    bestZ = moveZ;
                }
            }
        }
    }

    // Move to the best position found
    if (maxSafetyScore > 0.0f)
    {
        return MoveTo(bot->GetMapId(), bestX, bestY, bestZ, false, false, false, false,
                      MovementPriority::MOVEMENT_COMBAT);
    }

    return false;
}

bool MoveAwayFromCreatureAction::isPossible() { return bot->CanFreeMove(); }

bool MoveAwayFromPlayerWithDebuffAction::Execute(Event /*event*/)
{
    Group* const group = bot->GetGroup();

    if (!group)
        return false;

    std::vector<Player*> debuffedPlayers;

    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* player = gref->GetSource();
        if (player && player->IsAlive() && player->HasAura(spellId))
        {
            debuffedPlayers.push_back(player);
        }
    }

    if (debuffedPlayers.empty())
    {
        return false;
    }

    // Search for a safe position
    const int directions = 8;
    const float increment = 3.0f;
    float bestX = bot->GetPositionX();
    float bestY = bot->GetPositionY();
    float bestZ = bot->GetPositionZ();
    float maxSafetyScore = -1.0f;

    for (int i = 0; i < directions; ++i)
    {
        float angle = (i * 2 * M_PI) / directions;
        for (float distance = increment; distance <= (range + 5.0f); distance += increment)
        {
            float moveX = bot->GetPositionX() + distance * cos(angle);
            float moveY = bot->GetPositionY() + distance * sin(angle);
            float moveZ = bot->GetPositionZ();

            // Check creature distance constraints
            bool isSafeFromDebuffedPlayer = true;
            float minDebuffedPlayerDistance = std::numeric_limits<float>::max();
            for (Unit* debuffedPlayer : debuffedPlayers)
            {
                float dist = debuffedPlayer->GetExactDist2d(moveX, moveY);
                if (dist < range)
                {
                    isSafeFromDebuffedPlayer = false;
                    break;
                }
                if (dist < minDebuffedPlayerDistance)
                {
                    minDebuffedPlayerDistance = dist;
                }
            }

            if (isSafeFromDebuffedPlayer && bot->IsWithinLOS(moveX, moveY, moveZ))
            {
                // A simple safety score: the minimum distance to any debuffed player. Higher is better.
                if (minDebuffedPlayerDistance > maxSafetyScore)
                {
                    maxSafetyScore = minDebuffedPlayerDistance;
                    bestX = moveX;
                    bestY = moveY;
                    bestZ = moveZ;
                }
            }
        }
    }

    // Move to the best position found
    if (maxSafetyScore > 0.0f)
    {
        return MoveTo(bot->GetMapId(), bestX, bestY, bestZ, false, false, false, false,
                      MovementPriority::MOVEMENT_COMBAT, true);
    }

    return false;
}

bool MoveAwayFromPlayerWithDebuffAction::isPossible()
{
    return bot->CanFreeMove();
}

bool MovementAction::CheckSplineProgress(TravelPlan& state)
{
    if (!state.splineActive)
        return false;

    // walkPoints may have been cleared by a map transfer or external reset
    // while the spline was still flagged active; bail out safely.
    if (state.walkPoints.empty())
    {
        state.splineActive = false;
        return false;
    }

    if (bot->movespline->Finalized())
    {
        G3D::Vector3 const& endPt = state.walkPoints.back();
        float distToEnd = bot->GetExactDist(endPt.x, endPt.y, endPt.z);

        if (distToEnd < 10.0f)
        {
            state.splineActive = false;
            state.walkPoints.clear();
            return true;  // Arrived
        }

        // Spline finalized short of target — interrupted (combat/knockback/etc).
        // Caller will re-launch.
        state.splineActive = false;
        return false;
    }

    // Stuck detection
    if (state.splineStartTime &&
        GetMSTimeDiffToNow(state.splineStartTime) > state.expectedDuration * 2 + (30 * IN_MILLISECONDS))
    {
        G3D::Vector3 const& endPt = state.walkPoints.back();
        botAI->TeleportTo(WorldLocation(bot->GetMapId(), endPt.x, endPt.y, endPt.z));
        state.splineActive = false;
        state.walkPoints.clear();
        return true;
    }

    return false;  // Still moving
}

bool MovementAction::LaunchWalkSpline(TravelPlan& state)
{
    if (state.walkPoints.size() < 2)
    {
        state.walkPoints.clear();
        return false;
    }

    // Trim past any stored points the bot has already moved past — useful
    // when a spline is interrupted (combat, knockback, mid-spline reissue)
    // and we re-launch from a position later in the route.
    G3D::Vector3 botPos(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    float closestDist = FLT_MAX;
    size_t closestIdx = 0;
    for (size_t i = 0; i < state.walkPoints.size(); ++i)
    {
        float distance = (state.walkPoints[i] - botPos).squaredLength();
        if (distance < closestDist)
        {
            closestDist = distance;
            closestIdx = i;
        }
    }
    if (closestIdx > 0)
        state.walkPoints.erase(state.walkPoints.begin(), state.walkPoints.begin() + closestIdx);

    if (state.walkPoints.size() < 2)
    {
        state.walkPoints.clear();
        return true;
    }

    // Sparse-segment clip (cmangos parity): truncate the chain at the
    // first segment longer than ~11.18y. Spline interpolation between
    // sparse waypoints can cut corners through visual obstacles (trees,
    // walls) the navmesh routed around. Bot re-plans from a closer
    // position next tick where the resolved poly chain is denser.
    {
        constexpr float SPARSE_SEG_SQ = 125.0f;  // sqrt(125) ≈ 11.18y
        for (size_t i = 1; i < state.walkPoints.size(); ++i)
        {
            G3D::Vector3 d = state.walkPoints[i] - state.walkPoints[i - 1];
            if (d.squaredLength() > SPARSE_SEG_SQ)
            {
                state.walkPoints.resize(i);
                break;
            }
        }
        if (state.walkPoints.size() < 2)
        {
            state.walkPoints.clear();
            return true;
        }
    }

    // Re-clamp cached waypoints to current valid Z. Rows in
    // playerbots_travelnode_path store absolute coords baked at
    // offline generation; if the live navmesh has shifted since
    // (mmap regen, terrain change, vmap update), the stored z can
    // be above ground — MoveSplinePath plays back coords verbatim
    // and the bot looks like it's walking through the air.
    // UpdateAllowedPositionZ factors mmap polygon Z, water surface,
    // swimming, flying and transport state, so cave floors above
    // the terrain plane snap correctly.
    for (auto& pt : state.walkPoints)
        bot->UpdateAllowedPositionZ(pt.x, pt.y, pt.z);

    // Mount up
    if (!bot->IsMounted() && !bot->IsInCombat() && bot->IsOutdoors() && bot->IsAlive())
        botAI->DoSpecificAction("check mount state", Event(), true);

    float totalDist = 0;
    for (size_t i = 1; i < state.walkPoints.size(); ++i)
        totalDist += (state.walkPoints[i] - state.walkPoints[i - 1]).length();

    float speed = bot->GetSpeed(MOVE_RUN);
    state.expectedDuration = static_cast<uint32>((totalDist / speed) * IN_MILLISECONDS);

    bot->GetMotionMaster()->MoveSplinePath(&state.walkPoints, FORCED_MOVEMENT_RUN);

    state.splineStartTime = getMSTime();
    state.splineActive = true;

    G3D::Vector3 const& last = state.walkPoints.back();

    // Update LastMovement so MoveFarTo's spline-active early-out
    // knows about this in-flight walk and won't recompute the path
    // mid-spline. Mirror what MoveTo does after dispatching a spline.
    {
        float delay = static_cast<float>(state.expectedDuration);
        delay = std::min(delay, static_cast<float>(sPlayerbotAIConfig.MaxWaitForMove));
        delay = std::max(delay, 0.f);
        LastMovement& lastMove = AI_VALUE(LastMovement&, "last movement");
        lastMove.Set(bot->GetMapId(), last.x, last.y, last.z,
            bot->GetOrientation(), delay, MovementPriority::MOVEMENT_NORMAL);

        // Cache the dispatched waypoint chain so MoveFarTo's 10%
        // lastPath reuse and "no worse" reuse can pick it up next tick.
        std::vector<WorldPosition> wpts;
        wpts.reserve(state.walkPoints.size());
        for (auto const& pt : state.walkPoints)
            wpts.emplace_back(bot->GetMapId(), pt.x, pt.y, pt.z);
        lastMove.setPath(TravelPath(wpts));
    }

    EmitDebugMove("TravelPlan:walk-start", "mmap", last.x, last.y, last.z);

    return false;  // Walking
}

bool MovementAction::RefineWalkPoints(std::vector<G3D::Vector3>& walkPoints)
{
    if (walkPoints.size() < 2)
        return true;

    std::vector<G3D::Vector3> refined;
    refined.reserve(walkPoints.size() * 4);

    uint32 const mapId = bot->GetMapId();

    for (size_t i = 0; i + 1 < walkPoints.size(); ++i)
    {
        G3D::Vector3 const& a = walkPoints[i];
        G3D::Vector3 const& b = walkPoints[i + 1];

        WorldPosition aPos(mapId, a.x, a.y, a.z);
        WorldPosition bPos(mapId, b.x, b.y, b.z);

        // Per-segment mmap query: routes around geometry the offline
        // graph didn't account for, or returns empty if unreachable.
        std::vector<WorldPosition> segPath = bPos.getPathStepFrom(aPos, bot);

        // Trust the raw waypoint pair when mmap can't validate it —
        // navmesh gaps/tile-edge artifacts shouldn't kill an active plan.
        bool const trustRaw = segPath.empty() ||
                              TravelPath::IsPathCheating(segPath, aPos.distance(bPos));

        if (trustRaw)
        {
            if (i == 0)
                refined.emplace_back(a);
            refined.emplace_back(b);
            continue;
        }

        // Include the first segment's start; skip subsequent starts
        // to avoid duplicating the prior segment's tail.
        size_t startK = (i == 0) ? 0 : 1;
        for (size_t k = startK; k < segPath.size(); ++k)
            refined.emplace_back(segPath[k].GetPositionX(),
                                 segPath[k].GetPositionY(),
                                 segPath[k].GetPositionZ());
    }

    walkPoints = std::move(refined);
    return true;
}

bool MovementAction::MoveToSpline(TravelPlan& state, WorldPosition target)
{
    if (!IsMovingAllowed())
        return false;

    EmitDebugMove("TravelPlan:walk-waypoint", "mmap", target.GetPositionX(), target.GetPositionY(), target.GetPositionZ());

    // Generate path
    state.walkPoints.clear();
    PathResult path = GeneratePath(target.GetPositionX(), target.GetPositionY(), target.GetPositionZ());
    // Reject paths that PathGenerator marked unreachable. The default
    // accept mask is NORMAL | INCOMPLETE; anything else (NOT_USING_PATH
    // from BuildShortcut on invalid polys, NOPATH, etc.) means the
    // dispatched waypoints would either be a straight-line through
    // geometry or stop short of the target. Abort the plan instead so
    // MoveFarTo can re-derive via its own probe.
    if (!path.reachable)
    {
        state.walkPoints.clear();
        return false;
    }
    for (auto const& pt : path.points)
        state.walkPoints.push_back(G3D::Vector3(pt.x, pt.y, pt.z));

    if (state.walkPoints.size() < 2)
    {
        state.walkPoints.clear();
        return false;
    }

    // Launch spline movement
    LaunchWalkSpline(state);
    return true;
}

bool MovementAction::GetTravelPlan(TravelPlan& plan, WorldPosition destination)
{
    WorldPosition botPos(bot->GetMapId(), bot->GetPositionX(),
                         bot->GetPositionY(), bot->GetPositionZ());
    return sTravelNodeMap.GetFullPath(plan, botPos, bot->GetZoneId(), destination, bot);
}

// ============================================================================
// A->B movement dispatch + cross-continent legs
// ============================================================================

bool MovementAction::UseTaxi(PlayerbotAI* botAI, uint32 entry, bool needNpc)
{
    Player* bot = botAI->GetBot();
    AiObjectContext* context = botAI->GetAiObjectContext();  // for AI_VALUE("nearest npcs")

    TaxiPathEntry const* tEntry = sTaxiPathStore.LookupEntry(entry);

    if (!tEntry)
    {
        bot->CleanupAfterTaxiFlight();
        if (bot->IsMounted())
            bot->Dismount();
        // PORT-TODO: the source's spell-click fallback (HandleSpellClick on the
        // special Gryphon of Ebon Hold path) has no target equivalent. Without
        // a taxi path entry there is nothing to activate, so report failure.
        return false;
    }

    Creature* unit = nullptr;

    if (needNpc)
    {
        GuidVector npcs = AI_VALUE(GuidVector, "nearest npcs");
        for (auto i = npcs.begin(); i != npcs.end(); ++i)
        {
            unit = bot->GetNPCIfCanInteractWith(*i, UNIT_NPC_FLAG_FLIGHTMASTER);
            if (unit)
                break;
        }

        if (!unit)
            return false;

        if (unit && !bot->m_taxi.IsTaximaskNodeKnown(tEntry->from))
        {
            bot->GetSession()->SendLearnNewTaxiNode(unit);
            unit->SetFacingTo(unit->GetAngle(bot));
        }
    }

    uint32 botMoney = bot->GetMoney();
    if (botAI->HasCheat(BotCheatMask::gold) || botAI->HasCheat(BotCheatMask::taxi))
        bot->SetMoney(botMoney + tEntry->price);

    bot->CleanupAfterTaxiFlight();

    if (bot->IsMounted())
        bot->Dismount();

    bool goTaxi = bot->ActivateTaxiPathTo({tEntry->from, tEntry->to}, unit, 1);

    if (!goTaxi)
        bot->SetMoney(botMoney);

    return goTaxi;
}

bool MovementAction::MoveOnTransport(PlayerbotAI* botAI, Transport* transport, bool doTeleport)
{
    Player* bot = botAI->GetBot();
    if (!transport)
        return false;

    // PORT-TODO: the source walks onto a transport using
    // WorldPosition::RandomPointOnTrans + getPathStepFrom(transport-offset) +
    // toPointsArray, none of which exist in the target (AC has no transport
    // navmesh — see transport-navmesh-gap). v1 supports only the teleport path
    // (transportTeleportType > 0), which AC fully handles: relocate the bot to
    // the transport, then board. The spline-walk-onto-transport variant is
    // deferred to the transport-navmesh chapter.
    if (!doTeleport)
        return false;

    WorldPosition transPos(transport);
    bot->GetMap()->PlayerRelocation(bot, transPos.GetPositionX(), transPos.GetPositionY(),
                                    transPos.GetPositionZ(), bot->GetOrientation());
    transport->AddPassenger(bot, true);
    bot->SendMovementFlagUpdate();
    return true;
}

bool MovementAction::MoveOffTransport(PlayerbotAI* botAI, WorldPosition exitPos, bool doTeleport)
{
    Player* bot = botAI->GetBot();

    if (!bot->GetTransport())
        return false;

    Transport* transport = bot->GetTransport();
    transport->RemovePassenger(bot);

    // PORT-TODO: the source's spline disembark path uses getPathStepFrom +
    // toPointsArray (transport navmesh), absent in the target. v1 supports the
    // teleport disembark (transportTeleportType > 0), which AC fully handles.
    if (doTeleport)
    {
        SafeBotTeleport(bot, exitPos, exitPos.GetOrientation());
        return true;
    }

    bot->NearTeleportTo(bot->m_movementInfo.pos.GetPositionX(), bot->m_movementInfo.pos.GetPositionY(),
                        bot->m_movementInfo.pos.GetPositionZ(), bot->m_movementInfo.pos.GetOrientation());
    return false;
}

bool MovementAction::UseTransport(PlayerbotAI* botAI, uint32 entry, WorldPosition dockPosition,
                                  WorldPosition exitPosition, bool doTeleport)
{
    Player* bot = botAI->GetBot();
    WorldPosition botPos(bot);

    Transport* transport = bot->GetTransport();

    if (transport)
    {
        if (dockPosition.GetMapId() == bot->GetMapId() &&
            dockPosition.sqDistance2d(WorldPosition(transport)) < INTERACTION_DISTANCE * INTERACTION_DISTANCE)
        {
            MoveOffTransport(botAI, exitPosition, doTeleport);
            return true;
        }

        if (urand(0, 50))
            MoveOnTransport(botAI, transport, doTeleport);

        return false;
    }

    float minDist = 0;

    for (auto& trans : dockPosition.getTransports(entry))
    {
        float distance = dockPosition.sqDistance2d(WorldPosition(trans));

        if (minDist && distance > minDist)
            continue;

        transport = trans;
        minDist = distance;
    }

    if (transport && dockPosition.GetMapId() == bot->GetMapId() &&
        dockPosition.sqDistance2d(WorldPosition(transport)) < INTERACTION_DISTANCE * INTERACTION_DISTANCE)
    {
        MoveOnTransport(botAI, transport, doTeleport);
        return true;
    }

    return false;
}

bool MovementAction::WaitForTransport()
{
    LastMovement& lastMove = AI_VALUE(LastMovement&, "last movement");

    // Check if we need to resume transport journey.
    if (!lastMove.lastTransportEntry)
        return false;

    Transport* transport = bot->GetTransport();

    if (!transport || transport->GetEntry() != lastMove.lastTransportEntry || lastMove.lastPath.empty() ||
        lastMove.lastPath.getPath().front().type != PathNodeType::NODE_TRANSPORT ||
        lastMove.lastPath.getPath().front().entry != lastMove.lastTransportEntry)
    {
        lastMove.lastTransportEntry = 0;
        return false;
    }

    TravelPath path = lastMove.lastPath;

    if (!path.UpcommingSpecialMovement(WorldPosition(bot), 0.0f, bot->GetTransport()))
        return false;

    PathNodePoint dockPoint = path.getPath().front();
    PathNodePoint telePoint = *std::next(path.getPath().begin());

    if (!UseTransport(botAI, dockPoint.entry, dockPoint.point, telePoint.point,
                      sPlayerbotAIConfig.TransportTeleportType > 0))
        return true;

    lastMove.lastTransportEntry = 0;
    return false;
}

bool MovementAction::FlyDirect(WorldPosition const& /*startPosition*/, WorldPosition const& /*endPosition*/,
                               WorldPosition& /*movePosition*/, TravelPath /*movePath*/)
{
    // Fly directly to the destination on a free-flying mount.
    //
    // PORT-TODO: the source FlyDirect relies on WorldPosition helpers absent in
    // the target — isOutside(), canFly(), currentHeight(), getHeight(),
    // limit(), IsInLineOfSight() — plus direct SMSG_SPLINE_MOVE_SET_FLYING
    // packet manipulation. It is the optional "why walk if you can fly"
    // shortcut, not part of the core A->B reachability. v1 returns false so
    // MoveTo2 always dispatches the ground/spline path (flight travel is also
    // covered by the NewRpg TravelFlight layer + the NODE_FLIGHTPATH leg).
    return false;
}

void MovementAction::UpdateFlyingState(WorldPosition& /*movePosition*/, float /*totalDistance*/, float /*originalZ*/,
                                       float /*maxDist*/, bool /*isWalking*/)
{
    // PORT-TODO: per-waypoint ascend/descend flying-flag management. Depends on
    // the same absent WorldPosition flight helpers as FlyDirect and on raw
    // SMSG_SPLINE_MOVE_SET/UNSET_FLYING packets. Only reachable when
    // bot->IsFreeFlying(); since FlyDirect is a no-op in v1 and free-flying
    // ground dispatch already works, this stays a no-op for v1.
}

bool MovementAction::GeneratePathAvoidingHazards(std::vector<WorldPosition>& /*movePath*/)
{
    // PORT-TODO: the source pre-scans AI_VALUE(std::list<HazardPosition>,
    // "hazards") and reroutes path points around AoE hazards using
    // CalculatePerpendicularPoint / IsValidPosition. The target has no
    // "hazards" value nor those geometry helpers. v1 leaves the input path
    // unchanged (no hazard avoidance); the bot still follows the navmesh route.
    return false;
}

Unit* MovementAction::GetMover(Player* bot)
{
    // Resolve the controlling unit: the bot, or the vehicle it controls.
    if (Vehicle* botVehicle = bot->GetVehicle())
    {
        if (Unit* vehicle = botVehicle->GetBase())
        {
            VehicleSeatEntry const* seat = botVehicle->GetSeatForPassenger(bot);
            if (!seat || !seat->CanControl())
                return bot;
            return vehicle;
        }
    }
    return bot;
}

TravelPath MovementAction::ResolveMovePath(WorldPosition const& startPosition, WorldPosition const& endPosition,
                                           Unit* /*mover*/, LastMovement& lastMove)
{
    // Non-const working copies: distance()/getPathTo() are non-const in the
    // target's WorldPosition.
    WorldPosition startPos = startPosition;
    WorldPosition endPos = endPosition;

    float totalDistance = startPos.distance(endPos);
    float maxDistChange = totalDistance * 0.1f;

    // Last long path still leads to roughly the same destination — reuse it.
    if (!lastMove.lastPath.empty() && lastMove.lastPath.getBack().distance(endPos) < maxDistChange)
    {
        // Make reuse visible: without this line the debug log shows only
        // MoveFar+Dispatch pairs and a stale path being re-ridden is
        // indistinguishable from a fresh resolution.
        EmitDebugMove("Resolve", "reuse", lastMove.lastPath.getBack().GetPositionX(),
                      lastMove.lastPath.getBack().GetPositionY(), lastMove.lastPath.getBack().GetPositionZ());
        return lastMove.lastPath;
    }

    bool needsLongPath = false;

    // Hardcoded graph threshold: gating on sightDistance ties routing to
    // a combat-tuning key that deployments raise freely — with it raised,
    // multi-hundred-yard trips never consult the node graph and fall to
    // raw probes that dead-end against terrain. 50y is the established
    // cutoff; getFullPath's probe-first shortcut still takes the direct
    // walk whenever one exists, so short trips lose nothing.
    constexpr float TRAVELNODE_THRESHOLD = 50.0f;

    if (startPos.GetMapId() != endPos.GetMapId())
        needsLongPath = true;
    else if (totalDistance > TRAVELNODE_THRESHOLD)
        needsLongPath = true;
    // Acherus: The Ebon Hold (DK start, map 609) is a floating citadel; large
    // vertical moves cross elevator/platform Z-stratification that navmesh-only
    // pathing cannot resolve, so force the node graph there.
    else if (startPos.GetMapId() == 609 && std::fabs(startPos.GetPositionZ() - endPos.GetPositionZ()) > 20.0f)
        needsLongPath = true;

    TravelPath outMovePath;

    if (needsLongPath && !sTravelNodeMap.getNodes().empty() && !bot->InBattleground())
    {
        outMovePath = sTravelNodeMap.getFullPath(startPos, endPos, bot);  // Pathfind using nodes.

        // Diagnostic: with `debug move` on, say whether the graph
        // produced a route that actually reaches the destination or
        // we're following a raw partial probe (missing coverage /
        // failed route selection) — the difference between "walks the
        // node path" and "walks into a mountain" is invisible in-game.
        // On failure, append getRoute's per-stage fail counters.
        if (!outMovePath.empty())
        {
            float const endGap = outMovePath.getBack().distance(endPos);
            bool const reached = endGap < totalDistance * 0.1f;
            EmitDebugMove("Resolve", reached ? "node-route" : "partial-fallback",
                          outMovePath.getBack().GetPositionX(), outMovePath.getBack().GetPositionY(),
                          outMovePath.getBack().GetPositionZ(),
                          reached ? nullptr : TravelNodeMap::GetLastRouteFailReason().c_str());
        }
        else
            EmitDebugMove("Resolve", "no-route", endPos.GetPositionX(), endPos.GetPositionY(),
                          endPos.GetPositionZ(), TravelNodeMap::GetLastRouteFailReason().c_str());
    }
    else
    {
        std::vector<WorldPosition> path = startPos.getPathTo(endPos, bot);  // Navmesh pathfinding only.
        outMovePath.addPath(path);

        // Short trips had no resolution logging at all — failures under
        // the node threshold were invisible in the debug stream.
        if (!outMovePath.empty())
        {
            float const endGap = outMovePath.getBack().distance(endPos);
            EmitDebugMove("Resolve", "direct", outMovePath.getBack().GetPositionX(),
                          outMovePath.getBack().GetPositionY(), outMovePath.getBack().GetPositionZ(),
                          endGap > 5.0f ? "short (did not arrive)" : nullptr);
        }
        else
            EmitDebugMove("Resolve", "direct-none", endPos.GetPositionX(), endPos.GetPositionY(),
                          endPos.GetPositionZ());
    }

    // A "path" that goes nowhere is no path. Pathfinding failure returns
    // its seed point(s) — getPathFromPath yields the start on zero
    // progress — and dispatching that produces an endless stream of
    // zero-length splines: the bot hovers/jitters in place and no
    // recovery ever fires because the path is technically non-empty.
    // Treat no-progress results as EMPTY so MoveTo2's recovery path
    // (unstick / no-path / caller fallbacks) runs instead.
    if (!outMovePath.empty())
    {
        bool const noProgress = outMovePath.getPointPath().size() < 2 ||
            (startPos.distance(outMovePath.getBack()) < 1.0f && totalDistance > 2.0f);
        if (noProgress)
            outMovePath.clear();
    }

    if (!lastMove.lastPath.empty() && !outMovePath.empty() &&
        lastMove.lastPath.getBack().distance(endPos) <= outMovePath.getBack().distance(endPos))
        outMovePath = lastMove.lastPath;

    // When real pathfinding (node graph + navmesh) produced NOTHING, do NOT
    // fabricate a 1-point "path" to the raw endpoint. DispatchMovement would
    // "move" the bot ~0 yards to it and MoveTo2 would report SUCCESS, so the
    // bot loops travelStatus=TRAVEL forever on a destination it can never walk
    // to (the dominant frozen-bot symptom). Returning empty makes MoveTo2 fail
    // -> the travel driver increments its move-retry and eventually teleports
    // (when no player is watching). Reachable targets are unaffected (their
    // paths are non-empty).
    return outMovePath;
}

bool MovementAction::HandleSpecialMovement(TravelPath& path)
{
    PathNodePoint currentPoint = path.getPath().front();
    PathNodePoint nextPoint;
    if (path.getPath().size() > 1)
        nextPoint = *std::next(path.getPath().begin());

    // Game object portals (static spellcaster GO with a teleport effect).
    if (currentPoint.type == PathNodeType::NODE_STATIC_PORTAL && currentPoint.entry)
    {
        GameObjectTemplate const* goInfo = sObjectMgr->GetGameObjectTemplate(currentPoint.entry);
        if (!goInfo || goInfo->type != GAMEOBJECT_TYPE_SPELLCASTER)
            return false;

        uint32 spellId = goInfo->spellcaster.spellId;
        SpellInfo const* pSpellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!pSpellInfo)
            return false;

        if (pSpellInfo->Effects[EFFECT_0].TriggerSpell)
        {
            SpellInfo const* triggered = sSpellMgr->GetSpellInfo(pSpellInfo->Effects[EFFECT_0].TriggerSpell);
            if (triggered)
                pSpellInfo = triggered;
        }

        bool hasTeleportEffect = pSpellInfo->Effects[EFFECT_0].Effect == SPELL_EFFECT_TELEPORT_UNITS ||
                                 pSpellInfo->Effects[EFFECT_1].Effect == SPELL_EFFECT_TELEPORT_UNITS ||
                                 pSpellInfo->Effects[EFFECT_2].Effect == SPELL_EFFECT_TELEPORT_UNITS;
        if (!hasTeleportEffect)
            return false;

        if (bot->IsMounted())
        {
            // Source guard: don't dismount mid-air on a flying mount
            // (currentHeight() > 10y). Approximate height-above-ground via
            // the map water/ground level.
            if (bot->IsFlying() && bot->GetMapWaterOrGroundLevel(bot->GetPositionX(), bot->GetPositionY(),
                                                                 bot->GetPositionZ()) + 10.0f < bot->GetPositionZ())
                return false;
            bot->Dismount();
        }

        GuidVector gos = AI_VALUE(GuidVector, "nearest game objects");
        for (auto i = gos.begin(); i != gos.end(); ++i)
        {
            GameObject* go = botAI->GetGameObject(*i);
            if (!go || go->GetEntry() != currentPoint.entry)
                continue;

            if (!bot->GetGameObjectIfCanInteractWith(go->GetGUID(), go->GetGoType()))
                continue;

            WorldPacket packet(CMSG_GAMEOBJ_USE);
            packet << *i;
            bot->GetSession()->QueuePacket(new WorldPacket(packet));
            return true;
        }

        return false;
    }

    if (currentPoint.type == PathNodeType::NODE_AREA_TRIGGER)
    {
        if (currentPoint.entry)
            AI_VALUE(LastMovement&, "last area trigger").lastAreaTrigger = currentPoint.entry;
        else
            return SafeBotTeleport(bot, nextPoint.point, nextPoint.point.GetOrientation());
    }

    // We are getting 'on' transport.
    if (nextPoint.type == PathNodeType::NODE_TRANSPORT)
    {
        bool usedTransport = UseTransport(botAI, nextPoint.entry, nextPoint.point, WorldPosition(),
                                          sPlayerbotAIConfig.TransportTeleportType > 0);

        if (usedTransport)
            AI_VALUE(LastMovement&, "last movement").lastTransportEntry = nextPoint.entry;

        WaitForReach(1000.0f);
        return true;
    }

    if (currentPoint.type == PathNodeType::NODE_TRANSPORT)
    {
        bool usedTransport = UseTransport(botAI, currentPoint.entry, currentPoint.point, nextPoint.point,
                                          sPlayerbotAIConfig.TransportTeleportType > 0);

        uint32 lastTransportEntry = 0;

        if (!usedTransport)
        {
            if (bot->GetTransport())
                lastTransportEntry = nextPoint.entry;
        }
        else
        {
            if (!bot->GetTransport())
                return SafeBotTeleport(bot, nextPoint.point, nextPoint.point.GetOrientation());

            lastTransportEntry = nextPoint.entry;
        }

        if (lastTransportEntry)
            AI_VALUE(LastMovement&, "last movement").lastTransportEntry = lastTransportEntry;

        WaitForReach(1000.0f);
        return true;
    }

    if (nextPoint.type == PathNodeType::NODE_FLIGHTPATH && nextPoint.entry)
        return UseTaxi(botAI, nextPoint.entry, true);

    if (nextPoint.type == PathNodeType::NODE_TELEPORT && nextPoint.entry)
    {
        float ground = bot->GetMapWaterOrGroundLevel(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        bool canCastNow = !bot->IsFlying() || (bot->GetPositionZ() - ground) < 10.0f;

        if (nextPoint.entry == 8690)  // Hearthstone
        {
            if (AI_VALUE2(bool, "action useful", "hearthstone") && canCastNow)
                return botAI->DoSpecificAction("hearthstone", Event("move action"), true);
        }
        else if (!bot->HasSpellCooldown(nextPoint.entry) && canCastNow)
        {
            if (bot->IsMounted())
            {
                bot->Dismount();
                // WotLK: defer the cast one tick so the dismount transition
                // settles (casting mid-dismount fails); the bot re-enters this
                // branch next tick unmounted and casts then.
                return false;
            }

            botAI->RemoveShapeshift();

            if (botAI->DoSpecificAction(
                    "cast",
                    Event("rpg action",
                          ChatHelper::FormatWorldobject(bot) + " " + std::to_string(nextPoint.entry)),
                    true))
                return true;
        }

        AI_VALUE(LastMovement&, "last movement").setPath(TravelPath());
        return false;
    }

    return false;
}

void MovementAction::DispatchMovement(TravelPath movePath, bool generatePath, bool masterWalking)
{
    MotionMaster& mm = *bot->GetMotionMaster();

    mm.Clear();

    std::vector<WorldPosition> path = movePath.getPointPath();
    if (path.empty())
        return;

    WorldPosition movePosition = path.back();
    float size = WorldPosition().getPathLength(path);

    ForcedMovement moveMode = masterWalking ? FORCED_MOVEMENT_WALK : FORCED_MOVEMENT_RUN;
    if (bot->IsFlying())
        moveMode = FORCED_MOVEMENT_RUN;  // AC ForcedMovement has no FLIGHT mode.

    if (!generatePath || bot->IsFreeFlying())
    {
        // Bounded hop: target the furthest resolved waypoint within
        // reactDistance instead of the path end. The point generator's
        // internal pathfinder can straight-line to an invalid poly (the
        // NOT_USING_PATH shortcut for player movers), so a full-length
        // hop from shallow water toward a land target hundreds of yards
        // away walks the bot through the air the whole way; a bounded
        // hop self-corrects on the next dispatch, where the bot is
        // usually out of the water and back on the spline branch.
        WorldPosition botPos(bot);
        WorldPosition hopPosition = path.front();
        for (auto& p : path)
        {
            if (botPos.distance(p) > sPlayerbotAIConfig.ReactDistance)
                break;
            hopPosition = p;
        }

        mm.MovePoint(hopPosition.GetMapId(),
                     Position(hopPosition.GetPositionX(), hopPosition.GetPositionY(),
                              hopPosition.GetPositionZ(), 0.f),
                     moveMode,
                     bot->IsFlying() ? bot->GetSpeed(MOVE_FLIGHT) : 0.f,
                     bot->IsFlying());
        EmitDebugMove("Dispatch", "movepoint", hopPosition.GetPositionX(), hopPosition.GetPositionY(),
                      hopPosition.GetPositionZ());

        // Wait for the bounded hop, not the full path.
        size = botPos.distance(hopPosition);
    }
    else
    {
        GeneratePathAvoidingHazards(path);

        std::vector<G3D::Vector3> pointPath;
        pointPath.reserve(path.size() + 1);
        pointPath.emplace_back(WorldPosition(bot).getVector3());
        for (auto& p : path)
            pointPath.emplace_back(p.GetPositionX(), p.GetPositionY(), p.GetPositionZ());

        // Density guard: the spline interpolates STRAIGHT between
        // control points, so an over-long gap (sparse tail, degraded
        // link, stale cache) would walk the bot through terrain and
        // air for the whole stretch. Truncate the dispatch at the
        // first such gap — the bot walks the dense prefix and the
        // next re-resolution continues from there. Dense sources
        // space points a few yards apart; 50y is far beyond any
        // legitimate spacing.
        // Start at i=2: never truncate the FIRST segment (bot -> first
        // waypoint). Truncating there would resize to a single point and
        // return with no movement at all — a hard stall. Dense navmesh
        // paths (~4y spacing) never trip this; it only guards against a
        // sparse tail (degraded node leg / shortcut), and the bot must
        // still advance along its first leg and re-resolve next tick.
        constexpr float MAX_SPLINE_SEGMENT = 50.0f;
        bool truncated = false;
        for (size_t i = 2; i < pointPath.size(); ++i)
        {
            if ((pointPath[i] - pointPath[i - 1]).length() > MAX_SPLINE_SEGMENT)
            {
                pointPath.resize(i);
                truncated = true;
                break;
            }
        }
        if (pointPath.size() < 2)
            return;
        if (truncated)
        {
            // Wait for the dispatched prefix, not the full path.
            size = 0.0f;
            for (size_t i = 1; i < pointPath.size(); ++i)
                size += (pointPath[i] - pointPath[i - 1]).length();
        }

        // Use the ALREADY-COMPUTED navmesh path (MoveSplinePath ->
        // EscortMovementGenerator) instead of MovePoint(path.back(),
        // generatePath=true). The latter THROWS AWAY the good path and re-runs
        // PathGenerator from the bot to the endpoint; for many travel targets
        // that re-gen returns a degenerate/0-length path -> the spline
        // finalises instantly -> the bot moves ~0 and stays "traveling"
        // forever. Following the precomputed point path avoids the redundant,
        // failure-prone re-generation.
        // Ground-conform each control point (cmangos parity). The travel-node
        // route is now dense end-to-end (dense begin + tail legs from getFullPath
        // plus the dense cached node-link paths), so this per-point snap is all
        // that is needed for the spline to follow the terrain.
        for (auto& p : pointPath)
        {
            if (bot->GetTransport())
                bot->GetTransport()->CalculatePassengerPosition(p.x, p.y, p.z);
            bot->UpdateAllowedPositionZ(p.x, p.y, p.z);
            if (bot->GetTransport())
                bot->GetTransport()->CalculatePassengerOffset(p.x, p.y, p.z);
        }

        // Report the ACTUALLY dispatched endpoint (post-truncation), not
        // the pre-truncation path end — the step reading overstated
        // truncated dispatches.
        EmitDebugMove("Dispatch", "spline", pointPath.back().x, pointPath.back().y, pointPath.back().z);
        mm.MoveSplinePath(&pointPath, moveMode);
    }

    WaitForReach(size);
}

bool MovementAction::MoveTo2(WorldPosition const& endPos, bool idle, bool react, bool noPath, bool ignoreEnemyTargets)
{
    // Non-const working copy: several WorldPosition predicates
    // (IsValid/isInWater/isUnderWater) are non-const in the target.
    WorldPosition endPosNc = endPos;

    if (!endPosNc.IsValid())
        return false;

    UpdateMovementState();

    if (!botAI->CanMove())
        return false;

    Unit* mover = GetMover(bot);

    LastMovement& lastMove = AI_VALUE(LastMovement&, "last movement");

    bool detailedMove = botAI->AllowActivity(DETAILED_MOVE_ACTIVITY, true);
    if (!detailedMove && lastMove.nextTeleport)
    {
        time_t now = time(nullptr);
        if (lastMove.nextTeleport > now)
        {
            botAI->SetNextCheckDelay((uint32)((lastMove.nextTeleport - now) * 1000));
            return true;
        }
    }
    else
        lastMove.nextTeleport = 0;

    if (WaitForTransport())
        return true;

    WorldPosition startPos(bot);
    float totalDistance = startPos.distance(endPos);

    if (totalDistance < sPlayerbotAIConfig.TargetPosRecalcDistance)
    {
        if (!lastMove.lastPath.empty() && lastMove.lastPath.getBack().distance(endPos) <= totalDistance)
            lastMove.clear();

        if (mover == bot)
            bot->StopMoving();
        else
            mover->StopMoving();

        return false;
    }

    WorldPosition flyMovePosition;
    if (FlyDirect(startPos, endPos, flyMovePosition, lastMove.lastPath))
        return true;

    bool isWalking = false;

    TravelPath movePath = ResolveMovePath(startPos, endPos, mover, lastMove);

    lastMove.setPath(movePath);

    if (movePath.empty())
    {
        // Nothing resolves from HERE at all — the bot is usually
        // standing on a poly its own nav filter excludes (a steep
        // ledge it strayed onto): probes and begin legs all fail from
        // such a start and the bot freezes in place, re-resolving
        // every tick. Step to the nearest walkable poly so the next
        // resolution has a valid start. A short visible walk — safe
        // with players watching, unlike the teleport recovery.
        if (mover == bot && !bot->GetTransport())
        {
            WorldPosition correct = startPos;
            if (correct.ClosestCorrectPoint(10.0f, 10.0f))
            {
                float const stepDist = startPos.distance(correct);
                // LOS guard: ClosestCorrectPoint searches in every
                // direction and can snap to a walkable poly on the OTHER
                // side of a wall (treehouse interiors) — the raw step then
                // glitches the bot straight through. Only step to a point
                // we can actually see.
                bool const stepVisible =
                    bot->IsWithinLOS(correct.GetPositionX(), correct.GetPositionY(),
                                     correct.GetPositionZ() + bot->GetCollisionHeight());
                // >2y: a sub-2y nudge never unblocks anything — the bot
                // just inches in place tick after tick (visible as
                // hover-jitter) while the real blocker is elsewhere.
                // Skipping lets the no-path failure surface so caller
                // fallbacks (POI rotation, travel retry) take over.
                if (stepDist > 2.0f && stepDist < 15.0f && stepVisible)
                {
                    bot->GetMotionMaster()->Clear();
                    bot->GetMotionMaster()->MovePoint(
                        correct.GetMapId(),
                        Position(correct.GetPositionX(), correct.GetPositionY(), correct.GetPositionZ(), 0.f),
                        FORCED_MOVEMENT_RUN, 0.f, false);
                    EmitDebugMove("MoveTo2", "unstick-step", correct.GetPositionX(), correct.GetPositionY(),
                                  correct.GetPositionZ());
                    WaitForReach(stepDist);
                    return true;
                }
            }
        }
        // Make failure visible: the MoveFar line already printed intent,
        // so a silent false here reads as if the move went out.
        EmitDebugMove("MoveTo2", "no-path", endPos.GetPositionX(), endPos.GetPositionY(), endPos.GetPositionZ());
        return false;
    }

    if (!bot->GetTransport())
        movePath.makeShortCut(startPos, sPlayerbotAIConfig.ReactDistance, bot);

    if (movePath.empty())
    {
        lastMove.setPath(movePath);
        return true;  // Path collapsed — will rebuild next tick.
    }

    bool specialMovement =
        movePath.UpcommingSpecialMovement(startPos, sPlayerbotAIConfig.ReactDistance, bot->GetTransport());

    if (specialMovement)
        return HandleSpecialMovement(movePath);

    if (bot->GetTransport())  // Transports needed to be handled before now.
        return false;

    if (!movePath.empty())
        lastMove.setPath(movePath);

    movePath.ClipPath(botAI, mover, ignoreEnemyTargets);

    if (movePath.empty())
        return false;

    // PORT-TODO: the source snaps underwater waypoints to the water surface via
    // WorldPosition::setAtWaterSurface() (absent in the target) when the
    // destination is not itself underwater. That is a swim-at-surface
    // refinement; v1 leaves the navmesh-resolved Z, so the bot still reaches
    // the destination (possibly hugging the bottom rather than the surface).

    if (!react)
    {
        float fullPathDist = startPos.getPathLength(movePath.getPointPath());
        float waitDist = (totalDistance > sPlayerbotAIConfig.ReactDistance) ? fullPathDist - 10.0f : fullPathDist;
        WaitForReach(waitDist);
    }

    if (bot == mover)
    {
        bot->ClearEmoteState();
        if (!bot->IsStandState())
            bot->SetStandState(UNIT_STAND_STATE_STAND);

        if (bot->IsNonMeleeSpellCast(true, false, true))
            botAI->InterruptSpell();
    }

    if (totalDistance > sPlayerbotAIConfig.ReactDistance && !detailedMove)
    {
        WorldPosition teleportPosition = movePath.getBack();
        if (!botAI->HasPlayerNearby(&teleportPosition))
        {
            time_t now = time(nullptr);
            // Resolve + validate Z first; skip (re-path next tick) rather than
            // teleport to an unresolved -200000 node Z. Throttle only on success.
            if (!SafeBotTeleport(bot, teleportPosition, startPos.getAngleTo(teleportPosition)))
                return false;
            lastMove.nextTeleport = now + (time_t)MoveDelay(startPos.distance(teleportPosition));
            return true;
        }
    }

    bool masterWalking = false;
    if (sPlayerbotAIConfig.WalkDistance)
    {
        if (Unit* master = botAI->GetMaster())
        {
            if (bot->IsFriendlyTo(master) && master->HasUnitMovementFlag(MOVEMENTFLAG_WALKING) &&
                bot->GetExactDist2d(master) < sPlayerbotAIConfig.WalkDistance)
            {
                masterWalking = true;
            }
        }
    }

    bool generatePath = !noPath && !bot->IsFlying() &&
                        !bot->HasUnitMovementFlag(MOVEMENTFLAG_SWIMMING) && !bot->IsInWater();

    if (bot->IsFreeFlying())
    {
        if (bot->HasUnitMovementFlag(MOVEMENTFLAG_SWIMMING) && startPos.isInWater() &&
            !startPos.isUnderWater() && !endPosNc.isInWater())
        {
            generatePath = true;
        }

        WorldPosition movePosition = movePath.getBack();
        UpdateFlyingState(movePosition, totalDistance, startPos.GetPositionZ(),
                          sPlayerbotAIConfig.ReactDistance, isWalking);
    }

    DispatchMovement(movePath, generatePath, masterWalking);

    if (!idle)
        ClearIdleState();

    return true;
}

bool MovementAction::MoveTo2(uint32 mapId, float x, float y, float z, bool idle, bool react, bool noPath,
                             bool ignoreEnemyTargets)
{
    return MoveTo2(WorldPosition(mapId, x, y, z), idle, react, noPath, ignoreEnemyTargets);
}

bool MovementAction::ExecuteTravelPlan(TravelPlan& state)
{
    if (!state.IsActive())
        return false;

    if (bot->IsInFlight())
        return true;

    // Per-step labels (`walk`, `segment`, `flight`, `transport-*`,
    // `teleport(reason)`) cover every actual movement decision; emitting
    // an executor-ran-this-tick label here would whisper every tick
    // while the plan is active.

    // Handle active spline
    if (state.splineActive)
    {
        if (!CheckSplineProgress(state))
        {
            if (state.splineActive)
                return true;  // Still moving
            else
                LaunchWalkSpline(state);  // Interrupted, re-launch
        }
        return true;
    }

    if (state.stepIdx >= state.steps.size())
    {
        state.Reset();
        return true;
    }

    PathNodePoint const& pt = state.steps[state.stepIdx];

    switch (pt.type)
    {
        case PathNodeType::NODE_PREPATH:
        {
            if (state.stepIdx + 1 >= state.steps.size())
            {
                state.stepIdx++;
                return true;
            }

            float const botX = bot->GetPositionX();
            float const botY = bot->GetPositionY();
            float const botZ = bot->GetPositionZ();

            // Walk forward through the route while distance keeps shrinking.
            // Once it starts growing we're past the closest waypoint — break.
            size_t bestIdx = state.stepIdx + 1;
            float bestDistSq = FLT_MAX;
            for (size_t i = state.stepIdx + 1; i < state.steps.size(); ++i)
            {
                PathNodePoint const& cand = state.steps[i];
                if (cand.type != PathNodeType::NODE_PATH &&
                    cand.type != PathNodeType::NODE_NODE)
                    break;  // stop at portal/transport/etc — can't walk past

                float const dx = cand.point.GetPositionX() - botX;
                float const dy = cand.point.GetPositionY() - botY;
                float const dz = cand.point.GetPositionZ() - botZ;
                float const dSq = dx * dx + dy * dy + dz * dz;
                if (dSq >= bestDistSq)
                    break;  // moving away — closest waypoint already found

                bestDistSq = dSq;
                bestIdx = i;
            }

            constexpr float ARRIVAL_DIST = 5.0f;

            WorldPosition const& target = state.steps[bestIdx].point;
            float const distToTarget = bot->GetExactDist(
                target.GetPositionX(), target.GetPositionY(), target.GetPositionZ());

            if (distToTarget < ARRIVAL_DIST)
            {
                state.stepIdx = bestIdx;
                return true;
            }

            // Validate the path before MoveTo. PathGenerator can
            // return NORMAL | NOT_USING_PATH when start or end poly
            // is invalid (BuildShortcut → 2-point straight line).
            // PointMovementGenerator would then dispatch the bot
            // straight through any geometry between bot and target.
            // The default accept mask (NORMAL | INCOMPLETE) rejects
            // NOT_USING_PATH, so abort the plan and let MoveFarTo
            // re-derive instead of walking a known-bad shortcut.
            PathResult validate = GeneratePath(
                target.GetPositionX(), target.GetPositionY(), target.GetPositionZ(),
                DEFAULT_PATH_ACCEPT_MASK, false);
            if (!validate.reachable)
            {
                EmitDebugMove("TravelPlan", "prepath-unreachable",
                              target.GetPositionX(), target.GetPositionY(), target.GetPositionZ());
                state.Reset();
                return false;
            }

            return MoveTo(target.GetMapId(),
                target.GetPositionX(), target.GetPositionY(), target.GetPositionZ(),
                false, false, false, true /*exact_waypoint*/);
        }

        case PathNodeType::NODE_PATH:
        case PathNodeType::NODE_NODE:
        {
            // Batch consecutive walk points into one spline. Capped at
            // 20 points per dispatch as a cheap upper bound on per-tick
            // work; stepIdx advances exactly in step with what's
            // dispatched, so the next tick picks up from the cutoff.
            static constexpr uint32 MAX_SPLINE_POINTS = 20;
            state.walkPoints.clear();
            while (state.stepIdx < state.steps.size() && state.walkPoints.size() < MAX_SPLINE_POINTS)
            {
                PathNodePoint const& wp = state.steps[state.stepIdx];
                if (wp.type != PathNodeType::NODE_PATH && wp.type != PathNodeType::NODE_NODE)
                    break;
                state.walkPoints.push_back(G3D::Vector3(wp.point.GetPositionX(),
                    wp.point.GetPositionY(), wp.point.GetPositionZ()));
                state.stepIdx++;
            }

            if (state.walkPoints.empty())
                return true;

            // Already near end of batch?
            G3D::Vector3 const& last = state.walkPoints.back();
            float dist = bot->GetExactDist(last.x, last.y, last.z);
            if (dist < 10.0f)
            {
                state.walkPoints.clear();
                return true;
            }

            // Too far from first point — abort the plan and let the
            // caller's stuck-recovery decide what to do. An abandoned
            // plan is recovered by the next MoveFarTo cycle.
            if (state.walkPoints.size() >= 2)
            {
                G3D::Vector3 const& first = state.walkPoints.front();
                float distToFirst = bot->GetExactDist(first.x, first.y, first.z);
                if (distToFirst > MAX_PATHFINDING_DISTANCE)
                {
                    state.walkPoints.clear();
                    state.Reset();
                    return false;
                }
            }
            // Single point — use PathGenerator directly
            if (state.walkPoints.size() < 2)
            {
                WorldPosition target(bot->GetMapId(), last.x, last.y, last.z);
                MoveToSpline(state, target);
                state.walkPoints.clear();
                return true;
            }

            // Re-validate each segment against the live navmesh and
            // substitute mmap-routed sub-paths where needed.
            if (!RefineWalkPoints(state.walkPoints))
            {
                G3D::Vector3 const& failPt = state.walkPoints.empty()
                    ? G3D::Vector3(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ())
                    : state.walkPoints.front();
                EmitDebugMove("TravelPlan", "segment-unwalkable",
                              failPt.x, failPt.y, failPt.z);
                state.walkPoints.clear();
                state.Reset();
                return false;
            }

            LaunchWalkSpline(state);
            return true;
        }

        // Phase B: enum value 3 is now NODE_AREA_TRIGGER (was NODE_PORTAL).
        // This legacy ExecuteTravelPlan branch is superseded by the new
        // HandleSpecialMovement dispatch and is slated for Phase C deletion;
        // kept compiling for now under the renamed case.
        case PathNodeType::NODE_AREA_TRIGGER:
        {
            // Pair: source (pointIdx) + dest (pointIdx+1)
            if (state.stepIdx + 1 >= state.steps.size())
            {
                state.Reset();
                return false;
            }

            PathNodePoint const& src = state.steps[state.stepIdx];
            PathNodePoint const& dst = state.steps[state.stepIdx + 1];

            // Already on destination map?
            if (bot->GetMapId() == dst.point.GetMapId())
            {
                state.stepIdx += 2;
                return true;
            }
            // Walk to portal source
            float dist = bot->GetExactDist(src.point.GetPositionX(), src.point.GetPositionY(), src.point.GetPositionZ());
            if (dist > INTERACTION_DISTANCE)
                return MoveTo(src.point.GetMapId(), src.point.GetPositionX(), src.point.GetPositionY(), src.point.GetPositionZ());

            // At portal but didn't cross — natural collision missed.
            // Abort the plan; stuck-recovery in MoveFarTo will decide
            // whether to retry or teleport the bot.
            state.Reset();
            return false;
        }

        case PathNodeType::NODE_TRANSPORT:
        {
            if (state.stepIdx + 1 >= state.steps.size())
            {
                state.Reset();
                return false;
            }

            PathNodePoint const& board = state.steps[state.stepIdx];
            PathNodePoint const& arrive = state.steps[state.stepIdx + 1];
            // Arrived at destination?
            if (bot->GetMapId() == arrive.point.GetMapId() && !bot->GetTransport())
            {
                state.stepIdx += 2;
                return true;
            }
            // On transport — wait
            if (bot->GetTransport())
            {
                if (bot->GetMapId() == arrive.point.GetMapId())
                {
                    bot->GetTransport()->RemovePassenger(bot);
                    bot->StopMovingOnCurrentPos();
                    state.stepIdx += 2;
                }
                return true;
            }

            // Walk to boarding point
            float dist = bot->GetExactDist(board.point.GetPositionX(), board.point.GetPositionY(), board.point.GetPositionZ());
            if (dist > 60.0f)
                return MoveTo(board.point.GetMapId(), board.point.GetPositionX(), board.point.GetPositionY(), board.point.GetPositionZ());

            // Try to board
            if (board.entry)
            {
                Map* map = bot->GetMap();
                if (map)
                {
                    Transport* transport =
                        GetTransportForPosTolerant(map, bot, bot->GetPhaseMask(), board.point.GetPositionX(),
                            board.point.GetPositionY(), board.point.GetPositionZ());
                    if (transport && transport->GetEntry() == board.entry)
                    {
                        BoardTransport(transport);
                        return true;
                    }
                }
            }
            // Wait at boarding point
            if (dist > INTERACTION_DISTANCE)
                return MoveTo(board.point.GetMapId(), board.point.GetPositionX(), board.point.GetPositionY(), board.point.GetPositionZ());
            return true;
        }

        case PathNodeType::NODE_FLIGHTPATH:
        {
            if (state.stepIdx + 1 >= state.steps.size())
            {
                state.Reset();
                return false;
            }

            PathNodePoint const& dep = state.steps[state.stepIdx];
            PathNodePoint const& arr = state.steps[state.stepIdx + 1];

            if (bot->IsInFlight())
                return true;

            // Resolve taxi path
            if (state.route.empty())
            {
                uint32 fromTaxi = sObjectMgr->GetNearestTaxiNode(dep.point.GetPositionX(), dep.point.GetPositionY(),
                        dep.point.GetPositionZ(), dep.point.GetMapId(), bot->GetTeamId());
                uint32 toTaxi =  sObjectMgr->GetNearestTaxiNode(arr.point.GetPositionX(), arr.point.GetPositionY(),
                        arr.point.GetPositionZ(), arr.point.GetMapId(), bot->GetTeamId());

                if (fromTaxi && toTaxi && fromTaxi != toTaxi)
                    state.route = sTravelNodeMap.FindTaxiPath(fromTaxi, toTaxi);

                if (state.route.empty())
                {
                    state.stepIdx += 2;
                    return true;
                }
            }

            TravelMgr::FlightMasterInfo const* fmInfo = sTravelMgr.GetNearestFlightMasterInfo(bot);
            if (!fmInfo)
            {
                state.route.clear();
                state.stepIdx += 2;
                return true;
            }

            if (bot->GetDistance(fmInfo->pos) > INTERACTION_DISTANCE)
                return MoveTo(fmInfo->pos.GetMapId(), fmInfo->pos.GetPositionX(),
                              fmInfo->pos.GetPositionY(), fmInfo->pos.GetPositionZ());

            ObjectGuid fmGuid = ObjectGuid::Create<HighGuid::Unit>(fmInfo->templateEntry, fmInfo->dbGuid);
            Creature* flightMaster = ObjectAccessor::GetCreature(*bot, fmGuid);
            if (!flightMaster || !flightMaster->IsAlive())
            {
                state.route.clear();
                state.stepIdx += 2;
                return true;
            }

            botAI->RemoveShapeshift();
            if (bot->IsMounted())
                bot->Dismount();

            bot->ActivateTaxiPathTo(state.route, flightMaster, 0);

            state.route.clear();
            state.stepIdx += 2;
            return true;
        }

        case PathNodeType::NODE_TELEPORT:
        {
            // Teleport-spell node (e.g. mage portals). Not implemented
            // — abort the plan instead of silently teleporting the
            // bot. The plan executor regards this node as terminal.
            state.Reset();
            return false;
        }

        // Phase B: enum value 7 is now NODE_STATIC_PORTAL (was
        // NODE_FLYING_MOUNT). This legacy branch is superseded by the new
        // HandleSpecialMovement NODE_STATIC_PORTAL (GO teleport) leg and is
        // slated for Phase C deletion; kept compiling under the renamed case.
        case PathNodeType::NODE_STATIC_PORTAL:
        {
            // Static-portal node not handled by the legacy plan executor —
            // abort. The new HandleSpecialMovement dispatch handles GO portals.
            state.Reset();
            return false;
        }
        default:
        {
            LOG_ERROR("playerbots",
                "[TravelPlan] Bot {} encountered unknown PathNodeType ({}); resetting plan",
                bot->GetName(), static_cast<uint32>(pt.type));
            state.Reset();
            return false;
        }
    }
    return false;
}

Transport* MovementAction::GetTransportForPosTolerant(Map* map, WorldObject* ref, uint32 phaseMask, float x, float y, float z)
{
    if (!map || !ref)
        return nullptr;

    std::array<float, 4> const probes = { z, z + 0.5f, z + 1.5f, z - 0.5f };
    for (float const pz : probes)
    {
        if (Transport* transport = map->GetTransportForPos(phaseMask, x, y, pz, ref))
            return transport;
    }
    return nullptr;
}

bool MovementAction::FindBoardingPointOnTransport(Map* map, Transport* expectedTransport, WorldObject* ref,
    float refX, float refY, float refZ, float botX, float botY, float botZ, float& outX, float& outY, float& outZ)
{
    if (!map || !expectedTransport || !ref)
        return false;

    uint32 const phaseMask = ref->GetPhaseMask();
    if (GetTransportForPosTolerant(map, ref, phaseMask, refX, refY, refZ)
        != expectedTransport)
        return false;

    float const probeZ = std::max(refZ, botZ);
    float const dx2 = botX - refX;
    float const dy2 = botY - refY;
    float const dist2d = std::sqrt(dx2 * dx2 + dy2 * dy2);
    int32 const steps = std::clamp(static_cast<int32>(dist2d / 0.75f), 10, 28);
    float const dx = (botX - refX) / static_cast<float>(steps);
    float const dy = (botY - refY) / static_cast<float>(steps);

    if (map->GetTransportForPos(phaseMask, refX, refY, probeZ, ref) != expectedTransport)
        return false;

    float lastX = refX;
    float lastY = refY;
    bool found = false;

    for (int32 i = 1; i <= steps; ++i)
    {
        float const px = refX + dx * i;
        float const py = refY + dy * i;
        Transport* const t = GetTransportForPosTolerant(map, ref, phaseMask, px, py, probeZ);
        if (t != expectedTransport)
            break;
        lastX = px;
        lastY = py;
        found = true;
    }

    if (!found)
        return false;

    outX = lastX;
    outY = lastY;
    outZ = refZ;
    return true;
}

bool MovementAction::BoardTransport(Transport* transport)
{
    if (!transport || transport->IsStaticTransport())
        return false;

    Map* map = bot->GetMap();
    if (!map)
        return false;

    // Already on this transport
    if (bot->GetTransport() == transport)
        return true;

    // Check if bot is on the transport surface
    float probeZ = std::max(bot->GetPositionZ(), transport->GetPositionZ());
    Transport* surface = GetTransportForPosTolerant(map, bot, bot->GetPhaseMask(), bot->GetPositionX(),
        bot->GetPositionY(), probeZ);

    if (surface == transport)
    {
        transport->AddPassenger(bot, true);
        bot->StopMovingOnCurrentPos();
        EmitDebugMove("TravelPlan:transport-board", "teleport", transport->GetPositionX(),
                      transport->GetPositionY(), transport->GetPositionZ());
        return true;
    }
    // Not on surface — move toward the transport
    float destX = transport->GetPositionX();
    float destY = transport->GetPositionY();
    float destZ = transport->GetPositionZ();

    // Try to find nearest boarding edge
    float edgeX, edgeY, edgeZ;
    if (FindBoardingPointOnTransport(map, transport, transport, transport->GetPositionX(), transport->GetPositionY(),
        transport->GetPositionZ(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), edgeX, edgeY, edgeZ))
    {
        destX = edgeX;
        destY = edgeY;
        destZ = edgeZ;
    }

    // MovePoint without pathfinding (transport is a moving object)
    if (MotionMaster* mm = bot->GetMotionMaster())
    {
        if (bot->IsSitState())
            bot->SetStandState(UNIT_STAND_STATE_STAND);

        mm->MovePoint(0, destX, destY, destZ, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false, false);
        EmitDebugMove("TravelPlan:transport-walk", "spline", destX, destY, destZ);
    }

    return false;
}
