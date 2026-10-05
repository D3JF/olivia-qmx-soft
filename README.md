# Olivia QMX+

This is a small configurable Olivia MFSK terminal for a QRP Labs QMX+.

I started this because I wanted to try the software without having to solve
everything at once. The app can run against a real QMX+, or it can run in a
simulator mode with two local windows talking to each other.

The simulator is useful for testing the application before connecting the
radio.

## Disclaimer

**I vibe coded this.** __Please don't stone me to death__. I had to make a quick prototype and it turns out that GitHub Copilot is pretty good now.

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
.\olivia_gui.exe --station A
.\olivia_gui.exe --station B
```

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

The app displays a transmit waterfall below the connection controls. It shows
the Olivia signal energy across the selected mode's passband, centered at
1500 Hz, while the transmission is running. It uses a 1024-point FFT with a
128-sample hop for readable frequency detail and smooth scrolling. FFT work
runs in a dedicated waterfall worker; the radio worker only queues audio for
analysis and handles radio I/O. The GUI receives finished rows and paints them
on a 30 FPS timer, so waterfall processing does not block transmission. The
mode radio buttons are locked while transmitting, and the waterfall stops
after the final queued row from the transmission has been painted.

The live display uses a left-to-right time axis: frequency runs vertically and
each new FFT slice is added at the right edge.

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
