CREATE TABLE IF NOT EXISTS `playerbot_world_pvp_hotspot` (
  `id` int unsigned NOT NULL AUTO_INCREMENT,
  `name` varchar(64) NOT NULL,
  `enabled` tinyint unsigned NOT NULL DEFAULT 1,
  `attacker_team` tinyint unsigned NOT NULL DEFAULT 1 COMMENT '0 Alliance, 1 Horde, 2 Any',
  `defender_team` tinyint unsigned NOT NULL DEFAULT 0 COMMENT '0 Alliance, 1 Horde, 2 Any',
  `min_level` tinyint unsigned NOT NULL DEFAULT 20,
  `max_level` tinyint unsigned NOT NULL DEFAULT 60,
  `map_id` int unsigned NOT NULL DEFAULT 0,
  `rally_x` float NOT NULL DEFAULT 0,
  `rally_y` float NOT NULL DEFAULT 0,
  `rally_z` float NOT NULL DEFAULT 0,
  `rally_o` float NOT NULL DEFAULT 0,
  `target_x` float NOT NULL DEFAULT 0,
  `target_y` float NOT NULL DEFAULT 0,
  `target_z` float NOT NULL DEFAULT 0,
  `target_o` float NOT NULL DEFAULT 0,
  `attackers_min` int unsigned NOT NULL DEFAULT 2,
  `attackers_max` int unsigned NOT NULL DEFAULT 5,
  `defenders_min` int unsigned NOT NULL DEFAULT 2,
  `defenders_max` int unsigned NOT NULL DEFAULT 5,
  `duration_min` int unsigned NOT NULL DEFAULT 5,
  `duration_max` int unsigned NOT NULL DEFAULT 12,
  `weight` int unsigned NOT NULL DEFAULT 100,
  `cooldown_seconds` int unsigned NOT NULL DEFAULT 1800,
  `last_start` int unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_playerbot_world_pvp_hotspot_name` (`name`),
  KEY `idx_playerbot_world_pvp_enabled` (`enabled`,`weight`),
  KEY `idx_playerbot_world_pvp_level` (`min_level`,`max_level`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

DELETE FROM `command` WHERE `name` IN (
  'wpvp',
  'wpvp help',
  'wpvp status',
  'wpvp scan',
  'wpvp start',
  'wpvp stop',
  'wpvp spot',
  'wpvp spot list',
  'wpvp spot create',
  'wpvp spot rally',
  'wpvp spot target',
  'wpvp spot counts',
  'wpvp spot duration',
  'wpvp spot enable'
);

INSERT INTO `command` (`name`, `security`, `help`) VALUES
('wpvp', 3, 'Syntax: .wpvp help\nPlayerbot World PvP GM commands.'),
('wpvp help', 3, 'Syntax: .wpvp help\nShows Playerbot World PvP commands.'),
('wpvp status', 3, 'Syntax: .wpvp status\nShows active world-PvP events.'),
('wpvp scan', 3, 'Syntax: .wpvp scan [minLvl] [maxLvl]\nShows eligible online random Playerbots and per-hotspot readiness.'),
('wpvp start', 3, 'Syntax: .wpvp start <spot>\nForce-start a configured world-PvP hotspot.'),
('wpvp stop', 3, 'Syntax: .wpvp stop <id|all>\nStop one or all active world-PvP events.'),
('wpvp spot', 3, 'Syntax: .wpvp spot <subcommand>\nManage Playerbot World PvP hotspots.'),
('wpvp spot list', 3, 'Syntax: .wpvp spot list\nList configured hotspots.'),
('wpvp spot create', 3, 'Syntax: .wpvp spot create <name> <attacker> <defender> <minLvl> <maxLvl>\nCreate/update a hotspot at your current position.'),
('wpvp spot rally', 3, 'Syntax: .wpvp spot rally <name>\nSave your current location as attacker rally/outside-town point.'),
('wpvp spot target', 3, 'Syntax: .wpvp spot target <name>\nSave your current location as target/defender point.'),
('wpvp spot counts', 3, 'Syntax: .wpvp spot counts <name> <attMin> <attMax> <defMin> <defMax>\nSet event population.'),
('wpvp spot duration', 3, 'Syntax: .wpvp spot duration <name> <minMinutes> <maxMinutes>\nSet event duration.'),
('wpvp spot enable', 3, 'Syntax: .wpvp spot enable <name> <0|1>\nEnable or disable a hotspot.');

-- Vanilla world-PvP campaign seeds.
-- Map 0 = Eastern Kingdoms, Map 1 = Kalimdor.
-- Coordinates are seed points: use .wpvp spot rally <name> / .wpvp spot target <name> in-game to fine tune any spot.
-- All active seeds are 20+ so lowbie starter bots do not get dragged into war.

INSERT INTO `playerbot_world_pvp_hotspot`
(`name`,`enabled`,`attacker_team`,`defender_team`,`min_level`,`max_level`,`map_id`,`rally_x`,`rally_y`,`rally_z`,`rally_o`,`target_x`,`target_y`,`target_z`,`target_o`,`attackers_min`,`attackers_max`,`defenders_min`,`defenders_max`,`duration_min`,`duration_max`,`weight`,`cooldown_seconds`)
VALUES
-- Redridge classic gank: Horde raid Lakeshire, Alliance responds.
('Lakeshire',1,1,0,20,35,0,-9325,-2285,70,0,-9215,-2150,70,0,2,5,2,5,5,12,140,1800),

-- Hillsbrad forever-war. Keep both directions so the front breathes.
('Southshore',1,1,0,25,45,0,-760,-930,58,0,-845,-535,55,0,3,7,3,7,8,18,180,1500),
('TarrenMill',1,0,1,25,45,0,-860,-560,55,0,-20,-915,57,0,3,7,3,7,8,18,180,1500),

-- Ashenvale mid-level skirmishes.
('Astranaar',1,1,0,20,35,1,2300,-2500,110,0,2750,-430,110,0,2,5,2,5,5,12,120,1800),
('Splintertree',1,0,1,20,35,1,2700,-500,110,0,2300,-2500,110,0,2,5,2,5,5,12,100,1800),

-- Barrens road war. Good for 20+ when you have fewer higher bots.
('Crossroads',1,0,1,20,35,1,-940,-3720,10,0,-456,-2650,95,0,2,5,2,5,5,12,130,1800),
('RatchetRoad',1,1,0,20,35,1,-455,-2650,95,0,-955,-3740,8,0,2,5,2,5,5,12,90,1800),

-- Stonetalon / Thousand Needles flavor.
('SunRockRetreat',1,0,1,22,35,1,1040,760,105,0,960,915,105,0,2,5,2,5,5,12,80,2400),
('FreewindPost',1,0,1,25,38,1,-5400,-2500,-50,0,-5440,-2440,90,0,2,5,2,5,5,12,70,2400),

-- Arathi front. Strong classic 30-45 hotspot pair.
('RefugePointe',1,1,0,30,45,0,-920,-3490,70,0,-1260,-2520,35,0,3,7,3,7,8,18,130,1800),
('Hammerfall',1,0,1,30,45,0,-1260,-2520,35,0,-920,-3490,70,0,3,7,3,7,8,18,130,1800),

-- STV roaming/gank chaos. Any-team-ish is modeled as alternating faction pressure.
('NesingwaryCamp',1,1,0,30,45,0,-11670,-50,5,0,-11620,-70,10,0,2,6,2,6,6,14,100,1800),
('BootyBayRoad',1,0,1,35,50,0,-14450,460,15,0,-14300,520,20,0,2,6,2,6,6,14,80,2400),

-- Dustwallow / Desolace / Badlands. Enabled but lower weighted.
('TheramoreRoad',1,1,0,35,45,1,-3600,-4500,10,0,-3700,-4400,15,0,2,5,2,5,6,14,70,2400),
('Brackenwall',1,0,1,35,45,1,-3150,-2850,35,0,-2920,-3170,35,0,2,5,2,5,6,14,60,2400),
('NijelsPoint',1,1,0,30,42,1,280,-2380,105,0,150,-1780,105,0,2,5,2,5,6,14,60,2400),
('Shadowprey',1,0,1,30,42,1,-1650,3100,90,0,-1600,3050,90,0,2,5,2,5,6,14,60,2400),
('Kargath',1,0,1,35,50,0,-6700,-2400,240,0,-6650,-2200,245,0,2,5,2,5,6,14,70,2400),

-- Future 45+ / 50+ vanilla chaos. Enabled=0 until you have the population.
('SearingGorge',0,2,2,45,55,0,-6500,-1200,180,0,-6550,-1150,180,0,3,8,3,8,8,18,80,2400),
('BlackrockMountain',0,2,2,50,60,0,-7500,-1100,270,0,-7550,-1250,270,0,4,10,4,10,10,25,100,2400),
('LightHopeChapel',0,1,0,55,60,0,2250,-5300,85,0,2280,-5310,85,0,4,10,4,10,10,25,90,3600)
ON DUPLICATE KEY UPDATE
  `enabled`=VALUES(`enabled`),
  `attacker_team`=VALUES(`attacker_team`),
  `defender_team`=VALUES(`defender_team`),
  `min_level`=VALUES(`min_level`),
  `max_level`=VALUES(`max_level`),
  `map_id`=VALUES(`map_id`),
  `rally_x`=VALUES(`rally_x`),
  `rally_y`=VALUES(`rally_y`),
  `rally_z`=VALUES(`rally_z`),
  `rally_o`=VALUES(`rally_o`),
  `target_x`=VALUES(`target_x`),
  `target_y`=VALUES(`target_y`),
  `target_z`=VALUES(`target_z`),
  `target_o`=VALUES(`target_o`),
  `attackers_min`=VALUES(`attackers_min`),
  `attackers_max`=VALUES(`attackers_max`),
  `defenders_min`=VALUES(`defenders_min`),
  `defenders_max`=VALUES(`defenders_max`),
  `duration_min`=VALUES(`duration_min`),
  `duration_max`=VALUES(`duration_max`),
  `weight`=VALUES(`weight`),
  `cooldown_seconds`=VALUES(`cooldown_seconds`);

-- Same-faction duel practice seeds.
-- These are not city raids. They stage same-faction Playerbots outside capital cities and ask them to duel.
-- Tune with .wpvp spot rally StormwindDuel / .wpvp spot target StormwindDuel, etc.
INSERT INTO `playerbot_world_pvp_hotspot`
(`name`,`enabled`,`attacker_team`,`defender_team`,`min_level`,`max_level`,`map_id`,`rally_x`,`rally_y`,`rally_z`,`rally_o`,`target_x`,`target_y`,`target_z`,`target_o`,`attackers_min`,`attackers_max`,`defenders_min`,`defenders_max`,`duration_min`,`duration_max`,`weight`,`cooldown_seconds`)
VALUES
('StormwindDuel',1,0,0,20,60,0,-8834,622,94,0,-8795,585,96,0,2,5,2,5,5,10,90,1200),
('OrgrimmarDuel',1,1,1,20,60,1,1502,-4415,22,0,1450,-4418,25,0,2,5,2,5,5,10,90,1200)
ON DUPLICATE KEY UPDATE
  `enabled`=VALUES(`enabled`),
  `attacker_team`=VALUES(`attacker_team`),
  `defender_team`=VALUES(`defender_team`),
  `min_level`=VALUES(`min_level`),
  `max_level`=VALUES(`max_level`),
  `map_id`=VALUES(`map_id`),
  `rally_x`=VALUES(`rally_x`),
  `rally_y`=VALUES(`rally_y`),
  `rally_z`=VALUES(`rally_z`),
  `rally_o`=VALUES(`rally_o`),
  `target_x`=VALUES(`target_x`),
  `target_y`=VALUES(`target_y`),
  `target_z`=VALUES(`target_z`),
  `target_o`=VALUES(`target_o`),
  `attackers_min`=VALUES(`attackers_min`),
  `attackers_max`=VALUES(`attackers_max`),
  `defenders_min`=VALUES(`defenders_min`),
  `defenders_max`=VALUES(`defenders_max`),
  `duration_min`=VALUES(`duration_min`),
  `duration_max`=VALUES(`duration_max`),
  `weight`=VALUES(`weight`),
  `cooldown_seconds`=VALUES(`cooldown_seconds`);
