# Skate 3 — Linux

A native Linux build of the Skate 3 recompilation, with custom map support built
in: drop map packs in `dlc/` and the game asks which one to play.

**This package does not contain the game.** It contains the engine. You supply
your own Xbox 360 disc image of Skate 3; the first launch walks you through
installing it.

---

## Running it

```sh
./play-skate3.sh
```

On the first launch the setup screen asks for your Skate 3 ISO and installs the
game files into `game/` beside the executable. That happens once.

To add it to Steam (this is also how you play it on a Steam Deck): **Add a
Non-Steam Game**, point it at `play-skate3.sh`, and launch it from your library.
A controller works throughout, including the setup screens and the map picker.

## Custom maps

Put each map pack in its own folder under `dlc/`:

```
dlc/JOYRIDE/joyride_00000000.big
dlc/JOYRIDE/joyride_00000000.header
dlc/MCGAZZA/mcgazza_00000000.big
dlc/MCGAZZA/mcgazza_00000000.header
```

Signed content packages (the `LIVE`/`CON` files official DLC ships as, with no
file extension) go in the same folder and need nothing beside them.

Then:

- **No packs** — the base game, unchanged.
- **One pack** — it loads, no questions asked.
- **More than one** — the game asks which to play, every time it starts. Move
  the stick or the arrow keys, `A` or `Enter` to play, `B` or `Escape` for the
  base game with no add-on.

Only the pack you pick is installed. That is deliberate and not just tidiness:
the game's own content scan does not survive a large collection being installed
at once, and every installed pack piles its locations into the same menu. One
per session keeps a collection of any size working.

Your maps are never moved out of `dlc/`. Packs you are not playing are held in
`addons/parked/` inside the save folder and come straight back when you pick
them again, so switching between two maps you have played before is instant.

Once in the game, a custom map is reached the same way official DLC is: pause →
challenge map → tab across to **Locations** → your pack is the last entry.

### Cvars, if you want to script it

| cvar | what it does |
|---|---|
| `skate3_dlc_select=<name>` | play this add-on and skip the question. Matches a package name, a file name or the display name. `none` forces the base game. |
| `skate3_dlc_prompt=auto\|always\|never` | `auto` (default) asks only when more than one is installed. |
| `skate3_dlc_remember=false` | stop pre-selecting last session's choice. |
| `skate3_dlc_root=<dir>` | look for packs somewhere other than `dlc/`. |

## Performance

Press **Escape** in game (or **RB+Start** on a controller) and the Video page
opens with a **Preset** row at the top - one click sets every quality option
below it, and they all stay editable afterwards.

The game also picks a preset from your hardware on the **first run only**;
after that your own settings are kept. You can name one on the command line:

```sh
./play-skate3.sh --skate3_performance_profile=deck
```

| preset | what it does |
|---|---|
| `quality` | the desktop default: the world drawn at 2x the window, 4x MSAA, every effect |
| `balanced` | native resolution, 4x MSAA, every effect |
| `deck` | native resolution, 1x MSAA, no soft shadows or static shadow casters - keeps ambient occlusion, god rays and full draw distance |
| `performance` | every frame that can be had: also no shadows, no bloom, no ambient occlusion |
| `auto` | `deck` on a Steam Deck or any machine with no discrete GPU, `quality` otherwise |

Measured on an integrated Intel UHD (Raptor Lake-P, 48 EU) at 1280x800, median
over 600-frame windows of real gameplay:

| preset | fps |
|---|---|
| `quality` | 7.4 |
| `deck` | 36.0 |
| `performance` | 42.2 |

This build is compiled for **x86-64-v3 (AVX2)**, which the recompiled PowerPC
vector code benefits from directly: 225.2 fps against 186.7 for the same build
at the old generic baseline, measured on the discrete GPU where the CPU is what
limits. It needs a 2013-or-newer CPU (Intel Haswell, AMD Zen) - every Steam Deck
and modern handheld qualifies. Build with `-DSKATE3_ENABLE_X86_64_V3=OFF` for a
binary that runs on older machines.

The same build on a discrete RTX 4050 runs the full `quality` preset at 169.6
fps, so the guest CPU work is only about 5.4 ms a frame - on an integrated part
the GPU is what sets the number, and supersampling is most of it. If your
machine has both, make sure the game is using the discrete GPU; it picks one
automatically, and the log names it at startup ("Vulkan device '...'").

**On a weak GPU, turning effects off past the `deck` preset buys very little.**
Measured there: halving the draw distance changed nothing (36.3), quartering it
changed nothing (36.0), and quarter resolution reached only 37.3. Shadows are
the one exception, and that is exactly what `performance` takes.

Anything can still be set by hand, and an explicit setting always beats the
preset:

```sh
./play-skate3.sh --skate3_performance_profile=deck --skate3_native_render_scene_msaa=4
```

## Diagnostics

The console prints startup facts (graphics device, video preset, which add-on
loaded) and real warnings. The renderer's own per-mesh and texture-cache
bookkeeping is off by default - normal traffic that ran to several lines a
second and buried anything worth reading. Turn it back on when investigating:

| cvar | what it logs |
|---|---|
| `skate3_native_render_scene_verbose=true` | per-mesh capture: cloth/ropa, gap fills, clone mispairs, fog gating |
| `skate3_native_render_scene_tex_log=true` | texture re-decodes and near-black payloads served as white |
| `nrhi_vulkan_log_memory=true` | VMA heap usage against the driver budget, every 600 frames |
| `ffmpeg_verbose=true` | every FFmpeg decoder message instead of the first few of each |

Two messages that look alarming and are not: `XmaContext ... input offset
exceeds buffer size` is an audio packet boundary the decoder resynchronises
past with no audible effect, and `VFS: entry not found` lines for `.xexpp`,
`fileserver.ini` and `data/unlocks` are the game probing for optional files
that a disc install does not have.

## Known issues

- The first-run **difficulty** screen draws its options as blank rows. The list
  still works — press `A`/`Enter` to take the highlighted one — and it is only
  shown once, before your profile exists.

## Files

```
skate3               the engine
librexruntime.so     its runtime library
play-skate3.sh       launcher
dlc/                 drop map packs here
game/                your installed game files (created on first run)
```

Saves and settings live under `~/.local/share/skate3/`.
