#!/usr/bin/env python3
"""Strip ELF symbol-versioning requirements (Android verneed) so the libs
load against glibc. Zeros .gnu.version entries and disables DT_VERNEED."""
import struct, sys

def deversion(path, no_init=False):
    d = bytearray(open(path,'rb').read())
    e_shoff, = struct.unpack_from('<Q', d, 0x28)
    e_shentsize, e_shnum = struct.unpack_from('<HH', d, 0x3a)
    shs = []
    for i in range(e_shnum):
        off = e_shoff + i*e_shentsize
        shs.append(struct.unpack_from('<IIQQQQIIQQ', d, off))
    dyn = None
    for s in shs:
        if s[1] == 6: dyn = s  # SHT_DYNAMIC
    assert dyn, 'no .dynamic'
    off, size = dyn[4], dyn[5]
    tags = {}
    for i in range(size//16):
        tag, val = struct.unpack_from('<QQ', d, off+i*16)
        tags.setdefault(tag, []).append(i)
    # 1) zero all .gnu.version entries
    for s in shs:
        if s[0] == 7 and dyn and s[4] < len(d):  # SHT_DYNSYM handled below; versym by name instead
            pass
    # find versym by linked section name ".gnu.version"
    for i, s in enumerate(shs):
        if s[1] == 18:  # SHT_SYMTAB_SHNDX? no; we resolve by name via shstrtab
            pass
    # simpler: locate ".dynsym" then ".gnu.version" follows by link/name lookup
    e_shstrndx, = struct.unpack_from('<H', d, 0x3e)
    shstr = shs[e_shstrndx]
    def sname(n):
        s = shstr[4] + n
        return bytes(d[s:d.index(b'\x00', s)]).decode()
    for s in shs:
        if sname(s[0]) == '.gnu.version':
            voff, vsz = s[4], s[5]
            for i in range(vsz//2):
                struct.pack_into('<H', d, voff+i*2, 0)
    # 2) retag DT_VERNEED / DT_VERSYM / DT_VERNEEDNUM to DT_DEBUG (21) so the
    #    loader treats the library as completely unversioned.
    n = 0
    for tag in (0x6ffffffe, 0x6fffffff, 0x6ffffff0):
        for i in tags.get(tag, []):
            struct.pack_into('<Q', d, off+i*16, 21)
            struct.pack_into('<Q', d, off+i*16+8, 0)
            n += 1
    if no_init:
        for i in tags.get(27, []):        # DT_INIT_ARRAYSZ -> skip static ctors
            struct.pack_into('<Q', d, off+i*16+8, 0)
    open(path,'wb').write(d)
    return n

no_init = '--no-init' in sys.argv
for p in [a for a in sys.argv[1:] if not a.startswith('--')]:
    print(p, 'verneed entries disabled:', deversion(p, no_init))
