import serial
import sys
import time


def open_serial_no_reset(port: str):
    connection = serial.Serial()
    connection.port = port
    connection.baudrate = 115200
    connection.timeout = 0.5
    # Configure modem-control lines before opening the port. PySerial defaults
    # both to asserted, which can reset ESP32-P4 boards wired to DTR/RTS.
    connection.dtr = False
    connection.rts = False
    connection.open()
    return connection


def capture_serial(
    port: str,
    duration: int,
    reconnect: bool = False,
    output=None,
    clock=time.monotonic,
    sleep_fn=time.sleep,
) -> None:
    """Read without touching modem lines; optionally survive physical unplug/replug."""
    output = output or sys.stdout
    deadline = clock() + duration
    connection = None
    connected_once = False
    try:
        while clock() < deadline:
            if connection is None:
                try:
                    connection = open_serial_no_reset(port)
                except serial.SerialException:
                    if not reconnect:
                        raise
                    sleep_fn(0.1)
                    continue
                if connected_once:
                    print("# SERIAL_RECONNECTED no_reset=1", file=output, flush=True)
                connected_once = True
            try:
                line = connection.readline()
            except serial.SerialException as exc:
                try:
                    connection.close()
                finally:
                    connection = None
                if not reconnect:
                    raise
                print(
                    f"# SERIAL_DISCONNECTED waiting=1 error={type(exc).__name__}",
                    file=output,
                    flush=True,
                )
                sleep_fn(0.1)
                continue
            if line:
                print(
                    line.decode("utf-8", "replace").rstrip(),
                    file=output,
                    flush=True,
                )
    finally:
        if connection is not None:
            connection.close()


def main() -> int:
    port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
    duration = int(sys.argv[2]) if len(sys.argv) > 2 else 60
    reconnect = (
        len(sys.argv) > 3
        and sys.argv[3].strip().lower() in {"reconnect", "1", "true", "yes"}
    )
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    capture_serial(port, duration, reconnect=reconnect)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
