/*
 * mod-arena-solo-queue
 *
 * Allows players to queue for rated arenas without being in a pre-formed group:
 *   - Solo queue: player joins alone; the module pairs arenaType solos into one
 *     side and adds that side to the standard rated BG queue; the matchmaker then
 *     finds the opposing team normally (another solo-paired group or a regular group).
 *   - Cross-team group: two players from different arena teams can form a group
 *     and queue together; each member only needs their own team for the bracket.
 *
 * Core hooks used:
 *   PlayerScript::OnArenaQueueHandleSoloRated  (BattleGroundHandler.cpp)
 *   GroupScript::OnArenaQueueAllowCrossTeamGroup (Group.cpp)
 *
 * Player command: .arena2v2  (join/leave toggle)
 *
 * Phase-1 limitation: only the lead team's rating updates after the match;
 * personal rating and MMR for all players update correctly.
 */

#include "ArenaSeasonMgr.h"
#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "Chat.h"
#include "CommandScript.h"
#include "GameTime.h"
#include "Group.h"
#include "GroupMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "World.h"

#include <algorithm>
#include <list>
#include <map>
#include <set>
#include <vector>

using namespace Acore::ChatCommands;

// RBAC permission id for the .arena2v2 command.
// Must match the SQL inserted in data/sql/db-auth/rbac_arena_solo_queue.sql.
static constexpr uint32 RBAC_PERM_COMMAND_ARENA_SOLO_QUEUE = 1000;

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

struct SoloPendingEntry
{
    ObjectGuid playerGuid;
    uint8      bracketId;
    uint32     arenaTeamId;
    uint32     arenaRating;
    uint32     matchmakerRating;
    uint32     prevOpponentsTeamId;
    uint32     joinTime;
};

// ---------------------------------------------------------------------------
// Manager
// ---------------------------------------------------------------------------

class ArenaSoloQueueMgr
{
public:
    static ArenaSoloQueueMgr& Instance()
    {
        static ArenaSoloQueueMgr s_instance;
        return s_instance;
    }

    bool AddSolo(Player* player, uint8 arenaslot)
    {
        if (sArenaSeasonMgr->GetSeasonState() == ARENA_SEASON_STATE_DISABLED)
        {
            ChatHandler(player->GetSession()).PSendSysMessage("Rated arena season is not active.");
            return false;
        }

        uint32 teamId = player->GetArenaTeamId(arenaslot);
        ArenaTeam* team = sArenaTeamMgr->GetArenaTeamById(teamId);
        if (!team || !team->IsMember(player->GetGUID()))
        {
            ChatHandler(player->GetSession()).PSendSysMessage("You must have a registered arena team for this bracket.");
            return false;
        }

        if (player->InBattleground() || player->InBattlegroundQueue())
        {
            ChatHandler(player->GetSession()).PSendSysMessage("You are already in a battleground queue.");
            return false;
        }

        if (IsPlayerPending(player->GetGUID(), arenaslot))
        {
            ChatHandler(player->GetSession()).PSendSysMessage("You are already in the solo arena queue for this bracket.");
            return false;
        }

        Battleground* bgt = sBattlegroundMgr->GetBattlegroundTemplate(BATTLEGROUND_AA);
        if (!bgt)
            return false;

        PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bgt->GetMapId(), player->GetLevel());
        if (!bracketEntry)
            return false;

        ArenaTeamMember* memberInfo = team->GetMember(player->GetGUID());
        uint32 mmr = memberInfo ? memberInfo->MatchMakerRating : team->GetRating();

        SoloPendingEntry entry;
        entry.playerGuid          = player->GetGUID();
        entry.bracketId           = bracketEntry->GetBracketId();
        entry.arenaTeamId         = teamId;
        entry.arenaRating         = std::max<uint32>(1, team->GetRating());
        entry.matchmakerRating    = mmr;
        entry.prevOpponentsTeamId = team->GetPreviousOpponents();
        entry.joinTime            = GameTime::GetGameTimeMS().count();

        _pending[arenaslot].push_back(entry);

        ChatHandler(player->GetSession()).PSendSysMessage("Entered solo rated arena queue. Searching for a partner...");
        return true;
    }

    void RemovePlayer(ObjectGuid guid)
    {
        for (uint8 slot = 0; slot < SOLO_QUEUE_SLOT_COUNT; ++slot)
        {
            auto& list = _pending[slot];
            list.erase(
                std::remove_if(list.begin(), list.end(),
                    [guid](SoloPendingEntry const& e) { return e.playerGuid == guid; }),
                list.end());
        }
    }

    bool IsPlayerPending(ObjectGuid guid, uint8 slot) const
    {
        for (auto const& e : _pending[slot])
            if (e.playerGuid == guid)
                return true;
        return false;
    }

    void Update()
    {
        static constexpr uint8 arenaTypes[SOLO_QUEUE_SLOT_COUNT] = {
            ARENA_TYPE_2v2, ARENA_TYPE_3v3, ARENA_TYPE_5v5
        };

        for (uint8 slot = 0; slot < SOLO_QUEUE_SLOT_COUNT; ++slot)
            _TryPairSlot(slot, arenaTypes[slot]);
    }

private:
    static constexpr uint8 SOLO_QUEUE_SLOT_COUNT = 3;

    std::list<SoloPendingEntry> _pending[SOLO_QUEUE_SLOT_COUNT];

    void _TryPairSlot(uint8 slot, uint8 arenaType)
    {
        auto& list = _pending[slot];

        // Remove stale entries (offline / already queued / already in BG)
        list.erase(
            std::remove_if(list.begin(), list.end(), [](SoloPendingEntry const& e)
            {
                Player* p = ObjectAccessor::FindConnectedPlayer(e.playerGuid);
                return !p || p->InBattleground() || p->InBattlegroundQueue();
            }),
            list.end());

        // Need at least arenaType solos to form ONE side; the matchmaker pairs
        // that side against whatever team is already waiting in the rated queue
        // (another solo pair, a regular group, or a cross-team group).
        if (list.size() < static_cast<size_t>(arenaType))
            return;

        // Group by level bracket
        std::map<uint8, std::vector<SoloPendingEntry*>> byBracket;
        for (auto& e : list)
            byBracket[e.bracketId].push_back(&e);

        for (auto& [bracket, entries] : byBracket)
        {
            if (entries.size() < static_cast<size_t>(arenaType))
                continue;

            // FIFO order
            std::sort(entries.begin(), entries.end(),
                [](SoloPendingEntry const* a, SoloPendingEntry const* b)
                { return a->joinTime < b->joinTime; });

            // Take the first arenaType solos as one side and enter the BG queue.
            // Forming only one side avoids the bug where two members of the same
            // arena team could end up on opposite sides (which the BG matchmaker
            // would then refuse to pair, leaving them stuck forever).
            std::vector<SoloPendingEntry*> side(entries.begin(), entries.begin() + arenaType);

            if (_QueueSide(side, slot, arenaType))
            {
                std::set<ObjectGuid> paired;
                for (auto* e : side) paired.insert(e->playerGuid);

                list.erase(
                    std::remove_if(list.begin(), list.end(),
                        [&paired](SoloPendingEntry const& e) { return paired.count(e.playerGuid) > 0; }),
                    list.end());
            }
        }
    }

    static uint32 _AvgRating(std::vector<SoloPendingEntry*> const& side)
    {
        uint32 total = 0;
        for (auto const* e : side) total += e->arenaRating;
        return side.empty() ? 1 : std::max<uint32>(1, total / static_cast<uint32>(side.size()));
    }

    static uint32 _AvgMMR(std::vector<SoloPendingEntry*> const& side)
    {
        uint32 total = 0;
        for (auto const* e : side) total += e->matchmakerRating;
        return side.empty() ? 0 : total / static_cast<uint32>(side.size());
    }

    bool _QueueSide(std::vector<SoloPendingEntry*> const& side, uint8 arenaslot, uint8 arenaType)
    {
        Player* leader = ObjectAccessor::FindConnectedPlayer(side[0]->playerGuid);
        if (!leader)
            return false;

        Battleground* bgt = sBattlegroundMgr->GetBattlegroundTemplate(BATTLEGROUND_AA);
        if (!bgt)
            return false;

        PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bgt->GetMapId(), leader->GetLevel());
        if (!bracketEntry)
            return false;

        uint32 avgRating = _AvgRating(side);
        uint32 avgMMR    = _AvgMMR(side);
        uint32 teamId    = side[0]->arenaTeamId;
        uint32 prevOpp   = side[0]->prevOpponentsTeamId;

        BattlegroundTypeId      bgTypeId    = bgt->GetBgTypeID();
        BattlegroundQueueTypeId queueTypeId = BattlegroundMgr::BGQueueTypeId(bgTypeId, arenaType);
        BattlegroundQueue&      bgQueue     = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);

        // Build a temporary in-game group when the side has more than one player
        Group* tempGroup = nullptr;
        if (side.size() > 1)
        {
            tempGroup = new Group();
            if (!tempGroup->Create(leader))
            {
                delete tempGroup;
                return false;
            }
            sGroupMgr->AddGroup(tempGroup);

            for (size_t i = 1; i < side.size(); ++i)
            {
                Player* member = ObjectAccessor::FindConnectedPlayer(side[i]->playerGuid);
                if (!member)
                {
                    tempGroup->Disband();
                    return false;
                }
                tempGroup->AddMember(member);
            }
        }

        GroupQueueInfo* ginfo = bgQueue.AddGroup(
            leader, tempGroup, bgTypeId, bracketEntry,
            arenaType, true, false,
            avgRating, avgMMR, teamId, prevOpp);

        uint32 avgWait = bgQueue.GetAverageQueueWaitTime(ginfo);

        for (auto const* e : side)
        {
            Player* member = ObjectAccessor::FindConnectedPlayer(e->playerGuid);
            if (!member)
                continue;

            uint32 queueSlot = member->AddBattlegroundQueueId(queueTypeId);

            WorldPacket data;
            sBattlegroundMgr->BuildBattlegroundStatusPacket(
                &data, bgt, queueSlot, STATUS_WAIT_QUEUE,
                avgWait, 0, arenaType, TEAM_NEUTRAL, true);
            member->SendDirectMessage(&data);

            ChatHandler(member->GetSession()).PSendSysMessage("Partner found! Joined rated 2v2 arena queue.");

            LOG_DEBUG("bg.battleground",
                "ArenaSoloQueue: player {} queued for rated arena slot {} via solo-queue pairing",
                member->GetName(), arenaslot);
        }

        sBattlegroundMgr->ScheduleQueueUpdate(avgMMR, arenaType, queueTypeId, bgTypeId, bracketEntry->GetBracketId());
        return true;
    }
};

#define sArenaSoloQueueMgr ArenaSoloQueueMgr::Instance()

// ---------------------------------------------------------------------------
// WorldScript — periodic pairing check (every 5 s)
// ---------------------------------------------------------------------------

class ArenaSoloQueueWorld : public WorldScript
{
public:
    ArenaSoloQueueWorld() : WorldScript("ArenaSoloQueueWorld"), _timer(0) {}

    void OnUpdate(uint32 diff) override
    {
        _timer += diff;
        if (_timer < 5000)
            return;
        _timer = 0;
        sArenaSoloQueueMgr.Update();
    }

private:
    uint32 _timer;
};

// ---------------------------------------------------------------------------
// PlayerScript — intercept solo rated queue + cleanup on logout
// ---------------------------------------------------------------------------

class ArenaSoloQueuePlayer : public PlayerScript
{
public:
    ArenaSoloQueuePlayer() : PlayerScript("ArenaSoloQueuePlayer") {}

    void OnArenaQueueHandleSoloRated(Player* player, uint8 arenaslot) override
    {
        sArenaSoloQueueMgr.AddSolo(player, arenaslot);
    }

    void OnPlayerLogout(Player* player) override
    {
        sArenaSoloQueueMgr.RemovePlayer(player->GetGUID());
    }
};

// ---------------------------------------------------------------------------
// GroupScript — allow cross-team rated groups
// ---------------------------------------------------------------------------

class ArenaSoloQueueGroup : public GroupScript
{
public:
    ArenaSoloQueueGroup() : GroupScript("ArenaSoloQueueGroup") {}

    bool OnArenaQueueAllowCrossTeamGroup(Group const* /*group*/, Player* member, uint32 arenaSlot) override
    {
        uint32 teamId = member->GetArenaTeamId(arenaSlot);
        ArenaTeam* team = sArenaTeamMgr->GetArenaTeamById(teamId);
        return team && team->IsMember(member->GetGUID());
    }
};

// ---------------------------------------------------------------------------
// CommandScript — .arena2v2 (join/leave toggle, available to all players)
// ---------------------------------------------------------------------------

class ArenaSoloQueueCommand : public CommandScript
{
public:
    ArenaSoloQueueCommand() : CommandScript("ArenaSoloQueueCommand") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "arena2v2", HandleArena2v2Command, static_cast<rbac::RBACPermissions>(RBAC_PERM_COMMAND_ARENA_SOLO_QUEUE), Console::No },
        };
        return commandTable;
    }

    static bool HandleArena2v2Command(ChatHandler* handler, Optional<std::string_view> /*args*/)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        // Toggle: leave if already in the solo pending queue
        if (sArenaSoloQueueMgr.IsPlayerPending(player->GetGUID(), 0))
        {
            sArenaSoloQueueMgr.RemovePlayer(player->GetGUID());
            handler->PSendSysMessage("Left the solo rated 2v2 arena queue.");
            return true;
        }

        sArenaSoloQueueMgr.AddSolo(player, 0 /* slot 0 = 2v2 */);
        return true;
    }
};

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void AddArenaSoloQueueScripts()
{
    new ArenaSoloQueueWorld();
    new ArenaSoloQueuePlayer();
    new ArenaSoloQueueGroup();
    new ArenaSoloQueueCommand();
}
