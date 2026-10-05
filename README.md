# Olivia QMX+

This is a small configurable Olivia MFSK terminal for a QRP Labs QMX+.

I started this because I wanted to try the software without having to solve
everything at once. The app can run against a real QMX+, or it can run in a
simulator mode with two local windows talking to each other.

The simulator is useful for testing the application before connecting the
radio.

## Disclaimer

**I vibe coded this.** __Please don't stone me to death__. I had to make a quick prototype and it turns out that GitHub Copilot is pretty good now.

## License

This project is licensed under the GNU General Public License, version 3 or
later. See [LICENSE](LICENSE) for the complete license text.

## What it does

- Olivia modulation and demodulation with selectable tone counts (2-256) and
  bandwidths (125-2000 Hz)
- QMX+ serial setup
- QMX+ USB audio input and output
- A local simulator for two stations
- A simple PyQt5 interface
- Unit tests for the codec and application boundaries

The app defaults to Olivia 8/250, a common choice for CQ. The tone count and
bandwidth can be changed with the radio buttons before or during a connection.
The sample rate is 8000 Hz and the center frequency is 1500 Hz.

## C++ port

The `cpp-port` branch contains the start of a portable C++17 rewrite. The
first slice is the Olivia codec in `cpp/include/olivia_modem.hpp` and
`cpp/src/olivia_modem.cpp`; it has no Python, Qt, PortAudio, or serial
dependencies. A CMake/CTest target exercises incremental decoding and codec
configuration validation.

Build the C++ core on Linux, macOS, or Windows with CMake:

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The Python application remains the behavioral reference while the port is
developed in stages: codec, simulator transport, hardware adapters, then the
desktop UI.

When Qt5 development files are available, CMake also builds `olivia_gui`
(`olivia_gui.exe` on Windows). It reproduces the current simulator GUI:
station status, connection control, tone and bandwidth selectors, received and
outgoing text areas, transmit, and clear controls. It currently uses the
native UDP simulator; QMX+ serial and USB audio hardware support remains a
later adapter.

Run the Qt simulator with two instances:

```powershell
.\olivia_gui.exe
.\olivia_gui.exe --station B
```

Launching `olivia_gui.exe` from Explorer opens a setup dialog. Choose Test
mode, then Station A or Station B. The command-line options remain available
for automation; `--station B` skips the dialog. Real QMX+ mode currently
explains that the hardware backend is not implemented in the C++ version yet.

### Windows simulator executable

The first runnable C++ application slice is a native console simulator. Build
it with the same CMake command above; the executable is `olivia_app.exe` on
Windows. Run two Command Prompt or PowerShell windows:

```powershell
.\olivia_app.exe --station A
.\olivia_app.exe --station B
```

Type a message in either window and press Enter. The stations exchange
Olivia 8/250 audio over localhost UDP. Both instances must use the same
`--channel-port` value; the default listens on ports 45801 and 45802. A
single test message can be sent with:

```powershell
.\olivia_app.exe --station A --message "Hello from Windows"
```

## Requirements

This project is intended to run on Linux with Python 3.12 or newer.

For a real QMX+ connection, you also need:

- A QRP Labs QMX+ connected by USB
- Access to the QMX+ serial device
- The QMX+ USB audio device
- PortAudio installed on the system

On Ubuntu, install PortAudio with:

```bash
sudo apt install libportaudio2 portaudio19-dev
```

The C++ build detects PortAudio through `pkg-config` and builds the reusable
`olivia_audio` backend when the development package is present. It enumerates
the system's mono-capable input/output devices, identifies a QMX+ device by
`QMX` or `QRP Labs` in its name, opens 8 kHz float32 streams, queues transmit
samples for playback, and delivers captured samples on a non-audio worker
thread. Audio callbacks do not run the Olivia decoder directly, avoiding
blocking or allocation-heavy modem work in the PortAudio callback.

Audio backend selection is controlled by CMake:

```bash
cmake -S . -B build -DOLIVIA_AUDIO=AUTO
```

Use `OLIVIA_AUDIO=OFF` for a codec/simulator-only build. For a Windows
PortAudio SDK, set `PORTAUDIO_ROOT` to its installation prefix; the same C++
backend is then used with the Windows PortAudio library instead of Linux
`pkg-config`.

The C++ branch also contains a small portable serial layer for QMX+ control.
It enumerates Linux `/dev/ttyUSB*`, `/dev/ttyACM*`, and serial-by-id entries,
and Windows COM ports, using 115200 8N1 configuration. The serial layer is
currently exposed through `olivia_serial_info` and is not yet connected to the
Real-mode GUI.

The serial API uses native POSIX termios on Linux and Win32 COM handles on
Windows, while keeping those details behind `SerialPort`. It configures
115200 8N1 and exposes simple byte writes for the QMX+ `TX;` and `RX;`
commands. Device opening and command sequencing will be integrated into the
Real-mode worker after the audio and serial discovery milestones are complete.

QMX+ initialization and transmit-state sequencing are represented by the
separate `QmxRadio` helper. Its command writer is injectable, so command order
and failure handling can be tested without a connected radio.

When PortAudio is enabled, Real mode uses a dedicated Qt worker thread. It
opens the selected serial port, initializes the radio, starts the selected
mono input/output devices at the QMX+ 48 kHz USB-audio rate, downsamples
received audio to the Olivia modem's 8 kHz rate, and upsamples transmitted
modem audio back to 48 kHz. Hardware transmission and reception still require
a connected QMX+ and have not been verified on physical hardware here.

The graphical app also needs an X11 display. When I run it from a remote
Ubuntu machine, I use VcXsrv on Windows and connect with SSH X11 forwarding.
The VS Code terminal must actually have a non-empty `DISPLAY` variable.

## Install

Clone the repository and enter the project directory:

```bash
git clone https://github.com/D3JF/olivia-qmx-soft.git
cd olivia-qmx-soft
```

Create a virtual environment:

```bash
python3 -m venv .venv
```

Activate it:

```bash
source .venv/bin/activate
```

Install the Python packages:

```bash
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
```

## Run the simulator

The simulator does not need a QMX+ or a sound card. It uses two local UDP
ports and two app windows.

Open two terminals in the project directory. Run Station A in the first:

```bash
OLIVIA_QMX_SIMULATOR=1 \
OLIVIA_QMX_STATION=A \
python olivia_qmx.py
```

Run Station B in the second:

```bash
OLIVIA_QMX_SIMULATOR=1 \
OLIVIA_QMX_STATION=B \
python olivia_qmx.py
```

Both stations use the same default channel. Station A listens on port 45801
and Station B listens on port 45802.

If a port is already in use, close the old simulator windows. You can also
choose another shared base port:

```bash
OLIVIA_QMX_SIMULATOR=1 OLIVIA_QMX_STATION=A \
OLIVIA_QMX_CHANNEL_PORT=45900 python olivia_qmx.py
```

Use `OLIVIA_QMX_CHANNEL_PORT=45900` for Station B as well.

If the app reports that a simulator UDP port is already in use, an older
simulator window is still running on that channel. Close the older A/B pair,
or start both stations with another shared `OLIVIA_QMX_CHANNEL_PORT` value.
The transmit button remains disabled until the station connects successfully.

Type a message and press `TRANSMIT`. To create a new line in a transmitted
message, type `\n`. The app converts that two-character sequence into a real
line break before sending it.

The app displays a native C++ transmit waterfall below the connection
controls. It receives generated simulator audio from the radio worker, uses
1024-sample Hann-windowed frames are zero-padded to a 4096-point FFT, with a
256-sample hop. Each spectrum is appended as a color row from top to bottom:
frequency runs left-to-right and time runs top-to-bottom (oldest at the top,
newest at the bottom). The longer window and zero-padding provide denser
frequency interpolation at the cost of time resolution. The
vertical axis zooms around the 1500 Hz Olivia center frequency so the tones
are visible, with a small margin beyond the selected passband. The display
uses a MATLAB-style jet color palette, and the selected bandwidth is shown
below the plot. FFT painting
runs on the GUI thread while modulation, packetization, and transmission
remain on the worker thread, so the transmit operation does not block the
window. This simulator preview is not yet a physical QMX+ audio device
backend.

In simulator mode, the receiving station decodes each ordered UDP packet as
soon as it arrives instead of waiting for the complete message. The first
decoded text still appears after Olivia has received one complete modem frame,
which is expected for this slow mode.

## Run with a real QMX+

Make sure the QMX+ is connected and its serial and USB audio devices are
available to your user.

Start the app with:

```bash
python olivia_qmx.py
```

The app looks for a serial device identified as QRP Labs or QMX and for a
matching USB audio device.

The window shows the current connection state. If the QMX+ is unavailable at
startup, connect it and press `Reconnect`. Use `Disconnect` before unplugging
the radio, or to release the devices for another application. The
`TRANSMIT` button is disabled while disconnected and while a transmission is
in progress. If a transmission encounters a hardware error, the app returns
to the disconnected state so it can be safely reconnected.

## Run the tests

From the project directory:

```bash
.venv/bin/python -m unittest -v test_olivia_qmx.py test_olivia_modem.py test_olivia_waveform.py
```

The tests do not need a radio. The application tests use small dependency
doubles for the hardware-facing parts.

## Render a waveform image

The waveform tool uses the same Olivia 8/250 codec as the application. It
writes a JPEG containing the waveform and a spectrogram.

Give the message as an argument:

```bash
QT_QPA_PLATFORM=offscreen .venv/bin/python olivia_waveform.py \
  "Hello, World!" -o hello-world.jpg
```

Or pipe the message through standard input:

```bash
printf '%s\n' "Hello, World!" | \
  QT_QPA_PLATFORM=offscreen .venv/bin/python olivia_waveform.py \
  -o hello-world.jpg
```

The default output filename is `olivia_waveform.jpg`. JPEG files are ignored
by Git because these images are intended for local testing.

## VS Code tasks

The repository includes tasks for:

- Checking X11 forwarding
- Running the real QMX+ app
- Running simulator Station A
- Running simulator Station B
- Running the unit tests

Open the Command Palette in VS Code and choose `Tasks: Run Task`.

The GUI tasks expect the VS Code process to have X11 forwarding. If
`DISPLAY` is empty, run the app from the SSH terminal where `ssh -Y` created
the forwarding tunnel.

## Project files

- `olivia_qmx.py` is the application.
- `olivia_modem.py` is the local Olivia codec.
- `test_olivia_qmx.py` tests the application and hardware boundaries.
- `test_olivia_modem.py` tests real codec round trips.
- `olivia_waveform.py` renders codec output as a JPEG waveform image.
- `test_olivia_waveform.py` tests the image generator command line.
- `.vscode/tasks.json` contains the VS Code tasks.

This is an initial test version. I am keeping it small while I work out what
should come next.
