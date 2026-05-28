# EarthBound PPU — pending optimization ideas

Profile baseline: with `PPU_PROFILE=1` the FPS overlay reports BG ≈ 50% and
CLR ≈ 25% of per-frame PPU time. Ideas below are roughly ordered by
expected payoff vs. effort. Source line references are to
`external/earthbound/src/snes/ppu_render.c` unless otherwise noted.

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
- **Action taken:** reverted EB sources to `-O2` in `Makefile.common`
  (`earthbound_obj_prereq_gen`). **Do not** re-bump to `-O3` until the wild
  store is root-caused.
- **How to root-cause when revisiting:** the BSOD now prints `CFSR`/`HFSR`/`BFAR`
  (added to `BSOD()` in `Core/Src/main.c`). Use `gdb` single-step — each step
  drains the store buffer, making the fault precise so `BFAR` pins the address.
  Start from the file-select-confirm handler down through `reset_party_state()`
  in `overworld.c`.

## Pending wins

### 1. DTCM placement of per-scanline working buffers

The buffers at `ppu_render.c:844-861` (line_out, best_bg_color,
best_bg_gp_lm, sub_bg_color/gp, obj_color/prio, eff_tm/ts_line,
cm_prevented_line, the temp_* set) are accessed every pixel and memset
every scanline. They total ~7 KB and currently sit in RAM_EMU (AXI
SRAM, cached). DTCM is single-cycle dual-ported for data — eliminates
d-cache lookups and AXI contention with code fetches.

Mechanism (already wired up upstream):
- `-DPPU_LINEBUF_ATTR='__attribute__((section(".dtcm_eb")))'` in
  `C_DEFS_EARTHBOUND`.
- New `.dtcm_eb (NOLOAD)` output section between `.bss` and
  `._user_heap` in `STM32H7B0VBTx_SDCARD.ld`.

Blocker: only ~1.5 KB of DTCM padding is currently free (.map shows
`__dtc_padding_end__ - __dtc_padding_start__ = 0x5F0`). Need to free
~7 KB by one of:
- Shrinking `_Heap_Size` (currently 85K; PICO-8 needs 64K single
  allocation, ~13K headroom would remain).
- Shrinking `_Min_Stack_Size` (currently 24K).
- Accepting only a hottest-subset that fits in 1.5K (e.g.
  best_bg_gp_lm + cgram_render).

Expected gain: ~3-4× CLR; 15-30% BG (per-pixel store is the hot path).

### 2. AoS-pack `main_color[]` + `main_gp_lm[]` into one `uint32_t`

In `emit_tile_run` (`ppu_render.c:243-247`), every winning pixel does
two 16-bit stores to two separate arrays:

```c
ctx->main_gp_lm[_sx] = gp_lm;
ctx->main_color[_sx] = _rgb;
```

Pack into one `uint32_t` (color in upper half, gp_lm in lower):

```c
ctx->main_pixel[_sx] = ((uint32_t)_rgb << 16) | gp_lm;
```

The priority-check load above also collapses to one `LDR`. Same trick
for `sub_bg_color`+`sub_bg_gp` (sub_bg_gp is uint8 — pack into a uint32
with color in the high bits).

Expected gain: 10-15% on BG. Touches upstream `ppu_render.c`; portable,
helps the Pico port too.

### 3. ITCM placement of hot PPU functions

Move `render_bg_scanline`, `decode_*bpp_row`, `render_obj_scanline`,
`precompute_window_masks`, `ppu_render_frame_ex` to ITCMRAM (64 KB
unused per `STM32H7B0VBTx_SDCARD.ld:129`). Zero-wait-state I-bus
fetch frees AXI bandwidth for data accesses.

Scaffolding already in place (current branch):
- `.itcm_eb_text` section in linker, VMA in ITCMRAM, LMA via `AT > CORES`.
- `PPU_RAM_SECTION='".itcm_eb_text"'` in `C_DEFS_EARTHBOUND`.
- `objcopy --only-section=.itcm_eb_text` extracts `earthbound.itcm`.
- `sdpush` rule for `earthbound.itcm`.
- `__itcm_eb_text_*` and `_ITCM_EB_TEXT_SIZE` declared in `gw_linker.h`.

Needs runtime wiring in `app_main_earthbound`:
- `odroid_overlay_cache_file_in_ram("/roms/homebrew/earthbound.itcm",
  (uint8_t *)__itcm_eb_text_start__)` before `PatchCodeRodataOffset`.
- `SCB_InvalidateICache_by_Addr` over the loaded range.
- Extend `PatchRodataRange` call to also scan
  `[__itcm_eb_text_start__, __itcm_eb_text_end__)` for 0xCAFE.... refs
  in the freshly loaded literal pools.
- Halt with a clear message if the file is missing — code is linked
  at ITCM addresses, so the section must be present.

Build verified: section is 15.4 KB. Linker auto-inserts long-call
veneers (`__memset_veneer`, `__platform_timer_ticks_veneer`, 8 B each)
for cross-region calls.

Expected gain: 10-20% on BG and OBJ.

### 4. Skip `memset(line_out, 0)` in wide mode

`ppu_render.c:1077` clears 640 B per scanline. In wide mode
(`fb_x_offset == 0 && render_width == EB_VIEWPORT_WIDTH`) the
compositing loop fully overwrites the line — the clear is dead work.
640 B × 240 scanlines ≈ 150 KB of redundant memset per frame.

One-line guard. Expected gain: 5-10% off CLR.

### 5. Combine the per-scanline memsets

CLR currently issues up to 4 separate memsets (best_bg_gp_lm, sub_bg_gp,
obj_prio, line_out). If laid out contiguously in BSS, one larger memset
is meaningfully faster than four small ones (per-call setup amortized,
better burst sizes).

Either rely on careful BSS symbol ordering (fragile) or wrap into a
single per-frame `linebuf` struct in `ppu_render.c`. Also opens the
door to a custom 64-bit-unrolled memset for these specific buffers.

Expected gain: 5-10% off CLR. Stacks with #1 (memset to DTCM is
already fast; combining helps cache-resident memset most).

## Lower-priority / verify-first

### 6. Hoist `hflip` out of EMIT_PIXELS

`ppu_render.c:236` has a per-pixel `hflip ? (7 - (start_px + _i)) : ...`
branch on a loop-invariant. -O3 may already hoist this — check the
.lst before adding macro complexity. If not, split EMIT_PIXELS into
hflip/no-hflip variants (4 forms combined with HAS_SUB).

### 7. Specialize `emit_tile_run` on bpp

`bpp` is a parameter (already `always_inline`'d). The compiler should
const-propagate through inlining at -O3. Verify in the .lst before
hand-specializing.

### 8. Improve tile-row cache hash

`ppu_render.c:203`: `((tile_addr >> 1) ^ pixel_y) & TILE_CACHE_MASK`.
Direct-mapped 256-entry; collisions on neighboring rows are common.
A multiplicative hash (`(tile_addr * 0x9E3779B1u + pixel_y) >> 24`)
typically distributes better. Net win depends on the current hit rate
(stated ≥85% in the comment) — measure first.

### 9. Verify sub-screen elimination when color math is off

The `EMIT_PIXELS(HAS_SUB=0)` macro path (`ppu_render.c:258-261`) should
fully eliminate sub-screen work when `ctx->sub_gp == NULL`. Confirm in
the .lst that no spurious sub-buffer accesses survive — if they don't,
nothing to do here.

### 10. Verify window-mask cache hit rate

`precompute_window_masks` already has a memcmp-based cache key
(`win_cache_key`, `ppu_render.c:1117-1126`). If every scanline misses
the cache, the WIN phase is paying full price each line. Quick PMU
instrumentation would settle it.

## Notes / instrumentation

- The PPU profile timings come from `platform_timer_ticks()`
  (`Core/Src/porting/earthbound/gw_timer.c`). Verify it isn't itself
  a non-trivial fraction of measured time — called many times per
  scanline.
- The Cortex-M7 PMU can count cache misses directly
  (`DWT_CTRL`/`DWT_CYCCNT` + event counters). If a specific theory needs
  verification (e.g. "is d-cache thrashing the per-line buffers?"),
  wire one event into the existing PPU_PROFILE struct.
- Useful one-off measurements before committing to #1:
  - `arm-none-eabi-size build/earthbound/ppu_render.o` — code size of
    the hot translation unit; informs i-cache pressure.
  - `__bss_end__` of `.overlay_earthbound_bss` — working set, informs
    d-cache pressure.
