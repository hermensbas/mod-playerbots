/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MOVEMENTACTIONS_H
#define PLAYERBOTS_MOVEMENTACTIONS_H

#include "Action.h"
#include "LastMovementValue.h"
#include "PathGenerator.h"
#include "PlayerbotAIConfig.h"
#include <cmath>

class Player;
class PlayerbotAI;
class Unit;
class WorldObject;

struct Position;

#define ANGLE_45_DEG (static_cast<float>(M_PI) / 4.f)
#define ANGLE_90_DEG M_PI_2
#define ANGLE_120_DEG (2.f * static_cast<float>(M_PI) / 3.f)

// Default acceptable path types for GeneratePath
constexpr uint32 DEFAULT_PATH_ACCEPT_MASK = PATHFIND_NORMAL | PATHFIND_INCOMPLETE;
constexpr uint32 RELAXED_PATH_ACCEPT_MASK = PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;

struct PathResult
{
    Movement::PointsArray points;
    G3D::Vector3 actualEnd;
    G3D::Vector3 end;
    PathType pathType;
    bool reachable;
};

class MovementAction : public Action
{
public:
    MovementAction(PlayerbotAI* botAI, std::string const name);

protected:
    // Emit a one-line trace describing the imminent movement. No-op
    // unless the bot has the "debug move" non-combat strategy.
    // Subclasses (e.g. NewRpgBaseAction) may override to append richer
    // context such as RPG status and target name. Optional `extra`
    // is appended verbatim (use it to attach hop labels like
    // "node:Stormwind innkeeper" or fallback reasons).
    virtual void EmitDebugMove(char const* method, char const* generator, float x, float y, float z, char const* extra = nullptr);

    bool JumpTo(uint32 mapId, float x, float y, float z, MovementPriority priority = MovementPriority::MOVEMENT_NORMAL);
    bool MoveNear(uint32 mapId, float x, float y, float z, float distance = sPlayerbotAIConfig.ContactDistance,
                  MovementPriority priority = MovementPriority::MOVEMENT_NORMAL);
    bool MoveToLOS(WorldObject* target, bool ranged = false);
    bool MoveTo(uint32 mapId, float x, float y, float z, bool idle = false, bool react = false,
                bool normal_only = false, bool exact_waypoint = false,
                MovementPriority priority = MovementPriority::MOVEMENT_NORMAL, bool lessDelay = false,
                bool backwards = false);
    bool MoveTo(WorldObject* target, float distance = 0.0f,
                MovementPriority priority = MovementPriority::MOVEMENT_NORMAL);
    bool MoveNear(WorldObject* target, float distance = sPlayerbotAIConfig.ContactDistance,
                  MovementPriority priority = MovementPriority::MOVEMENT_NORMAL);
    float GetFollowAngle();
    bool Follow(Unit* target, float distance = sPlayerbotAIConfig.FollowDistance);
    bool Follow(Unit* target, float distance, float angle);
    bool ChaseTo(WorldObject* obj, float distance = 0.0f);
    bool ReachCombatTo(Unit* target, float distance = 0.0f);
    float MoveDelay(float distance, bool backwards = false);
    void WaitForReach(float distance);
    void SetNextMovementDelay(float delayMillis);
    bool IsMovingAllowed(WorldObject* target);
    bool IsDuplicateMove(float x, float y, float z);
    bool IsWaitingForLastMove(MovementPriority priority);
    bool IsMovingAllowed();
    bool Flee(Unit* target);
    void ClearIdleState();
    void UpdateMovementState();
    bool MoveAway(Unit* target, float distance = sPlayerbotAIConfig.FleeDistance, bool backwards = false);
    bool MoveFromGroup(float distance);
    bool Move(float angle, float distance);
    bool GetRandomPointInCone(Position const& origin, float orientation, float halfArc, float minDistance,
                              float maxDistance, Position& out, float maxBelow = 3.0f, float maxAbove = 3.0f,
                              uint8 attempts = 8);
    bool MoveInside(uint32 mapId, float x, float y, float z, float distance = sPlayerbotAIConfig.FollowDistance,
                    MovementPriority priority = MovementPriority::MOVEMENT_NORMAL);
    void CreateWp(Player* wpOwner, float x, float y, float z, float o, uint32 entry, bool important = false);
    Position BestPositionForMeleeToFlee(Position pos, float radius);
    Position BestPositionForRangedToFlee(Position pos, float radius);
    bool FleePosition(Position pos, float radius, uint32 minInterval = 1000);
    bool CheckLastFlee(float curAngle, std::list<FleeInfo>& infoList);

    PathResult GeneratePath(float x, float y, float z, uint32 acceptMask = DEFAULT_PATH_ACCEPT_MASK, bool forceDestination = false);

    bool GetTravelPlan(TravelPlan& plan, WorldPosition destination);
    bool ExecuteTravelPlan(TravelPlan& state);

    // ---- A->B movement dispatch (long-path routing + cross-continent legs) ----
    // Orchestrator: validates the endpoint, resolves a full route (node graph
    // for cross-map / long distance, navmesh otherwise), then either dispatches
    // a precomputed spline walk or hands off to a special-movement leg.
    bool MoveTo2(WorldPosition const& endPos, bool idle = false, bool react = false,
                 bool noPath = false, bool ignoreEnemyTargets = false);
    // Coordinate forwarding entry. Implemented as a MoveTo2 OVERLOAD (distinct
    // parameter list) rather than reusing the source name MoveTo(mapId,...):
    // the existing MoveTo(mapId, x, y, z, normal_only, exact_waypoint, ...) and
    // NewRpgBaseAction::MoveFarTo(WorldPosition) both already exist, so an
    // overload of MoveTo2 avoids clobbering / name-hiding either of them.
    bool MoveTo2(uint32 mapId, float x, float y, float z, bool idle = false,
                 bool react = false, bool noPath = false, bool ignoreEnemyTargets = false);

    // Routing brain. Reuses the last long path when it still leads to roughly
    // the same destination; otherwise picks node-graph routing for cross-map /
    // long-distance and navmesh routing otherwise. Returns an EMPTY path on
    // pathfinding failure (never fabricates a 1-point path).
    TravelPath ResolveMovePath(WorldPosition const& startPosition, WorldPosition const& endPosition,
                               Unit* mover, LastMovement& lastMove);
    // Plays back the already-computed point path with MoveSplinePath
    // (usePath=true) to avoid the engine re-generating it and freezing the bot.
    void DispatchMovement(TravelPath movePath, bool generatePath, bool masterWalking);
    // Handles the cross-continent legs: static GO portals, area triggers,
    // transports, flight paths and teleport spells (e.g. hearthstone).
    bool HandleSpecialMovement(TravelPath& path);
    // Resolve the controlling unit (the bot, or the vehicle it controls).
    Unit* GetMover(Player* bot);

    // Flight helpers (free-flying mounts only).
    bool FlyDirect(WorldPosition const& startPosition, WorldPosition const& endPosition,
                   WorldPosition& movePosition, TravelPath movePath);
    void UpdateFlyingState(WorldPosition& movePosition, float totalDistance, float originalZ,
                           float maxDist, bool isWalking);

    // Cross-continent leg helpers.
    static bool UseTaxi(PlayerbotAI* botAI, uint32 entry, bool needNpc);
    static bool MoveOnTransport(PlayerbotAI* botAI, Transport* transport, bool doTeleport);
    static bool MoveOffTransport(PlayerbotAI* botAI, WorldPosition exitPos, bool doTeleport);
    static bool UseTransport(PlayerbotAI* botAI, uint32 entry, WorldPosition dockPosition,
                             WorldPosition exitPosition, bool doTeleport);
    bool WaitForTransport();

    // Reroute a precomputed point path around known AoE hazards. v1 is a
    // pass-through (no hazard value wired); see PORT-TODO in the .cpp.
    bool GeneratePathAvoidingHazards(std::vector<WorldPosition>& movePath);

    // Transport boarding helpers (shared by FollowAction and travel plan)
    static Transport* GetTransportForPosTolerant(Map* map, WorldObject* ref,
        uint32 phaseMask, float x, float y, float z);
    static bool FindBoardingPointOnTransport(Map* map, Transport* transport,
        WorldObject* ref, float refX, float refY, float refZ,
        float botX, float botY, float botZ,
        float& outX, float& outY, float& outZ);
    bool BoardTransport(Transport* transport);

private:
    bool LaunchWalkSpline(TravelPlan& state);
    bool CheckSplineProgress(TravelPlan& state);
    bool MoveToSpline(TravelPlan& state, WorldPosition target);
    // Per-segment mmap refinement of a travel-node-graph walk batch.
    // The graph stores offline-baked coords whose straight-line
    // interpolation may pass through geometry the bot can't actually
    // traverse. Returns false if any segment is unwalkable per the
    // live navmesh, in which case the caller should abort the plan.
    bool RefineWalkPoints(std::vector<G3D::Vector3>& walkPoints);

protected:
    struct CheckAngle
    {
        float angle;
        bool strict;
    };

private:
    bool wasMovementRestricted = false;
    void DoMovePoint(Unit* unit, float x, float y, float z, bool generatePath, bool backwards);
};

class FleeAction : public MovementAction
{
public:
    FleeAction(PlayerbotAI* botAI) : MovementAction(botAI, "flee")
    {
    }

    bool Execute(Event event) override;
    bool isUseful() override;
};

class FleeWithPetAction : public MovementAction
{
public:
    FleeWithPetAction(PlayerbotAI* botAI) : MovementAction(botAI, "flee with pet") {}

    bool Execute(Event event) override;
};

class AvoidAoeAction : public MovementAction
{
public:
    AvoidAoeAction(PlayerbotAI* botAI, int moveInterval = 1000)
        : MovementAction(botAI, "avoid aoe"), moveInterval(moveInterval)
    {
    }

    bool isUseful() override;
    bool Execute(Event event) override;

protected:
    bool AvoidAuraWithDynamicObj();
    bool AvoidGameObjectWithDamage();
    bool AvoidUnitWithDamageAura();
    time_t lastTellTimer = 0;
    int lastMoveTimer = 0;
    int moveInterval;
};

class CombatFormationMoveAction : public MovementAction
{
public:
    CombatFormationMoveAction(PlayerbotAI* botAI, std::string name = "combat formation move", int moveInterval = 1000)
        : MovementAction(botAI, name), moveInterval(moveInterval)
    {
    }

    bool isUseful() override;
    bool Execute(Event event) override;

protected:
    Position AverageGroupPos(float dis = sPlayerbotAIConfig.SightDistance, bool ranged = false, bool self = false);
    Player* NearestGroupMember(float dis = sPlayerbotAIConfig.SightDistance);
    float AverageGroupAngle(Unit* from, bool ranged = false, bool self = false);
    Position GetNearestPosition(std::vector<Position> const& positions);
    int lastMoveTimer = 0;
    int moveInterval;
};

class TankFaceAction : public CombatFormationMoveAction
{
public:
    TankFaceAction(PlayerbotAI* botAI) : CombatFormationMoveAction(botAI, "tank face") {}

    bool Execute(Event event) override;
};

class RearFlankAction : public MovementAction
{
    // 90 degree minimum angle prevents any frontal cleaves/breaths and avoids parry-hasting the current target.
    // 120 degree maximum angle leaves a 120 degree symmetrical cone at the tail end which is usually enough to avoid
    // tail swipes. Some dragons or mobs may have different danger zone angles, override if needed.
public:
    RearFlankAction(PlayerbotAI* botAI, float distance = 0.0f, float minAngle = ANGLE_90_DEG,
                    float maxAngle = ANGLE_120_DEG, std::string const& name = "rear flank")
        : MovementAction(botAI, name)
    {
        this->distance = distance;
        this->minAngle = minAngle;
        this->maxAngle = maxAngle;
    }

    std::string const GetTargetName() override { return "current target"; };
    bool Execute(Event event) override;
    bool isUseful() override;

protected:
    float distance, minAngle, maxAngle;
};

class BossRearFlankAction : public RearFlankAction
{
public:
    BossRearFlankAction(PlayerbotAI* botAI, std::string bossName, float distance = 0.0f, float minAngle = ANGLE_90_DEG,
                        float maxAngle = ANGLE_120_DEG)
        : RearFlankAction(botAI, distance, minAngle, maxAngle, bossName + " rear flank"),
          bossName(std::move(bossName)) {}

    Unit* GetTarget() override;

protected:
    const std::string bossName;
};

class DisperseSetAction : public Action
{
public:
    DisperseSetAction(PlayerbotAI* botAI, std::string const name = "disperse set") : Action(botAI, name) {}

    bool Execute(Event event) override;
    float DEFAULT_DISPERSE_DISTANCE_RANGED = 5.0f;
    float DEFAULT_DISPERSE_DISTANCE_MELEE = 2.0f;
};

class RunAwayAction : public MovementAction
{
public:
    RunAwayAction(PlayerbotAI* botAI) : MovementAction(botAI, "runaway") {}

    bool Execute(Event event) override;
};

class MoveToLootAction : public MovementAction
{
public:
    MoveToLootAction(PlayerbotAI* botAI) : MovementAction(botAI, "move to loot") {}

    bool Execute(Event event) override;

private:
    // Progress watchdog: stop chasing a loot target we are not actually getting closer to.
    ObjectGuid lootWatchGuid;
    float lootWatchDistance = -1.0f;
    uint32 lootWatchMs = 0;
    uint32 lootWatchLastEvalMs = 0;
};

class MoveOutOfEnemyContactAction : public MovementAction
{
public:
    MoveOutOfEnemyContactAction(PlayerbotAI* botAI) : MovementAction(botAI, "move out of enemy contact") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

class SetFacingTargetAction : public Action
{
public:
    SetFacingTargetAction(PlayerbotAI* botAI) : Action(botAI, "set facing") {}

    bool Execute(Event event) override;
    bool isUseful() override;
    bool isPossible() override;
};

class SetBehindTargetAction : public CombatFormationMoveAction
{
public:
    SetBehindTargetAction(PlayerbotAI* botAI) : CombatFormationMoveAction(botAI, "set behind") {}

    bool Execute(Event event) override;
};

class MoveOutOfCollisionAction : public MovementAction
{
public:
    MoveOutOfCollisionAction(PlayerbotAI* botAI) : MovementAction(botAI, "move out of collision") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

class MoveRandomAction : public MovementAction
{
public:
    MoveRandomAction(PlayerbotAI* botAI) : MovementAction(botAI, "move random") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

class MoveInsideAction : public MovementAction
{
public:
    MoveInsideAction(PlayerbotAI* botAI, float x, float y, float distance = 5.0f) : MovementAction(botAI, "move inside")
    {
        this->x = x;
        this->y = y;
        this->distance = distance;
    }
    virtual bool Execute(Event event);

protected:
    float x, y, distance;
};

class RotateAroundTheCenterPointAction : public MovementAction
{
public:
    RotateAroundTheCenterPointAction(PlayerbotAI* botAI, std::string name, float center_x, float center_y,
                                     float radius = 40.0f, uint32 intervals = 16, bool clockwise = true,
                                     float start_angle = 0)
        : MovementAction(botAI, name)
    {
        this->center_x = center_x;
        this->center_y = center_y;
        this->radius = radius;
        this->intervals = intervals;
        this->clockwise = clockwise;
        this->call_counters = 0;
        for (int i = 0; i < intervals; i++)
        {
            float angle = start_angle + 2 * M_PI * i / intervals;
            waypoints.push_back(std::make_pair(center_x + cos(angle) * radius, center_y + sin(angle) * radius));
        }
    }
    virtual bool Execute(Event event);

protected:
    virtual uint32 GetCurrWaypoint() { return 0; }
    uint32 FindNearestWaypoint();
    float center_x, center_y, radius;
    uint32 intervals, call_counters;
    bool clockwise;
    std::vector<std::pair<float, float>> waypoints;
};

class MoveFromGroupAction : public MovementAction
{
public:
    MoveFromGroupAction(PlayerbotAI* botAI, std::string const name = "move from group") : MovementAction(botAI, name) {}

    bool Execute(Event event) override;
};

class MoveAwayFromCreatureAction : public MovementAction
{
public:
    MoveAwayFromCreatureAction(PlayerbotAI* botAI, std::string name, uint32 creatureId, float range, bool alive = true)
        : MovementAction(botAI, name), creatureId(creatureId), range(range), alive(alive)
    {
    }

    bool Execute(Event event) override;
    bool isPossible() override;

private:
    uint32 creatureId;
    float range;
    bool alive;
};

class MoveAwayFromPlayerWithDebuffAction : public MovementAction
{
public:
    MoveAwayFromPlayerWithDebuffAction(PlayerbotAI* botAI, std::string name, uint32 spellId, float range)
        : MovementAction(botAI, name), spellId(spellId), range(range)
    {
    }

    bool Execute(Event event) override;
    bool isPossible() override;

private:
    uint32 spellId;
    float range;
};

#endif
