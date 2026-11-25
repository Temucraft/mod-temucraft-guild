-- mod-temucraft-guild: its command (under .temucraft) as RBAC permission 104201 (range 104201-104249). Admin only by
-- default: linked to the administrator roles 192 and 1014. Grant it per account with .rbac account grant <account> 104201.
DELETE FROM `rbac_linked_permissions` WHERE `linkedId` BETWEEN 104201 AND 104249;
DELETE FROM `rbac_permissions` WHERE `id` BETWEEN 104201 AND 104249;
INSERT INTO `rbac_permissions` (`id`, `name`) VALUES
(104201, 'Command: temucraft guildchat');
INSERT INTO `rbac_linked_permissions` (`id`, `linkedId`) VALUES
(192, 104201),
(1014, 104201);
