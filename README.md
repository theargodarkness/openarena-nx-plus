# Quake3-NX

A Nintendo Switch port of ioquake3 that plays **OpenArena** and **Quake III Arena**, with four-player split-screen, native controller handling, gyro aiming and an ARM64 JIT. It is a fork of [OpenArenaNX](https://github.com/faithvoid/OpenArenaNX), which is based on fgsfdsfgs' ioquake3-nx.

Each build produces two homebrew apps from the same engine, with the same features:

| App | Plays | Game data you supply |
|---|---|---|
| `OpenArenaNX.nro` | OpenArena (free) | the `baseoa` folder from OpenArena 0.8.8 |
| `Quake3NX.nro` | Quake III Arena | the `baseq3` folder from your own copy (`pak0.pk3` to `pak8.pk3`) |

> **Status:** everything listed here builds in CI and the engine side has been tested on a desktop build. Testing on real hardware is ongoing, so expect rough edges and please report what you find.

## What's new in this fork

- **Split-screen for up to four players**, with the stock game data and mods.
- **Controllers read natively** (Pro Controller, Joy-Con pair, single sideways Joy-Con, handheld), with clean handling of disconnects.
- **Gyro aiming per player**, with separate sensitivity for full controllers and single Joy-Cons.
- **Controller settings screen** built into the engine, the same in both games.
- **Co-op mode**: split-screen players can't hurt each other.
- **Radar, enemy highlights and hit markers** (optional, local games only for the first two).
- **Field of view and HUD size** settings.
- **ARM64 JIT** for game code (ported from Quake3e).

## Installation

1. Copy the `.nro` files to a folder on your SD card, for example `/switch`.
2. Put the game data next to them in `/switch`: `baseoa` for OpenArena, `baseq3` for Quake III Arena.
3. Launch from the Homebrew Menu **by holding R on a game**, not from the album. Album mode has too little memory and cannot open the system controller screen.

Each game keeps its own config (`baseoa/q3config.cfg`, `baseq3/q3config.cfg`). If a config has no controller binds, the default layout below is applied automatically.

## Controls

### Pro Controller, Joy-Con pair or handheld

| Control | Action |
|---|---|
| Left stick | Move |
| Right stick | Look |
| ZL / B | Jump |
| ZR / A | Fire |
| L | Crouch |
| R | Next weapon |
| D-pad left / right | Previous / next weapon |
| D-pad up | Zoom |
| D-pad down | Console |
| X | Use item |
| Y | Taunt |
| Minus | Scores (hold for controller settings) |
| Plus | Menu |
| Stick clicks | Vote no (left) / yes (right) |

### Single Joy-Con held sideways

| Control | Action |
|---|---|
| Stick forward / back | Move |
| Stick left / right | Turn (or strafe, see settings) |
| Gyro | Aim |
| Bottom face button | Jump |
| Right face button | Fire |
| Left face button | Next weapon |
| Top face button | Centre the view |
| SL / SR | Jump / fire |
| Stick click | Menu |
| Minus or Plus | Scores (hold for controller settings) |
| Buttons on the end | Crouch and use item |

### In menus

Left stick moves the cursor, A selects, B goes back, the d-pad moves between items, L opens the on-screen keyboard and R deletes a character. Any connected controller can drive the menus. USB keyboard and mouse work too.

## Split-screen

Every local player is a complete client running the unmodified game code, drawn into its own part of the screen. Two players are stacked or side by side; three and four get a quarter each.

**Starting a game with several players**

- When you open Single Player or Multiplayer, the system controller screen comes up. Everyone presses **L+R**, or **SL+SR** to hold a Joy-Con sideways. The number of controllers set up there is the number of players.
- Docked, that screen always appears. Undocked with the Joy-Cons attached it is skipped: the console is player 1 and any other connected controllers join as players 2 to 4.

**Joining a game in progress**

- Hold **L+R** for a second on a spare controller (SL+SR on a sideways Joy-Con) while hosting.
- Holding SL+SR on one Joy-Con of a pair splits the pair; keep holding and it joins.

**Good to know**

- The server needs a free slot for each player.
- Sound is mixed for player 1. The other players hear their own weapon and movement at full volume.
- Split-screen works in games you host. Several local players cannot join a remote server.
- Binds are shared by all players. Name and model per player: `p2_name`, `p2_model`, `p2_headmodel` (and `p3_`, `p4_`).

## Controller settings

Hold **Minus** for about a second (Plus on a lone right Joy-Con), in a menu or during a game. Move with the d-pad or stick, change with left/right, close with B. Sticks and gyro stay live while it is open, so you can feel a change as you make it.

| Setting | What it does |
|---|---|
| Stick aim speed / precision | How fast the right stick turns, and how fine small movements are |
| Invert stick aim | Flips up and down |
| Stick dead zone | How far a stick must move before it counts |
| Gyro aiming | Gyro on or off for full controllers (always on for a single Joy-Con) |
| Gyro sensitivity (controller / single Joy-Con) | Separate sliders for each |
| Single Joy-Con: stick turns | Turn with the stick, or strafe instead |
| Controller screen at game start | The system controller screen described above |
| Field of view | 60 to 130 |
| HUD size | Shrinks the HUD towards the middle of the view (50% to 100%) |
| 2 players: side by side | Side by side instead of stacked |
| Players can hurt each other | Off = co-op: shots, projectiles and splash from one local player don't hurt the others |
| Highlight enemies | Red glow, red name, or both. Not through walls. Hosted games only |
| Hit markers | A mark at the crosshair when a shot lands; shape, size, colour and duration |
| Radar | Sweeping radar in the corner of each view; size and sweep time. Hosted games only |
| Show controller debug info | Prints what each controller sends, for troubleshooting |

## Console commands and cvars

| Name | Purpose |
|---|---|
| `splitscreen <1-4>` | Set the number of local players |
| `controllers` | Open the system controller screen |
| `padsettings` | Open the controller settings |
| `lp <player> <command>` | Run a command as a local player (0 = player 1) |
| `cl_splitscreenVertical` | 1 = two players side by side |
| `sv_splitscreenPvP` | 0 = co-op |
| `cl_enemyMarker` | 0 off, 1 glow, 2 name, 3 both |
| `cl_hitMarker`, `cl_hitMarkerShape`, `cl_hitMarkerSize`, `cl_hitMarkerColor`, `cl_hitMarkerTime` | Hit marker |
| `cl_radar`, `cl_radarSize`, `cl_radarSweep`, `cl_radarRange` | Radar |
| `cl_hudScale` | HUD size, 0.5 to 1 |
| `j_lookSensitivity`, `j_lookCurve`, `j_lookInvert` | Stick aiming |
| `in_gyroSensitivity`, `in_gyroSensitivitySingle` | Gyro sensitivity |
| `in_joyconStickTurn`, `in_joyconStickRotate` | Single Joy-Con stick behaviour |
| `in_controllerPrompt`, `in_padJoin` | Controller screen at game start; joining with L+R |
| `vm_jit` | 0 = run game code in the interpreter |

## Performance

Game code is compiled to native ARM64 when a map loads. Menus always run interpreted, so the game starts even if the compiler misbehaves; if a map fails to load, open the console from the menu (Y) and set `vm_jit 0`.

## Known issues

- Putting the console to sleep while the game runs can crash it.
- Busy fights on large maps can slow down, more so with three or four players.
- HUD size can only shrink the HUD, and it shrinks towards the middle of the view.
- Co-op covers weapons fire and explosions. Other sources of damage decided by the game code are unchanged.
- Enemy highlights, radar and co-op are for games hosted on the console.

## Building

Builds run on GitHub Actions (`.github/workflows/build.yml`) in the devkitPro `devkita64` container. Locally:

```
make -f Makefile.nx                 # OpenArenaNX.nro
make -f Makefile.nx GAME=quake3     # Quake3NX.nro
make -f Makefile.nx2                # Switch 2 build, also takes GAME=quake3
```

## Credits

- ioquake3 team (engine)
- OpenArena team (game assets)
- Quake3e project (ARM64 QVM compiler)
- devkitPro team (toolchain)
- fgsfdsfgs (ioquake3-nx)
- faithvoid (OpenArenaNX)

Released under the GPL, version 2 or later, like ioquake3.
