import importlib
import os
import sys
import types
import unittest
from unittest.mock import patch


class FakeSerialPort:
    def __init__(self, device, baudrate, timeout):
        self.device = device
        self.baudrate = baudrate
        self.timeout = timeout
        self.writes = []
        self.is_open = True

    def write(self, data):
        self.writes.append(data)

    def flush(self):
        pass

    def close(self):
        self.is_open = False


class FakeInputStream:
    def __init__(self, **kwargs):
        self.kwargs = kwargs

    def start(self):
        pass

    def stop(self):
        pass

    def close(self):
        pass


class FakeOutputStream:
    last_instance = None

    def __init__(self, **kwargs):
        self.kwargs = kwargs
        self.writes = []
        FakeOutputStream.last_instance = self

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        return False

    def write(self, samples):
        self.writes.append(samples)


def install_dependency_doubles():
    numpy = types.ModuleType("numpy")
    numpy.float32 = "float32"

    class FakeArray(list):
        def reshape(self, *shape):
            if shape == (-1, 1):
                return [[value] for value in self]
            return self

    numpy.asarray = lambda value, dtype=None: FakeArray(value)

    serial = types.ModuleType("serial")
    serial.Serial = FakeSerialPort
    serial.SerialException = RuntimeError
    serial_tools = types.ModuleType("serial.tools")
    list_ports = types.ModuleType("serial.tools.list_ports")
    list_ports.comports = lambda: []
    serial_tools.list_ports = list_ports
    serial.tools = serial_tools

    sounddevice = types.ModuleType("sounddevice")
    sounddevice.InputStream = FakeInputStream
    sounddevice.OutputStream = FakeOutputStream
    sounddevice.query_devices = lambda: []

    olivia_modem = types.ModuleType("olivia_modem")

    class FakeOliviaModem:
        def __init__(self, tones, bandwidth, sample_rate):
            self.configuration = (tones, bandwidth, sample_rate)

        def modulate(self, text):
            return FakeArray([float(len(text)), 2.0])

        def demodulate(self, samples):
            return "decoded"

    olivia_modem.OliviaModem = FakeOliviaModem
    original_modules = {
        name: sys.modules.get(name)
        for name in (
            "numpy",
            "serial",
            "serial.tools",
            "serial.tools.list_ports",
            "sounddevice",
            "olivia_modem",
        )
    }
    sys.modules.update(
        {
            "numpy": numpy,
            "serial": serial,
            "serial.tools": serial_tools,
            "serial.tools.list_ports": list_ports,
            "sounddevice": sounddevice,
            "olivia_modem": olivia_modem,
        }
    )
    return original_modules


_ORIGINAL_MODULES = install_dependency_doubles()
app = importlib.import_module("olivia_qmx")
for _name, _module in _ORIGINAL_MODULES.items():
    if _module is None:
        sys.modules.pop(_name, None)
    else:
        sys.modules[_name] = _module


class Port:
    def __init__(self, device, description="", manufacturer="", product=""):
        self.device = device
        self.description = description
        self.manufacturer = manufacturer
        self.product = product


class OliviaQmxTests(unittest.TestCase):
    def test_normalize_transmit_text_converts_every_typed_escape(self):
        self.assertEqual(
            app.normalize_transmit_text(r"\nfirst\nsecond\n"),
            "\nfirst\nsecond\n",
        )

    def test_normalize_transmit_text_converts_typed_newline_escape(self):
        self.assertEqual(
            app.normalize_transmit_text(r"Hello\nstation B"),
            "Hello\nstation B",
        )

    def test_normalize_transmit_text_preserves_real_line_breaks(self):
        self.assertEqual(
            app.normalize_transmit_text("Hello\nstation B"),
            "Hello\nstation B",
        )

    def test_received_chunks_are_joined_without_forced_line_breaks(self):
        class FakeCursor:
            End = object()

        class FakeTextEdit:
            def __init__(self):
                self.cursor = FakeCursor()
                self.inserted = []

            def textCursor(self):
                return self.cursor

            def moveCursor(self, position):
                self.moved_to = position

            def insertPlainText(self, message):
                self.inserted.append(message)

        class FakeWindow:
            received_text = FakeTextEdit()

        window = FakeWindow()

        app.MainWindow._show_received(window, "Aho")
        app.MainWindow._show_received(window, "is")
        app.MainWindow._show_received(window, "tan")

        self.assertIs(window.received_text.moved_to, window.received_text.cursor.End)
        self.assertEqual(window.received_text.inserted, ["Aho", "is", "tan"])

    def test_station_port_mapping(self):
        self.assertEqual(app.station_port("A", 45800), 45801)
        self.assertEqual(app.station_port("B", 45800), 45802)

    def test_target_station_port_routes_both_directions(self):
        self.assertEqual(app.target_station_port("A", 45800), 45802)
        self.assertEqual(app.target_station_port("B", 45800), 45801)

    def test_target_station_port_rejects_invalid_station(self):
        with self.assertRaisesRegex(ValueError, "must be A or B"):
            app.target_station_port("C", 45800)

    def test_station_port_rejects_invalid_station(self):
        with self.assertRaisesRegex(ValueError, "must be A or B"):
            app.station_port("C", 45800)

    def test_station_listening_status_is_clear_after_transmission(self):
        self.assertEqual(
            app.station_listening_status("A"),
            "Station A transmitted; listening",
        )

    def test_qmx_port_detects_qrp_labs_device(self):
        ports = [
            Port("/dev/ttyUSB0", description="USB UART"),
            Port("/dev/ttyUSB1", manufacturer="QRP Labs"),
        ]
        with patch.object(app.list_ports, "comports", return_value=ports):
            self.assertEqual(app.qmx_port(), "/dev/ttyUSB1")

    def test_qmx_port_reports_missing_radio(self):
        with patch.object(app.list_ports, "comports", return_value=[]):
            with self.assertRaisesRegex(RuntimeError, "No QRP Labs or QMX"):
                app.qmx_port()

    def test_audio_device_detects_qmx(self):
        devices = [{"name": "Built-in Audio"}, {"name": "QMX USB Audio"}]
        with patch.object(app.sd, "query_devices", return_value=devices):
            self.assertEqual(app.qmx_audio_device(), 1)

    def test_codec_configures_olivia_8_250(self):
        codec = app.OliviaCodec()
        self.assertEqual(codec._modem.configuration, (8, 250, app.SAMPLE_RATE))
        self.assertEqual(codec.encode("hello"), [5.0, 2.0])
        self.assertEqual(codec.decode([1.0]), "decoded")

    def test_transmit_without_connection_reports_error(self):
        worker = app.RadioWorker()
        errors = []
        worker.error.connect(errors.append)

        worker.transmit("CQ")

        self.assertEqual(errors, ["TX failed: QMX+ is not connected."])

    def test_transmit_ignores_whitespace_only_messages(self):
        worker = app.RadioWorker()
        worker._codec = app.OliviaCodec()
        worker._simulator = True

        with patch.object(worker._codec, "encode") as encode:
            worker.transmit(" \n\t ")

        encode.assert_not_called()

    def test_audio_callback_reports_decoder_errors(self):
        worker = app.RadioWorker()
        worker._rx_codec = app.OliviaCodec()
        errors = []
        worker.error.connect(errors.append)

        class AudioBlock:
            def __getitem__(self, key):
                self.assert_key = key
                return [1.0]

        with patch.object(worker._rx_codec, "decode", side_effect=ValueError("bad samples")):
            worker._audio_callback(AudioBlock(), 1, None, None)

        self.assertEqual(errors, ["RX decode failed: bad samples"])

    def test_worker_start_sends_frequency_and_usb_mode(self):
        worker = app.RadioWorker()
        serial_instance = FakeSerialPort("/dev/ttyUSB0", app.BAUD_RATE, 1)
        with patch.object(app, "qmx_port", return_value="/dev/ttyUSB0"), \
             patch.object(app, "qmx_audio_device", return_value=3), \
             patch.object(app.serial, "Serial", return_value=serial_instance):
            worker.start()
        self.assertEqual(serial_instance.writes, [b"FA00007040000;", b"MD2;"])
        self.assertEqual(worker._audio.kwargs["device"], 3)

    def test_transmit_sends_tx_audio_then_rx(self):
        worker = app.RadioWorker()
        serial_instance = FakeSerialPort("/dev/ttyUSB0", app.BAUD_RATE, 1)
        worker._codec = app.OliviaCodec()
        worker._serial = serial_instance
        with patch.object(app, "qmx_audio_device", return_value=4):
            worker.transmit("CQ")
        self.assertEqual(serial_instance.writes, [b"TX;", b"RX;"])
        self.assertEqual(FakeOutputStream.last_instance.kwargs["device"], 4)
        self.assertEqual(FakeOutputStream.last_instance.writes, [[[2.0], [2.0]]])

    def test_stop_returns_radio_to_receive_and_closes_serial(self):
        worker = app.RadioWorker()
        serial_instance = FakeSerialPort("/dev/ttyUSB0", app.BAUD_RATE, 1)
        worker._serial = serial_instance
        worker.stop()
        self.assertEqual(serial_instance.writes, [b"RX;"])
        self.assertFalse(serial_instance.is_open)

    def test_simulator_transmits_and_loops_back_message(self):
        import queue

        worker = app.RadioWorker()
        statuses = []
        worker.status.connect(statuses.append)
        worker._codec = app.OliviaCodec()
        worker._rx_codec = app.OliviaCodec()
        worker._simulator = True
        worker._simulated_rx = queue.Queue()
        worker.transmit("HI")
        samples = []
        while not worker._simulated_rx.empty():
            samples.append(worker._simulated_rx.get_nowait())
        self.assertEqual(samples, [[2.0, 2.0]])
        self.assertEqual(statuses, ["Simulator transmitting...", "Simulator receiving"])
        self.assertNotIn("Receiving", statuses)

    def test_simulator_packet_size_is_suitable_for_long_messages(self):
        self.assertEqual(app.SIMULATOR_PACKET_BYTES, 8192)
        self.assertGreater(app.SIMULATOR_RECEIVE_BUFFER_BYTES, 3_000_000)
        self.assertGreater(app.SIMULATOR_PACKET_DELAY_SECONDS, 0)

if __name__ == "__main__":
    unittest.main()
