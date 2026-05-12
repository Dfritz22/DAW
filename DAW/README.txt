DAW - Quick Start
=================

To run:
  Double-click daw_gui.exe

Requirements:
  - Windows 10 or Windows 11 (64-bit)
  - An audio output device

If Windows SmartScreen warns about an unrecognized app:
  - Click "More info" -> "Run anyway"
  (This happens because the .exe is not code-signed.)

If it still won't start, install the Microsoft Visual C++
Redistributable (x64):
  https://aka.ms/vs/17/release/vc_redist.x64.exe

Audio tips:
  - Default output uses WASAPI Shared mode (works with anything else
    using the speakers).
  - For lowest latency, switch to WASAPI Exclusive in the device menu.
    Note: Exclusive mode takes over the device - other apps won't be
    able to use it while DAW is running.

Files in this folder:
  daw_gui.exe          - The application
  MSVCP140.dll         - Visual C++ runtime
  VCRUNTIME140.dll     - Visual C++ runtime
  VCRUNTIME140_1.dll   - Visual C++ runtime
  README.txt           - This file
