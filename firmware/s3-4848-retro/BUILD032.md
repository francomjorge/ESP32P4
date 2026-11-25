# BUILD 032 — DOS + NES performance

Stable baseline: 26d23bcf101c8c86b424166813f242f05470f828 (stable/build-031). Do not merge until hardware acceptance.

## Scope
- Preserve LCD timing, async video, dirty-rect menu and NES Nofrendo behavior from 031.
- Replace GB with DOS; exclude Mega Drive. No sprite redesign.
- Candidate: Adafruit Faux86; independently verify CPU, video modes, disk and input APIs before integration.
- DOS catalog: Prince of Persia, Wolfenstein 3D, Indiana Jones and the Last Crusade, Indiana Jones and the Temple of Doom, The Incredible Machine, Where in the World Is Carmen Sandiego? v2.2 (1991).
- Do not commit commercial game data; use local/private disk images.

## Integration gates
1. Pin compatible x86 core and dependencies; compile a standalone smoke test.
2. Implement DOS adapter with independent guest timing, start/stop, indexed video and keyboard/mouse input; reuse validated LCD path where supported.
3. Add DOS catalog and replace GB UI entry. Keep NES path unchanged.
4. Build 16 MB flash image using existing CI packaging; verify partition bounds and logs.
5. Hardware: DOS boot, each game, input, NES regression, return to menu, no flicker. Record guest speed, rendered FPS, core/video time, dropped frames, heap and PSRAM. No invented measurements.

Status: branch initialized; emulator integration and compilation not yet performed.
