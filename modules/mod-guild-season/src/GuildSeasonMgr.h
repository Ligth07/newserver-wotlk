#ifndef GUILD_SEASON_MGR_H
#define GUILD_SEASON_MGR_H

#include "Define.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Player;

enum GuildBoardType : uint8
{
    GUILD_BOARD_DAILY  = 0,
    GUILD_BOARD_WEEKLY = 1
};

struct GuildQuestReward
{
    uint32 GuildPoints = 0;
    uint32 GuildRep    = 0;
};

struct SeasonPrizeItem
{
    uint32 ItemEntry = 0;
    uint32 ItemCount = 1;
};

struct GuildPerk
{
    uint8  PerkId  = 0;
    uint32 Points  = 0;
    uint32 SpellId = 0;
};

struct GuildSeasonData
{
    uint64 SeasonPoints     = 0;
    uint32 SeasonPerksFlags = 0;
};

struct MemberSeasonData
{
    uint32 GuildReputation      = 0;
    uint32 SeasonContribution   = 0;
    uint32 DailyContribution    = 0;
    uint32 DailyContributionDay = 0;
};

class GuildSeasonMgr
{
public:
    static GuildSeasonMgr* instance();

    void LoadFromDB();

    // Ciclo de vida del jugador
    void OnPlayerLogin(uint32 playerGuid, uint32 guildId);
    void OnPlayerLogout(uint32 playerGuid);

    // Ciclo de vida de la hermandad
    void OnGuildCreate(uint32 guildId);
    void OnGuildDisband(uint32 guildId);

    // Recompensas de misión
    [[nodiscard]] bool HasGuildReward(uint32 questId) const;
    [[nodiscard]] GuildQuestReward const* GetQuestReward(uint32 questId) const;

    // Reclamaciones
    [[nodiscard]] bool IsQuestClaimed(uint32 guildId, uint32 questId) const;
    void ClaimQuest(uint32 guildId, uint32 questId, uint32 claimerGuid);

    // Progresión
    uint32 AddSeasonPoints(uint32 guildId, uint32 contributorGuid, uint32 points);
    void   AddGuildReputation(uint32 playerGuid, uint32 amount);
    void   ResetMemberOnLeave(uint32 playerGuid, uint32 guildId);

    // Getters
    [[nodiscard]] GuildSeasonData const*  GetGuildData(uint32 guildId) const;
    [[nodiscard]] MemberSeasonData const* GetMemberData(uint32 playerGuid) const;
    [[nodiscard]] uint8 GetCurrentSeasonId() const { return _currentSeason; }

    // Tablón
    [[nodiscard]] std::unordered_set<uint32> const& GetActiveBoardQuests(GuildBoardType type) const;

    // Perks
    void ApplyGuildPerks(Player* player, uint32 guildId);
    void RemoveGuildPerks(Player* player, uint32 guildId);
    void CheckAndUnlockPerks(uint32 guildId);

    // Timers (llamados desde WorldScript)
    void CheckSeasonReset();
    void CheckBoardRefresh();

private:
    GuildSeasonMgr() = default;

    void ResetSeason();
    void LoadSeasonConfig();
    void LoadPerksFromDB();
    void AwardTopGuilds();
    void RefreshVendor();
    void LoadBoardFromDB();
    void RefreshBoard(GuildBoardType type);
    void ClearBoardClaims(GuildBoardType type);

    // --- Temporada ---
    uint8  _currentSeason    = 1;
    uint32 _seasonStartTime  = 0;
    uint32 _seasonDuration   = 0;
    uint32 _dailyCap         = 0;
    bool   _resetRepOnLeave  = true;
    bool   _announceReset    = true;

    // --- Tablón ---
    uint32 _boardDailyCount  = 5;
    uint32 _boardWeeklyCount = 2;
    uint32 _vendorItemCount  = 10;
    uint32 _lastDailyRefresh  = 0;
    uint32 _lastWeeklyRefresh = 0;

    std::vector<uint32> _dailyPool;   // pool completo de diarias
    std::vector<uint32> _weeklyPool;  // pool completo de semanales

    std::unordered_set<uint32> _activeDailyQuests;
    std::unordered_set<uint32> _activeWeeklyQuests;

    // --- Progresión ---
    // rank (1-3) -> lista de ítems premio de la temporada actual
    std::unordered_map<uint8, std::vector<SeasonPrizeItem>> _currentPrizes;

    // perks ordenados por Points ascendente
    std::vector<GuildPerk> _perks;

    std::unordered_map<uint32, GuildQuestReward>           _questRewards;
    std::unordered_map<uint32, GuildSeasonData>            _guildData;
    std::unordered_map<uint32, std::unordered_set<uint32>> _claimedQuests;
    std::unordered_map<uint32, MemberSeasonData>           _memberData;
};

#define sGuildSeasonMgr GuildSeasonMgr::instance()

#endif // GUILD_SEASON_MGR_H
