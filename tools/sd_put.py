#!/usr/bin/env python3
"""Copy a file onto the badge's microSD card over the USB console (no card reader).

    python tools/sd_put.py --port /dev/cu.usbserial-110 game.gb roms/game.gb

Uses the console's `sdrecv` command (main/lua/lua_cmd.c): the badge asks for the
file's base64 a block at a time and checks the size and SHA-256 at the end. About
11 KB/s at 115200 baud, so a 1 MB file takes about 2 minutes.
"""
import argparse, base64, hashlib, os, re, sys, time
import serial

p = argparse.ArgumentParser()
p.add_argument("src", help="file on this computer")
p.add_argument("dest", help="path on the card, e.g. roms/game.gb (letters, digits, . _ - /)")
p.add_argument("--port", default="/dev/cu.usbserial-110")
a = p.parse_args()

data = open(a.src, "rb").read()
b64 = base64.b64encode(data)
sha = hashlib.sha256(data).hexdigest()
if not re.fullmatch(r"[A-Za-z0-9._\-/]{1,120}", a.dest) or ".." in a.dest or a.dest.startswith("/"):
    sys.exit("dest: relative path of letters, digits, . _ - / only (no spaces)")

s = serial.Serial()
s.port, s.baudrate, s.timeout = a.port, 115200, 0.1
s.dtr = s.rts = False       # don't hold the chip in reset/boot
s.open()

def read_line(timeout):
    buf, end = b"", time.time() + timeout
    while time.time() < end:
        c = s.read(1)
        if not c:
            continue
        if c == b"\n":
            return buf.decode("utf-8", "replace").strip()
        buf += c
    return None

# Opening the CH340 port can reset the board: wait for its console if it did.
head = s.read(4096)
if b"ESP-ROM" in head or b"rst:" in head:
    print("badge rebooted, waiting for the console...")
    end = time.time() + 20
    while time.time() < end and b"console ready" not in head:
        head += s.read(4096)
    time.sleep(2.5)     # SD idle after CH340 reset; 16 MB puts are expensive to retry
s.reset_input_buffer()

s.write(f"sdrecv {a.dest} {len(data)} {sha}\r\n".encode())
sent, t0 = 0, time.time()
while True:
    line = read_line(10)
    if line is None:
        sys.exit("no answer from the badge (is the port right, is Solana OS running?)")
    m = re.match(r"recv: (ready|more) (\d+)", line)
    if m:
        n = int(m.group(2))
        s.write(b64[sent:sent + n])
        sent += min(n, len(b64) - sent)
        pct = 100 * sent // max(len(b64), 1)
        print(f"\r{pct:3d}%  {sent * 3 // 4 // 1024} / {len(data) // 1024} KB", end="", flush=True)
    elif line.startswith("recv: ok"):
        print(f"\ndone: {a.dest}, {len(data)} bytes, SHA-256 checked, {time.time() - t0:.0f} s")
        break
    elif line.startswith("recv: error") or line.startswith("usage: sdrecv"):
        sys.exit(f"\n{line}")
s.close()
