# Mafia: Definitive Edition (PPSA36458): red tint in the open city

Work-in-progress investigation branch. Not meant for a PR as is: `investigations/` breaks the repository rule of
adding no files beyond code, tests and debt fragments, and the `Recompiler.cpp` change is a debug aid.

Status (2026-11-23): cause narrowed to the colour-grading LUT contents. Not fixed. Reproduces on `main` at
`a182406ffa`.

## Symptom

- The whole 3D scene is tinted red: lights turn pink and the sky purple. The HUD looks normal.
- Every launch except the first one after a fresh import is red.
- The real trigger is the save game: with a save, the game loads straight into the open city. A fresh folder has no
  save and starts at the intro, which isn't red.
- Moving `_sd\gaslo*` (the two save games) aside removes the tint. Setting aside the profile, video settings, boot
  counter, `app0\edit\vfs_prospero.bin` or `shader_cache` changes nothing.

## Ruled out

- Shader disk cache. A run that compiled every shader fresh was red, and `APS5_VERIFY_SHADER_CACHE` (the debug
  change on this branch) found 0 cached shaders that differ from a fresh compile.
- Pipeline journal and replay (`APS5_NO_PIPELINE_JOURNAL`, `APS5_PIPELINE_JOURNAL_THREADS=0`), disk plans
  (`APS5_NO_PERF_DISK_PLANS`).
- The `agc-sampled-surface-import` change (`APS5_NO_SAMPLED_SURFACE_IMPORT`, `APS5_NO_SAMPLED_FROM_STORAGE`):
  still red, and red on `main` too.
- The 3D texture swizzle: the LUT memory itself holds the wrong values, so this isn't a layout problem.
- Physical aliasing: an `APS5_TRACE_MEMORY` run shows no physical memory mapped at two virtual addresses.

## What was found

Capture: `D:\dev\ps5\captures\mafia-city` (6 frames, recorded with the branch build at `8f787c5d295e`). The
captured reference frames and the frames replayed through `main` are both red (average RGB about 115/17/17).

1. Skipping shaders one at a time with `APS5_SKIP_SHADER` (`scripts/skipscan.sh`) shows that the final
   tone-map/composite pass is fragment `0x453eb3c700` (vertex `0x453eb3c000`, 384 indices) drawing into
   `0x45219d0000`, which is then copied to the 4K output `0x4ae57e0000`. Skipping it gives
   `data/replay-final-pass-skipped.png`.
2. Its texture descriptors (from `APS5_DUMP_DRAW_SHADERS=0x45219d0000` and `agc_shader_replay --mem`, decoded with
   `scripts/tsharp.py`) include a **3D 16x16x16 texture, format 130, swizzle ZYXW, swizzle mode 9 (64KB_S), at
   `0x45474a0000`**: the colour-grading LUT. The others are 2D bloom/exposure inputs (format 36, 2048x1152 and
   1024x576, and so on).
3. The LUT memory (`agc_frame_replay --dump 0x45474a0000:65536:lut.bin`, kept as
   `data/lut-0x45474a0000.bin`, analysed by `scripts/lut.py`): 3251 of 4096 texels are `0xFF800000`. As
   BGRA8 that's R=128 G=0 B=0 A=255, a dark red; as a float it's -infinity. The other 845 texels are plausible
   BGRA colours with A=255. Under the 3D 64KB_S layout, the real texels fall in bands of rows rather than any LUT
   shape.
4. No draw, dispatch (`APS5_TRACE_DISPATCH_IO`, also with `APS5_NO_DISPATCH_CACHE`), texture upload, flush or
   fill trace in the captured frames touches the LUT range. So it's written before the capture, during the city
   load.
5. In live runs the LUT isn't at `0x45474a0000` (that page is never mapped there), so the address has to be read
   from the final pass's descriptor in each run.

## Next steps

- Add a debug watch: read the LUT base from the final pass's T# (3D, 16x16x16, format 130) in a live run, then
  report the first write of `0xFF800000` into it and its source (CPU store, CP DMA / copy, compute or draw, clear
  or fill).
- Find out what the game expects to put there (a CPU upload, a GPU copy from a staging buffer, a blend pass) and
  why AnyPS5 ends up with -infinity in most texels. The -infinity pattern suggests a float buffer cleared or
  reduced over the same memory.

## Helpers

- `scripts/reset-test.ps1`: runs the game once with chosen files set aside, then restores them (used for the
  save data bisect).
- `scripts/skipscan.sh`: replay with one shader skipped per run, printing the average frame colour.
- `scripts/bmp.py`: reads replay BMP frames, prints the average colour and writes a small PNG.
- `scripts/tsharp.py`: decodes RDNA image descriptors from raw descriptor memory.
- `scripts/lut.py`, `scripts/ramp.py`: analyse the LUT dump and scan memory for colour ramps.
- `APS5_VERIFY_SHADER_CACHE=1` (this branch): on each disk cache hit, also compiles fresh and logs
  `[shader-cache-verify]` differences. `APS5_VERIFY_SHADER_CACHE_DUMP=<dir>` saves the differing modules.

Note: `Recompiler.cpp` changes the shader cache source version, so a build of this branch starts a new cache.

## Related

- Intermittent crash seen during this work: GabbeHags/AnyPS5#2 (`rectangle ABI refers to an unregistered shader`).
