-- mod-temucraft-guild (2026-11-26): retention. Re-runnable, strict-mode safe.
-- 1. `guild_time` on the chat table is used by no query (every read is WHERE guild = ? [AND id < ?] ORDER BY id,
--    served by `guild_id`; pruning goes by primary key): drop it when it is there.
SET @has := (SELECT COUNT(*) FROM information_schema.STATISTICS WHERE `TABLE_SCHEMA` = DATABASE()
             AND `TABLE_NAME` = 'mod_temucraft_guild_chat' AND `INDEX_NAME` = 'guild_time');
SET @sql := IF(@has > 0, 'ALTER TABLE `mod_temucraft_guild_chat` DROP INDEX `guild_time`', 'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- 2. rows of guilds that no longer exist (disbanded before this version); the module also does this at startup
DELETE t FROM `mod_temucraft_guild_chat` t LEFT JOIN `guild` g ON g.`guildid` = t.`guild` WHERE g.`guildid` IS NULL;
DELETE t FROM `mod_temucraft_guild_bank_log` t LEFT JOIN `guild` g ON g.`guildid` = t.`guild` WHERE g.`guildid` IS NULL;
DELETE t FROM `mod_temucraft_guild_event_log` t LEFT JOIN `guild` g ON g.`guildid` = t.`guild` WHERE g.`guildid` IS NULL;
DELETE t FROM `mod_temucraft_guild_news` t LEFT JOIN `guild` g ON g.`guildid` = t.`guild` WHERE g.`guildid` IS NULL;
