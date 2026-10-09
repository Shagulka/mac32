#!/usr/bin/env python3
"""List the legacy (i386) ObjC classes, categories, selectors and class refs in Mach-O files."""
import struct, sys

def load(path):
    d = open(path, 'rb').read()
    magic, cpu, sub, ftype, ncmds, sz, flags = struct.unpack_from('<7I', d, 0)
    assert magic == 0xfeedface, path
    off, segs, sects = 28, [], {}
    for _ in range(ncmds):
        cmd, csz = struct.unpack_from('<2I', d, off)
        if cmd == 1:
            segname = d[off+8:off+24].rstrip(b'\0').decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<4I', d, off+24)
            nsects = struct.unpack_from('<I', d, off+48)[0]
            segs.append((vmaddr, vmsize, fileoff, filesize))
            so = off + 56
            for _ in range(nsects):
                sn = d[so:so+16].rstrip(b'\0').decode(); sg = d[so+16:so+32].rstrip(b'\0').decode()
                addr, size, fo = struct.unpack_from('<3I', d, so+32)
                sects[(sg, sn)] = (addr, size, fo)
                so += 68
        off += csz
    def rd(addr, n):
        for va, vs, fo, fs in segs:
            if va <= addr < va + vs:
                o = addr - va + fo
                return d[o:o+n]
        return b''
    def cstr(addr):
        for va, vs, fo, fs in segs:
            if va <= addr < va + vs:
                o = addr - va + fo
                return d[o:d.index(b'\0', o)].decode('latin-1')
        return '?'
    return d, sects, rd, cstr

sels, clsrefs = set(), set()
for path in sys.argv[1:]:
    d, sects, rd, cstr = load(path)
    name = path.split('/')[-1]
    for key, acc in ((('__OBJC', '__message_refs'), sels), (('__OBJC', '__cls_refs'), clsrefs)):
        if key in sects:
            a, s, fo = sects[key]
            for (p,) in struct.iter_unpack('<I', d[fo:fo+s]):
                acc.add(cstr(p))
    if ('__OBJC', '__class') in sects:
        a, s, fo = sects[('__OBJC', '__class')]
        for i in range(0, s, 48):
            isa, sup, nm, ver, info, isz, ivars, mlists = struct.unpack_from('<8I', d, fo + i)
            print(f'{name}: class {cstr(nm)} : {cstr(sup)} (instance {isz} bytes)')
    if ('__OBJC', '__category') in sects:
        a, s, fo = sects[('__OBJC', '__category')]
        for i in range(0, s, 28):
            cn, cl = struct.unpack_from('<2I', d, fo + i)
            print(f'{name}: category {cstr(cn)} on {cstr(cl)}')
print(f'\n{len(clsrefs)} class refs:', ' '.join(sorted(clsrefs)))
print(f'\n{len(sels)} selectors:', ' '.join(sorted(sels)))
