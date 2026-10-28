#!/usr/bin/env python3
"""Decode PortCls filter descriptors (x86 layouts) from yswds.sys into text.

Prints a PCFILTER_DESCRIPTOR's pins (with KSDATARANGE_AUDIO / KSDATARANGE
contents), nodes, connections and automation tables, naming pointers from
re/symbols.csv. Structure layouts are the public WDK ones (32-bit):

  PCFILTER_DESCRIPTOR  0x30: Version, AutomationTable*, PinSize, PinCount, Pins*,
                             NodeSize, NodeCount, Nodes*, ConnectionCount,
                             Connections*, CategoryCount, Categories*
  PCPIN_DESCRIPTOR     0x40: MaxGlobal, MaxFilter, MinFilter, AutomationTable*,
                             KSPIN_DESCRIPTOR (0x30)
  KSPIN_DESCRIPTOR     0x30: InterfacesCount, Interfaces*, MediumsCount, Mediums*,
                             DataRangesCount, DataRanges**, DataFlow, Communication,
                             Category*, Name*, Reserved(8)
  PCNODE_DESCRIPTOR    0x10: Flags, AutomationTable*, Type*, Name*
  PCCONNECTION_DESCRIPTOR 0x10: FromNode, FromNodePin, ToNode, ToNodePin
  PCAUTOMATION_TABLE   0x28: PropItemSize, PropCount, Props*, MethItemSize,
                             MethCount, Methods*, EventItemSize, EventCount, Events*, Reserved
  PCPROPERTY_ITEM      0x10: Set*, Id, Flags, Handler
  PCEVENT_ITEM         0x10: Set*, Id, Flags, Handler

Usage: python3 -I tools/ks_descriptors.py ../yswds.sys re/symbols.csv <descriptor VA> [...]
"""
import csv
import struct
import sys
import uuid

BASE = 0x10000
FLOW = {1: 'IN', 2: 'OUT'}
COMM = {0: 'NONE', 1: 'SINK', 2: 'SOURCE', 3: 'BOTH', 4: 'BRIDGE'}
PROPFLAGS = {1: 'GET', 2: 'SET', 0x200: 'BASICSUPPORT', 0x100: 'DEFAULTVALUES', 0x10000000: 'SERIALIZE'}
CONN_FILTER = 0xffffffff


class Img:
    def __init__(self, path, ledger):
        self.d = open(path, 'rb').read()
        self.names = {int(r['va'], 16): r['name'] for r in csv.DictReader(open(ledger))}

    def u32(self, va, n=1):
        v = struct.unpack_from('<%dI' % n, self.d, va - BASE)
        return v if n > 1 else v[0]

    def guid(self, va):
        if not va:
            return 'NULL'
        nm = self.names.get(va)
        g = str(uuid.UUID(bytes_le=self.d[va - BASE:va - BASE + 16])).upper()
        return '%s {%s}' % (nm, g) if nm and not nm.startswith('_GUID') else '{%s}' % g

    def sym(self, va):
        return self.names.get(va, '0x%x' % va) if va else 'NULL'


def flags(v, table):
    out = [n for b, n in table.items() if v & b]
    return '|'.join(out) or hex(v)


def automation(img, va, indent):
    if not va:
        return
    psz, pc, pp, msz, mc, mp, esz, ec, ep, _ = img.u32(va, 10)
    print('%sautomation @0x%x: %d properties, %d methods, %d events' % (indent, va, pc, mc, ec))
    for i in range(pc):
        s, pid, fl, h = img.u32(pp + psz * i, 4)
        print('%s  prop %s id=%d %s -> %s' % (indent, img.guid(s), pid, flags(fl, PROPFLAGS), img.sym(h)))
    for i in range(ec):
        s, eid, fl, h = img.u32(ep + esz * i, 4)
        print('%s  event %s id=%d flags=0x%x -> %s' % (indent, img.guid(s), eid, fl, img.sym(h)))


def datarange(img, va, indent):
    fsize, fl, ssize, _ = img.u32(va, 4)
    major = img.guid(va + 0x10)
    sub = img.guid(va + 0x20)
    spec = img.guid(va + 0x30)
    line = '%srange @0x%x size=0x%x %s / %s / %s' % (indent, va, fsize, major.split(' ')[0], sub.split(' ')[0], spec.split(' ')[0])
    if fsize >= 0x54:
        mc, minb, maxb, minf, maxf = img.u32(va + 0x40, 5)
        line += ' ch<=%d bits %d-%d rate %d-%d' % (mc, minb, maxb, minf, maxf)
    print(line)


def describe(img, va):
    ver, at, psz, pc, pins, nsz, nc, nodes, cc, conns, catc, cats = img.u32(va, 12)
    print('=== filter @0x%x version %d: %d pins, %d nodes, %d connections, %d categories' % (va, ver, pc, nc, cc, catc))
    automation(img, at, '  ')
    for i in range(catc):
        print('  category', img.guid(img.u32(cats + 16 * i) if False else cats + 16 * i))
    for i in range(pc):
        p = pins + psz * i
        maxg, maxf, minf, pat = img.u32(p, 4)
        ic, ip, mc, mp, drc, drp, flow, comm, cat, name = img.u32(p + 0x10, 10)
        print('  pin %d @0x%x: inst max %d/%d min %d, flow %s, comm %s, category %s, name %s' % (
            i, p, maxg, maxf, minf, FLOW.get(flow, flow), COMM.get(comm, comm), img.guid(cat), img.guid(name)))
        automation(img, pat, '    ')
        for k in range(drc):
            datarange(img, img.u32(drp + 4 * k), '    ')
    for i in range(nc):
        n = nodes + nsz * i
        fl, nat, typ, name = img.u32(n, 4)
        print('  node %d: type %s, name %s' % (i, img.guid(typ), img.guid(name)))
        automation(img, nat, '    ')
    for i in range(cc):
        fn, fp, tn, tp = img.u32(conns + 16 * i, 4)
        f = 'FILTER' if fn == CONN_FILTER else 'node %d' % fn
        t = 'FILTER' if tn == CONN_FILTER else 'node %d' % tn
        print('  conn %s pin %d -> %s pin %d' % (f, fp, t, tp))


def main():
    img = Img(sys.argv[1], sys.argv[2])
    for a in sys.argv[3:]:
        describe(img, int(a, 16))


if __name__ == '__main__':
    main()
