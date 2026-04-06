#include "ScriptMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "ObjectAccessor.h"
#include "WorldSession.h"

#include "Playerbots.h"

namespace
{
    static Player* FindConnectedAltbotByGuid(ObjectGuid guid)
    {
        if (!guid)
            return nullptr;

        Player* p = ObjectAccessor::FindConnectedPlayer(guid);
        if (!p)
            return nullptr;

        PlayerbotAI* ai = GET_PLAYERBOT_AI(p);
        if (!ai || ai->IsRealPlayer())
            return nullptr;

        return p;
    }

    static void ForceLogoutViaPlayerbotHolder(Player* target)
    {
        if (!target)
            return;

        PlayerbotAI* ai = GET_PLAYERBOT_AI(target);

        if (!ai)
            return;

        if (Player* master = ai->GetMaster())
        {
            if (PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(master))
            {
                mgr->LogoutPlayerBot(target->GetGUID());
                return;
            }
        }

        sRandomPlayerbotMgr.LogoutPlayerBot(target->GetGUID());
    }
}

class PlayerbotsSecureLoginServerScript : public ServerScript
{
public:
    PlayerbotsSecureLoginServerScript()
        : ServerScript("PlayerbotsSecureLoginServerScript", { SERVERHOOK_CAN_PACKET_RECEIVE }) {}

    bool CanPacketReceive(WorldSession* session, WorldPacket& packet) override
    {
        if (packet.GetOpcode() != CMSG_PLAYER_LOGIN)
            return true;

        WorldPacket pkt(packet);
        ObjectGuid loginGuid;
        pkt >> loginGuid;

        if (!loginGuid)
            return true;

        Player* existingAltbot = FindConnectedAltbotByGuid(loginGuid);
        if (!existingAltbot)
            return true;

        ForceLogoutViaPlayerbotHolder(existingAltbot);

        // The bot logout is deferred onto the world-thread queue, so the character is still
        // connected right now. Reject this login attempt and let the client retry after the bot
        // instance is fully removed; otherwise we can transiently create two live Player objects
        // with the same GUID and corrupt shared state.
        if (session)
        {
            LOG_WARN("playerbots",
                     "Rejected login for {} because an altbot instance of the same character is still connected; bot logout was requested and the client must retry.",
                     loginGuid.ToString());
            session->SendCharLoginFailed(LoginFailureReason::DuplicateCharacter);
        }

        return false;
    }
};

void AddPlayerbotsSecureLoginScripts()
{
    new PlayerbotsSecureLoginServerScript();
}
