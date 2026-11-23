# Mafia: Definitive Edition (PPSA36458): red tint in the open city

Work-in-progress investigation branch. Not meant for a PR as is: `investigations/` breaks the repository rule of
adding no files beyond code, tests and debt fragments, and the `Recompiler.cpp` change is a debug aid.

Status (2026-11-23, evening): the LUT never receives its colour-grading data in guest memory; the city LUT
holds leftovers of whatever used that memory before. Not fixed. Reproduces on `main` at `a182406ffa` and at
`6555c1425a` (re-tested after rebasing this branch onto it).

## Live LUT watch (APS5_TRACE_LUT_WATCH)

Debug aid on this branch: `DebugLutWatch` in `Graphics/src/GuestTextureResource.cpp`, called from
`DecodeTextureResource` and `DecodeColorBuffer`. With `APS5_TRACE_LUT_WATCH=1` it logs `[lut-watch] first ...`
for small 3D, 16x16-ish and format-130 views, keeps a history of every distinct texture and color-target view,
and for each 16x16x16 volume prints the earlier views that overlap it (`[lut-watch] overlap ...`) and its
contents whenever they change, dumping each version to `lut-watch-<addr>-NN.bin` in
`APS5_TRACE_LUT_WATCH_DIR`. Game-folder launcher: `Mafia Definitive Edition (test N-lut-watch).cmd`
(`io` as its argument also sets `APS5_TRACE_DISPATCH_IO`).

First run (`logs\20261123-192445-lut-watch`, before the overlap history existed):

- Two 16x16x16 format-130 LUTs, same descriptor apart from the base (`c8200000 0003c003 a0900f2e 0000000f
  00700000`, swizzle mode 9): `0x4540470000` first, then the city one at `0x4538a70000`. Addresses differ
  between runs (the capture had `0x45474a0000`), so a fixed-address watch does not work.
- Format 130 is `R8G8B8A8_SRGB`. The 4096 texel slots of the 64 KiB block are the first 4096 dwords
  (`scripts/lutcompare.py`).
- `0x4540470000` is entirely `0xff000000` (opaque black) in its slots and zero padding: initialised, never
  filled.
- `0x4538a70000`: the padding is all `0x3f800000` (1.0f) and every slot has its top 9 bits set: 3254 are
  exactly `0xff800000` (-inf), the rest negative NaNs with non-canonical payloads (`0xfffda107` ...). It looks
  like a float buffer's old contents with bytes over it, not anything written as a LUT.
- Both LUTs already held these values at their first decode, and neither changed afterwards. So no draw or
  dispatch that binds the LUT as an image wrote them, and nothing later fills them in guest memory.
- Ruled out: `v_cvt_pk_u8_f32` (it masks the byte correctly, and upstream measured it against hardware).

Second run (`logs\20261123-193311-lut-watch`, with `io`): the same picture. The first LUT (`0x453ec00000`) is
all `0xff000000`, and the city LUT (`0x4537220000`, first decoded at 72.8 s) has 3253 `0xff800000` and 1.0f
padding. `scripts/iocover.py` finds no `[dispatch-io]` resource covering the LUT's 64 KiB. The LUT sits in a run
of 64 KiB slots between `128x16` format-71 (`R16G16B16A16_SFLOAT`) textures at `0x4537210000`, `0x4537230000`
and `0x4537250000`, which compute passes read, just above a `1024x576` R16F image ending at `0x4537200000`.
The `overlap` lines are too wide to be useful: they assume 16 bytes per texel, so every screen-size target
"overlaps".

The watch has a gap: dispatch and draw caches, recipes and disk plans replay work without decoding
descriptors, so a cached writer never reaches the hooks. The launcher's `nocache` argument turns those off.
If the tint goes away with `nocache`, a cache is to blame. If it stays, the hooks now see every view.

Third run (`logs\20261123-193724-lut-watch-io-nocache`, with `io nocache`): still red, with the same
contents. The city LUT is again at `0x4537220000`, so with the same build its address is stable between
runs. With every cache off, no decoded texture or color-target view and none of the 7318 traced dispatches
touches the LUT. So it isn't a cache, and no GPU view of that memory exists. Its data has to come from a CP
packet or from the CPU, or the real writer is missing from what AnyPS5 runs.

Fourth run (`logs\20261123-194053-lut-watch`, `APS5_WATCH_ADDR=4537220000`): the LUT moved one slot up, to
`0x4537230000`, so the watch caught its neighbour's slot instead, which is still useful. That memory is written on
the CPU by the driver itself, byte by byte (stores to `+2`, `+3`, `+6`, `+7`), in two bursts: first float32
values (`0x3f800000`, `0x00800000`), then half floats (`0x3c00`, `0x38c0`). Stack (symbolised with `nm` on the
installed `libSceAgcDriver.prx`, image base `0x298e10000`):

    GuestMemory::writeChanged          <- WriteChangedCommitted
    StorageTexture::writeBackLayers    <- writeBack <- FlushPending

Fifth run (`logs\20261123-195507-lut-watch-keepguest`, `keepguest`): **still red**, and no storage write-back
touched the LUT (`0x4537230000` again) before or after its first decode. So the write-back below is real but is
not what corrupts the LUT. It wrote the neighbouring slot in run four. The keep-guest switch stays on the branch
as a harmless experiment.

New suspect: surface metadata. The bad texels look like HTILE / DCC words rather than colours
(`0xfffc9f95`, `0xfffc03f1`, `0xfffc0022`), the neighbouring `128x16` textures keep their DCC in the same slot
row (`0x4537204000`, `0x453720e000`), and recent upstream commits change how HTILE is sized and placed. The watch
now records exact ranges of depth planes, stencil planes and HTILE (`decodeDepthPlane`), color DCC / CMASK /
FMASK (`DecodeColorBuffer`) and texture DCC / CMASK (`DecodeTextureResource`) through `DebugLutWatchRange`. Any
of them overlapping a LUT is printed in its `overlap` list, or as `[lut-watch] at ... covers LUT` once the LUT is
known.

Sixth run (`logs\20261123-200733-lut-watch`): the city LUT, at `0x4537240000` this time, lies inside the
**depth plane of a 2048x2048 D32_SFLOAT target** (`0x4536bd0000..0x4537bd0000`, HTILE at `0x4537bd0000`),
probably a shadow map, used at 47.2 s during loading. The same 16 MiB is also seen as a 2048x2048 R32F texture
and a 1920x1080 texture, so the game uses it as transient memory. No metadata range overlaps the LUT.

Suspect: AnyPS5 keeps depth planes on the GPU and marks their 64 KiB units stale, then stores a stale unit into
guest memory when something reads over it (`DepthSurface::StoreDepthPlane`, units sized in `SizeUnits`;
`APS5_DEPTH_PLANE_WHOLE_STORE` stores whole planes instead). The bad LUT block is exactly one 64 KiB unit of that
plane, and its 1.0f padding fits a cleared depth plane. If the depth owner isn't retired when the game reuses
the memory for the LUT, reading the LUT pulls the old depth unit over it. Upstream `57e8c7c527` fixed that kind
of missed retirement for multisample depth only (`GuestReplacedSincePublication` returns false for
`samples == 1`).

The watch now logs every `StoreDepthPlane` unit (`depth store ... of stale depth plane ...`) and prints, up to
40 times, any recorded range (depth plane uses included) that covers a LUT, as `[lut-watch] at ... covers a
LUT`.

Seventh run (`logs\20261123-201242-lut-watch`): **confirmed.** The city LUT is at `0x4537210000`, and at
71.830 s `StoreDepthPlane` stores `0x4537210000..0x4537220000`, exactly the LUT's 64 KiB, from the stale
2048x2048 depth plane `0x4536bd0000` (last used as a depth target at 47.05 s). The LUT's first decode follows
at 72.9 s. So AnyPS5 keeps that depth plane's units stale for 25 s while the game reuses the memory, then copies
old depth data over the LUT.

Why the unit was still stale: both store paths (`retireStoringStale` and the read path around
`StoreDepthPlane` in `DepthSurface.cpp`) drop units the guest wrote using `WrittenUnits` /
`GuestMemory::CollectWrites`, which only sees CPU writes. Packet stores (`Driver::commitStore`) and DMA copies
(`CopyBuffer` on the GPU) never touch stale depth planes. If the game uploads the LUT with a DMA / COPY_DATA
packet, nothing clears the unit. The watch now records `dma copy`, `dma direct`, `copy data` and `packet store`
ranges (`PacketMemory.cpp`, `Pm4.cpp`) to show which of these wrote the LUT.

Likely fix (once the writer is known): any guest-visible write that isn't a CPU store (packet store, DMA copy,
and also storage image and render target writes) over a stale depth unit has to drop that unit (`ClearStale`)
instead of leaving it to be stored later, as `DepthReadCause::Write` already does for buffer writes on
multisample planes.

Eighth run (`logs\20261123-202050-lut-watch`): the LUT is at `0x4537220000`, the stale depth store hits it
again (72.740 s, unit `0x4537220000..0x4537230000`), and the first decode follows at 73.8 s. **No** DMA copy,
driver DMA_DATA, COPY_DATA or packet store covers the LUT, so the LUT isn't uploaded through those packets.

`FlushDepthPlanes` stores synchronously: `SyncFromMemory` drops units the CPU wrote (`WrittenUnits`), then the
rest is stored and flushed at once, before the access goes ahead. So the stored 64 KiB is the depth plane's own
stale data, not a merge with something written later. Either the game's write of the LUT wasn't tracked, or the
LUT data arrives by a route none of the hooks see. Open question.

Experiment: `APS5_DEPTH_DROP_STALE_STORE=1` (launcher argument `dropdepth`) drops stale depth units instead of
storing them, in both `FlushDepthPlanes` and `retireStoringStale`, with up to 16 `[depth-drop]` lines. If the
city colours come back, the stale store is the whole cause. It can break effects that read depth through guest
memory, so it is not a fix.

Ninth run (`logs\20261123-202537-lut-watch-dropdepth`, `dropdepth`): **still red.** The LUT, at
`0x4537250000`, now has **zero** padding (12288 zero dwords) instead of 1.0f, but its 4096 texel slots are as
bad as before (3235 `0xff800000`, 861 other). So the stale depth store only wrote the padding, and it isn't the
cause. The bad texels are what the LUT's own writer stores. No GPU view, dispatch, draw, DMA or packet ever
writes it, so the writer runs on the CPU: game code or one of the reimplemented system libraries. The menu LUT
is all `0xff000000`, which fits a CPU routine that builds the LUT from inputs (zeros in the menu, bad values in
the city).

Next: `APS5_WATCH_RANGE=<64 KiB-aligned hex address>:<hex bytes>` (new, in libc's `CrashReport.cpp`) write-protects
the first page of every 64 KiB slot in the range. It reports the first 4 writes into each slot with frame-pointer
and stack return addresses (`[range-watch]`) and up to 12 writes per slot that leave `0xff800000` or `0xff000000`
behind. A slot is released after 400 faults. Run with `APS5_WATCH_RANGE=4537200000:80000` to cover the slots the
LUT has used so far (`0x4537210000` to `0x4537250000`). Caveat: the protection may hide writes from AnyPS5's own
write tracking on those pages.

Tenth run (`logs\20261123-203050-lut-watch`, `APS5_WATCH_RANGE=4537200000:80000`): the picture changed from red
to **red and yellow** (yellow sky, `anyps5-screenshots\PPSA36458-20261123-203228.png`). So the watch's page
protection changes what is rendered. It probably hides writes from AnyPS5's own write tracking for the textures
that share those slots (the `128x16` RGBA16F images that compute passes read every frame), which hints that
they also feed the grading. The LUT (`0x4537210000`, first decode at 70.2 s) still holds 3257 `0xff800000`.
Every slot used up its 400-fault budget on the driver's own byte-by-byte write-back
(`libSceAgcDriver.prx+0x175618`, the `writeChanged` path) before the LUT was written, so the game's writer
wasn't seen. The watch now gives driver writes a separate budget of 200000 and reports only the first one
per slot; non-driver writes get the 400 budget and 6 full reports.

Eleventh run (`logs\20261123-203421-lut-watch`, range watch with driver writes exempt from the budget): the game
aborted at about 79 s with `AGC graphics: unsupported vertex format 83` (`VertexInput.hpp:100`). 83 isn't a gfx10
buffer format, so a vertex buffer descriptor was garbage. It has appeared in only this run of about 20, so the
watch most likely caused it. Filed as GabbeHags/AnyPS5#3. The range watch doesn't work as is: either it misses the
writer (fault budget) or it disturbs the game too much. Run without it to confirm the crash goes away.

Twelfth run (`logs\20261123-203921-lut-watch`, same range watch, no crash; the rerun is noted on GabbeHags/AnyPS5#3):
**the writer is the game's own CPU code.** The LUT was at `0x4537230000`. At 9782330 ms (host clock) thread 3616
stores `0xff800000` into it from `Mafia Definitive Edition.exe+0x3f8f342` / `+0x3f8f350` / `+0x3f8f35e` / `+0x3f8f372`
(image base `0x3f0000`). The stores go to offsets `0x0`, `0x40`, `0x200`, `0x240`, `0x8`, ..., with `rdx` = LUT
base and `r8` = `0x11fb1cc640` advancing by `0x40`. That code is a statically linked tiling loop: it reads 8-byte
chunks linearly from `r8` and stores them at `rdx + (table[rdi] ^ [rsi])`. Its caller `+0x3f8c82f` is a generic
"tile a surface region" helper looping over slices (source = base + slice * pitch). Engine stack above it:
`+0x3f8c544`, `+0x8d8742`, `+0x8dda00`, `+0x93a996`, `+0x93bdd7`, `+0x6e678f`, `+0x808fb3`, which looks like a
texture upload.

So the engine builds the LUT as a linear RGBA8 image in an ordinary CPU buffer (around `0x11fb1cc640`, not GPU
memory) and tiles it into place. The `0xff800000` texels are already in that source buffer. The tiler and
AnyPS5's GPU-memory handling are not the cause. What fills the source buffer is the open question. Candidates:
game code that computes the LUT using a function AnyPS5 reimplements (math in libc, for example a
`powf`/`log2f`/`expf` returning `-inf`), or data loaded from disk through a reimplemented file or
decompression path (for example AMPR / APR).

Walking the frames (`scripts/framestrings.py` lists the strings each frame's function references):
`+0x8d8742` is a per-mip upload loop (source in `r15`, advancing by each mip's size; texture object fields
`0x50`/`0x54` width/height, `0x80` depth, `0x78` mip count), and `+0x6e678f` references
`TextureUpdaterCache Type %u %ux%ux%u, %u mips 0x%X`. So the LUT is a dynamic texture updated from the CPU through
the engine's TextureUpdaterCache: the engine fills a CPU staging buffer and the updater tiles it into GPU memory.

Thirteenth run (`logs\20261123-204934-lut-watch`, `APS5_WATCH_ADDR=11fb1cc640`): no `[watch]` lines at all, so the
staging buffer landed elsewhere. Its address isn't stable.

The game's strings name the producer: `C_ColorGradingTextureUpdater` (type registration at `+0x80c6b0`, type id
`0x30513043`), with an input `T057_ColorGradingInput` / `m_ColorGradingInputTexture` (code refs `+0x1af38d0`,
`+0x1af3c50`). Strings and references come from `scripts/xref.py`. In the `io nocache` run the city LUT's first
decode (line 16342) comes right after a compute dispatch (line 16341) that reads a tiled `256x16` RGBA8 texture
(`0x46f15b9000`) and writes a linear `256x16` RGBA8 storage image (`0x48e9a9c000`, 16 KiB = 4096 texels = an
unwrapped 16^3 LUT). The menu had the same dispatch at line 14368, and its output was all zero (`nz=0.00`), which
gives exactly the all-black menu LUT with alpha forced to 0xff. Likely chain: a compute shader writes the LUT
unwrapped to 256x16, then the CPU updater reads it and tiles it into the 3D texture. Either the shader computes
`0xff800000` (a shader translation bug) or the CPU reads that output before AnyPS5 writes it back (stale memory).

The watch now dumps the first 16 KiB of every `256x16` RGBA8 view seen so far (`lut-watch-src-<addr>-for-<lut>.bin`)
when a 16^3 LUT first appears, with `[lut-watch] source for ...` counts of `0xff800000`, zero and alpha-0xff dwords.

Fourteenth run (`logs\20261123-205423-lut-watch-io`): **root cause found.** The city LUT (`0x4527240000`) again
comes right after the compute dispatch writing `256x16` RGBA8 storage `0x48e9aed000`. At LUT time that output holds
a clean LUT (0,0,0 to 255,255,255 ramps, 4096 distinct colours, alpha 0xff everywhere, no `0xff800000`) with **no
value in common** with the 3D LUT. The menu's output (`0x48ea68fe00`) is also full of colours, while the menu
LUT is all black. The dispatch trace, printed right after each dispatch and before write-back, shows the output
memory as `nz=0.00` for the menu and `nz=1.00` (old contents) for the city. So:

- the compute shader is correct (AnyPS5 translates it fine);
- the game's CPU code (`C_ColorGradingTextureUpdater` through TextureUpdaterCache) reads the output memory
  **before AnyPS5 has written the storage image's GPU results back**, and tiles those stale bytes into the 3D LUT:
  zeros in the menu (black LUT), leftover float data in the city (`-inf`, the red tint);
- AnyPS5 then writes the result back, too late to matter.

Fifteenth run (`logs\20261123-205725-lut-watch-sync`, `sync`): **red and yellow**, like the range-watch run. The LUT
(`0x4538a70000`) is still bad (3268 `0xff800000`) and the menu LUT is still black, although `[flush]` shows the
city output (`0x48e9acf000`) written back "for readback before label" (line 15694) just before the LUT appears.
So the result does reach the output memory in time, and the plain stale read below is not the whole story.

- The tiler's source is a **heap** buffer (near `0x11fb1cc640`), not the `0x48e9...` output. Something copies the
  output into that staging buffer first.
- The output memory lies inside an older 2048x2048 R32F linear storage image (`0x48e9683e00` / `0x48e9685e00`,
  16 MiB) that a compute pass filled from the shadow map, and that the game also reads back on the CPU
  ("readback before label", line 14276). That is where the `-inf` / 1.0f values come from.
- Current suspect: the game copies the output into the heap staging buffer with a DMA / COPY_DATA queued after the
  dispatch, and AnyPS5 executes that copy before the dispatch's results land. It then copies the old contents:
  zeros in the menu (black LUT), shadow R32F data in the city (`-inf`). `sync` waits for dispatches, not for that
  copy, which would explain why it didn't help.

The watch now prints every DMA copy, driver-executed DMA_DATA (with its source) and COPY_DATA / packet store of
4-64 KiB (`[lut-watch] at ... dma copy 0x...: DMA copy from 0x...`), up to 600 lines.

Sixteenth run (`logs\20261123-210207-lut-watch-io`): **no** DMA copy, driver DMA_DATA, COPY_DATA or packet store of
4-64 KiB in the whole run, so no GPU copy moves the output into the heap staging buffer; the CPU reads the output
itself. The city LUT (`0x4524380000`, 3190 `0xff800000`) is first decoded at line 16905, right after the dispatch
writing output `0x48e9c2cd00` is recorded (16904, `nz=1.00` before write-back). So the game records the dispatch,
waits on its completion, reads the output on the CPU and tiles the LUT, all before the final pass. Its read still
beats AnyPS5's write-back of the result.

Next: launcher argument `coherent` sets the strictest ordering AnyPS5 has (`APS5_SYNC_DISPATCH`,
`APS5_STORE_AT_LABEL=1`, `APS5_DRAIN_COMPLETION_LABELS`, `APS5_LABEL_GATE_ALL`, `APS5_DRAIN_ALL`, plus
`APS5_TRACE_FLUSH`). Colours right means label/result ordering is the bug, and dropping switches one at a time
names the mechanism. Still red means ordering isn't it.

Seventeenth run (`logs\20261123-210539-lut-watch-coherent`, `coherent`): **still red.** The city LUT
(`0x4538a70000`) has 3237 `0xff800000`, and the city output (`0x48e9ae3000`) held no LUT-like data at LUT time
(zero opaque texels). Ordering is not the cause. Investigation handed over; see [HANDOFF.md](HANDOFF.md).

(Superseded hypothesis, disproved by runs fifteen to seventeen:) This is a CPU/GPU coherence bug: the game waits for the dispatch (probably on a label or fence) and then reads
the output, but the storage image's results are still pending and the CPU read doesn't force them out. Test:
launcher argument `sync` (`APS5_SYNC_DISPATCH=1` writes results back after every dispatch, and
`APS5_TRACE_FLUSH=1` names what flushes them). If the colours come back, the fix belongs where pending storage
results must be published before the CPU can observe the dispatch's completion (label / fence writes, or the
CPU read trap on the output pages).

### Earlier diagnosis (ruled out for the LUT by run five)

`writeBackLayers` stores a storage image's GPU results with `GuestMemory::WriteChangedCommitted`. That
writes every byte where the result differs from `original`, the snapshot the image was uploaded from, *even
if the guest wrote that byte since*. The buffer write-backs (`GuestBufferMemory.cpp`) use
`WriteChangedKeepingGuest` instead, and the upload path (`Texture.cpp`, "guest memory changed while GPU
results were pending; keeping the CPU's blocks") protects CPU writes. The flush path doesn't.

The likely sequence: a float storage image used the LUT's 64 KiB slot, and its results stayed pending in
AnyPS5. The game then reused the slot for the LUT and wrote it. Later a flush stores the stale results over
it, byte by byte, so the LUT ends up as a mix of the game's bytes and stale float bytes. That fits everything
above: every texel has its top 9 bits set, the padding is 1.0f, no GPU view ever writes the LUT, and the
contents are already bad at its first decode. On hardware those GPU writes landed long before the game
reused the memory.

Experiment on this branch: `APS5_STORAGE_WRITEBACK_KEEP_GUEST=1` (launcher argument `keepguest`) makes
`writeBackLayers` use the new `GuestMemory::WriteChangedCommittedKeepingGuest`. The watch also records each
storage write-back, logging `[lut-watch] at ... write-back ... covers LUT` when one lands on a known LUT and
adding them to the `overlap` history. If the tint goes away with `keepguest`, the diagnosis holds. The proper
fix then needs thought: byte comparison can't tell a guest write that happens to equal the snapshot from no
write, so it probably belongs at block level, with the write watch, like the upload path.

Earlier plan: the second run's `overlap` lines name every earlier view of the LUT memory, which shows its previous
user and any writer that sees it with another shape (2D array, 16x256 or a storage view). With `io`,
`[dispatch-io]` buffer writes covering the LUT's range name compute passes that write it as a buffer. If none
exists, the data must come through a CP DMA / copy, a GPU-side result that never reaches guest memory, or the
CPU.

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
