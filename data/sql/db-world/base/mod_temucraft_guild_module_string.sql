-- mod-temucraft-guild: texts of .temucraft guildchat. Translations go in module_string_locale (same module and id, plus the locale).
DELETE FROM `module_string` WHERE `module` = 'mod-temucraft-guild';
INSERT INTO `module_string` (`module`, `id`, `string`) VALUES
('mod-temucraft-guild', 1, 'Usage: .temucraft guildchat <guild name or id> [lines]'),
('mod-temucraft-guild', 2, 'No such guild.'),
('mod-temucraft-guild', 3, '|cff40ff40Guild chat of {}|r (last {} lines):'),
('mod-temucraft-guild', 4, '[{}] {} {}: {}'),
('mod-temucraft-guild', 5, 'Nothing stored yet.'),
('mod-temucraft-guild', 6, '|cff40c040[Officer]|r'),
('mod-temucraft-guild', 7, '|cff40ff40[Guild]|r');
