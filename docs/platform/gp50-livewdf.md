# LiveWDF on GP-50

The model pack runs the LiveWDF DOD 250 and MXR Micro Amp circuit models
with optional cabinet, echo and stereo reverb. The board build targets analog
audio at 44.1 kHz. Test it in emulation while the hardware bring-up proceeds
in [OpenGP5 #12](https://github.com/OpenGP5/OpenGP5/issues/12).

Install the [OpenGP5 toolchains](setup.md), CMake and a C++20 compiler. Build
with the LiveWDF source branch for the model pack.

```sh
git clone -b feat/gp50-model-pack https://github.com/Yahiake/LiveWDF.git
tools/build_gp50_livewdf.sh LiveWDF
python3 tools/stock_control_emu.py --gp50-app build/GP-50_livewdf/control.elf
python3 tools/dsp_emu_check.py build/GP-50_livewdf/audio.dsp --gp50-raw
```

Use the generated firmware source in the
[desktop emulator](https://github.com/jopdorp/OpenGP5-Emulator) for live JACK
audio and a GP-50 window. From the emulator checkout, supply the absolute path
to `build/GP-50_livewdf/livewdf-build/examples/opengp5/gp50/livewdf-gp50`
as the firmware argument.

```sh
pw-jack python3 tools/simulate.py /path/to/livewdf-gp50 --gp50 --live --demo guitar.wav
```

Left and middle set Gain and Level. Right and FS2 select the circuit and
FS1 toggles bypass. Turn and press PARA to edit the cabinet, delay and
reverb settings. Both circuits share the output effects. The firmware
runs one circuit at a time.

The build produces a control ELF and a DSP section for the emulators.
Physical timing and recovery need verification before a hardware release.
