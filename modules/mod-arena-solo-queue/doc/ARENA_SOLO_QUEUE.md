# mod-arena-solo-queue

Sistema de cola individual y grupos cross-team para arenas clasificatorias en AzerothCore WotLK 3.3.5a.

---

## Problema que resuelve

El cliente WoW 3.3.5a impide anotar arenas rated a menos que:
1. El jugador esté en un grupo.
2. Todos los miembros del grupo pertenezcan al **mismo** equipo de arena.

Este módulo elimina ambas restricciones desde el servidor, sin modificar ninguna lógica de emparejamiento o ratings existente.

---

## Funcionalidades

### 1. Cola individual (solo queue)

Un jugador solo, sin grupo, puede anotar arenas clasificatorias. El módulo mantiene una lista de espera interna y forma parejas cuando hay jugadores compatibles disponibles. Una vez formado el par, entra a la cola rated normal del servidor y el matchmaker estándar busca rivales.

**Requisito:** el jugador debe tener un equipo de arena registrado para el bracket solicitado.

### 2. Grupos cross-team

Dos o más jugadores de **distintos** equipos de arena pueden formar un grupo manualmente y anotar juntos como equipo. Cada miembro solo necesita tener su propio equipo registrado para el bracket.

### 3. Actualización de ratings completa

Cuando un lado de la arena está formado por jugadores de distintos equipos, el sistema estándar solo actualiza el equipo del líder (el que figura en `GroupQueueInfo`). Este módulo detecta los equipos secundarios y les aplica `WonAgainst`/`LostAgainst` y `MemberWon`/`MemberLost` con el MMR propio de cada equipo, de forma idéntica a una partida normal.

---

## Arquitectura

### Hooks añadidos al core

| Archivo | Hook | Propósito |
|---------|------|-----------|
| `BattleGroundHandler.cpp:710` | `PlayerScript::OnArenaQueueHandleSoloRated` | Intercepta `isRated=1, asGroup=0` antes del early-return |
| `Group.cpp:2188` | `GroupScript::OnArenaQueueAllowCrossTeamGroup` | Bypass del check de same-team en `CanJoinBattlegroundQueue` |

Ambos hooks son no-ops por defecto: sin el módulo el comportamiento original se preserva 100%.

### Scripts del módulo

| Clase | Base | Responsabilidad |
|-------|------|----------------|
| `ArenaSoloQueueWorld` | `WorldScript` | Ejecuta pairing + limpieza de grupos temporales cada 5 s |
| `ArenaSoloQueuePlayer` | `PlayerScript` | Intercepta `OnArenaQueueHandleSoloRated`; limpia al logout |
| `ArenaSoloQueueGroup` | `GroupScript` | Implementa `OnArenaQueueAllowCrossTeamGroup` |
| `ArenaSoloQueueCommand` | `CommandScript` | Comando `.arena2v2` (toggle join/leave) |
| `ArenaSoloQueueBG` | `AllBattlegroundScript` | `OnAddGroup`: detecta miembros cross-team y registra secundarios |
| `ArenaSoloQueueArena` | `ArenaScript` | `OnBeforeArenaTeamMemberUpdate`: actualiza ratings de equipos secundarios |

### Gestor central: `ArenaSoloQueueMgr`

Singleton que centraliza toda la lógica:

- **`_pending[3]`** — listas de espera por slot (0=2v2, 1=3v3, 2=5v5).
- **`_secondaryMap`** — mapea el jugador líder a sus compañeros de lado cuyos equipos no están en el `GroupQueueInfo`.
- **`_tempGroups`** — GUIDs de grupos WoW temporales creados para el pairing, para disbandearlos si se declina la invitación.

---

## Flujo completo — solo queue 2v2

```
Jugador A (.arena2v2)  →  _pending[0]: [A]
Jugador B (.arena2v2)  →  _pending[0]: [A, B]

WorldScript tick (5s):
  _TryPairSlot(slot=0, arenaType=2)
    _BuildSide([A,B], 2)  →  side=[A,B]  (mismo team juntos, distintos teams FIFO)
    _QueueSide([A,B]):
      tempGroup = new Group(A) + AddMember(B)
      bgQueue.AddGroup(A, tempGroup, rated=true, MMR=avg(A,B), team=TeamA)
        └── OnAddGroup hook → RegisterSecondaries(A, group, TeamA_ID, 2v2)
                             → _secondaryMap[A] = [{B, TeamB_ID}]
      TrackTempGroup(tempGroup.GUID)
      SendStatusPacket(A, B, STATUS_WAIT_QUEUE)

Matchmaker encuentra rival (Team C, grupo normal):
  Arena creada → InviteGroupToBG(A+B, ALLIANCE), InviteGroupToBG(C, HORDE)

Arena termina (A+B ganan):
  TeamA->WonAgainst()  →  stats Team A actualizados
  TeamA->MemberWon(A)  →  OnBeforeArenaTeamMemberUpdate(TeamA, A, won=true, ...)
    └── GetAndClearSecondaries(A) = [{B, TeamB_ID}]
        TeamB->WonAgainst(MMR_B, opponentMMR, ...)  →  stats Team B actualizados
        TeamB->MemberWon(B, opponentMMR, mmrChange)  →  personalRating B actualizado
        TeamB->SaveToDB()
  TeamC->LostAgainst()  →  stats Team C actualizados
  TeamC->MemberLost(C)  →  personalRating C actualizado
```

---

## Algoritmo de pairing: `_BuildSide`

Dado un conjunto de solos ordenados por tiempo de entrada (FIFO), selecciona `arenaType` jugadores para formar un lado con las siguientes reglas de prioridad:

1. **Mismos compañeros de equipo primero**: si hay `arenaType` jugadores del mismo equipo de arena, se emparejan entre sí. Previene que dos miembros del mismo equipo acaben en lados opuestos (lo que haría que el matchmaker los rechazara para siempre).
2. **FIFO con un equipo por posición**: si no hay grupo del mismo equipo suficiente, toma por orden de llegada pero garantizando que cada equipo aparece como máximo una vez en el lado.
3. **FIFO puro**: fallback si ninguna regla anterior completa el lado.

---

## Gestión de grupos temporales

Para que dos solos compartan un `GroupQueueInfo` en la BG queue, el módulo crea un grupo WoW real. Este grupo:

- Es visible en el UI del cliente durante la espera (ambos jugadores se ven como pareja).
- Se disuelve automáticamente cuando los jugadores entran a la arena (el sistema de arena llama `RemoveFromGroup()` en todos los miembros).
- Si el jugador **declina la invitación**, el módulo detecta en el siguiente tick de `CleanupTempGroups()` que ningún miembro está en cola y disbanda el grupo (máximo 5 s de delay).

---

## Instalación

### 1. SQL (base de datos `acore_auth`)

Ejecutar antes de iniciar el servidor:

```
modules/mod-arena-solo-queue/data/sql/db-auth/rbac_arena_solo_queue.sql
```

Registra el permiso RBAC 1000 (`Command: arena2v2`) y lo asigna al rol de jugador (secId 0) para que todos los jugadores puedan usar `.arena2v2`.

### 2. Compilación

El módulo se detecta automáticamente por CMake al estar en `modules/mod-arena-solo-queue/`. No requiere ninguna configuración adicional.

---

## Comando `.arena2v2`

Toggle de cola individual para 2v2 clasificatoria. Accesible para todos los jugadores.

| Estado | Acción |
|--------|--------|
| No en cola | Entra a la cola solo — valida team registrado, temporada activa |
| Ya en cola | Sale de la cola |

**Nota:** este comando es el entry point actual mientras se resuelve la limitación del cliente nativo. Una vez que el cliente envíe `isRated=1, asGroup=0` directamente desde el battlemaster, el comando queda como fallback opcional.

---

## Limitaciones conocidas

| Limitación | Estado |
|-----------|--------|
| El cliente nativo no permite marcar "Rated" sin grupo desde el UI del battlemaster | Pendiente — requiere cambio en `PVPFrame.lua` del cliente |
| Solo queue 3v3/5v5 via comando no implementado | El motor lo soporta; falta un `.arena3v3` / `.arena5v5` |

---

## Archivos modificados en el core

```
src/server/game/Handlers/BattleGroundHandler.cpp   — hook en línea 710
src/server/game/Groups/Group.cpp                   — hook en línea 2188
src/server/game/Scripting/ScriptMgr.h              — declaraciones
src/server/game/Scripting/ScriptDefines/PlayerScript.h/cpp   — OnArenaQueueHandleSoloRated
src/server/game/Scripting/ScriptDefines/GroupScript.h/cpp    — OnArenaQueueAllowCrossTeamGroup
```
