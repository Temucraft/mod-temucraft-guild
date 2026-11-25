# mod-temucraft-guild

The server half of the Temucraft guild window and guild bank (the client half is `TemucraftGuild.lua` and
`TemucraftGuildBank.lua` in [mod-temucraft-client-version](https://github.com/Temucraft/mod-temucraft-client-version)).
It keeps, in the characters DB and forever:

- **Guild chat**: every guild and officer chat line players write (`mod_temucraft_guild_chat`). The guild window
  shows the history with timestamps (pages of `PageSize` lines, "Load older messages" for more); after logging in the
  last `LoginLines` lines are written to the chat window. Officer lines only go to members whose rank may listen to
  officer chat.
- **Guild bank logs**: the core keeps only the last 25 entries per bank tab (`guild_bank_eventlog`) and of the guild
  log (`guild_eventlog`). Every new entry is copied into `mod_temucraft_guild_bank_log` /
  `mod_temucraft_guild_event_log`, and at startup the stock tables' entries the archive doesn't have yet are copied
  too (so the archive starts with what the guild had). The guild bank's Log and Money Log and the guild window's
  guild log show all of it, page by page; a bank tab's log only to members who may view that tab.
- **Guild news** (`mod_temucraft_guild_news`): achievements, epic items looted or made, members joining, leaving,
  removed, promoted and demoted.
- **Perks**: if [mod-temucraft-guild-perks](https://github.com/Temucraft/mod-temucraft-guild-perks) is installed, the
  guild window's Perks tab shows its perks and their state for the guild (read from its tables; that module still
  decides).

Inserts are asynchronous and the reads are asynchronous queries answered on the player's session, so nothing waits
on the database on the world thread.

## Command

`.temucraft guildchat <guild name or id> [lines]` (20 lines by default, at most 200) shows a guild's stored chat,
officer lines included. RBAC permission 104251, linked to the administrator roles 192 and 1014; give it to others
with `.rbac account grant <account> 104251`.

## Install

1. Put this folder in `modules/` of your AzerothCore source, re-run CMake and build.
2. The SQL in `data/sql/` is applied by the worldserver on its next start: the four tables (characters), the RBAC
   permission (auth) and the command's texts (world, `module_string`).
3. Copy `conf/mod-temucraft-guild.conf.dist` to `configs/modules/` as `mod-temucraft-guild.conf` to change the
   settings (`.reload config` applies them):

| Setting | Default | |
|---|---|---|
| `Temucraft.Guild.ChatHistory.Enable` | 1 | keep and send the guild chat |
| `Temucraft.Guild.ChatHistory.LoginLines` | 10 | lines sent after login (0 = none, at most 50) |
| `Temucraft.Guild.ChatHistory.PageSize` | 50 | lines / log entries / news per page (5-200) |
| `Temucraft.Guild.BankLogArchive.Enable` | 1 | archive the bank and guild logs and send them |
| `Temucraft.Guild.News.Enable` | 1 | guild news |
| `Temucraft.Guild.News.Achievements` / `.Items` | 1 | achievements / items are news |
| `Temucraft.Guild.News.MinItemQuality` | 4 | lowest item quality that is news (heirlooms never) |

## The client's messages

Addon messages with the prefix `TGD`, whispered to oneself (`AddonChannel = 1`, the default). The client asks
`HELLO`, `HIST~beforeId`, `BLOG~tab~offset` (tab 1-6, 7 = the money log), `NEWS~offset`, `ELOG~offset` and `PERKS`;
the answers are described at the top of `src/TemucraftGuild.cpp`. Ages are sent as seconds before now, so a wrong
clock on the player's computer doesn't matter.

## Limits

- Guild chat that doesn't go through the chat handler isn't seen: playerbots' own guild chat
  (`Guild::BroadcastToGuild` directly) is not stored, nor are addon messages.
- The startup copy of the stock logs matches entries on guild, tab, time, type, player, item and count: two
  identical entries in the same second before this module was installed are copied once.
- Nothing is ever deleted, also not when a guild disbands; delete rows by `guild` by hand if you need to.

## Independence

Works on its own; uses mod-temucraft-guild-perks' tables only when they exist.
