#!/usr/bin/env python3
"""Connect OpenSSH to a Henge guest over the Mac's paired USB connection."""

import argparse
import hashlib
import os
import plistlib
import re
import shlex
import socket
import struct
import sys
import threading


MUX_SOCKET = "/var/run/usbmuxd"
HENGE_PORT = 40222
MUX_HEADER = struct.Struct("<IIII")


def read_exact(stream, length):
    data = bytearray()
    while len(data) < length:
        chunk = stream.recv(length - len(data))
        if not chunk:
            raise OSError("The USB service closed the connection")
        data.extend(chunk)
    return data


def mux_request(stream, message, **fields):
    body = plistlib.dumps({
        "MessageType": message,
        "ProgName": "Henge SSH",
        "ClientVersionString": "Henge SSH",
        **fields,
    })
    stream.sendall(MUX_HEADER.pack(MUX_HEADER.size + len(body), 1, 8, 1) + body)
    size, version, kind, tag = MUX_HEADER.unpack(read_exact(stream, MUX_HEADER.size))
    if not MUX_HEADER.size < size <= 1024 * 1024 or (version, kind, tag) != (1, 8, 1):
        raise OSError("Invalid reply from the USB service")
    return plistlib.loads(read_exact(stream, size - MUX_HEADER.size))


def mux_socket():
    stream = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    stream.settimeout(8)
    try:
        stream.connect(MUX_SOCKET)
    except OSError:
        stream.close()
        raise
    return stream


def normalize(serial):
    return serial.replace("-", "").lower()


def find_device(serial):
    with mux_socket() as stream:
        devices = mux_request(stream, "ListDevices").get("DeviceList", [])
    attached = []
    for device in devices:
        props = device.get("Properties", {})
        if props.get("ConnectionType") != "USB" or "SerialNumber" not in props:
            continue
        if not serial or normalize(props["SerialNumber"]) == normalize(serial):
            attached.append((device["DeviceID"], props["SerialNumber"]))
    if len(attached) != 1:
        raise OSError("Connect and trust one iPad over USB, or select it with --device")
    return attached[0]


def open_guest(device_id):
    stream = mux_socket()
    try:
        reply = mux_request(stream, "Connect", DeviceID=device_id, PortNumber=socket.htons(HENGE_PORT))
        if reply.get("Number") != 0:
            raise OSError("The iPad refused the connection. Enable Local SSH in Henge’s Details.")
    except Exception:
        stream.close()
        raise
    return stream


def relay(stream):
    def upload():
        try:
            while chunk := os.read(sys.stdin.fileno(), 65536):
                stream.sendall(chunk)
            stream.shutdown(socket.SHUT_WR)
        except OSError:
            pass

    stream.settimeout(None)
    threading.Thread(target=upload, daemon=True).start()
    while chunk := stream.recv(65536):
        sys.stdout.buffer.write(chunk)
        sys.stdout.buffer.flush()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", help="iPad UDID, needed only when multiple devices are connected")
    parser.add_argument("--check", action="store_true", help="check that the guest replies with an SSH banner")
    parser.add_argument("--proxy", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--identity", help="SSH private key file")
    parser.add_argument("--control-socket", help="reuse authenticated sessions for up to ten minutes")
    parser.add_argument("--batch", action="store_true", help="disable interactive prompts")
    parser.add_argument("user", nargs="?", help="short macOS username inside the guest")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="optional command to run in the guest")
    args = parser.parse_args()

    if not (args.proxy or args.check) and not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]*", args.user or ""):
        parser.error("a valid guest username is required")
    try:
        device_id, serial = find_device(args.device)
        if args.proxy or args.check:
            with open_guest(device_id) as stream:
                if args.check:
                    if read_exact(stream, 4) != b"SSH-":
                        raise OSError("The guest did not return an SSH banner")
                    print("The virtual Mac is responding to SSH over USB.")
                else:
                    relay(stream)
            return

        proxy = shlex.join([sys.executable, os.path.abspath(__file__), "--proxy", "--device", serial])
        alias = "henge-ipad-" + hashlib.sha256(normalize(serial).encode()).hexdigest()[:16]
        ssh = ["/usr/bin/ssh", "-o", f"ProxyCommand={proxy}", "-o", f"HostKeyAlias={alias}"]
        if args.identity:
            ssh += ["-i", args.identity, "-o", "IdentitiesOnly=yes"]
        if args.batch:
            ssh += ["-o", "BatchMode=yes"]
        if args.control_socket:
            ssh += ["-o", f"ControlPath={args.control_socket}", "-o", "ControlMaster=auto", "-o", "ControlPersist=10m"]
        command = args.command[1:] if args.command[:1] == ["--"] else args.command
        os.execv(ssh[0], ssh + [f"{args.user}@henge-guest", *command])
    except (OSError, ValueError, plistlib.InvalidFileException) as error:
        print(f"Henge SSH: {error}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
