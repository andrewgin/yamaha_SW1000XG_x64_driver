#!/usr/bin/env python3
"""Extract DSP tables from the user's own copy of yswds.sys.

The manifest (tools/blobs_sw1000.csv) lists names, virtual addresses, lengths
and SHA-256 only, in the order of the driver's microcode bundle (SW_BLOB in
port/inc/sw1000_blobs.h). This tool copies those byte ranges out of the binary
into an output directory OUTSIDE the repository and checks each one against its
recorded hash. It refuses to run unless the input binary's SHA-256 matches.

With --bundle FILE it also writes the driver's microcode bundle: every blob
concatenated in manifest order, no header (port/BUILD.md). On Windows the same
step is done by port/install/Install-Microcode.ps1, which also writes the
registry value the driver reads.

Usage: python3 -I tools/extract_blobs.py ../yswds.sys ../extracted
           [--manifest blobs.csv] [--bundle ../extracted/microcode.bundle]
"""
import argparse
import csv
import hashlib
import os
import sys

EXPECTED = '0e475f6af8a4e23af5d5fd3533533fc50433ae10e98aaab9d96368cda35eb4b4'
IMAGE_BASE = 0x10000


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('src')
    ap.add_argument('outdir')
    ap.add_argument('--manifest', default=os.path.join(here, 'blobs_sw1000.csv'))
    ap.add_argument('--bundle')
    args = ap.parse_args()

    data = open(args.src, 'rb').read()
    if hashlib.sha256(data).hexdigest() != EXPECTED:
        sys.exit('input is not the expected yswds.sys (SHA-256 mismatch)')
    repo = os.path.realpath(os.path.join(here, '..'))
    for path in [args.outdir] + ([args.bundle] if args.bundle else []):
        if os.path.realpath(path).startswith(repo + os.sep):
            sys.exit('refusing to write extracted Yamaha data inside the repository')
    os.makedirs(args.outdir, exist_ok=True)

    bundle = bytearray()
    for row in csv.DictReader(open(args.manifest)):
        off = int(row['va'], 16) - IMAGE_BASE  # file offset = VA - image base for every section
        blob = data[off:off + int(row['length'], 16)]
        digest = hashlib.sha256(blob).hexdigest()
        if digest != row['sha256']:
            sys.exit('%s: SHA-256 mismatch (%s)' % (row['name'], digest))
        with open(os.path.join(args.outdir, row['name'] + '.bin'), 'wb') as f:
            f.write(blob)
        bundle += blob
        print('%s,0x%x,0x%x,%s' % (row['name'], off, len(blob), digest))

    if args.bundle:
        with open(args.bundle, 'wb') as f:
            f.write(bundle)
        print('bundle: %s, 0x%x bytes, SHA-256 %s'
              % (args.bundle, len(bundle), hashlib.sha256(bundle).hexdigest()))


if __name__ == '__main__':
    main()
