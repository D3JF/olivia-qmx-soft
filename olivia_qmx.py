#!/usr/bin/env python3
"""Minimal Olivia 8/250 terminal for a QRP Labs QMX+.

Install the runtime dependencies with:
    python3 -m pip install PyQt5 pyserial sounddevice numpy
"""

from __future__ import annotations

import sys
import threading
import os
import queue
import socket
import struct
import time
from typing import Any, Iterable, Optional

import numpy as np
import serial
from serial.tools import list_ports
import sounddevice as sd
from PyQt5.QtCore import QObject, QThread, pyqtSignal, pyqtSlot
from PyQt5.QtWidgets import (
    QApplication,
    QMainWindow,
    QLabel,
    QPushButton,
    QTextEdit,
    QVBoxLayout,
    QWidget,
)

from olivia_modem import OliviaModem


SAMPLE_RATE = 8000
CHANNELS = 1
BAUD_RATE = 115200
SERIAL_COMMANDS = (b"FA00007040000;", b"MD2;")
SIMULATOR_PACKET_BYTES = 8 * 1024
SIMULATOR_RECEIVE_BUFFER_BYTES = 4 * 1024 * 1024
SIMULATOR_PACKET_DELAY_SECONDS = 0.005
SIMULATOR_AUDIO_BLOCK_SAMPLES = 1024


def normalize_transmit_text(message: str) -> str:
    """Turn the typed two-character sequence ``\\n`` into a line break."""
    return message.replace("\\n", "\n")


def station_port(station: str, channel_port: int) -> int:
    """Return the simulator UDP listen port for a station."""
    if station not in ("A", "B"):
        raise ValueError("Simulator station must be A or B.")
    return channel_port + (1 if station == "A" else 2)


def target_station_port(station: str, channel_port: int) -> int:
    """Return the simulator UDP port for the station being contacted."""
    if station not in ("A", "B"):
        raise ValueError("Simulator station must be A or B.")
    return station_port("B" if station == "A" else "A", channel_port)


def station_listening_status(station: str) -> str:
    """Describe a station after its transmission has completed."""
    return f"Station {station} transmitted; listening"


def qmx_port() -> str:
    """Return the first serial device identified as a QRP Labs QMX+."""
    for port in list_ports.comports():
        text = " ".join(
            value or ""
            for value in (port.description, port.manufacturer, port.product)
        ).lower()
        if "qrp labs" in text or "qmx" in text:
            return port.device
    raise RuntimeError("No QRP Labs or QMX serial port was detected.")


def qmx_audio_device() -> int:
    """Return the QMX USB audio device index, if sounddevice exposes one."""
    devices = sd.query_devices()
    for index, device in enumerate(devices):
        name = str(device.get("name", "")).lower()
        if "qmx" in name or "qrp labs" in name:
            return index
    raise RuntimeError("No QMX USB soundcard was detected.")


class OliviaCodec:
    """Adapt the common olivia-modem APIs to the app's audio interface."""

    def __init__(self) -> None:
        try:
            self._modem = OliviaModem(
                tones=8, bandwidth=250, sample_rate=SAMPLE_RATE
            )
        except TypeError:
            self._modem = OliviaModem(8, 250, SAMPLE_RATE)

    def encode(self, text: str) -> np.ndarray:
        for name in ("modulate", "encode", "tx"):
            method = getattr(self._modem, name, None)
            if method is not None:
                samples = np.asarray(method(text), dtype=np.float32)
                return samples.reshape(-1)
        raise RuntimeError("Installed olivia-modem has no modulate/encode/tx method.")

    def decode(self, samples: np.ndarray) -> str:
        for name in ("demodulate", "decode", "rx"):
            method = getattr(self._modem, name, None)
            if method is not None:
                result = method(samples)
                if isinstance(result, bytes):
                    return result.decode("utf-8", errors="replace")
                return str(result or "")
        raise RuntimeError("Installed olivia-modem has no demodulate/decode/rx method.")


class RadioWorker(QObject):
    received = pyqtSignal(str)
    status = pyqtSignal(str)
    error = pyqtSignal(str)
    ready = pyqtSignal()

    def __init__(self) -> None:
        super().__init__()
        self._serial: Optional[serial.Serial] = None
        self._audio: Optional[sd.InputStream] = None
        self._codec: Optional[OliviaCodec] = None
        self._rx_codec: Optional[OliviaCodec] = None
        self._running = True
        self._tx_lock = threading.Lock()
        self._simulator = os.environ.get("OLIVIA_QMX_SIMULATOR") == "1"
        self._station_id = os.environ.get("OLIVIA_QMX_STATION", "").upper()
        self._channel_port = int(os.environ.get("OLIVIA_QMX_CHANNEL_PORT", "45800"))
        self._simulated_socket: Optional[socket.socket] = None
        self._simulated_rx: queue.Queue[np.ndarray | None] = queue.Queue()
        self._simulated_rx_thread: Optional[threading.Thread] = None
        # The demodulator owns mutable stream state, so all audio blocks pass
        # through one ordered queue and one decoder thread.
        self._decoder_queue: queue.Queue[np.ndarray | None] = queue.Queue()
        self._decoder_thread: Optional[threading.Thread] = None
        self._simulated_tx_id = 0

    def _start_decoder_thread(self) -> None:
        self._decoder_thread = threading.Thread(
            target=self._decoder_loop,
            name="olivia-decoder",
            daemon=True,
        )
        self._decoder_thread.start()

    @pyqtSlot()
    def start(self) -> None:
        try:
            self._codec = OliviaCodec()
            self._rx_codec = OliviaCodec()
            self._start_decoder_thread()
            if self._simulator:
                if self._station_id not in ("A", "B"):
                    self._station_id = ""
                if self._station_id:
                    listen_port = station_port(self._station_id, self._channel_port)
                    self._simulated_socket = socket.socket(
                        socket.AF_INET, socket.SOCK_DGRAM
                    )
                    self._simulated_socket.setsockopt(
                        socket.SOL_SOCKET,
                        socket.SO_RCVBUF,
                        SIMULATOR_RECEIVE_BUFFER_BYTES,
                    )
                    self._simulated_socket.settimeout(0.2)
                    try:
                        self._simulated_socket.bind(("127.0.0.1", listen_port))
                    except OSError as exc:
                        self._simulated_socket.close()
                        self._simulated_socket = None
                        if exc.errno == 98:
                            raise RuntimeError(
                                f"Station {self._station_id} cannot start: "
                                f"UDP port {listen_port} is already in use. "
                                "Close the old simulator window or choose a "
                                "different OLIVIA_QMX_CHANNEL_PORT."
                            ) from exc
                        raise
                self._simulated_rx_thread = threading.Thread(
                    target=self._simulated_channel_loop
                    if self._station_id
                    else self._simulated_rx_loop,
                    name="olivia-simulator-rx",
                    daemon=True,
                )
                self._simulated_rx_thread.start()
                if self._station_id:
                    self.status.emit(
                        f"Simulator station {self._station_id}; "
                        "waiting for the other station"
                    )
                else:
                    self.status.emit("Simulator ready; virtual TX/RX active")
                self.ready.emit()
                return
            port = qmx_port()
            self._serial = serial.Serial(port, BAUD_RATE, timeout=1)
            for command in SERIAL_COMMANDS:
                self._serial.write(command)
                self._serial.flush()
            device = qmx_audio_device()
            self._audio = sd.InputStream(
                samplerate=SAMPLE_RATE,
                channels=CHANNELS,
                dtype="float32",
                device=device,
                blocksize=1024,
                callback=self._audio_callback,
            )
            self._audio.start()
            self.status.emit(f"Connected to {port}; Olivia 8/250 RX active")
            self.ready.emit()
        except Exception as exc:
            self.error.emit(str(exc))

    def _audio_callback(
        self, data: np.ndarray, frames: int, time_info: Any, status: Any
    ) -> None:
        if status:
            self.status.emit(f"Audio: {status}")
        if self._rx_codec is None or not self._running:
            return
        # Keep FFT work out of the sounddevice callback. Blocking a real audio
        # callback can cause dropped input blocks and make the receiver fail.
        self._decoder_queue.put(np.asarray(data[:, 0], dtype=np.float32))

    def _decoder_loop(self) -> None:
        while True:
            block = self._decoder_queue.get()
            if block is None:
                return
            self._decode_audio_block(block)
            if self._simulator:
                # The simulator has all samples immediately, unlike a sound
                # card. Pace decoding so a long message does not monopolize
                # the CPU and starve the GUI or the other station.
                time.sleep(len(block) / SAMPLE_RATE)

    def _decode_audio_block(self, samples: np.ndarray) -> None:
        if self._rx_codec is None:
            return
        try:
            text = self._rx_codec.decode(samples)
            if text:
                self.received.emit(text)
        except Exception as exc:
            self.error.emit(f"RX decode failed: {exc}")

    def _simulated_rx_loop(self) -> None:
        while self._running:
            block = self._simulated_rx.get()
            if block is None:
                return
            block = np.asarray(block, dtype=np.float32)
            self._audio_callback(
                block.reshape(-1, 1), len(block), None, None
            )

    def _simulated_channel_loop(self) -> None:
        assert self._simulated_socket is not None
        packets: dict[int, dict[int, bytes]] = {}
        packet_counts: dict[int, int] = {}
        while self._running:
            try:
                packet, _ = self._simulated_socket.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                return
            if len(packet) < 12:
                continue
            magic, tx_id, sequence, total = struct.unpack("!4sIHH", packet[:12])
            if magic != b"OLIV":
                continue
            if total == 0 or sequence >= total:
                continue
            packets.setdefault(tx_id, {})[sequence] = packet[12:]
            packet_counts[tx_id] = total
            if len(packets[tx_id]) != total or set(packets[tx_id]) != set(range(total)):
                continue
            payload = b"".join(packets[tx_id][index] for index in range(total))
            del packets[tx_id]
            del packet_counts[tx_id]
            samples = np.frombuffer(payload, dtype=np.float32)
            # UDP reassembly is only transport. Feed the reconstructed audio
            # to the same block path used by the real audio input.
            for start in range(0, len(samples), SIMULATOR_AUDIO_BLOCK_SAMPLES):
                block = samples[start : start + SIMULATOR_AUDIO_BLOCK_SAMPLES]
                self._audio_callback(block.reshape(-1, 1), len(block), None, None)

    @pyqtSlot(str)
    def transmit(self, message: str) -> None:
        if not message.strip():
            return
        with self._tx_lock:
            try:
                if self._codec is None:
                    raise RuntimeError("QMX+ is not connected.")
                if self._simulator:
                    self.status.emit("Simulator transmitting...")
                    samples = self._codec.encode(message)
                    if self._station_id:
                        # The UDP header is simulator-only. It preserves packet
                        # order and message boundaries without changing the
                        # Olivia waveform sent to the decoder.
                        target_port = target_station_port(
                            self._station_id, self._channel_port
                        )
                        assert self._simulated_socket is not None
                        self._simulated_tx_id += 1
                        chunk_size = SIMULATOR_PACKET_BYTES
                        chunks = [
                            samples[start : start + chunk_size].astype(np.float32).tobytes()
                            for start in range(0, len(samples), chunk_size)
                        ]
                        for sequence, chunk in enumerate(chunks):
                            self._simulated_socket.sendto(
                                struct.pack(
                                    "!4sIHH",
                                    b"OLIV",
                                    self._simulated_tx_id,
                                    sequence,
                                    len(chunks),
                                ) + chunk,
                                ("127.0.0.1", target_port),
                            )
                            time.sleep(SIMULATOR_PACKET_DELAY_SECONDS)
                        self.status.emit(station_listening_status(self._station_id))
                    else:
                        for start in range(0, len(samples), 1024):
                            self._simulated_rx.put(samples[start : start + 1024])
                        self.status.emit("Simulator receiving")
                    return
                if self._serial is None:
                    raise RuntimeError("QMX+ is not connected.")
                device = qmx_audio_device()
                samples = self._codec.encode(message)
                self._serial.write(b"TX;")
                self._serial.flush()
                self.status.emit("Transmitting...")
                with sd.OutputStream(
                    samplerate=SAMPLE_RATE,
                    channels=CHANNELS,
                    dtype="float32",
                    device=device,
                    blocksize=1024,
                ) as output:
                    output.write(samples.reshape(-1, 1))
            except Exception as exc:
                self.error.emit(f"TX failed: {exc}")
            finally:
                if self._serial is not None and self._serial.is_open:
                    try:
                        self._serial.write(b"RX;")
                        self._serial.flush()
                    except serial.SerialException as exc:
                        self.error.emit(f"Could not return QMX+ to RX: {exc}")
                if not self._simulator:
                    self.status.emit("Receiving")

    @pyqtSlot()
    def stop(self) -> None:
        if self._simulator:
            if self._station_id and self._simulated_socket is not None:
                self._simulated_socket.close()
            else:
                self._simulated_rx.put(None)
            if self._simulated_rx_thread is not None:
                self._simulated_rx_thread.join(timeout=3)
        self._decoder_queue.put(None)
        if self._decoder_thread is not None:
            self._decoder_thread.join(timeout=3)
        self._running = False
        if self._audio is not None:
            self._audio.stop()
            self._audio.close()
        if self._serial is not None and self._serial.is_open:
            self._serial.write(b"RX;")
            self._serial.close()


class MainWindow(QMainWindow):
    transmit_requested = pyqtSignal(str)
    shutdown_requested = pyqtSignal()

    def __init__(self) -> None:
        super().__init__()
        station = os.environ.get("OLIVIA_QMX_STATION", "").upper()
        station_label = f"Station {station}" if station in ("A", "B") else "Local"
        self.setWindowTitle(f"Olivia MFSK - QMX+ ({station_label})")
        self.resize(720, 520)

        self.station_label = QLabel(station_label)
        self.received_text = QTextEdit()
        self.received_text.setReadOnly(True)
        self.received_text.setPlaceholderText("Received text")
        self.outgoing_text = QTextEdit()
        self.outgoing_text.setPlaceholderText("Message to transmit")
        self.transmit_button = QPushButton("TRANSMIT")
        self.clear_button = QPushButton("Clear received")
        self.transmit_button.setMinimumHeight(64)
        self.transmit_button.clicked.connect(self._transmit)
        self.clear_button.clicked.connect(self.received_text.clear)

        layout = QVBoxLayout()
        layout.addWidget(self.station_label)
        layout.addWidget(self.received_text, 3)
        layout.addWidget(self.outgoing_text, 1)
        layout.addWidget(self.transmit_button)
        layout.addWidget(self.clear_button)
        container = QWidget()
        container.setLayout(layout)
        self.setCentralWidget(container)
        self.statusBar().showMessage("Starting...")

        self.thread = QThread(self)
        self.worker = RadioWorker()
        self.worker.moveToThread(self.thread)
        self.thread.started.connect(self.worker.start)
        self.transmit_requested.connect(self.worker.transmit)
        self.shutdown_requested.connect(self.worker.stop)
        self.worker.received.connect(self._show_received)
        self.worker.status.connect(self.statusBar().showMessage)
        self.worker.error.connect(self._show_error)
        self.worker.ready.connect(lambda: self.transmit_button.setEnabled(True))
        self.transmit_button.setEnabled(False)
        self.thread.start()

    def _transmit(self) -> None:
        message = normalize_transmit_text(self.outgoing_text.toPlainText())
        self.outgoing_text.clear()
        self.transmit_requested.emit(message)

    def _show_error(self, message: str) -> None:
        self.statusBar().showMessage(message)
        self.transmit_button.setEnabled(False)

    def _show_received(self, message: str) -> None:
        self.received_text.moveCursor(self.received_text.textCursor().End)
        self.received_text.insertPlainText(message)

    def closeEvent(self, event: Any) -> None:
        self.shutdown_requested.emit()
        self.thread.quit()
        self.thread.wait(3000)
        event.accept()


def main() -> int:
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    return app.exec_()


if __name__ == "__main__":
    raise SystemExit(main())
