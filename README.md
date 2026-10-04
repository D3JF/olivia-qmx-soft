# Olivia QMX+

This is a small Olivia MFSK 8/250 terminal for a QRP Labs QMX+.

I started this because I wanted to try the software without having to solve
everything at once. The app can run against a real QMX+, or it can run in a
simulator mode with two local windows talking to each other.

The simulator is useful for testing the application before connecting the
radio.

## What it does

- Olivia 8/250 modulation and demodulation
- QMX+ serial setup
- QMX+ USB audio input and output
- A local simulator for two stations
- A simple PyQt5 interface
- Unit tests for the codec and application boundaries

The local codec currently supports Olivia 8/250 only. The sample rate is
8000 Hz and the center frequency is 1500 Hz.

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

Type a message and press `TRANSMIT`. To create a new line in a transmitted
message, type `\n`. The app converts that two-character sequence into a real
line break before sending it.

## Run with a real QMX+

Make sure the QMX+ is connected and its serial and USB audio devices are
available to your user.

Start the app with:

```bash
python olivia_qmx.py
```

The app looks for a serial device identified as QRP Labs or QMX and for a
matching USB audio device.

## Run the tests

From the project directory:

```bash
.venv/bin/python -m unittest -v test_olivia_qmx.py test_olivia_modem.py
```

The tests do not need a radio. The application tests use small dependency
doubles for the hardware-facing parts.

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
- `.vscode/tasks.json` contains the VS Code tasks.

This is an initial test version. I am keeping it small while I work out what
should come next.

## Disclaimer

**I vibe coded this.** Please don't stone me to death. I had to make a quick prototype and it turns out that GitHub Copilot is pretty good now.
