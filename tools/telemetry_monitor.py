#!/usr/bin/env python3
import argparse
import json
import os
import select
import sys
import termios
import time
import tty


def configure_serial(fd, baud):
    attrs = termios.tcgetattr(fd)
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    attrs[4] = baud
    attrs[5] = baud
    attrs[2] |= termios.CLOCAL | termios.CREAD
    attrs[2] &= ~termios.CSTOPB
    attrs[2] &= ~termios.PARENB
    attrs[2] &= ~termios.CSIZE
    attrs[2] |= termios.CS8
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("port")
    parser.add_argument("--interval", type=float, default=2.0)
    parser.add_argument("--seconds", type=float, default=0)
    args = parser.parse_args()

    fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    configure_serial(fd, termios.B115200)
    start = time.monotonic()
    next_poll = 0
    request_id = 1
    buffer = ""

    try:
        while args.seconds <= 0 or time.monotonic() - start < args.seconds:
            now = time.monotonic()
            if now >= next_poll:
                request = {"qt": 1, "id": request_id, "command": "get-telemetry"}
                os.write(fd, (json.dumps(request) + "\n").encode())
                request_id += 1
                next_poll = now + args.interval

            readable, _, _ = select.select([fd], [], [], 0.1)
            if fd not in readable:
                continue

            try:
                chunk = os.read(fd, 4096)
            except BlockingIOError:
                continue
            if not chunk:
                continue

            buffer += chunk.decode(errors="replace")
            while "\n" in buffer:
                line, buffer = buffer.split("\n", 1)
                line = line.strip()
                if not line:
                    continue
                try:
                    message = json.loads(line)
                except json.JSONDecodeError:
                    print(f"raw {line}", flush=True)
                    continue

                if message.get("type") != "telemetry":
                    print(f"msg {line}", flush=True)
                    continue

                data = message.get("data", {})
                counters = data.get("counters", {})
                last = data.get("last", {})
                print(
                    "telemetry "
                    f"up={data.get('uptimeMs')} heap={data.get('freeHeap')} "
                    f"host={data.get('hostInterfaces')} kbdIf={data.get('keyboardInterfaces')} "
                    f"req={counters.get('hostReportRequests')} cb={counters.get('hostReportCallbacks')} "
                    f"kbd={counters.get('keyboardReports')} fwd={counters.get('forwardedKeyboard')} "
                    f"zlen={counters.get('zeroLengthReports')} decFail={counters.get('keyboardDecodeFails')} "
                    f"q={counters.get('hostQuiesces')} rec={counters.get('hostRecovers')} "
                    f"abort={counters.get('hostReceiveAborts')} abortFail={counters.get('hostReceiveAbortFails')} "
                    f"sendFail={counters.get('keyboardSendFails')} hidNotReady={counters.get('hidNotReady')} "
                    f"lastHost={last.get('hostReportMs')} lastFwd={last.get('forwardedKeyboardMs')}",
                    flush=True,
                )
    finally:
        os.close(fd)


if __name__ == "__main__":
    sys.exit(main())
