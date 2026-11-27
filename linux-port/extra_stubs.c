/* Supplementary stubs for symbols referenced only by the Android APS2-packed
   relocations (not in .rela.plt, so gen_stubs.py never saw them). */

#include <pthread.h>
#include <stddef.h>

void _ZNSt3__119__shared_weak_countD2Ev(void) { }
void _ZNKSt3__119__shared_weak_count13__get_deleterERKSt9type_info(void) { }
void _ZN7android7RefBase10onFirstRefEv(void) { }
void _ZN7android7RefBase15onLastStrongRefEPKv(void) { }
void _ZN7android7RefBase20onIncStrongAttemptedEjPKv(void) { }
void _ZN7android7RefBase13onLastWeakRefEPKv(void) { }

#define HIDL_NOP(name) void name(void) { }
HIDL_NOP(_ZN6vendor5dolby8hardware3dms4V2_013IDmsCallbacks14interfaceChainENSt3__18functionIFvRKN7android8hardware8hidl_vecINS8_11hidl_stringEEEEEE)
HIDL_NOP(_ZN6vendor5dolby8hardware3dms4V2_013IDmsCallbacks5debugERKN7android8hardware11hidl_handleERKNS6_8hidl_vecINS6_11hidl_stringEEE)
HIDL_NOP(_ZN6vendor5dolby8hardware3dms4V2_013IDmsCallbacks19interfaceDescriptorENSt3__18functionIFvRKN7android8hardware11hidl_stringEEEE)
HIDL_NOP(_ZN6vendor5dolby8hardware3dms4V2_013IDmsCallbacks12getHashChainENSt3__18functionIFvRKN7android8hardware8hidl_vecINS8_10hidl_arrayIhLm32EJEEEEEEEE)
HIDL_NOP(_ZN6vendor5dolby8hardware3dms4V2_013IDmsCallbacks21setHALInstrumentationEv)
HIDL_NOP(_ZN6vendor5dolby8hardware3dms4V2_013IDmsCallbacks11linkToDeathERKN7android2spINS5_8hardware20hidl_death_recipientEEEm)
HIDL_NOP(_ZN6vendor5dolby8hardware3dms4V2_013IDmsCallbacks4pingEv)
HIDL_NOP(_ZN6vendor5dolby8hardware3dms4V2_013IDmsCallbacks12getDebugInfoENSt3__18functionIFvRKN7android4hidl4base4V1_09DebugInfoEEEE)
HIDL_NOP(_ZN6vendor5dolby8hardware3dms4V2_013IDmsCallbacks21notifySyspropsChangedEv)
HIDL_NOP(_ZN6vendor5dolby8hardware3dms4V2_013IDmsCallbacks13unlinkToDeathERKN7android2spINS5_8hardware20hidl_death_recipientEEE)
HIDL_NOP(_ZN7android4hidl4base4V1_05IBase14interfaceChainENSt3__18functionIFvRKNS_8hardware8hidl_vecINS6_11hidl_stringEEEEEE)
HIDL_NOP(_ZN7android4hidl4base4V1_05IBase5debugERKNS_8hardware11hidl_handleERKNS4_8hidl_vecINS4_11hidl_stringEEE)
HIDL_NOP(_ZN7android4hidl4base4V1_05IBase19interfaceDescriptorENSt3__18functionIFvRKNS_8hardware11hidl_stringEEEE)
HIDL_NOP(_ZN7android4hidl4base4V1_05IBase12getHashChainENSt3__18functionIFvRKNS_8hardware8hidl_vecINS4_10hidl_arrayIhLm32EJEEEEEEEE)
HIDL_NOP(_ZN7android4hidl4base4V1_05IBase21setHALInstrumentationEv)
HIDL_NOP(_ZN7android4hidl4base4V1_05IBase11linkToDeathERKNS_2spINS_8hardware20hidl_death_recipientEEEm)
HIDL_NOP(_ZN7android4hidl4base4V1_05IBase4pingEv)
HIDL_NOP(_ZN7android4hidl4base4V1_05IBase12getDebugInfoERKNS2_9DebugInfoE)
HIDL_NOP(_ZN7android4hidl4base4V1_05IBase21notifySyspropsChangedEv)
HIDL_NOP(_ZN7android4hidl4base4V1_05IBase13unlinkToDeathERKNS_2spINS_8hardware20hidl_death_recipientEEE)

pthread_mutex_t _ZN7android9SingletonINS_19PregainSynchronizerEE5sLockE = PTHREAD_MUTEX_INITIALIZER;
void *_ZN7android9SingletonINS_19PregainSynchronizerEE9sInstanceE = 0;

/* Return-correct overrides (linked ahead of the generic no-op stub libs). */
long _ZNK7android16SortedVectorImpl7indexOfEPKv(void) { return -1; }
int _ZNK7android8AMessage9findInt32EPKcPi(void) { return 0; }
const char *_ZNK7android8hardware11hidl_string5c_strEv(void) { return ""; }
unsigned long _ZNK7android8hardware11hidl_string4sizeEv(void) { return 0; }
int _ZNK7android7String86lengthEv(void) { return 0; }

/* HIDL DMS service lookup: emulate "service not available" by returning a
   null sp<> through the sret register. */
#include <string.h>
__asm__(
".text\n"
".global _ZN6vendor5dolby8hardware3dms4V2_04IDms13tryGetServiceERKNSt3__112basic_stringIcNS5_11char_traitsIcEENS5_9allocatorIcEEEEb\n"
"_ZN6vendor5dolby8hardware3dms4V2_04IDms13tryGetServiceERKNSt3__112basic_stringIcNS5_11char_traitsIcEENS5_9allocatorIcEEEEb:\n"
"    stp xzr, xzr, [x8]\n"
"    ret\n"
);

/* Functionally correct overrides for symbols the DSP path needs. */
#include <stdlib.h>
void *_Znwm(unsigned long n) { return malloc(n ? n : 1); }
void _ZdlPv(void *p) { free(p); }
void _ZdlPvm(void *p, unsigned long) { free(p); }
void *_Znam(unsigned long n) { return malloc(n ? n : 1); }
void _ZdaPv(void *p) { free(p); }
void _ZSt17__throw_bad_allocv(void) { __builtin_trap(); }
