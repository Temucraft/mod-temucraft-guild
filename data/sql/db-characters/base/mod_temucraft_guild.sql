-- mod-temucraft-guild: guild chat, guild bank log, guild event log and guild news (kept Temucraft.Guild.KeepDays.* days).

-- every guild (channel 0) and officer (channel 1) chat line
CREATE TABLE IF NOT EXISTS `mod_temucraft_guild_chat` (
    `id`           BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `guild`        INT UNSIGNED NOT NULL,
    `sender`       INT UNSIGNED NOT NULL,
    `sender_name`  VARCHAR(24) NOT NULL DEFAULT '',
    `sender_class` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `channel`      TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `text`         VARCHAR(255) NOT NULL DEFAULT '',
    `time`         INT UNSIGNED NOT NULL,
    PRIMARY KEY (`id`),
    KEY `guild_id` (`guild`, `id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- every guild bank log entry (the columns of guild_bank_eventlog; tab 100 = the money log, as there)
CREATE TABLE IF NOT EXISTS `mod_temucraft_guild_bank_log` (
    `id`            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `guild`         INT UNSIGNED NOT NULL,
    `tab`           TINYINT UNSIGNED NOT NULL,
    `type`          TINYINT UNSIGNED NOT NULL,
    `player`        INT UNSIGNED NOT NULL,
    `player_name`   VARCHAR(24) NOT NULL DEFAULT '',
    `player_class`  TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `item_or_money` INT UNSIGNED NOT NULL DEFAULT 0,
    `count`         SMALLINT UNSIGNED NOT NULL DEFAULT 0,
    `dest_tab`      TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `time`          INT UNSIGNED NOT NULL,
    PRIMARY KEY (`id`),
    KEY `guild_tab_time` (`guild`, `tab`, `time`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- every guild event log entry (the columns of guild_eventlog)
CREATE TABLE IF NOT EXISTS `mod_temucraft_guild_event_log` (
    `id`       BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `guild`    INT UNSIGNED NOT NULL,
    `type`     TINYINT UNSIGNED NOT NULL,
    `player1`  INT UNSIGNED NOT NULL DEFAULT 0,
    `name1`    VARCHAR(24) NOT NULL DEFAULT '',
    `class1`   TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `player2`  INT UNSIGNED NOT NULL DEFAULT 0,
    `name2`    VARCHAR(24) NOT NULL DEFAULT '',
    `class2`   TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `new_rank` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `time`     INT UNSIGNED NOT NULL,
    PRIMARY KEY (`id`),
    KEY `guild_time` (`guild`, `time`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- guild news: a achievement (value = id), i item looted / c item made (value = item entry), j joined, l left,
-- k removed (extra = by whom), p promoted / d demoted (value = the new rank, extra = by whom)
CREATE TABLE IF NOT EXISTS `mod_temucraft_guild_news` (
    `id`           BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `guild`        INT UNSIGNED NOT NULL,
    `kind`         CHAR(1) NOT NULL,
    `player`       INT UNSIGNED NOT NULL DEFAULT 0,
    `player_name`  VARCHAR(24) NOT NULL DEFAULT '',
    `player_class` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `value`        INT UNSIGNED NOT NULL DEFAULT 0,
    `extra`        VARCHAR(64) NOT NULL DEFAULT '',
    `time`         INT UNSIGNED NOT NULL,
    PRIMARY KEY (`id`),
    KEY `guild_time` (`guild`, `time`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
