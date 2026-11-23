# Mafia: Definitive Edition red tint — handoff (2026-11-23)

Summary of everything found so far, for a fresh session. The run-by-run log with every detail is
[NOTES.md](NOTES.md); this file is the short version. Nothing here is fixed.

## The symptom and the angle to take next

- Mafia: Definitive Edition (PPSA36458) on AnyPS5: the whole 3D scene is tinted red (lights pink, sky purple). The HUD
  is fine. Still present on `main` at `6555c1425a`.
- **The trigger is the save game.** With the two saves `_sd\gaslo7dd33c65*` and `_sd\gasloc9e9842f*` in place the game
  loads straight into the open city and is red. With them set aside it starts at the intro, which is **not** red.
  (Found in the first session with `launchers\reset-test.ps1`; setting aside the profile, video settings, boot
  counter, `vfs_prospero.bin` or `shader_cache` changed nothing.)
- Suggested new angle: find out **why the intro / no-save path is not red**. Does it build its colour-grading LUT
  through the same path described below (compute pass, then `C_ColorGradingTextureUpdater` on the CPU)? If yes, what
  differs: inputs, timing, memory placement? If no, which path does it use? The lut-watch tooling below works for
  that comparison.

## What is wrong (established)

1. The final tone-map pass samples a **colour-grading LUT**: 3D texture, 16x16x16, format 130 (`R8G8B8A8_SRGB`),
   swizzle ZYXW, swizzle mode 9 (64KB_S), descriptor words `c8200000 0003c003 a0900f2e 0000000f 00700000` after the
   base. It lives in one 64 KiB slot. Its address moves between runs (`0x4524380000`, `0x4527240000`,
   `0x4537210000`-`0x4537250000`, `0x4538a70000`, ...).
2. In the city, about 3200-3270 of its 4096 texels are `0xff800000` (float `-inf`; through ZYXW it reads as dark
   red, R=128). The rest are negative NaNs with non-canonical payloads. Every texel has its top 9 bits set.
3. A second LUT exists earlier (menu / loading): all texels `0xff000000` (opaque black).
4. **The LUT is written by the game's own CPU code**, not by the GPU. A write watch caught
   `Mafia Definitive Edition.exe+0x3f8f342` / `+0x3f8f350` / `+0x3f8f35e` / `+0x3f8f372` (image base `0x3f0000`)
   storing `0xff800000` into it. That is a statically linked tiling loop copying 8-byte chunks from a linear CPU
   buffer (`r8`, e.g. `0x11fb1cc640`, heap memory) into the tiled 3D texture (`rdx`). Caller `+0x3f8c82f` is a
   generic "tile a surface region" helper. Above it: a per-mip upload loop (`+0x8d8742`) and
   `TextureUpdaterCache` (`+0x6e678f`, string `TextureUpdaterCache Type %u %ux%ux%u, %u mips 0x%X`).
5. The producer class is **`C_ColorGradingTextureUpdater`** (type registration at `+0x80c6b0`, type id
   `0x30513043`), with input `T057_ColorGradingInput` / `m_ColorGradingInputTexture` (code refs `+0x1af38d0`,
   `+0x1af3c50`). Other related strings: `Disable color-grading Post Fx`, `m_HdrColorGradingMode`,
   `E_SM_ESSV_UPDATE_LUT_CS`.
6. So the `-inf` values are **already in the CPU staging buffer** when the tiler copies it. The tiler, the GPU-side
   handling of the LUT memory and the final pass are not at fault.

## Strong lead, not fully explained

- Every time a 16^3 LUT appears, it comes right after a compute dispatch (16x1x1 groups) that reads a tiled
  `256x16` RGBA8 texture (`0x46f236f000` / `0x46f15b9000` / `0x46f15d9000`, t2) and writes a linear `256x16` RGBA8
  storage image (4096 texels = a 16^3 LUT unwrapped): menu `0x48ea68fe00`-ish, city `0x48e9...` (varies), plus a
  0x30-byte constant buffer.
- When dumped at the LUT's first decode, the **city compute output is a clean LUT** (smooth ramps from 0,0,0 to
  255,255,255, 4096 distinct colours, alpha 0xff) with **no value in common** with the bad 3D LUT. The menu output
  is also full of colours while the menu LUT is black.
- At dispatch time (before write-back) the output memory reads `nz=0.00` in the menu and `nz=1.00` (old contents) in
  the city. The city output lies inside an older 2048x2048 R32F linear storage image (16 MiB, e.g. `0x48e9683e00`)
  that a compute pass filled from the shadow map and that the game reads back on the CPU. Its float data
  (`-inf`, 1.0f ...) matches the kind of values in the bad LUT.
- That looked like a stale CPU read (CPU reads the output before AnyPS5 writes the GPU result back), **but**:
  - `APS5_SYNC_DISPATCH=1` (with `[flush]` showing the output written back "for readback before label" just before
    the LUT appears) did **not** fix it, and turned the tint red-and-yellow;
  - the strictest ordering (`coherent`: `APS5_SYNC_DISPATCH`, `APS5_STORE_AT_LABEL=1`,
    `APS5_DRAIN_COMPLETION_LABELS`, `APS5_LABEL_GATE_ALL`, `APS5_DRAIN_ALL`) did **not** fix it either (LUT 3237
    `0xff800000`), and in that run the city output held no LUT-like data at LUT time;
  - no DMA copy, DMA_DATA, COPY_DATA or packet store of 4-64 KiB occurs in a run, so nothing on the GPU side copies
    the output into the heap staging buffer;
  - it was never shown that the CPU staging buffer is actually filled from that compute output. The link is timing
    and size only.
- Open question: **what fills the CPU staging buffer**, and from what? The staging address isn't stable between
  runs, so a fixed-address watch on it failed.

## Ruled out

- Shader disk cache (`APS5_VERIFY_SHADER_CACHE` found no difference), pipeline journal, disk plans,
  `agc-sampled-surface-import`, 3D swizzle / layout, physical aliasing.
- Every AnyPS5 cache and replay path (`nocache`: dispatch/draw caches, recipes, memos, resource plans): still red.
- `v_cvt_pk_u8_f32` (masks correctly; upstream measured it against hardware) and the compute shader translation
  (its output is a correct LUT).
- **Storage-image write-back byte merge** (`writeBackLayers` → `WriteChangedCommitted`, which overwrites bytes the guest
  changed since upload): real, and it writes the LUT's neighbouring slots, but no write-back touched the LUT
  (`keepguest` experiment: still red).
- **Stale depth-plane store**: AnyPS5 does copy a stale 2048x2048 D32 shadow-map plane unit (`StoreDepthPlane`) over
  the LUT's 64 KiB about a second before first use, because CPU-only write tracking never retires it. But dropping
  those stores (`dropdepth`) only changed the **padding** (1.0f → zero); the 4096 texels stayed bad. A real AnyPS5
  bug worth its own issue, but not the tint.
- DMA / COPY_DATA / packet stores into the LUT.
- Label / result ordering (`sync`, `coherent`).

## Side effects seen

- Page-protecting the LUT's slot row (`APS5_WATCH_RANGE`) and `sync` both turn the tint **red and yellow** (yellow
  sky, `anyps5-screenshots\PPSA36458-20261123-203228.png`, `-205905.png`): something else in the sky / grading is
  timing- or write-tracking-sensitive. The 128x16 RGBA16F textures next to the LUT, read by compute every frame,
  are candidates.
- One crash with the driver-exempt range watch: `AGC graphics: unsupported vertex format 83` (garbage vertex buffer
  descriptor). Not reproduced on an identical rerun. Filed as GabbeHags/AnyPS5#3, probably caused by the watch.
- Older intermittent crash: GabbeHags/AnyPS5#2 (`rectangle ABI refers to an unregistered shader`).

## Where things are

- Repo: `D:\dev\ps5\AnyPS5`, branch `debug/mafia-red-tint` on `GabbeHags/AnyPS5` (two commits on `main` at
  `6555c1425a`; the debug aids below are in `f6e4d0f0e6`). The checkout is shared with the user's other work
  (branch `agc-sampled-surface-import`), so check the branch before switching.
- Game: `D:\dev\ps5\games\Mafia Definitive Edition`. Install a build with
  `cp -a build/core/libs/libs/. "<game>/libs/"` (originals in `libs-backup-before-main\`). Build with
  `ninja -C build` and `D:/winlibs_mingw/mingw64/bin` on `PATH`.
- Launchers: `Mafia Definitive Edition (offline).cmd` (normal) and `Mafia Definitive Edition (test N-lut-watch).cmd`
  (arguments, any order: `io`, `nocache`, `keepguest`, `dropdepth`, `sync`, `coherent`). Each run writes
  `logs\<date-time>-lut-watch[-args]\` with `run.log` and dumps. Older launchers (bisect tests A-M, Z, capture,
  profile) and `reset-test.ps1` are in `launchers\`. Old logs are in `logs\archive-2026-11-23-bisect\`.
- Capture from the first session: `D:\dev\ps5\captures\mafia-city` (replays red).

### Debug aids (all off by default)

| Switch | Where | What |
|---|---|---|
| `APS5_TRACE_LUT_WATCH=1` | `Graphics/src/GuestTextureResource.cpp` (`DebugLutWatch*`), hooks in `DecodeTextureResource`, `DecodeColorBuffer`, `decodeDepthPlane`, `StoreDepthPlane`, `writeBackLayers`, `PacketMemory.cpp`, `Pm4.cpp` | `[lut-watch] first ...` for small 3D / 16x16 / format-130 views; contents and dumps of each 16^3 LUT (`lut-watch-<addr>-NN.bin`); `overlap` history (views, depth/colour metadata, depth stores, write-backs, DMA / packet stores) at the LUT's first sight; `covers a LUT`; dumps of all `256x16` RGBA8 views at LUT time (`lut-watch-src-*.bin`); every 4-64 KiB DMA / COPY_DATA / packet store |
| `APS5_TRACE_LUT_WATCH_DIR` | same | dump folder (launcher sets it) |
| `APS5_STORAGE_WRITEBACK_KEEP_GUEST=1` | `Texture.cpp` `writeBackLayers`, `GuestMemory::WriteChangedCommittedKeepingGuest` | write-back keeps guest-changed bytes (experiment, no effect on the tint) |
| `APS5_DEPTH_DROP_STALE_STORE=1` | `DepthSurface.cpp` (`FlushDepthPlanes`, `retireStoringStale`) | drops stale depth units instead of storing them (experiment; can break effects) |
| `APS5_WATCH_RANGE=<64K-aligned hex>:<hex bytes>` | libc `CrashReport.cpp` | write-protects the first page of each 64 KiB slot, reports writers with stacks, separate fault budget for driver writes. Disturbs rendering, may have caused #3 |
| `APS5_VERIFY_SHADER_CACHE=1` | `Recompiler.cpp` (committed) | compares disk-cache hits with fresh compiles |

Existing AnyPS5 aids that were useful: `APS5_WATCH_ADDR=<hex>` (one-page CPU write watch with stack),
`APS5_TRACE_DISPATCH_IO`, `APS5_TRACE_FLUSH`, `APS5_SKIP_SHADER`, `APS5_DUMP_DRAW_SHADERS`, `agc_frame_replay`.

### Scripts (`investigations/mafia-red-tint/scripts/`)

`lut.py`, `lutcompare.py` (compare two 64 KiB LUT dumps), `iocover.py` (`[dispatch-io]` resources covering an
address), `framestrings.py` (strings referenced near game code offsets, to name stripped stack frames),
`xref.py` (code references to a string), `tsharp.py` (decode image descriptors), `skipscan.sh`, `bmp.py`, `ramp.py`,
`reset-test.ps1`. To symbolise driver offsets: `nm -C` on `build/core/libs/libs/libSceAgcDriver.prx`
(image base `0x298e10000`). To disassemble game offsets: `objdump -d` on the exe with address `0x3f0000 + offset`.
