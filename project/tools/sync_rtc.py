"""set and verify the display RTC over USB using fresh computer UTC.

called after PlatformIO upload; for RTC power-loss recovery, connect USB and
run with PlatformIO's Python. no Bluetooth, cached time or extra package needed.
"""
from __future__ import annotations

import argparse
from collections import deque
from datetime import datetime, timezone
import re
import secrets
import sys
import time

# bound UTC dates, device discovery and hardware readback tolerance.
MIN_UTC, MAX_UTC = 946684800, 4102444799
USB_VID, USB_PID = 0x2886, 0x8045
DISCOVERY_SECONDS = 30.0
COMMUNICATION_SECONDS = 15.0
TOLERANCE_SECONDS = 2


# report discovery, readiness or verification failures to the caller.
class RtcSyncError(RuntimeError):
    """watch discovery, readiness or RTC verification failed."""


# read fresh computer UTC within the RTC calendar range.
def host_utc():
    stamp = int(time.time())
    # reject host time outside the supported RTC years.
    if not MIN_UTC <= stamp <= MAX_UTC:
        raise RtcSyncError("Computer UTC clock must be in years 2000 through 2099")
    return stamp


# select one matching XIAO application port without guessing.
def application_port(ports, requested=None):
    """match the application VID/PID; never guess a port or select the bootloader."""
    matching = [p for p in ports if p.vid == USB_VID and p.pid == USB_PID]
    # honor an explicit port only when its VID/PID also matches.
    if requested:
        matching = [p for p in matching if p.device == requested]
    # reject ambiguous device selection.
    if len(matching) > 1:
        raise RtcSyncError(
            "Multiple XIAO application ports found; connect only the watch or "
            "run tools/sync_rtc.py --port <application-port>"
        )
    return matching[0].device if matching else None


# frame bounded USB lines while diagnostics and RTC replies interleave.
class _Lines:
    """bound partial USB frames and handle interleaved diagnostics."""

    # retain partial input, completed lines and overflow-discard state.
    def __init__(self, connection):
        self.connection = connection
        self.pending = bytearray()
        self.ready = deque(maxlen=32)
        self.discarding = False

    # read the next bounded line before the monotonic deadline.
    def read(self, deadline):
        # poll until a complete line arrives or the deadline expires.
        while time.monotonic() < deadline:
            # return already-framed input before reading more USB bytes.
            if self.ready:
                return self.ready.popleft()
            chunk = self.connection.read(min(256, max(1, self.connection.in_waiting)))
            # frame each received byte without allowing unbounded buffering.
            for byte in chunk:
                # finish a line and reset discard state at LF.
                if byte == 10:
                    # queue only lines that stayed within the length limit.
                    if not self.discarding:
                        self.ready.append(self.pending.decode("ascii", errors="replace").rstrip("\r"))
                    self.pending.clear()
                    self.discarding = False
                # collect bytes only while the current frame remains valid.
                elif not self.discarding:
                    # discard an oversized line through its next LF.
                    if len(self.pending) >= 512:
                        self.pending.clear()
                        self.discarding = True
                    # append an in-range byte to the partial line.
                    else:
                        self.pending.append(byte)
        return None


# write one complete ASCII command without an unbounded flush.
def _write(connection, command):
    payload = command.encode("ascii")
    # reject incomplete serial writes.
    if connection.write(payload) != len(payload):
        raise RtcSyncError("Incomplete USB command write; RTC synchronization was not verified")
    # avoid flush(): serial drivers may block indefinitely.


# recognize fresh diagnostics from a progressing watch task.
def _running(line):
    # ignore unrelated or absent diagnostic lines.
    if not line or not line.startswith("WATCH uptime_ms="):
        return False
    fields = dict(re.findall(r"([a-z_]+)=([^ ]+)", line))
    # validate stage, board readiness and recent task progress.
    try:
        return (fields.get("stage") == "running" and fields.get("board") == "ready"
                and int(fields.get("loops", "0")) > 0
                and 0 <= int(fields.get("progress_age_ms", "99999")) < 2000
                and 0 <= int(fields.get("snapshot_age_ms", "99999")) < 2000)
    # treat malformed diagnostic numbers as not ready.
    except ValueError:
        return False


# wait for both a running application and the RTC USB protocol marker.
def _wait_running(connection, lines, deadline):
    running = False
    protocol_ready = False
    # request status repeatedly within the overall deadline.
    while time.monotonic() < deadline:
        _write(connection, "?\n")
        poll_until = min(deadline, time.monotonic() + 0.75)
        # consume interleaved status lines during this poll window.
        while time.monotonic() < poll_until:
            line = lines.read(poll_until)
            # update readiness from the latest watch report.
            if line and line.startswith("WATCH uptime_ms="):
                running = _running(line)
            # recognize the supported RTC command protocol.
            if line == "RTC USB protocol=1":
                protocol_ready = True
            # continue provisioning only when both readiness checks pass.
            if running and protocol_ready:
                return
    raise RtcSyncError("Watch did not report a running application with RTC USB protocol 1 before timeout")


# match a nonce-tagged RTC reply and validate it against fresh host UTC.
def _response(lines, operation, nonce, deadline):
    prefix = "%s,%d," % (operation, nonce)
    # wait for the requested operation within its deadline.
    while time.monotonic() < deadline:
        line = lines.read(deadline)
        # ignore diagnostics and replies from other requests.
        if line is None or not line.startswith(prefix):
            continue
        match = re.fullmatch(re.escape(prefix) + r"OK,([0-9]{1,10})", line)
        # reject failure replies or malformed success payloads.
        if not match:
            raise RtcSyncError("Watch rejected %s or returned an invalid reply: %s" % (operation, line))
        stamp = int(match.group(1))
        # reject RTC dates outside the supported calendar.
        if not MIN_UTC <= stamp <= MAX_UTC:
            raise RtcSyncError("Watch returned an out-of-range RTC date")
        # reject readback more than two seconds from host UTC.
        if abs(stamp - host_utc()) > TOLERANCE_SECONDS:
            raise RtcSyncError("RTC readback differs from computer UTC by more than two seconds")
        return stamp
    raise RtcSyncError("Timed out waiting for %s confirmation from the watch" % operation)


# provision fresh UTC only after readiness, then verify physical ticking.
def synchronize_connection(connection):
    """wait for running firmware, set fresh host UTC and verify RTC ticking."""
    deadline = time.monotonic() + COMMUNICATION_SECONDS
    lines = _Lines(connection)
    _wait_running(connection, lines, min(deadline, time.monotonic() + 7.0))
    nonce = secrets.randbits(32)
    # sample UTC after upload and USB enumeration, immediately before setting it.
    stamp = host_utc()
    _write(connection, "RTCSET,%d,%d\n" % (nonce, stamp))
    written = _response(lines, "RTCSET", nonce, min(deadline, time.monotonic() + 3.0))
    # RTCGET reads hardware; advancing time proves ticking, not an echoed value.
    time.sleep(1.2)
    nonce = (nonce + 1) & 0xFFFFFFFF
    _write(connection, "RTCGET,%d\n" % nonce)
    advanced = _response(lines, "RTCGET", nonce, min(deadline, time.monotonic() + 3.0))
    # reject an RTC that accepts writes but does not advance.
    if advanced <= written:
        raise RtcSyncError("RTC accepted the time but did not advance; oscillator verification failed")
    return advanced


# discover and open the watch with bounded retries and guaranteed closure.
def synchronize(port=None):
    """open the application port with bounded retries; always close it."""
    # use pyserial from the PlatformIO environment.
    try:
        import serial
        from serial.tools import list_ports
    # explain how to recover a missing serial dependency.
    except ImportError as error:
        raise RtcSyncError("Run this helper with PlatformIO's Python (which includes pyserial)") from error

    # validate host time before starting bounded device discovery.
    host_utc()
    deadline = time.monotonic() + DISCOVERY_SECONDS
    last_error = "No XIAO application USB port (VID 2886, PID 8045) appeared"
    connection = None
    # retry application-port discovery until its deadline.
    while time.monotonic() < deadline:
        # rediscover after each failed open or USB renumbering.
        try:
            device = application_port(list_ports.comports(), port)
            # open only a verified application port.
            if device:
                # 115200 baud avoids the bootloader's 1200-baud reset trigger.
                connection = serial.Serial(device, 115200, timeout=0.1, write_timeout=1.0)
                break
        # retain the latest device/open error for the final diagnostic.
        except (serial.SerialException, OSError) as error:
            last_error = str(error)
        time.sleep(0.25)
    # fail if no application port could be opened.
    if connection is None:
        raise RtcSyncError("Cannot open watch USB port: %s. Close serial monitors and retry." % last_error)
    # synchronize and print the verified UTC timestamp.
    try:
        print("RTC: waiting for watch application on %s" % connection.port, flush=True)
        verified = synchronize_connection(connection)
        readable = datetime.fromtimestamp(verified, timezone.utc).isoformat()
        print("RTC: set from computer UTC and verified advancing: %s (%d)" % (readable, verified), flush=True)
        return verified
    # translate transport failures into an RTC synchronization error.
    except (serial.SerialException, OSError) as error:
        raise RtcSyncError("USB RTC synchronization failed: %s" % error) from error
    # release the serial port after success or failure.
    finally:
        connection.close()


# parse the optional port and return a shell-friendly result.
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="Optional application USB port; must match the watch's VID/PID")
    args = parser.parse_args()
    # run provisioning using the requested or discovered application port.
    try:
        synchronize(args.port)
    # report synchronization failure with a nonzero exit status.
    except RtcSyncError as error:
        print("RTC sync error: %s" % error, file=sys.stderr)
        return 1
    return 0


# run the command-line helper only when invoked directly.
if __name__ == "__main__":
    sys.exit(main())
