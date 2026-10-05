# Misiones de Hermandad en AzerothCore

> Documento de diseño e implementación. **Rama: `feat/guild-quest-type`**

---

## 1. Resumen

Sistema de misiones de hermandad que reutiliza el sistema de misiones existente
sin modificar su comportamiento nativo. La detección se hace via `ZoneOrSort` y
la cantidad mínima de miembros de hermandad requeridos se configura por misión
en `quest_template_addon`.

---

## 2. Diseño

### Principio fundamental

`QuestInfoID` es **100% cosmético** en el core original (salvo `IsRaidQuest()`
e `IsPVPQuest()` que son nativos e intactos). Nosotros **no lo usamos** para
ninguna validación. La detección de misión de hermandad se hace por `ZoneOrSort`.

### Campo nuevo: `RequiredGuildMembers`

Columna añadida en `quest_template_addon`:

```sql
RequiredGuildMembers tinyint unsigned NOT NULL DEFAULT 0
```

| Valor | Comportamiento |
|---|---|
| `0` | Misión individual — solo chequea que el jugador tenga hermandad |
| `N > 0` | Requiere N miembros de la misma hermandad en el grupo/raid |

### Detección de misión de hermandad

`ZoneOrSort = -400` (`QUEST_SORT_GUILD = 400` en `SharedDefines.h`).

### Lógica en `CanCompleteQuest()`

```cpp
if (qInfo->IsGuildQuest())           // ZoneOrSort == -400
{
    if (!GetGuildId())
        return false;

    if (qInfo->GetRequiredGuildMembers() > 0)
    {
        if (!GetGroup())
            return false;
        if (!HasGuildMembersInGroup(qInfo->GetRequiredGuildMembers()))
            return false;
    }
}
```

### Reglas de conteo de miembros

- Solo jugadores **online** (offline se ignoran automáticamente).
- Jugadores **muertos** cuentan.
- El **propio jugador** cuenta como miembro.
- El check ocurre en `CanCompleteQuest()` únicamente. Una vez completada,
  el jugador entrega libremente aunque haya salido del grupo.

### Comportamiento nativo conservado

`IsAllowedInRaid()` / `IsRaidQuest()` no se tocaron. Si el jugador está en
raid y la misión no tiene `QuestInfoID` 62/88/89, los objetivos no avanzan
— comportamiento nativo intacto. El diseñador de la misión debe usar
`QuestInfoID = 62/88/89` si quiere que sea completable en raid.

---

## 3. Configuración de una misión de hermandad en DB

### `quest_template`

| Campo | Valor | Notas |
|---|---|---|
| `ZoneOrSort` | `-400` | Obligatorio — identifica la misión como de hermandad |
| `QuestInfoID` | cualquiera | 100% cosmético. Usar 62/88/89 si se quiere completar en raid |
| `Flags` | `0x1000` | Opcional — añade flag diaria |

### `quest_template_addon`

| Campo | Valor | Notas |
|---|---|---|
| `RequiredGuildMembers` | `0` | Misión individual de hermandad |
| `RequiredGuildMembers` | `N > 0` | Requiere N miembros en grupo/raid |

### Ejemplos

```sql
-- Misión individual de hermandad diaria
INSERT INTO quest_template (ID, QuestInfoID, QuestType, QuestLevel, Title, Flags, ZoneOrSort)
VALUES (50001, 0, 2, 80, 'Tarea de Hermandad', 0x1000, -400);

-- Misión de hermandad en grupo (mínimo 3 miembros)
INSERT INTO quest_template (ID, QuestInfoID, QuestType, QuestLevel, Title, ZoneOrSort)
VALUES (50002, 1, 2, 80, 'Misión de Hermandad [Grupo]', -400);

INSERT INTO quest_template_addon (ID, RequiredGuildMembers)
VALUES (50002, 3);

-- Misión de hermandad en raid (mínimo 10 miembros, objetivos cuentan en raid)
INSERT INTO quest_template (ID, QuestInfoID, QuestType, QuestLevel, Title, ZoneOrSort)
VALUES (50003, 62, 2, 80, 'Misión de Hermandad [Banda]', -400);

INSERT INTO quest_template_addon (ID, RequiredGuildMembers)
VALUES (50003, 10);
```

---

## 4. Cliente

### `QuestSort.dbc`
Añadir fila `ID = 400`, nombre = `Hermandad`. Todas las misiones con
`ZoneOrSort = -400` aparecerán agrupadas bajo esta categoría en el log.

### `QuestInfo.dbc`
No requiere modificación — `QuestInfoID` es cosmético y usa valores nativos.
Opcional: corregir la traducción esES del ID `1` de "Elite" a "Grupo".

---

## 5. Archivos modificados

| Archivo | Cambio |
|---|---|
| `src/server/shared/SharedDefines.h` | `QUEST_SORT_GUILD = 400` en `enum QuestSorts` |
| `src/server/game/Quests/QuestDef.h` | Campo `RequiredGuildMembers`, predicado `IsGuildQuest()`, getter |
| `src/server/game/Quests/QuestDef.cpp` | `LoadQuestTemplateAddon()` lee `RequiredGuildMembers` en index 19 |
| `src/server/game/Globals/ObjectMgr.cpp` | Query de `quest_template_addon` incluye `RequiredGuildMembers` |
| `src/server/game/Entities/Player/Player.h` | Declaración de `HasGuildMembersInGroup(uint8)` |
| `src/server/game/Entities/Player/Player.cpp` | Implementación de `HasGuildMembersInGroup()` |
| `src/server/game/Entities/Player/PlayerQuest.cpp` | Check de hermandad en `CanCompleteQuest()` |
| `data/sql/custom/db_world/quest_guild_type.sql` | `ALTER TABLE quest_template_addon ADD COLUMN RequiredGuildMembers` |

---

## 6. Pendientes

- **`SMSG_QUESTUPDATE_FAILED` (0x196)** — el cliente muestra la misión como
  completa visualmente cuando todos los objetivos de ítems/kills están al máximo,
  aunque el servidor la mantenga `INCOMPLETE` por el check de hermandad. Explorar
  si enviar este opcode (actualmente `STATUS_NEVER`) resetea el visual del cliente.
  Candidato a implementar junto con nuevos hooks de módulo.

---

## 7. Lo que NO se modificó

- `IsRaidQuest()` / `IsAllowedInRaid()` — comportamiento nativo intacto.
- `CanRewardQuest()` — sin check de hermandad, entrega libre una vez completada.
- Cualquier lógica de validación por `QuestInfoID` — 100% cosmético.
