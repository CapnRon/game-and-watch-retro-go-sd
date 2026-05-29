# EarthBound PPU — optimization log & pending ideas

Profile baseline: with `PPU_PROFILE=1` the FPS overlay reports BG ≈ 50% and
CLR ≈ 25% of per-frame PPU time. Source line references are to
`external/earthbound/src/snes/ppu_render.c` unless otherwise noted.

## Shipped gains — the real bottleneck was vsync quantization

EB's original ~20 fps cap was **vsync quantization**, not PPU compute. Two
commits on `earthbound` (shipped) prove it:

- `127a20f3` triple-buffering: **20 → 28 fps**
- `7fc78e11` `-O3` on `ppu_render.c` only: **28 → 30.3 fps**
- **#4 skip `line_out` clear in wide mode** (below): **30.3 → 33.3 fps**
  (CLR 50 → 30, TOTAL 238 → 217 in 0.1 ms units).

Key lesson from this work: **eliminating work beats relocating it.** Every
attempt to move the *same* work to faster memory (ITCM/DTCM, see "Reverted")
landed in the noise; the only post-vsync wins came from *removing* work
(`-O3` codegen, and #4's dead memset). EB is otherwise **compute-bound** at
280 MHz — three full-width layers composited in software with no single hotspot
that memory-placement or cache tuning can fix. When hunting further gains, look
for redundant/dead work, not faster homes for existing work.

**The remaining real levers are not micro-optimizations:**
- **Frameskip.** zelda3 — the same per-scanline software-PPU architecture, also
  capped at ~30 fps — ships `ZELDA3_LIMIT_30FPS=1` (renders every other game
  frame). Deferred so far in favor of compute headroom, but ITCM/DTCM/packing
  experiments have now shown there is no compute headroom to find.
- **Algorithmic PPU changes** (reduce per-pixel/per-tile work, not relocate it).

## Reverted: EarthBound-wide -O3

- Compiling EarthBound sources at `-O3` instead of `-O2` (for speed) grew
  `EarthBound.bin` ~36% but still fit RAM_EMU.
- Booted fine to the title/file-select screen, but **BSODs immediately** when
  the overworld loads (after a file is selected).
- On-screen `Hardfault PC=velocity_store` is **misleading**. Real status:
  `CFSR=0x00000400` (BFSR IMPRECISERR), `BFAR=0` invalid, `HFSR=0x40000000`
  (FORCED) — an imprecise bus fault. A wild store drained through the
  Cortex-M7 store buffer late; the reported PC (inside `velocity_store`'s many
  cacheable SRAM stores) is innocent.
- Root cause: a **latent UB in the file-load path** (likely an uninitialized
  pointer landing on a bus-rejecting address) that `-O3` codegen exposes but
  `-O2` leaves benign. Adding `-fno-strict-aliasing` did **not** fix it, so it
  is not an aliasing bug.
- **Action taken:** EB sources stay at `-O2` in `Makefile.common`
  (`earthbound_obj_prereq_gen`); only `ppu_render.c` is forced to `-O3` via
  `PPU_FORCE_SPEED_OPT` (that TU is not on the file-load path). **Do not**
  re-bump the whole EB build to `-O3` until the wild store is root-caused.
- **How to root-cause when revisiting:** the BSOD now prints `CFSR`/`HFSR`/`BFAR`
  (added to `BSOD()` in `Core/Src/main.c`). Use `gdb` single-step — each step
  drains the store buffer, making the fault precise so `BFAR` pins the address.
  Start from the file-select-confirm handler down through `reset_party_state()`
  in `overworld.c`.

## Reverted: ITCM placement (both code and VRAM) — measured dead end

Two hardware A/B tests on the G&W (60-frame BGPROF average, overworld; values
in 0.1 ms units as reported by `PPU_PROFILE`):

| Config | TOTAL | BG |
|--------|-------|-----|
| **A (baseline)** — all code/data in cacheable AXI-SRAM | 238 | 118 |
| **B — VRAM → ITCM** (64 KB buffer) | 234 | **120** |
| **C — hot code → ITCM** (`ppu_render.o .text`, ~16 KB) | 239 | 119 |

All three are within run-to-run jitter (~2% of ~24 ms). In config B the BG read
path it targets actually got *worse* (118 → 120). **ITCM is a confirmed dead end
for EB FPS** — the render is neither VRAM-read-latency-bound (B) nor
I-cache-eviction-bound (C; the hot code was already I-cache-resident). Both
experiments were fully reverted (no residual changes in `external/earthbound`
or the firmware).

Implementation notes, in case ITCM is ever revisited for a *different* reason
(e.g. freeing RAM_EMU, not speed):

- **VRAM → ITCM:** gate `ppu.vram` on `PPU_VRAM_EXTERNAL` (pointer vs inline
  array, keep it the FIRST struct field); `ppu_init` must preserve+zero the
  pointer **unconditionally** (ITCM base is `0x0`, so a valid buffer can have
  address 0 — never NULL-test it); the port calls `itc_calloc(1,0x10000)` +
  `ppu_set_vram()` (failure sentinel is `0xffffffff`, not NULL). The 13
  `sizeof(ppu.vram)` bounds checks (sprite/overworld/map_loader) MUST become
  `VRAM_SIZE` or a pointer silently collapses them to 4 and drops VRAM writes.
- **Code → ITCM:** a linker `.eb_itcram` section (VMA=ITCM, `AT > CORES`)
  selecting `build/earthbound/ppu_render.o (.text .text*)`, placed *before*
  `.overlay_earthbound` so first-match pulls it out of the overlay (no
  EXCLUDE_FILE). Ship it as a separate `EarthBound_itcm.bin`; at boot stage it
  through RAM then `memcpy` → ITCM (the SD path may DMA, and **ITCM is not
  DMA-reachable**) + `DSB`/`ISB`. The linker auto-inserts long-call veneers
  (`__printf_veneer`, `__memset_veneer`, …) at the end of the section.
- **★ Critical gotcha (cost a BSOD):** `PatchCodeRodataOffset` in
  `main_earthbound.c` scans **RAM_EMU only**. `.rodata_earthbound` (printf
  format strings, LUTs, the asset blob) loads to a per-boot-varying address and
  every literal-pool pointer to it is rewritten by that scan. Any `.o` whose
  code/data you move OUT of `.overlay_earthbound` is no longer scanned, so its
  rodata pointers keep the `0xCAFF..` link address → BSOD with PC inside newlib
  `_vfiprintf_r` (garbage format string). Fix: re-run `PatchRodataRange()` over
  the relocated region with the SAME base/offset. (Now documented in the
  `PatchCodeRodataOffset` comment, commit `ddbfd4a9`.)

## Reverted: DTCM placement of per-scanline buffers (was idea #1)

The per-scanline working buffers (`line_out`, `best_bg_color/gp_lm`,
`sub_bg_*`, `obj_*`, `eff_tm/ts_line`, `cm_prevented_line`, the `temp_*` set,
~7 KB) were moved to DTCM. **Zero measurable gain.** They are touched every
pixel of every scanline, so they are *already* permanently d-cache resident in
their AXI-SRAM home — an M7 cache hit is ~1 cycle = DTCM speed, and there were
no misses to remove. The "AXI contention with code fetch" rationale is also
near-zero since hot loops run from the I-cache. (Only ~1.5 KB of DTCM padding
was free anyway.) Scaffolding (`.dtcm_eb` section, `-DPPU_LINEBUF_ATTR`) removed.

## Reverted: AoS-packed pixel stores (was idea #2)

`main_color[]` + `main_gp_lm[]` packed into one `uint32_t` per pixel. On
hardware this was a **net loss**: the per-scanline clear doubled (memset
2 → 4 B/pixel, since packing forces clearing the color half too), while the
priority check was already a single load — so the second store just became a
pack `orr` and the M7 store buffer had already been pipelining the two narrow
stores. CLR got worse, BG unchanged. Reverted.

## Forward-looking ideas (untested or lower-priority)

These are per-pixel/per-scanline *work reductions* (not relocations), which is
the category that might still help a compute-bound renderer. Measure each
against the BGPROF baseline before committing.

### 4. Skip `memset(line_out, 0)` in wide mode — ✅ DONE (30.3 → 33.3 fps)

In wide mode (`fb_x_offset == 0 && render_width == EB_VIEWPORT_WIDTH`) the
compositor writes every output pixel (verified: the composite loop covers
`[0, EB_VIEWPORT_WIDTH)` with no per-pixel skips), so the per-scanline
`line_out` clear only ever painted left/right borders that don't exist there.
Guarded the clear on a hoisted loop-invariant (`line_out_full_cover`). Removed
640 B × 240 ≈ 150 KB/frame of memset. **Measured: CLR 50 → 30, TOTAL 238 → 217,
FPS 30.3 → 33.3 (+10%).** The vertical-border-scanline clear and the
non-wide/centered path are untouched.

### 5. Combine the per-scanline memsets — ❌ NOT WORTH IT (declined)

The idea was to fold CLR's separate memsets into one. But the remaining clears
are *already* conditionally skipped — `sub_bg_gp` only on color-math scanlines
(rare), `obj_prio` only when sprites are enabled. Those `if`s ARE the
work-elimination this item wanted. Combining into one contiguous unconditional
memset would FORCE the usually-skipped `sub_bg_gp` clear on most scanlines —
adding work (the same failure mode as the reverted AoS-pack). A careful combine
that preserves the conditionals saves only ~one memset *call* setup per scanline
(the zeroing *work* is irreducible) = sub-noise, while adding fragile BSS-layout
coupling to the hot path. After #4 took the one real work-elimination in CLR,
nothing here is worth the risk.

### 6. Hoist `hflip` out of EMIT_PIXELS

`ppu_render.c:236` has a per-pixel `hflip ? (7 - (start_px + _i)) : ...` branch
on a loop-invariant. `ppu_render.c` is built at `-O3`, which may already hoist
it — check the `.lst` before adding macro complexity. If not, split EMIT_PIXELS
into hflip/no-hflip variants (4 forms combined with HAS_SUB).

### 7. Specialize `emit_tile_run` on bpp

`bpp` is a parameter (already `always_inline`'d). The compiler should
const-propagate through inlining at `-O3`. Verify in the `.lst` before
hand-specializing.

### 8. Improve tile-row cache hash

`ppu_render.c:203`: `((tile_addr >> 1) ^ pixel_y) & TILE_CACHE_MASK`.
Direct-mapped 256-entry; collisions on neighboring rows are common. A
multiplicative hash (`(tile_addr * 0x9E3779B1u + pixel_y) >> 24`) typically
distributes better. Net win depends on the current hit rate (BGPROF reports
~66% on the overworld) — measure first.

### 9. Verify sub-screen elimination when color math is off

The `EMIT_PIXELS(HAS_SUB=0)` macro path (`ppu_render.c:258-261`) should fully
eliminate sub-screen work when `ctx->sub_gp == NULL`. Confirm in the `.lst`
that no spurious sub-buffer accesses survive — if they don't, nothing to do.

### 10. Verify window-mask cache hit rate

`precompute_window_masks` already has a memcmp-based cache key (`win_cache_key`,
`ppu_render.c:1117-1126`). If every scanline misses the cache, the WIN phase is
paying full price each line. Quick PMU instrumentation would settle it.

## Notes / instrumentation

- The PPU profile timings come from `platform_timer_ticks()`
  (`Core/Src/porting/earthbound/gw_timer.c`). Verify it isn't itself a
  non-trivial fraction of measured time — called many times per scanline.
- The Cortex-M7 PMU can count cache misses directly (`DWT_CTRL`/`DWT_CYCCNT` +
  event counters). If a specific theory needs verification (e.g. "is d-cache
  thrashing the per-line buffers?"), wire one event into the PPU_PROFILE struct.
  Given the ITCM/DTCM results above, **wire the PMU before trying any further
  memory-placement idea** — blind placement tuning has a perfect failure record
  here.
- Useful one-off measurements:
  - `arm-none-eabi-size build/earthbound/ppu_render.o` — code size of the hot
    translation unit; informs i-cache pressure (it is ~16 KB = the full I-cache).
  - `__bss_end__` of `.overlay_earthbound_bss` — working set, informs d-cache
    pressure.
