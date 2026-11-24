# PRISMA RETRO / S3 — Build 001 (experimental)

Target: Guition ESP32-4848S040C, ESP32-S3 N16R8. 480×480 RGB ST7701, GT911, 16 MB flash, 8 MB octal PSRAM. USB power only; disconnect any mains-powered relay assembly before connecting USB.

This is a separate, initial Game Boy DMG emulator using the real Peanut-GB core, not a port of ESP-BOX-EMU yet. It is intended to validate the display, microSD and touch together before integrating other cores. Compilation and physical testing are pending until CI and board tests succeed. Four redistributable homebrew ROMs are generated and embedded in flash; no commercial ROMs are supplied.

## Build
Fetch the pinned MIT-licensed header before compiling:
```sh
curl -fL https://raw.githubusercontent.com/deltabeard/Peanut-GB/d0bcca771c83a2638c93a9ae61f3d51f226dc905/peanut_gb.h -o src/peanut_gb.h
python tools/prepare_roms.py
pio run
```
GitHub Actions performs these steps and merges the bootloader, partition table and application into a BIN at offset 0x0 using PlatformIO's actual build metadata. Download the artifact from the Actions run only when successful. Keep a backup of the previous firmware. The image targets 16 MB flash; the merged file need not physically occupy 16 MB.

## Use
The built-in games (2048, Breakout, TicTacToe, Gamepad Test) work without SD when firmware is validated. Their source licenses and SHA-256 manifest are included in the artifact. For additional games, format microSD FAT32, place legally obtained or homebrew .gb ROMs in /roms/gb, boot and select a title. Multi-touch controls and EXIT are provided. DMG only, no sound or persistent saves in 001, no Wi-Fi or OTA. The menu lists up to 128 ROMs, size 32 KB–4 MB.

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
