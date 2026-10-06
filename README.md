# Olivia QMX+

This is a small configurable Olivia MFSK terminal for a QRP Labs QMX+.
The maintained application is a portable C++17 program that can run against a
real QMX+ or in simulator mode with two local stations.

## Disclaimer

**I vibe coded this.** __Please don't stone me to death__. I had to make a
quick prototype and it turns out that GitHub Copilot is pretty good now.

## License

This project is licensed under the GNU General Public License, version 3 or
later. See [LICENSE](LICENSE) for the complete license text.

## What it does

- Olivia modulation and demodulation with selectable tone counts (2-256) and
  bandwidths (125-2000 Hz)
- QMX+ serial setup and USB audio support
- A local UDP simulator for two stations
- A Qt graphical interface when Qt5 development files are available
- CTest coverage for the codec and radio-control helpers

The app defaults to Olivia 8/250. The sample rate is 8000 Hz and the center
frequency is 1500 Hz.

## Requirements

- CMake 3.16 or newer
- A C++17 compiler
- Qt5 development files for the graphical application
- PortAudio for real-mode audio support

On Ubuntu, install PortAudio with:

```bash
sudo apt install libportaudio2 portaudio19-dev
```

The C++ build detects PortAudio through `pkg-config`. Use
`-DOLIVIA_AUDIO=OFF` for a codec/simulator-only build, or set `PORTAUDIO_ROOT`
to a Windows PortAudio SDK installation prefix.

## Build and test

Configure and build the project with CMake:

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The build provides:

- `olivia_app`, the console simulator
- `olivia_gui`, the Qt simulator and real-mode application when Qt5 is found
- `olivia_serial_info`, which lists serial devices
- `olivia_audio_info`, which lists audio devices when PortAudio is enabled
- `olivia_core_tests` and `olivia_serial_tests`

## Run the simulator

The simulator does not need a QMX+ or a sound card. Run two instances from
the build directory:

```bash
./olivia_app --station A
./olivia_app --station B
```

Type a message in either window and press Enter. Both instances must use the
same `--channel-port` value; the default listens on ports 45801 and 45802. A
single test message can be sent with:

```bash
./olivia_app --station A --message "Hello from Olivia"
```

When the Qt application is available, run:

```bash
./olivia_gui
./olivia_gui --station B
```

Launching `olivia_gui` opens a setup dialog. Choose Test mode, then Station A
or Station B. The command-line options remain available for automation.
Real QMX+ mode requires the serial and audio devices to be connected.

The graphical app needs an X11 display. When running it over SSH, use X11
forwarding and ensure that `DISPLAY` is non-empty.

## Real QMX+ support

For a real QMX+ connection, connect the radio by USB and make its serial and
USB audio devices available to the current user. The C++ application:

- discovers QMX+ serial and audio devices
- configures serial control at 115200 8N1
- opens mono 48 kHz USB audio streams
- converts between the QMX+ audio rate and the modem's 8 kHz rate
- keeps audio callbacks separate from modem processing

Hardware transmission and reception require a connected QMX+ and have not
been verified on physical hardware here.

## Project layout

- `cpp/include/` contains public C++ interfaces.
- `cpp/src/` contains the modem, simulator, GUI, audio, and serial
  implementations.
- `cpp/tests/` contains the CTest test programs.
- `.vscode/tasks.json` contains CMake build, test, and run tasks.

The former Python implementation is preserved on the
[`python-legacy`](https://github.com/D3JF/olivia-qmx-soft/tree/python-legacy)
branch and is not part of the maintained C++ application.

## Windows installer

On Windows 10 x64, install CMake, Ninja, NSIS, a C++17 compiler, and Qt5
(including `windeployqt`). Configure the project from a developer PowerShell
with the Qt `bin` directory on `PATH`:

```powershell
cmake -S . -B build-windows -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DOLIVIA_AUDIO=ON `
  -DPORTAUDIO_ROOT="C:\path\to\portaudio" `
  -DOLIVIA_BUILD_INSTALLER=ON
cmake --build build-windows
cpack --config build-windows/CPackConfig.cmake
```

This creates an NSIS installer in `build-windows`. The installer deploys the
Qt and MinGW compiler runtime DLLs, including `libwinpthread-1.dll`, alongside
`olivia_gui.exe`, and installs
the simulator, real-mode GUI, and serial/audio-information tools in the same
directory. Always distribute this CPack-generated installer rather than
packaging only the executable files manually; the compiler runtime is required
on machines that do not already have the MinGW toolchain installed. The
PortAudio SDK must provide `include\portaudio.h`,
`lib\libportaudio.dll.a` (or `portaudio.lib`), and the matching
`libportaudio.dll`.

## Debian and Ubuntu package

On Debian or Ubuntu, install the native build dependencies:

```bash
sudo apt install build-essential cmake dpkg-dev pkg-config \
    qtbase5-dev portaudio19-dev
```

Configure and build a release package with the GUI and real-mode audio support:

```bash
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=ON -DOLIVIA_AUDIO=ON
cmake --build build-linux
ctest --test-dir build-linux --output-on-failure
(cd build-linux && cpack --config CPackConfig.cmake)
```

This creates an architecture-specific `.deb` file in `build-linux` (for
example, `olivia-qmx-plus_0.1.1_amd64.deb`). Install it with:

```bash
sudo apt install ./olivia-qmx-plus_0.1.1_*.deb
```

The package installs the GUI, simulator, serial/audio information tools, a
desktop menu entry, and the application icon. Debian's shared-library scan
records the Qt and PortAudio runtime dependencies; those libraries are provided
by the distribution and are not bundled into the package.

For a simulator-only package without PortAudio, use
`-DOLIVIA_AUDIO=OFF`. If Qt5 development files are unavailable, CMake still
builds the console tools but does not include the GUI or desktop entry.

## Red Hat-based package

On Fedora, RHEL, Rocky Linux, or AlmaLinux, install the native build
dependencies and RPM tooling:

```bash
sudo dnf install gcc-c++ cmake rpm-build pkgconf-pkg-config \
    qt5-qtbase-devel portaudio-devel
```

Use the same CMake configuration and build commands shown above, then create
the RPM explicitly:

```bash
(cd build-linux && cpack -G RPM)
```

This creates an architecture-specific package such as
`olivia-qmx-plus-0.1.1-1.x86_64.rpm`. Install it with:

```bash
sudo dnf install ./olivia-qmx-plus-0.1.1-1.*.rpm
```

CPack's RPM dependency scan records the Qt, PortAudio, C++ runtime, and system
library requirements provided by the target distribution. The RPM does not
bundle those system libraries.
