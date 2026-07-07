<p align="center">
  <img src="configurator/FlowaGunSetup.ico" width="64" alt="XEMU LightGun Edition icon"/>
</p>

<h1 align="center">XEMU LightGun Edition — By Code Flow</h1>

<p align="center">
  A modified build of <a href="https://github.com/xemu-project/xemu">xemu</a> (Original Xbox emulator)
  with <b>native lightgun support</b>: Sinden Lightgun and any HID mouse gun.<br/>
  Born to make <b>Silent Scope Complete</b> playable — SS1, SS2 and SS3 all working,
  now at <b>full speed even on low-end PCs</b>.
</p>

<p align="center">
  <a href="https://github.com/flowa1911you/xemu-sinden-lightgun/releases"><b>⬇ DOWNLOAD (Releases)</b></a>
  &nbsp;·&nbsp;
  <a href="https://www.youtube.com/@flowachannel4731"><b>📺 YouTube channel</b></a>
  &nbsp;·&nbsp;
  <a href="CHANGELOG.md"><b>📋 Full changelog</b></a>
</p>

---

## 🚀 What's new in v3.0

- **Massive performance overhaul** — a deep profiling campaign inside the NV2A
  GPU core found and fixed a chain of per-draw CPU bottlenecks (texture state
  tracking, descriptor pools, bulk vertex processing and more). Per-frame CPU
  cost roughly **cut in half**: Silent Scope Complete went from a slideshow to
  its full frame rate cap on an Intel iGPU laptop. **Every game benefits.**
- **Fixed the long-standing random freeze when starting a game** — an ABBA
  deadlock between the disc DMA and the GPU thread, found with a debugger and
  fixed for good. Affected all games since forever.
- **Full button mapping** — new *Mapping* tab in the configurator: Trigger, B,
  X, Start, Back, D-Pad, plus the *Additional* pad inputs (Y, White, Black,
  stick clicks, analog triggers, Guide — for service/extra modes in games like
  Virtua Cop). Bind them to any button of **any detected mouse** or **any
  keyboard key**: click a slot, press the input within 10 seconds, done. The
  **aim is deliberately not mappable** — it always follows the raw input
  device selected in Players.
- **100% portable** — `xemu.toml` now lives next to the exe (native xemu
  portable mode) and game ISOs inside the folder are saved with relative
  paths. Copy the folder to a USB stick or another PC and it just works.
  Zero traces in AppData.
- **New startup splash** with auto-close, and the configurator rebranded to
  **Code Flow GunSetup v3.0**.

See [CHANGELOG.md](CHANGELOG.md) for the complete list.

## Features

- **Emulated EMS TopGun II lightgun** as a true low-level USB device
  (VID `0x0b9a` / PID `0x016b`, XID subtype `0x50`) — games genuinely detect a
  lightgun on the port, including in-game calibration
  (`XInputSetLightgunCalibration` reaches the emulated gun, like real hardware)
- **Per-device gun input, DemulShooter-style**: every HID pointer device is
  listed individually by name (Windows Raw Input API). Assign a specific gun to
  a specific player port — **two guns for 2-player games** work out of the box
- **Native absolute-coordinate aiming** (Sinden mouse mode), mapped to the
  actual game render area with correct offscreen (reload) reporting
- **Full button mapping** to any mouse or keyboard key (v3.0) — aim stays
  locked to the selected gun
- **Adaptive aim smoothing** (1-euro filter: steady crosshair when still, zero
  added latency on fast moves) + aim sensitivity setting
- **Code Flow GunSetup** — zero-install configurator: game ISO, gun-to-player
  assignment, button mapping, graphics settings, machine files status. One
  place for everything
- **100% portable package**: settings, BIOS, MCPX boot ROM, HDD image, EEPROM
  and game ISO all live inside the app folder
- **Lightgun-friendly UX**: always-hidden cursor, hideable menu bar, and guards
  so the trigger can't toggle fullscreen or open menus
- **Major NV2A performance fixes** (v3.0) and the **Silent Scope 2 Vulkan
  crash fix** (submitted upstream to the xemu project)

## Quick start

1. Download the latest zip from [Releases](https://github.com/flowa1911you/xemu-sinden-lightgun/releases) and extract it anywhere
2. Drop your files into the folders (each one contains a readme):
   - `bios\` — Xbox flash BIOS image
   - `mcpxbootrom\` — MCPX boot ROM
   - `harddisk\` — Xbox HDD image
   - `eeprom\` — optional (created automatically on first start)
3. Run **`FlowaGunSetup.exe`**: select your game ISO, assign your gun to
   Player 1 (already set to *Lightgun (EMS TopGun II)*), then **Save & Launch**
4. In game: **left click = trigger**, right click = B, middle click = Start —
   or remap everything in the **Mapping** tab

> ⚠️ BIOS, boot ROM, HDD image and games are **not included** — dump them from
> your own console and discs. The game region must match your BIOS/EEPROM
> region (EU ↔ EU, US ↔ US).

## Known limitations

- Gun-to-port bindings are tied to the physical USB port (like DemulShooter)
- ReShade users: hook xemu as **OpenGL** (the presentation layer is OpenGL
  even when the internal renderer is Vulkan)

## Source code

All modifications live in the [`flowa-lightgun`](https://github.com/flowa1911you/xemu-sinden-lightgun/tree/flowa-lightgun)
branch ([full diff vs upstream](https://github.com/flowa1911you/xemu-sinden-lightgun/compare/master...flowa-lightgun)).
Build like regular xemu (MSYS2 UCRT64 on Windows: `./build.sh`). The
configurator compiles with the C# compiler bundled with Windows.

## Credits

- Made by **Code Flow (flowa)** — [youtube.com/@flowachannel4731](https://www.youtube.com/@flowachannel4731).
  Subscribe so you don't miss future releases and updates!
- Based on [xemu](https://xemu.app) by Matt Borgerson and contributors —
  see also the upstream site for general documentation

## License

Same as upstream xemu (GPLv2). See [LICENSE](LICENSE).
