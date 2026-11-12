#!/usr/bin/env python3
"""Make the text of an APRS message that changes a setting on a LoRa APRS digipeater over the air.

    python3 tools/remote_code.py K6ATV-10 --chip 2F0040001350524E34393620 "set moving on"
    python3 tools/remote_code.py K6ATV-10 --secret "my own secret" "get moving"

Send the printed text as an APRS message to the digipeater's callsign (APRSdroid, a tracker, the LoRa APRS Pi...).
It answers with a message back. The key is the digipeater's chip ID (printed at start-up on its serial port) unless a
secret was set on it with  set secret ...  The command itself stays readable; the key never goes on the air.

The counter is the time in seconds, so each message is new and a recorded one cannot be sent again. Only Python 3, no packages.
"""
import argparse
import struct
import sys
import time

MASK = (1 << 64) - 1
B32 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"


def _rotl(x, b):
    return ((x << b) | (x >> (64 - b))) & MASK


def siphash24(key, msg):
    """SipHash-2-4, 64-bit output (the same as src/digi3/remote.c)."""
    k0, k1 = struct.unpack("<QQ", key)
    v = [0x736f6d6570736575 ^ k0, 0x646f72616e646f6d ^ k1, 0x6c7967656e657261 ^ k0, 0x7465646279746573 ^ k1]

    def rounds(n):
        for _ in range(n):
            v[0] = (v[0] + v[1]) & MASK; v[1] = _rotl(v[1], 13); v[1] ^= v[0]; v[0] = _rotl(v[0], 32)
            v[2] = (v[2] + v[3]) & MASK; v[3] = _rotl(v[3], 16); v[3] ^= v[2]
            v[0] = (v[0] + v[3]) & MASK; v[3] = _rotl(v[3], 21); v[3] ^= v[0]
            v[2] = (v[2] + v[1]) & MASK; v[1] = _rotl(v[1], 17); v[1] ^= v[2]; v[2] = _rotl(v[2], 32)

    full = len(msg) & ~7
    for i in range(0, full, 8):
        m = struct.unpack_from("<Q", msg, i)[0]
        v[3] ^= m; rounds(2); v[0] ^= m
    b = (len(msg) & 0xFF) << 56
    for i, c in enumerate(msg[full:]):
        b |= c << (8 * i)
    v[3] ^= b; rounds(2); v[0] ^= b
    v[2] ^= 0xFF; rounds(4)
    return v[0] ^ v[1] ^ v[2] ^ v[3]


def key_from_chip(chip_id):
    """The 24 hex digits the digipeater prints (three 32-bit words), as its key."""
    h = chip_id.strip().replace(" ", "").replace(":", "")
    if len(h) != 24:
        raise ValueError("the chip ID is 24 hex digits, as the digipeater prints it at start-up")
    w = [int(h[i:i + 8], 16) for i in (0, 8, 16)]
    return struct.pack("<III", *w) + b"LORA"


def key_from_secret(secret):
    s = secret.encode()
    return struct.pack("<QQ", siphash24(b"LoRa-digi-key-A!", s), siphash24(b"LoRa-digi-key-B!", s))


def code(key, mycall, counter, command):
    h = siphash24(key, f"{mycall.upper()}|{counter}|{command}".encode()) & ((1 << 40) - 1)
    return "".join(B32[(h >> (5 * i)) & 31] for i in range(7, -1, -1))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("digi", help="the digipeater's callsign with SSID, e.g. K6ATV-10")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--chip", help="its chip ID (24 hex digits, printed at start-up)")
    g.add_argument("--secret", help="the secret set on it with  set secret ...")
    ap.add_argument("command", nargs="+", help='e.g. "set moving on", "get move_m", save, beacon, reboot')
    ap.add_argument("--counter", type=int, help="default: the time in seconds")
    a = ap.parse_args()
    command = " ".join(" ".join(a.command).split())
    counter = a.counter if a.counter is not None else int(time.time())
    key = key_from_chip(a.chip) if a.chip else key_from_secret(a.secret)
    text = f"{counter} {command} {code(key, a.digi, counter, command)}"
    if len(text) > 67:
        sys.exit("too long for one APRS message (67 characters): shorten the command")
    print(text)


if __name__ == "__main__":
    main()
