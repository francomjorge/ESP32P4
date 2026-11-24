# PRISMA RETRO / S3 — Build 003 (experimental)

Target: Guition ESP32-4848S040C, ESP32-S3 N16R8. 480×480 RGB ST7701, GT911, 16 MB flash, 8 MB octal PSRAM. USB power only; disconnect any mains-powered relay assembly before connecting USB.

Build 002 adds an experimental Nofrendo NES core to the validated 001 DIO40 Game Boy base, a console selector, and additional licensed ROMs. Build 001 is preserved on stable-s3-001-dio40. Physical validation of 002 is pending.

## Build
Fetch the pinned MIT-licensed header before compiling:
```sh
curl -fL https://raw.githubusercontent.com/deltabeard/Peanut-GB/d0bcca771c83a2638c93a9ae61f3d51f226dc905/peanut_gb.h -o src/peanut_gb.h
python tools/prepare_roms.py
bash tools/prepare_nes.sh
pio run
```
GitHub Actions performs these steps and merges the bootloader, partition table and application into a BIN at offset 0x0 using PlatformIO's actual build metadata. Download the artifact from the Actions run only when successful. Keep a backup of the previous firmware. The image targets 16 MB flash; the merged file need not physically occupy 16 MB.

## Build 003 changes
Boot directly to the Game Boy internal library; HOME opens system selector. SD scan only on request. Adds Tobu Tobu Girl Deluxe (MIT/CC BY 4.0, Tangram Games, music potato-tan) and Thwaite (GPL-3.0, Damian Yerrick); credits and licenses included. ROM counts and names logged to serial. CI asserts raw ESP image headers and DIO40 settings. Build 002 boot confirmed by user, ROM library issue not yet diagnosed on hardware.

## Use
Built-in titles: Big2Small, 2048, Breakout, Gamepad Test (GB), Platformer and NES Hello (NES). SD folders /roms/gb and /roms/nes. No audio or persistent saves yet. NES iNES NTSC only for this first port. Licenses and source links in dist/sd-card/manifest.json; Nofrendo source in dist/sources. The Nofrendo core is LGPL-2.0 as specified by upstream headers; source is pinned and included. Big2Small is GPL-3.0; corresponding source available at https://github.com/mdsteele/big2small/tree/v1.0.0.

## Hardware references
- https://github.com/jantielens/esp32-macropad/blob/49fd9b060fa4feb8b472cc49dfa869103faabedf/docs/dev/esp32-4848s040.md
- https://homeding.github.io/boards/esp32s3/panel-4848S040.htm
- https://github.com/moononournation/Arduino_GFX/tree/v1.6.4
- https://github.com/deltabeard/Peanut-GB/tree/d0bcca771c83a2638c93a9ae61f3d51f226dc905
- https://github.com/esp-cpp/esp-box-emu/tree/9908f4f439cf5b087d81718365e4791e2d2f4e5d

LCD command SPI CS39/SCK48/MOSI47. SD shares SCK48/MOSI47, MISO41/CS42; inactive CS high during LCD init. GT911 SDA19/SCL45, address 0x5D or 0x14. Backlight GPIO38. Do not touch relay pins GPIO1/2/40.

## Validation matrix
- CI compile / merge: pending
- Serial boot, flash and PSRAM detection: pending
- Display test and color order: pending
- GT911 orientation and multi-touch: pending
- SD FAT32 detection / rescan: pending
- DMG homebrew ROM execution and exit: pending
- FPS, tearing, heap and stability: pending

## Full ESP-BOX-EMU port
Upstream is tied to ESP32-S3-BOX's display, audio and custom gamepad, including board-dependent initialization and updater. Adapt its box-emu component and updater to a separate RGB/GT911 board HAL; validate memory budget, UI geometry, SD bus and gamepad mapping. Then bring up NES, GBC, SMS, Genesis, MSX and Doom individually. No claims of compatibility until tested. DOS is a separate x86 emulation project, not a feature of this build.

## Build 004
Adds experimental iNES mappers 225 and 227, including 225 nibble RAM and 227 CHR write-protect; solder-pad menu selection is not emulated. Optional private ROM pack uses existing storage partition at 0x610000, no user ROMs in repository. Raw factory image DIO40. Physical validation pending. Storage partition must not be formatted or used for a filesystem when a private pack is installed.

## Build 005 — performance experiment
Preserves stable-s3-004b (user confirmed working). 240 MHz CPU build setting, frame pacing includes input/rendering, adaptive display skipping, optimized 320x288 NES scaler runs only when presenting. NES PPU still runs every frame. Serial PERF every 3 s reports emulated/presented FPS, work time, overruns and free memory. DIO40, panel settings, private storage layout and mappers unchanged. No physical performance claims until measured.

## Build 006 — fullscreen performance
Confirmed stable 005 preserved in stable-s3-005. Default fullscreen 480x480 nearest-neighbor; FIT toggles square-pixel proportions. Touch controls and EXIT blended over live image. Native RGB565 buffers (NES 256x240, GB 160x144) scaled directly into existing Arduino RGB framebuffer using lookup tables and row reuse, with one explicit flush per presented frame. No extra fullscreen framebuffer. Mapper, DIO40, private partition layout and FPS diagnostics unchanged. Actual display FPS and tearing must be checked on device.

## Build 007 — regression recovery
User reported 006 alpha corruption and game stalls. Restore confirmed 005 emulator buffers, cores, pacing and classic SAFE rendering. FULL uses 16-row internal SRAM staging via Arduino_GFX draw16bitRGBBitmap; no direct scanout pointer or alpha blend, outline controls on game. Default full presentation at 30 FPS max, dynamically down to 20/15 if emulation falls behind. SAFE toggle restores 005-sized output without restarting. Serial logs separate draw cost and FPS. DIO40 and ROM partition unchanged. No claim of physical fix before testing.

## Build 008 — experimental Sega Mega Drive
Uses confirmed 005 graphics path, no 006/007 fullscreen changes. Gwenesis from Retro-Go pinned revision 4ced120, source + upstream license included. Adds SEGA tab, raw .md/.gen/.bin ROMs with SEGA header (up to 4 MiB) under /roms/md, ABC/Start controls, Z80 active, audio muted, 30 FPS max presentation; PAL frame period based on core mode. No save states, SMD, 32X or Sega CD. User ROMs are not committed. No physical compatibility or FPS claims.

## Build 009 — Sega performance
Preserves 008D on experimental-s3-008d and 005 stable. Optimize Gwenesis only at O2, prefer internal SRAM for indexed video, convert each unique source row once, adaptive Mega Drive frameskip 1–3, instrument CPU/VDP/scaler/LCD costs and correct MD diagnostic label. Same display path, Z80, ROMs and DIO40. Target normal game speed before arcade work; no physical FPS claims.

## Build 010 — native indexed display and fixed timeline
Golden Axe on 009 measured 42–44 emu fps / 10–11 displayed fps, CPU ~9 ms, VDP ~9 ms, scaling ~9 ms, LCD ~10 ms. Remove RGB565 intermediate and scaler for MD/NES; use Arduino_GFX indexed bitmap API (native 320x224/256x224 MD or 256x240 NES), retain GB 2x with callback skipped on unpresented frames. Fixed frame deadline allows cheap frames to catch up with expensive ones, bounded debt, adaptive frameskip on all consoles. No direct framebuffer writes, no transparency, no panel timing changes. Actual FPS not yet measured.

## Build 011 — asynchronous native presentation
Retains 010 and stable 005. LCD indexed presentation on core 0 using a single immutable snapshot with copied palette, nonblocking submission, menu drain, synchronous fallback on resource failure. MD starts divisor 3; promotion requires five healthy 3s windows; demotion quick. NES also uses async LCD; GB rendering unchanged. No timing or panel pin changes. Physical performance not verified.

## Build 012 — zero-copy Mega Drive presentation
Golden Axe 011 measured 55.5–58.2 emulation fps, 14–15 displayed fps, LCD worker 10.5–10.9 ms, submit copy 3.7–3.8 ms, no queue drops. MD owns two separately allocated indexed framebuffers (spare PSRAM). Submit borrows the rendered buffer and copies only palette, swaps render target, and drains worker before freeing. If spare allocation fails, use 011 copy path. NES and GB unchanged. Preserve 011 on experimental-s3-011. No performance claim until physical testing.

## Build 013 — measured frame budget
012 hardware log confirms 33–34 us zero-copy submission, no dropped jobs, 59.6 emu fps / 20 displayed fps in Golden Axe first window. Keep 012 as experimental-s3-012. Sega-only controller measures rendered vs skipped frame cost, predicts budgets for divisor 2/3/4, promotes gradually when sustained headroom exists, demotes on slow windows. VDP graphics translation unit O3 A/B experiment, CPU/bus/Z80 remain O2. NES/GB pacing and panel setup unchanged. Target 60 emu FPS with 20–30 displayed, no physical claims until validation.

## Build 014 — Z80 A/B performance mode
013 hardware log: 57.1–60.6 emu FPS, 14.3–15.3 displayed, 35–36 us submit; 30 FPS predicted average cost 18–20 ms. Preserve 013 as experimental-s3-013. Add SEGA library Z80 ON/OFF button, default ON. Selection takes effect only when opening a game, never changes CPU configuration during gameplay. OFF uses upstream Retro-Go style large Z80 clock sentinel and omits scheduled Z80 slices; this experimental mode can break games or cause hangs, so return to ON if any issue. Separate sampled 68k and explicit Z80 slice timing; 68k includes bus synchronization and instrumentation overhead. No change to video/panel/NES/GB/ROM data. No claim of improved performance until tested.

## Build 015 — audio synthesis diagnostics (no physical output)
Audio is a mandatory acceptance requirement for MD/NES/GB. Sega Z80 always ON in this build. PCM ON/OFF in SEGA library compares native YM2612 and SN76489 synthesis, synchronizes both at frame end and measures sample count, peak, mean absolute level and finalize/mix cost; full CPU timing includes synthesis during register access. No physical audio output: Guition 4848S040 speaker and relay variants share GPIO1/2/40; do not drive these until exact hardware routing and speaker confirmed. NES/GB audio not yet implemented. No audio/FPS hardware claims. Preserve 014 separately.

## Build 016 — video-first CPU performance
User prioritizes video stability and FPS; physical audio postponed. Default PCM OFF, Z80 ON. Existing PCM synthesis test remains available, no physical audio GPIO used. Try O3 for M68K and Z80 interpreter translation units in addition to existing VDP O3, without modifying instruction semantics or clock ratios. Preserve 015 and earlier baselines. Existing per-CPU timing, MD BUDGET, zero-copy and conservative frameskip retained. No hardware FPS improvement asserted until comparison.

## Build 017 — fixed cadence stability A/B
User reports stutter/freezing despite 016. Do not infer a cause from average FPS. MD menu mode cycles 20A (default fixed divisor 3 async), 20S (same cadence synchronous), 15A (async divisor 4), 30A (experimental async divisor 2). No automatic MD cadence changes. Z80 ON, PCM OFF. I2C transaction timeout 10 ms. Core0 monitor reports STALL if frame progress stops, phase 1 touch, 2 core, 3 submit/draw, 4 pacing/log. Max work measured each window. NES/GB core unchanged. 016 preserved. No claim of physical resolution.

## Build 018 — guest freeze isolation
User clarified game freezes while EXIT remains responsive. That suggests guest logic or image freeze rather than complete appliance stall. Default COMP mode enables YM2612/PSG synthesis/timers without physical output and renders every guest frame, presents every third synchronously to avoid worker involvement. PCM mode enables same timers but retains skipped rendering and async display. 20A/20S retain muted reference with async/sync display. COMP may be substantially slower; diagnostic not a performance baseline. MD STATE logs guest PC, SR, PC variation, sampled RGB image hash changes, VDP state and keys independently of LCD counters. No claims that frozen guest has been fixed; samples are heuristic not proof. NES/GB unchanged. No audio GPIO used.
