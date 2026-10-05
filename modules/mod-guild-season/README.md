# mod-guild-season

Sistema de progresión de hermandad por temporadas para AzerothCore (WotLK 3.3.5a).

## Descripción

Introduce un **Tablón de Misiones de Hermandad** con misiones diarias y semanales. Completarlas otorga reputación personal permanente al jugador y Puntos de Temporada colectivos a la hermandad. Al cierre de cada temporada se premia al top 3 de hermandades y se reinicia la progresión colectiva.

## Dependencias

- Rama del core: `feat/guild-quest-type`
  - Provee `ZoneOrSort = -400` (QUEST_SORT_GUILD), `IsGuildQuest()`, `RequiredGuildMembers`, `RewardGuildPoints`, `RewardGuildRep` y los hooks de fallo/completado de misiones de hermandad.

## Instalación

1. Copiar el módulo en `modules/mod-guild-season/`
2. Recompilar AzerothCore
3. Importar SQL manualmente:
   - `data/sql/db-characters/guild_season_schema.sql` → base de datos `acore_characters`
   - `data/sql/db-characters/guild_season_content.sql` → base de datos `acore_characters`
   - `data/sql/db-world/quest_template_addon_guild_rewards.sql` → base de datos `acore_world`
4. Copiar `conf/mod-guild-season.conf.dist` a `mod-guild-season.conf` y ajustar valores
5. Reiniciar el servidor

## Configuración

Ver `conf/mod-guild-season.conf.dist` para todas las opciones disponibles.

## Cómo configurar una temporada

### 1. Definir la temporada en DB

```sql
INSERT INTO guild_season_config (SeasonId, DurationDays, Description)
VALUES (1, 30, 'Temporada 1: El Despertar');
```

### 2. Definir premios

```sql
INSERT INTO guild_season_prizes (SeasonId, Rank, ItemEntry, ItemCount) VALUES
(1, 1, 12345, 1),
(1, 2, 12346, 1),
(1, 3, 12347, 1);
```

### 3. Añadir misiones al tablón

Las misiones deben existir en `quest_template` con `ZoneOrSort = -400` y tener valores en `quest_template_addon`:

```sql
UPDATE quest_template_addon SET RewardGuildPoints = 100, RewardGuildRep = 50 WHERE Id = XXXX;
INSERT INTO guild_board_pool (QuestId, Type) VALUES (XXXX, 0); -- 0=diaria, 1=semanal
```

### 4. Configurar perks

Crear hechizos pasivos en el servidor y registrarlos:

```sql
INSERT INTO guild_season_perks (PerkId, Points, SpellId) VALUES
(1,   5000, 99001),
(2,  15000, 99002),
(3,  35000, 99003);
```

### 5. Añadir ítems al vendor

```sql
INSERT INTO guild_vendor_pool (ItemEntry, TierRequired, Price) VALUES
(49426, 0, 100000),   -- disponible desde Recluta
(49427, 3, 500000);   -- requiere Veterano
```

## Diseño del sistema

Ver `doc/GUILD_SEASON_SYSTEM.md` para el diseño completo.

## UI

La interfaz de usuario (tablón, vendor, ranking, panel de reputación) se implementa mediante AIO Lua. Ver `doc/AIO_FRONTEND_INSTRUCTIONS.md` para las instrucciones de desarrollo.
