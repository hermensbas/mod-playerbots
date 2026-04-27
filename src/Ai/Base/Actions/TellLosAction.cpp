/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "TellLosAction.h"
#include <algorithm>
#include <cctype>
#include <istream>
#include <set>
#include <sstream>
#include <vector>

#include "ChatHelper.h"
#include "Event.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "StatsWeightCalculator.h"
#include "World.h"
#include "Group.h"
#include "WorldSession.h"

namespace
{
struct LosRequest
{
    std::string category;
    std::set<uint8> subgroups;
};

bool IsAllDigits(std::string const& token)
{
    return !token.empty() &&
           std::all_of(token.begin(), token.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

bool CanUseGameObject(Player* bot, GameObject* go)
{
    if (!bot || !go || !go->isSpawned())
        return false;

    if (go->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE | GO_FLAG_IN_USE))
        return false;

    if (!go->IsWithinDistInMap(bot, go->GetInteractionDistance()))
        return false;

    if (bot->m_mover != bot)
    {
        if (!(bot->IsOnVehicle(bot->m_mover) || bot->IsMounted()) && !go->GetGOInfo()->IsUsableMounted())
            return false;
    }

    return true;
}

std::string ToLowerCopy(std::string token)
{
    std::transform(token.begin(), token.end(), token.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return token;
}

void ParseSubgroupToken(std::string const& token, std::set<uint8>& subgroups)
{
    if (token.size() <= 6 || token.substr(0, 6) != "@group")
        return;

    std::string const groupSpec = token.substr(6);
    std::istringstream ss(groupSpec);
    std::string part;

    while (std::getline(ss, part, ','))
    {
        if (part.empty())
            continue;

        size_t dashPos = part.find('-');
        if (dashPos != std::string::npos)
        {
            std::string const fromToken = part.substr(0, dashPos);
            std::string const toToken = part.substr(dashPos + 1);
            if (!IsAllDigits(fromToken) || !IsAllDigits(toToken))
                continue;

            uint32 from = static_cast<uint32>(std::stoul(fromToken));
            uint32 to = static_cast<uint32>(std::stoul(toToken));
            if (from > to)
                std::swap(from, to);

            for (uint32 groupNumber = from; groupNumber <= to; ++groupNumber)
            {
                if (groupNumber >= 1 && groupNumber <= MAX_RAID_SUBGROUPS)
                    subgroups.insert(static_cast<uint8>(groupNumber - 1));
            }

            continue;
        }

        if (!IsAllDigits(part))
            continue;

        uint32 groupNumber = static_cast<uint32>(std::stoul(part));
        if (groupNumber >= 1 && groupNumber <= MAX_RAID_SUBGROUPS)
            subgroups.insert(static_cast<uint8>(groupNumber - 1));
    }
}

LosRequest ParseLosRequest(std::string const& param)
{
    LosRequest request;
    std::vector<std::string> words;
    std::istringstream iss(param);
    std::string token;
    while (iss >> token)
    {
        token = ToLowerCopy(token);

        if (token.find("@group") == 0)
        {
            ParseSubgroupToken(token, request.subgroups);
            continue;
        }

        words.push_back(token);
    }

    if (words.empty())
        return request;

    if (words.size() == 1)
    {
        request.category = words[0];
        return request;
    }

    if (words.size() == 2 && words[0] == "game" && words[1] == "objects")
    {
        request.category = "game objects";
        return request;
    }

    std::ostringstream joined;
    for (size_t i = 0; i < words.size(); ++i)
    {
        if (i)
            joined << ' ';
        joined << words[i];
    }

    request.category = joined.str();
    return request;
}

bool IsPartyLikeChat(uint32 type)
{
    return type == CHAT_MSG_PARTY || type == CHAT_MSG_PARTY_LEADER;
}
} // namespace

static bool IsLosResponder(PlayerbotAI* botAI, Event const& event, LosRequest const& request, Player* owner)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot)
        return false;

    if (event.getType() == CHAT_MSG_WHISPER)
        return true;

    Group* group = bot->GetGroup();
    if (!group)
        return true;

    bool filterBySubgroup = false;
    std::set<uint8> targetSubgroups;

    if (!request.subgroups.empty())
    {
        filterBySubgroup = true;
        targetSubgroups = request.subgroups;
    }
    else if (group->isRaidGroup() && IsPartyLikeChat(event.getType()) && owner)
    {
        uint8 ownerSubgroup = group->GetMemberGroup(owner->GetGUID());
        if (ownerSubgroup < MAX_RAID_SUBGROUPS)
        {
            filterBySubgroup = true;
            targetSubgroups.insert(ownerSubgroup);
        }
    }

    uint8 botSubgroup = group->GetMemberGroup(bot->GetGUID());
    if (filterBySubgroup)
    {
        if (botSubgroup >= MAX_RAID_SUBGROUPS)
            return false;

        if (targetSubgroups.find(botSubgroup) == targetSubgroups.end())
            return false;
    }

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

        if (filterBySubgroup && group->GetMemberGroup(member->GetGUID()) != botSubgroup)
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

        if (filterBySubgroup && group->GetMemberGroup(member->GetGUID()) != botSubgroup)
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
    Player* owner = event.getOwner();
    if (!owner)
        owner = botAI->GetMaster();

    LosRequest const request = ParseLosRequest(event.getParam());
    std::string const& category = request.category;

    if (!IsLosResponder(botAI, event, request, owner))
        return true;

    if (category.empty() || category == "targets")
    {
        ListUnits("--- Targets ---", *context->GetValue<GuidVector>("possible targets"));
        ListUnits("--- Targets (All) ---", *context->GetValue<GuidVector>("all targets"));
    }

    if (category.empty() || category == "npcs")
    {
        ListUnits("--- NPCs ---", *context->GetValue<GuidVector>("nearest npcs"));
    }

    if (category.empty() || category == "corpses")
    {
        ListUnits("--- Corpses ---", *context->GetValue<GuidVector>("nearest corpses"));
    }

    if (category.empty() || category == "gos" || category == "game objects")
    {
        ListGameObjects("--- Game objects ---", *context->GetValue<GuidVector>("nearest game objects"), owner);
    }

    if (category.empty() || category == "players")
    {
        ListUnits("--- Friendly players ---", *context->GetValue<GuidVector>("nearest friendly players"));
    }

    if (category.empty() || category == "triggers")
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
        if (!CanUseGameObject(bot, go))
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
