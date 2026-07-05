<p align="center">
  <img src="configurator/FlowaGunSetup.ico" width="64" alt="flowa's xemu icon"/>
</p>

<h1 align="center">flowa's xemu — Sinden Lightgun Edition</h1>

<p align="center">
  A modified build of <a href="https://github.com/xemu-project/xemu">xemu</a> (Original Xbox emulator)
  with <b>native lightgun support</b>: Sinden Lightgun and any HID mouse gun.<br/>
  Born to make <b>Silent Scope Complete</b> playable — SS1, SS2 and SS3 all working.<br/>
  Created in collaboration with the <b>Light Gun Lunatics</b> community.
</p>

<p align="center">
  <a href="https://github.com/flowa1911you/xemu-sinden-lightgun/releases"><b>⬇ DOWNLOAD (Releases)</b></a>
  &nbsp;·&nbsp;
  <a href="https://www.youtube.com/@flowachannel4731"><b>📺 YouTube channel</b></a>
</p>

---

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
- **Adaptive aim smoothing** (1-euro filter: steady crosshair when still, zero
  added latency on fast moves) + aim sensitivity setting
- **FlowaGunSetup** — zero-install configurator: game ISO, gun-to-player
  assignment, graphics settings, machine files status. One place for everything
- **Fully portable package**: BIOS, MCPX boot ROM, HDD image and EEPROM are
  auto-detected from folders next to the executable
- **Lightgun-friendly UX**: always-hidden cursor, hideable menu bar, and guards
  so the trigger can't toggle fullscreen or open menus
- **Fixed the Vulkan renderer crash in Silent Scope 2** (also submitted
  upstream to the xemu project)

## Quick start

1. Download the latest zip from [Releases](https://github.com/flowa1911you/xemu-sinden-lightgun/releases) and extract it anywhere
2. Drop your files into the folders (each one contains a readme):
   - `bios\` — Xbox flash BIOS image
   - `mcpxbootrom\` — MCPX boot ROM
   - `harddisk\` — Xbox HDD image
   - `eeprom\` — optional (created automatically on first start)
3. Run **`FlowaGunSetup.exe`**: select your game ISO, assign your gun to
   Player 1 (already set to *Lightgun (EMS TopGun II)*), then **Save & Launch**
4. In game: **left click = trigger**, right click = B, middle click = Start

> ⚠️ BIOS, boot ROM, HDD image and games are **not included** — dump them from
> your own console and discs. The game region must match your BIOS/EEPROM
> region (EU ↔ EU, US ↔ US).

## Known limitations (beta)

- Silent Scope 3 runs below full speed even on strong hardware — an inherent
  limit of xemu's low-level GPU emulation, not of the lightgun integration
- Gun-to-port bindings are tied to the physical USB port (like DemulShooter)
- ReShade users: hook xemu as **OpenGL** (the presentation layer is OpenGL
  even when the internal renderer is Vulkan)

## Source code

All modifications live in the [`flowa-lightgun`](https://github.com/flowa1911you/xemu-sinden-lightgun/tree/flowa-lightgun)
branch ([full diff vs upstream](https://github.com/flowa1911you/xemu-sinden-lightgun/compare/master...flowa-lightgun)).
Build like regular xemu (MSYS2 UCRT64 on Windows: `./build.sh`). The
configurator compiles with the C# compiler bundled with Windows.

## Credits

- Made by **flowa** — [youtube.com/@flowachannel4731](https://www.youtube.com/@flowachannel4731).
  Subscribe so you don't miss future releases and updates!
- Created in collaboration with the **Light Gun Lunatics** community — my
  personal thanks to all the fantastic people there
- Based on [xemu](https://xemu.app) by Matt Borgerson and contributors —
  see also the upstream site for general documentation

## License

Same as upstream xemu (GPLv2). See [LICENSE](LICENSE).
