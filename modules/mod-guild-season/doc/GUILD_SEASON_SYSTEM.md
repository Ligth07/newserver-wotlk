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
- Al cruzar umbrales configurables se desbloquean **perks pasivos** para todos los miembros
- Determina el **ranking global** entre hermandades al cierre de temporada
- **Resetea** al inicio de cada nueva temporada (perks incluidos)

### Eje 2 — Reputación Personal de Hermandad

- Valor individual por miembro, **permanente** (no resetea entre temporadas)
- Desbloquea tramos del **Vendor de Hermandad** (nuevos ítems cada temporada)
- **Se pierde al abandonar la hermandad** — ítems ya comprados se conservan
- Al unirse a una hermandad nueva el jugador empieza con 0

### Eje 3 — Contribución de Temporada (ranking interno)

- Cuántos PTH ha aportado personalmente el miembro esta temporada
- Solo se incrementa cuando el jugador es el **primero** en entregar una misión (reclama los PTH para la hermandad)
- Resetea con la temporada
- Usado para el ranking interno de la hermandad

---

## 3. Tablón de Misiones

- NPC o objeto en el mundo que muestra las misiones disponibles
- Todas las misiones tienen `ZoneOrSort = -400` (`QUEST_SORT_GUILD`)
- Tipos: **diarias** (menos PTH/rep) y **semanales** (más PTH/rep)
- Cada misión puede reclamar PTH para la hermandad **una sola vez por temporada**

### Regla de reclamación

```
Primer jugador en entregar:
  → guild_member.GuildReputation    += RewardGuildRep
  → guild.SeasonPoints              += RewardGuildPoints
  → guild_member.SeasonContribution += RewardGuildPoints
  → registrar en guild_season_quest_claims

Jugadores siguientes:
  → guild_member.GuildReputation    += RewardGuildRep
  → (nada más para la hermandad)
```

---

## 4. Temporadas

- Duración configurable (por defecto 30 días)
- Reset automático vía timer en WorldScript
- Al resetear:
  - `guild.SeasonPoints` → 0
  - `guild.SeasonPerksFlags` → 0
  - `guild_member.SeasonContribution` → 0
  - `guild_member.GuildReputation` **intacto**
- Top 3 hermandades reciben premios al cierre (a definir por temporada)

---

## 5. Base de datos

### Tablas modificadas (core — `acore_characters`)

```sql
ALTER TABLE guild
    ADD COLUMN SeasonPoints     bigint unsigned NOT NULL DEFAULT 0,
    ADD COLUMN SeasonPerksFlags int unsigned    NOT NULL DEFAULT 0;

ALTER TABLE guild_member
    ADD COLUMN GuildReputation    int unsigned NOT NULL DEFAULT 0,
    ADD COLUMN SeasonContribution int unsigned NOT NULL DEFAULT 0;
```

### Tabla nueva (módulo — `acore_characters`)

```sql
CREATE TABLE guild_season_quest_claims (
    GuildId    int unsigned     NOT NULL,
    QuestId    int unsigned     NOT NULL,
    SeasonId   tinyint unsigned NOT NULL,
    ClaimedBy  int unsigned     NOT NULL,
    ClaimedAt  int unsigned     NOT NULL,
    PRIMARY KEY (GuildId, QuestId, SeasonId)
);
```

### Columnas nuevas en el core (`acore_world` — `quest_template_addon`)

```sql
ALTER TABLE quest_template_addon
    ADD COLUMN RewardGuildPoints int unsigned NOT NULL DEFAULT 0,
    ADD COLUMN RewardGuildRep    int unsigned NOT NULL DEFAULT 0;
```

---

## 6. Archivos

| Archivo | Propósito |
|---|---|
| `src/GuildSeason.cpp` | Hooks: `OnQuestReward`, `OnGuildMemberLeave` |
| `src/GuildSeasonMgr.h/.cpp` | Manager singleton: carga, AddPoints, ClaimQuest, ResetSeason |
| `data/sql/db-characters/` | ALTER TABLE guild/guild_member + CREATE guild_season_quest_claims |
| `data/sql/db-world/` | ALTER TABLE quest_template_addon |
| `conf/mod-guild-season.conf.dist` | Configuración del módulo |

---

## 7. Configuración

```ini
GuildSeason.Enable           = 1
GuildSeason.Duration.Days    = 30
GuildSeason.DailyCap.Points  = 0       ; 0 = sin cap
GuildSeason.PerkThreshold.1  = 5000
GuildSeason.PerkThreshold.2  = 15000
GuildSeason.PerkThreshold.3  = 35000
```

---

## 8. Pendientes

- Definir perks concretos por umbral
- Definir premios del top 3 por temporada
- Definir tramos de reputación personal y qué desbloquea cada uno
- UI del tablón (AIO Lua)
- Vendor de hermandad
