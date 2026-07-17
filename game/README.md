# NEON OUTRIDER — a cyberpunk hoverbike blaster

A fast, loud, single-file HTML5 arcade game. No dependencies, no build step —
just open `index.html` in any browser (or serve the folder and open it on your
phone; touch controls are built in).

## Play

| Input | Action |
|---|---|
| WASD / Arrow keys / drag | Steer the hoverbike |
| (guns) | Always firing — stay greedy |
| SHIFT or SPACE | **NITRO** — speed burst, 2× score, ramming kills enemies |
| X or B | **MEGABOMB** — clears the screen |
| P | Pause |
| M | Mute |
| Enter / tap | Start / restart |

## What's in the box

- Wave-based combat: scout drones, kamikaze swarmers, rival riders, armored
  gun-vans — and **THE ENFORCER** boss every 5th wave with three attack phases
  (aimed fans, twin spirals, drone escorts).
- Combo multiplier up to **×16** — keep killing to keep it alive; getting hit
  resets it.
- Pickups: weapon upgrades (5 levels, up to homing missile pods), nitro fuel,
  shields, bombs, repairs.
- Full procedural synthwave soundtrack + SFX via WebAudio (138 BPM, four on
  the floor, arps get denser when your combo is hot). No audio files.
- Busy-busy-busy visuals: parallax neon city with flickering kanji signs, rain,
  sky traffic, synth sun, scrolling neon highway, additive-blended particles,
  shockwaves, screen shake, CRT scanline overlay.
- Hi-score persisted in `localStorage`.

Logical resolution 540×860, letterboxed to any screen, DPI-aware.
Everything lives in `index.html` (~1000 lines of vanilla JS on a 2D canvas).
