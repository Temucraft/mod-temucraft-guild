/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * mod-temucraft-guild: what the retail-style guild window and guild bank of the Temucraft client show, kept forever.
 *
 *   - Guild chat history: every guild and officer chat line (players' own, not addon messages) goes into
 *     mod_temucraft_guild_chat (characters DB) and stays there. The client asks for it when the guild window opens
 *     (pages of PageSize lines, older pages on request) and gets the last LoginLines lines after logging in. Officer
 *     lines only go to members whose rank may listen to officer chat, guild lines only to those who may listen to
 *     guild chat.
 *   - Guild bank and guild event logs: the core keeps only the last 25 entries per bank tab (guild_bank_eventlog) and
 *     of the guild's own log (guild_eventlog). Every new entry is copied into mod_temucraft_guild_bank_log and
 *     mod_temucraft_guild_event_log; at startup the entries the stock tables have and the archive doesn't (from
 *     before this module, or while it was off) are copied too. The client asks for pages of them.
 *   - Guild news: achievements, epic items looted or made, members joining, leaving, removed, promoted and demoted,
 *     in mod_temucraft_guild_news.
 *   - Perks: the guild's perks from mod-temucraft-guild-perks' tables when they exist (read only: that module stays
 *     the one that decides).
 *
 * Addon messages (prefix TGD, see TemucraftAddonMessage.h). Client asks:
 *   HELLO                      -> S~chat~bank~news~perks~pageSize, then the login lines: H... and HE~L~count~more
 *   HIST~beforeId              -> H~id~channel(g|o)~age~class~name~text ... (oldest first), HE~P~count~more
 *   BLOG~tab~offset            -> L~id~age~type~tab~class~name~itemOrMoney~count~destTab~quality~itemName ...
 *                                 (newest first; tab 1-6, 7 = the money log), LE~tab~offset~count~more
 *   NEWS~offset                -> N~age~kind~class~name~value~quality~text ..., NE~offset~count~more
 *   ELOG~offset                -> E~age~type~class1~name1~class2~name2~rank ..., EE~offset~count~more
 *   PERKS                      -> K~id~state(1 on, 0 off, l locked, x not on this server)~spell~name ..., KE~count
 * Ages are seconds before now (the client's clock may be off). A record longer than one message goes as "+piece"
 * messages and a last "=piece".
 *
 * GM command: .temucraft guildchat <guild name or id> [lines] (RBAC 104251) shows a guild's stored chat, officer
 * lines included.
 *
 * Nothing here blocks the world thread: inserts are asynchronous Executes, reads are asynchronous queries answered
 * through the session's query processor.
 */
#include "CharacterCache.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "GuildScript.h"
#include "Item.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "TemucraftAddonMessage.h"
#include "TemucraftModuleText.h"
#include "Timer.h"
#include "WorldScript.h"
#include "WorldSession.h"
#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    constexpr char const* MODULE = "mod-temucraft-guild";
    constexpr char const* PREFIX = "TGD";
    constexpr size_t PIECE = 200;
    constexpr uint32 MONEY_TAB = 100;   // the money log's tab in the stock table (GUILD_BANK_MONEY_LOGS_TAB)

    struct Settings
    {
        bool chat = true;
        uint32 loginLines = 10;
        uint32 pageSize = 50;
        bool bankArchive = true;
        bool news = true;
        bool newsAchievements = true;
        bool newsItems = true;
        uint32 newsMinQuality = ITEM_QUALITY_EPIC;
    } cfg;

    // mod-temucraft-guild-perks' perk list (world DB), read at startup when the table exists
    struct PerkDef { std::string name; uint32 spell = 0; bool allowed = true, unlocked = true, enabled = true; };
    std::map<uint32, PerkDef> perks;
    bool perkStateTable = false;

    enum Text : uint32 { T_USAGE = 1, T_NO_GUILD, T_HEAD, T_LINE, T_NONE, T_OFFICER, T_GUILD };
    char const* Say(ChatHandler* handler, uint32 id, char const* fallback) { return TemucraftModuleText(MODULE, handler, id, fallback); }

    uint32 Now() { return uint32(GameTime::GetGameTime().count()); }
    uint32 Age(uint32 time) { uint32 now = Now(); return now > time ? now - time : 0; }

    std::string Escaped(std::string text)
    {
        CharacterDatabase.EscapeString(text);
        return text;
    }

    // one record, in pieces when it is too long for one addon message
    void Send(Player* player, std::string const& record)
    {
        if (record.size() <= PIECE + 30)
        {
            TemucraftAddonSend(player, PREFIX, record);
            return;
        }
        size_t at = 0;
        for (; at + PIECE < record.size(); at += PIECE)
            TemucraftAddonSend(player, PREFIX, "+" + record.substr(at, PIECE));
        TemucraftAddonSend(player, PREFIX, "=" + record.substr(at));
    }

    void LoadSettings()
    {
        cfg.chat = sConfigMgr->GetOption<bool>("Temucraft.Guild.ChatHistory.Enable", true);
        cfg.loginLines = std::min<uint32>(sConfigMgr->GetOption<uint32>("Temucraft.Guild.ChatHistory.LoginLines", 10), 50);
        cfg.pageSize = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Temucraft.Guild.ChatHistory.PageSize", 50), 5, 200);
        cfg.bankArchive = sConfigMgr->GetOption<bool>("Temucraft.Guild.BankLogArchive.Enable", true);
        cfg.news = sConfigMgr->GetOption<bool>("Temucraft.Guild.News.Enable", true);
        cfg.newsAchievements = sConfigMgr->GetOption<bool>("Temucraft.Guild.News.Achievements", true);
        cfg.newsItems = sConfigMgr->GetOption<bool>("Temucraft.Guild.News.Items", true);
        cfg.newsMinQuality = sConfigMgr->GetOption<uint32>("Temucraft.Guild.News.MinItemQuality", ITEM_QUALITY_EPIC);
    }

    void LoadPerks()
    {
        perks.clear();
        perkStateTable = false;
        if (!WorldDatabase.Query("SHOW TABLES LIKE 'mod_temucraft_guild_perk'"))
            return;
        if (QueryResult r = WorldDatabase.Query("SELECT `id`, `name`, `spell`, `allowed`, `unlocked`, `enabled` FROM `mod_temucraft_guild_perk`"))
            do
            {
                Field* f = r->Fetch();
                PerkDef d;
                d.name = TemucraftAddonClean(f[1].Get<std::string>());
                d.spell = f[2].Get<uint32>();
                d.allowed = f[3].Get<uint8>() != 0;
                d.unlocked = f[4].Get<uint8>() != 0;
                d.enabled = f[5].Get<uint8>() != 0;
                perks[f[0].Get<uint32>()] = d;
            } while (r->NextRow());
        perkStateTable = CharacterDatabase.Query("SHOW TABLES LIKE 'mod_temucraft_guild_perk_state'") != nullptr;
    }

    // the stock logs' entries the archive doesn't have yet (from before this module, or while it was off)
    void SeedArchives()
    {
        if (!cfg.bankArchive)
            return;
        CharacterDatabase.Execute(
            "INSERT INTO `mod_temucraft_guild_bank_log` (`guild`, `tab`, `type`, `player`, `player_name`, `player_class`, `item_or_money`, `count`, `dest_tab`, `time`) "
            "SELECT g.`guildid`, g.`TabId`, g.`EventType`, g.`PlayerGuid`, IFNULL(c.`name`, ''), IFNULL(c.`class`, 0), g.`ItemOrMoney`, g.`ItemStackCount`, g.`DestTabId`, g.`TimeStamp` "
            "FROM `guild_bank_eventlog` g LEFT JOIN `characters` c ON c.`guid` = g.`PlayerGuid` "
            "WHERE NOT EXISTS (SELECT 1 FROM `mod_temucraft_guild_bank_log` a WHERE a.`guild` = g.`guildid` AND a.`tab` = g.`TabId` "
            "AND a.`time` = g.`TimeStamp` AND a.`type` = g.`EventType` AND a.`player` = g.`PlayerGuid` AND a.`item_or_money` = g.`ItemOrMoney` "
            "AND a.`count` = g.`ItemStackCount`) ORDER BY g.`TimeStamp`, g.`LogGuid`");
        CharacterDatabase.Execute(
            "INSERT INTO `mod_temucraft_guild_event_log` (`guild`, `type`, `player1`, `name1`, `class1`, `player2`, `name2`, `class2`, `new_rank`, `time`) "
            "SELECT g.`guildid`, g.`EventType`, g.`PlayerGuid1`, IFNULL(c1.`name`, ''), IFNULL(c1.`class`, 0), g.`PlayerGuid2`, IFNULL(c2.`name`, ''), "
            "IFNULL(c2.`class`, 0), g.`NewRank`, g.`TimeStamp` "
            "FROM `guild_eventlog` g LEFT JOIN `characters` c1 ON c1.`guid` = g.`PlayerGuid1` LEFT JOIN `characters` c2 ON c2.`guid` = g.`PlayerGuid2` "
            "WHERE NOT EXISTS (SELECT 1 FROM `mod_temucraft_guild_event_log` a WHERE a.`guild` = g.`guildid` AND a.`time` = g.`TimeStamp` "
            "AND a.`type` = g.`EventType` AND a.`player1` = g.`PlayerGuid1` AND a.`player2` = g.`PlayerGuid2`) ORDER BY g.`TimeStamp`, g.`LogGuid`");
    }

    // a character's name and class from the cache (the player may be offline or deleted)
    void NameAndClass(ObjectGuid::LowType guid, std::string& name, uint8& cls)
    {
        name.clear();
        cls = 0;
        if (!guid)
            return;
        if (CharacterCacheEntry const* c = sCharacterCache->GetCharacterCacheByGuid(ObjectGuid::Create<HighGuid::Player>(guid)))
        {
            name = c->Name;
            cls = c->Class;
        }
    }

    void AddNews(uint32 guild, char kind, ObjectGuid::LowType player, std::string const& name, uint8 cls, uint32 value, std::string const& extra = "")
    {
        if (!cfg.news || !guild)
            return;
        CharacterDatabase.Execute("INSERT INTO `mod_temucraft_guild_news` (`guild`, `kind`, `player`, `player_name`, `player_class`, `value`, `extra`, `time`) "
            "VALUES ({}, '{}', {}, '{}', {}, {}, '{}', {})", guild, std::string(1, kind), player, Escaped(name), cls, value, Escaped(extra), Now());
    }

    // an asynchronous read answered on the session's thread, while the player is still logged in
    void Read(WorldSession* session, std::string const& sql, std::function<void(Player*, QueryResult)> answer)
    {
        session->GetQueryProcessor().AddCallback(CharacterDatabase.AsyncQuery(sql).WithCallback([session, answer](QueryResult r)
        {
            if (Player* player = session->GetPlayer())
                answer(player, r);
        }));
    }

    std::string Fields(std::initializer_list<std::string> parts)
    {
        std::string out;
        for (std::string const& p : parts)
        {
            if (!out.empty())
                out += '~';
            out += p;
        }
        return out;
    }

    // chat history: `limit` lines older than `beforeId` (0 = the newest), sent oldest first
    void SendHistory(Player* player, uint32 beforeId, uint32 limit, char const* tag)
    {
        Guild* guild = player->GetGuild();
        if (!guild || !cfg.chat || !limit)
        {
            TemucraftAddonSend(player, PREFIX, Fields({ "HE", tag, "0", "0" }));
            return;
        }
        bool const guildLines = guild->HasRankRight(player, GR_RIGHT_GCHATLISTEN);
        bool const officerLines = guild->HasRankRight(player, GR_RIGHT_OFFCHATLISTEN);
        if (!guildLines && !officerLines)
        {
            TemucraftAddonSend(player, PREFIX, Fields({ "HE", tag, "0", "0" }));
            return;
        }
        std::string where = Acore::StringFormat("`guild` = {}", guild->GetId());
        if (!guildLines)
            where += " AND `channel` = 1";
        else if (!officerLines)
            where += " AND `channel` = 0";
        if (beforeId)
            where += Acore::StringFormat(" AND `id` < {}", beforeId);
        std::string const sql = Acore::StringFormat("SELECT `id`, `channel`, `time`, `sender_class`, `sender_name`, `text` FROM `mod_temucraft_guild_chat` "
            "WHERE {} ORDER BY `id` DESC LIMIT {}", where, limit + 1);
        std::string const what = tag;
        Read(player->GetSession(), sql, [limit, what](Player* p, QueryResult r)
        {
            std::vector<std::string> lines;
            bool more = false;
            if (r)
                do
                {
                    if (lines.size() == limit)
                    {
                        more = true;
                        break;
                    }
                    Field* f = r->Fetch();
                    lines.push_back(Fields({ "H", std::to_string(f[0].Get<uint64>()), f[1].Get<uint8>() ? "o" : "g", std::to_string(Age(f[2].Get<uint32>())),
                        std::to_string(f[3].Get<uint8>()), TemucraftAddonClean(f[4].Get<std::string>()), f[5].Get<std::string>() }));
                } while (r->NextRow());
            for (auto it = lines.rbegin(); it != lines.rend(); ++it)
                Send(p, *it);
            TemucraftAddonSend(p, PREFIX, Fields({ "HE", what, std::to_string(lines.size()), more ? "1" : "0" }));
        });
    }

    // a page of a bank tab's log (clientTab 1-6) or of the money log (7), newest first
    void SendBankLog(Player* player, uint32 clientTab, uint32 offset)
    {
        Guild* guild = player->GetGuild();
        std::string const head = std::to_string(clientTab), off = std::to_string(offset);
        if (!guild || !cfg.bankArchive || clientTab < 1 || clientTab > uint32(GUILD_BANK_MAX_TABS) + 1)
        {
            TemucraftAddonSend(player, PREFIX, Fields({ "LE", head, off, "0", "0" }));
            return;
        }
        uint32 const tab = clientTab == uint32(GUILD_BANK_MAX_TABS) + 1 ? MONEY_TAB : clientTab - 1;
        if (tab != MONEY_TAB && !guild->MemberHasTabRights(player->GetGUID(), uint8(tab), GUILD_BANK_RIGHT_VIEW_TAB))
        {
            TemucraftAddonSend(player, PREFIX, Fields({ "LE", head, off, "0", "0" }));
            return;
        }
        uint32 const limit = cfg.pageSize;
        std::string const sql = Acore::StringFormat("SELECT `id`, `time`, `type`, `tab`, `player_class`, `player_name`, `item_or_money`, `count`, `dest_tab`, `player` "
            "FROM `mod_temucraft_guild_bank_log` WHERE `guild` = {} AND `tab` = {} ORDER BY `time` DESC, `id` DESC LIMIT {}, {}",
            guild->GetId(), tab, offset, limit + 1);
        Read(player->GetSession(), sql, [limit, head, off](Player* p, QueryResult r)
        {
            uint32 count = 0;
            bool more = false;
            if (r)
                do
                {
                    if (count == limit)
                    {
                        more = true;
                        break;
                    }
                    Field* f = r->Fetch();
                    uint8 const type = f[2].Get<uint8>();
                    uint32 const itemOrMoney = f[6].Get<uint32>();
                    std::string name = f[5].Get<std::string>();
                    uint8 cls = f[4].Get<uint8>();
                    if (name.empty())
                        NameAndClass(f[9].Get<uint32>(), name, cls);
                    std::string quality = "0", itemName;
                    bool const money = type == GUILD_BANK_LOG_DEPOSIT_MONEY || type == GUILD_BANK_LOG_WITHDRAW_MONEY
                        || type == GUILD_BANK_LOG_REPAIR_MONEY || type == GUILD_BANK_LOG_UNK1 || type == GUILD_BANK_LOG_BUY_SLOT;
                    if (!money)
                        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemOrMoney))
                        {
                            quality = std::to_string(proto->Quality);
                            itemName = TemucraftAddonClean(proto->Name1);
                        }
                    uint32 tab = f[3].Get<uint8>();
                    Send(p, Fields({ "L", std::to_string(f[0].Get<uint64>()), std::to_string(Age(f[1].Get<uint32>())), std::to_string(type),
                        std::to_string(tab == MONEY_TAB ? uint32(GUILD_BANK_MAX_TABS) + 1 : tab + 1), std::to_string(cls), TemucraftAddonClean(name),
                        std::to_string(itemOrMoney), std::to_string(f[7].Get<uint16>()), std::to_string(uint32(f[8].Get<uint8>()) + 1), quality, itemName }));
                    ++count;
                } while (r->NextRow());
            TemucraftAddonSend(p, PREFIX, Fields({ "LE", head, off, std::to_string(count), more ? "1" : "0" }));
        });
    }

    void SendNews(Player* player, uint32 offset)
    {
        Guild* guild = player->GetGuild();
        std::string const off = std::to_string(offset);
        if (!guild || !cfg.news)
        {
            TemucraftAddonSend(player, PREFIX, Fields({ "NE", off, "0", "0" }));
            return;
        }
        uint32 const limit = cfg.pageSize;
        std::string const sql = Acore::StringFormat("SELECT `time`, `kind`, `player_class`, `player_name`, `value`, `extra` FROM `mod_temucraft_guild_news` "
            "WHERE `guild` = {} ORDER BY `time` DESC, `id` DESC LIMIT {}, {}", guild->GetId(), offset, limit + 1);
        Read(player->GetSession(), sql, [limit, off](Player* p, QueryResult r)
        {
            uint32 count = 0;
            bool more = false;
            if (r)
                do
                {
                    if (count == limit)
                    {
                        more = true;
                        break;
                    }
                    Field* f = r->Fetch();
                    std::string const kind = f[1].Get<std::string>();
                    uint32 const value = f[4].Get<uint32>();
                    std::string quality = "0", text = TemucraftAddonClean(f[5].Get<std::string>());
                    if (kind == "i" || kind == "c")
                        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(value))
                        {
                            quality = std::to_string(proto->Quality);
                            text = TemucraftAddonClean(proto->Name1);
                        }
                    Send(p, Fields({ "N", std::to_string(Age(f[0].Get<uint32>())), kind, std::to_string(f[2].Get<uint8>()),
                        TemucraftAddonClean(f[3].Get<std::string>()), std::to_string(value), quality, text }));
                    ++count;
                } while (r->NextRow());
            TemucraftAddonSend(p, PREFIX, Fields({ "NE", off, std::to_string(count), more ? "1" : "0" }));
        });
    }

    void SendEventLog(Player* player, uint32 offset)
    {
        Guild* guild = player->GetGuild();
        std::string const off = std::to_string(offset);
        if (!guild || !cfg.bankArchive)
        {
            TemucraftAddonSend(player, PREFIX, Fields({ "EE", off, "0", "0" }));
            return;
        }
        uint32 const limit = cfg.pageSize;
        std::string const sql = Acore::StringFormat("SELECT `time`, `type`, `class1`, `name1`, `class2`, `name2`, `new_rank` FROM `mod_temucraft_guild_event_log` "
            "WHERE `guild` = {} ORDER BY `time` DESC, `id` DESC LIMIT {}, {}", guild->GetId(), offset, limit + 1);
        Read(player->GetSession(), sql, [limit, off](Player* p, QueryResult r)
        {
            uint32 count = 0;
            bool more = false;
            if (r)
                do
                {
                    if (count == limit)
                    {
                        more = true;
                        break;
                    }
                    Field* f = r->Fetch();
                    Send(p, Fields({ "E", std::to_string(Age(f[0].Get<uint32>())), std::to_string(f[1].Get<uint8>()), std::to_string(f[2].Get<uint8>()),
                        TemucraftAddonClean(f[3].Get<std::string>()), std::to_string(f[4].Get<uint8>()), TemucraftAddonClean(f[5].Get<std::string>()),
                        std::to_string(f[6].Get<uint8>()) }));
                    ++count;
                } while (r->NextRow());
            TemucraftAddonSend(p, PREFIX, Fields({ "EE", off, std::to_string(count), more ? "1" : "0" }));
        });
    }

    void SendPerks(Player* player)
    {
        Guild* guild = player->GetGuild();
        if (!guild || perks.empty())
        {
            TemucraftAddonSend(player, PREFIX, "KE~0");
            return;
        }
        auto answer = [](Player* p, QueryResult r)
        {
            std::map<uint32, std::pair<int, int>> state;   // perk -> (unlocked, enabled), -1 = the perk's default
            if (r)
                do
                {
                    Field* f = r->Fetch();
                    state[f[0].Get<uint32>()] = { f[1].IsNull() ? -1 : int(f[1].Get<uint8>()), f[2].IsNull() ? -1 : int(f[2].Get<uint8>()) };
                } while (r->NextRow());
            for (auto const& [id, d] : perks)
            {
                auto s = state.find(id);
                bool const unlocked = (s != state.end() && s->second.first >= 0) ? s->second.first != 0 : d.unlocked;
                bool const enabled = (s != state.end() && s->second.second >= 0) ? s->second.second != 0 : d.enabled;
                char const* st = !d.allowed ? "x" : !unlocked ? "l" : enabled ? "1" : "0";
                Send(p, Fields({ "K", std::to_string(id), st, std::to_string(d.spell), d.name }));
            }
            TemucraftAddonSend(p, PREFIX, "KE~" + std::to_string(perks.size()));
        };
        if (!perkStateTable)
        {
            answer(player, nullptr);
            return;
        }
        Read(player->GetSession(), Acore::StringFormat("SELECT `perk`, `unlocked`, `enabled` FROM `mod_temucraft_guild_perk_state` WHERE `guild` = {}",
            guild->GetId()), answer);
    }

    uint32 Number(std::vector<std::string> const& f, size_t i)
    {
        if (i >= f.size())
            return 0;
        return Acore::StringTo<uint32>(f[i]).value_or(0);
    }
}

class TemucraftGuild_Player : public PlayerScript
{
public:
    TemucraftGuild_Player() : PlayerScript("TemucraftGuild_Player", {
        PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT, PLAYERHOOK_CAN_PLAYER_USE_GUILD_CHAT, PLAYERHOOK_ON_ACHI_COMPLETE,
        PLAYERHOOK_ON_LOOT_ITEM, PLAYERHOOK_ON_CREATE_ITEM }) { }

    // the client's requests
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg, Player* receiver) override
    {
        std::string body;
        if (!TemucraftAddonParse(player, type, language, msg, receiver, PREFIX, body))
            return true;
        std::vector<std::string> f = TemucraftAddonFields(body);
        if (f.empty())
            return false;
        if (f[0] == "HELLO")
        {
            TemucraftAddonSend(player, PREFIX, Fields({ "S", cfg.chat ? "1" : "0", cfg.bankArchive ? "1" : "0", cfg.news ? "1" : "0",
                perks.empty() ? "0" : "1", std::to_string(cfg.pageSize) }));
            SendHistory(player, 0, cfg.loginLines, "L");
        }
        else if (f[0] == "HIST")
            SendHistory(player, Number(f, 1), cfg.pageSize, "P");
        else if (f[0] == "BLOG")
            SendBankLog(player, Number(f, 1), Number(f, 2));
        else if (f[0] == "NEWS")
            SendNews(player, Number(f, 1));
        else if (f[0] == "ELOG")
            SendEventLog(player, Number(f, 1));
        else if (f[0] == "PERKS")
            SendPerks(player);
        return false;
    }

    // guild and officer chat, kept forever (only what the core will broadcast: the speaker may speak there)
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg, Guild* guild) override
    {
        if (!cfg.chat || !player || !guild || language == LANG_ADDON || msg.empty())
            return true;
        if (type != CHAT_MSG_GUILD && type != CHAT_MSG_OFFICER)
            return true;
        bool const officer = type == CHAT_MSG_OFFICER;
        if (!guild->HasRankRight(player, officer ? GR_RIGHT_OFFCHATSPEAK : GR_RIGHT_GCHATSPEAK))
            return true;
        CharacterDatabase.Execute("INSERT INTO `mod_temucraft_guild_chat` (`guild`, `sender`, `sender_name`, `sender_class`, `channel`, `text`, `time`) "
            "VALUES ({}, {}, '{}', {}, {}, '{}', {})", guild->GetId(), player->GetGUID().GetCounter(), Escaped(player->GetName()),
            player->getClass(), officer ? 1 : 0, Escaped(msg), Now());
        return true;
    }

    void OnPlayerAchievementComplete(Player* player, AchievementEntry const* achievement) override
    {
        if (!cfg.news || !cfg.newsAchievements || !player || !achievement || !player->GetGuildId())
            return;
        if (achievement->flags & (ACHIEVEMENT_FLAG_COUNTER | ACHIEVEMENT_FLAG_HIDDEN))
            return;
        AddNews(player->GetGuildId(), 'a', player->GetGUID().GetCounter(), player->GetName(), player->getClass(), achievement->ID);
    }

    void OnPlayerLootItem(Player* player, Item* item, uint32 /*count*/, ObjectGuid /*lootguid*/) override { ItemNews(player, item, 'i'); }
    void OnPlayerCreateItem(Player* player, Item* item, uint32 /*count*/) override { ItemNews(player, item, 'c'); }

private:
    static void ItemNews(Player* player, Item* item, char kind)
    {
        if (!cfg.news || !cfg.newsItems || !player || !item || !player->GetGuildId())
            return;
        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || proto->Quality < cfg.newsMinQuality || proto->Quality >= ITEM_QUALITY_HEIRLOOM)
            return;
        AddNews(player->GetGuildId(), kind, player->GetGUID().GetCounter(), player->GetName(), player->getClass(), proto->ItemId);
    }
};

class TemucraftGuild_Guild : public GuildScript
{
public:
    TemucraftGuild_Guild() : GuildScript("TemucraftGuild_Guild", { GUILDHOOK_ON_EVENT, GUILDHOOK_ON_BANK_EVENT }) { }

    // every bank log entry the core writes (it keeps the last 25 per tab), into the archive
    void OnBankEvent(Guild* guild, uint8 eventType, uint8 tabId, ObjectGuid::LowType playerGuid, uint32 itemOrMoney, uint16 itemStackCount, uint8 destTabId) override
    {
        if (!cfg.bankArchive || !guild)
            return;
        bool const money = eventType == GUILD_BANK_LOG_DEPOSIT_MONEY || eventType == GUILD_BANK_LOG_WITHDRAW_MONEY || eventType == GUILD_BANK_LOG_REPAIR_MONEY;
        uint32 const tab = money ? MONEY_TAB : tabId;
        std::string name;
        uint8 cls = 0;
        NameAndClass(playerGuid, name, cls);
        CharacterDatabase.Execute("INSERT INTO `mod_temucraft_guild_bank_log` (`guild`, `tab`, `type`, `player`, `player_name`, `player_class`, `item_or_money`, `count`, `dest_tab`, `time`) "
            "VALUES ({}, {}, {}, {}, '{}', {}, {}, {}, {}, {})", guild->GetId(), tab, eventType, playerGuid, Escaped(name), cls, itemOrMoney,
            itemStackCount, destTabId, Now());
    }

    // the guild's own log (invites, joins, promotions, ...), into the archive and the news
    void OnEvent(Guild* guild, uint8 eventType, ObjectGuid::LowType playerGuid1, ObjectGuid::LowType playerGuid2, uint8 newRank) override
    {
        if (!guild)
            return;
        std::string name1, name2;
        uint8 class1 = 0, class2 = 0;
        NameAndClass(playerGuid1, name1, class1);
        NameAndClass(playerGuid2, name2, class2);
        if (cfg.bankArchive)
            CharacterDatabase.Execute("INSERT INTO `mod_temucraft_guild_event_log` (`guild`, `type`, `player1`, `name1`, `class1`, `player2`, `name2`, `class2`, `new_rank`, `time`) "
                "VALUES ({}, {}, {}, '{}', {}, {}, '{}', {}, {}, {})", guild->GetId(), eventType, playerGuid1, Escaped(name1), class1, playerGuid2,
                Escaped(name2), class2, newRank, Now());
        switch (eventType)
        {
            case GUILD_EVENT_LOG_JOIN_GUILD:      AddNews(guild->GetId(), 'j', playerGuid1, name1, class1, 0); break;
            case GUILD_EVENT_LOG_LEAVE_GUILD:     AddNews(guild->GetId(), 'l', playerGuid1, name1, class1, 0); break;
            case GUILD_EVENT_LOG_UNINVITE_PLAYER: AddNews(guild->GetId(), 'k', playerGuid2, name2, class2, 0, name1); break;
            case GUILD_EVENT_LOG_PROMOTE_PLAYER:  AddNews(guild->GetId(), 'p', playerGuid2, name2, class2, newRank, name1); break;
            case GUILD_EVENT_LOG_DEMOTE_PLAYER:   AddNews(guild->GetId(), 'd', playerGuid2, name2, class2, newRank, name1); break;
            default: break;
        }
    }
};

class TemucraftGuild_World : public WorldScript
{
public:
    TemucraftGuild_World() : WorldScript("TemucraftGuild_World", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        LoadSettings();
        if (reload)
            LoadPerks();
    }

    void OnStartup() override
    {
        LoadPerks();
        SeedArchives();
        LOG_INFO("server.loading", "{}: chat history {}, bank log archive {}, {} guild perks shown.", MODULE,
            cfg.chat ? "on" : "off", cfg.bankArchive ? "on" : "off", perks.size());
    }
};

class TemucraftGuild_Commands : public CommandScript
{
public:
    TemucraftGuild_Commands() : CommandScript("TemucraftGuild_Commands") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable root =
        {
            { "temucraft guildchat", HandleGuildChat, 104251, Console::No },
        };
        return root;
    }

    // .temucraft guildchat <guild name or id> [lines]
    static bool HandleGuildChat(ChatHandler* handler, Tail text)
    {
        std::string args = std::string(text);
        while (!args.empty() && args.back() == ' ')
            args.pop_back();
        uint32 lines = 20;
        if (size_t space = args.rfind(' '); space != std::string::npos)
            if (auto n = Acore::StringTo<uint32>(args.substr(space + 1)))
            {
                lines = std::clamp<uint32>(*n, 1, 200);
                args = args.substr(0, space);
            }
        if (args.empty())
        {
            handler->SendSysMessage(Say(handler, T_USAGE, "Usage: .temucraft guildchat <guild name or id> [lines]"));
            return true;
        }
        Guild* guild = sGuildMgr->GetGuildByName(args);
        if (!guild)
            if (auto id = Acore::StringTo<uint32>(args))
                guild = sGuildMgr->GetGuildById(*id);
        if (!guild)
        {
            handler->SendSysMessage(Say(handler, T_NO_GUILD, "No such guild."));
            return true;
        }
        WorldSession* session = handler->GetSession();
        if (!session)
            return false;
        handler->PSendSysMessage(Say(handler, T_HEAD, "|cff40ff40Guild chat of {}|r (last {} lines):"), guild->GetName(), lines);
        std::string const officerTag = Say(handler, T_OFFICER, "|cff40c040[Officer]|r"), guildTag = Say(handler, T_GUILD, "|cff40ff40[Guild]|r");
        std::string const lineFormat = Say(handler, T_LINE, "[{}] {} {}: {}"), none = Say(handler, T_NONE, "Nothing stored yet.");
        Read(session, Acore::StringFormat("SELECT `time`, `channel`, `sender_name`, `text` FROM `mod_temucraft_guild_chat` WHERE `guild` = {} "
            "ORDER BY `id` DESC LIMIT {}", guild->GetId(), lines), [officerTag, guildTag, lineFormat, none](Player* p, QueryResult r)
        {
            ChatHandler out(p->GetSession());
            if (!r)
            {
                out.SendSysMessage(none);
                return;
            }
            std::vector<std::string> rows;
            do
            {
                Field* f = r->Fetch();
                rows.push_back(Acore::StringFormat(lineFormat, Acore::Time::TimeToTimestampStr(Seconds(f[0].Get<uint32>()), "%Y-%m-%d %H:%M"),
                    f[1].Get<uint8>() ? officerTag : guildTag, f[2].Get<std::string>(), f[3].Get<std::string>()));
            } while (r->NextRow());
            for (auto it = rows.rbegin(); it != rows.rend(); ++it)
                out.SendSysMessage(*it);
        });
        return true;
    }
};

void AddTemucraftGuildScripts()
{
    new TemucraftGuild_Player();
    new TemucraftGuild_Guild();
    new TemucraftGuild_World();
    new TemucraftGuild_Commands();
}
