// apply_packed.c - apply Android APS2 packed relocations (DT_ANDROID_RELA)
// to an already-loaded ELF at `base`. Parses .dynsym/.dynstr from memory.
// Returns number of relocations applied; unresolved externals are skipped.
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
#include <dlfcn.h>

#ifndef DT_ANDROID_RELA
#define DT_ANDROID_RELA 0x60000011
#endif
#define R_AARCH64_ABS64 257
#define R_AARCH64_GLOB_DAT 1025
#define R_AARCH64_JUMP_SLOT 1026
#define APS2_OFF 0x1b8a8UL
#define APS2_SZ  0x1e5cUL
#define DYNSYM_OFF 0x2a8UL
#define DYNSYM_SZ  0x9f30UL
#define DYNSTR_OFF 0xde2cUL

static int64_t rd_sleb(const uint8_t **pp) {
    int64_t result = 0; unsigned shift = 0;
    const uint8_t *p = *pp;
    for (;;) {
        uint8_t b = *p++;
        result |= (int64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) {
            if (b & 0x40) result -= (int64_t)1 << shift;
            break;
        }
    }
    *pp = p;
    return result;
}

int apply_packed_relocs(uintptr_t base) {
    const uint8_t *data = (const uint8_t *)base;
    const uint8_t *p = data + APS2_OFF;
    if (memcmp(p, "APS2", 4)) return -1;
    p += 4;
    long remaining = (long)rd_sleb(&p);
    uint64_t offset = (uint64_t)rd_sleb(&p);
    int64_t addend = 0;
    int applied = 0, skipped = 0;

    /* page-walk writer with mprotect */
    uintptr_t lo = (uintptr_t)-1, hi = 0;
    #define NOTE_PAGE(a) do { if ((a) < lo) lo = (a); if ((a) + 8 > hi) hi = (a) + 8; } while (0)

    /* first pass: apply; assume .got/.data.rel.ro pages need mprotect */
    uintptr_t pg_lo = APS2_OFF < DYNSYM_OFF ? base : base; /* full span below */
    (void)pg_lo;
    uintptr_t start = base + 0x235000UL, end = base + 0x23c000UL; /* .data.rel.ro + .got */
    if (mprotect((void *)start, end - start, PROT_READ | PROT_WRITE)) {
        perror("mprotect");
        return -2;
    }

    while (remaining > 0) {
        long gsize = (long)rd_sleb(&p);
        int64_t gflags = rd_sleb(&p);
        int by_info = gflags & 1, by_delta = gflags & 2;
        int by_addend = gflags & 4, has_addend = gflags & 8;
        int64_t godelta = 0, ginfo = 0;
        if (by_delta) godelta = rd_sleb(&p);
        if (by_info) ginfo = rd_sleb(&p);
        if (has_addend && by_addend) addend += rd_sleb(&p);
        else if (!has_addend) addend = 0;
        for (long i = 0; i < gsize; i++) {
            if (by_delta) offset += (uint64_t)godelta;
            else offset += (uint64_t)rd_sleb(&p);
            uint64_t info = by_info ? (uint64_t)ginfo : (uint64_t)rd_sleb(&p);
            if (has_addend && !by_addend) addend += rd_sleb(&p);
            uint32_t type = (uint32_t)(info & 0xffffffff);
            uint32_t symi = (uint32_t)(info >> 32);
            if (type != R_AARCH64_ABS64 && type != R_AARCH64_GLOB_DAT && type != R_AARCH64_JUMP_SLOT) continue;
            uintptr_t *loc = (uintptr_t *)(base + offset);
            NOTE_PAGE((uintptr_t)loc);
            if (symi == 0) { *loc = (uintptr_t)(base + (uint64_t)addend); applied++; continue; }
            const uint8_t *sy = data + DYNSYM_OFF + (uint64_t)symi * 24;
            uint64_t st_value; uint16_t st_shndx; uint32_t st_name;
            memcpy(&st_name, sy, 4);
            memcpy(&st_shndx, sy + 6, 2);
            memcpy(&st_value, sy + 8, 8);
            const char *name = (const char *)(data + DYNSTR_OFF + st_name);
            if (st_shndx != 0) {
                *loc = (uintptr_t)(base + st_value + (uint64_t)addend);
                applied++;
            } else {
                void *s = dlsym(RTLD_DEFAULT, name);
                if (s) { *loc = (uintptr_t)s; applied++; }
                else skipped++;
            }
        }
        remaining -= gsize;
    }
    /* restore read-only (RELRO region) */
    mprotect((void *)start, end - start, PROT_READ);
    return applied;
}
