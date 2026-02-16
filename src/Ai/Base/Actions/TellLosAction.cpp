/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "TellLosAction.h"
#include <algorithm>
#include <istream>
#include <sstream>
#include <vector>

#include "ChatHelper.h"
#include "Event.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "Playerbots.h"
#include "StatsWeightCalculator.h"
#include "World.h"
#include "Group.h"
#include "WorldSession.h"


static bool IsLosResponder(PlayerbotAI* botAI)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot)
        return false;

    Group* group = bot->GetGroup();
    if (!group)
        return true;

    // Prefer the first bot found in the group iteration.
    Player* responder = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member)
            continue;

        WorldSession* session = member->GetSession();
        if (!session || !session->IsBot())
            continue;

        responder = member;
        break;
    }

    if (responder)
        return responder == bot;

    // Fallback: choose the smallest GUID among bots (stable selection).
    ObjectGuid minGuid;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member)
            continue;

        WorldSession* session = member->GetSession();
        if (!session || !session->IsBot())
            continue;

        if (!minGuid || member->GetGUID() < minGuid)
            minGuid = member->GetGUID();
    }

    if (!minGuid)
        return true;

    return bot->GetGUID() == minGuid;
}

bool TellLosAction::Execute(Event event)
{
    if (!IsLosResponder(botAI))
        return true;

    std::string const param = event.getParam();
    Player* owner = event.getOwner();
    if (!owner)
        owner = botAI->GetMaster();

    if (param.empty() || param == "targets")
    {
        ListUnits("--- Targets ---", *context->GetValue<GuidVector>("possible targets"));
        ListUnits("--- Targets (All) ---", *context->GetValue<GuidVector>("all targets"));
    }

    if (param.empty() || param == "npcs")
    {
        ListUnits("--- NPCs ---", *context->GetValue<GuidVector>("nearest npcs"));
    }

    if (param.empty() || param == "corpses")
    {
        ListUnits("--- Corpses ---", *context->GetValue<GuidVector>("nearest corpses"));
    }

    if (param.empty() || param == "gos" || param == "game objects")
    {
        ListGameObjects("--- Game objects ---", *context->GetValue<GuidVector>("nearest game objects"), owner);
    }

    if (param.empty() || param == "players")
    {
        ListUnits("--- Friendly players ---", *context->GetValue<GuidVector>("nearest friendly players"));
    }

    if (param.empty() || param == "triggers")
    {
        ListUnits("--- Triggers ---", *context->GetValue<GuidVector>("possible triggers"));
    }

    return true;
}

void TellLosAction::ListUnits(std::string const title, GuidVector units)
{
    botAI->TellMaster(title);

    for (ObjectGuid const guid : units)
    {
        if (Unit* unit = botAI->GetUnit(guid))
        {
            botAI->TellMaster(unit->GetNameForLocaleIdx(sWorld->GetDefaultDbcLocale()));
        }
    }
}
void TellLosAction::ListGameObjects(std::string const title, GuidVector gos, Player* owner)
{
    botAI->TellMaster(title);

    struct ListedGo
    {
        ObjectGuid guid;
        float dist;
    };

    std::vector<ListedGo> filtered;
    filtered.reserve(gos.size());

    for (ObjectGuid const guid : gos)
    {
        GameObject* go = botAI->GetGameObject(guid);
        if (!go)
            continue;

        float dist = bot->GetDistance2d(go);
        if (dist > 30.0f)
            continue;

        filtered.push_back({go->GetGUID(), dist});
    }

    // Farthest first so the closest object is printed last (most visible in chat).
    std::sort(filtered.begin(), filtered.end(), [](ListedGo const& a, ListedGo const& b) {
        return a.dist > b.dist;
    });

    std::vector<ObjectGuid> listed;
    listed.reserve(filtered.size());

    uint32 index = 1;
    for (ListedGo const& entry : filtered)
    {
        GameObject* go = botAI->GetGameObject(entry.guid);
        if (!go)
            continue;

        listed.push_back(entry.guid);

        uint32 distM = entry.dist >= 0.0f ? uint32(entry.dist + 0.5f) : 0;

        std::ostringstream out;
        out << "[" << index << "] (" << distM << "m) " << go->GetName();
        botAI->TellMaster(out.str());
        ++index;
    }

    if (owner)
        PlayerbotAI::SetSharedLosGameObjects(owner->GetGUID(), listed, owner);
}

bool TellAuraAction::Execute(Event /*event*/)
{
    botAI->TellMaster("--- Auras ---");
    sLog->outMessage("playerbot", LOG_LEVEL_DEBUG, "--- Auras ---");
    Unit::AuraApplicationMap& map = bot->GetAppliedAuras();
    for (Unit::AuraApplicationMap::iterator i = map.begin(); i != map.end(); ++i)
    {
        Aura* aura = i->second->GetBase();
        if (!aura)
            continue;
        const std::string auraName = aura->GetSpellInfo()->SpellName[0];
        sLog->outMessage("playerbot", LOG_LEVEL_DEBUG, "Info of Aura - name: " + auraName);
        AuraObjectType type = aura->GetType();
        WorldObject* owner = aura->GetOwner();
        std::string owner_name = owner ? owner->GetName() : "unknown";
        float distance = bot->GetDistance2d(owner);
        Unit* caster = aura->GetCaster();
        std::string caster_name = caster ? caster->GetName() : "unknown";
        bool is_area = aura->IsArea();
        int32 duration = aura->GetDuration();
        int32 spellId = aura->GetSpellInfo()->Id;
        bool isPositive = aura->GetSpellInfo()->IsPositive();
        sLog->outMessage("playerbot", LOG_LEVEL_DEBUG,
                         "Info of Aura - name: " + auraName + " caster: " + caster_name + " type: " +
                             std::to_string(type) + " owner: " + owner_name + " distance: " + std::to_string(distance) +
                             " isArea: " + std::to_string(is_area) + " duration: " + std::to_string(duration) +
                             " spellId: " + std::to_string(spellId) + " isPositive: " + std::to_string(isPositive));

        botAI->TellMaster("Info of Aura - name: " + auraName + " caster: " + caster_name + " type: " +
                          std::to_string(type) + " owner: " + owner_name + " distance: " + std::to_string(distance) +
                          " isArea: " + std::to_string(is_area) + " duration: " + std::to_string(duration) +
                          " spellId: " + std::to_string(spellId) + " isPositive: " + std::to_string(isPositive));

        if (type == DYNOBJ_AURA_TYPE)
        {
            DynamicObject* dyn_owner = aura->GetDynobjOwner();
            float radius = dyn_owner->GetRadius();
            int32 spellId = dyn_owner->GetSpellId();
            int32 duration = dyn_owner->GetDuration();
            sLog->outMessage("playerbot", LOG_LEVEL_DEBUG,
                             std::string("Info of DynamicObject -") + " name: " + dyn_owner->GetName() +
                                 " radius: " + std::to_string(radius) + " spell id: " + std::to_string(spellId) +
                                 " duration: " + std::to_string(duration));

            botAI->TellMaster(std::string("Info of DynamicObject -") + " name: " + dyn_owner->GetName() +
                              " radius: " + std::to_string(radius) + " spell id: " + std::to_string(spellId) +
                              " duration: " + std::to_string(duration));
        }
    }
    return true;
}

bool TellEstimatedDpsAction::Execute(Event /*event*/)
{
    float dps = AI_VALUE(float, "estimated group dps");
    botAI->TellMaster("Estimated Group DPS: " + std::to_string(dps));
    return true;
}

bool TellCalculateItemAction::Execute(Event event)
{
    std::string const text = event.getParam();
    ItemWithRandomProperty item = chat->parseItemWithRandomProperty(text);
    StatsWeightCalculator calculator(bot);

    const ItemTemplate* proto = sObjectMgr->GetItemTemplate(item.itemId);
    if (!proto)
        return false;
    float score = calculator.CalculateItem(item.itemId, item.randomPropertyId);

    std::ostringstream out;
    out << "Calculated score of " << chat->FormatItem(proto) << " : " << score;
    botAI->TellMasterNoFacing(out.str());
    return true;
}
