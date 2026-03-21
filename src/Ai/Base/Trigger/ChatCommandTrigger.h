/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_CHATCOMMANDTRIGGER_H
#define _PLAYERBOT_CHATCOMMANDTRIGGER_H

#include "Common.h"
#include "Trigger.h"

class Event;
class Player;
class PlayerbotAI;

class ChatCommandTrigger : public Trigger
{
public:
    using Trigger::ExternalEvent;

    ChatCommandTrigger(PlayerbotAI* botAI, std::string const command);

    void ExternalEvent(std::string const param, Player* owner = nullptr) override;
    void ExternalChatEvent(std::string const param, Player* owner, uint32 type);
    Event Check() override;
    void Reset() override;

private:
    std::string param;
    bool triggered;
    Player* owner;
    uint32 type;
};

#endif
