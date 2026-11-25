# BetterDimmer

Native Win32 tray app for software dimming of the **primary display**. It changes the GPU gamma LUT, not the monitor backlight. No overlay, DDC/CI, installer, or separate runtime. It does not register itself to start with Windows.

| Shortcut | Action |
| --- | --- |
| Ctrl+Alt+Win+Up | Increase brightness (up to the original level) |
| Ctrl+Alt+Win+Down | Decrease brightness |
| Ctrl+Alt+Win+Left | Decrease gamma (darker midtones) |
| Ctrl+Alt+Win+Right | Increase gamma (lighter midtones) |
| Ctrl+Alt+Win+Home | Reset both controls |

Click the tray icon for the same actions and **Exit and restore display**. Unavailable shortcuts are listed in the menu; the corresponding menu actions still work.

Successful changes are saved under HKCU\Software\BetterDimmer and reapplied on the next launch. Reset saves neutral settings; exiting restores the original ramp without discarding saved settings. Forced termination cannot guarantee restoration. Brightness spans 50–100% of the original signal and gamma spans 0.70–1.50. Drivers may accept only part of these ranges.

## Try it on the AOC

Make the AOC the Windows primary display and turn off HDR. Close any running BetterDimmer instance. From the folder with BetterDimmer.exe, run in PowerShell:

```powershell
(Start-Process -FilePath (Join-Path $PWD 'BetterDimmer.exe') -ArgumentList '--probe' -Wait -PassThru).ExitCode
```

The probe writes a 90% ramp, checks driver readback, and immediately restores the previous ramp. Exit codes: **0** accepted and restored, **2** another instance is running, **3** the ramp could not be set or verified, **4** restoration could not be verified, **1** initialization failed. Use `--check` for a read-only check. After the probe, launch the app and check Alt+Tab and SwashMoji visually while dimmed. Use Reset or Exit to restore normal output.

[Microsoft documents](https://learn.microsoft.com/windows/win32/api/wingdi/nf-wingdi-setdevicegammaramp) that drivers can silently reject ramps, display changes can clear them, and HDR and color calibration interactions are undefined. BetterDimmer compares driver readback, retries saved levels after a display change or wake, and avoids deliberately replacing a ramp another app has set when exiting. Readback does not prove the rendered output changes. The effect can extend to cloned displays or outputs that share a gamma LUT.

## Build

Use CMake 3.20+ with MSVC or MinGW-w64 on Windows:

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The executable is at `build\Release\BetterDimmer.exe` with Visual Studio, or `build\BetterDimmer.exe` with a single-configuration generator. The automated test checks the curve math without changing the desktop.
