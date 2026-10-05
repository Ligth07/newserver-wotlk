#include "GuildSeasonMgr.h"
#include "Chat.h"
#include "Config.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"

// Envía un mensaje de sistema a todos los miembros online de una hermandad
static void BroadcastToGuildMembers(uint32 guildId, std::string const& msg)
{
    Guild* guild = sGuildMgr->GetGuildById(guildId);
    if (!guild)
        return;

    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_UNIVERSAL, nullptr, nullptr, msg);
    guild->BroadcastPacket(&data);
}

// ---------------------------------------------------------------------------
// PlayerScript — login, logout, quest reward
// ---------------------------------------------------------------------------

class GuildSeasonPlayerScript : public PlayerScript
{
public:
    GuildSeasonPlayerScript() : PlayerScript("GuildSeasonPlayerScript") {}

    void OnPlayerLogin(Player* player) override
    {
        uint32 guildId = player->GetGuildId();
        if (!guildId)
            return;

        sGuildSeasonMgr->OnPlayerLogin(player->GetGUID().GetCounter(), guildId);
        sGuildSeasonMgr->ApplyGuildPerks(player, guildId);
    }

    void OnPlayerLogout(Player* player) override
    {
        sGuildSeasonMgr->OnPlayerLogout(player->GetGUID().GetCounter());
    }

    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
    {
        if (!quest->IsGuildQuest())
            return;

        uint32 questId   = quest->GetQuestId();
        uint32 guildId   = player->GetGuildId();
        uint32 playerLow = player->GetGUID().GetCounter();

        if (!guildId)
            return;

        GuildQuestReward const* reward = sGuildSeasonMgr->GetQuestReward(questId);
        if (!reward)
            return;

        // --- Reputación personal: siempre ---
        if (reward->GuildRep > 0)
        {
            sGuildSeasonMgr->AddGuildReputation(playerLow, reward->GuildRep);

            MemberSeasonData const* data = sGuildSeasonMgr->GetMemberData(playerLow);
            uint32 total = data ? data->GuildReputation : 0;

            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff00ff96[Hermandad]|r Ganaste %u puntos de reputación. (Total: %u)",
                reward->GuildRep, total);
        }

        // --- Puntos de temporada ---
        if (reward->GuildPoints == 0)
            return;

        bool isGroupQuest = quest->GetRequiredGuildMembers() > 0;

        if (isGroupQuest)
        {
            // Grupal/Raid: solo la primera entrega aporta puntos
            if (sGuildSeasonMgr->IsQuestClaimed(guildId, questId))
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cff00ff96[Hermandad]|r Esta misión ya fue reclamada por otro grupo. "
                    "Solo recibes recompensa personal.");
                return;
            }
            sGuildSeasonMgr->ClaimQuest(guildId, questId, playerLow);
        }

        // Individual: siempre aporta | Grupal: primera reclamación
        uint32 added = sGuildSeasonMgr->AddSeasonPoints(guildId, playerLow, reward->GuildPoints);
        if (added > 0)
            sGuildSeasonMgr->CheckAndUnlockPerks(guildId);

        if (added == 0)
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff00ff96[Hermandad]|r Has alcanzado el límite diario de contribución.");
            return;
        }

        // Anuncio a todos los miembros online
        GuildSeasonData const* guildData = sGuildSeasonMgr->GetGuildData(guildId);
        uint64 total = guildData ? guildData->SeasonPoints : 0;

        std::string announcement;
        if (!isGroupQuest)
            announcement = Acore::StringFormat(
                "|cff00ff96[Hermandad]|r ¡{} completó '{}' y aportó {} puntos! (Total temporada: {})",
                player->GetName(), quest->GetTitle(), added, total);
        else
            announcement = Acore::StringFormat(
                "|cff00ff96[Hermandad]|r ¡La hermandad completó '{}' y recibe {} puntos! (Total temporada: {})",
                quest->GetTitle(), added, total);

        BroadcastToGuildMembers(guildId, announcement);
    }
};

// ---------------------------------------------------------------------------
// GuildScript — miembro abandona la hermandad
// ---------------------------------------------------------------------------

class GuildSeasonGuildScript : public GuildScript
{
public:
    GuildSeasonGuildScript() : GuildScript("GuildSeasonGuildScript") {}

    void OnAddMember(Guild* guild, Player* player, uint8& /*plRank*/) override
    {
        if (!player)
            return;

        uint32 guildId = guild->GetId();

        // Cargar datos del nuevo miembro (fila recién creada con valores 0)
        sGuildSeasonMgr->OnPlayerLogin(player->GetGUID().GetCounter(), guildId);

        // Aplicar los perks que la hermandad ya tiene desbloqueados
        sGuildSeasonMgr->ApplyGuildPerks(player, guildId);
    }

    void OnRemoveMember(Guild* guild, Player* player, bool isDisbanding, bool /*isKicked*/) override
    {
        if (!player)
            return;

        uint32 playerLow = player->GetGUID().GetCounter();

        if (isDisbanding)
        {
            // El core elimina las filas de guild_member, solo limpiamos cache
            sGuildSeasonMgr->OnPlayerLogout(playerLow);
            return;
        }

        sGuildSeasonMgr->RemoveGuildPerks(player, guild->GetId());
        sGuildSeasonMgr->ResetMemberOnLeave(playerLow, guild->GetId());

        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cff00ff96[Hermandad]|r Has abandonado la hermandad. "
            "Tu reputación de hermandad ha sido reiniciada.");
    }

    void OnCreate(Guild* guild, Player* /*leader*/, std::string const& /*name*/) override
    {
        sGuildSeasonMgr->OnGuildCreate(guild->GetId());
    }

    void OnDisband(Guild* guild) override
    {
        sGuildSeasonMgr->OnGuildDisband(guild->GetId());
    }
};

// ---------------------------------------------------------------------------
// WorldScript — carga inicial y chequeo periódico de reset
// ---------------------------------------------------------------------------

class GuildSeasonWorldScript : public WorldScript
{
public:
    GuildSeasonWorldScript() : WorldScript("GuildSeasonWorldScript") {}

    void OnStartup() override
    {
        if (!sConfigMgr->GetOption<bool>("GuildSeason.Enable", true))
            return;

        _checkInterval = sConfigMgr->GetOption<uint32>("GuildSeason.ResetCheckInterval.Hours", 1) * HOUR * IN_MILLISECONDS;
        sGuildSeasonMgr->LoadFromDB();
    }

    void OnUpdate(uint32 diff) override
    {
        _checkTimer += diff;
        if (_checkTimer < _checkInterval)
            return;
        _checkTimer = 0;

        sGuildSeasonMgr->CheckSeasonReset();
        sGuildSeasonMgr->CheckBoardRefresh();
    }

private:
    uint32 _checkInterval = 0;
    uint32 _checkTimer    = 0;
};

// ---------------------------------------------------------------------------
// Registro
// ---------------------------------------------------------------------------

void AddGuildSeasonScripts()
{
    new GuildSeasonPlayerScript();
    new GuildSeasonGuildScript();
    new GuildSeasonWorldScript();
}
