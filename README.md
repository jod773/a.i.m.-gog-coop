# A.I.M. Co-op

Online co-op and engine improvements for **A.I.M.: Artificial Intelligence Machine** (2003) and
**A.I.M. 2: Clan Wars** (GOG versions). Fly the same world together with friends: shared NPCs,
combat, quests and journal, chat, plus a Direct3D 9 renderer with dynamic lighting, display
options and full gamepad support.

The mod is a `dinput8.dll` loaded by the game. It patches the original executables in memory;
no game file is replaced.

## Features

**Co-op (both games)**
- LAN / virtual LAN (Radmin VPN, Hamachi) co-op for up to 32 players: host a new game or a save,
  others join from the in-game MULTIPLAYER menu and receive the host's world.
- Mirrored NPCs, shots, deaths and time of day; each player keeps their own glider and equipment
  (saved per host).
- Shared quest log and journal, with notices when a teammate gets, completes or fails a task.
- In-game chat (`T` in flight).

**Graphics and display**
- Optional Direct3D 9 renderer: dynamic lighting from explosions, missiles and shots, higher
  shadow resolution and distance, anti-aliasing, anisotropic filtering.
- Fullscreen / windowed / borderless, frame-rate limit, VSync pacing, gamma fixes.

**Controls and quality of life**
- Full gamepad play (XInput) through the game's own key bindings, configurable per button.
- In-game mod settings menus, debug menu, skippable intro videos.
- Clan Wars: "Smart supply" economy fix (clan buildings keep getting supplied).

## Status

**Work in progress, and vibe-coded.** All of this mod's code was written by AI coding assistants,
directed and play-tested by two players - not by experienced game or engine developers. It patches
20-year-old games from the outside, so expect bugs and crashes, and back up your saves before you
try it. Co-op is playable, but not yet stable past the first missions.

Something broken, or something you'd like changed? Please [open an issue](../../issues/new/choose)
(see [Reporting a bug](#reporting-a-bug)) - that is how we find out, and we work through the issues
regularly. The [open issues](../../issues) list the problems we already know about.

## Install (players)

Builds are packaged as zips (`ClanWars-Coop.zip`, `AIM-Coop.zip`). Copy the zip's contents into
the game folder (next to `AIM.exe` / `AIMII.exe`). Settings live in `AIMCoop.ini` /
`ClanWarsCoop.ini` and in the in-game MOD SETTINGS / OPTIONS menus. Allow the game through the
Windows firewall (port 28620, TCP and UDP, also on "Public" networks for VPN LANs).

## Reporting a bug

Open an [issue](../../issues/new/choose) with the "Crash / bug" form. Say which game you played
(A.I.M. or A.I.M. 2 Clan Wars) and attach the log files from the game folder: `AIMCoop.log` /
`ClanWarsCoop.log` and the game's `error.log` (drag them into the issue). The logs can contain
your folder names; edit them out if you like.

## Building (developers)

Requirements: Windows, Python 3 with Pillow, and the llvm-mingw toolchain
(`ext/llvm-mingw-*-msvcrt-x86_64`).

```
powershell -File src/build.ps1        # builds build/aim1 and build/aim2cw dinput8.dll
python tools/make_aim1.py             # dist/AIM-Coop.zip
python tools/make_cw.py               # dist/ClanWars-Coop.zip
```

Layout:

| Folder  | Contents |
|---------|----------|
| `src/common` | shared core: co-op session and network, Direct3D 8 to 9 layer, dynamic lighting, display, input, gamepad, chat, crash log |
| `src/aim1`   | A.I.M. adapter (addresses, mirroring, lobby, quest log) |
| `src/aim2`   | A.I.M. 2: Clan Wars adapter (addresses in `addr_cw.h`) |
| `tools`      | packaging, test harness drivers, reverse-engineering helpers |
| `tr`         | the mod's interface text and replacement UI textures |

Game copies, toolchain, reverse-engineering exports and builds are not in the repository (see
`.gitignore`).

## License

The mod's code and tools are licensed under the
[PolyForm Noncommercial License 1.0.0](LICENSE): you may use, change and share them for any
noncommercial purpose, as long as the license and its `Required Notice:` line go with every copy.
Commercial use is not allowed.

A.I.M. and A.I.M. 2 and their data belong to their respective owners; you need your own copy of
the games.
