/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ChooseTargetActions.h"

#include "Battleground.h"
#include "ChooseRpgTargetAction.h"
#include "Event.h"
#include "LootObjectStack.h"
#include "NewRpgStrategy.h"
#include "Playerbots.h"
#include "PossibleRpgTargetsValue.h"
#include "PvpTriggers.h"
#include "PvpValues.h"
#include "RtiTargetValue.h"
#include "ServerFacade.h"

bool AttackEnemyPlayerAction::isUseful()
{
    if (PlayerHasFlag::IsCapturingFlag(bot))
        return false;

    return !sPlayerbotAIConfig.IsPvpProhibited(bot->GetZoneId(), bot->GetAreaId());
}

bool AttackEnemyFlagCarrierAction::isUseful()
{
    Unit* target = context->GetValue<Unit*>("enemy flag carrier")->Get();
    Battleground* bg = bot->GetBattleground();
    if (bg && bg->GetBgTypeID(true) == BATTLEGROUND_WS)
    {
        if (botAI->GetState() == BOT_STATE_COMBAT && AI_VALUE(Unit*, "current target") == target)
            return false;
        // A queued action can outlive the trigger that selected the carrier.
        return CanRespondToWsgCarrier(botAI, target);
    }

    if (!target || !ServerFacade::instance().IsDistanceLessOrEqualThan(
                       ServerFacade::instance().GetDistance2d(bot, target), 100.0f))
        return false;

    return PlayerHasFlag::IsCapturingFlag(bot);
}

bool AggressiveTargetAction::isUseful()
{
    if (bot->IsInCombat())
        return false;

    return true;
}

bool DropTargetAction::Execute(Event /*event*/)
{
    Unit* target = context->GetValue<Unit*>("current target")->Get();
    if (target && target->isDead())
    {
        ObjectGuid guid = target->GetGUID();
        if (guid)
            context->GetValue<LootObjectStack*>("available loot")->Get()->Add(guid);
    }

    // ObjectGuid pullTarget = context->GetValue<ObjectGuid>("pull target")->Get();
    // GuidVector possible = botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get();

    // if (pullTarget && find(possible.begin(), possible.end(), pullTarget) == possible.end())
    // {
    //     context->GetValue<ObjectGuid>("pull target")->Set(ObjectGuid::Empty);
    // }

    context->GetValue<Unit*>("current target")->Set(nullptr);

    bot->SetTarget(ObjectGuid::Empty);
    bot->SetSelection(ObjectGuid());
    botAI->ChangeEngine(BOT_STATE_NON_COMBAT);
    if (bot->getClass() == CLASS_HUNTER) // Check for Hunter Class
    {
        Spell const* spell = bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL); // Get the current spell being cast by the bot
        if (spell && spell->m_spellInfo->Id == 75) //Check spell is not nullptr before accessing m_spellInfo
            bot->InterruptSpell(CURRENT_AUTOREPEAT_SPELL); // Interrupt Auto Shot
    }
    bot->AttackStop();

    // if (Pet* pet = bot->GetPet())
    // {
    //     if (CreatureAI* creatureAI = ((Creature*)pet)->AI())
    //     {
    //         pet->SetReactState(REACT_PASSIVE);
    //         pet->GetCharmInfo()->SetCommandState(COMMAND_FOLLOW);
    //         pet->GetCharmInfo()->SetIsCommandFollow(true);
    //         pet->AttackStop();
    //         pet->GetCharmInfo()->IsReturning();
    //         pet->GetMotionMaster()->MoveFollow(bot, PET_FOLLOW_DIST, pet->GetFollowAngle());
    //     }
    // }

    return true;
}

bool TargetRequesterAction::Execute(Event event)
{
    Player* requester = event.getOwner();

    if (!requester)
        return false;

    bot->SetSelection(requester->GetGUID());

    botAI->TellMasterNoFacing(
        PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "target_requester_success",
            "Targeting %target",
            {{"%target", requester->GetName()}}));

    return true;
}

bool AttackAnythingAction::Execute(Event event)
{
    bool result = AttackAction::Execute(event);
    if (result)
    {
        if (Unit* grindTarget = GetTarget())
        {
            context->GetValue<ObjectGuid>("pull target")->Set(grindTarget->GetGUID());
            bot->GetMotionMaster()->Clear();
            // bot->StopMoving();
        }
    }

    return result;
}

// True when this creature IS a quest objective for the bot: a kill-credit
// target still needed, or a mob that drops a quest item the bot still
// needs. Used to exempt such mobs from the travel-focus attack gate — a
// spider that drops the venom we are questing for is the objective, not a
// distraction to walk past.
static bool IsQuestObjectiveCreature(Player* bot, Creature* creature)
{
    if (!creature)
        return false;

    uint32 const entry = creature->GetEntry();

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
            int32 req = quest->RequiredNpcOrGo[i];
            if (req > 0 && uint32(req) == entry && qs.CreatureOrGOCount[i] < quest->RequiredNpcOrGoCount[i])
                return true;
        }
    }

    // Mob drops a still-needed quest item (HaveQuestLootForPlayer already
    // filters by what this player still needs).
    if (CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(entry))
        if (uint32 lootId = ct->lootid)
            if (LootTemplates_Creature.HaveQuestLootForPlayer(lootId, bot))
                return true;

    return false;
}

bool AttackAnythingAction::isUseful()
{
    if (!bot || !botAI)  // Prevents invalid accesses
        return false;

    if (!botAI->AllowActivity(GRIND_ACTIVITY))  // Bot cannot be active
        return false;

    if (botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT))
        return false;

    if (bot->IsInCombat())
        return false;

    Unit* target = GetTarget();
    if (!target || !target->IsInWorld())  // Checks if the target is valid and in the world
        return false;

    std::string const name = std::string(target->GetName());
    if (!name.empty() &&
        (name.find("Dummy") != std::string::npos ||
         name.find("Charge Target") != std::string::npos ||
         name.find("Melee Target") != std::string::npos ||
         name.find("Ranged Target") != std::string::npos))
    {
        return false;
    }

    // Quest focus: while traveling for a quest — to the POI or back to
    // the turn-in — grinding is not useful at all. A relevance damp
    // cannot enforce this (the engine executes any popped action whose
    // multiplied relevance stays above zero), so gate it here. The one
    // exception is a target that would aggro us anyway: hostile, in
    // front, inside 1.5x its aggro range and within our level band —
    // fighting that on our own terms beats being jumped mid-travel.
    if (botAI->rpgInfo.GetStatus() == RPG_DO_QUEST)
    {
        auto* data = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data);
        bool const headingToTurnIn = data && data->questId &&
            bot->GetQuestStatus(data->questId) == QUEST_STATUS_COMPLETE;
        bool const travelingToPOI = data && !data->lastReachPOI;
        if (headingToTurnIn || travelingToPOI)
        {
            Creature* creature = target->ToCreature();
            // Exempt quest objectives while traveling TO the POI: a mob
            // that gives kill credit or drops a needed quest item IS the
            // objective (Webwood Venom spiders), so engage it rather than
            // walk past. Not exempt while heading to turn-in — that quest
            // is already complete, so its mobs are just distractions.
            if (travelingToPOI && IsQuestObjectiveCreature(bot, creature))
                return true;

            bool const wouldAggroAnyway = creature && creature->IsHostileTo(bot) &&
                bot->GetDistance(creature) < creature->GetAggroRange(bot) * 1.5f &&
                static_cast<int32>(creature->GetLevel()) <= static_cast<int32>(bot->GetLevel()) + 3 &&
                creature->CanStartAttack(bot);
            if (!wouldAggroAnyway)
                return false;
        }
    }

    return true;
}

bool AttackAnythingAction::isPossible() { return GetTarget() && AttackAction::isPossible(); }

bool DpsAssistAction::isUseful()
{
    if (PlayerHasFlag::IsCapturingFlag(bot))
        return false;

    return true;
}

bool AttackRtiTargetAction::Execute(Event /*event*/)
{
    Unit* rtiTarget = AI_VALUE(Unit*, "rti target");

    // Fallback: if the "rti target" value did not resolve a valid unit yet,
    // try to resolve the raid icon directly from the group.
    if (!rtiTarget)
    {
        if (Group* group = bot->GetGroup())
        {
            std::string const rti = AI_VALUE(std::string, "rti");
            int32 const index = RtiTargetValue::GetRtiIndex(rti);
            if (index >= 0)
            {
                ObjectGuid const guid = group->GetTargetIcon(index);
                if (!guid.IsEmpty())
                    rtiTarget = botAI->GetUnit(guid);
            }
        }
    }

    if (rtiTarget && rtiTarget->IsInWorld() && rtiTarget->GetMapId() == bot->GetMapId())
    {
        botAI->GetAiObjectContext()->GetValue<GuidVector>("prioritized targets")->Set({rtiTarget->GetGUID()});
        bool result = Attack(botAI->GetUnit(rtiTarget->GetGUID()));
        if (result)
        {
            context->GetValue<ObjectGuid>("pull target")->Set(rtiTarget->GetGUID());
            return true;
        }
    }
    else
        botAI->TellError("I dont see my rti attack target");

    return false;
}

bool AttackRtiTargetAction::isUseful()
{
    if (botAI->ContainsStrategy(STRATEGY_TYPE_HEAL))
        return false;

    return true;
}
