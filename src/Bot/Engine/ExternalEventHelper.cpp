/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "ExternalEventHelper.h"

#include "ChatCommandTrigger.h"
#include "ChatHelper.h"
#include "Playerbots.h"
#include "Trigger.h"

bool ExternalEventHelper::ParseChatCommand(std::string const command, Player* owner, uint32 type)
{
    if (HandleCommand(command, "", owner, type))
        return true;

    size_t i = std::string::npos;
    while (true)
    {
        size_t found = command.rfind(" ", i);
        if (found == std::string::npos || !found)
            break;

        std::string const name = command.substr(0, found);
        std::string const param = command.substr(found + 1);

        i = found - 1;

        if (HandleCommand(name, param, owner, type))
            return true;
    }

    if (!ChatHelper::parseableItem(command))
        return false;

    HandleCommand("c", command, owner, type);
    HandleCommand("t", command, owner, type);

    return true;
}

void ExternalEventHelper::HandlePacket(std::map<uint16, std::string>& handlers, WorldPacket const& packet,
                                       Player* owner)
{
    uint16 opcode = packet.GetOpcode();
    std::string const name = handlers[opcode];
    if (name.empty())
        return;

    Trigger* trigger = aiObjectContext->GetTrigger(name);
    if (!trigger)
        return;

    WorldPacket p(packet);
    trigger->ExternalEvent(p, owner);
}

bool ExternalEventHelper::HandleCommand(std::string const name, std::string const param, Player* owner, uint32 type)
{
    Trigger* trigger = aiObjectContext->GetTrigger(name);
    if (!trigger)
        return false;

    if (ChatCommandTrigger* chatTrigger = dynamic_cast<ChatCommandTrigger*>(trigger))
        chatTrigger->ExternalChatEvent(param, owner, type);
    else
        trigger->ExternalEvent(param, owner);

    return true;
}
