-- mod-guild-season: static content (tiers + perks)
-- Applied to: acore_characters

-- Tramos de reputación personal de hermandad
CREATE TABLE IF NOT EXISTS `guild_season_rep_tiers` (
    `TierLevel`   tinyint unsigned NOT NULL,
    `Name`        varchar(50)      NOT NULL,
    `RequiredRep` int unsigned     NOT NULL,
    PRIMARY KEY (`TierLevel`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT IGNORE INTO `guild_season_rep_tiers` (`TierLevel`, `Name`, `RequiredRep`) VALUES
(0, 'Recluta',  0),
(1, 'Iniciado', 3000),
(2, 'Hermano',  9000),
(3, 'Veterano', 21000),
(4, 'Guardián', 42000),
(5, 'Leyenda',  75000);

-- Perks pasivos de hermandad por umbral de Puntos de Temporada
-- SpellId: hechizo que se aplica a todos los miembros online al desbloquear
CREATE TABLE IF NOT EXISTS `guild_season_perks` (
    `PerkId`  tinyint unsigned NOT NULL,
    `Points`  int unsigned     NOT NULL COMMENT 'PTH requeridos para desbloquear',
    `SpellId` int unsigned     NOT NULL COMMENT 'Hechizo aplicado a los miembros',
    PRIMARY KEY (`PerkId`),
    KEY `idx_points` (`Points`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Filas de ejemplo — reemplazar SpellId con los hechizos reales
-- INSERT INTO `guild_season_perks` (`PerkId`, `Points`, `SpellId`) VALUES
-- (1,   5000, 0),
-- (2,  15000, 0),
-- (3,  35000, 0),
-- (4,  75000, 0),
-- (5, 150000, 0);

-- Pool de ítems del vendor de hermandad (administrado manualmente)
CREATE TABLE IF NOT EXISTS `guild_vendor_pool` (
    `ItemEntry`    int unsigned     NOT NULL,
    `TierRequired` tinyint unsigned NOT NULL DEFAULT 0 COMMENT 'Tramo mínimo de reputación personal (0-5)',
    `Price`        int unsigned     NOT NULL DEFAULT 0 COMMENT 'Precio en cobre (0 = precio base del ítem)',
    `Enabled`      tinyint unsigned NOT NULL DEFAULT 1,
    PRIMARY KEY (`ItemEntry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Ítems activos del vendor para la temporada actual (generado automáticamente)
CREATE TABLE IF NOT EXISTS `guild_vendor_active` (
    `ItemEntry`    int unsigned     NOT NULL,
    `TierRequired` tinyint unsigned NOT NULL DEFAULT 0,
    `Price`        int unsigned     NOT NULL DEFAULT 0,
    `SeasonId`     tinyint unsigned NOT NULL,
    PRIMARY KEY (`ItemEntry`, `SeasonId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
