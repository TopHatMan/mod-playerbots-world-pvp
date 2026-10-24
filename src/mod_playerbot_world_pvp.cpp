/*
 * mod-playerbot-world-pvp
 * Lightweight autonomous Playerbot world-PvP event simulator.
 *
 * Design goal:
 *   - Pick configured PvP hotspots from WORLD DB.
 *   - Pull online random Playerbot characters by account prefix/range.
 *   - Spawn attackers outside town, move them toward target/defense point.
 *   - Spawn defenders/responders at target/defense point.
 *   - Let normal Playerbot combat/PvP behavior take over for a short chaos window.
 *   - End the event and teleport surviving bots back to where they came from.
 */

#include "ScriptMgr.h"
#include "WorldScript.h"
#include "PlayerScript.h"
#include "World.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "ObjectAccessor.h"
#include "WorldSession.h"
#include "SharedDefines.h"
#include "Random.h"
#include "Log.h"
#include "Chat.h"
#include "GameTime.h"
#include "MotionMaster.h"
#include "Group.h"
#include "Map.h"
#include "RandomPlayerbotMgr.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

// Canonical reservation bus is BotActivityMgr in mod-playerbots.
// This module is disabled; the include is a compatibility shim.
#include "BotActivityRegistry.h"

namespace PlayerbotWorldPvp
{
    enum TeamSide : uint8
    {
        TEAM_SIDE_ALLIANCE = 0,
        TEAM_SIDE_HORDE = 1,
        TEAM_SIDE_ANY = 2
    };

    struct Hotspot
    {
        uint32 Id = 0;
        std::string Name;
        bool Enabled = true;
        TeamSide AttackerTeam = TEAM_SIDE_HORDE;
        TeamSide DefenderTeam = TEAM_SIDE_ALLIANCE;
        uint8 MinLevel = 10;
        uint8 MaxLevel = 60;
        uint32 MapId = 0;
        float RallyX = 0.0f;
        float RallyY = 0.0f;
        float RallyZ = 0.0f;
        float RallyO = 0.0f;
        float TargetX = 0.0f;
        float TargetY = 0.0f;
        float TargetZ = 0.0f;
        float TargetO = 0.0f;
        uint32 AttackersMin = 3;
        uint32 AttackersMax = 8;
        uint32 DefendersMin = 2;
        uint32 DefendersMax = 8;
        uint32 DurationMin = 5;
        uint32 DurationMax = 12;
        uint32 Weight = 100;
        uint32 CooldownSeconds = 1800;
        uint32 LastStart = 0;
    };

    struct BotCandidate
    {
        uint32 GuidLow = 0;
        uint32 AccountId = 0;
        std::string Name;
        uint8 Level = 1;
        uint8 Race = 0;
        uint8 Class = 0;
        TeamSide Team = TEAM_SIDE_ANY;
        bool Online = false;
    };

    struct Participant
    {
        BotCandidate Bot;
        bool Attacker = false;
        uint32 OriginalMap = 0;
        float OriginalX = 0.0f;
        float OriginalY = 0.0f;
        float OriginalZ = 0.0f;
        float OriginalO = 0.0f;
    };

    struct ActiveEvent
    {
        uint64 EventId = 0;
        Hotspot Spot;
        uint32 StartedAt = 0;
        uint32 EndsAt = 0;
        std::vector<Participant> Members;
    };

    struct MoveStep
    {
        uint64 EventId = 0;
        uint32 GuidLow = 0;
        std::string Name;
        uint32 MapId = 0;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float O = 0.0f;
        uint32 ExecuteAt = 0;
        bool UseTeleport = false;
    };

    struct DuelStep
    {
        uint64 EventId = 0;
        uint32 ChallengerGuidLow = 0;
        uint32 TargetGuidLow = 0;
        std::string ChallengerName;
        std::string TargetName;
        uint32 ExecuteAt = 0;
        uint8 RetryCount = 0;
    };

    static bool Enable = true;
    static bool Debug = true;
    static uint32 TickSeconds = 60;
    static uint32 StartupDelaySeconds = 120;
    static uint32 _timerMs = 0;
    static uint32 _startupElapsedMs = 0;
    static bool _startupDelayLogged = false;

    static bool BotOnlyEligibilityFilter = true;
    static bool UsePlayerbotConfig = true;
    static std::string BotAccountPrefix = "auto";
    static uint32 BotAccountMin = 0;
    static uint32 BotAccountMax = 0;

    static bool TeleportBots = true;
    static bool ReturnBotsAfterEvent = true;
    static bool UseMovePointAfterRally = true;
    static bool HardTeleportToTarget = false;
    static uint32 MoveFromRallyDelaySeconds = 15;
    static uint32 EventChancePerTick = 25;
    static uint32 MaxActiveEvents = 2;
    static uint32 MaxBotsPerSide = 12;
    static uint32 PositionJitter = 8;
    static uint32 BotQueryLimit = 400;
    static uint32 MinOnlineBotsRequired = 3;
    static uint32 GlobalMinLevel = 20;

    static bool ReactiveRaidResponse = true;
    static uint32 ReactiveMinGroupSize = 20;
    static uint32 ReactiveMaxResponders = 40;
    static uint32 ReactiveLoginDelaySeconds = 20;
    static uint32 ReactiveCooldownSeconds = 1800;
    static uint32 ReactiveDurationMinutes = 20;
    static std::string PvpCombatStrategies = "+pvp,+boost,+dps debuff,-passive,-stay";
    static std::string PvpNonCombatStrategies = "+pvp,+duel,+boost,-passive,-stay";
    static uint32 _lastReactiveResponse = 0;

    struct PendingRaidResponse
    {
        uint32 TriggerGuidLow = 0;
        TeamSide ResponseTeam = TEAM_SIDE_ANY;
        uint32 MapId = 0;
        float TargetX = 0.0f;
        float TargetY = 0.0f;
        float TargetZ = 0.0f;
        float TargetO = 0.0f;
        uint32 StartAt = 0;
        std::vector<BotCandidate> Bots;
    };

    static std::vector<PendingRaidResponse> _pendingRaidResponses;

    static bool SameFactionDuels = true;
    static uint32 DuelRequestDelaySeconds = 8;
    static uint32 DuelSpellId = 7266;
    static uint32 DuelPairLimit = 12;

    static bool FileLog = true;
    static std::string FileLogPath = "Logs/playerbot_world_pvp.log";

    static uint64 _nextEventId = 1;
    static std::vector<ActiveEvent> _activeEvents;
    static std::vector<MoveStep> _moveQueue;
    static std::vector<DuelStep> _duelQueue;

    static std::string ToLowerCopy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    static bool IsAutoValue(std::string const& value)
    {
        std::string v = ToLowerCopy(value);
        return v.empty() || v == "auto" || v == "playerbots" || v == "playerbot";
    }

    static std::string TeamName(TeamSide team)
    {
        switch (team)
        {
        case TEAM_SIDE_ALLIANCE: return "Alliance";
        case TEAM_SIDE_HORDE: return "Horde";
        default: return "Any";
        }
    }

    static bool ParseTeam(std::string token, TeamSide& out)
    {
        token = ToLowerCopy(token);
        if (token == "a" || token == "alliance" || token == "ally")
        {
            out = TEAM_SIDE_ALLIANCE;
            return true;
        }
        if (token == "h" || token == "horde")
        {
            out = TEAM_SIDE_HORDE;
            return true;
        }
        if (token == "any" || token == "both")
        {
            out = TEAM_SIDE_ANY;
            return true;
        }
        return false;
    }

    static TeamSide TeamForRace(uint8 race)
    {
        switch (race)
        {
        case RACE_HUMAN:
        case RACE_DWARF:
        case RACE_NIGHTELF:
        case RACE_GNOME:
        case RACE_DRAENEI:
            return TEAM_SIDE_ALLIANCE;
        case RACE_ORC:
        case RACE_UNDEAD_PLAYER:
        case RACE_TAUREN:
        case RACE_TROLL:
        case RACE_BLOODELF:
            return TEAM_SIDE_HORDE;
        default:
            return TEAM_SIDE_ANY;
        }
    }

    static void StatusLog(std::string const& text)
    {
        LOG_INFO("module", "[PlayerbotWorldPvp] {}", text);
    }

    static void DebugLog(std::string const& text)
    {
        if (Debug)
            StatusLog(text);
    }

    static void AuditLog(std::string const& tag, std::string const& text)
    {
        if (!FileLog)
            return;

        std::ofstream out(FileLogPath, std::ios::app);
        if (!out.is_open())
            return;

        out << GameTime::GetGameTime().count() << " [" << tag << "] " << text << "\n";
    }

    static Player* FindOnlinePlayer(uint32 guidLow)
    {
        ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(guidLow);
        if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
            return player;
        return ObjectAccessor::FindPlayer(guid);
    }

    static void ImportPlayerbotConfigDefaults()
    {
        if (!UsePlayerbotConfig)
            return;

        std::string playerbotPrefix = sConfigMgr->GetOption<std::string>("AiPlayerbot.RandomBotAccountPrefix", "rndbot");
        uint32 playerbotAccountCount = sConfigMgr->GetOption<uint32>("AiPlayerbot.RandomBotAccountCount", 0);
        bool playerbotRandomAutologin = sConfigMgr->GetOption<bool>("AiPlayerbot.RandomBotAutologin", true);
        uint32 playerbotMinRandomBots = sConfigMgr->GetOption<uint32>("AiPlayerbot.MinRandomBots", 0);
        uint32 playerbotMaxRandomBots = sConfigMgr->GetOption<uint32>("AiPlayerbot.MaxRandomBots", 0);
        uint32 loginDelay = sConfigMgr->GetOption<uint32>("AiPlayerbot.DisabledWithoutRealPlayerLoginDelay", 30);

        if (IsAutoValue(BotAccountPrefix))
            BotAccountPrefix = playerbotPrefix;

        if (!BotAccountPrefix.empty())
            BotOnlyEligibilityFilter = true;

        if (StartupDelaySeconds < loginDelay)
            StartupDelaySeconds = loginDelay;

        LOG_INFO("module", "[PlayerbotWorldPvp] Playerbot config import: randomPrefix='{}' accountCount={} randomAutologin={} randomBots={}..{} startupDelay={}s",
            BotAccountPrefix, playerbotAccountCount, playerbotRandomAutologin ? 1 : 0, playerbotMinRandomBots, playerbotMaxRandomBots, StartupDelaySeconds);
    }

    static bool AccountMatchesBotFilter(uint32 accountId)
    {
        if (!BotOnlyEligibilityFilter)
            return true;

        bool hasPrefix = !BotAccountPrefix.empty();
        bool hasRange = BotAccountMax >= BotAccountMin && BotAccountMax > 0;
        if (!hasPrefix && !hasRange)
            return false;

        if (hasRange && (accountId < BotAccountMin || accountId > BotAccountMax))
            return false;

        if (!hasPrefix)
            return true;

        QueryResult result = LoginDatabase.Query("SELECT username FROM account WHERE id = {} LIMIT 1", accountId);
        if (!result)
            return false;

        std::string username = result->Fetch()[0].Get<std::string>();

        // AzerothCore stores account usernames uppercase because AccountMgr::CreateAccount
        // normalizes usernames, while the configured Playerbots prefix is usually lowercase
        // in playerbots.conf (example: ashrandbot). A case-sensitive prefix compare rejects
        // every valid random bot account and causes "not enough online bots" forever.
        return ToLowerCopy(username).rfind(ToLowerCopy(BotAccountPrefix), 0) == 0;
    }

    static bool IsBotSafeToMove(Player* player, BotCandidate const& b)
    {
        if (!player)
            return false;
        if (!AccountMatchesBotFilter(b.AccountId))
            return false;
        return true;
    }

    static bool VerifyDatabaseSchema()
    {
        QueryResult result = WorldDatabase.Query("SHOW TABLES LIKE 'playerbot_world_pvp_hotspot'");
        if (!result)
        {
            LOG_ERROR("module", "[PlayerbotWorldPvp] Missing WORLD table playerbot_world_pvp_hotspot. Apply data/sql/manual/world_playerbot_world_pvp.sql");
            return false;
        }
        return true;
    }

    static Hotspot ReadHotspot(Field* f)
    {
        Hotspot h;
        h.Id = f[0].Get<uint32>();
        h.Name = f[1].Get<std::string>();
        h.Enabled = f[2].Get<uint8>() != 0;
        h.AttackerTeam = static_cast<TeamSide>(f[3].Get<uint8>());
        h.DefenderTeam = static_cast<TeamSide>(f[4].Get<uint8>());
        h.MinLevel = f[5].Get<uint8>();
        h.MaxLevel = f[6].Get<uint8>();
        h.MapId = f[7].Get<uint32>();
        h.RallyX = f[8].Get<float>();
        h.RallyY = f[9].Get<float>();
        h.RallyZ = f[10].Get<float>();
        h.RallyO = f[11].Get<float>();
        h.TargetX = f[12].Get<float>();
        h.TargetY = f[13].Get<float>();
        h.TargetZ = f[14].Get<float>();
        h.TargetO = f[15].Get<float>();
        h.AttackersMin = f[16].Get<uint32>();
        h.AttackersMax = f[17].Get<uint32>();
        h.DefendersMin = f[18].Get<uint32>();
        h.DefendersMax = f[19].Get<uint32>();
        h.DurationMin = f[20].Get<uint32>();
        h.DurationMax = f[21].Get<uint32>();
        h.Weight = f[22].Get<uint32>();
        h.CooldownSeconds = f[23].Get<uint32>();
        h.LastStart = f[24].Get<uint32>();
        return h;
    }

    static std::string HotspotSelectSql(std::string const& whereExtra = "")
    {
        return "SELECT id,name,enabled,attacker_team,defender_team,min_level,max_level,map_id,"
            "rally_x,rally_y,rally_z,rally_o,target_x,target_y,target_z,target_o,"
            "attackers_min,attackers_max,defenders_min,defenders_max,duration_min,duration_max,weight,cooldown_seconds,last_start "
            "FROM playerbot_world_pvp_hotspot " + whereExtra;
    }

    static bool LoadHotspotByName(std::string name, Hotspot& out)
    {
        WorldDatabase.EscapeString(name);
        QueryResult result = WorldDatabase.Query(HotspotSelectSql("WHERE name='" + name + "' LIMIT 1").c_str());
        if (!result)
            return false;
        out = ReadHotspot(result->Fetch());
        return true;
    }

    static std::vector<Hotspot> LoadAvailableHotspots()
    {
        std::vector<Hotspot> spots;
        uint32 now = GameTime::GetGameTime().count();
        QueryResult result = WorldDatabase.Query(HotspotSelectSql("WHERE enabled=1 AND (last_start=0 OR last_start + cooldown_seconds <= " + std::to_string(now) + ") ORDER BY weight DESC, id ASC").c_str());
        if (!result)
            return spots;

        do
        {
            Hotspot h = ReadHotspot(result->Fetch());
            if (h.Weight > 0)
                spots.push_back(h);
        } while (result->NextRow());
        return spots;
    }

    static uint8 EffectiveMinLevel(uint8 minLevel)
    {
        uint32 effective = std::max<uint32>(minLevel, GlobalMinLevel);
        return static_cast<uint8>(std::min<uint32>(effective, 80));
    }

    static std::vector<BotCandidate> LoadCandidates(TeamSide team, uint8 minLevel, uint8 maxLevel, std::unordered_set<uint32> const& exclude)
    {
        std::vector<BotCandidate> candidates;
        uint8 effectiveMin = EffectiveMinLevel(minLevel);
        uint8 effectiveMax = maxLevel;
        if (effectiveMax < effectiveMin)
            return candidates;

        QueryResult result = CharacterDatabase.Query(
            "SELECT guid,account,name,level,race,class,online FROM characters "
            "WHERE online=1 AND level BETWEEN {} AND {} ORDER BY RAND() LIMIT {}",
            effectiveMin, effectiveMax, BotQueryLimit);

        if (!result)
            return candidates;

        do
        {
            Field* f = result->Fetch();
            BotCandidate b;
            b.GuidLow = f[0].Get<uint32>();
            b.AccountId = f[1].Get<uint32>();
            b.Name = f[2].Get<std::string>();
            b.Level = f[3].Get<uint8>();
            b.Race = f[4].Get<uint8>();
            b.Class = f[5].Get<uint8>();
            b.Online = f[6].Get<uint8>() != 0;
            b.Team = TeamForRace(b.Race);

            if (exclude.find(b.GuidLow) != exclude.end())
                continue;
            if (BotActivity::IsReserved(b.GuidLow))
                continue;   // busy in a dungeon/raid run
            if (team != TEAM_SIDE_ANY && b.Team != team)
                continue;
            if (!AccountMatchesBotFilter(b.AccountId))
                continue;
            if (!IsBotSafeToMove(FindOnlinePlayer(b.GuidLow), b))
                continue;

            candidates.push_back(b);
        } while (result->NextRow());

        return candidates;
    }

    static std::vector<BotCandidate> LoadOfflineCandidates(TeamSide team, uint8 level, std::unordered_set<uint32> const& exclude)
    {
        std::vector<BotCandidate> candidates;
        QueryResult result = CharacterDatabase.Query(
            "SELECT guid,account,name,level,race,class,online FROM characters "
            "WHERE online=0 AND level={} ORDER BY RAND() LIMIT {}", level, BotQueryLimit);
        if (!result)
            return candidates;

        do
        {
            Field* f = result->Fetch();
            BotCandidate b;
            b.GuidLow = f[0].Get<uint32>();
            b.AccountId = f[1].Get<uint32>();
            b.Name = f[2].Get<std::string>();
            b.Level = f[3].Get<uint8>();
            b.Race = f[4].Get<uint8>();
            b.Class = f[5].Get<uint8>();
            b.Online = false;
            b.Team = TeamForRace(b.Race);
            if (exclude.find(b.GuidLow) != exclude.end() || BotActivity::IsReserved(b.GuidLow))
                continue;
            if (b.Team != team || !AccountMatchesBotFilter(b.AccountId))
                continue;
            candidates.push_back(b);
        } while (result->NextRow());
        return candidates;
    }

    static uint32 RandomCount(uint32 minCount, uint32 maxCount)
    {
        minCount = std::min(minCount, MaxBotsPerSide);
        maxCount = std::min(maxCount, MaxBotsPerSide);
        if (maxCount < minCount)
            maxCount = minCount;
        return urand(minCount, maxCount);
    }

    static float Jitter()
    {
        if (PositionJitter == 0)
            return 0.0f;
        return static_cast<float>(irand(-static_cast<int32>(PositionJitter), static_cast<int32>(PositionJitter)));
    }

    static void ScheduleMove(uint64 eventId, BotCandidate const& b, uint32 mapId, float x, float y, float z, float o, uint32 delay, bool teleport)
    {
        MoveStep step;
        step.EventId = eventId;
        step.GuidLow = b.GuidLow;
        step.Name = b.Name;
        step.MapId = mapId;
        step.X = x;
        step.Y = y;
        step.Z = z;
        step.O = o;
        step.ExecuteAt = GameTime::GetGameTime().count() + delay;
        step.UseTeleport = teleport;
        _moveQueue.push_back(step);
    }

    static bool IsSameFactionDuelSpot(Hotspot const& spot)
    {
        return SameFactionDuels && spot.AttackerTeam != TEAM_SIDE_ANY && spot.AttackerTeam == spot.DefenderTeam;
    }

    static void ScheduleDuel(uint64 eventId, BotCandidate const& challenger, BotCandidate const& target, uint32 delay)
    {
        DuelStep step;
        step.EventId = eventId;
        step.ChallengerGuidLow = challenger.GuidLow;
        step.TargetGuidLow = target.GuidLow;
        step.ChallengerName = challenger.Name;
        step.TargetName = target.Name;
        step.ExecuteAt = GameTime::GetGameTime().count() + delay;
        _duelQueue.push_back(step);
    }

    static void MoveBot(Player* player, uint32 mapId, float x, float y, float z, float o, bool teleport)
    {
        if (!player)
            return;

        if (player->IsInCombat())
            player->CombatStop(true);

        if (teleport || player->GetMapId() != mapId)
            player->TeleportTo(mapId, x, y, z, o);
        else if (UseMovePointAfterRally)
            player->GetMotionMaster()->MovePoint(0, x, y, z);
        else
            player->TeleportTo(mapId, x, y, z, o);
    }

    static void ProcessMoveQueue()
    {
        if (_moveQueue.empty())
            return;

        uint32 now = GameTime::GetGameTime().count();
        std::vector<MoveStep> keep;
        keep.reserve(_moveQueue.size());

        for (MoveStep const& step : _moveQueue)
        {
            if (step.ExecuteAt > now)
            {
                keep.push_back(step);
                continue;
            }

            Player* p = FindOnlinePlayer(step.GuidLow);
            if (!p || p->IsBeingTeleported())
            {
                MoveStep retry = step;
                retry.ExecuteAt = now + 2;
                keep.push_back(retry);
                continue;
            }

            MoveBot(p, step.MapId, step.X, step.Y, step.Z, step.O, step.UseTeleport);
            StatusLog("[Move] " + step.Name + (step.UseTeleport ? " teleported" : " moving") + " to event #" + std::to_string(step.EventId));
            AuditLog("MOVE", "event=" + std::to_string(step.EventId) + " player=" + step.Name + " guid=" + std::to_string(step.GuidLow) +
                " map=" + std::to_string(step.MapId) + " x=" + std::to_string(step.X) + " y=" + std::to_string(step.Y) + " z=" + std::to_string(step.Z) +
                " mode=" + (step.UseTeleport ? std::string("teleport") : std::string("movepoint")));
        }

        _moveQueue.swap(keep);
    }

    static void ProcessDuelQueue()
    {
        if (_duelQueue.empty())
            return;

        uint32 now = GameTime::GetGameTime().count();
        std::vector<DuelStep> keep;
        keep.reserve(_duelQueue.size());

        for (DuelStep const& step : _duelQueue)
        {
            if (step.ExecuteAt > now)
            {
                keep.push_back(step);
                continue;
            }

            Player* challenger = FindOnlinePlayer(step.ChallengerGuidLow);
            Player* target = FindOnlinePlayer(step.TargetGuidLow);
            if (!challenger || !target || challenger->IsBeingTeleported() || target->IsBeingTeleported())
            {
                DuelStep retry = step;
                if (retry.RetryCount < 20)
                {
                    ++retry.RetryCount;
                    retry.ExecuteAt = now + 2;
                    keep.push_back(retry);
                }
                continue;
            }

            BotCandidate cb; cb.GuidLow = step.ChallengerGuidLow; cb.AccountId = challenger->GetSession() ? challenger->GetSession()->GetAccountId() : 0; cb.Name = step.ChallengerName;
            BotCandidate tb; tb.GuidLow = step.TargetGuidLow; tb.AccountId = target->GetSession() ? target->GetSession()->GetAccountId() : 0; tb.Name = step.TargetName;
            if (!IsBotSafeToMove(challenger, cb) || !IsBotSafeToMove(target, tb))
                continue;

            if (challenger->GetMapId() != target->GetMapId() || challenger->GetDistance(target) > 35.0f)
            {
                // Bring the pair close enough for the Duel spell/request to have a chance.
                target->TeleportTo(challenger->GetMapId(), challenger->GetPositionX() + 3.0f, challenger->GetPositionY() + 3.0f, challenger->GetPositionZ(), challenger->GetOrientation());
                DuelStep retry = step;
                if (retry.RetryCount < 20)
                {
                    ++retry.RetryCount;
                    retry.ExecuteAt = now + 3;
                    keep.push_back(retry);
                }
                continue;
            }

            if (challenger->IsInCombat())
                challenger->CombatStop(true);
            if (target->IsInCombat())
                target->CombatStop(true);

            // 7266 is the classic Duel spell/effect. Playerbot AI may auto-accept; if not,
            // this still creates a visible duel request and lets us verify the event staging.
            challenger->CastSpell(target, DuelSpellId, true);
            challenger->SetFacingToObject(target);
            target->SetFacingToObject(challenger);

            StatusLog("[Duel] event #" + std::to_string(step.EventId) + " " + step.ChallengerName + " challenged " + step.TargetName + ".");
            AuditLog("DUEL", "event=" + std::to_string(step.EventId) + " challenger=" + step.ChallengerName + " target=" + step.TargetName + " spell=" + std::to_string(DuelSpellId));
        }

        _duelQueue.swap(keep);
    }

    static bool StartHotspot(Hotspot spot, ChatHandler* handler = nullptr)
    {
        std::unordered_set<uint32> excluded;
        for (ActiveEvent const& e : _activeEvents)
            for (Participant const& p : e.Members)
                excluded.insert(p.Bot.GuidLow);

        uint32 needAttackers = RandomCount(spot.AttackersMin, spot.AttackersMax);
        uint32 needDefenders = RandomCount(spot.DefendersMin, spot.DefendersMax);
        std::vector<BotCandidate> attackers = LoadCandidates(spot.AttackerTeam, spot.MinLevel, spot.MaxLevel, excluded);
        for (BotCandidate const& b : attackers)
            excluded.insert(b.GuidLow);
        std::vector<BotCandidate> defenders = LoadCandidates(spot.DefenderTeam, spot.MinLevel, spot.MaxLevel, excluded);

        if (attackers.size() < needAttackers || defenders.size() < needDefenders || attackers.size() + defenders.size() < MinOnlineBotsRequired)
        {
            std::string msg = "not enough online bots for " + spot.Name + ": attackers " + std::to_string(attackers.size()) + "/" + std::to_string(needAttackers) +
                " defenders " + std::to_string(defenders.size()) + "/" + std::to_string(needDefenders);
            StatusLog(msg);
            if (handler)
                handler->PSendSysMessage("PlayerbotWorldPvp: {}", msg);
            return false;
        }

        attackers.resize(needAttackers);
        defenders.resize(needDefenders);

        uint64 eventId = _nextEventId++;
        uint32 now = GameTime::GetGameTime().count();
        ActiveEvent event;
        event.EventId = eventId;
        event.Spot = spot;
        event.StartedAt = now;
        event.EndsAt = now + urand(spot.DurationMin, spot.DurationMax) * MINUTE;

        bool sameFactionDuel = IsSameFactionDuelSpot(spot);
        StatusLog("EVENT #" + std::to_string(eventId) + " START " + TeamName(spot.AttackerTeam) + (sameFactionDuel ? " duel practice at " : " attacking ") + spot.Name +
            " vs " + TeamName(spot.DefenderTeam) + (sameFactionDuel ? " sparring partners=" : " defenders attackers=") + std::to_string(attackers.size()) + " defenders=" + std::to_string(defenders.size()));
        AuditLog("START", "event=" + std::to_string(eventId) + " spot=" + spot.Name + " mode=" + (sameFactionDuel ? std::string("duel") : std::string("world_pvp")) +
            " attackerTeam=" + TeamName(spot.AttackerTeam) + " defenderTeam=" + TeamName(spot.DefenderTeam) + " attackers=" + std::to_string(attackers.size()) + " defenders=" + std::to_string(defenders.size()) +
            " durationSeconds=" + std::to_string(event.EndsAt - event.StartedAt));

        auto addParticipant = [&](BotCandidate const& b, bool attacker)
            {
                Player* p = FindOnlinePlayer(b.GuidLow);
                if (!IsBotSafeToMove(p, b))
                    return;

                Participant part;
                part.Bot = b;
                part.Attacker = attacker;
                part.OriginalMap = p->GetMapId();
                part.OriginalX = p->GetPositionX();
                part.OriginalY = p->GetPositionY();
                part.OriginalZ = p->GetPositionZ();
                part.OriginalO = p->GetOrientation();
                event.Members.push_back(part);
                BotActivity::Reserve(b.GuidLow);   // claim so the dungeon-sim won't grab it

                float sx = (attacker ? spot.RallyX : spot.TargetX) + Jitter();
                float sy = (attacker ? spot.RallyY : spot.TargetY) + Jitter();
                float sz = attacker ? spot.RallyZ : spot.TargetZ;
                float so = attacker ? spot.RallyO : spot.TargetO;
                if (TeleportBots)
                    MoveBot(p, spot.MapId, sx, sy, sz, so, true);

                AuditLog("MEMBER", "event=" + std::to_string(eventId) + " side=" + (attacker ? std::string("attacker") : std::string("defender")) +
                    " name=" + b.Name + " guid=" + std::to_string(b.GuidLow) + " level=" + std::to_string(b.Level) + " class=" + std::to_string(b.Class));

                if (attacker)
                {
                    ScheduleMove(eventId, b, spot.MapId, spot.TargetX + Jitter(), spot.TargetY + Jitter(), spot.TargetZ, spot.TargetO,
                        MoveFromRallyDelaySeconds, HardTeleportToTarget);
                }
            };

        for (BotCandidate const& b : attackers)
            addParticipant(b, true);
        for (BotCandidate const& b : defenders)
            addParticipant(b, false);

        if (sameFactionDuel)
        {
            uint32 pairCount = std::min<uint32>(std::min<uint32>(attackers.size(), defenders.size()), DuelPairLimit);
            for (uint32 i = 0; i < pairCount; ++i)
                ScheduleDuel(eventId, attackers[i], defenders[i], MoveFromRallyDelaySeconds + DuelRequestDelaySeconds + i);
            AuditLog("DUEL_QUEUE", "event=" + std::to_string(eventId) + " spot=" + spot.Name + " pairs=" + std::to_string(pairCount));
        }

        _activeEvents.push_back(event);

        WorldDatabase.Execute("UPDATE playerbot_world_pvp_hotspot SET last_start = {} WHERE id = {}", now, spot.Id);

        if (handler)
            handler->PSendSysMessage("PlayerbotWorldPvp: started event #{} at {}. Attackers={} Defenders={} Duration={}m.", eventId, spot.Name, attackers.size(), defenders.size(), (event.EndsAt - event.StartedAt) / MINUTE);
        return true;
    }

    static void EndEvent(ActiveEvent const& event, std::string const& reason)
    {
        StatusLog("EVENT #" + std::to_string(event.EventId) + " END " + event.Spot.Name + " reason=" + reason);
        AuditLog("END", "event=" + std::to_string(event.EventId) + " spot=" + event.Spot.Name + " reason=" + reason + " members=" + std::to_string(event.Members.size()));

        // Free the reservation for every participant (even if we don't teleport
        // them back) so these bots become available to other activities again.
        for (Participant const& part : event.Members)
            BotActivity::Release(part.Bot.GuidLow);

        if (!ReturnBotsAfterEvent)
            return;

        for (Participant const& part : event.Members)
        {
            Player* p = FindOnlinePlayer(part.Bot.GuidLow);
            if (!IsBotSafeToMove(p, part.Bot))
                continue;
            if (p->IsBeingTeleported())
                continue;
            if (p->IsInCombat())
                p->CombatStop(true);

            p->TeleportTo(part.OriginalMap, part.OriginalX, part.OriginalY, part.OriginalZ, part.OriginalO);
            AuditLog("RETURN", "event=" + std::to_string(event.EventId) + " player=" + part.Bot.Name + " map=" + std::to_string(part.OriginalMap));
        }
    }

    static void ExpireEvents()
    {
        if (_activeEvents.empty())
            return;

        uint32 now = GameTime::GetGameTime().count();
        std::vector<ActiveEvent> keep;
        keep.reserve(_activeEvents.size());

        for (ActiveEvent const& e : _activeEvents)
        {
            if (e.EndsAt <= now)
                EndEvent(e, "timer");
            else
                keep.push_back(e);
        }
        _activeEvents.swap(keep);
    }

    static void StartPendingRaidResponses()
    {
        uint32 now = GameTime::GetGameTime().count();
        std::vector<PendingRaidResponse> keep;
        for (PendingRaidResponse const& pending : _pendingRaidResponses)
        {
            if (pending.StartAt > now)
            {
                keep.push_back(pending);
                continue;
            }

            ActiveEvent event;
            event.EventId = _nextEventId++;
            event.Spot.Name = "Reactive raid response";
            event.Spot.MapId = pending.MapId;
            event.StartedAt = now;
            event.EndsAt = now + ReactiveDurationMinutes * MINUTE;
            float angle = pending.TargetO + 3.14159265f;
            uint32 index = 0;
            for (BotCandidate bot : pending.Bots)
            {
                Player* player = FindOnlinePlayer(bot.GuidLow);
                if (!IsBotSafeToMove(player, bot))
                    continue;

                bot.Online = true;
                Participant part;
                part.Bot = bot;
                part.Attacker = true;
                part.OriginalMap = player->GetMapId();
                part.OriginalX = player->GetPositionX();
                part.OriginalY = player->GetPositionY();
                part.OriginalZ = player->GetPositionZ();
                part.OriginalO = player->GetOrientation();
                event.Members.push_back(part);
                BotActivity::Reserve(bot.GuidLow);
                player->SetPvP(true);
                if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
                {
                    ai->ChangeStrategy(PvpCombatStrategies, BOT_STATE_COMBAT);
                    ai->ChangeStrategy(PvpNonCombatStrategies, BOT_STATE_NON_COMBAT);
                }

                float lane = static_cast<float>(static_cast<int32>(index % 9) - 4) * 4.0f;
                float rallyX = pending.TargetX + std::cos(angle) * 120.0f - std::sin(angle) * lane;
                float rallyY = pending.TargetY + std::sin(angle) * 120.0f + std::cos(angle) * lane;
                MoveBot(player, pending.MapId, rallyX, rallyY, pending.TargetZ, angle, true);
                ScheduleMove(event.EventId, bot, pending.MapId,
                    pending.TargetX + std::cos(angle) * 25.0f + Jitter(),
                    pending.TargetY + std::sin(angle) * 25.0f + Jitter(),
                    pending.TargetZ, pending.TargetO, MoveFromRallyDelaySeconds, HardTeleportToTarget);
                ++index;
            }

            if (!event.Members.empty())
            {
                StatusLog("REACTIVE EVENT #" + std::to_string(event.EventId) + " START responders=" + std::to_string(event.Members.size()));
                AuditLog("REACTIVE_START", "event=" + std::to_string(event.EventId) + " trigger=" + std::to_string(pending.TriggerGuidLow) +
                    " team=" + TeamName(pending.ResponseTeam) + " responders=" + std::to_string(event.Members.size()));
                _activeEvents.push_back(event);
            }
        }
        _pendingRaidResponses.swap(keep);
    }

    static void TryQueueReactiveRaidResponse()
    {
        if (!ReactiveRaidResponse || !_pendingRaidResponses.empty() || _activeEvents.size() >= MaxActiveEvents)
            return;

        uint32 now = GameTime::GetGameTime().count();
        if (_lastReactiveResponse && _lastReactiveResponse + ReactiveCooldownSeconds > now)
            return;

        HashMapHolder<Player>::MapType const& players = ObjectAccessor::GetPlayers();
        for (auto const& pair : players)
        {
            Player* leader = pair.second;
            if (!leader || !leader->GetSession() || !leader->IsInWorld() || !leader->GetMap() || leader->GetMap()->Instanceable())
                continue;
            if (AccountMatchesBotFilter(leader->GetSession()->GetAccountId()))
                continue;
            if (!leader->IsPvP())
                continue;
            Group* group = leader->GetGroup();
            if (!group || !group->IsLeader(leader->GetGUID()) || group->GetMembersCount() < ReactiveMinGroupSize)
                continue;

            TeamSide responseTeam = leader->GetTeamId() == TEAM_ALLIANCE ? TEAM_SIDE_HORDE : TEAM_SIDE_ALLIANCE;
            uint32 desired = std::min<uint32>(group->GetMembersCount(), ReactiveMaxResponders);
            std::unordered_set<uint32> excluded;
            for (ActiveEvent const& event : _activeEvents)
                for (Participant const& member : event.Members)
                    excluded.insert(member.Bot.GuidLow);

            std::vector<BotCandidate> responders = LoadCandidates(responseTeam, leader->GetLevel(), 60, excluded);
            if (responders.size() > desired)
                responders.resize(desired);
            for (BotCandidate const& bot : responders)
                excluded.insert(bot.GuidLow);

            if (responders.size() < desired)
            {
                std::vector<BotCandidate> offline = LoadOfflineCandidates(responseTeam, 60, excluded);
                uint32 need = desired - static_cast<uint32>(responders.size());
                if (offline.size() > need)
                    offline.resize(need);
                for (BotCandidate const& bot : offline)
                {
                    responders.push_back(bot);
                    sRandomPlayerbotMgr.AddPlayerBot(ObjectGuid::Create<HighGuid::Player>(bot.GuidLow), 0);
                }
            }

            if (responders.empty())
                return;

            PendingRaidResponse pending;
            pending.TriggerGuidLow = leader->GetGUID().GetCounter();
            pending.ResponseTeam = responseTeam;
            pending.MapId = leader->GetMapId();
            pending.TargetX = leader->GetPositionX();
            pending.TargetY = leader->GetPositionY();
            pending.TargetZ = leader->GetPositionZ();
            pending.TargetO = leader->GetOrientation();
            pending.StartAt = now + ReactiveLoginDelaySeconds;
            pending.Bots = std::move(responders);
            _pendingRaidResponses.push_back(std::move(pending));
            _lastReactiveResponse = now;
            StatusLog("Queued " + TeamName(responseTeam) + " raid response for " + std::to_string(group->GetMembersCount()) + " invaders.");
            return;
        }
    }

    static void TryStartRandomEvent()
    {
        if (_activeEvents.size() >= MaxActiveEvents)
            return;
        if (urand(1, 100) > EventChancePerTick)
            return;

        std::vector<Hotspot> spots = LoadAvailableHotspots();
        if (spots.empty())
            return;

        uint32 totalWeight = 0;
        for (Hotspot const& h : spots)
            totalWeight += h.Weight;
        uint32 roll = totalWeight ? urand(1, totalWeight) : 1;
        uint32 cursor = 0;
        for (Hotspot const& h : spots)
        {
            cursor += h.Weight;
            if (roll <= cursor)
            {
                StartHotspot(h);
                return;
            }
        }
    }

    static void PrintStatus(ChatHandler* handler = nullptr)
    {
        std::string header = "active events=" + std::to_string(_activeEvents.size()) + " queuedMoves=" + std::to_string(_moveQueue.size()) + " queuedDuels=" + std::to_string(_duelQueue.size());
        if (handler)
            handler->PSendSysMessage("PlayerbotWorldPvp: {}", header);
        else
            StatusLog(header);

        for (ActiveEvent const& e : _activeEvents)
        {
            uint32 now = GameTime::GetGameTime().count();
            uint32 left = e.EndsAt > now ? e.EndsAt - now : 0;
            uint32 attackers = 0;
            uint32 defenders = 0;
            for (Participant const& p : e.Members)
                p.Attacker ? ++attackers : ++defenders;

            std::string line = "#" + std::to_string(e.EventId) + " " + e.Spot.Name + " " + (IsSameFactionDuelSpot(e.Spot) ? std::string("DUEL ") : std::string("WAR ")) + TeamName(e.Spot.AttackerTeam) + " vs " + TeamName(e.Spot.DefenderTeam) +
                " attackers=" + std::to_string(attackers) + " defenders=" + std::to_string(defenders) + " eta=" + std::to_string(left / MINUTE) + "m";
            if (handler)
                handler->PSendSysMessage("{}", line);
            else
                StatusLog(line);
        }
    }

    static void StopEventById(uint64 id, ChatHandler* handler)
    {
        if (id == 0)
        {
            for (ActiveEvent const& e : _activeEvents)
                EndEvent(e, "gm-stop-all");
            _activeEvents.clear();
            _moveQueue.clear();
            _duelQueue.clear();
            if (handler)
                handler->PSendSysMessage("PlayerbotWorldPvp: stopped all events.");
            return;
        }

        std::vector<ActiveEvent> keep;
        bool found = false;
        for (ActiveEvent const& e : _activeEvents)
        {
            if (e.EventId == id)
            {
                found = true;
                EndEvent(e, "gm-stop");
            }
            else
                keep.push_back(e);
        }
        _activeEvents.swap(keep);
        _moveQueue.erase(std::remove_if(_moveQueue.begin(), _moveQueue.end(), [id](MoveStep const& s) { return s.EventId == id; }), _moveQueue.end());
        _duelQueue.erase(std::remove_if(_duelQueue.begin(), _duelQueue.end(), [id](DuelStep const& s) { return s.EventId == id; }), _duelQueue.end());
        if (handler)
            handler->PSendSysMessage("PlayerbotWorldPvp: {} event #{}.", found ? "stopped" : "did not find", id);
    }

    static void PrintScan(ChatHandler* handler, uint8 minLevel, uint8 maxLevel)
    {
        uint8 effectiveMin = EffectiveMinLevel(minLevel);
        uint8 effectiveMax = maxLevel;
        if (effectiveMax < effectiveMin)
            effectiveMax = effectiveMin;

        uint32 alliance = 0;
        uint32 horde = 0;
        uint32 total = 0;

        QueryResult result = CharacterDatabase.Query(
            "SELECT guid,account,name,level,race,class,online FROM characters "
            "WHERE online=1 AND level BETWEEN {} AND {} ORDER BY level DESC LIMIT {}",
            effectiveMin, effectiveMax, BotQueryLimit);

        if (result)
        {
            do
            {
                Field* f = result->Fetch();
                BotCandidate b;
                b.GuidLow = f[0].Get<uint32>();
                b.AccountId = f[1].Get<uint32>();
                b.Name = f[2].Get<std::string>();
                b.Level = f[3].Get<uint8>();
                b.Race = f[4].Get<uint8>();
                b.Class = f[5].Get<uint8>();
                b.Online = f[6].Get<uint8>() != 0;
                b.Team = TeamForRace(b.Race);

                if (!AccountMatchesBotFilter(b.AccountId))
                    continue;
                if (!IsBotSafeToMove(FindOnlinePlayer(b.GuidLow), b))
                    continue;

                ++total;
                if (b.Team == TEAM_SIDE_ALLIANCE)
                    ++alliance;
                else if (b.Team == TEAM_SIDE_HORDE)
                    ++horde;
            } while (result->NextRow());
        }

        handler->PSendSysMessage("PlayerbotWorldPvp scan lvl {}..{} prefix='{}': Alliance={} Horde={} Total={} queryLimit={} globalMin={}.",
            effectiveMin, effectiveMax, BotAccountPrefix, alliance, horde, total, BotQueryLimit, GlobalMinLevel);

        std::vector<Hotspot> spots = LoadAvailableHotspots();
        if (spots.empty())
        {
            handler->PSendSysMessage("No enabled/cooldown-ready hotspots right now.");
            return;
        }

        std::unordered_set<uint32> exclude;
        uint32 shown = 0;
        for (Hotspot const& h : spots)
        {
            if (shown >= 12)
                break;
            std::vector<BotCandidate> attackers = LoadCandidates(h.AttackerTeam, h.MinLevel, h.MaxLevel, exclude);
            std::vector<BotCandidate> defenders = LoadCandidates(h.DefenderTeam, h.MinLevel, h.MaxLevel, exclude);
            handler->PSendSysMessage("{}: {} attackers={} need={}..{} / {} defenders={} need={}..{} lvl={}..{} weight={}",
                h.Name, TeamName(h.AttackerTeam), attackers.size(), h.AttackersMin, h.AttackersMax,
                TeamName(h.DefenderTeam), defenders.size(), h.DefendersMin, h.DefendersMax, EffectiveMinLevel(h.MinLevel), h.MaxLevel, h.Weight);
            ++shown;
        }
    }

    static void ShowHelp(ChatHandler* handler)
    {
        handler->PSendSysMessage("PlayerbotWorldPvp commands:");
        handler->PSendSysMessage(".wpvp status  / console: wpvp status");
        handler->PSendSysMessage(".wpvp scan [minLvl] [maxLvl]  / console: wpvp scan [minLvl] [maxLvl]");
        handler->PSendSysMessage(".wpvp start <spot>  / console: wpvp start <spot>");
        handler->PSendSysMessage(".wpvp stop <id|all>  / console: wpvp stop <id|all>");
        handler->PSendSysMessage(".wpvp spot list  / console: wpvp spot list");
        handler->PSendSysMessage(".wpvp spot create <name> <attacker:alliance|horde> <defender:alliance|horde> <minLvl> <maxLvl>  (in-game GM only: uses your position)");
        handler->PSendSysMessage(".wpvp spot rally <name>   - in-game GM only: save your current position as attacker rally/outside-town point");
        handler->PSendSysMessage(".wpvp spot target <name>  - in-game GM only: save your current position as target/defender point");
        handler->PSendSysMessage(".wpvp spot counts <name> <attMin> <attMax> <defMin> <defMax>");
        handler->PSendSysMessage(".wpvp spot duration <name> <minMinutes> <maxMinutes>");
        handler->PSendSysMessage(".wpvp spot enable <name> <0|1>");
        handler->PSendSysMessage("Same-faction spots are duel practice: e.g. StormwindDuel / OrgrimmarDuel.");
    }

    static bool HandleSpotCommand(ChatHandler* handler, std::vector<std::string> const& args)
    {
        if (args.size() < 2)
        {
            ShowHelp(handler);
            return true;
        }

        std::string action = ToLowerCopy(args[1]);
        if (action == "list")
        {
            QueryResult result = WorldDatabase.Query(HotspotSelectSql("ORDER BY enabled DESC, weight DESC, id ASC").c_str());
            if (!result)
            {
                handler->PSendSysMessage("PlayerbotWorldPvp: no hotspots found.");
                return true;
            }
            do
            {
                Hotspot h = ReadHotspot(result->Fetch());
                handler->PSendSysMessage("#{} {} enabled={} {}->{} lvl={}..{} map={} attackers={}..{} defenders={}..{} duration={}..{}m cooldown={}s last={}",
                    h.Id, h.Name, h.Enabled ? 1 : 0, TeamName(h.AttackerTeam), TeamName(h.DefenderTeam), h.MinLevel, h.MaxLevel,
                    h.MapId, h.AttackersMin, h.AttackersMax, h.DefendersMin, h.DefendersMax, h.DurationMin, h.DurationMax, h.CooldownSeconds, h.LastStart);
            } while (result->NextRow());
            return true;
        }

        if (action == "create")
        {
            if (args.size() < 7)
            {
                handler->PSendSysMessage("Syntax: .wpvp spot create <name> <attacker> <defender> <minLvl> <maxLvl>");
                return true;
            }
            Player* gm = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
            if (!gm)
            {
                handler->PSendSysMessage("PlayerbotWorldPvp: this command needs an in-game GM because it uses your current map/X/Y/Z position.");
                return true;
            }

            TeamSide attacker;
            TeamSide defender;
            if (!ParseTeam(args[3], attacker) || !ParseTeam(args[4], defender))
            {
                handler->PSendSysMessage("Bad team. Use alliance or horde.");
                return true;
            }
            uint32 minLevel = std::stoul(args[5]);
            uint32 maxLevel = std::stoul(args[6]);
            std::string name = args[2];
            WorldDatabase.EscapeString(name);

            WorldDatabase.Execute(
                "INSERT INTO playerbot_world_pvp_hotspot "
                "(name,enabled,attacker_team,defender_team,min_level,max_level,map_id,rally_x,rally_y,rally_z,rally_o,target_x,target_y,target_z,target_o) "
                "VALUES ('{}',1,{},{},{},{},{},{},{},{},{},{},{},{},{}) "
                "ON DUPLICATE KEY UPDATE attacker_team=VALUES(attacker_team), defender_team=VALUES(defender_team), min_level=VALUES(min_level), max_level=VALUES(max_level), map_id=VALUES(map_id), rally_x=VALUES(rally_x), rally_y=VALUES(rally_y), rally_z=VALUES(rally_z), rally_o=VALUES(rally_o), target_x=VALUES(target_x), target_y=VALUES(target_y), target_z=VALUES(target_z), target_o=VALUES(target_o)",
                name, static_cast<uint32>(attacker), static_cast<uint32>(defender), minLevel, maxLevel, gm->GetMapId(),
                gm->GetPositionX(), gm->GetPositionY(), gm->GetPositionZ(), gm->GetOrientation(),
                gm->GetPositionX(), gm->GetPositionY(), gm->GetPositionZ(), gm->GetOrientation());
            handler->PSendSysMessage("PlayerbotWorldPvp: created/updated hotspot {}. Now set rally/target positions.", args[2]);
            return true;
        }

        if ((action == "rally" || action == "target") && args.size() >= 3)
        {
            Player* gm = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
            if (!gm)
            {
                handler->PSendSysMessage("PlayerbotWorldPvp: spot rally/target needs an in-game GM because it saves your current map/X/Y/Z position.");
                return true;
            }
            std::string name = args[2];
            WorldDatabase.EscapeString(name);
            std::string prefix = action == "rally" ? "rally" : "target";
            WorldDatabase.Execute("UPDATE playerbot_world_pvp_hotspot SET map_id={}, {}_x={}, {}_y={}, {}_z={}, {}_o={} WHERE name='{}'",
                gm->GetMapId(), prefix, gm->GetPositionX(), prefix, gm->GetPositionY(), prefix, gm->GetPositionZ(), prefix, gm->GetOrientation(), name);
            handler->PSendSysMessage("PlayerbotWorldPvp: saved {} point for {} at map={} x={} y={} z={}.", prefix, args[2], gm->GetMapId(), gm->GetPositionX(), gm->GetPositionY(), gm->GetPositionZ());
            return true;
        }

        if (action == "counts" && args.size() >= 7)
        {
            std::string name = args[2];
            WorldDatabase.EscapeString(name);
            WorldDatabase.Execute("UPDATE playerbot_world_pvp_hotspot SET attackers_min={}, attackers_max={}, defenders_min={}, defenders_max={} WHERE name='{}'",
                std::stoul(args[3]), std::stoul(args[4]), std::stoul(args[5]), std::stoul(args[6]), name);
            handler->PSendSysMessage("PlayerbotWorldPvp: updated counts for {}.", args[2]);
            return true;
        }

        if (action == "duration" && args.size() >= 5)
        {
            std::string name = args[2];
            WorldDatabase.EscapeString(name);
            WorldDatabase.Execute("UPDATE playerbot_world_pvp_hotspot SET duration_min={}, duration_max={} WHERE name='{}'",
                std::stoul(args[3]), std::stoul(args[4]), name);
            handler->PSendSysMessage("PlayerbotWorldPvp: updated duration for {}.", args[2]);
            return true;
        }

        if (action == "enable" && args.size() >= 4)
        {
            std::string name = args[2];
            WorldDatabase.EscapeString(name);
            WorldDatabase.Execute("UPDATE playerbot_world_pvp_hotspot SET enabled={} WHERE name='{}'", std::stoul(args[3]) ? 1 : 0, name);
            handler->PSendSysMessage("PlayerbotWorldPvp: updated enabled for {}.", args[2]);
            return true;
        }

        ShowHelp(handler);
        return true;
    }

    static bool HandleGmCommand(ChatHandler* handler, std::string text)
    {
        std::istringstream iss(text);
        std::vector<std::string> args;
        std::string token;
        while (iss >> token)
            args.push_back(token);

        if (args.empty())
        {
            ShowHelp(handler);
            return true;
        }
        if (args[0] == ".wpvp")
            args.erase(args.begin());
        if (args.empty() || ToLowerCopy(args[0]) == "help")
        {
            ShowHelp(handler);
            return true;
        }

        std::string cmd = ToLowerCopy(args[0]);
        if (cmd == "status")
        {
            PrintStatus(handler);
            return true;
        }
        if (cmd == "scan")
        {
            uint8 minLevel = args.size() >= 2 ? static_cast<uint8>(std::min<uint32>(std::stoul(args[1]), 80)) : static_cast<uint8>(GlobalMinLevel);
            uint8 maxLevel = args.size() >= 3 ? static_cast<uint8>(std::min<uint32>(std::stoul(args[2]), 80)) : 60;
            PrintScan(handler, minLevel, maxLevel);
            return true;
        }
        if (cmd == "spot")
            return HandleSpotCommand(handler, args);
        if (cmd == "start")
        {
            if (args.size() < 2)
            {
                handler->PSendSysMessage("Syntax: .wpvp start <spot>");
                return true;
            }
            Hotspot h;
            if (!LoadHotspotByName(args[1], h))
            {
                handler->PSendSysMessage("PlayerbotWorldPvp: hotspot '{}' not found.", args[1]);
                return true;
            }
            StartHotspot(h, handler);
            return true;
        }
        if (cmd == "stop")
        {
            if (args.size() < 2 || ToLowerCopy(args[1]) == "all")
            {
                StopEventById(0, handler);
                return true;
            }
            StopEventById(std::stoull(args[1]), handler);
            return true;
        }

        ShowHelp(handler);
        return true;
    }
}

class PlayerbotWorldPvpWorldScript : public WorldScript
{
public:
    PlayerbotWorldPvpWorldScript() : WorldScript("PlayerbotWorldPvpWorldScript") {}

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        using namespace PlayerbotWorldPvp;
        Enable = sConfigMgr->GetOption<bool>("PlayerbotWorldPvp.Enable", true);
        Debug = sConfigMgr->GetOption<bool>("PlayerbotWorldPvp.Debug", true);
        TickSeconds = sConfigMgr->GetOption<uint32>("PlayerbotWorldPvp.TickSeconds", 60);
        StartupDelaySeconds = sConfigMgr->GetOption<uint32>("PlayerbotWorldPvp.StartupDelaySeconds", 120);
        BotOnlyEligibilityFilter = sConfigMgr->GetOption<bool>("PlayerbotWorldPvp.BotOnlyEligibilityFilter", true);
        UsePlayerbotConfig = sConfigMgr->GetOption<bool>("PlayerbotWorldPvp.UsePlayerbotConfig", true);
        BotAccountPrefix = sConfigMgr->GetOption<std::string>("PlayerbotWorldPvp.BotAccountPrefix", "auto");
        BotAccountMin = sConfigMgr->GetOption<uint32>("PlayerbotWorldPvp.BotAccountMin", 0);
        BotAccountMax = sConfigMgr->GetOption<uint32>("PlayerbotWorldPvp.BotAccountMax", 0);
        TeleportBots = sConfigMgr->GetOption<bool>("PlayerbotWorldPvp.TeleportBots", true);
        ReturnBotsAfterEvent = sConfigMgr->GetOption<bool>("PlayerbotWorldPvp.ReturnBotsAfterEvent", true);
        UseMovePointAfterRally = sConfigMgr->GetOption<bool>("PlayerbotWorldPvp.UseMovePointAfterRally", true);
        HardTeleportToTarget = sConfigMgr->GetOption<bool>("PlayerbotWorldPvp.HardTeleportToTarget", false);
        MoveFromRallyDelaySeconds = sConfigMgr->GetOption<uint32>("PlayerbotWorldPvp.MoveFromRallyDelaySeconds", 15);
        EventChancePerTick = sConfigMgr->GetOption<uint32>("PlayerbotWorldPvp.EventChancePerTick", 25);
        MaxActiveEvents = sConfigMgr->GetOption<uint32>("PlayerbotWorldPvp.MaxActiveEvents", 2);
        MaxBotsPerSide = sConfigMgr->GetOption<uint32>("PlayerbotWorldPvp.MaxBotsPerSide", 12);
        PositionJitter = sConfigMgr->GetOption<uint32>("PlayerbotWorldPvp.PositionJitter", 8);
        BotQueryLimit = sConfigMgr->GetOption<uint32>("PlayerbotWorldPvp.BotQueryLimit", 400);
        MinOnlineBotsRequired = sConfigMgr->GetOption<uint32>("PlayerbotWorldPvp.MinOnlineBotsRequired", 3);
        GlobalMinLevel = sConfigMgr->GetOption<uint32>("PlayerbotWorldPvp.GlobalMinLevel", 20);
        ReactiveRaidResponse = sConfigMgr->GetOption<bool>("PlayerbotWorldPvp.ReactiveRaidResponse.Enable", true);
        ReactiveMinGroupSize = sConfigMgr->GetOption<uint32>("PlayerbotWorldPvp.ReactiveRaidResponse.MinGroupSize", 20);
        ReactiveMaxResponders = sConfigMgr->GetOption<uint32>("PlayerbotWorldPvp.ReactiveRaidResponse.MaxResponders", 40);
        ReactiveLoginDelaySeconds = sConfigMgr->GetOption<uint32>("PlayerbotWorldPvp.ReactiveRaidResponse.LoginDelaySeconds", 20);
        ReactiveCooldownSeconds = sConfigMgr->GetOption<uint32>("PlayerbotWorldPvp.ReactiveRaidResponse.CooldownSeconds", 1800);
        ReactiveDurationMinutes = sConfigMgr->GetOption<uint32>("PlayerbotWorldPvp.ReactiveRaidResponse.DurationMinutes", 20);
        PvpCombatStrategies = sConfigMgr->GetOption<std::string>("PlayerbotWorldPvp.PvpCombatStrategies", "+pvp,+boost,+dps debuff,-passive,-stay");
        PvpNonCombatStrategies = sConfigMgr->GetOption<std::string>("PlayerbotWorldPvp.PvpNonCombatStrategies", "+pvp,+duel,+boost,-passive,-stay");
        SameFactionDuels = sConfigMgr->GetOption<bool>("PlayerbotWorldPvp.SameFactionDuels", true);
        DuelRequestDelaySeconds = sConfigMgr->GetOption<uint32>("PlayerbotWorldPvp.DuelRequestDelaySeconds", 8);
        DuelSpellId = sConfigMgr->GetOption<uint32>("PlayerbotWorldPvp.DuelSpellId", 7266);
        DuelPairLimit = sConfigMgr->GetOption<uint32>("PlayerbotWorldPvp.DuelPairLimit", 12);
        FileLog = sConfigMgr->GetOption<bool>("PlayerbotWorldPvp.FileLog", true);
        FileLogPath = sConfigMgr->GetOption<std::string>("PlayerbotWorldPvp.FileLogPath", "Logs/playerbot_world_pvp.log");

        EventChancePerTick = std::min<uint32>(EventChancePerTick, 100);
        GlobalMinLevel = std::min<uint32>(std::max<uint32>(GlobalMinLevel, 1), 80);
        MaxBotsPerSide = std::min<uint32>(std::max<uint32>(MaxBotsPerSide, 1), 40);
        ReactiveMinGroupSize = std::min<uint32>(std::max<uint32>(ReactiveMinGroupSize, 2), 40);
        ReactiveMaxResponders = std::min<uint32>(std::max<uint32>(ReactiveMaxResponders, 1), 40);
        DuelPairLimit = std::min<uint32>(std::max<uint32>(DuelPairLimit, 1), 40);
        if (TickSeconds == 0)
            TickSeconds = 60;
        if (MoveFromRallyDelaySeconds == 0)
            MoveFromRallyDelaySeconds = 1;

        ImportPlayerbotConfigDefaults();

        bool hasBotPrefix = !BotAccountPrefix.empty();
        bool hasBotRange = BotAccountMax >= BotAccountMin && BotAccountMax > 0;
        if (Enable && (!BotOnlyEligibilityFilter || (!hasBotPrefix && !hasBotRange)))
        {
            LOG_ERROR("module", "[PlayerbotWorldPvp] SAFETY: disabled. BotOnlyEligibilityFilter must be enabled with BotAccountPrefix or BotAccountMin/Max.");
            Enable = false;
        }

        if (Enable && !VerifyDatabaseSchema())
            Enable = false;

        LOG_INFO("module", "[PlayerbotWorldPvp] Enable={} tick={}s chance={} maxActive={} prefix='{}' globalMinLevel={} movePoint={} returnBots={} sameFactionDuels={} fileLog='{}'",
            Enable ? 1 : 0, TickSeconds, EventChancePerTick, MaxActiveEvents, BotAccountPrefix, GlobalMinLevel, UseMovePointAfterRally ? 1 : 0, ReturnBotsAfterEvent ? 1 : 0, SameFactionDuels ? 1 : 0, FileLogPath);
        AuditLog("CONFIG", "enabled=" + std::to_string(Enable ? 1 : 0) + " prefix=" + BotAccountPrefix + " chance=" + std::to_string(EventChancePerTick) + " globalMin=" + std::to_string(GlobalMinLevel) + " sameFactionDuels=" + std::to_string(SameFactionDuels ? 1 : 0));
        _startupElapsedMs = 0;
        _startupDelayLogged = false;
    }

    void OnUpdate(uint32 diff) override
    {
        using namespace PlayerbotWorldPvp;
        if (!Enable)
            return;

        if (StartupDelaySeconds > 0 && _startupElapsedMs < StartupDelaySeconds * IN_MILLISECONDS)
        {
            _startupElapsedMs += diff;
            if (!_startupDelayLogged)
            {
                _startupDelayLogged = true;
                LOG_INFO("module", "[PlayerbotWorldPvp] Startup delay active for {} seconds so Playerbots can finish logging in.", StartupDelaySeconds);
            }
            return;
        }

        ProcessMoveQueue();
        ProcessDuelQueue();
        ExpireEvents();
        StartPendingRaidResponses();

        if (_timerMs <= diff)
        {
            _timerMs = TickSeconds * IN_MILLISECONDS;
            TryQueueReactiveRaidResponse();
            TryStartRandomEvent();
        }
        else
            _timerMs -= diff;
    }
};

class PlayerbotWorldPvpPlayerScript : public PlayerScript
{
public:
    PlayerbotWorldPvpPlayerScript()
        : PlayerScript("PlayerbotWorldPvpPlayerScript", { PLAYERHOOK_CAN_PLAYER_USE_CHAT })
    {}

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg) override
    {
        (void)type;
        (void)language;
        if (msg.rfind(".wpvp", 0) == 0 && player && player->GetSession() && player->GetSession()->GetSecurity() >= SEC_GAMEMASTER)
        {
            ChatHandler handler(player->GetSession());
            PlayerbotWorldPvp::HandleGmCommand(&handler, msg);
            return false;
        }
        return true;
    }
};

class PlayerbotWorldPvpCommandScript : public CommandScript
{
public:
    PlayerbotWorldPvpCommandScript() : CommandScript("PlayerbotWorldPvpCommandScript") {}

    std::vector<Acore::ChatCommands::ChatCommandBuilder> GetCommands() const override
    {
        using namespace Acore::ChatCommands;
        static std::vector<ChatCommandBuilder> commandTable =
        {
            { "wpvp", HandleWpvpCommand, SEC_GAMEMASTER, Console::Yes }
        };
        return commandTable;
    }

    static bool HandleWpvpCommand(ChatHandler* handler, char const* args)
    {
        return PlayerbotWorldPvp::HandleGmCommand(handler, args ? args : "");
    }
};

void AddSC_mod_playerbot_world_pvp()
{
    new PlayerbotWorldPvpWorldScript();
    new PlayerbotWorldPvpPlayerScript();
    new PlayerbotWorldPvpCommandScript();
}

void Addmod_playerbots_world_pvpScripts()
{
    AddSC_mod_playerbot_world_pvp();
}
