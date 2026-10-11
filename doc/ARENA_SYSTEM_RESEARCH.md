# Arena System — Investigación Técnica

> **Rama:** `feat/guild-quest-type` · **Fecha:** 2026-10-11  
> **Objetivo:** Entender el sistema de arenas actual antes de modificar el emparejamiento.

---

## 1. Archivos clave

| Archivo | Propósito |
|---------|-----------|
| `src/server/game/Battlegrounds/BattlegroundQueue.h` | Estructuras de datos: `GroupQueueInfo`, `SelectionPool`, `BattlegroundQueue` |
| `src/server/game/Battlegrounds/BattlegroundQueue.cpp` | Implementación del algoritmo de emparejamiento: `BattlegroundQueueUpdate()`, `AddGroup()` |
| `src/server/game/Battlegrounds/ArenaTeam.h` | Definición de `ArenaTeam`, `ArenaTeamMember`, `ArenaTeamStats` |
| `src/server/game/Battlegrounds/ArenaTeam.cpp` | Fórmulas de rating: `GetMatchmakerRatingMod()`, `GetRatingMod()`, `WonAgainst()`, `LostAgainst()` |
| `src/server/game/Battlegrounds/BattlegroundMgr.h/cpp` | Gestión global de colas: `GetMaxRatingDifference()`, `GetRatingDiscardTimer()`, `ScheduleQueueUpdate()` |
| `src/server/game/Battlegrounds/Arena.cpp` | Lógica específica de arenas y actualización de ratings al finalizar: `EndBattleground()` |
| `src/server/game/Handlers/BattleGroundHandler.cpp` | Handler principal de entrada a cola: `HandleBattlemasterJoinArena()` (línea 698) |
| `src/server/game/Handlers/ArenaTeamHandler.cpp` | Handlers de gestión de equipos: invitaciones, abandono, disbanding |

---

## 2. Tablas SQL

### `arena_team` (characters DB)

| Columna | Tipo | Descripción |
|---------|------|-------------|
| `arenaTeamId` | int unsigned | PK — ID único del equipo |
| `name` | varchar(24) | Nombre del equipo |
| `captainGuid` | int unsigned | GUID del capitán |
| `type` | tinyint | 2 = 2v2, 3 = 3v3, 5 = 5v5 |
| `rating` | smallint unsigned | Rating actual del equipo |
| `seasonGames` / `seasonWins` | smallint | Estadísticas de temporada |
| `weekGames` / `weekWins` | smallint | Estadísticas semanales (para puntos de arena) |
| `rank` | int unsigned | Ranking dentro del tipo |
| `backgroundColor`, `emblemStyle`, etc. | varios | Datos cosméticos del emblema |

### `arena_team_member` (characters DB)

| Columna | Tipo | Descripción |
|---------|------|-------------|
| `arenaTeamId` | int unsigned | FK → `arena_team` |
| `guid` | int unsigned | GUID del jugador (PK compuesta) |
| `weekGames` / `weekWins` | smallint | Partidas de la semana |
| `seasonGames` / `seasonWins` | smallint | Partidas de la temporada |
| `personalRating` | smallint | Rating personal del jugador |

### `character_arena_stats` (characters DB)

| Columna | Tipo | Descripción |
|---------|------|-------------|
| `guid` | int unsigned | GUID del jugador (PK compuesta) |
| `slot` | tinyint | 0=2v2, 1=3v3, 2=5v5 |
| `matchMakerRating` | smallint unsigned | MMR actual |
| `maxMMR` | smallint | MMR máximo alcanzado en la temporada |

---

## 3. Estructuras de datos en memoria

### `GroupQueueInfo` (`BattlegroundQueue.h:34-53`)

Representa a un equipo/grupo en cola. Campos relevantes para el emparejamiento:

```cpp
struct GroupQueueInfo {
    GuidSet       Players;                     // GUIDs de los jugadores del grupo
    TeamId        teamId;                      // Facción asignada al juego (Alliance/Horde)
    TeamId        RealTeamID;                  // Facción real del jugador (cross-faction)
    bool          IsRated;                     // true = arena clasificatoria
    uint8         ArenaType;                   // 2, 3 o 5
    uint32        ArenaTeamId;                 // ID del equipo (si es rated)
    uint32        JoinTime;                    // Timestamp de entrada a la cola (ms)
    uint32        RemoveInviteTime;            // Límite para aceptar la invitación
    uint32        IsInvitedToBGInstanceGUID;   // != 0 si ya fue invitado a una arena
    uint32        ArenaTeamRating;             // Rating del equipo al entrar a cola
    uint32        ArenaMatchmakerRating;       // MMR del equipo al entrar a cola
    uint32        OpponentsTeamRating;         // Rating del rival (se asigna al hacer match)
    uint32        OpponentsMatchmakerRating;   // MMR del rival (se asigna al hacer match)
    uint32        PreviousOpponentsTeamId;     // Para evitar rematches
    uint8         BracketId;                   // Bracket de nivel
    uint8         GroupType;                   // BG_QUEUE_PREMADE_ALLIANCE/HORDE, etc.
};
```

### `BattlegroundQueue::m_QueuedGroups` (`BattlegroundQueue.h:112`)

```cpp
GroupsQueueType m_QueuedGroups[MAX_BATTLEGROUND_BRACKETS][BG_QUEUE_MAX];
//  Índices de GroupType:
//  BG_QUEUE_PREMADE_ALLIANCE (0) — equipos rated y grupos premade
//  BG_QUEUE_PREMADE_HORDE    (1)
//  BG_QUEUE_NORMAL_ALLIANCE  (2) — jugadores solos / no premade
//  BG_QUEUE_NORMAL_HORDE     (3)
//  BG_QUEUE_CFBG             (4) — cross-faction BG
```

### `SelectionPool` (BattlegroundQueue.h:115-127)

Usada para BGs normales (no arenas rated). Contiene una lista acumulada de grupos hasta completar el número de jugadores necesario.

---

## 4. Flujo de entrada a la cola

```
Jugador → battlemaster → CMSG_BATTLEMASTER_JOIN_ARENA
   └─→ HandleBattlemasterJoinArena() [BattleGroundHandler.cpp:698]
         ├─ Valida: no en BG, no en cola, grupo correcto, equipo de arena existe
         ├─ Obtiene ArenaTeam del DB → lee rating y MMR promedio del grupo
         ├─ Crea GroupQueueInfo con snapshot de rating/MMR
         ├─ BattlegroundQueue::AddGroup() → inserta en m_QueuedGroups[bracket][BG_QUEUE_PREMADE_*]
         └─ ScheduleQueueUpdate() → programa BattlegroundQueueUpdate() en el próximo tick
```

---

## 5. Algoritmo de emparejamiento

**Función:** `BattlegroundQueue::BattlegroundQueueUpdate()` (`BattlegroundQueue.cpp:908-1038`)

### Paso 1 — Calcular ventana de rating

Se toma el MMR del equipo más antiguo en cola (el que lleva más tiempo esperando) como centro de la ventana:

```
arenaRating     = MMR del equipo más antiguo sin invitación
arenaMinRating  = max(0, arenaRating - CONFIG_ARENA_MAX_RATING_DIFFERENCE)
arenaMaxRating  = arenaRating + CONFIG_ARENA_MAX_RATING_DIFFERENCE
```

`CONFIG_ARENA_MAX_RATING_DIFFERENCE` por defecto = **5000** (prácticamente sin límite).

### Paso 2 — Buscar equipo 1

Itera `BG_QUEUE_PREMADE_ALLIANCE` y `BG_QUEUE_PREMADE_HORDE`. El primer equipo que cumpla:

- **No está ya invitado** a otra arena.
- Su MMR está dentro de `[arenaMinRating, arenaMaxRating]`, **O** lleva en cola más tiempo que `CONFIG_ARENA_RATING_DISCARD_TIMER` (en ese caso se ignora el rating).

### Paso 3 — Buscar equipo 2

Desde la posición del equipo 1, busca un segundo equipo que cumpla lo anterior y además:

- **No sea el mismo equipo** (`ArenaTeamId` diferente).
- **No sea el rival de la última partida** (`PreviousOpponentsTeamId`), salvo que haya pasado `CONFIG_ARENA_PREV_OPPONENTS_DISCARD_TIMER`.

### Paso 4 — Crear la partida

Si se encontraron 2 equipos:

1. Se registran los ratings del rival en cada `GroupQueueInfo` (`OpponentsTeamRating`, `OpponentsMatchmakerRating`).
2. Se gestiona cross-faction: si un equipo de facción Alliance cayó en la cola Horde, se reasigna.
3. Se crea la instancia de arena con `CreateNewBattleground()`.
4. Se asignan los MMR iniciales a la arena (usados para calcular los cambios al final).
5. Se invitan ambos grupos con `InviteGroupToBG()` → paquete `STATUS_WAIT_JOIN` → **80 segundos** para aceptar.
6. `arena->StartBattleground()`.

### Resumen visual del algoritmo

```
m_QueuedGroups[bracket][PREMADE_ALLIANCE]  →  [ T1, T3, T5, ... ]
m_QueuedGroups[bracket][PREMADE_HORDE]     →  [ T2, T4, T6, ... ]

Tick del servidor:
  1. Tomar el más antiguo sin invitación → establecer ventana MMR ± maxDiff
  2. Buscar equipo1 que encaje (rating o tiempo)
  3. Buscar equipo2 que encaje + no sea rematch
  4. Si found == 2 → crear arena e invitar
```

---

## 6. Sistema de rating (ELO modificado)

### Dos capas de rating

| Tipo | Dónde se guarda | Qué mide |
|------|-----------------|----------|
| **Team Rating** | `arena_team.rating` | Rating compartido del equipo |
| **MMR** (Matchmaker Rating) | `character_arena_stats.matchMakerRating` | Rating individual por bracket, usado para emparejar |

### Probabilidad de victoria (ELO estándar)

```cpp
// ArenaTeam.cpp:724-729
float GetChanceAgainst(uint32 own, uint32 opponent) {
    return 1.0 / (1.0 + exp(ln(10) * (opponent - own) / 650.0));
}
```

### Cambio de MMR

```cpp
// ArenaTeam.cpp:731-755
mod = (won ? 1.0 : 0.0) - GetChanceAgainst(own, opponent);
mod *= CONFIG_ARENA_MATCHMAKER_RATING_MODIFIER;  // ~24 por defecto
```

- **Ganar contra rival más fuerte** → ganancia grande.
- **Ganar contra rival más débil** → ganancia pequeña.
- **Perder contra rival más fuerte** → pérdida pequeña.
- **Perder contra rival más débil** → pérdida grande.

### Cambio de Team Rating

```cpp
// ArenaTeam.cpp:757-785
if (won) {
    if (ownRating < 1000)
        mod = WIN_MOD_1 * (1.0 - chance);       // ~2.4 × (1 − chance)
    else if (ownRating < 1300)
        mod = interpolado entre WIN_MOD_1/2 y WIN_MOD_1  // 1000–1300: zona de transición
    else
        mod = WIN_MOD_2 * (1.0 - chance);       // ~0.2 × (1 − chance)  — rendimiento decreciente
} else {
    mod = LOSE_MOD * (-chance);                 // ~-32 × chance
}
```

Efecto: los equipos con rating bajo (<1300) suben mucho más rápido. A ratings altos la progresión se frena.

### Puntos de arena semanales

```cpp
// ArenaTeam.cpp:664-692
if (rating <= 1500)
    points = 344;
else
    points = 1511.26 / (1.0 + 1639.28 * exp(-0.00412 * rating));  // curva logística

points *= RATE_ARENA_POINTS_2V2;   // 0.76 para 2v2
points *= RATE_ARENA_POINTS;       // multiplicador global
```

Requisitos para recibir puntos: ≥10 partidas en la semana y el jugador participó en ≥30% de los juegos del equipo.

---

## 7. Actualización de ratings al finalizar la arena

**Función:** `Arena::EndBattleground()` (`Arena.cpp:207-381`)

1. **Duración mínima válida:** 15 segundos. Partidas más cortas no modifican ratings.
2. **Empate por timeout:** −16 puntos de team rating a ambos equipos (`ARENA_TIMELIMIT_POINTS_LOSS`).
3. **Victoria/derrota:**
   - `winnerTeam->WonAgainst(winnerMMR, loserMMR, ratingChange, bgMap)` — actualiza rating y MMR del ganador.
   - `loserTeam->LostAgainst(loserMMR, winnerMMR, ratingChange, bgMap)` — actualiza el perdedor.
   - `MemberWon()` / `MemberLost()` — actualiza `personalRating` y `matchMakerRating` de cada jugador.
   - Restricción: el MMR de un jugador no puede superar el `teamRating` del equipo.
4. **Persistencia:** Se guarda en DB solo si la partida fue válida (≥15 s). Se actualizan `arena_team`, `arena_team_member` y `character_arena_stats`.

---

## 8. Opcodes relevantes

| Opcode | Handler | Descripción |
|--------|---------|-------------|
| `CMSG_BATTLEMASTER_JOIN_ARENA` | `HandleBattlemasterJoinArena()` | Unirse a la cola de arena |
| `MSG_BATTLEFIELD_STATUS` | `HandleBattlefieldStatusOpcode()` | Consultar estado de la cola |
| `CMSG_LEAVE_BATTLEFIELD` | `HandleBattlefieldLeaveOpcode()` | Abandonar arena/BG |
| `CMSG_ARENA_TEAM_QUERY` | `HandleArenaTeamQueryOpcode()` | Info del equipo |
| `CMSG_ARENA_TEAM_ROSTER` | `HandleArenaTeamRosterOpcode()` | Lista de miembros |
| `CMSG_ARENA_TEAM_INVITE` | `HandleArenaTeamInviteOpcode()` | Invitar jugador al equipo |
| `CMSG_ARENA_TEAM_ACCEPT` | `HandleArenaTeamAcceptOpcode()` | Aceptar invitación |
| `CMSG_ARENA_TEAM_LEAVE` | `HandleArenaTeamLeaveOpcode()` | Salir del equipo |
| `CMSG_ARENA_TEAM_DISBAND` | `HandleArenaTeamDisbandOpcode()` | Disolver equipo |
| `MSG_INSPECT_ARENA_TEAMS` | `HandleInspectArenaTeamsOpcode()` | Inspeccionar stats de arena de otro jugador |

---

## 9. Constantes de configuración

| Config | Defecto | Efecto |
|--------|---------|--------|
| `CONFIG_ARENA_MAX_RATING_DIFFERENCE` | 5000 | Ventana de MMR para emparejar (±) |
| `CONFIG_ARENA_RATING_DISCARD_TIMER` | variable | Tiempo tras el que se ignora el rating al emparejar |
| `CONFIG_ARENA_PREV_OPPONENTS_DISCARD_TIMER` | ~3600000 ms | Tiempo mínimo entre rematches |
| `CONFIG_ARENA_MATCHMAKER_RATING_MODIFIER` | ~24 | Multiplicador de cambio de MMR |
| `CONFIG_ARENA_WIN_RATING_MODIFIER_1` | ~2.4 | Modificador de rating para equipos <1000 |
| `CONFIG_ARENA_WIN_RATING_MODIFIER_2` | ~0.2 | Modificador de rating para equipos >1300 |
| `CONFIG_ARENA_LOSE_RATING_MODIFIER` | ~32 | Modificador de pérdida de rating |
| `CONFIG_ARENA_START_RATING` | configurable | Rating inicial de equipo nuevo |
| `CONFIG_ARENA_START_PERSONAL_RATING` | 1000–1500 | Rating personal inicial |
| `CONFIG_ARENA_START_MATCHMAKER_RATING` | configurable | MMR inicial |
| `CONFIG_MAX_ALLOWED_MMR_DROP` | configurable | Caída máxima de MMR permitida |
| `ARENA_TIMELIMIT_POINTS_LOSS` | −16 | Penalización por empate (timeout) |

---

## 10. Puntos clave para modificar el emparejamiento

El algoritmo de pairing vive enteramente en:

```
BattlegroundQueue::BattlegroundQueueUpdate()
  → BattlegroundQueue.cpp, líneas 908-1038
```

Los **puntos de extensión naturales** son:

1. **Ventana de rating** (líneas 940-941): Cambiar la fórmula `arenaMinRating`/`arenaMaxRating`.
2. **Condición de matching** (líneas 960-974 y 979-992): El bloque `if` que compara MMR del candidato con la ventana.
3. **Filtro de rematches** (línea 985): `PreviousOpponentsTeamId` check.
4. **Ordenación de la cola**: Actualmente las colas son `std::list` FIFO; cambiar la inserción o el recorrido permite ordenar por MMR, tiempo, etc.
5. **Datos adicionales en `GroupQueueInfo`**: Para un matchmaking basado en criterios extra (puntuación de temporada, wins récientes, etc.) se pueden añadir campos aquí sin cambiar la interfaz pública.

Los **ratings que se usan** para emparejar son los snapshots capturados en `AddGroup()` → `GroupQueueInfo::ArenaMatchmakerRating`. Cambiar qué valor se snapshottea ahí es otra palanca.

---

## Diagrama de flujo general

```
Jugador une cola
       │
       ▼
AddGroup() ──→ m_QueuedGroups[bracket][PREMADE_*]
                        │
                        │  (cada tick del servidor)
                        ▼
          BattlegroundQueueUpdate()
                        │
               ┌────────┴────────┐
               │                 │
          found < 2          found == 2
               │                 │
          próximo tick      CreateNewBattleground()
                                 │
                          InviteGroupToBG() × 2
                                 │
                          StartBattleground()
                                 │
                          ... partida ...
                                 │
                          EndBattleground()
                                 │
                          WonAgainst() / LostAgainst()
                                 │
                          Guardar en DB
```
