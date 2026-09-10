import importlib.util
import io
from pathlib import Path
import unittest

import serial


SCRIPT = Path(__file__).resolve().parents[1] / "read_serial.py"


def load_script():
    spec = importlib.util.spec_from_file_location("read_serial_under_test", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class FakeSerial:
    def __init__(self):
        object.__setattr__(self, "events", [])

    def __setattr__(self, name, value):
        object.__setattr__(self, name, value)
        if name != "events":
            self.events.append((name, value))

    def open(self):
        self.events.append(("open", None))


class FakeReadable:
    def __init__(self, reads):
        self.reads = list(reads)
        self.closed = False

    def readline(self):
        if not self.reads:
            return b""
        item = self.reads.pop(0)
        if isinstance(item, Exception):
            raise item
        return item

    def close(self):
        self.closed = True


class ReadSerialNoResetTest(unittest.TestCase):
    def test_control_lines_are_deasserted_before_open(self):
        module = load_script()
        fake = FakeSerial()
        module.serial.Serial = lambda: fake

        self.assertIs(fake, module.open_serial_no_reset("COM7"))
        self.assertEqual(
            [
                ("port", "COM7"),
                ("baudrate", 115200),
                ("timeout", 0.5),
                ("dtr", False),
                ("rts", False),
                ("open", None),
            ],
            fake.events,
        )

    def test_reconnect_mode_survives_unplug_without_toggling_reset(self):
        module = load_script()
        first = FakeReadable([serial.SerialException("unplugged")])
        second = FakeReadable([b"boot line\n"])
        connections = [first, second]
        module.open_serial_no_reset = lambda _port: connections.pop(0)
        ticks = iter((0.0, 0.0, 0.0, 0.0, 2.0))
        output = io.StringIO()

        module.capture_serial(
            "COM7",
            1,
            reconnect=True,
            output=output,
            clock=lambda: next(ticks),
            sleep_fn=lambda _seconds: None,
        )

        self.assertTrue(first.closed)
        self.assertTrue(second.closed)
        self.assertIn("SERIAL_DISCONNECTED", output.getvalue())
        self.assertIn("SERIAL_RECONNECTED no_reset=1", output.getvalue())
        self.assertIn("boot line", output.getvalue())


if __name__ == "__main__":
    unittest.main()
