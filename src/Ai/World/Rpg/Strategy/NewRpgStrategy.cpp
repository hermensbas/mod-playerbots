/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgStrategy.h"
#include "NewRpgMultipliers.h"

#include "Action.h"
#include "NewRpgInfo.h"
#include "Player.h"
#include "PlayerbotAI.h"

static bool IsGatherObjectiveForDoQuest(NewRpgInfo::DoQuest const* data)
{
    if (!data || !data->quest)
        return false;
    Quest const* q = data->quest;
    int32 obj = data->objectiveIdx;
    // objectiveIdx is -1 while heading to turn-in (DoCompletedQuest) and can
    // still be -1 here when the quest leaves COMPLETE again (failed timer,
    // required item destroyed) before the RPG action resets state.
    if (obj < 0)
        return false;
    if (obj < QUEST_OBJECTIVES_COUNT)
    {
        int32 entry = q->RequiredNpcOrGo[obj];
        if (entry < 0)  // GO objective
            return true;
        if (entry == 0 && obj < QUEST_ITEM_OBJECTIVES_COUNT && q->RequiredItemId[obj])
            return true;
    }
    else if (obj < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
    {
        return true;
    }
    // source-item quest: need to find the right target to use it on
    if (q->GetSrcItemId())
        return true;
    return false;
}

float NewRpgDoQuestMultiplier::GetValue(Action* action)
{
    if (!action || action->getName() != "attack anything")
        return 1.0f;

    NewRpgInfo& info = botAI->rpgInfo;
    if (info.GetStatus() != RPG_DO_QUEST)
        return 1.0f;

    auto* data = std::get_if<NewRpgInfo::DoQuest>(&info.data);
    if (!data)
        return 1.0f;

    // heading back to turn in, don't get sidetracked
    if (data->questId && bot->GetQuestStatus(data->questId) == QUEST_STATUS_COMPLETE)
        return 0.15f;

    // at POI: gather stays low so mobs don't pull us off the cluster;
    // kill runs full so attack-anything drives behavior
    if (data->lastReachPOI)
        return IsGatherObjectiveForDoQuest(data) ? 0.30f : 1.0f;

    // traveling
    return 0.20f;
}

NewRpgStrategy::NewRpgStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

std::vector<NextAction> NewRpgStrategy::getDefaultActions()
{
    // must outrank grind
    return {
        NextAction("new rpg status update", 11.0f)
    };
}

void NewRpgStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(
        new TriggerNode(
            "go grind status",
            {
                NextAction("new rpg go grind", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "go camp status",
            {
                NextAction("new rpg go camp", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "wander random status",
            {
                NextAction("new rpg wander random", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "wander npc status",
            {
                NextAction("new rpg wander npc", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "do quest status",
            {
                // 4.5: above attack-anything (4.0), below loot (5.0+)
                NextAction("new rpg do quest", 4.5f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "travel flight status",
            {
                NextAction("new rpg travel flight", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "outdoor pvp status",
            {
                NextAction("new rpg outdoor pvp", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "do gather status",
            {
                NextAction("new rpg do gather", 3.0f)
            }
        )
    );
}

void NewRpgStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new NewRpgQuestGiverMultiplier(botAI));
    multipliers.push_back(new NewRpgDoQuestMultiplier(botAI));
}
