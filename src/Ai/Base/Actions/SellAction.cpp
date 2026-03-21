/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "SellAction.h"

#include "Event.h"
#include "ItemUsageValue.h"
#include "ItemVisitors.h"
#include "Playerbots.h"
#include "ItemPackets.h"

namespace
{
class CollectItemGuidsVisitor : public IterateItemsVisitor
{
public:
    std::vector<ObjectGuid> const& GetItemGuids() const { return itemGuids; }

protected:
    void Remember(Item* item)
    {
        if (item)
            itemGuids.push_back(item->GetGUID());
    }

private:
    std::vector<ObjectGuid> itemGuids;
};

class CollectGrayItemGuidsVisitor : public CollectItemGuidsVisitor
{
public:
    bool Visit(Item* item) override
    {
        if (!item)
            return true;

        ItemTemplate const* itemTemplate = item->GetTemplate();
        if (!itemTemplate || itemTemplate->Quality != ITEM_QUALITY_POOR)
            return true;

        Remember(item);
        return true;
    }
};

class CollectVendorItemGuidsVisitor : public CollectItemGuidsVisitor
{
public:
    explicit CollectVendorItemGuidsVisitor(AiObjectContext* con) : context(con) {}

    bool Visit(Item* item) override
    {
        if (!item || !context)
            return true;

        ItemUsage usage = context->GetValue<ItemUsage>("item usage", item->GetEntry())->Get();
        if (usage != ITEM_USAGE_VENDOR && usage != ITEM_USAGE_AH)
            return true;

        Remember(item);
        return true;
    }

private:
    AiObjectContext* context;
};

std::vector<ObjectGuid> ToItemGuids(std::vector<Item*> const& items)
{
    std::vector<ObjectGuid> itemGuids;
    itemGuids.reserve(items.size());

    for (Item* item : items)
        if (item)
            itemGuids.push_back(item->GetGUID());

    return itemGuids;
}
} // namespace

bool SellAction::Execute(Event event)
{
    std::string const text = event.getParam();
    if (text == "gray" || text == "*")
    {
        CollectGrayItemGuidsVisitor visitor;
        IterateItems(&visitor);

        for (ObjectGuid const& itemGuid : visitor.GetItemGuids())
            if (Item* item = bot->GetItemByGuid(itemGuid))
                Sell(item);

        return true;
    }

    if (text == "vendor")
    {
        CollectVendorItemGuidsVisitor visitor(context);
        IterateItems(&visitor);

        for (ObjectGuid const& itemGuid : visitor.GetItemGuids())
            if (Item* item = bot->GetItemByGuid(itemGuid))
                Sell(item);

        return true;
    }

    if (text != "")
    {
        for (ObjectGuid const& itemGuid : ToItemGuids(parseItems(text, ITERATE_ITEMS_IN_BAGS)))
            if (Item* item = bot->GetItemByGuid(itemGuid))
                Sell(item);

        return true;
    }

    botAI->TellError("Usage: s gray/*/vendor/[item link]");
    return false;
}

void SellAction::Sell(FindItemVisitor* visitor)
{
    IterateItems(visitor);

    for (ObjectGuid const& itemGuid : ToItemGuids(visitor->GetResult()))
        if (Item* item = bot->GetItemByGuid(itemGuid))
            Sell(item);
}

void SellAction::Sell(Item* item)
{
    if (!item)
        return;

    ItemTemplate const* itemTemplate = item->GetTemplate();
    if (!itemTemplate)
        return;

    std::ostringstream out;
    GuidVector vendors = botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest npcs")->Get();

    for (ObjectGuid const vendorguid : vendors)
    {
        Creature* pCreature = bot->GetNPCIfCanInteractWith(vendorguid, UNIT_NPC_FLAG_VENDOR);
        if (!pCreature)
            continue;

        ObjectGuid itemguid = item->GetGUID();
        uint32 count = item->GetCount();
        std::string itemLabel = chat->FormatItem(itemTemplate);

        uint32 botMoney = bot->GetMoney();

        WorldPacket p(CMSG_SELL_ITEM);
        p << vendorguid << itemguid << count;

        WorldPackets::Item::SellItem nicePacket(std::move(p));
        nicePacket.Read();
        bot->GetSession()->HandleSellItemOpcode(nicePacket);

        if (botAI->HasCheat(BotCheatMask::gold))
        {
            bot->SetMoney(botMoney);
        }

        out << "Selling " << itemLabel;
        botAI->TellMaster(out);

        bot->PlayDistanceSound(120);
        break;
    }
}
