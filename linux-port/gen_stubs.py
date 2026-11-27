#!/usr/bin/env python3
"""Generate stub C source satisfying all unresolved imports of the Dolby
Android libraries not provided by the Dolby libs themselves, glibc or libc++."""
import re, subprocess, sys, os

BIN = sys.argv[1] if len(sys.argv) > 1 else '.'
LIBS = ['libswdap.so','libdapparamstorage.so','libdlbpreg.so',
        'libspatializerparamstorage.so']

def symbols(path):
    out = subprocess.check_output(['readelf','--dyn-syms','-W',path], text=True)
    for line in out.splitlines():
        f = line.split()
        if len(f) < 8 or ':' not in f[0]: continue
        yield f[6], f[7].split('@')[0], f[3], f[2]

und, defined = {}, set()
for lib in LIBS:
    for ndx, name, typ, size in symbols(os.path.join(BIN,lib)):
        if not name: continue
        if ndx == 'UND':
            old = und.get(name)
            if old is None or (old[0]=='NOTYPE' and typ != 'NOTYPE'):
                und[name] = (typ, size)
        else:
            defined.add(name)

skip = set()
for root, sos in [('/tmp/crossroot/usr/aarch64-linux-gnu/lib',
                   ['libc.so.6','libm.so.6','libdl.so.2','libpthread.so.0','librt.so.1']),
                  ('/tmp/crossroot-arm64/usr/lib/aarch64-linux-gnu',
                   ['libc++.so.1','libc++abi.so.1','libunwind.so.8'])]:
    for so in sos:
        p = os.path.join(root, so)
        if os.path.exists(p):
            for ndx, name, typ, size in symbols(p):
                if name: skip.add(name)
skip |= {'_Unwind_Resume','_Unwind_Complete','__gnu_unwind_frame'}

todo = {n:t for n,t in und.items() if n not in defined and n not in skip and not n.startswith('_ITM')}
funcs = sorted(n for n,(t,s) in todo.items() if t != 'OBJECT')
objs  = sorted(n for n,(t,s) in todo.items() if t == 'OBJECT')
print(f'{len(funcs)} funcs, {len(objs)} objects', file=sys.stderr)

PRELUDE = r'''
/* ---- functional implementations ---- */
#include <stdarg.h>

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[dolby:%s] ", tag ? tag : "?");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    return 1;
}
void __android_log_assert(const char *cond, const char *tag, const char *fmt, ...) {
    if (!cond) {
        va_list ap; va_start(ap, fmt);
        fprintf(stderr, "[dolby-assert:%s] ", tag ? tag : "?");
        if (fmt) vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n"); va_end(ap);
        abort();
    }
}
int property_get(const char *key, char *value, const char *default_value) {
    char env[512];
    snprintf(env, sizeof(env), "PROP_%s", key);
    const char *v = getenv(env);
    if (!v) v = default_value ? default_value : "";
    size_t n = strlen(v);
    memcpy(value, v, n + 1);
    return (int)n;
}
int property_get_int32(const char *key, int32_t def) {
    char env[512];
    snprintf(env, sizeof(env), "PROP_%s", key);
    const char *v = getenv(env);
    if (!v || !*v) return def;
    return (int32_t)strtol(v, NULL, 0);
}
int property_set(const char *key, const char *value) { (void)key; (void)value; return 0; }
void atrace_begin_body(const char *n) { (void)n; }
void atrace_end_body(void) {}
uint64_t atrace_get_enabled_tags(void) { return 0; }
void *memcpy_by_channel_mask(void *dst, uint32_t dst_mask, const void *src,
        uint32_t src_mask, size_t sample_size, size_t count) {
    (void)dst_mask; (void)src_mask;
    if (dst != src) memcpy(dst, src, sample_size * count);
    return dst;
}
size_t strlcpy(char *d, const char *s, size_t n) {
    size_t l = strlen(s);
    if (n) { size_t c = l < n-1 ? l : n-1; memcpy(d, s, c); d[c] = 0; }
    return l;
}
size_t strlcat(char *d, const char *s, size_t n) {
    size_t dl = strnlen(d, n);
    if (dl < n - 1) { size_t r = strlcpy(d+dl, s, n-dl); return dl + r; }
    return dl + strlen(s);
}
/* IDms::tryGetService returns android::sp<IDms> via the x8 sret slot
   (16-byte struct in x0/x1 would be the wrong ABI here); write a null sp. */
__asm__(".text\n"
".global _ZN6vendor5dolby8hardware3dms4V2_04IDms13tryGetServiceERKNSt3__112basic_stringIcNS5_11char_traitsIcEENS5_9allocatorIcEEEEb\n"
"_ZN6vendor5dolby8hardware3dms4V2_04IDms13tryGetServiceERKNSt3__112basic_stringIcNS5_11char_traitsIcEENS5_9allocatorIcEEEEb:\n"
"    stp xzr, xzr, [x8]\n"
"    ret\n");
/* ---- AOSP String8 -------------------------------------------------------
   Minimal working String8 on top of a single heap char*: the only layout
   guarantee needed is that +0 holds the pointer. */
static char *s8_dup(const char *s) { return strdup(s ? s : ""); }
void *_ZN7android7String8C1EPKc(void *self, const char *s) { *(char **)self = s8_dup(s); return self; }
void *_ZN7android7String8C1Ev(void *self) { *(char **)self = s8_dup(""); return self; }
void *_ZN7android7String8C1ERKS0_(void *self, void *o) { *(char **)self = s8_dup(o ? *(char **)o : ""); return self; }
void *_ZN7android7String85setToEPKc(void *self, const char *s) { free(*(char **)self); *(char **)self = s8_dup(s); return self; }
void *_ZN7android7String85setToERKS0_(void *self, void *o) { return _ZN7android7String85setToEPKc(self, o ? *(char **)o : ""); }
void *_ZN7android7String86appendEPKc(void *self, const char *s) {
    char *old = *(char **)self, *n = malloc(strlen(old) + strlen(s ? s : "") + 1);
    strcpy(n, old); strcat(n, s ? s : ""); free(old); *(char **)self = n; return self;
}
void *_ZN7android7String86appendERKS0_(void *self, void *o) { return _ZN7android7String86appendEPKc(self, o ? *(char **)o : ""); }
/* String8::format is STATIC and returns String8 by value: caller passes the
   return slot in x8, the format string in x0 and varargs from x1. */
__attribute__((used)) static char **g_s8_sret;
__asm__(".text\n"
".align 2\n"
".globl _ZN7android7String86formatEPKcz\n"
"_ZN7android7String86formatEPKcz:\n"
"    adrp x9, g_s8_sret\n"
"    str  x8, [x9, :lo12:g_s8_sret]\n"
"    b    s8_format_impl\n");
__attribute__((used)) static void s8_format_impl(const char *fmt, ...) {
    char buf[512]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    *g_s8_sret = s8_dup(buf);
}
void *_ZN7android7String812appendFormatEPKcz(void *self, const char *fmt, ...) {
    char buf[512]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    return _ZN7android7String86appendEPKc(self, buf);
}
void _ZN7android7String8D1Ev(void *self) { free(*(char **)self); *(char **)self = (char *)0; }
int _ZNK7android7String86lengthEv(void *self) { return (int)strlen(*(char **)self); }
unsigned long _ZNK7android8hardware11hidl_string4sizeEv(void *self) { return self ? ((unsigned long *)self)[1] : 0; }
const char *_ZNK7android8hardware11hidl_string5c_strEv(void *self) { return self ? *(const char **)self : 0; }
/* ---- AOSP VectorImpl / SortedVectorImpl emulation -----------------------
   libswdap imports the base-class methods from libutils; the per-type
   do_* methods and vtables live in libswdap and the object's vptr is set
   by libswdap right after the C2 call.  Object layout (LP64):
     +0 vptr, +8 mStorage, +16 mCount, +24 mFlags(u32), +32 mItemSize
   vtable slots relative to vptr:
     +0 D1, +8 D0, +0x10 do_construct, +0x18 do_destroy, +0x20 do_copy,
     +0x28 do_splat, +0x30 do_move_forward, +0x38 do_move_backward,
     +0x40 do_compare                                                        */
struct SVImpl { void **vptr; void *mStorage; size_t mCount; unsigned mFlags; size_t mItemSize; };
struct SVHdr  { size_t cap; size_t pad; };
#define SVT(self, off) (((void **)(*(void ***)(self)))[(off) / 8])
typedef void (*do_fn2)(void *, void *, size_t);
typedef void (*do_fn3)(void *, void *, const void *, size_t);
typedef int  (*do_cmp)(void *, const void *, const void *);
static int sv_trace(void) { static int t = -1; if (t < 0) t = getenv("PROP_SVTRACE") ? 1 : 0; return t; }
static void sv_init(void *self, size_t itemSize, unsigned flags) {
    struct SVImpl *v = self;
    v->mStorage = 0; v->mCount = 0; v->mFlags = flags; v->mItemSize = itemSize;
    if (sv_trace()) fprintf(stderr, "[sv] C2 self=%p vp=%p is=%zu fl=%zu\\n", self, (void *)v->vptr, itemSize, flags);
}
static void sv_release(void *self) {
    struct SVImpl *v = self;
    if (v->mStorage) {
        if (v->mCount) ((do_fn2)SVT(self, 0x18))(self, v->mStorage, v->mCount);
        free(((struct SVHdr *)v->mStorage) - 1);
        v->mStorage = 0; v->mCount = 0;
    }
}
static void sv_ensure(void *self, size_t need) {
    struct SVImpl *v = self;
    struct SVHdr *h = v->mStorage ? ((struct SVHdr *)v->mStorage) - 1 : 0;
    if (h && need <= h->cap) return;
    size_t cap = h ? h->cap * 2 : 4;
    if (cap < need) cap = need;
    h = (struct SVHdr *)realloc(h, sizeof(*h) + cap * v->mItemSize);
    v->mStorage = h + 1; h->cap = cap; h->pad = 0;
}
void _ZN7android10VectorImplC2Emj(void *self, size_t itemSize, unsigned flags) { sv_init(self, itemSize, flags); }
void _ZN7android16SortedVectorImplC2Emj(void *self, size_t itemSize, unsigned flags) { sv_init(self, itemSize, flags); }
void _ZN7android10VectorImpl13finish_vectorEv(void *self) { sv_release(self); }
void _ZN7android10VectorImplD2Ev(void *self) { sv_release(self); }
void _ZN7android16SortedVectorImplD2Ev(void *self) { sv_release(self); }
void _ZN7android10VectorImpl5clearEv(void *self) {
    struct SVImpl *v = self;
    if (v->mStorage && v->mCount) ((do_fn2)SVT(self, 0x18))(self, v->mStorage, v->mCount);
    v->mCount = 0;
}
void _ZN7android10VectorImpl11appendArrayEPKvm(void *self, const void *items, size_t n) {
    struct SVImpl *v = self;
    if (!n) return;
    sv_ensure(self, v->mCount + n);
    char *base = v->mStorage; size_t is = v->mItemSize;
    for (size_t i = 0; i < n; i++) {
        void *slot = base + (v->mCount + i) * is;
        ((do_fn2)SVT(self, 0x10))(self, slot, 1);
        ((do_fn3)SVT(self, 0x20))(self, slot, (const char *)items + i * is, 1);
    }
    v->mCount += n;
}
void _ZN7android16SortedVectorImpl3addEPKv(void *self, const void *item) {
    struct SVImpl *v = self;
    size_t is = v->mItemSize, lo = 0, hi = v->mCount;
    do_cmp cmp = (do_cmp)SVT(self, 0x40);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const char *elem = (const char *)v->mStorage + mid * is;
        if (cmp(self, elem, item) < 0) lo = mid + 1; else hi = mid;
    }
    sv_ensure(self, v->mCount + 1);
    char *slot = (char *)v->mStorage + lo * is;
    if (lo < v->mCount)
        ((do_fn3)SVT(self, 0x30))(self, slot + is, slot, v->mCount - lo);
    ((do_fn2)SVT(self, 0x10))(self, slot, 1);
    ((do_fn3)SVT(self, 0x20))(self, slot, item, 1);
    v->mCount++;
}
long _ZNK7android16SortedVectorImpl7indexOfEPKv(void *self, const void *item) {
    struct SVImpl *v = self;
    if (!v->mCount || !v->mStorage) return -1;
    size_t is = v->mItemSize, lo = 0, hi = v->mCount;
    do_cmp cmp = (do_cmp)SVT(self, 0x40);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const char *elem = (const char *)v->mStorage + mid * is;
        int c = cmp(self, elem, item);
        if (c < 0) lo = mid + 1;
        else if (c > 0) hi = mid;
        else return (long)mid;
    }
    return -1;
}
'''
HAND = {'__android_log_print','__android_log_assert','property_get','property_get_int32',
        'property_set','atrace_begin_body','atrace_end_body','atrace_get_enabled_tags',
        'memcpy_by_channel_mask','strlcpy','strlcat',
        '_ZN6vendor5dolby8hardware3dms4V2_04IDms13tryGetServiceERKNSt3__112basic_stringIcNS5_11char_traitsIcEENS5_9allocatorIcEEEEb',
        '_ZN7android7String8C1EPKc','_ZN7android7String8C1Ev','_ZN7android7String8C1ERKS0_',
        '_ZN7android7String85setToEPKc','_ZN7android7String85setToERKS0_',
        '_ZN7android7String86appendEPKc','_ZN7android7String86appendERKS0_',
        '_ZN7android7String86formatEPKcz','_ZN7android7String812appendFormatEPKcz',
        '_ZN7android7String8D1Ev','_ZNK7android7String86lengthEv',
        '_ZNK7android8hardware11hidl_string4sizeEv','_ZNK7android8hardware11hidl_string5c_strEv',
        '_ZN7android10VectorImplC2Emj','_ZN7android16SortedVectorImplC2Emj',
        '_ZN7android10VectorImpl13finish_vectorEv','_ZN7android10VectorImplD2Ev',
        '_ZN7android16SortedVectorImplD2Ev','_ZN7android10VectorImpl5clearEv',
        '_ZN7android10VectorImpl11appendArrayEPKvm','_ZN7android16SortedVectorImpl3addEPKv',
        '_ZNK7android16SortedVectorImpl7indexOfEPKv'}

with open('stub_auto.c','w') as f:
    f.write('/* Auto-generated Android platform stubs — satisfy dynamic linkage only.\n')
    f.write('   The CPDP DSP core does not need these; they exist because the Android\n')
    f.write('   wrapper code inside the libraries is eagerly bound (BIND_NOW). */\n')
    f.write('''
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
''')
    f.write(PRELUDE)
    f.write('\n/* ---- no-op placeholders (not exercised on the CPDP path) ---- */\n')
    for n in funcs:
        if n in HAND: continue
        f.write(f'void {n}(void) {{ }}\n')
    for n in objs:
        f.write(f'unsigned char {n}[128] __attribute__((aligned(8))) = {{0}};\n')
print('wrote stub_auto.c', file=sys.stderr)
