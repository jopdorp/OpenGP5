# GP-50 stock comparison

These findings compare the official GP-5 V1.0.6 and GP-50 V1.0.5 updates. The Andes
instruction simulator ran the stock LCD, PWM and control input routines. Unicorn
ran both stock DSPs through startup, UART and an audio tone with modeled flash,
codec, DMA and interrupt behavior. The findings describe the firmware's register
behavior. Board traces, analog behavior, complete stock boot and recovery still
need verification.

## Update images

All five sections have the same type, container address and ordering. Each section
has different contents between the two images. The DSP vector table points to
`0x4400` in both. The control app runs at `0x10000` and its header at
`+0xa4..+0x105` matches.

| Section | HTFW address | GP-5 bytes | GP-50 bytes |
|---|---:|---:|---:|
| DSP program, `0x63` | `0x200000` | `0xc348` | `0xb874` |
| DSP data, `0x67` | `0x240000` | `0x55028` | `0x573e8` |
| Models, `0x66` | `0x280000` | `0x42fe4` | `0x43004` |
| Control app, `0x62` | `0x000000` | `0x12e168` | `0x155338` |
| Factory presets, `0x65` | `0x190000` | `0x2d000` | `0x2d000` |

GP-50's app ends at `0x165338` and overlaps the GP-5 settings and file allocation.
The two resident bootloaders are outside the stock update sections.

## Control processor

Both control apps use the NDS32 instruction set, and their DSP link uses control
UART0 on A0/A1 at 460800 baud. GP-50 uses control UART1 on A9/A10 at 31250 baud for
its external MIDI jacks.

| Function | GP-5 | GP-50 |
|---|---|---|
| Display | 160 × 80 | 160 × 128 |
| Display SPI clock and data | A5, A6 | A5, A6 |
| Display data/command, CS, reset | A7, A9, A16 | A7, A16, B6 |
| Display backlight | A10, Timer 5 PWM | A24, Timer 5 PWM |
| PARA turn and press | B2/B3, B6 | B2/B3, A15 |
| Footswitches | A17 | Control shift input bits 14 and 15 |
| Ring LEDs | A29/A30/A31 | Control shift output bits 9/8/10 and 12/11/13 |
| Analog inputs | A28, ADC selector 9 | A29/A30/A31 enabled, ADC selectors 12/12/11/10 |

GP-50 stock `0x4851e` shifts two bytes using A17 to load, A28 for serial out,
B4 for serial in and B5 for clock. Stock `0x14dac` reads the left, middle and
right knobs from shift input pairs 4/5, 0/1 and 2/3. Buttons are active low.
DRUM, EXIT, GLOBAL and SAVE use bits 6, 7, 12 and 13. Output bit 6
switches the expression ADC between its first two readings.

The EXP/FS jack shares an input with the optional external footswitches.
In EXP mode, the V1.0.5 event task at `0x49370` processes only logical ADC
slot 0, selector 12 with shift output bit 6 low. The other channel-12 slot is
used in Dual FS mode. Selectors 11 and 10 serve voltage and power functions.
The stock app filters analog changes at `0x4763c` and maps the sampled range
at `0x444a8`. The factory calibration screen collects heel and toe extrema
and rejects too little movement. Its saved endpoints are six raw counts above
the heel and sixteen below the toe, with a minimum effective span of 51.
[Valeton GP-50 V1.0.5 manual](https://www.valeton.net/download/312/online-manual/22311/gp-50_online-manual_en_firmware-v1-0-5-pdf.pdf)

When firmware defines `fw_gp50_expression`, the GP-50 samples selector 12
every 10 ms and reports changes from 0 to 255. The first reading establishes a
baseline. The default raw range is 0..200, as in the stock factory settings.
An application can read the latest ADC sample and set effective heel and toe
bounds in RAM using the [firmware API](api.md). The physical jack, pedal travel
and the external footswitch modes need separate validation.

Stock GP-50 initializes the panel at `0x47e48` and draws windows at `0x47d74`.
It uses MADCTL `0x68`, COLMOD `0x55` and a zero drawing offset. Its display
driver enables 180° software rotation at `0x4061a`. OpenGP5 uses MADCTL `0xA8`
to rotate the display in the panel. GP-5's normal stock drawing offset is
+1, +26. The simulator ran the LCD GPIO and PWM setup and captured the panel
commands with SPI sends and delays intercepted.

GP-50 control outputs 0–7 also participate in its Off/Charge/Run state machine,
including analog bypass and power. Those outputs need a board-specific startup
sequence. The stock callbacks and saved power state are referenced from
`0x4aaae` and `0x4000100c`. The attached rails and switches still need
electrical verification.

## DSP

Both execute Cortex-M4F code. The stock routines at GP-5 `0x7660` and GP-50
`0x785c` program the audio clock in the same way, including the two values
for 44.1 and 48 kHz. The DSP core clock of the GP-50 needs measurement.

| Function | GP-5 | GP-50 |
|---|---|---|
| DSP-side UART | UART0 at `0x40005000` | UART1 at `0x40006000` |
| Audio DMA base | `0x40030000` | `0x40030000` |
| Audio DMA input FIFO addresses | `0x50000034`, `0x50000038` | `0x5000000c`, `0x50000010` |
| Audio DMA output FIFO addresses | `0x5000003c`, `0x50000040` | `0x50000014`, `0x50000018` |
| Stock audio interface enable write | `0x8070741b` | `0x0000741f` |
| Stock codec initialization | GP-5 audio init path | Bit-banged I²C on DSP GPIO bits 9/8 |

The GP-50 mux setup at `0x594c` changes different fields from the GP-5.
It also clears memory at `0x29000000..0x290d757f` during startup. GP-50
audio init at `0x5670` writes I²C wire addresses `0x26` and `0x10` before
configuring DMA. The DSP trace captures those transfers with modeled ACKs.
Codec identity, physical memory geometry and the DSP resident updater still
need validation on the board.

The first LiveWDF build that reached the physical DSP used the same DMA
channels, requests and FIFO addresses as stock. The codec transactions had
the same data bytes with different timing. In a forced-NACK test, stock
continued to configure audio DMA and OpenGP5 left audio disabled. With clock
select register `0x4000e020` seeded at `0x00080404`, stock reached
`0x0000000a` and OpenGP5 retained `0x0008040e`. The initial state on the
physical pedal still needs a readout.

## Run the checks

Use the two official stock update ZIPs or their BIN files, the Andes tools
installed by [toolchain setup](setup.md), and Unicorn 2.1 or newer.

```sh
python3 tools/stock_control_emu.py --stock path/to/gp5.zip
python3 tools/stock_control_emu.py --stock path/to/gp5.zip --stock-cold
python3 tools/stock_control_emu.py --stock path/to/gp50.zip
python3 tools/stock_control_emu.py --stock path/to/gp50.zip --stock-radio
python3 tools/stock_control_emu.py --stock path/to/gp50.zip --stock-cold
python3 tools/stock_control_emu.py --stock path/to/gp50.zip --gp50-warm-handoff
python3 tools/stock_dsp_emu.py path/to/gp5.zip > gp5-dsp.json
python3 tools/stock_dsp_emu.py path/to/gp50.zip > gp50-dsp.json
diff -u gp5-dsp.json gp50-dsp.json
python3 tools/stock_dsp_emu.py path/to/gp50.zip --runtime
```

Both stock control apps reach the TLB setup instruction. The simulator uses
a flat memory map, so the harness replaces that unsupported instruction in
simulated memory. It supplies eFuse, PLL, oscillator and ADC status and models
the first FreeRTOS task return. Both apps reach board initialization. The
warm handoff replay checks the stock GP-50's acknowledgment and jump to the
factory loader with USB emission and delays intercepted. A cold-start recovery
test still requires the pedal.

The `--stock-radio` check continues through settings and Bluetooth radio
initialization on both models. It seeds virtual NOR with the stock factory
presets, models NOR writes and supplies entropy and calibration status. It
checks settings and Bluetooth configuration records and the stock radio clock
latch sequence. Discovery, pairing and audio over Bluetooth require radio and
peer device behavior beyond this fixture.

The `--stock-cold` check delivers stock FreeRTOS scheduler events and replays
Cortex-M4 UART replies through the control processor's receive DMA. It checks
the exact startup preset and settings commands against the stock DSP. Both
models complete their LCD initialization. The GP-50 fixture also supplies the
Run power event and checks the first complete LVGL frame before the app loop.
The GP-5 fixture includes its additional eight SnapTone metadata exchanges.
The fixture supplies NOR and SPI status and defers the Bluetooth service task.

The DSP harness reports the last write to each register per phase. By default
it replays selected init routines after the reset checkpoint. With `--runtime`
it queries the version through the stock UART handlers and supplies a 440 Hz
tone to successive 64-sample DMA buffers. The stock DSPs produce stereo output
with effects off and go quiet after an output-level command. The test models
DMA completion and interrupt delivery, including the stock PendSV audio work.
The flash and codec model uses virtual state, and the test fast-forwards fixed
delay loops.

For an OpenGP5 GP-5 baseline, build passthrough and relink the control ELF
without EX9 relaxation for the Andes simulator. Run from the repository root.

```sh
platform/build_firmware.sh passthrough
(
  cd platform/nds32/out
  ../../../fw-build/andes/v3s/nds32le-elf-mculib-v3s/bin/nds32le-elf-gcc \
    -Os -nostartfiles -static -mcmodel=medium -mext-dsp -mvh -T ../start/app.ld \
    -Wl,--gc-sections,--mno-ex9 -o app-emu.elf ./*.o -ldsp
)
python3 tools/stock_control_emu.py --open-elf platform/nds32/out/app-emu.elf
python3 tools/dsp_emu_check.py build/GP-5_passthrough.bin
```

The relinked OpenGP5 control app reaches `main`. The existing DSP check
verifies the UART reply and processed audio blocks. A full board emulator
still needs the flash, interrupt timing, panel, control shift I/O and audio
peripherals, plus the link between the two CPUs.

## OpenGP5 GP-50 bring-up

The GP-50 build produces a control ELF and a raw DSP section for the emulators.
The control test executes startup through LCD initialization and checks the
command stream, display windows, backlight and control GPIO. The DSP test runs
the codec setup, UART and 64-sample processing. It also checks that a codec
NACK keeps audio off.

```sh
platform/build_firmware.sh --gp50-emu tremolo
python3 tools/stock_control_emu.py --gp50-app build/GP-50_tremolo/control.elf
python3 tools/dsp_emu_check.py build/GP-50_tremolo/audio.dsp --gp50-raw --set 0=520
python3 tools/dsp_emu_check.py build/GP-50_tremolo/audio.dsp --gp50-raw --codec-nack
```

In the desktop simulator, left, middle and right turn RATE, DEPTH and SHAPE.
FS1 toggles bypass and FS2 changes the shape. For example:

```sh
printf '0.2 left turn 2\n0.4 middle turn 3\n0.6 right turn 1\n0.8 fs2 click\n' > controls.txt
python3 tools/simulate.py tremolo --gp50 --script controls.txt
```

The build starts with the stock cold control output and enables the Run
outputs before querying the DSP. After flashing this change, the GP-50
answered the DSP version and load queries over USB. The cycle counters
were zero during the check, and the guitar output was silent.

### Experimental update package

```sh
platform/build_firmware.sh --gp50-package tremolo build/GP-50_tremolo-experimental.bin
python3 tools/htfw.py info build/GP-50_tremolo-experimental.bin
```

The build packages the GP-50 DSP and control binaries as sections `0x63`
and `0x62`, using the V1.0.5 container layout and freshly computed section
checksums. The two sections contain OpenGP5 code. The build runs offline.
The factory updater accepted both sections in the first LiveWDF trial. The
control app booted, but stalled while waiting for the DSP.

On Linux, check the experimental build and the official V1.0.5 recovery image
before connecting to the factory loader.

```sh
python3 tools/gp50_flash.py check build/GP-50_tremolo-experimental.bin
python3 tools/gp50_flash.py check-stock "GP-50 Firmware V1.0.5.zip"
python3 tools/gp50_flash.py status
```

`status` requires an already running factory loader at USB `84ef:018b`
and checks its update-mode reply. The installer uses the 19-byte MIDI data
frames and section order from [Valeton Suite V1.0.9](https://www.valeton.net/wpfd_file/valeton-suite-setup-v1-0-9-for-mac-zip/).
`flash` verifies the supplied stock recovery file before opening USB MIDI.
Both `flash` and `restore` write the DSP and control program sections only.
The tool stops if an erase or write acknowledgement is missing.

The first trial's USB re-entry command restarted the control app. The pedal
later entered factory update mode using the external footswitch in place of
front-panel A, with PARA pressed at startup. The loader's USB identity and
MIDI reply both matched the factory updater.

```sh
python3 tools/gp50_flash.py flash build/GP-50_tremolo-experimental.bin --stock "GP-50 Firmware V1.0.5.zip"
python3 tools/gp50_flash.py restore "GP-50 Firmware V1.0.5.zip"
```

The Andes simulator can replay the custom app's warm handoff.

```sh
platform/build_firmware.sh --gp50-emu tremolo
python3 tools/stock_control_emu.py --gp50-app build/GP-50_tremolo/control.elf --gp50-warm-handoff
```

The fixture checks that the app stays running if the host fails to receive
the update acknowledgment. Once the host takes it, the app follows the stock
GP-50 sequence of two control output updates, two settle delays and a direct
handoff to the resident loader at address zero. The simulator stops at the
loader entry. The official update does not include the loader.

### Processor link and USB

A separate register-level replay used the two compiled GP-50 tremolo binaries.
The control UART0 generated a version request, the DSP UART1 returned the version
frame, and the control firmware accepted it with zero receive errors. The emulators
exchanged captured UART bytes and supplied the receive interrupts.

In a modeled USB controller, the compiled control firmware answered SETUP
requests with its device, configuration and GP-50 string descriptors. It
configured 64-byte bulk MIDI endpoints `0x03` and `0x82` and answered an
OpenGP5 version request. USB bus timing and host enumeration require a device
test. The emulator target covers USB MIDI. Stock USB audio also uses an SPI and
DMA link between the processors and separate isochronous USB endpoints.

The stock control firmware also contains Bluetooth A2DP audio and BLE/GATT
command handling. The wireless and USB transports share the stock command
dispatcher. Bluetooth audio and BLE editing are separate porting work.

### Native assets and recovery

The V1.0.5 stock control app has a SnapTone export handler. Its corresponding
DSP read command, `0x7004`, returned without an asset in the stock UART
replay. The IR and SnapTone metadata queries did return data.

The stock DSP references asset metadata at flash offsets
`0x114000..0x121fff`, a SnapTone bank at `0x122000..0x157fff`, and the
shared user IR and SnapTone bank at `0x158000..0x1cffff`. A full backup
requires hardware readout of both processors' flash. The GP-50 probe pinout
still needs confirmation.
