#!/usr/bin/env python3
"""Check that the three copies of the microcode manifest agree.

The DSP-table manifest (name, VA, length, SHA-256, in bundle order) lives in:
  tools/blobs_sw1000.csv               - the reference (used by extract_blobs.py)
  port/src/blobs.cpp                   - g_SwBlobs: name, length, SHA-256 (driver)
  port/install/Install-Microcode.ps1   - $Manifest: name, VA, length, SHA-256

and the source binary's SHA-256 in extract_blobs.py and the PowerShell script.
Needs no Yamaha bytes, so CI runs it. Exit status 1 on any difference.

Usage: python3 -I tools/check_manifest.py
"""
import csv
import os
import re
import sys

REPO = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))


def read(rel):
    with open(os.path.join(REPO, rel), encoding='ascii') as f:
        return f.read()


def main():
    csv_rows = [(r['name'], int(r['va'], 16), int(r['length'], 16), r['sha256'])
                for r in csv.DictReader(open(os.path.join(REPO, 'tools', 'blobs_sw1000.csv')))]
    cpp_rows = [(n, int(l, 16), h) for n, l, h in re.findall(
        r'\{\s*"(\w+)",\s*(0x[0-9a-fA-F]+),\s*"([0-9a-f]{64})"\s*\}', read('port/src/blobs.cpp'))]
    ps1 = read('port/install/Install-Microcode.ps1')
    ps1_rows = [(n, int(v, 16), int(l, 16), h) for n, v, l, h in re.findall(
        r"@\('(\w+)',\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),\s*'([0-9a-f]{64})'\)", ps1)]

    errors = []
    if [(n, l, h) for n, _, l, h in csv_rows] != cpp_rows:
        errors.append('port/src/blobs.cpp g_SwBlobs differs from tools/blobs_sw1000.csv')
    if ps1_rows != csv_rows:
        errors.append('Install-Microcode.ps1 $Manifest differs from tools/blobs_sw1000.csv')
    enum_count = len(re.findall(r'^\s*SW_BLOB_\w+(?:\s*=\s*0)?,', read('port/inc/sw1000_blobs.h'), re.M))
    if enum_count != len(csv_rows):
        errors.append('SW_BLOB enum has %d entries, manifest has %d' % (enum_count, len(csv_rows)))

    py_sha = re.search(r"EXPECTED = '([0-9a-f]{64})'", read('tools/extract_blobs.py')).group(1)
    ps_sha = re.search(r"\$SourceSha256 = '([0-9a-f]{64})'", ps1).group(1)
    if py_sha != ps_sha:
        errors.append('source SHA-256 differs between extract_blobs.py and Install-Microcode.ps1')

    for e in errors:
        print('error: ' + e)
    if errors:
        sys.exit(1)
    print('manifest OK: %d tables, 0x%x bytes' % (len(csv_rows), sum(r[2] for r in csv_rows)))


if __name__ == '__main__':
    main()
