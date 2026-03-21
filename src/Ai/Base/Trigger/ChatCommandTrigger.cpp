/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "ChatCommandTrigger.h"

#include "Playerbots.h"

ChatCommandTrigger::ChatCommandTrigger(PlayerbotAI* botAI, std::string const command)
    : Trigger(botAI, command), triggered(false), owner(nullptr), type(0)
{
}

void ChatCommandTrigger::ExternalEvent(std::string const paramName, Player* eventPlayer)
{
    ExternalChatEvent(paramName, eventPlayer, 0);
}

void ChatCommandTrigger::ExternalChatEvent(std::string const paramName, Player* eventPlayer, uint32 eventType)
{
    param = paramName;
    owner = eventPlayer;
    type = eventType;
    triggered = true;
}

Event ChatCommandTrigger::Check()
{
    if (!triggered)
        return Event();

    return Event(getName(), param, owner, type);
}

void ChatCommandTrigger::Reset()
{
    triggered = false;
    type = 0;
}
