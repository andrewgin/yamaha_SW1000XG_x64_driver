#!/usr/bin/env python3
"""Extract the CodeView (NB11) public symbols and module map embedded in yswds.sys.

yswds.sys carries an IMAGE_DEBUG_TYPE_CODEVIEW record (NB11 signature) appended
after its last section. It holds sstGlobalPub (public symbols with MSVC-mangled
names), sstModule (per-object-file section contributions) and sstLibraries.
This tool reads those structures and prints facts about them; it never copies
code or data bytes from the image, except the text of short string literals
named by public symbols.

Usage:
    python3 -I tools/cv_publics.py ../yswds.sys symbols   > generated.csv
    python3 -I tools/cv_publics.py ../yswds.sys modules   > modules.txt
    python3 -I tools/cv_publics.py ../yswds.sys ghidra    > names.json

`symbols` emits rows in the re/symbols.csv format
(va,name,kind,confidence,source,notes). `ghidra` emits a {va: name} JSON map
for REA's set_addresses_names.
"""
import csv
import json
import struct
import sys

IMAGE_BASE = 0x10000

SST_MODULE = 0x120
SST_LIBRARIES = 0x128
SST_GLOBALPUB = 0x12A
S_PUB32 = 0x1009  # CV4.1 32-bit-type-index public: typind, off, seg, name


class Image:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        d = self.d
        pe = struct.unpack_from('<I', d, 0x3C)[0]
        nsec, = struct.unpack_from('<H', d, pe + 6)
        optsz, = struct.unpack_from('<H', d, pe + 20)
        opt = pe + 24
        self.sections = []  # (name, va, vsize, rawoff)
        sh = opt + optsz
        for i in range(nsec):
            name = d[sh:sh + 8].rstrip(b'\0').decode()
            vsize, rva, _rawsz, rawoff = struct.unpack_from('<IIII', d, sh + 8)
            self.sections.append((name, IMAGE_BASE + rva, vsize, rawoff))
            sh += 40
        dbg_rva, dbg_size = struct.unpack_from('<II', d, opt + 96 + 8 * 6)
        self.cv_base = None
        for i in range(dbg_size // 28):
            _c, _t, _ma, _mi, typ, size, _rva, ptr = struct.unpack_from(
                '<IIHHIIII', d, dbg_rva + 28 * i)
            if typ == 2 and d[ptr:ptr + 4] == b'NB11':
                self.cv_base = ptr
        if self.cv_base is None:
            sys.exit('no NB11 CodeView record found')

    def va_to_off(self, va):
        for _n, sva, vsize, rawoff in self.sections:
            if sva <= va < sva + vsize:
                return va - sva + rawoff
        return None

    def section_of(self, va):
        for n, sva, vsize, _r in self.sections:
            if sva <= va < sva + vsize:
                return n
        return '?'

    def pstr(self, o):
        n = self.d[o]
        return self.d[o + 1:o + 1 + n].decode('latin1')

    def subsections(self):
        d, base = self.d, self.cv_base
        dirofs, = struct.unpack_from('<I', d, base + 4)
        p = base + dirofs
        cb_hdr, cb_ent, count = struct.unpack_from('<HHI', d, p)
        for i in range(count):
            yield struct.unpack_from('<HHII', d, p + cb_hdr + i * cb_ent)

    def seg_va(self, seg, off):
        if 0 < seg <= len(self.sections):
            return self.sections[seg - 1][1] + off
        return None

    def modules(self):
        out = []
        for sst, _imod, lfo, _cb in self.subsections():
            if sst != SST_MODULE:
                continue
            o = self.cv_base + lfo
            _ovl, _ilib, cseg, _style = struct.unpack_from('<HHH2s', self.d, o)
            name = self.pstr(o + 8 + 12 * cseg)
            for k in range(cseg):
                seg, _pad, off, cb = struct.unpack_from('<HHII', self.d, o + 8 + 12 * k)
                va = self.seg_va(seg, off)
                if va is not None:
                    out.append((va, cb, self.section_of(va), name.split('\\')[-1]))
        return sorted(out)

    def publics(self):
        for sst, _imod, lfo, cb in self.subsections():
            if sst != SST_GLOBALPUB:
                continue
            o = self.cv_base + lfo
            cb_sym, = struct.unpack_from('<I', self.d, o + 4)
            q, end = o + 16, o + 16 + cb_sym
            while q < end:
                ln, ty = struct.unpack_from('<HH', self.d, q)
                if ty == S_PUB32:
                    _ti, off, seg = struct.unpack_from('<IIH', self.d, q + 4)
                    va = self.seg_va(seg, off)
                    if va is not None:
                        yield va, self.pstr(q + 14)
                q += ln + 2


# ---- minimal MSVC demangling: enough for qualified names and labels ----

def _num(s, i):
    """MSVC encoded number: '0'-'9' => 1..10, else hex digits A-P ending in '@'."""
    if s[i].isdigit():
        return int(s[i]) + 1, i + 1
    j = s.index('@', i)
    return int(''.join('%x' % (ord(c) - ord('A')) for c in s[i:j]) or '0', 16), j + 1


_TARG = {'E': 'uchar', 'D': 'char', 'F': 'short', 'G': 'ushort', 'H': 'int',
         'I': 'uint', 'J': 'long', 'K': 'ulong'}


def _template(s, i):
    """Parse ?$Name@args@ starting at i (pointing at '?$'); return (text, i)."""
    j = s.index('@', i + 2)
    name, i = s[i + 2:j], j + 1
    args = []
    while s[i] != '@':
        if s.startswith('$0', i):
            v, i = _num(s, i + 2)
            args.append('0x%x' % v)
        elif s[i] in _TARG:
            args.append(_TARG[s[i]])
            i += 1
        else:
            return name + '<?>', s.index('@@', i) + 1
    return '%s<%s>' % (name, ','.join(args)), i + 1


def _scopes(s, i):
    names = []
    while i < len(s) and s[i] != '@':
        if s.startswith('?$', i):
            t, i = _template(s, i)
            names.append(t)
        elif s[i] == '?' and s[i + 1].isdigit():  # function-local static: ?N??Func@Scope@@sig@
            # The enclosing function's scope follows; the signature is skipped.
            fn_end = s.index('@', i + 4)
            outer, _ = _scopes(s, fn_end + 1)
            return outer + [s[i + 4:fn_end]] + list(reversed(names)), len(s)
        else:
            j = s.index('@', i)
            names.append(s[i:j])
            i = j + 1
    return list(reversed(names)), i + 1


SPECIAL = {'0': None, '1': None, '2': 'operator_new', '3': 'operator_delete',
           '_7': 'vftable', '_G': 'scalar_deleting_dtor',
           '_E': 'vector_deleting_dtor'}


def demangle(m):
    """Return (qualified_name, info dict) for an MSVC-mangled or C name."""
    info = {}
    if not m.startswith('?'):
        n = m
        if n.startswith('_'):
            n = n[1:]
        if '@' in n and n.rsplit('@', 1)[1].isdigit():
            n = n.rsplit('@', 1)[0]
        return n, info
    if m.startswith('??_C@'):
        info['string'] = 'wide' if m.startswith('??_C@_1') else 'ascii'
        return None, info
    if m.startswith('??'):
        for k in sorted(SPECIAL, key=len, reverse=True):
            if m.startswith(k, 2):
                sc, i = _scopes(m, 2 + len(k))
                cls = sc[-1].split('<')[0]
                nm = {'0': cls, '1': '~' + cls}.get(k, SPECIAL[k])
                if k == '_7' and m[i:i + 2] == '6B' and m[i + 2] != '@':
                    base, _ = _scopes(m, i + 2)
                    nm = 'vftable_for_' + '::'.join(base)
                return '::'.join(sc + [nm]), info
        return m, info
    if m.startswith('?$'):
        base, i = _template(m, 1)
    else:
        j = m.index('@', 1)
        base, i = m[1:j], j + 1
    sc, i = _scopes(m, i)
    code = m[i] if i < len(m) else ''
    if code in '23':
        info['data'] = True
    elif code in 'GHOPWX':  # virtual adjustor thunk
        adj, _ = _num(m, i + 1)
        info['thunk'] = adj
    return '::'.join(sc + [base]), info


def read_literal(img, va, kind):
    off = img.va_to_off(va)
    if off is None:
        return ''
    if kind == 'wide':
        end = off
        while img.d[end:end + 2] != b'\0\0':
            end += 2
        return img.d[off:end].decode('utf-16-le', 'replace')
    end = img.d.index(b'\0', off)
    return img.d[off:end].decode('latin1')


def classify(img, va, mangled, qname, info, code_ranges):
    sec = img.section_of(va)
    if info.get('string'):
        return 'string'
    if qname.endswith('vftable') or '::vftable_for_' in qname:
        return 'vtable'
    if mangled.startswith(('_IID_', '_CLSID_', '_KSPROPSETID_', '__GUID_')):
        return 'guid'
    in_code = any(lo <= va < hi for lo, hi in code_ranges)
    if info.get('data') or not in_code:
        if '@@3' in mangled and ('QAY' in mangled or 'QB' in mangled or 'PA' in mangled) \
                or not mangled.startswith('?'):
            return 'table'
        return 'global'
    return 'func'


SKIP_PREFIXES = ('__imp_', '__IMPORT_DESCRIPTOR', '__NULL_IMPORT', '\x7f')


def build(img):
    mods = img.modules()

    def module_of(va):
        for mva, cb, _s, name in mods:
            if mva <= va < mva + cb:
                return name
        return ''

    # Code ranges: PAGE/INIT and .text from the first function contribution on.
    # .text below that point is the IAT plus const data (vtables, GUIDs, tables).
    code_ranges = []
    for name, sva, vsize, _r in img.sections:
        if name in ('PAGE', 'INIT'):
            code_ranges.append((sva, sva + vsize))
    text_code = min(va for va, m in img.publics() if m == '__purecall')
    text = [s for s in img.sections if s[0] == '.text'][0]
    code_ranges.append((text_code, text[1] + text[2]))

    rows, seen = {}, {}
    for va, mangled in sorted(set(img.publics())):
        if mangled.startswith(SKIP_PREFIXES) or mangled.endswith('_NULL_THUNK_DATA'):
            continue
        qname, info = demangle(mangled)
        kind = classify(img, va, mangled, qname or '', info, code_ranges)
        notes = []
        if kind == 'string':
            text_ = read_literal(img, va, info['string'])
            qname = 'str_' + ''.join(c if c.isalnum() else '_' for c in text_)[:40]
            notes.append('%s literal "%s"' % (info['string'], text_))
        if 'thunk' in info:
            qname += '_thunk_0x%x' % info['thunk']
            notes.append('virtual adjustor thunk (this -= 0x%x)' % info['thunk'])
        if va in rows:  # alias at same address (scalar/vector deleting dtor)
            rows[va]['notes'] += '; alias %s' % mangled
            continue
        if qname in seen:
            qname += '_%x' % va
        seen[qname] = va
        mod = module_of(va)
        if mod.startswith(('dsp0', 'dsp1', 'dsp2', 'dsp3', 'dsp4', 'dspAsio')) and kind == 'table':
            kind = 'blob'
        if mod:
            notes.append(mod)
        notes.append(mangled)
        rows[va] = dict(va='0x%x' % va, name=qname, kind=kind, confidence='confirmed',
                        source='codeview', notes='; '.join(notes))
    return [rows[k] for k in sorted(rows)]


def main():
    img = Image(sys.argv[1])
    mode = sys.argv[2] if len(sys.argv) > 2 else 'symbols'
    if mode == 'modules':
        for va, cb, sec, name in img.modules():
            print('0x%x,0x%x,%s,%s' % (va, cb, sec, name))
    elif mode == 'ghidra':
        print(json.dumps({r['va']: r['name'] for r in build(img)
                          if r['kind'] != 'string'}, indent=0))
    else:
        w = csv.DictWriter(sys.stdout, ['va', 'name', 'kind', 'confidence', 'source', 'notes'],
                           lineterminator='\n')
        for r in build(img):
            w.writerow(r)


if __name__ == '__main__':
    main()
