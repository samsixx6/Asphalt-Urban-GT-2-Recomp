# Windows x64

Extract the entire Windows release ZIP and double-click `Asphalt2.exe`. Windows
10 1903+ or Windows 11 x64 and an OpenGL 3.3 graphics driver are required. Keep
`avcodec-61.dll`, `avutil-59.dll` and `swscale-8.dll` beside the executable.

The first launch asks for your supported original ULES00719 ISO. The player
verifies the ISO and remembers its location. Keep the ISO available; moving or
removing it returns to selection. The Windows ZIP contains no original ISO,
BOOT, extracted game assets or startup ELF. Players need no Python, compiler or
external emulator.

Settings, saves, cache and logs use `%LOCALAPPDATA%/Asphalt2Recomp`. In gameplay, W/Up accelerates, S/Down brakes, A/Left and D/Right steer, and Space uses NOS.
In menus, arrows or WASD navigate and Space confirms. Enter pauses gameplay. XInput
controllers are supported. F11 toggles fullscreen. The window can
be resized while preserving the original aspect ratio. Close it to exit.
Use the in-game Recomp Settings for native 30/60 and display/input preferences.

For a launch error, read the displayed message and log in the data folder. A
graphics error may require the GPU vendor's current OpenGL-capable driver.

Fresh Windows profiles default to 4x resolution, Device native aspect, 60 FPS,
Auto Accelerate Off, FPS Counter Off and Original UI. Existing saved settings
are retained. Android keeps its separate factory defaults.

## Build from source

Use Python 3.12+ and the pinned portable w64devkit x64 2.11.0 toolchain (GCC
16.2.0). Follow [building.md](building.md) for dependency setup and supported ISO
preparation, then run from the prototype source directory:

```powershell
python tools/prepare_release_inputs.py --iso "D:\Games\original.iso"
python tools/build_windows.py
python tools/package_windows.py
```

The builder compiles the current shared game, runtime, display and desktop
sources. All Windows objects, checked registration splits, derived display
hooks, FFmpeg build files and logs stay under `build/windows/`; Android objects
and APKs use their existing independent paths. Game code keeps `-O1`, sibling
call optimization and disabled FP contraction. Strict unsupported-state traps
remain active. It never invokes an emulator or automated gameplay script.

`build/windows/build.json` records source hashes, compiler, registration count,
architecture, imported DLLs and output hashes. Packaging checks those inputs
again, audits all bundled DLLs and reads the ZIP back. The default release is
`build/releases/Asphalt-Urban-GT-2-v1.0-windows-x64.zip`.

The resource script uses `windows/Asphalt2.ico` and `windows/app.manifest`; an
ICO supplied at build time is compiled directly. No artwork hash is enforced
by the generic builder. The application is a GUI x64 executable with its
Windows compatibility/DPI manifest embedded.

## FFmpeg source and replacement

The player links the public FFmpeg API through shared DLLs. FFmpeg 7.1.4 is built
locally from the exact upstream source archive, SHA-256
`71f4aac3573ed9060489cb62526a6c7dda815ae10993789611acd7be9fa9fbf4`.
The minimal configuration enables H.264, ATRAC3/ATRAC3+ and swscale, disables
network/autodetection and enables no GPL/nonfree components. Existing static
and Android FFmpeg builds are left in their original directories.

The Windows ZIP includes that source archive, the exact build recipe and
configuration, LGPL text, FFmpeg notices, psprecomp/adapted-code notices and
MinGW runtime notices. The three compatible x64 DLLs can be replaced beside the
executable without relinking the player. `ffmpeg-source/REBUILD.txt` gives the
standalone rebuild command. No runtime check prevents using modified DLLs.
