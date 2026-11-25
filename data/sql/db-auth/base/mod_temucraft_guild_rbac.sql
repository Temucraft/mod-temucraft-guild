-- mod-temucraft-guild: its command (under .temucraft) as RBAC permission 104251 (range 104251-104299). Admin only by
-- default: linked to the administrator roles 192 and 1014. Grant it per account with .rbac account grant <account> 104251.
DELETE FROM `rbac_linked_permissions` WHERE `linkedId` BETWEEN 104251 AND 104299;
DELETE FROM `rbac_permissions` WHERE `id` BETWEEN 104251 AND 104299;
INSERT INTO `rbac_permissions` (`id`, `name`) VALUES
(104251, 'Command: temucraft guildchat');
INSERT INTO `rbac_linked_permissions` (`id`, `linkedId`) VALUES
(192, 104251),
(1014, 104251);
