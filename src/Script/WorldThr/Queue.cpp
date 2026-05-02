/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include <memory>

#include "Queue.h"
#include "AiObjectContext.h"
#include "Log.h"
#include "PlayerbotAIConfig.h"

void Queue::Push(ActionBasket* action)
{
    std::lock_guard<std::recursive_mutex> lock(actionsLock_);

    if (!action)
    {
        return;
    }

    for (ActionBasket* basket : actions)
    {
        if (action->getAction()->getName() == basket->getAction()->getName())
        {
            updateExistingBasket(basket, action);
            return;
        }
    }

    actions.push_back(action);
}

ActionNode* Queue::Pop()
{
    std::unique_ptr<ActionBasket> basket(PopBasket());
    if (!basket)
    {
        return nullptr;
    }

    return basket->getAction();
}

ActionBasket* Queue::PopBasket()
{
    std::lock_guard<std::recursive_mutex> lock(actionsLock_);

    ActionBasket* highestRelevanceBasket = findHighestRelevanceBasket();
    if (!highestRelevanceBasket)
    {
        return nullptr;
    }

    auto itr = std::find(actions.begin(), actions.end(), highestRelevanceBasket);
    if (itr == actions.end())
    {
        LOG_ERROR("playerbots", "Queue::PopBasket called for a basket that is no longer in the queue");
        return nullptr;
    }

    actions.erase(itr);
    return highestRelevanceBasket;
}

ActionBasket* Queue::Peek()
{
    std::lock_guard<std::recursive_mutex> lock(actionsLock_);
    return findHighestRelevanceBasket();
}

uint32 Queue::Size()
{
    std::lock_guard<std::recursive_mutex> lock(actionsLock_);
    return actions.size();
}

void Queue::RemoveExpired()
{
    std::lock_guard<std::recursive_mutex> lock(actionsLock_);

    if (!sPlayerbotAIConfig.expireActionTime)
    {
        return;
    }

    std::list<ActionBasket*> expiredBaskets;
    collectExpiredBaskets(expiredBaskets);
    removeAndDeleteBaskets(expiredBaskets);
}

// Private helper methods
void Queue::updateExistingBasket(ActionBasket* existing, ActionBasket* newBasket)
{
    if (existing->getRelevance() < newBasket->getRelevance())
    {
        existing->setRelevance(newBasket->getRelevance());
    }

    if (ActionNode* actionNode = newBasket->getAction())
    {
        delete actionNode;
    }

    delete newBasket;
}

ActionBasket* Queue::findHighestRelevanceBasket() const
{
    if (actions.empty())
    {
        return nullptr;
    }

    float maxRelevance = -1.0f;
    ActionBasket* selection = nullptr;

    for (ActionBasket* basket : actions)
    {
        if (!basket)
        {
            continue;
        }

        if (basket->getRelevance() > maxRelevance)
        {
            maxRelevance = basket->getRelevance();
            selection = basket;
        }
    }

    return selection;
}

ActionNode* Queue::extractAndDeleteBasket(ActionBasket* basket)
{
    std::lock_guard<std::recursive_mutex> lock(actionsLock_);

    if (!basket)
        return nullptr;

    auto itr = std::find(actions.begin(), actions.end(), basket);
    if (itr == actions.end())
    {
        LOG_ERROR("playerbots", "Queue::extractAndDeleteBasket called for a basket that is no longer in the queue");
        return nullptr;
    }

    ActionNode* action = basket->getAction();
    actions.erase(itr);
    delete basket;
    return action;
}

void Queue::collectExpiredBaskets(std::list<ActionBasket*>& expiredBaskets)
{
    uint32 expiryTime = sPlayerbotAIConfig.expireActionTime;
    for (ActionBasket* basket : actions)
    {
        if (basket->isExpired(expiryTime))
        {
            expiredBaskets.push_back(basket);
        }
    }
}

void Queue::removeAndDeleteBaskets(std::list<ActionBasket*>& basketsToRemove)
{
    for (ActionBasket* basket : basketsToRemove)
    {
        if (!basket)
            continue;

        auto itr = std::find(actions.begin(), actions.end(), basket);
        if (itr == actions.end())
        {
            LOG_ERROR("playerbots", "Queue::removeAndDeleteBaskets skipped a basket that is no longer in the queue");
            continue;
        }

        actions.erase(itr);

        if (ActionNode* action = basket->getAction())
        {
            delete action;
        }

        delete basket;
    }
}
