# Sokol Highway

A 90s-arcade-style 3D racing demo written in C with [sokol](https://github.com/floooh/sokol)
(`sokol_app` + `sokol_gfx` on the Metal backend), doubling as a **presentation tool**: every
slide is a giant sign on a van that overtakes your car, cuts in ahead and drives off when you
advance to the next slide. A divided highway with two lanes in each
direction loops through rolling hills and mountains. AI traffic (sedans, sports cars, vans and
trucks) follows the car ahead, overtakes and keeps right. Your red sports car drives itself
until you take over.

Everything is generated procedurally at startup: the track, terrain, trees, bridges, signs,
billboards and all textures, including a small pixel font. The app has no asset files.

## Presentation mode (default)

The app opens on a 90s-style title screen ("Welcome to the GAIA annual meeting 2026 ...",
blinking PRESS A BUTTON TO START) over a dimmed, auto-filmed highway. Any key, clicker
button or mouse click starts the talk: the title fades out and the first slide van drives
up. `--no-intro` skips it (the title text lives in `draw_title_screen()` in `src/main.c`).

Slides live in `slides.txt` (or `--slides PATH`) and are read at startup:

```text
# Big yellow title
## Large white subtitle
### Smaller cyan heading
- Bullet point (indented lines continue the bullet)
Plain text
---
@image slides/metr-time-horizons.png
---
@camera heli
# Next slide, shown from the helicopter
```

- `---` separates slides; long lines wrap automatically.
- `@image PATH` shows a PNG/JPEG on the sign (path relative to the slides file). Any text on
  the slide (e.g. a `#` title) goes above the image; the image is aspect-fitted below it.
- `@zoom on|off` controls the close-up: while such a slide is presenting the camera moves in
  until the sign fills the screen. Image slides zoom by default, text slides don't.
- `@camera present|chase|bumper|heli|tv` cuts to that camera when the slide arrives.
- `@chart FILE.yaml` draws METR's time-horizon chart natively from a `benchmark_results`
  YAML file (log-scale 50% horizon vs. release date, CI bars, SOTA models highlighted,
  "above 16 hrs: unreliable" band, trend lines using METR's doubling times). It builds up
  with clicks: the empty chart arrives, the next click sweeps through the years and pops in
  each model, then the all-time trend, then the 2023+ trend. The click after that moves on
  (going back to a chart shows it complete). Options, placed after `@chart`:
  - `@steps 2023 2025` adds intermediate sweep stops (decimal years)
  - `@label <model_id> <text>` renames a model (ids as in the YAML; names are auto-derived otherwise)
  - `@hide <model_id>` leaves a model out

  Labels are placed automatically and skipped where they would overlap; shorter names fit better.
  Chart slides zoom in like image slides.
- `@chart FILE.csv` draws the **Epoch Capabilities Index** from Epoch AI's `eci_scores.csv`:
  ECI vs. release date, closed-weights (orange) and open-weights (cyan) models, with each
  group's frontier as a staircase that climbs during the sweep. Annotation steps: linear
  frontier trends from 2024 on (ECI points per year), then lag markers showing how long after
  closed weights each open-weights frontier model reached the same score (average in the legend).
  Model ids contain spaces here, so quote them: `@label "GPT-4 (Mar 2023)" GPT-4`.
  `@maxlabels N` limits the number of labels (default 14; the newest frontier models and the
  biggest jumps are labelled first).
- `@source TEXT` adds a small attribution line in the sign's top-right corner (Epoch AI's data
  is CC BY and needs attribution).
- Text uses an 8x8 retro bitmap font (ASCII; common UTF-8 punctuation is converted, and
  å ä ö Å Ä Ö ü é are composed from a base letter plus dots/accents).

| key | action |
| --- | --- |
| Space, Right, Down, PageDown, Enter, N, left click | next slide |
| Left, Up, PageUp, Backspace, right click | previous slide |
| C | cycle cameras (present, chase, bumper, heli, TV) — every camera frames the presenting sign, and signs turn towards the camera on their turntable mast |
| H | show camera name, F1 key help |

Most presentation clickers send PageDown/PageUp or arrow keys, so they work out of the box.
Run with `--demo` for the original free-roaming traffic demo.

## Scenery

One lap of the 5.1 km loop (~2.5 min at presentation speed) passes through zones that blend
into each other, all generated procedurally in `src/world.c` (zone ranges in `ZONES[]`):

| loop position | zone |
| --- | --- |
| 0.00 - 0.12 | countryside, overpass, SOKOL / GAIA 2026 billboards, "SINGULARITY 42 KM" gantry |
| 0.12 - 0.29 | dense pine forest, second overpass |
| 0.31 - 0.40 | village of Gaiaby: falu-red / yellow / white houses, church, town signs |
| 0.44 - 0.73 | mountain pass: road climbs ~45 m, rock wall on one side, valley and snowy peaks on the other |
| 0.77 - 0.88 | GPU campus: data-centre halls with cooling units, "GPU CAMPUS" facade sign, wind turbines |
| 0.89 - 0.99 | farmland: patchwork fields, red barns, hay bales, wind turbines |

Talk-themed billboards (RSI "improve yourself... recursively", "time horizon x2 every 4 months",
"agents working nonstop", "fresh tokens farm shop") and gantries are drawn in `src/textures.c`.
`--at F` starts at a given loop fraction, handy for checking a zone.

## Build and run (macOS)

Requirements: Xcode Command Line Tools (`clang`, `make`).

```sh
make          # builds build/highway
make run      # or: ./build/highway
```

Command-line options (`./build/highway --help`):

| option | meaning |
| --- | --- |
| `--slides PATH` | slides file (default `slides.txt`) |
| `--demo` | free-roaming traffic demo instead of the presentation |
| `--no-intro` | skip the title screen |
| `--at F` | start at fraction F of the loop (0.2 forest, 0.36 village, 0.58 mountain, 0.8 campus, 0.93 farm) |
| `--cam N` | start camera: 0 present, 1 chase, 2 bumper, 3 heli, 4 TV |
| `--res N` | render resolution: 0 = 240p, 1 = 360p, 2 = 540p, 3 = 720p (default), 4 = native |
| `--car N` | follow car N (0 = player) |
| `--fullscreen` | start fullscreen |
| `--shot PATH` / `--shot-frame N` | save a PNG after N frames (default 240) and quit |
| `--next-at N` | advance to the next slide at frame N (repeatable; for scripted screenshots) |

## Demo-mode controls (`--demo`)

| key | action |
| --- | --- |
| Up / Down | raise / lower cruise speed (switches to manual) |
| Left / Right | change lane |
| P | toggle autopilot |
| C | next camera (chase, bumper, heli, TV) |
| A | toggle demo mode (cycles cameras every 11 s) |
| Tab / 0 | follow next traffic car / back to the player |
| R | render resolution 240p / 360p / 540p / 720p / native |
| J | PS1-style vertex jitter |
| T | affine (wobbly) texture mapping |
| D | 15-bit colour with ordered dithering |
| F | nearest / bilinear texture filtering |
| S | scanlines |
| H / F1 | HUD / key help |
| F12 | save screenshot (`highway-shot-NNN.png`) |
| Esc | quit |

## How the retro look works

- The scene renders into a low-resolution offscreen target (720 lines by default, R for 240p) with a
  reversed-Z float depth buffer. A fullscreen pass then upscales it with nearest filtering
  and can apply 4x4 Bayer dithering down to 15-bit colour.
- Lighting and linear fog are computed per vertex (Gouraud). Fog blends into the sky panorama
  at the horizon.
- Optionally, vertices snap to the low-res pixel grid and UVs can be interpolated without
  perspective correction (`center_no_perspective`), giving the PS1 wobble.
- Cars are flat-shaded hexahedra. Body-paint vertices are marked with alpha 0 and tinted per
  car in the vertex shader. Shadows are soft alpha-blended blobs.

## Source layout

| file | contents |
| --- | --- |
| `src/main.c` | app setup, pipelines, camera, rendering, HUD, input, CLI |
| `src/world.c` | closed-loop track, terrain heightfield, road, barriers, lamps, bridges, signs, trees, sky |
| `src/cars.c` | car models + traffic simulation (IDM car-following, lane changes) |
| `src/present.c` | slides parser, slide-to-texture rendering, sign-van choreography |
| `src/chart.c` | METR YAML loader + animated time-horizon chart geometry |
| `src/textures.c` | procedural textures: road, sky panorama, billboard/sign atlas, 5x7 font |
| `src/shaders.h` | Metal shaders (scene + display) |
| `src/mesh.c`, `src/mathx.h`, `src/noise.h` | mesh builder, vector math, value noise |
| `src/sokol_impl.m` | sokol implementation (Objective-C) + Metal readback for screenshots |
| `third_party/sokol` | vendored sokol headers (floooh/sokol @ `7151e64`) |
