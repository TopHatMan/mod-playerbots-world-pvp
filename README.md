# mod-playerbots-world-pvp

Autonomous **world PvP** for [AzerothCore](https://www.azerothcore.org/) +
[Playerbots](https://github.com/liyunfan1223/mod-playerbots): bots stage
faction-vs-faction fights at weighted, cooldown-gated world hotspots and duel
outside the capitals, so the open world feels contested and alive.

This build adds **cross-mod coordination** so it plays nicely with the dungeon-sim
and artisans mods.

## Features

- Autonomous hotspot events (attackers vs defenders) on a configurable tick,
  chance, and active-event cap.
- Same-faction duels seeded outside Stormwind / Orgrimmar.
- Account-prefix safety filter so only intended bot accounts are used.
- GM commands: `.wpvp start`, `.wpvp status`, `.wpvp spot`.

## Cross-mod coordination (shared reservation)

The mod includes `BotActivityRegistry.h` (a header-only, inline registry shared
byte-identically with the dungeon-sim and artisans mods). With it:

- world-PvP **skips bots that are reserved** (in a dungeon run or raid sim), so a
  bot is never yanked out of a dungeon mid-run;
- world-PvP **reserves** each bot it pulls into an event and **releases** it when
  the event ends, so the dungeon sim won't grab a bot that's currently fighting.

The registry has no link dependency between modules (inline function-local statics
resolve to one instance program-wide), so each mod still builds and runs on its
own — they simply coordinate when more than one is loaded.

## Requirements

- AzerothCore + mod-playerbots.
- Apply the SQL under `data/sql/`.
- If you also run mod-playerbot-dungeon-sim / mod-playerbots-artisans, keep every
  copy of `src/BotActivityRegistry.h` byte-identical.

## Installation

```bash
cd azerothcore/modules
git clone https://github.com/<your-user>/mod-playerbots-world-pvp
# re-run CMake configure, then rebuild
```

Keep the default directory name. AzerothCore generates the module's entry point
from the directory it lives in (`Add<directory>Scripts()`), so a directory named
anything other than `mod-playerbots-world-pvp` fails to link with
`undefined reference to 'Addmod_playerbots_world_pvpScripts()'`.

Re-running CMake **configure** is required, not just a rebuild: the loader that
calls into this module is generated at configure time.

## Credits

Built on AzerothCore and mod-playerbots.