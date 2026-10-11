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
 * Rating update strategy:
 *   When solos from different teams are paired on the same side, only the lead
 *   player's team (slot 0) is tracked in the GroupQueueInfo that the core uses.
 *   The module stores a secondary-player map and uses ArenaScript::OnBeforeArenaTeamMemberUpdate
 *   to trigger WonAgainst/LostAgainst + MemberWon/MemberLost on every secondary
 *   team so ALL teams on a paired side get correct rating and stat updates.
 */

#include "ArenaScript.h"
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

// Maps the lead player of a paired side to any secondary players on that side.
// Used to update secondary teams (those not tracked in GroupQueueInfo) at match end.
struct SecondaryEntry
{
    ObjectGuid playerGuid;
    uint32     arenaTeamId;
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
        // Also remove any secondary mappings involving this player
        _secondaryMap.erase(guid);
        for (auto& [primary, secondaries] : _secondaryMap)
            secondaries.erase(std::remove_if(secondaries.begin(), secondaries.end(),
                [guid](SecondaryEntry const& e) { return e.playerGuid == guid; }), secondaries.end());
    }

    // Called from ArenaScript at match end to retrieve and clear secondaries for a primary player.
    std::vector<SecondaryEntry> GetAndClearSecondaries(ObjectGuid primaryGuid)
    {
        auto it = _secondaryMap.find(primaryGuid);
        if (it == _secondaryMap.end())
            return {};
        auto result = std::move(it->second);
        _secondaryMap.erase(it);
        return result;
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
    // primary player GUID → secondary players that were paired on the same side
    std::map<ObjectGuid, std::vector<SecondaryEntry>> _secondaryMap;

    // Returns arenaType entries that should form one side.
    // Prefers grouping same-team members together so they are never placed on
    // opposite sides of a match.
    static std::vector<SoloPendingEntry*> _BuildSide(
        std::vector<SoloPendingEntry*> const& sorted, uint8 arenaType)
    {
        // Fast path: look for arenaType entries that all share the same ArenaTeamId
        // (e.g. two members of the same 2v2 team both queued solo).
        std::map<uint32, std::vector<SoloPendingEntry*>> byTeam;
        for (auto* e : sorted)
            byTeam[e->arenaTeamId].push_back(e);

        for (auto& [teamId, members] : byTeam)
        {
            if (members.size() >= static_cast<size_t>(arenaType))
                return std::vector<SoloPendingEntry*>(members.begin(), members.begin() + arenaType);
        }

        // No same-team group large enough: FIFO with the constraint that each
        // team appears at most once per side (avoid partial-teammate splits).
        std::vector<SoloPendingEntry*> side;
        std::set<uint32> usedTeams;
        for (auto* e : sorted)
        {
            if (usedTeams.count(e->arenaTeamId))
                continue; // skip — this team is already represented on the side
            side.push_back(e);
            usedTeams.insert(e->arenaTeamId);
            if (side.size() == static_cast<size_t>(arenaType))
                break;
        }

        // If we couldn't fill the side with distinct teams, fall back to plain FIFO
        if (side.size() < static_cast<size_t>(arenaType))
            return std::vector<SoloPendingEntry*>(sorted.begin(), sorted.begin() + arenaType);

        return side;
    }

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

            // Build one side. Same-team members must always end up on the SAME side:
            // if we let FIFO split teammates onto opposite sides the BG matchmaker
            // would either pair them against each other (ArenaTeamId == ArenaTeamId
            // check fails → stuck forever) or put team-mates on opposite sides of
            // an arena match, which is wrong gameplay-wise.
            //
            // Priority rule: if any two entries share an ArenaTeamId, pair them
            // together first (they play as a team). Otherwise fall back to FIFO.
            std::vector<SoloPendingEntry*> side = _BuildSide(entries, arenaType);

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

        // Register secondary players so the ArenaScript hook can update their teams at match end.
        // side[0] is the lead player (their team is in GroupQueueInfo); side[1..] are secondaries.
        for (size_t i = 1; i < side.size(); ++i)
            _secondaryMap[side[0]->playerGuid].push_back({ side[i]->playerGuid, side[i]->arenaTeamId });

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
// ArenaScript — update secondary team ratings at match end
// ---------------------------------------------------------------------------

class ArenaSoloQueueArena : public ArenaScript
{
public:
    // Enable only the one hook we need; not specifying hooks would enable all
    // ArenaScript hooks which share a default of `return false` (block update).
    ArenaSoloQueueArena() : ArenaScript("ArenaSoloQueueArena", { ARENAHOOK_ON_BEFORE_TEAM_MEMBER_UPDATE }) {}

    // Called inside ArenaTeam::MemberWon / MemberLost for every team member.
    // Returning true  → allow the primary team's update to proceed.
    // Returning false → block the update (we never want that here).
    bool OnBeforeArenaTeamMemberUpdate(ArenaTeam* /*team*/, Player* player, bool won,
        uint32 opponentMatchmakerRating, int32 matchmakerChange) override
    {
        // Guard against re-entrancy when we call MemberWon/MemberLost below.
        if (_handlingSecondary)
            return true;

        auto secondaries = sArenaSoloQueueMgr.GetAndClearSecondaries(player->GetGUID());
        if (secondaries.empty())
            return true;

        Map const* bgMap = player->FindMap();

        for (auto const& sec : secondaries)
        {
            Player* secPlayer = ObjectAccessor::FindConnectedPlayer(sec.playerGuid);
            if (!secPlayer)
                continue;

            ArenaTeam* secTeam = sArenaTeamMgr->GetArenaTeamById(sec.arenaTeamId);
            if (!secTeam)
                continue;

            // Use the secondary player's own MMR for proper rating-change computation.
            ArenaTeamMember* secMember = secTeam->GetMember(secPlayer->GetGUID());
            uint32 secOwnMMR = secMember ? secMember->MatchMakerRating : secTeam->GetRating();

            _handlingSecondary = true;

            int32 secRatingChange = 0;
            int32 secMMRChange;
            if (won)
            {
                secMMRChange = secTeam->WonAgainst(secOwnMMR, opponentMatchmakerRating, secRatingChange, bgMap);
                secTeam->MemberWon(secPlayer, opponentMatchmakerRating, secMMRChange);
            }
            else
            {
                secMMRChange = secTeam->LostAgainst(secOwnMMR, opponentMatchmakerRating, secRatingChange, bgMap);
                secTeam->MemberLost(secPlayer, opponentMatchmakerRating, secMMRChange);
            }

            _handlingSecondary = false;

            // Persist secondary team stats — the core only saves the primary team.
            secTeam->SaveToDB();

            LOG_DEBUG("bg.battleground",
                "ArenaSoloQueue: updated secondary team {} ({}) for player {} — won={} ratingChange={}",
                secTeam->GetName(), sec.arenaTeamId, secPlayer->GetName(), won, secRatingChange);
        }

        return true;
    }

private:
    bool _handlingSecondary = false;
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
    new ArenaSoloQueueArena();
}
