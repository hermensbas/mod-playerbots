/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGBASEACTION_H
#define PLAYERBOTS_NEWRPGBASEACTION_H

#include "LastMovementValue.h"
#include "MovementActions.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "PlayerbotAI.h"
#include "QuestDef.h"
#include "TravelMgr.h"

struct POIInfo
{
    G3D::Vector2 pos;
    int32 objectiveIdx;
};

/// A base (composition) class for all new rpg actions
/// All functions that may be shared by multiple actions should be declared here
/// And we should make all actions composable instead of inheritable
class NewRpgBaseAction : public MovementAction
{
public:
    NewRpgBaseAction(PlayerbotAI* botAI, std::string name) : MovementAction(botAI, name) {}

protected:
    /* MOVEMENT RELATED */
    bool MoveFarTo(WorldPosition dest);
    bool MoveWorldObjectTo(ObjectGuid guid, float distance = INTERACTION_DISTANCE);
    bool MoveRandomNear(float moveStep = 50.0f, MovementPriority priority = MovementPriority::MOVEMENT_NORMAL, WorldObject* center = nullptr);
    bool ForceToWait(uint32 duration, MovementPriority priority = MovementPriority::MOVEMENT_NORMAL);
    bool TakeFlight(std::vector<uint32> const& taxiNodes, Creature* flightMaster);

    /* QUEST RELATED CHECK */
    bool InteractWithNpcOrGameObjectForQuest(ObjectGuid guid);
    bool IsWithinInteractionDist(Object* object);
    uint32 BestRewardIndex(Quest const* quest);

    /* QUEST RELATED ACTION */
    bool SearchQuestGiverAndAcceptOrReward();
    bool AcceptQuest(Quest const* quest, ObjectGuid guid);
    bool TurnInQuest(Quest const* quest, ObjectGuid guid);
    bool OrganizeQuestLog();

    /* QUEST PROGRESSION HELPERS (at POI) */
    // Walk to a GO that drops a needed quest item. The loot strategy
    // opens and loots it once in range.
    bool TryLootQuestGO(ObjectGuid& pursuedGO, float searchRange = 60.0f);

    // Walk to / use a GO that is itself the objective (rune, lever,
    // altar, coffin — RequiredNpcOrGo with a negative entry).
    bool TryUseQuestGO(ObjectGuid& pursuedGO, float searchRange = 60.0f);

    // Fire a quest item's OnUse spell at the right target: a spell-focus
    // GO (moonwell), a required creature, or the bot itself.
    bool TryUseQuestItem(ObjectGuid& pursuedGO, ObjectGuid& pursuedTarget, float searchRange = 60.0f);

    // True when a quest-relevant mob is within range — used during
    // travel so we yield to attack-anything instead of running past.
    bool HasNearbyQuestMob(float range = 20.0f);

    // The nearest quest-relevant mob (kill credit or needed item drop)
    // within range, else nullptr. Lets the gather roam head toward mobs
    // in sight instead of camping for local respawns.
    Creature* NearestQuestMob(float range = 20.0f);

protected:
    // Last object the approach failed to path to, skipped by the pickers
    // for a short while so the bot tries an ALTERNATIVE (the next
    // mushroom over) instead of re-committing the same unreachable one
    // every tick forever.
    ObjectGuid lastUnreachableGO{};
    uint32 lastUnreachableGOMs{0};

    void MarkUnreachable(ObjectGuid guid);
    bool IsMarkedUnreachable(ObjectGuid guid) const;

    bool GetQuestPOIPosAndObjectiveIdx(uint32 questId, std::vector<POIInfo>& poiInfo, bool toComplete = false);
    float ResolveQuestPOIDestZ(Quest const* quest, int32 objectiveIdx, float dx, float dy, float surfaceZ);
    float ResolveQuestTurnInDestZ(uint32 questId, float dx, float dy, float surfaceZ);
    float NearestQuestSpawnZ(std::vector<uint32> const& creatureEntries, std::vector<uint32> const& goEntries,
                             float dx, float dy, float surfaceZ);
    static WorldPosition SelectRandomGrindPos(Player* bot);
    static WorldPosition SelectRandomCampPos(Player* bot);
    bool SelectRandomFlightTaxiNode(uint32& flightMasterEntry, WorldPosition& flightMasterPos, std::vector<uint32>& path);
    bool RandomChangeStatus(std::vector<NewRpgStatus> candidateStatus);
    bool CheckRpgStatusAvailable(NewRpgStatus status);

protected:
    /* FOR QUEST GIVER */
    // Time on one giver, walking and interacting, after which it is skipped until the next IDLE.
    const uint32 questGiverGiveUpTime = 60 * IN_MILLISECONDS;
    // A longer pause (combat, eating) starts the timer over.
    const uint32 questGiverRetryGap = 5 * IN_MILLISECONDS;
};

#endif
