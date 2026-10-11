-- mod-guild-season: guild reward columns in quest_template_addon
-- Applied to: acore_world

ALTER TABLE `quest_template_addon`
    ADD COLUMN `RewardGuildPoints` int unsigned NOT NULL DEFAULT 0 AFTER `RequiredGuildMembers`,
    ADD COLUMN `RewardGuildRep`    int unsigned NOT NULL DEFAULT 0 AFTER `RewardGuildPoints`;
