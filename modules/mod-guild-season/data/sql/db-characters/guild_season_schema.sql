-- mod-guild-season: guild season progression schema
-- Applied to: acore_characters

ALTER TABLE `guild`
    ADD COLUMN `SeasonPoints`     bigint unsigned NOT NULL DEFAULT 0,
    ADD COLUMN `SeasonPerksFlags` int unsigned    NOT NULL DEFAULT 0;

ALTER TABLE `guild_member`
    ADD COLUMN `GuildReputation`    int unsigned NOT NULL DEFAULT 0,
    ADD COLUMN `SeasonContribution` int unsigned NOT NULL DEFAULT 0;

-- Historial de reclamaciones de misiones por hermandad y temporada
CREATE TABLE IF NOT EXISTS `guild_season_quest_claims` (
    `GuildId`   int unsigned     NOT NULL,
    `QuestId`   int unsigned     NOT NULL,
    `SeasonId`  tinyint unsigned NOT NULL,
    `ClaimedBy` int unsigned     NOT NULL COMMENT 'GUID del jugador que reclamó los puntos',
    `ClaimedAt` int unsigned     NOT NULL COMMENT 'Unix timestamp de la reclamación',
    PRIMARY KEY (`GuildId`, `QuestId`, `SeasonId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Estado de temporada activa y último refresh del tablón (una sola fila)
CREATE TABLE IF NOT EXISTS `guild_season_state` (
    `lock`              tinyint unsigned NOT NULL DEFAULT 1,
    `SeasonId`          tinyint unsigned NOT NULL DEFAULT 1,
    `StartTime`         int unsigned     NOT NULL DEFAULT 0,
    `LastDailyRefresh`  int unsigned     NOT NULL DEFAULT 0,
    `LastWeeklyRefresh` int unsigned     NOT NULL DEFAULT 0,
    PRIMARY KEY (`lock`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT IGNORE INTO `guild_season_state` (`lock`, `SeasonId`, `StartTime`, `LastDailyRefresh`, `LastWeeklyRefresh`)
VALUES (1, 1, UNIX_TIMESTAMP(), 0, 0);

-- Configuración por temporada (pre-configurable para temporadas futuras)
CREATE TABLE IF NOT EXISTS `guild_season_config` (
    `SeasonId`     tinyint unsigned NOT NULL,
    `DurationDays` tinyint unsigned NOT NULL DEFAULT 30,
    `Description`  varchar(255)     NOT NULL DEFAULT '',
    PRIMARY KEY (`SeasonId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT IGNORE INTO `guild_season_config` (`SeasonId`, `DurationDays`, `Description`)
VALUES (1, 30, 'Temporada 1');

-- Premios por temporada y posición (múltiples ítems por posición)
CREATE TABLE IF NOT EXISTS `guild_season_prizes` (
    `SeasonId`  tinyint unsigned  NOT NULL,
    `Rank`      tinyint unsigned  NOT NULL COMMENT '1=primero, 2=segundo, 3=tercero',
    `ItemEntry` int unsigned      NOT NULL,
    `ItemCount` smallint unsigned NOT NULL DEFAULT 1,
    PRIMARY KEY (`SeasonId`, `Rank`, `ItemEntry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Pool de misiones disponibles para el tablón (administrado manualmente)
CREATE TABLE IF NOT EXISTS `guild_board_pool` (
    `QuestId` int unsigned     NOT NULL,
    `Type`    tinyint unsigned NOT NULL DEFAULT 0 COMMENT '0=diaria, 1=semanal',
    `Enabled` tinyint unsigned NOT NULL DEFAULT 1,
    PRIMARY KEY (`QuestId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Misiones activas del tablón (generadas automáticamente por el módulo)
CREATE TABLE IF NOT EXISTS `guild_board_active` (
    `QuestId`   int unsigned     NOT NULL,
    `Type`      tinyint unsigned NOT NULL COMMENT '0=diaria, 1=semanal',
    `ExpiresAt` int unsigned     NOT NULL COMMENT 'Unix timestamp del próximo reset',
    PRIMARY KEY (`QuestId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
