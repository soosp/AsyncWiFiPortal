#!/usr/bin/env python3
"""genpass.py — AsyncWiFiPortal MAC-derived AP password generator (deployment tool).

Reproduces, byte for byte, the firmware's derive(salt, MAC):

    password = crockford32( HMAC-SHA256(salt, mac6)[:NBYTES] )

so a printed label matches the password the device will accept. Use it to print
labels for a batch of devices before deployment.

The MAC is the factory *base* MAC (the STA MAC, the one printed on the chip) --
NOT the soft-AP BSSID, which on ESP32 is base or base+1 depending on build config
and on ESP8266 is a local-bit variant. The firmware derives from the base MAC via
esp_read_mac(ESP_MAC_WIFI_STA) / WiFi.macAddress(), so the CSV must carry base
MACs (as recorded at manufacturing), not scanned BSSIDs.

The salt is the secret. Pass it through the environment (--salt-env), not on the
command line (visible in the process list / shell history) and never in a file.

Examples:
    export WIFIPORTAL_SALT='...'
    genpass.py --salt-env WIFIPORTAL_SALT --mac AA:BB:CC:DD:EE:FF
    genpass.py --salt-env WIFIPORTAL_SALT --in macs.csv --out labels.csv

Python 3 standard library only — no pip dependencies, runs on Windows/Linux/mac.
"""

import argparse
import csv
import hashlib
import hmac
import os
import sys

# Crockford base32: uppercase, no I/L/O/U, so a label transcribes without the
# 0/O, 1/I/L ambiguities. Must match the firmware's alphabet exactly.
CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"

# 10 bytes -> 16 chars (80 bits). WPA2-PSK needs 8..63 printable ASCII.
DEFAULT_NBYTES = 10


def crockford32(data: bytes) -> str:
    """Big-endian base32 of the bytes, MSB first, left-aligned last group."""
    bits = 0
    nbits = 0
    out = []
    for b in data:
        bits = (bits << 8) | b
        nbits += 8
        while nbits >= 5:
            nbits -= 5
            out.append(CROCKFORD[(bits >> nbits) & 0x1F])
    if nbits:                                   # leftover bits, left-aligned
        out.append(CROCKFORD[(bits << (5 - nbits)) & 0x1F])
    return "".join(out)


def norm_mac(s: str) -> bytes:
    """A MAC in any separator style -> 6 raw bytes. 'AA:BB..' == 'aa-bb..'."""
    hexd = "".join(c for c in s.lower() if c in "0123456789abcdef")
    if len(hexd) != 12:
        raise ValueError("not a 48-bit MAC: %r" % s)
    return bytes.fromhex(hexd)


def derive(salt: bytes, mac_str: str, nbytes: int = DEFAULT_NBYTES) -> str:
    """The AP password for a device, from the salt and its MAC."""
    digest = hmac.new(salt, norm_mac(mac_str), hashlib.sha256).digest()
    return crockford32(digest[:nbytes])


def read_macs(path):
    """Yield the MAC from the first column of each non-empty CSV row."""
    with open(path, newline="") as f:
        for row in csv.reader(f):
            if row and row[0].strip():
                yield row[0].strip()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--mac", help="a single MAC address")
    src.add_argument("--in", dest="infile",
                     help="CSV whose first column is the MAC")
    salt = ap.add_mutually_exclusive_group(required=True)
    salt.add_argument("--salt-env", metavar="VAR",
                      help="environment variable holding the salt (preferred)")
    salt.add_argument("--salt",
                      help="the salt directly (unsafe: visible in the process "
                           "list and shell history)")
    ap.add_argument("--out", help="write CSV here instead of stdout")
    ap.add_argument("--ssid", help="also emit this AP SSID column")
    ap.add_argument("--bytes", type=int, default=DEFAULT_NBYTES,
                    help="hash bytes to keep (default %d, -> %d chars)"
                         % (DEFAULT_NBYTES, DEFAULT_NBYTES * 8 // 5))
    args = ap.parse_args(argv)

    if args.salt_env:
        raw = os.environ.get(args.salt_env)
        if raw is None:
            ap.error("environment variable %r is not set" % args.salt_env)
    else:
        raw = args.salt
        sys.stderr.write("warning: --salt is visible in the process list; "
                         "prefer --salt-env\n")
    salt_bytes = raw.encode("utf-8")

    macs = [args.mac] if args.mac else list(read_macs(args.infile))

    out = open(args.out, "w", newline="") if args.out else sys.stdout
    try:
        w = csv.writer(out)
        header = ["mac", "password"] + (["ssid"] if args.ssid else [])
        w.writerow(header)
        for m in macs:
            try:
                pw = derive(salt_bytes, m, args.bytes)
            except ValueError as e:
                sys.stderr.write("skipped: %s\n" % e)
                continue
            w.writerow([m, pw] + ([args.ssid] if args.ssid else []))
    finally:
        if out is not sys.stdout:
            out.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
