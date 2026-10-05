#include "GuildSeasonMgr.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Chat.h"
#include "GuildMgr.h"
#include "Item.h"
#include "Log.h"
#include "Mail.h"
#include "Player.h"
#include "SpellMgr.h"
#include "World.h"
#include "WorldSessionMgr.h"
#include <algorithm>
#include <random>

GuildSeasonMgr* GuildSeasonMgr::instance()
{
    static GuildSeasonMgr instance;
    return &instance;
}

void GuildSeasonMgr::LoadFromDB()
{
    _seasonDuration   = sConfigMgr->GetOption<uint32>("GuildSeason.Duration.Days", 30) * DAY; // fallback
    _dailyCap         = sConfigMgr->GetOption<uint32>("GuildSeason.DailyCap.Points", 0);
    _resetRepOnLeave  = sConfigMgr->GetOption<bool>("GuildSeason.ResetRepOnLeave", true);
    _announceReset    = sConfigMgr->GetOption<bool>("GuildSeason.AnnounceReset", true);
    _boardDailyCount  = sConfigMgr->GetOption<uint32>("GuildSeason.Board.DailyCount", 5);
    _boardWeeklyCount = sConfigMgr->GetOption<uint32>("GuildSeason.Board.WeeklyCount", 2);
    _vendorItemCount  = sConfigMgr->GetOption<uint32>("GuildSeason.Vendor.ItemCount", 10);

    // Estado de temporada
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT SeasonId, StartTime, LastDailyRefresh, LastWeeklyRefresh FROM guild_season_state LIMIT 1");
        if (result)
        {
            Field* fields      = result->Fetch();
            _currentSeason     = fields[0].Get<uint8>();
            _seasonStartTime   = fields[1].Get<uint32>();
            _lastDailyRefresh  = fields[2].Get<uint32>();
            _lastWeeklyRefresh = fields[3].Get<uint32>();
        }
        else
        {
            _currentSeason   = 1;
            _seasonStartTime = static_cast<uint32>(GameTime::GetGameTime().count());
            CharacterDatabase.Execute(
                "INSERT INTO guild_season_state (lock, SeasonId, StartTime, LastDailyRefresh, LastWeeklyRefresh) "
                "VALUES (1, {}, {}, 0, 0)",
                _currentSeason, _seasonStartTime);
        }
        LOG_INFO("module", "GuildSeason: Temporada {} activa (inicio: {})", _currentSeason, _seasonStartTime);
    }

    // Recompensas de misiones de hermandad (acore_world)
    {
        QueryResult result = WorldDatabase.Query(
            "SELECT Id, RewardGuildPoints, RewardGuildRep FROM quest_template_addon "
            "WHERE RewardGuildPoints > 0 OR RewardGuildRep > 0");

        uint32 count = 0;
        if (result)
        {
            do
            {
                Field* fields   = result->Fetch();
                uint32 questId  = fields[0].Get<uint32>();
                _questRewards[questId] = { fields[1].Get<uint32>(), fields[2].Get<uint32>() };
                ++count;
            } while (result->NextRow());
        }
        LOG_INFO("module", "GuildSeason: {} misiones con recompensa de hermandad cargadas", count);
    }

    // Datos de temporada por hermandad
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT guildid, SeasonPoints, SeasonPerksFlags FROM guild");

        if (result)
        {
            do
            {
                Field* fields  = result->Fetch();
                uint32 guildId = fields[0].Get<uint32>();
                _guildData[guildId] = { fields[1].Get<uint64>(), fields[2].Get<uint32>() };
            } while (result->NextRow());
        }
        LOG_INFO("module", "GuildSeason: {} hermandades cargadas", _guildData.size());
    }

    // Reclamaciones de la temporada actual
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT GuildId, QuestId FROM guild_season_quest_claims WHERE SeasonId = {}",
            _currentSeason);

        uint32 count = 0;
        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                _claimedQuests[fields[0].Get<uint32>()].insert(fields[1].Get<uint32>());
                ++count;
            } while (result->NextRow());
        }
        LOG_INFO("module", "GuildSeason: {} reclamaciones de misión cargadas", count);
    }

    LoadSeasonConfig();
    LoadPerksFromDB();
    LoadBoardFromDB();
}

void GuildSeasonMgr::OnPlayerLogin(uint32 playerGuid, uint32 guildId)
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT GuildReputation, SeasonContribution FROM guild_member WHERE guid = {} AND guildid = {}",
        playerGuid, guildId);

    if (result)
    {
        Field* fields = result->Fetch();
        _memberData[playerGuid] = { fields[0].Get<uint32>(), fields[1].Get<uint32>() };
    }
}

void GuildSeasonMgr::OnPlayerLogout(uint32 playerGuid)
{
    _memberData.erase(playerGuid);
}

void GuildSeasonMgr::OnGuildCreate(uint32 guildId)
{
    // La fila en guild ya existe con valores DEFAULT 0, solo inicializamos el cache
    _guildData[guildId] = {};
}

void GuildSeasonMgr::OnGuildDisband(uint32 guildId)
{
    _guildData.erase(guildId);
    _claimedQuests.erase(guildId);

    // Limpiar reclamaciones huérfanas — el core elimina las filas de guild y guild_member
    CharacterDatabase.Execute(
        "DELETE FROM guild_season_quest_claims WHERE GuildId = {}", guildId);
}

bool GuildSeasonMgr::HasGuildReward(uint32 questId) const
{
    return _questRewards.find(questId) != _questRewards.end();
}

GuildQuestReward const* GuildSeasonMgr::GetQuestReward(uint32 questId) const
{
    auto it = _questRewards.find(questId);
    return it != _questRewards.end() ? &it->second : nullptr;
}

bool GuildSeasonMgr::IsQuestClaimed(uint32 guildId, uint32 questId) const
{
    auto it = _claimedQuests.find(guildId);
    if (it == _claimedQuests.end())
        return false;
    return it->second.find(questId) != it->second.end();
}

void GuildSeasonMgr::ClaimQuest(uint32 guildId, uint32 questId, uint32 claimerGuid)
{
    _claimedQuests[guildId].insert(questId);

    CharacterDatabase.Execute(
        "INSERT INTO guild_season_quest_claims (GuildId, QuestId, SeasonId, ClaimedBy, ClaimedAt) "
        "VALUES ({}, {}, {}, {}, {})",
        guildId, questId, _currentSeason, claimerGuid,
        static_cast<uint32>(GameTime::GetGameTime().count()));
}

uint32 GuildSeasonMgr::AddSeasonPoints(uint32 guildId, uint32 contributorGuid, uint32 points)
{
    auto it = _memberData.find(contributorGuid);
    if (it != _memberData.end() && _dailyCap > 0)
    {
        MemberSeasonData& data = it->second;
        uint32 today = static_cast<uint32>(GameTime::GetGameTime().count()) / DAY;

        // Resetear contador si es un día nuevo
        if (data.DailyContributionDay != today)
        {
            data.DailyContribution    = 0;
            data.DailyContributionDay = today;
        }

        // Ajustar puntos al cap restante del día
        uint32 remaining = _dailyCap > data.DailyContribution ? _dailyCap - data.DailyContribution : 0;
        if (remaining == 0)
            return 0;

        points = std::min(points, remaining);
    }

    _guildData[guildId].SeasonPoints += points;

    CharacterDatabase.Execute(
        "UPDATE guild SET SeasonPoints = SeasonPoints + {} WHERE guildid = {}",
        points, guildId);

    if (it != _memberData.end())
    {
        it->second.SeasonContribution   += points;
        it->second.DailyContribution    += points;
        it->second.DailyContributionDay  = static_cast<uint32>(GameTime::GetGameTime().count()) / DAY;

        CharacterDatabase.Execute(
            "UPDATE guild_member SET SeasonContribution = SeasonContribution + {} WHERE guid = {}",
            points, contributorGuid);
    }

    return points;
}

void GuildSeasonMgr::AddGuildReputation(uint32 playerGuid, uint32 amount)
{
    auto it = _memberData.find(playerGuid);
    if (it == _memberData.end())
        return;

    it->second.GuildReputation += amount;
    CharacterDatabase.Execute(
        "UPDATE guild_member SET GuildReputation = GuildReputation + {} WHERE guid = {}",
        amount, playerGuid);
}

void GuildSeasonMgr::ResetMemberOnLeave(uint32 playerGuid, uint32 guildId)
{
    _memberData.erase(playerGuid);

    if (_resetRepOnLeave)
        CharacterDatabase.Execute(
            "UPDATE guild_member SET GuildReputation = 0, SeasonContribution = 0 "
            "WHERE guid = {} AND guildid = {}",
            playerGuid, guildId);
    else
        CharacterDatabase.Execute(
            "UPDATE guild_member SET SeasonContribution = 0 WHERE guid = {} AND guildid = {}",
            playerGuid, guildId);
}

GuildSeasonData const* GuildSeasonMgr::GetGuildData(uint32 guildId) const
{
    auto it = _guildData.find(guildId);
    return it != _guildData.end() ? &it->second : nullptr;
}

MemberSeasonData const* GuildSeasonMgr::GetMemberData(uint32 playerGuid) const
{
    auto it = _memberData.find(playerGuid);
    return it != _memberData.end() ? &it->second : nullptr;
}

void GuildSeasonMgr::CheckSeasonReset()
{
    if (_seasonDuration == 0)
        return;

    uint32 now = static_cast<uint32>(GameTime::GetGameTime().count());
    if (now >= _seasonStartTime + _seasonDuration)
        ResetSeason();
}

void GuildSeasonMgr::ResetSeason()
{
    LOG_INFO("module", "GuildSeason: Iniciando reset de temporada {}...", _currentSeason);

    AwardTopGuilds();

    ++_currentSeason;
    _seasonStartTime = static_cast<uint32>(GameTime::GetGameTime().count());

    // Quitar perks a todos los miembros online antes de limpiar flags
    sWorldSessionMgr->DoForAllOnlinePlayers([this](Player* player)
    {
        if (player->GetGuildId())
            RemoveGuildPerks(player, player->GetGuildId());
    });

    // Limpiar estado en memoria
    for (auto& [guildId, data] : _guildData)
    {
        data.SeasonPoints     = 0;
        data.SeasonPerksFlags = 0;
    }
    for (auto& [guid, data] : _memberData)
        data.SeasonContribution = 0;
    _claimedQuests.clear();

    // Actualizar DB
    CharacterDatabase.Execute("UPDATE guild SET SeasonPoints = 0, SeasonPerksFlags = 0");
    CharacterDatabase.Execute("UPDATE guild_member SET SeasonContribution = 0");
    CharacterDatabase.Execute(
        "UPDATE guild_season_state SET SeasonId = {}, StartTime = {} WHERE lock = 1",
        _currentSeason, _seasonStartTime);

    LoadSeasonConfig();

    if (_announceReset)
    {
        std::string msg = "¡La temporada de hermandad ha finalizado! Comienza una nueva temporada.";
        sWorldSessionMgr->DoForAllOnlinePlayers([&msg](Player* player)
        {
            ChatHandler(player->GetSession()).SendSysMessage(msg);
        });
    }

    LOG_INFO("module", "GuildSeason: Temporada {} iniciada", _currentSeason);
}

// ---------------------------------------------------------------------------
// Perks pasivos
// ---------------------------------------------------------------------------

void GuildSeasonMgr::LoadPerksFromDB()
{
    _perks.clear();

    QueryResult result = CharacterDatabase.Query(
        "SELECT PerkId, Points, SpellId FROM guild_season_perks ORDER BY Points ASC");

    if (!result)
    {
        LOG_INFO("module", "GuildSeason: Sin perks configurados");
        return;
    }

    do
    {
        Field* fields = result->Fetch();
        GuildPerk perk;
        perk.PerkId  = fields[0].Get<uint8>();
        perk.Points  = fields[1].Get<uint32>();
        perk.SpellId = fields[2].Get<uint32>();

        if (sSpellMgr->GetSpellInfo(perk.SpellId))
            _perks.push_back(perk);
        else
            LOG_ERROR("module", "GuildSeason: Perk {} tiene SpellId {} inválido, omitido",
                perk.PerkId, perk.SpellId);
    } while (result->NextRow());

    LOG_INFO("module", "GuildSeason: {} perks cargados", _perks.size());
}

void GuildSeasonMgr::ApplyGuildPerks(Player* player, uint32 guildId)
{
    if (_perks.empty())
        return;

    GuildSeasonData const* data = GetGuildData(guildId);
    if (!data)
        return;

    for (GuildPerk const& perk : _perks)
        if (data->SeasonPerksFlags & (1u << perk.PerkId))
            player->CastSpell(player, perk.SpellId, true);
}

void GuildSeasonMgr::RemoveGuildPerks(Player* player, uint32 guildId)
{
    if (_perks.empty())
        return;

    GuildSeasonData const* data = GetGuildData(guildId);
    if (!data)
        return;

    for (GuildPerk const& perk : _perks)
        if (data->SeasonPerksFlags & (1u << perk.PerkId))
            player->RemoveAurasDueToSpell(perk.SpellId);
}

void GuildSeasonMgr::CheckAndUnlockPerks(uint32 guildId)
{
    if (_perks.empty())
        return;

    GuildSeasonData* data = &_guildData[guildId];

    for (GuildPerk const& perk : _perks)
    {
        uint32 flag = 1u << perk.PerkId;
        if (data->SeasonPerksFlags & flag)
            continue; // ya desbloqueado

        if (data->SeasonPoints < perk.Points)
            continue;

        // Desbloquear perk
        data->SeasonPerksFlags |= flag;
        CharacterDatabase.Execute(
            "UPDATE guild SET SeasonPerksFlags = {} WHERE guildid = {}",
            data->SeasonPerksFlags, guildId);

        // Aplicar a todos los miembros online de la hermandad
        sWorldSessionMgr->DoForAllOnlinePlayers([&](Player* player)
        {
            if (player->GetGuildId() == guildId)
                player->CastSpell(player, perk.SpellId, true);
        });

        LOG_INFO("module", "GuildSeason: Hermandad {} desbloqueó perk {} (spell {})",
            guildId, perk.PerkId, perk.SpellId);
    }
}

// ---------------------------------------------------------------------------
// Configuración y premios de temporada
// ---------------------------------------------------------------------------

void GuildSeasonMgr::LoadSeasonConfig()
{
    _currentPrizes.clear();

    // Cargar config de la temporada actual (sobreescribe el fallback del conf)
    QueryResult config = CharacterDatabase.Query(
        "SELECT DurationDays, Description FROM guild_season_config WHERE SeasonId = {}",
        _currentSeason);

    if (config)
    {
        Field* fields  = config->Fetch();
        _seasonDuration = fields[0].Get<uint8>() * DAY;
            uint8 duration = fields[0].Get<uint8>();
        LOG_INFO("module", "GuildSeason: Config temporada {} — duración {} días, '{}'",
            _currentSeason, duration, fields[1].Get<std::string>());
    }
    else
        LOG_INFO("module", "GuildSeason: Sin config en DB para temporada {}, usando defaults del conf",
            _currentSeason);

    // Refrescar vendor si no hay ítems activos para esta temporada
    {
        QueryResult check = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM guild_vendor_active WHERE SeasonId = {}", _currentSeason);
        if (!check || check->Fetch()[0].Get<uint32>() == 0)
            RefreshVendor();
    }

    // Cargar premios de la temporada actual
    QueryResult prizes = CharacterDatabase.Query(
        "SELECT Rank, ItemEntry, ItemCount FROM guild_season_prizes "
        "WHERE SeasonId = {} ORDER BY Rank, ItemEntry",
        _currentSeason);

    if (prizes)
    {
        do
        {
            Field* fields = prizes->Fetch();
            uint8 rank    = fields[0].Get<uint8>();
            _currentPrizes[rank].push_back({ fields[1].Get<uint32>(), fields[2].Get<uint32>() });
        } while (prizes->NextRow());

        LOG_INFO("module", "GuildSeason: Premios cargados para temporada {}", _currentSeason);
    }
    else
        LOG_WARN("module", "GuildSeason: Sin premios configurados para temporada {}", _currentSeason);
}

void GuildSeasonMgr::AwardTopGuilds()
{
    if (_currentPrizes.empty())
    {
        LOG_WARN("module", "GuildSeason: Sin premios definidos para temporada {}, omitiendo entrega",
            _currentSeason);
        return;
    }

    QueryResult top3 = CharacterDatabase.Query(
        "SELECT guildid, SeasonPoints FROM guild "
        "WHERE SeasonPoints > 0 ORDER BY SeasonPoints DESC LIMIT 3");

    if (!top3)
    {
        LOG_INFO("module", "GuildSeason: Ninguna hermandad con puntos, sin premios que otorgar");
        return;
    }

    uint8 rank = 1;
    do
    {
        Field* fields  = top3->Fetch();
        uint32 guildId = fields[0].Get<uint32>();
        uint64 points  = fields[1].Get<uint64>();

        Guild* guild = sGuildMgr->GetGuildById(guildId);
        if (!guild)
        {
            ++rank;
            continue;
        }

        auto prizeIt = _currentPrizes.find(rank);
        if (prizeIt == _currentPrizes.end() || prizeIt->second.empty())
        {
            ++rank;
            continue;
        }

        std::string subject = Acore::StringFormat(
            "Premio Temporada {} - Puesto {}", _currentSeason, rank);
        std::string body = Acore::StringFormat(
            "¡Felicitaciones! Tu hermandad obtuvo el puesto {} en la Temporada {} "
            "con {} puntos de temporada.", rank, _currentSeason, points);

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        MailDraft draft(subject, body);
        for (SeasonPrizeItem const& prize : prizeIt->second)
        {
            if (Item* item = Item::CreateItem(prize.ItemEntry, prize.ItemCount, nullptr))
            {
                item->SaveToDB(trans);
                draft.AddItem(item);
            }
        }

        draft.SendMailTo(trans,
            MailReceiver(guild->GetLeaderGUID().GetCounter()),
            MailSender(MAIL_NORMAL, 0, MAIL_STATIONERY_GM));

        CharacterDatabase.CommitTransaction(trans);

        LOG_INFO("module", "GuildSeason: Premio rango {} enviado al líder de hermandad {}",
            rank, guild->GetName());

        // Anuncio público
        if (_announceReset)
        {
            std::string msg = Acore::StringFormat(
                "¡La hermandad {} obtuvo el puesto {} con {} puntos de temporada!",
                guild->GetName(), rank, points);
            sWorldSessionMgr->DoForAllOnlinePlayers([&msg](Player* player)
            {
                ChatHandler(player->GetSession()).SendSysMessage(msg);
            });
        }

        ++rank;
    } while (top3->NextRow());
}

// ---------------------------------------------------------------------------
// Vendor de hermandad
// ---------------------------------------------------------------------------

void GuildSeasonMgr::RefreshVendor()
{
    // Leer pool completo
    QueryResult result = CharacterDatabase.Query(
        "SELECT ItemEntry, TierRequired, Price FROM guild_vendor_pool WHERE Enabled = 1");

    if (!result)
    {
        LOG_WARN("module", "GuildSeason: Pool del vendor vacío, sin ítems para temporada {}",
            _currentSeason);
        return;
    }

    struct VendorPoolItem { uint32 ItemEntry; uint8 TierRequired; uint32 Price; };
    std::vector<VendorPoolItem> pool;

    do
    {
        Field* fields = result->Fetch();
        pool.push_back({ fields[0].Get<uint32>(), fields[1].Get<uint8>(), fields[2].Get<uint32>() });
    } while (result->NextRow());

    // Mezclar y seleccionar N
    std::shuffle(pool.begin(), pool.end(), std::mt19937{ std::random_device{}() });

    uint32 count = std::min<uint32>(_vendorItemCount, static_cast<uint32>(pool.size()));

    // Limpiar ítems activos de temporadas anteriores y de la actual
    CharacterDatabase.Execute(
        "DELETE FROM guild_vendor_active WHERE SeasonId = {}", _currentSeason);

    for (uint32 i = 0; i < count; ++i)
    {
        CharacterDatabase.Execute(
            "INSERT INTO guild_vendor_active (ItemEntry, TierRequired, Price, SeasonId) "
            "VALUES ({}, {}, {}, {})",
            pool[i].ItemEntry, pool[i].TierRequired, pool[i].Price, _currentSeason);
    }

    LOG_INFO("module", "GuildSeason: Vendor temporada {} generado — {} ítems",
        _currentSeason, count);
}

// ---------------------------------------------------------------------------
// Tablón de misiones
// ---------------------------------------------------------------------------

void GuildSeasonMgr::LoadBoardFromDB()
{
    _dailyPool.clear();
    _weeklyPool.clear();
    _activeDailyQuests.clear();
    _activeWeeklyQuests.clear();

    // Cargar pool completo
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT QuestId, Type FROM guild_board_pool WHERE Enabled = 1");
        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 questId = fields[0].Get<uint32>();
                uint8  type    = fields[1].Get<uint8>();
                if (type == GUILD_BOARD_DAILY)
                    _dailyPool.push_back(questId);
                else
                    _weeklyPool.push_back(questId);
            } while (result->NextRow());
        }
        LOG_INFO("module", "GuildSeason: Pool del tablón — {} diarias, {} semanales",
            _dailyPool.size(), _weeklyPool.size());
    }

    // Cargar tablón activo
    {
        uint32 now = static_cast<uint32>(GameTime::GetGameTime().count());
        QueryResult result = CharacterDatabase.Query(
            "SELECT QuestId, Type FROM guild_board_active WHERE ExpiresAt > {}", now);
        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 questId = fields[0].Get<uint32>();
                uint8  type    = fields[1].Get<uint8>();
                if (type == GUILD_BOARD_DAILY)
                    _activeDailyQuests.insert(questId);
                else
                    _activeWeeklyQuests.insert(questId);
            } while (result->NextRow());
        }
        LOG_INFO("module", "GuildSeason: Tablón activo — {} diarias, {} semanales",
            _activeDailyQuests.size(), _activeWeeklyQuests.size());
    }
}

std::unordered_set<uint32> const& GuildSeasonMgr::GetActiveBoardQuests(GuildBoardType type) const
{
    return type == GUILD_BOARD_DAILY ? _activeDailyQuests : _activeWeeklyQuests;
}

void GuildSeasonMgr::CheckBoardRefresh()
{
    uint32 lastDailyReset  = static_cast<uint32>(sWorld->GetNextDailyQuestsResetTime().count()) - DAY;
    uint32 lastWeeklyReset = static_cast<uint32>(sWorld->GetNextWeeklyQuestsResetTime().count()) - WEEK;

    if (_lastDailyRefresh < lastDailyReset)
        RefreshBoard(GUILD_BOARD_DAILY);

    if (_lastWeeklyRefresh < lastWeeklyReset)
        RefreshBoard(GUILD_BOARD_WEEKLY);
}

void GuildSeasonMgr::RefreshBoard(GuildBoardType type)
{
    std::vector<uint32>& pool         = (type == GUILD_BOARD_DAILY) ? _dailyPool        : _weeklyPool;
    std::unordered_set<uint32>& active = (type == GUILD_BOARD_DAILY) ? _activeDailyQuests : _activeWeeklyQuests;
    uint32 count  = (type == GUILD_BOARD_DAILY) ? _boardDailyCount  : _boardWeeklyCount;
    uint32 now    = static_cast<uint32>(GameTime::GetGameTime().count());

    // Limpiar claims de las misiones que salen del tablón
    ClearBoardClaims(type);

    // Eliminar tablón activo de este tipo en DB
    CharacterDatabase.Execute(
        "DELETE FROM guild_board_active WHERE Type = {}", static_cast<uint32>(type));
    active.clear();

    if (pool.empty())
    {
        LOG_WARN("module", "GuildSeason: Pool de {} vacío, tablón no generado",
            type == GUILD_BOARD_DAILY ? "diarias" : "semanales");
        return;
    }

    // Mezclar pool y tomar los primeros N
    std::vector<uint32> shuffled = pool;
    std::shuffle(shuffled.begin(), shuffled.end(), std::mt19937{ std::random_device{}() });

    uint32 selected = std::min<uint32>(count, static_cast<uint32>(shuffled.size()));
    uint32 expiresAt = (type == GUILD_BOARD_DAILY)
        ? static_cast<uint32>(sWorld->GetNextDailyQuestsResetTime().count())
        : static_cast<uint32>(sWorld->GetNextWeeklyQuestsResetTime().count());

    for (uint32 i = 0; i < selected; ++i)
    {
        uint32 questId = shuffled[i];
        active.insert(questId);
        CharacterDatabase.Execute(
            "INSERT INTO guild_board_active (QuestId, Type, ExpiresAt) VALUES ({}, {}, {})",
            questId, static_cast<uint32>(type), expiresAt);
    }

    // Actualizar timestamp de último refresh
    if (type == GUILD_BOARD_DAILY)
    {
        _lastDailyRefresh = now;
        CharacterDatabase.Execute(
            "UPDATE guild_season_state SET LastDailyRefresh = {} WHERE lock = 1", now);
    }
    else
    {
        _lastWeeklyRefresh = now;
        CharacterDatabase.Execute(
            "UPDATE guild_season_state SET LastWeeklyRefresh = {} WHERE lock = 1", now);
    }

    LOG_INFO("module", "GuildSeason: Tablón de {} refrescado — {} misiones activas",
        type == GUILD_BOARD_DAILY ? "diarias" : "semanales", selected);
}

void GuildSeasonMgr::ClearBoardClaims(GuildBoardType type)
{
    if (type == GUILD_BOARD_DAILY && _activeDailyQuests.empty())
        return;
    if (type == GUILD_BOARD_WEEKLY && _activeWeeklyQuests.empty())
        return;

    std::unordered_set<uint32> const& active =
        (type == GUILD_BOARD_DAILY) ? _activeDailyQuests : _activeWeeklyQuests;

    // Limpiar claims en DB para estas misiones
    for (uint32 questId : active)
    {
        CharacterDatabase.Execute(
            "DELETE FROM guild_season_quest_claims WHERE QuestId = {} AND SeasonId = {}",
            questId, _currentSeason);

        // Limpiar cache en memoria
        for (auto& [guildId, claimed] : _claimedQuests)
            claimed.erase(questId);
    }
}
