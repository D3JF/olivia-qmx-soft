#!/usr/bin/env python3
"""Minimal configurable Olivia terminal for a QRP Labs QMX+.

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
from collections import deque
from typing import Any, Iterable, Optional

import numpy as np
import serial
from serial.tools import list_ports
import sounddevice as sd
from PyQt5.QtCore import QTimer, QObject, QThread, QRect, pyqtSignal, pyqtSlot
from PyQt5.QtGui import QColor, QImage, QPainter
from PyQt5.QtWidgets import (
    QApplication,
    QButtonGroup,
    QGroupBox,
    QHBoxLayout,
    QMainWindow,
    QLabel,
    QPushButton,
    QRadioButton,
    QTextEdit,
    QVBoxLayout,
    QWidget,
)

from olivia_modem import OliviaModem


SAMPLE_RATE = 8000
TONE_OPTIONS = (2, 4, 8, 16, 32, 64, 128, 256)
BANDWIDTH_OPTIONS = (125, 250, 500, 1000, 2000)
DEFAULT_TONES = 8
DEFAULT_BANDWIDTH = 250
CHANNELS = 1
BAUD_RATE = 115200
SERIAL_COMMANDS = (b"FA00007040000;", b"MD2;")
SIMULATOR_PACKET_BYTES = 8 * 1024
SIMULATOR_RECEIVE_BUFFER_BYTES = 4 * 1024 * 1024
SIMULATOR_PACKET_DELAY_SECONDS = 0.005
SIMULATOR_AUDIO_BLOCK_SAMPLES = 1024
CENTER_FREQUENCY = 1500
WATERFALL_FFT_SIZE = 1024
WATERFALL_HOP_SIZE = 128
WATERFALL_MIN_DB = -35.0
WATERFALL_MAX_DB = 0.0


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


def reassemble_simulator_packet(
    packet: bytes,
    packets: dict[int, dict[int, bytes]],
    packet_totals: dict[int, int],
    next_sequences: dict[int, int],
) -> list[bytes]:
    """Return newly contiguous audio payloads from one simulator packet."""
    if len(packet) < 12:
        return []
    magic, tx_id, sequence, total = struct.unpack("!4sIHH", packet[:12])
    if magic != b"OLIV" or total == 0 or sequence >= total:
        return []
    packets.setdefault(tx_id, {})[sequence] = packet[12:]
    packet_totals[tx_id] = total
    next_sequences.setdefault(tx_id, 0)

    payloads = []
    while next_sequences[tx_id] in packets[tx_id]:
        payloads.append(packets[tx_id].pop(next_sequences[tx_id]))
        next_sequences[tx_id] += 1
    if next_sequences[tx_id] == packet_totals[tx_id]:
        del packets[tx_id]
        del packet_totals[tx_id]
        del next_sequences[tx_id]
    return payloads


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


class WaterfallWidget(QWidget):
    """Render precomputed waterfall rows without doing signal processing."""

    def __init__(self, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)
        self.setMinimumHeight(180)
        self._image = QImage(1, 1, QImage.Format_RGB32)
        self._image.fill(QColor("#101820"))
        self._pending_rows: deque[np.ndarray] = deque()
        self._transmission_active = False
        self._transmission_finishing = False
        self._next_column = 0
        self._bandwidth = DEFAULT_BANDWIDTH
        self._paint_timer = QTimer(self)
        self._paint_timer.setInterval(33)
        self._paint_timer.timeout.connect(self._paint_pending_rows)
        self._paint_timer.start()

    def resizeEvent(self, event: Any) -> None:
        self._resize_image()
        super().resizeEvent(event)

    def _resize_image(self) -> None:
        width = max(1, self.width())
        height = max(1, self.height())
        if self._image.width() == width and self._image.height() == height:
            return
        image = QImage(width, height, QImage.Format_RGB32)
        image.fill(QColor("#101820"))
        painter = QPainter(image)
        painter.drawImage(0, max(0, height - self._image.height()), self._image)
        painter.end()
        self._image = image

    @pyqtSlot()
    def start_transmission(self) -> None:
        """Clear the display and prepare to receive live transmit samples."""
        self._resize_image()
        self._image.fill(QColor("#101820"))
        self._pending_rows.clear()
        self._transmission_active = True
        self._transmission_finishing = False
        self._next_column = 0
        self._paint_timer.start()
        self.update()

    @pyqtSlot(int)
    def set_bandwidth(self, bandwidth: int) -> None:
        if bandwidth not in BANDWIDTH_OPTIONS:
            raise ValueError("Unsupported waterfall bandwidth.")
        self._bandwidth = bandwidth
        self.update()

    @pyqtSlot(object)
    def add_spectrum_row(self, row: np.ndarray) -> None:
        """Queue one worker-computed row for the next display tick."""
        if not self._transmission_active:
            return
        self._pending_rows.append(np.asarray(row, dtype=np.float32))

    @pyqtSlot()
    def finish_transmission(self) -> None:
        """Paint queued rows, then stop accepting waterfall data."""
        self._transmission_finishing = True

    def _paint_pending_rows(self) -> None:
        if not self._pending_rows:
            if self._transmission_finishing:
                self._transmission_active = False
                self._transmission_finishing = False
                self._paint_timer.stop()
            return
        self._resize_image()
        painter = QPainter(self._image)
        for _ in range(min(4, len(self._pending_rows))):
            source_row = self._pending_rows.popleft()
            positions = np.linspace(
                0, len(source_row) - 1, self._image.height()
            )
            column = np.interp(
                positions, np.arange(len(source_row)), source_row
            )
            width = self._image.width()
            height = self._image.height()
            if self._next_column >= width:
                painter.drawImage(
                    QRect(0, 0, width - 1, height),
                    self._image,
                    QRect(1, 0, width - 1, height),
                )
                self._next_column = width - 1
            for y, value in enumerate(column):
                red = int(255 * value)
                green = int(180 * value)
                blue = int(255 * (1 - value))
                painter.fillRect(
                    self._next_column,
                    y,
                    1,
                    1,
                    QColor(red, green, blue),
                )
            self._next_column += 1
        painter.end()
        self.update()

    def paintEvent(self, event: Any) -> None:
        painter = QPainter(self)
        painter.drawImage(0, 0, self._image)
        painter.setPen(QColor("#d0d7de"))
        minimum = CENTER_FREQUENCY - self._bandwidth / 2
        maximum = CENTER_FREQUENCY + self._bandwidth / 2
        painter.drawText(
            8,
            18,
            f"TX waterfall: {minimum:g}-{maximum:g} Hz",
        )
        painter.end()


class OliviaCodec:
    """Adapt the common olivia-modem APIs to the app's audio interface."""

    def __init__(
        self, tones: int = DEFAULT_TONES, bandwidth: int = DEFAULT_BANDWIDTH
    ) -> None:
        try:
            self._modem = OliviaModem(
                tones=tones, bandwidth=bandwidth, sample_rate=SAMPLE_RATE
            )
        except TypeError:
            self._modem = OliviaModem(tones, bandwidth, SAMPLE_RATE)

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
    connected = pyqtSignal(bool)
    transmitting = pyqtSignal(bool)
    transmission_started = pyqtSignal()
    transmission_finished = pyqtSignal()
    transmission_samples = pyqtSignal(object)
    transmission_spectrum = pyqtSignal(object)

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
        self._connected = False
        self._transmitting = False
        self._tones = DEFAULT_TONES
        self._bandwidth = DEFAULT_BANDWIDTH
        self._codec_lock = threading.Lock()
        self._waterfall_queue: queue.Queue[
            tuple[str, np.ndarray | int | None]
        ] = queue.Queue()
        self._waterfall_thread = threading.Thread(
            target=self._waterfall_loop,
            name="olivia-waterfall",
            daemon=True,
        )
        self._waterfall_thread.start()

    def _start_decoder_thread(self) -> None:
        self._decoder_thread = threading.Thread(
            target=self._decoder_loop,
            name="olivia-decoder",
            daemon=True,
        )
        self._decoder_thread.start()

    def _reset_waterfall(self) -> None:
        self._waterfall_queue.put(("reset", self._bandwidth))
        self._waterfall_queue.join()

    def _publish_transmission_block(self, block: np.ndarray) -> None:
        self.transmission_samples.emit(block)
        self._waterfall_queue.put(
            ("samples", np.asarray(block, dtype=np.float32).copy())
        )

    def _waterfall_loop(self) -> None:
        buffer: Optional[np.ndarray] = None
        window: Optional[np.ndarray] = None
        frequency_mask: Optional[np.ndarray] = None
        while True:
            operation, value = self._waterfall_queue.get()
            try:
                if operation == "stop":
                    return
                if operation == "reset":
                    assert isinstance(value, int)
                    if not hasattr(np, "fft") or not hasattr(np, "empty"):
                        continue
                    buffer = np.empty(0, dtype=np.float32)
                    window = np.hanning(WATERFALL_FFT_SIZE)
                    frequencies = np.fft.rfftfreq(
                        WATERFALL_FFT_SIZE, 1 / SAMPLE_RATE
                    )
                    frequency_mask = (
                        frequencies
                        >= CENTER_FREQUENCY - value / 2
                    ) & (
                        frequencies
                        <= CENTER_FREQUENCY + value / 2
                    )
                    continue
                if operation != "samples" or value is None:
                    continue
                if window is None or frequency_mask is None:
                    continue
                assert buffer is not None
                buffer = np.concatenate((buffer, value))
                while buffer.size >= WATERFALL_FFT_SIZE:
                    spectrum = np.abs(
                        np.fft.rfft(buffer[:WATERFALL_FFT_SIZE] * window)
                    )
                    level = 20 * np.log10(np.maximum(spectrum, 1e-12))
                    selected = level[frequency_mask]
                    relative_level = selected - float(selected.max())
                    row = np.clip(
                        (relative_level - WATERFALL_MIN_DB)
                        / (WATERFALL_MAX_DB - WATERFALL_MIN_DB),
                        0,
                        1,
                    ).astype(np.float32)
                    self.transmission_spectrum.emit(row)
                    buffer = buffer[WATERFALL_HOP_SIZE:]
            finally:
                self._waterfall_queue.task_done()

    @pyqtSlot()
    def start(self) -> None:
        self.connect_radio()

    @pyqtSlot(int, int)
    def set_configuration(self, tones: int, bandwidth: int) -> None:
        if tones not in TONE_OPTIONS or bandwidth not in BANDWIDTH_OPTIONS:
            raise ValueError("Unsupported Olivia configuration.")
        if self._transmitting:
            self.status.emit("Finish transmitting before changing Olivia mode")
            return
        with self._codec_lock:
            self._tones = tones
            self._bandwidth = bandwidth
            if self._connected:
                self._codec = OliviaCodec(tones, bandwidth)
                self._rx_codec = OliviaCodec(tones, bandwidth)
        self.status.emit(f"Olivia {tones}/{bandwidth} configured")

    @pyqtSlot()
    def connect_radio(self) -> None:
        if self._connected:
            return
        self._running = True
        try:
            self._codec = OliviaCodec(self._tones, self._bandwidth)
            self._rx_codec = OliviaCodec(self._tones, self._bandwidth)
            if self._decoder_thread is None or not self._decoder_thread.is_alive():
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
                self._connected = True
                self.connected.emit(True)
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
            self._connected = True
            self.status.emit(
                f"Connected to {port}; Olivia {self._tones}/{self._bandwidth} RX active"
            )
            self.connected.emit(True)
            self.ready.emit()
        except Exception as exc:
            self._close_connection()
            self._codec = None
            self._rx_codec = None
            self.status.emit(f"Disconnected: {exc}")
            self.connected.emit(False)
            self.error.emit(str(exc))

    @pyqtSlot()
    def disconnect_radio(self) -> None:
        if not self._connected:
            return
        self._close_connection()
        self.status.emit("Disconnected")
        self.connected.emit(False)

    def _close_connection(self) -> None:
        if self._simulator:
            if not self._station_id:
                self._simulated_rx.put(None)
            if self._simulated_socket is not None:
                self._simulated_socket.close()
                self._simulated_socket = None
            if self._simulated_rx_thread is not None:
                self._simulated_rx_thread.join(timeout=3)
                self._simulated_rx_thread = None
        if self._audio is not None:
            self._audio.stop()
            self._audio.close()
            self._audio = None
        if self._serial is not None and self._serial.is_open:
            try:
                self._serial.write(b"RX;")
                self._serial.flush()
            except serial.SerialException as exc:
                self.error.emit(f"Could not return QMX+ to RX: {exc}")
            self._serial.close()
        self._serial = None
        self._connected = False

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
        try:
            with self._codec_lock:
                if self._rx_codec is None:
                    return
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
        packet_totals: dict[int, int] = {}
        next_sequences: dict[int, int] = {}
        while self._running:
            try:
                packet, _ = self._simulated_socket.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                return
            # Decode each contiguous packet as soon as it arrives. This keeps
            # simulator latency close to radio latency while still handling
            # occasional UDP reordering.
            for payload in reassemble_simulator_packet(
                packet, packets, packet_totals, next_sequences
            ):
                samples = np.frombuffer(payload, dtype=np.float32)
                for start in range(0, len(samples), SIMULATOR_AUDIO_BLOCK_SAMPLES):
                    block = samples[start : start + SIMULATOR_AUDIO_BLOCK_SAMPLES]
                    self._audio_callback(
                        block.reshape(-1, 1), len(block), None, None
                    )

    @pyqtSlot(str)
    def transmit(self, message: str) -> None:
        if not message.strip():
            return
        with self._tx_lock:
            try:
                if not self._connected or self._codec is None:
                    raise RuntimeError("QMX+ is not connected.")
                self.transmitting.emit(True)
                self._transmitting = True
                self.transmission_started.emit()
                self._reset_waterfall()
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
                            packet_samples = np.frombuffer(
                                chunk, dtype=np.float32
                            )
                            for start in range(
                                0, len(packet_samples), SIMULATOR_AUDIO_BLOCK_SAMPLES
                            ):
                                block = packet_samples[start : start + SIMULATOR_AUDIO_BLOCK_SAMPLES]
                                self._publish_transmission_block(block)
                                time.sleep(
                                    len(block) / SAMPLE_RATE
                                )
                        self.status.emit(station_listening_status(self._station_id))
                    else:
                        for start in range(0, len(samples), 1024):
                            block = samples[start : start + 1024]
                            for offset in range(0, len(block), SIMULATOR_AUDIO_BLOCK_SAMPLES):
                                audio_block = block[
                                    offset : offset + SIMULATOR_AUDIO_BLOCK_SAMPLES
                                ]
                                self._publish_transmission_block(audio_block)
                                self._simulated_rx.put(audio_block)
                                time.sleep(len(audio_block) / SAMPLE_RATE)
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
                    for start in range(0, len(samples), 1024):
                        block = np.asarray(
                            samples[start : start + 1024], dtype=np.float32
                        )
                        self._publish_transmission_block(block)
                        output.write(block.reshape(-1, 1))
            except Exception as exc:
                if not self._simulator:
                    self._close_connection()
                    self.status.emit("Disconnected after TX failure")
                    self.connected.emit(False)
                self.error.emit(f"TX failed: {exc}")
            finally:
                self._waterfall_queue.join()
                self._transmitting = False
                if self._serial is not None and self._serial.is_open:
                    try:
                        self._serial.write(b"RX;")
                        self._serial.flush()
                    except serial.SerialException as exc:
                        self.error.emit(f"Could not return QMX+ to RX: {exc}")
                if not self._simulator and self._connected:
                    self.status.emit("Receiving")
                self.transmitting.emit(False)
                self.transmission_finished.emit()

    @pyqtSlot()
    def stop(self) -> None:
        self._running = False
        if self._simulator and not self._station_id:
            self._simulated_rx.put(None)
        self._close_connection()
        self._decoder_queue.put(None)
        self._waterfall_queue.join()
        self._waterfall_queue.put(("stop", None))
        self._waterfall_thread.join(timeout=3)
        if self._decoder_thread is not None:
            self._decoder_thread.join(timeout=3)


class MainWindow(QMainWindow):
    transmit_requested = pyqtSignal(str)
    configuration_requested = pyqtSignal(int, int)
    shutdown_requested = pyqtSignal()
    connect_requested = pyqtSignal()
    disconnect_requested = pyqtSignal()

    def __init__(self) -> None:
        super().__init__()
        station = os.environ.get("OLIVIA_QMX_STATION", "").upper()
        station_label = f"Station {station}" if station in ("A", "B") else "Local"
        self.setWindowTitle(f"Olivia MFSK - QMX+ ({station_label})")
        self.resize(720, 520)

        self.station_label = QLabel(station_label)
        self.waterfall = WaterfallWidget()
        self.received_text = QTextEdit()
        self.received_text.setReadOnly(True)
        self.received_text.setPlaceholderText("Received text")
        self.outgoing_text = QTextEdit()
        self.outgoing_text.setPlaceholderText("Message to transmit")
        self.connection_button = QPushButton("Reconnect")
        self.transmit_button = QPushButton("TRANSMIT")
        self.clear_button = QPushButton("Clear received")
        self.tone_group = QButtonGroup(self)
        self.bandwidth_group = QButtonGroup(self)
        self.tone_buttons = self._create_radio_group(
            "Tones", TONE_OPTIONS, self.tone_group, DEFAULT_TONES
        )
        self.bandwidth_buttons = self._create_radio_group(
            "Bandwidth (Hz)",
            BANDWIDTH_OPTIONS,
            self.bandwidth_group,
            DEFAULT_BANDWIDTH,
        )
        self.connection_button.setMinimumHeight(40)
        self.transmit_button.setMinimumHeight(64)
        self.connection_button.clicked.connect(self._toggle_connection)
        self.transmit_button.clicked.connect(self._transmit)
        self.clear_button.clicked.connect(self.received_text.clear)

        layout = QVBoxLayout()
        layout.addWidget(self.station_label)
        layout.addWidget(self.connection_button)
        layout.addWidget(self.tone_buttons)
        layout.addWidget(self.bandwidth_buttons)
        layout.addWidget(self.waterfall)
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
        self.configuration_requested.connect(self.worker.set_configuration)
        self.connect_requested.connect(self.worker.connect_radio)
        self.disconnect_requested.connect(self.worker.disconnect_radio)
        self.shutdown_requested.connect(self.worker.stop)
        self.worker.received.connect(self._show_received)
        self.worker.status.connect(self.statusBar().showMessage)
        self.worker.error.connect(self._show_error)
        self.worker.ready.connect(lambda: self.transmit_button.setEnabled(True))
        self.worker.connected.connect(self._connection_changed)
        self.worker.transmitting.connect(self._transmitting_changed)
        self.worker.transmission_started.connect(self.waterfall.start_transmission)
        self.worker.transmission_finished.connect(self.waterfall.finish_transmission)
        self.worker.transmission_spectrum.connect(self.waterfall.add_spectrum_row)
        self.connection_button.setEnabled(False)
        self.transmit_button.setEnabled(False)
        self.thread.start()

    def _create_radio_group(
        self,
        title: str,
        options: tuple[int, ...],
        group: QButtonGroup,
        default: int,
    ) -> QGroupBox:
        box = QGroupBox(title)
        buttons_layout = QHBoxLayout(box)
        for value in options:
            label = f"{value} Hz" if group is self.bandwidth_group else str(value)
            button = QRadioButton(label)
            button.setProperty("configuration_value", value)
            group.addButton(button, value)
            buttons_layout.addWidget(button)
            if value == default:
                button.setChecked(True)
        group.buttonClicked[int].connect(self._configuration_changed)
        return box

    def _configuration_changed(self, _value: int) -> None:
        tones = self.tone_group.checkedId()
        bandwidth = self.bandwidth_group.checkedId()
        if tones > 0 and bandwidth > 0:
            self.waterfall.set_bandwidth(bandwidth)
            self.configuration_requested.emit(tones, bandwidth)

    def _transmit(self) -> None:
        message = normalize_transmit_text(self.outgoing_text.toPlainText())
        self.outgoing_text.clear()
        self.transmit_requested.emit(message)

    def _show_error(self, message: str) -> None:
        self.statusBar().showMessage(message)

    def _toggle_connection(self) -> None:
        if self.connection_button.text() == "Disconnect":
            self.disconnect_requested.emit()
        else:
            self.connection_button.setEnabled(False)
            self.connect_requested.emit()

    def _connection_changed(self, connected: bool) -> None:
        self.connection_button.setEnabled(True)
        self.connection_button.setText("Disconnect" if connected else "Reconnect")
        self.transmit_button.setEnabled(connected)

    def _transmitting_changed(self, transmitting: bool) -> None:
        for button in self.tone_group.buttons() + self.bandwidth_group.buttons():
            button.setEnabled(not transmitting)
        if transmitting:
            self.transmit_button.setEnabled(False)
        else:
            self.transmit_button.setEnabled(self.connection_button.text() == "Disconnect")

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
