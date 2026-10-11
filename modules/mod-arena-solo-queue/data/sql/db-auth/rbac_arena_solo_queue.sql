-- mod-arena-solo-queue: register the .arena2v2 command permission
-- and grant it to all players (secId 0 = Sec Level Player, role 195).

DELETE FROM `rbac_permissions`         WHERE `id` = 1000;
DELETE FROM `rbac_linked_permissions`  WHERE `roleId` = 195 AND `permissionId` = 1000;

INSERT INTO `rbac_permissions` (`id`, `name`) VALUES
(1000, 'Command: arena2v2');

-- Link to the Player role so every account with sec level 0 can use .arena2v2
INSERT INTO `rbac_linked_permissions` (`roleId`, `permissionId`) VALUES
(195, 1000);
