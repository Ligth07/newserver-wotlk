# Sistema de Temporadas de Hermandad

> Documento de diseño e implementación. **Módulo: `mod-guild-season`**
> Requiere rama del core: `feat/guild-quest-type`

---

## 1. Resumen

Sistema de progresión de hermandad por temporadas. Los jugadores completan misiones del **Tablón de Hermandad** (diarias y semanales) que otorgan reputación personal permanente y puntos de temporada colectivos. Al cierre de cada temporada se premia al top 3 de hermandades y se reinicia la progresión colectiva.

---

## 2. Ejes de progresión

### Eje 1 — Puntos de Temporada de Hermandad (PTH)

- Acumulado colectivo de la hermandad, **sin límite máximo**
- Al cruzar umbrales definidos en DB se desbloquean **perks pasivos** (spells) para todos los miembros
- Determina el **ranking global** entre hermandades al cierre de temporada
- **Resetea** al inicio de cada nueva temporada (perks incluidos)
- Cap diario por jugador configurable (`GuildSeason.DailyCap.Points`; 0 = sin límite)

### Eje 2 — Reputación Personal de Hermandad

- Valor individual por miembro, **permanente** (no resetea entre temporadas)
- Desbloquea tramos del **Vendor de Hermandad** (`TierRequired` en `guild_vendor_active`)
- Comportamiento al abandonar la hermandad configurable (`GuildSeason.ResetRepOnLeave`)
- Al unirse a una hermandad nueva el jugador empieza con 0 (si la opción está activa)

### Eje 3 — Contribución de Temporada (ranking interno)

- Cuántos PTH ha aportado personalmente el miembro esta temporada
- Resetea con la temporada
- Usado para el ranking interno de la hermandad

---

## 3. Tablón de Misiones

Misiones con `quest_template_addon.RewardGuildPoints > 0` o `RewardGuildRep > 0` son misiones de hermandad. El tablón muestra un subconjunto rotativo del pool configurado en `guild_board_pool`.

### Rotación automática

- **Diarias**: se sincronizan con el reset diario de misiones del servidor (`GetNextDailyQuestsResetTime`)
- **Semanales**: se sincronizan con el reset semanal (`GetNextWeeklyQuestsResetTime`)
- Al rotar, se limpian las reclamaciones de las misiones que salen del tablón
- La cantidad mostrada es configurable: `GuildSeason.Board.DailyCount` y `GuildSeason.Board.WeeklyCount`

### Mecánica de recompensa al entregar

```
Toda misión de hermandad:
  → guild_member.GuildReputation += RewardGuildRep  (siempre, a todos)

Misión INDIVIDUAL (RequiredGuildMembers == 0):
  → guild.SeasonPoints              += RewardGuildPoints  (siempre)
  → guild_member.SeasonContribution += RewardGuildPoints
  → respeta cap diario si está configurado

Misión GRUPAL (RequiredGuildMembers > 0):
  Solo la PRIMERA entrega de la temporada:
    → guild.SeasonPoints              += RewardGuildPoints
    → guild_member.SeasonContribution += RewardGuildPoints
    → registrar en guild_season_quest_claims
  Entregas siguientes:
    → solo reputación personal (nada de PTH)
```

Después de añadir puntos se llama a `CheckAndUnlockPerks` para verificar nuevos umbrales.

---

## 4. Temporadas

- Duración configurable por conf (`GuildSeason.Duration.Days`) o por DB en `guild_season_config` (la DB tiene precedencia)
- Reset automático vía `WorldScript::OnUpdate`, verificado cada `GuildSeason.ResetCheckInterval.Hours`
- Al resetear:
  1. Se ejecuta `AwardTopGuilds()` — envía premios por correo al líder del top 3
  2. Se eliminan perks a todos los miembros online
  3. `guild.SeasonPoints` → 0, `guild.SeasonPerksFlags` → 0
  4. `guild_member.SeasonContribution` → 0
  5. `guild_member.GuildReputation` — **intacto** (permanente)
  6. Se limpia `_claimedQuests` en memoria
  7. `guild_season_state` se actualiza con el nuevo `SeasonId` y `StartTime`
  8. Se recarga config desde `guild_season_config` y se llama a `RefreshVendor()`
- Si `GuildSeason.AnnounceReset = 1`, se notifica a todos los jugadores online

---

## 5. Perks pasivos

- Definidos en la tabla `guild_season_perks` (DB), cargados al startup
- Cada perk tiene un `SpellId` que se aplica/quita al jugador con `CastSpell`/`RemoveAurasDueToSpell`
- Se desbloquean cuando `guild.SeasonPoints` supera el campo `Points` del perk
- El estado se persiste como bitmask en `guild.SeasonPerksFlags` (`1 << PerkId`)
- Se aplican al login del jugador, al unirse a la hermandad y al desbloquearse en vivo
- Se quitan al abandonar la hermandad, al disolverse y al resetear la temporada

---

## 6. Vendor de Hermandad

- Pool de ítems configurado en `guild_vendor_pool` (con `TierRequired`, `Price`, `Enabled`)
- Al inicio de cada temporada se seleccionan `GuildSeason.Vendor.ItemCount` ítems al azar y se insertan en `guild_vendor_active`
- `TierRequired` permite bloquear ítems según la reputación personal del comprador (lógica de acceso a implementar en la UI/NPC)

---

## 7. Base de datos

### Tablas modificadas (core — `acore_characters`)

```sql
ALTER TABLE guild
    ADD COLUMN SeasonPoints     bigint unsigned NOT NULL DEFAULT 0,
    ADD COLUMN SeasonPerksFlags int unsigned    NOT NULL DEFAULT 0;

ALTER TABLE guild_member
    ADD COLUMN GuildReputation    int unsigned NOT NULL DEFAULT 0,
    ADD COLUMN SeasonContribution int unsigned NOT NULL DEFAULT 0;
```

### Columnas nuevas en el core (`acore_world` — `quest_template_addon`)

```sql
ALTER TABLE quest_template_addon
    ADD COLUMN RewardGuildPoints int unsigned NOT NULL DEFAULT 0 AFTER RequiredGuildMembers,
    ADD COLUMN RewardGuildRep    int unsigned NOT NULL DEFAULT 0 AFTER RewardGuildPoints;
```

### Tablas nuevas del módulo (`acore_characters`)

```sql
-- Reclamaciones de misiones grupales por hermandad y temporada
guild_season_quest_claims (GuildId, QuestId, SeasonId, ClaimedBy, ClaimedAt)
  PK: (GuildId, QuestId, SeasonId)

-- Estado activo de la temporada (una sola fila, lock=1)
guild_season_state (lock, SeasonId, StartTime, LastDailyRefresh, LastWeeklyRefresh)
  PK: lock

-- Configuración por temporada (permite pre-configurar temporadas futuras)
guild_season_config (SeasonId, DurationDays, Description)
  PK: SeasonId

-- Premios al top 3 al cierre de temporada (múltiples ítems por rango)
guild_season_prizes (SeasonId, Rank, ItemEntry, ItemCount)
  PK: (SeasonId, Rank, ItemEntry)

-- Pool de misiones disponibles para el tablón
guild_board_pool (QuestId, Type, Enabled)
  PK: QuestId   -- Type: 0=diaria, 1=semanal

-- Misiones activas en el tablón (generadas automáticamente)
guild_board_active (QuestId, Type, ExpiresAt)
  PK: QuestId

-- Pool completo de ítems del vendor de hermandad
guild_vendor_pool (ItemEntry, TierRequired, Price, Enabled)

-- Ítems activos del vendor por temporada (selección aleatoria)
guild_vendor_active (ItemEntry, TierRequired, Price, SeasonId)

-- Perks definidos (SpellId aplicado al superar Points de PTH)
guild_season_perks (PerkId, Points, SpellId)
  PK: PerkId
```

---

## 8. Archivos

| Archivo | Propósito |
|---|---|
| `src/GuildSeason.cpp` | Scripts: `PlayerScript` (login/logout/quest), `GuildScript` (join/leave/create/disband), `WorldScript` (startup/update) |
| `src/GuildSeasonMgr.h` | Manager singleton: structs, interfaz pública |
| `src/GuildSeasonMgr.cpp` | Lógica: carga DB, AddPoints, ClaimQuest, Perks, Vendor, Tablón, ResetSeason, AwardTopGuilds |
| `src/mod_guild_season_loader.cpp` | Registro del módulo (`AddGuildSeasonScripts`) |
| `data/sql/db-characters/guild_season_schema.sql` | Todas las tablas nuevas + ALTER guild/guild_member |
| `data/sql/db-characters/guild_season_content.sql` | Datos de ejemplo (config, premios, pool) |
| `data/sql/db-world/quest_template_addon_guild_rewards.sql` | ALTER quest_template_addon |
| `conf/mod-guild-season.conf.dist` | Configuración del módulo |

---

## 9. Configuración

```ini
GuildSeason.Enable                    = 1      ; Activa el módulo
GuildSeason.Duration.Days             = 30     ; Fallback si no hay fila en guild_season_config
GuildSeason.ResetCheckInterval.Hours  = 1      ; Frecuencia de verificación del reset
GuildSeason.AnnounceReset             = 1      ; Anunciar inicio de nueva temporada
GuildSeason.DailyCap.Points           = 0      ; Cap diario de PTH por jugador (0 = sin límite)
GuildSeason.ResetRepOnLeave           = 1      ; Resetear reputación al abandonar hermandad
GuildSeason.Board.DailyCount          = 5      ; Misiones diarias en el tablón
GuildSeason.Board.WeeklyCount         = 2      ; Misiones semanales en el tablón
GuildSeason.Vendor.ItemCount          = 10     ; Ítems activos del vendor por temporada
```

> Los umbrales y spells de perks se definen directamente en la tabla `guild_season_perks`.

---

## 10. Pendientes

- Lógica de acceso al vendor según `TierRequired` (NPC gossip / AIO)
- Definir tramos de reputación personal y qué desbloquea cada uno
- UI del tablón (AIO Lua)
- UI del vendor de hermandad
