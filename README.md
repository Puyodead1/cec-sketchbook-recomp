# Sketch Book static recompilation

Static recompilation of the Sketch Book photo-booth game (`v8mnu22b`, i386 Linux ELF) into a native Win32 executable.

The original game binary and data are **not** included. Supply them yourself from the game disc.

## Layout

| Path | What |
|---|---|
| `recomp.py` | Translates the i386 ELF into C (`gen/`). Uses `analyze.py` for disassembly and function discovery. |
| `analyze.py` | Recursive-descent disassembly and function discovery, used by `recomp.py`. |
| `rt/` | Win32 runtime: CPU state, libc and syscall shims, X11 to GDI window, sound, camera, UIB buttons, printer. |
| `build.ps1` | Compiles `gen/` and `rt/` with MinGW gcc into `v8mnu22b.exe`. |
| `buildtool.ps1` | Recompiles the helper programs in `vend\bin` (tgascale, voronoi, …) the same way. |
| `fixlinks.ps1` | Replaces the WSL symlinks in `vend\` with junctions and copies that Win32 can open. |
| `package.ps1`, `dist/README.txt` | Builds `release\SketchBook-win32.7z` (exe, trimmed `vend\`, end-user README). |
| `linux/` | Reference setup for running the original game under WSL. `uibemu.py` emulates the UIB button/coin box (serial protocol); `dongle/dongle.c` is an `LD_PRELOAD` parallel-port dongle emulator (built and installed by `dongle/install.sh`); `dongle/dictkey.py` decrypts `dict.db`; `run_vend.sh` starts the game in a Fedora Core 5 rootfs. Set `ROOTFS` to the extracted rootfs before running them. |

## Building

Requirements: Python 3 with `capstone`, and MinGW gcc in `C:\MinGW\bin`.

1. Put the game binary at `v8mnu22b.elf` and the extracted game data in `vend\`.
2. Run `powershell -ExecutionPolicy Bypass -File fixlinks.ps1`.
3. Run `python recomp.py v8mnu22b.elf gen`.
4. Run `powershell -ExecutionPolicy Bypass -File build.ps1`.
5. Optional, for the helper tools: `powershell -ExecutionPolicy Bypass -File buildtool.ps1`.
6. Optional, for the release archive (needs `7z` on PATH): `powershell -ExecutionPolicy Bypass -File package.ps1`.

## Running

Run `v8mnu22b.exe`. It looks for the game data in `vend\` next to the exe; set `VEND_ROOT` to use another folder.

- **Controls:** `C` adds a coin. `1`–`8` are the cabinet buttons.
- **Fullscreen:** `F11` or `Alt+Enter` toggles it. Start with `--fullscreen` or set `VEND_FULLSCREEN=1`. The picture keeps its 4:3 shape with black bars.
- **Camera picture:** to sketch a picture instead of the camera, do any of these:
  - drop it on the window
  - give it on the command line
  - set `RT_CAMERA=<file>`
- **Printer output:** "prints" are saved as PNGs in `prints\` next to the exe.
