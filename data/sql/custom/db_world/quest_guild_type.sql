ALTER TABLE `quest_template_addon`
    ADD COLUMN `RequiredGuildMembers` tinyint unsigned NOT NULL DEFAULT 0
    AFTER `SpecialFlags`;
