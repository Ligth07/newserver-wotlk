# Instrucciones para el Frontend AIO — mod-guild-season

> Este documento describe qué debe mostrar cada panel de la UI, qué tablas consultar
> y qué lógica aplicar. No contiene código — solo diseño e instrucciones.

---

## Contexto general

El sistema tiene cuatro superficies de UI:

1. **Tablón de misiones** — misiones diarias y semanales de hermandad
2. **Panel de hermandad** — progresión colectiva, perks, ranking
3. **Panel de reputación personal** — standing del jugador con su hermandad
4. **Vendor de hermandad** — tienda filtrada por tier de reputación

Todas las tablas están en la base de datos `acore_characters` salvo `quest_template` y `quest_template_addon` que están en `acore_world`.

---

## Tablas relevantes y sus campos clave

### `guild_season_state`
Estado de la temporada activa. Siempre tiene una sola fila.

| Campo | Descripción |
|---|---|
| `SeasonId` | ID de la temporada activa |
| `StartTime` | Unix timestamp de inicio |
| `LastDailyRefresh` | Último refresh del tablón diario |
| `LastWeeklyRefresh` | Último refresh del tablón semanal |

### `guild_season_config`
Configuración por temporada. JOIN con `guild_season_state` por `SeasonId`.

| Campo | Descripción |
|---|---|
| `SeasonId` | ID de la temporada |
| `DurationDays` | Duración en días |
| `Description` | Nombre/tema de la temporada (ej: "Temporada 1: El Despertar") |

### `guild_board_active`
Misiones activas en el tablón hoy/esta semana. El módulo la gestiona automáticamente.

| Campo | Descripción |
|---|---|
| `QuestId` | ID de la misión |
| `Type` | `0` = diaria, `1` = semanal |
| `ExpiresAt` | Unix timestamp de expiración |

### `guild_season_quest_claims`
Registro de qué misiones ya aportaron puntos a cada hermandad esta temporada.

| Campo | Descripción |
|---|---|
| `GuildId` | ID de la hermandad |
| `QuestId` | ID de la misión reclamada |
| `SeasonId` | Temporada en la que se reclamó |
| `ClaimedBy` | GUID del jugador que reclamó |

Para saber si una misión ya fue reclamada por la hermandad del jugador:
> `SELECT 1 FROM guild_season_quest_claims WHERE GuildId = X AND QuestId = Y AND SeasonId = Z`

### `quest_template` (acore_world)
Información de la misión. JOIN por `QuestId`.

| Campo | Descripción |
|---|---|
| `ID` | ID de la misión |
| `Title` | Nombre de la misión |
| `QuestLevel` | Nivel requerido |
| `Flags` | Flags nativos (0x1000 = diaria) |
| `ZoneOrSort` | `-400` para misiones de hermandad |

### `quest_template_addon` (acore_world)
Campos extendidos de la misión.

| Campo | Descripción |
|---|---|
| `Id` | ID de la misión |
| `RequiredGuildMembers` | `0` = individual, `>0` = grupal/raid |
| `RewardGuildPoints` | Puntos de temporada que aporta |
| `RewardGuildRep` | Reputación personal que otorga |

### `guild`
Datos de la hermandad. Requiere que el jugador tenga `GuildId`.

| Campo | Descripción |
|---|---|
| `guildid` | ID de la hermandad |
| `name` | Nombre de la hermandad |
| `SeasonPoints` | Puntos de temporada acumulados |
| `SeasonPerksFlags` | Bitmask de perks desbloqueados |

### `guild_member`
Datos del miembro dentro de la hermandad.

| Campo | Descripción |
|---|---|
| `guid` | GUID del jugador |
| `guildid` | ID de la hermandad |
| `GuildReputation` | Reputación personal acumulada (permanente) |
| `SeasonContribution` | PTH aportados esta temporada por este miembro |

### `guild_season_rep_tiers`
Definición de los tramos de reputación personal.

| Campo | Descripción |
|---|---|
| `TierLevel` | Nivel del tramo (0 al 5) |
| `Name` | Nombre del tramo (ej: "Veterano") |
| `RequiredRep` | Reputación necesaria para alcanzar este tramo |

Para calcular el tramo actual del jugador:
> Buscar el `TierLevel` más alto donde `RequiredRep <= GuildReputation del jugador`

### `guild_season_perks`
Definición de perks por umbral de PTH.

| Campo | Descripción |
|---|---|
| `PerkId` | ID del perk |
| `Points` | PTH de hermandad necesarios para desbloquearlo |
| `SpellId` | ID del hechizo aplicado (informativo para UI) |

Para saber si un perk está desbloqueado:
> `SeasonPerksFlags & (1 << PerkId) != 0`

### `guild_season_prizes`
Premios del top 3 al cierre de temporada. Solo para mostrar en UI.

| Campo | Descripción |
|---|---|
| `SeasonId` | Temporada |
| `Rank` | Posición (1, 2 o 3) |
| `ItemEntry` | ID del ítem premiado |
| `ItemCount` | Cantidad |

### `guild_vendor_active`
Ítems disponibles en el vendor de hermandad esta temporada.

| Campo | Descripción |
|---|---|
| `ItemEntry` | ID del ítem |
| `TierRequired` | Tramo mínimo requerido para comprar (0-5) |
| `Price` | Precio en cobre (`0` = usar precio base del ítem) |
| `SeasonId` | Temporada a la que pertenece |

---

## Componente 1 — Tablón de Misiones

### Descripción general

Ventana abierta desde un NPC. Muestra dos secciones: misiones diarias arriba y semanales abajo. Cada misión muestra su estado respecto a la hermandad del jugador.

### Layout genérico

```
┌─────────────────────────────────────────────────────┐
│  TABLÓN DE HERMANDAD          Temporada 1 · Día 12  │
├─────────────────────────────────────────────────────┤
│  MISIONES DIARIAS                    ⏱ Reset: 06:00 │
│                                                     │
│  [Icono] Nombre misión              +100 PTH +50 Rep│
│          Individual · Lv80          ✓ RECLAMADA     │
│                                                     │
│  [Icono] Nombre misión              +100 PTH +50 Rep│
│          Grupal (3) · Lv80          DISPONIBLE      │
│                                                     │
│  [Icono] Nombre misión              +100 PTH +50 Rep│
│          Raid (10) · Lv80           DISPONIBLE      │
├─────────────────────────────────────────────────────┤
│  MISIONES SEMANALES                  ⏱ Reset: Lunes │
│                                                     │
│  [Icono] Nombre misión              +500 PTH +200Rep│
│          Grupal (5) · Lv80          DISPONIBLE      │
└─────────────────────────────────────────────────────┘
```

### Lógica por misión

- Mostrar nombre, nivel, tipo (individual/grupal/raid) y recompensas `RewardGuildPoints` + `RewardGuildRep`
- Para misiones grupales mostrar `RequiredGuildMembers` como "(N miembros)"
- Estado de la misión:
  - **RECLAMADA** — existe fila en `guild_season_quest_claims` para `GuildId` + `QuestId` + `SeasonId` del jugador
  - **DISPONIBLE** — no existe dicha fila
- **Ambos tipos de misión** siempre otorgan reputación personal a cualquier jugador que las complete
- Para misiones **individuales** RECLAMADAS: mostrar como DISPONIBLE — cada jugador puede completarla y seguir ganando rep. Los PTH ya no se otorgan a la hermandad pero la recompensa personal sigue activa
- Para misiones **grupales/raid** RECLAMADAS: mostrar como RECLAMADA — otro grupo ya aportó los PTH a la hermandad, pero el jugador aún puede completarla y recibir su reputación personal
- Mostrar tiempo restante hasta `ExpiresAt` de la misión

### Consultas necesarias

1. Obtener misiones activas del tablón con sus datos:
   - `guild_board_active` JOIN `quest_template` JOIN `quest_template_addon`
2. Obtener la temporada actual: `guild_season_state`
3. Obtener claims de la hermandad del jugador: `guild_season_quest_claims`

---

## Componente 2 — Panel de Hermandad

### Descripción general

Muestra el progreso colectivo de la hermandad: puntos de temporada, perks desbloqueados, ranking estimado y premios de la temporada.

### Layout genérico

```
┌─────────────────────────────────────────────────────┐
│  HERMANDAD: Los Inmortales       Temporada 1        │
├─────────────────────────────────────────────────────┤
│  Puntos de Temporada                                │
│  ████████████░░░░░░  12.400 PTH                     │
│  Próximo perk: Flujo de Oro (15.000 PTH)            │
├─────────────────────────────────────────────────────┤
│  PERKS ACTIVOS                                      │
│  ✓ [Icono hechizo] Vía Rápida                       │
│  ✗ [Icono hechizo] Flujo de Oro      15.000 PTH     │
│  ✗ [Icono hechizo] Prestigio         35.000 PTH     │
├─────────────────────────────────────────────────────┤
│  PREMIOS TOP 3 ESTA TEMPORADA                       │
│  🥇 [Ítem] x1    🥈 [Ítem] x1    🥉 [Ítem] x1      │
├─────────────────────────────────────────────────────┤
│  RANKING INTERNO          Tu contribución: 3.200 PTH│
│  1. Nombre           5.800 PTH                      │
│  2. Nombre           4.100 PTH                      │
│  3. Tú               3.200 PTH                      │
└─────────────────────────────────────────────────────┘
```

### Lógica

- **Barra de progreso**: mostrar hacia el próximo umbral de perk no desbloqueado
- **Perks**: iterar `guild_season_perks` ordenado por `Points`. Verificar si está activo con `SeasonPerksFlags & (1 << PerkId)`
- **Premios**: leer `guild_season_prizes` WHERE `SeasonId` = actual
- **Ranking interno**: leer `guild_member` WHERE `guildid` = X, ordenar por `SeasonContribution DESC`

### Consultas necesarias

1. `guild` — `SeasonPoints`, `SeasonPerksFlags`, nombre
2. `guild_season_perks` — lista de perks con umbrales
3. `guild_season_prizes` — premios de la temporada actual
4. `guild_member` — ranking interno por `SeasonContribution`
5. `guild_season_state` — SeasonId actual

---

## Componente 3 — Panel de Reputación Personal

### Descripción general

Muestra el standing personal del jugador con su hermandad y su progreso hacia el siguiente tramo.

### Layout genérico

```
┌─────────────────────────────────────────────────────┐
│  TU REPUTACIÓN CON LA HERMANDAD                     │
├─────────────────────────────────────────────────────┤
│  Tramo actual:  ★★★  VETERANO                       │
│                                                     │
│  Progreso:  21.000 / 42.000                         │
│  ████████████░░░░░░  hacia Guardián                 │
│                                                     │
│  Recluta → Iniciado → Hermano → Veterano            │
│  → [Guardián] → Leyenda                             │
│                                                     │
│  Tu contribución esta temporada: 3.200 PTH          │
└─────────────────────────────────────────────────────┘
```

### Lógica

- Leer `GuildReputation` del jugador desde `guild_member`
- Calcular tramo actual: recorrer `guild_season_rep_tiers` y encontrar el `TierLevel` más alto donde `RequiredRep <= GuildReputation`
- Mostrar progreso hacia el siguiente tramo
- Mostrar `SeasonContribution` del jugador

### Consultas necesarias

1. `guild_member` WHERE `guid` = playerGuid — `GuildReputation`, `SeasonContribution`
2. `guild_season_rep_tiers` — todos los tramos ordenados por `TierLevel ASC`

---

## Componente 4 — Vendor de Hermandad

### Descripción general

Tienda de ítems exclusivos. Solo muestra los ítems que el jugador puede comprar según su tramo de reputación. Los ítems disponibles cambian cada temporada.

### Layout genérico

```
┌─────────────────────────────────────────────────────┐
│  TIENDA DE HERMANDAD          Temporada 1           │
│  Tu tramo: Veterano (3)                             │
├─────────────────────────────────────────────────────┤
│  DISPONIBLES PARA TI                                │
│                                                     │
│  [Icono] Nombre del ítem          Precio            │
│          Desde: Recluta                             │
│                                                     │
│  [Icono] Nombre del ítem          Precio            │
│          Desde: Veterano                            │
│                                                     │
├─────────────────────────────────────────────────────┤
│  REQUIEREN MAYOR REPUTACIÓN                         │
│                                                     │
│  [Icono oscuro] Nombre del ítem   🔒 Guardián       │
│  [Icono oscuro] Nombre del ítem   🔒 Leyenda        │
└─────────────────────────────────────────────────────┘
```

### Lógica

- Leer todos los ítems de `guild_vendor_active` WHERE `SeasonId` = actual
- Separar en dos grupos:
  - **Disponibles**: `TierRequired <= tierActualDelJugador`
  - **Bloqueados**: `TierRequired > tierActualDelJugador` (mostrar con candado y nombre del tramo requerido)
- El precio viene de `guild_vendor_active.Price`. Si es `0`, usar el precio base del ítem
- La compra se maneja enteramente en Lua: verificar oro del jugador, descontar, entregar ítem

### Consultas necesarias

1. `guild_vendor_active` WHERE `SeasonId` = actual
2. `guild_season_rep_tiers` — para mostrar el nombre del tramo requerido por ítem
3. `guild_member` — tier actual del jugador

---

## Datos del jugador necesarios en cada panel

| Dato | Cómo obtenerlo |
|---|---|
| GuildId | `UnitFactionGroup("player")` o desde Lua nativo |
| GUID del jugador | API de AIO |
| GuildReputation | `SELECT GuildReputation FROM guild_member WHERE guid = X AND guildid = Y` |
| SeasonContribution | `SELECT SeasonContribution FROM guild_member WHERE guid = X AND guildid = Y` |
| TierActual | Calcular desde GuildReputation contra guild_season_rep_tiers |
| SeasonId actual | `SELECT SeasonId FROM guild_season_state LIMIT 1` |

---

## Notas importantes

- Un jugador **sin hermandad** no debe poder abrir ningún panel. Verificar `GuildId != 0` antes de mostrar cualquier UI.
- La reputación personal es **permanente** — no se resetea entre temporadas.
- Los puntos de temporada (`SeasonPoints` en `guild`) se **resetean** al inicio de cada temporada.
- Las misiones **individuales** (`RequiredGuildMembers = 0`) pueden ser completadas por múltiples jugadores — la hermandad solo recibe PTH de la primera entrega, pero todos reciben rep.
- Las misiones **grupales/raid** (`RequiredGuildMembers > 0`) solo aportan PTH una vez — verificar `guild_season_quest_claims` para mostrar el estado correcto.
