/* Guest CPU model for recompiled i386 code. Guest addresses == host addresses (32-bit process). */
#pragma once
#include <stdint.h>
#include <setjmp.h>
#include <math.h>

typedef uint8_t u8;   typedef int8_t s8;
typedef uint16_t u16; typedef int16_t s16;
typedef uint32_t u32; typedef int32_t s32;
typedef uint64_t u64; typedef int64_t s64;
typedef long double f80;

typedef union { u32 d; u16 w; struct { u8 l, h; } b; } Reg;

typedef struct CPU {
    Reg r[8];                 /* eax ecx edx ebx esp ebp esi edi */
    f80 st[8];
    u32 top;
    u16 fcw, fsw;
    u32 cur;                  /* current guest function (debug) */
    u32 trace[64];
    u32 ti;
    int err;                  /* errno (guest-visible via __errno_location) */
    int is_main;
    int tid;
    struct VforkCtx *vfork;   /* active vfork child emulation */
    void *stack_base;
} CPU;

#define M8(a)  (*(u8  *)(uintptr_t)(a))
#define M16(a) (*(u16 *)(uintptr_t)(a))
#define M32(a) (*(u32 *)(uintptr_t)(a))
#define MF32(a) (*(float *)(uintptr_t)(a))
#define MF64(a) (*(double *)(uintptr_t)(a))
#define MF80(a) (*(long double *)(uintptr_t)(a))

/* ---- lazy flags: fop = kind*4 + size(0=8,1=16,2=32) */
enum { K_ADD, K_ADC, K_SUB, K_SBB, K_LOGIC, K_INC, K_DEC, K_SHL, K_SHR, K_SAR, K_MUL, K_BT, K_FLAGS };
/* K_FLAGS (sahf): r holds the raw low flag byte (CF bit0, PF bit2, ZF bit6, SF bit7) */

static inline u32 f_mask(u32 op) { return (op & 3) == 0 ? 0xffu : (op & 3) == 1 ? 0xffffu : 0xffffffffu; }
static inline u32 f_sign(u32 op) { return (op & 3) == 0 ? 0x80u : (op & 3) == 1 ? 0x8000u : 0x80000000u; }
static inline u32 f_bits(u32 op) { return (op & 3) == 0 ? 8 : (op & 3) == 1 ? 16 : 32; }

static inline u32 CF(u32 op, u32 a, u32 b, u32 r, u32 cf) {
    u32 M = f_mask(op);
    a &= M; b &= M; r &= M;
    switch (op >> 2) {
    case K_ADD: return r < a;
    case K_ADC: return cf ? r <= a : r < a;
    case K_SUB: return a < b;
    case K_SBB: return cf ? a <= b : a < b;
    case K_LOGIC: return 0;
    case K_INC: case K_DEC: return cf;
    case K_SHL: return b <= f_bits(op) ? (a >> (f_bits(op) - b)) & 1 : 0;
    case K_SHR: return (a >> (b - 1)) & 1;
    case K_SAR: {
        s32 sa = (op & 3) == 0 ? (s32)(s8)a : (op & 3) == 1 ? (s32)(s16)a : (s32)a;
        return (u32)(sa >> (b - 1)) & 1;
    }
    default: return cf;   /* K_MUL, K_BT */
    }
}
static inline u32 ZF(u32 op, u32 a, u32 b, u32 r, u32 cf) { (void)a; (void)b; (void)cf; if ((op >> 2) == K_FLAGS) return (r >> 6) & 1; return (r & f_mask(op)) == 0; }
static inline u32 SF(u32 op, u32 a, u32 b, u32 r, u32 cf) { (void)a; (void)b; (void)cf; if ((op >> 2) == K_FLAGS) return (r >> 7) & 1; return (r & f_sign(op)) != 0; }
static inline u32 PF(u32 op, u32 a, u32 b, u32 r, u32 cf) { (void)a; (void)b; (void)cf; if ((op >> 2) == K_FLAGS) return (r >> 2) & 1; return !__builtin_parity(r & 0xff); }
static inline u32 OF(u32 op, u32 a, u32 b, u32 r, u32 cf) {
    u32 S = f_sign(op), M = f_mask(op);
    switch (op >> 2) {
    case K_ADD: case K_ADC: return ((a ^ r) & (b ^ r) & S) != 0;
    case K_SUB: case K_SBB: return ((a ^ b) & (a ^ r) & S) != 0;
    case K_INC: return (r & M) == S;
    case K_DEC: return (r & M) == S - 1;
    case K_SHL: return ((r & S) != 0) ^ CF(op, a, b, r, cf);
    case K_SHR: return (a & S) != 0;
    case K_MUL: return cf;
    default: return 0;
    }
}
#define FARGS u32 op, u32 a, u32 b, u32 r, u32 cf
#define FPASS op, a, b, r, cf
static inline int CC_E(FARGS)  { return ZF(FPASS); }
static inline int CC_NE(FARGS) { return !ZF(FPASS); }
static inline int CC_B(FARGS)  { return CF(FPASS); }
static inline int CC_AE(FARGS) { return !CF(FPASS); }
static inline int CC_BE(FARGS) { return CF(FPASS) | ZF(FPASS); }
static inline int CC_A(FARGS)  { return !(CF(FPASS) | ZF(FPASS)); }
static inline int CC_S(FARGS)  { return SF(FPASS); }
static inline int CC_NS(FARGS) { return !SF(FPASS); }
static inline int CC_L(FARGS)  { return SF(FPASS) != OF(FPASS); }
static inline int CC_GE(FARGS) { return SF(FPASS) == OF(FPASS); }
static inline int CC_LE(FARGS) { return ZF(FPASS) | (SF(FPASS) != OF(FPASS)); }
static inline int CC_G(FARGS)  { return !ZF(FPASS) && (SF(FPASS) == OF(FPASS)); }
static inline int CC_P(FARGS)  { return PF(FPASS); }
static inline int CC_NP(FARGS) { return !PF(FPASS); }

/* ---- x87 */
#define ST(i) c->st[(c->top + (i)) & 7]
#define FPUSH(v) do { c->top = (c->top - 1) & 7; c->st[c->top] = (v); } while (0)
#define FPOP() (c->top = (c->top + 1) & 7)
#define FSW() ((u16)((c->fsw & ~0x3800) | ((c->top & 7) << 11)))
static inline void FCOM_(struct CPU *c, f80 a, f80 b) {
    u16 s = c->fsw & ~0x4500;
    if (a != a || b != b) s |= 0x4500;
    else if (a < b) s |= 0x0100;
    else if (a == b) s |= 0x4000;
    c->fsw = s;
}
#define FCOM(a, b) FCOM_(c, (a), (b))
s32 rt_fist32(CPU *c, f80 v);
s64 rt_fist64(CPU *c, f80 v);

/* ---- runtime services used by generated code */
extern volatile int rt_sigpending;
void rt_deliver(CPU *c);
#define SIGCHK(c) do { if (rt_sigpending) rt_deliver(c); } while (0)
#define ENTER(c, a) do { (c)->cur = (a); (c)->trace[(c)->ti++ & 63] = (a); SIGCHK(c); } while (0)

void rt_call_ind(CPU *c, u32 target);
void rt_jmp_ind(CPU *c, u32 target, u32 from);
void rt_divzero(CPU *c, u32 at);
void rt_hlt(CPU *c, u32 at);
void rt_unimpl(CPU *c, u32 at);
u8 rt_inb(CPU *c, u16 port);
void rt_outb(CPU *c, u16 port, u8 v);
typedef void *rt_jb[8];
#define RT_SETJMP(jb) __builtin_setjmp((void **)(jb))
rt_jb *rt_setjmp_prep(CPU *c, int sig);

void rt_setjmp_back(CPU *c);

/* ---- image / dispatch tables produced by the recompiler */
typedef void (*GFn)(CPU *c);
typedef struct { u32 addr; GFn fn; } DispEnt;
typedef struct { u32 got, stub; const char *name; } GotEnt;
typedef struct { u32 addr; int type; const char *name; } DataImp;
typedef struct { u32 va, filesz, memsz; const unsigned char *data; } ImgSeg;
extern const DispEnt guest_dispatch[];
extern const int guest_dispatch_n;
extern const u32 guest_entry;
extern const int guest_is_game;     /* 1 for v8mnu22b, 0 for the helper tools */
extern const char guest_name[];
extern const GotEnt guest_got[];
extern const int guest_got_n;
extern const DataImp guest_dataimp[];
extern const int guest_dataimp_n;
extern const ImgSeg guest_segs[];
extern const int guest_nsegs;
