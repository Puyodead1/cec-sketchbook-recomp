#include <io.h>
#include <fcntl.h>
/* Win32 runtime core: image mapping, dispatch, setjmp/longjmp, startup, crash reporting. */
#include "rt.h"
#include <stdarg.h>
#include <direct.h>
#include <float.h>
#include <math.h>

CPU *rt_main_cpu;
DWORD rt_main_tid;
char rt_root[MAX_PATH];
FILE *rt_log;
static CRITICAL_SECTION log_cs;

void rt_logf(const char *fmt, ...) {
    va_list ap;
    if (!rt_log) return;
    EnterCriticalSection(&log_cs);
    va_start(ap, fmt);
    vfprintf(rt_log, fmt, ap);
    va_end(ap);
    fflush(rt_log);
    LeaveCriticalSection(&log_cs);
}

static int readable(u32 a) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void *)(uintptr_t)a, &mbi, sizeof mbi)) return 0;
    return mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}

static void dump_cpu(CPU *c) {
    int i, j;
    if (!c) return;
    rt_logf("  in guest fn %08x  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n",
            c->cur, c->r[0].d, c->r[1].d, c->r[2].d, c->r[3].d, c->r[4].d, c->r[5].d, c->r[6].d, c->r[7].d);
    rt_logf("  recent entries:");
    for (i = 16; i >= 1; i--) rt_logf(" %08x", c->trace[(c->ti - i) & 63]);
    rt_logf("\n  stack:");
    for (i = 0; i < 16; i++) {
        u32 a = c->r[4].d + 4 * i;
        if (!readable(a)) break;
        rt_logf(" %08x", M32(a));
    }
    rt_logf("\n  guest return addresses:");   /* likely call chain: words on the guest stack that point into guest code */
    for (i = 0, j = 0; i < 4096 && j < 32; i++) {
        u32 a = c->r[4].d + 4 * i, v;
        if ((a & 0xfff) == 0 && !readable(a)) break;
        v = M32(a);
        if (v >= 0x8048000 && v < 0x80b0000) { rt_logf(" %08x", v); j++; }
    }
    rt_logf("\n");
}

void rt_fatal(CPU *c, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rt_logf("FATAL: %s\n", buf);
    dump_cpu(c);
    fprintf(stderr, "FATAL: %s (see rt.log)\n", buf);
    fflush(stderr);
    ExitProcess(3);
}

/* ---------------- dispatch */
#define NHOSTCB 64
#define HOSTCB_BASE 0x0FFF0000u
static void (*hostcb[NHOSTCB])(CPU *c);
static int nhostcb;

u32 rt_host_callback(void (*fn)(CPU *c)) {
    int i;
    for (i = 0; i < nhostcb; i++) if (hostcb[i] == fn) return HOSTCB_BASE + 16 * i;
    hostcb[nhostcb] = fn;
    return HOSTCB_BASE + 16 * nhostcb++;
}

GFn rt_lookup(u32 a) {
    int lo = 0, hi = guest_dispatch_n - 1;
    if (a >= HOSTCB_BASE && a < HOSTCB_BASE + 16 * NHOSTCB && !((a - HOSTCB_BASE) & 15)) {
        int k = (a - HOSTCB_BASE) / 16;
        return k < nhostcb ? (GFn)hostcb[k] : 0;
    }
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        u32 m = guest_dispatch[mid].addr;
        if (m == a) return guest_dispatch[mid].fn;
        if (m < a) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

void rt_call_ind(CPU *c, u32 t) {
    GFn f = rt_lookup(t);
    if (!f) rt_fatal(c, "indirect call to unknown target %08x", t);
    f(c);
}

void rt_jmp_ind(CPU *c, u32 t, u32 from) {
    GFn f = rt_lookup(t);
    if (!f) rt_fatal(c, "indirect jump from %08x to unknown target %08x", from, t);
    f(c);
}

#define MAGIC_RET 0x0FFFFFF0u
void rt_call_guest(CPU *c, u32 addr, int n, const u32 *args) {
    int i;
    GFn f = rt_lookup(addr);
    if (!f) rt_fatal(c, "call to unknown guest function %08x", addr);
    for (i = n - 1; i >= 0; i--) { c->r[4].d -= 4; M32(c->r[4].d) = args[i]; }
    c->r[4].d -= 4; M32(c->r[4].d) = MAGIC_RET;
    f(c);
    c->r[4].d += 4 * n;
}

void rt_divzero(CPU *c, u32 at) { rt_fatal(c, "integer divide error at %08x", at); }
void rt_hlt(CPU *c, u32 at) { rt_fatal(c, "hlt at %08x", at); }
void rt_unimpl(CPU *c, u32 at) { rt_fatal(c, "untranslated function %08x", at); }
u8 rt_inb(CPU *c, u16 port) { (void)c; (void)port; return 0xff; }
void rt_outb(CPU *c, u16 port, u8 v) { (void)c; (void)port; (void)v; }

s64 rt_fist64(CPU *c, f80 v) {
    f80 r;
    if (v != v) return (s64)0x8000000000000000ull;
    switch ((c->fcw >> 10) & 3) {
    case 0: r = rintl(v); break;
    case 1: r = (f80)(s64)v; if (r > v) r -= 1; break;
    case 2: r = (f80)(s64)v; if (r < v) r += 1; break;
    default: r = (f80)(s64)v; break;
    }
    if (r >= 9223372036854775808.0L || r < -9223372036854775808.0L) return (s64)0x8000000000000000ull;
    return (s64)r;
}

s32 rt_fist32(CPU *c, f80 v) {
    f80 r;
    if (v != v) return (s32)0x80000000;
    switch ((c->fcw >> 10) & 3) {
    case 0: r = rintl(v); break;                          /* nearest even (host default mode) */
    case 1: r = (f80)(s64)v; if (r > v) r -= 1; break;    /* down */
    case 2: r = (f80)(s64)v; if (r < v) r += 1; break;    /* up */
    default: r = (f80)(s64)v; break;                      /* chop */
    }
    if (r > 2147483647.0L || r < -2147483648.0L) return (s32)0x80000000;
    return (s32)(s64)r;
}

/* ---------------- setjmp / longjmp */
typedef struct {
    u32 env;          /* guest jmp_buf address */
    DWORD tid;
    rt_jb jb;
    int savemask;
    u64 mask;
    u32 ebx, esi, edi, ebp, esp;
    u32 val;
    int used;
} SjEnt;
#define NSJ 128
static SjEnt sj[NSJ];
static CRITICAL_SECTION sj_cs;

static SjEnt *sj_find(u32 env, DWORD tid, int create) {
    int i, fr = -1;
    SjEnt *e = 0;
    EnterCriticalSection(&sj_cs);
    for (i = 0; i < NSJ; i++) {
        if (sj[i].used && sj[i].env == env && sj[i].tid == tid) { e = &sj[i]; break; }
        if (!sj[i].used && fr < 0) fr = i;
    }
    if (!e && create) {
        if (fr < 0) fr = (int)(env % NSJ);   /* evict */
        e = &sj[fr];
        memset(e, 0, sizeof *e);
        e->used = 1; e->env = env; e->tid = tid;
    }
    LeaveCriticalSection(&sj_cs);
    return e;
}

u64 rt_sigmask_get(void);
void rt_sigmask_set(u64 m);
rt_jb *rt_setjmp_prep(CPU *c, int sig) {
    u32 env = ARG(0);
    SjEnt *e = sj_find(env, GetCurrentThreadId(), 1);
    e->savemask = sig && ARG(1);
    if (e->savemask) e->mask = rt_sigmask_get();
    c->r[4].d += 4;   /* pop the return address pushed by the call */
    e->ebx = c->r[3].d; e->esi = c->r[6].d; e->edi = c->r[7].d; e->ebp = c->r[5].d; e->esp = c->r[4].d;
    /* mirror into the guest buffer (glibc layout: ebx esi edi ebp esp pc) */
    M32(env + 0) = e->ebx; M32(env + 4) = e->esi; M32(env + 8) = e->edi;
    M32(env + 12) = e->ebp; M32(env + 16) = e->esp; M32(env + 20) = 0;
    return &e->jb;
}

void rt_setjmp_back(CPU *c) {
    /* registers were restored by rt_longjmp before the host longjmp */
    (void)c;
}

void rt_longjmp(CPU *c, u32 env, u32 val) {
    SjEnt *e = sj_find(env, GetCurrentThreadId(), 0);
    if (!e) rt_fatal(c, "longjmp to unknown jmp_buf %08x", env);
    c->r[3].d = e->ebx; c->r[6].d = e->esi; c->r[7].d = e->edi; c->r[5].d = e->ebp; c->r[4].d = e->esp;
    c->r[0].d = val ? val : 1;
    if (e->savemask && c->is_main) rt_sigmask_set(e->mask);
    __builtin_longjmp((void **)e->jb, 1);
}

/* ---------------- CPU / FPU */
void rt_fpu_init(void) {
    unsigned short cw = 0x037F;   /* extended precision, all exceptions masked (Linux default) */
    __asm__ volatile("fldcw %0" : : "m"(cw));
}

CPU *rt_new_cpu(void) {
    CPU *c = (CPU *)calloc(1, sizeof(CPU));
    c->fcw = 0x037F;
    c->tid = (int)GetCurrentThreadId();
    return c;
}

/* ---------------- crash reporting */
static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_PRIV_INSTRUCTION) {
        rt_logf("host exception %08lx at %p (addr %08lx)\n", code, ep->ExceptionRecord->ExceptionAddress,
                ep->ExceptionRecord->NumberParameters > 1 ? (unsigned long)ep->ExceptionRecord->ExceptionInformation[1] : 0ul);
        if (GetCurrentThreadId() == rt_main_tid) dump_cpu(rt_main_cpu);
        else {
            /* secondary thread: list stack words that point into our exe's code (resolve with nm) */
            u8 *base = (u8 *)GetModuleHandleA(0);
            u32 lo = (u32)(uintptr_t)base, hi = lo + ((IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew))->OptionalHeader.SizeOfImage;
            u32 *sp = (u32 *)(uintptr_t)ep->ContextRecord->Esp;
            int i, n = 0;
            rt_logf("  (in a secondary thread) eip=%08lx host stack:", (unsigned long)ep->ContextRecord->Eip);
            for (i = 0; i < 2048 && n < 24; i++) {
                MEMORY_BASIC_INFORMATION mi;
                if (((uintptr_t)(sp + i) & 0xfff) == 0 && (!VirtualQuery(sp + i, &mi, sizeof mi) || mi.State != MEM_COMMIT)) break;
                if (sp[i] > lo && sp[i] < hi) { rt_logf(" %08x", sp[i]); n++; }
            }
            rt_logf("\n");
        }
        fprintf(stderr, "crash %08lx, see rt.log\n", code);
        fflush(stderr);
        ExitProcess(4);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---------------- startup */
static void map_image(void) {
    int i;
    u32 lo = 0xffffffffu, hi = 0;
    for (i = 0; i < guest_nsegs; i++) {
        if (guest_segs[i].va < lo) lo = guest_segs[i].va;
        if (guest_segs[i].va + guest_segs[i].memsz > hi) hi = guest_segs[i].va + guest_segs[i].memsz;
    }
    lo &= ~0xFFFFu;
    hi = (hi + 0xFFFF) & ~0xFFFFu;
    void *p = VirtualAlloc((void *)(uintptr_t)lo, hi - lo, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p != (void *)(uintptr_t)lo) {
        fprintf(stderr, "cannot map guest image at %08x (err %lu)\n", lo, GetLastError());
        ExitProcess(2);
    }
    for (i = 0; i < guest_nsegs; i++)
        memcpy((void *)(uintptr_t)guest_segs[i].va, guest_segs[i].data, guest_segs[i].filesz);
    /* the host-callback page must not collide with anything */
    VirtualAlloc((void *)(uintptr_t)HOSTCB_BASE, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
}

extern void rt_link_imports(void);   /* libc.c: GOT + data imports */
static int g_argc;
static char **g_argv;

/* RT_WATCH=<sec>: periodically log where the main guest thread is, with return addresses on its stack */
static DWORD WINAPI watch_thread(void *arg) {
    int sec = (int)(intptr_t)arg, i, k;
    for (;;) {
        CPU *c = rt_main_cpu;
        Sleep(sec * 1000);
        if (!c) continue;
        rt_logf("WATCH:\n");
        dump_cpu(c);
        rt_logf("  callers:");
        for (i = 0, k = 0; i < 4096 && k < 24; i++) {
            u32 a = c->r[4].d + 4 * i, v;
            if (!readable(a)) break;
            v = M32(a);
            if (v >= 0x08048000 && v < 0x080B0000) { rt_logf(" %08x", v); k++; }
        }
        rt_logf("\n");
    }
    return 0;
}

/* RT_PROF=<seconds>: sample the guest thread's host instruction pointer every ~1 ms,
   dump a histogram to tmp\prof.txt every <seconds> (resolve with nm). */
static HANDLE prof_target;
static DWORD WINAPI prof_thread(void *arg) {
    enum { NB = 1 << 16 };
    static u64 key[NB]; static u32 cnt[NB];
    int sec = (int)(intptr_t)arg; u32 total = 0, waits = 0;
    DWORD next = GetTickCount() + sec * 1000;
    for (;;) {
        CONTEXT ctx; Sleep(1);
        if (!prof_target) continue;
        if (SuspendThread(prof_target) == (DWORD)-1) continue;
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(prof_target, &ctx)) {
#ifdef _WIN64
            u64 ip = ctx.Rip;
#else
            u64 ip = ctx.Eip;
#endif
            static uintptr_t lo, hi;
            if (!lo) { IMAGE_NT_HEADERS *nt; lo = (uintptr_t)GetModuleHandleA(0);
                       nt = (IMAGE_NT_HEADERS *)(lo + ((IMAGE_DOS_HEADER *)lo)->e_lfanew); hi = lo + nt->OptionalHeader.SizeOfImage; }
            if (ip < lo || ip >= hi) {   /* in a DLL: attribute to the nearest exe return address (x2 = inner, x4 = outer) */
#ifdef _WIN64
                uintptr_t *sp = (uintptr_t *)ctx.Rsp;
#else
                uintptr_t *sp = (uintptr_t *)ctx.Esp;
#endif
                int j, found = 0;
                for (j = 0; j < 512 && found < 2; j++) {
                    uintptr_t v; if (IsBadReadPtr(sp + j, sizeof v)) break;
                    v = sp[j];
                    if (v >= lo && v < hi) {
                        u64 k2 = (u64)v | (found ? 0x400000000000ull : 0x200000000000ull);
                        u32 h2 = (u32)(k2 * 2654435761u) & (NB - 1);
                        while (key[h2] && key[h2] != k2) h2 = (h2 + 1) & (NB - 1);
                        key[h2] = k2; cnt[h2]++; found++;
                    }
                }
            }
#ifdef _WIN64
            ip = ctx.Rip;
#else
            ip = ctx.Eip;
#endif
            u32 h = (u32)(ip * 2654435761u) & (NB - 1);
            while (key[h] && key[h] != ip) h = (h + 1) & (NB - 1);
            key[h] = ip; cnt[h]++; total++;
        }
        ResumeThread(prof_target);
        if ((int)(GetTickCount() - next) >= 0) {
            char p[MAX_PATH]; FILE *f; int i;
            snprintf(p, sizeof p, "%s\\tmp\\prof.txt", rt_root);
            if ((f = fopen(p, "w"))) {
                fprintf(f, "total %u\n", total);
                for (i = 0; i < NB; i++) if (key[i]) fprintf(f, "%llx %u\n", (unsigned long long)key[i], cnt[i]);
                fclose(f);
            }
            next = GetTickCount() + sec * 1000;
        }
        (void)waits;
    }
    return 0;
}

static DWORD WINAPI guest_main_thread(void *arg) {
    int i;
    CPU *c;
    u32 sp, gargv, genvp, gargc;
    static const char *genv[] = { "HOME=/vend", "TMPDIR=/vend/tmp", "PATH=.:/vend/bin:/bin:/usr/bin:/usr/X11R6/bin",
                                  "DISPLAY=:0", "TERM=linux", "LANG=C", 0 };
    int argc = g_argc;
    char **argv = g_argv;
    (void)arg;
    rt_fpu_init();
    rt_main_tid = GetCurrentThreadId();
    c = rt_main_cpu = rt_new_cpu();
    c->is_main = 1;

    /* guest stack: 16 MB */
    c->stack_base = VirtualAlloc(0, 16 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!c->stack_base) { fprintf(stderr, "no stack\n"); return 2; }
    sp = (u32)(uintptr_t)c->stack_base + (16 << 20) - 4096;

    /* argv: game name + our args (default -vt1: plain XImage video) */
    {
        const char *gav[32];
        int gac = 0, k;
        u32 ptrs[32], eptrs[16];
        static char self[64];
        snprintf(self, sizeof self, guest_is_game ? "/vend/bin/bttv/%s" : "/vend/bin/%s", guest_name);
        gav[gac++] = self;
        for (k = 1; k < argc && gac < 31; k++) gav[gac++] = argv[k];
        if (gac == 1 && guest_is_game) gav[gac++] = "-vt1";
        for (k = 0; k < gac; k++) {
            size_t L = strlen(gav[k]) + 1;
            sp -= (u32)L; memcpy((void *)(uintptr_t)sp, gav[k], L); ptrs[k] = sp;
        }
        for (k = 0; genv[k]; k++) {
            size_t L = strlen(genv[k]) + 1;
            sp -= (u32)L; memcpy((void *)(uintptr_t)sp, genv[k], L); eptrs[k] = sp;
        }
        sp &= ~15u;
        /* envp */
        sp -= 4; M32(sp) = 0;
        for (i = k - 1; i >= 0; i--) { sp -= 4; M32(sp) = eptrs[i]; }
        genvp = sp;
        sp -= 4; M32(sp) = 0;
        for (i = gac - 1; i >= 0; i--) { sp -= 4; M32(sp) = ptrs[i]; }
        gargv = sp;
        sp -= 4; M32(sp) = gac;
        gargc = gac;
    }
    (void)genvp; (void)gargv; (void)gargc;
    c->r[4].d = sp;
    c->r[2].d = 0;   /* edx: rtld fini */
    rt_logf("entering guest at %08x\n", guest_entry);
    {
        GFn f = rt_lookup(guest_entry);
        if (!f) rt_fatal(c, "no entry");
        f(c);
    }
    rt_exit(c, 0);
    return 0;
}

int main(int argc, char **argv) {
    char exe[MAX_PATH], *s;
    g_argc = argc; g_argv = argv;
    _setmode(0, _O_BINARY); _setmode(1, _O_BINARY); _setmode(2, _O_BINARY);   /* guest stdio is raw bytes */
    InitializeCriticalSection(&log_cs);
    InitializeCriticalSection(&sj_cs);
    map_image();
    rt_fpu_init();

    /* the game only: --fullscreen starts in fullscreen (same as VEND_FULLSCREEN=1), and
       a picture or video file named on the command line (e.g. dragged onto the exe) is the camera.
       Resolved here, before the chdir into the data folder; the helper tools take file arguments of their own. */
    if (guest_is_game) {
        int k, n = 1;
        for (k = 1; k < argc; k++) {
            DWORD at = GetFileAttributesA(argv[k]);
            if (!_stricmp(argv[k], "--fullscreen") || !_stricmp(argv[k], "-fullscreen")) _putenv("VEND_FULLSCREEN=1");
            else if (argv[k][0] != '-' && at != INVALID_FILE_ATTRIBUTES && !(at & FILE_ATTRIBUTE_DIRECTORY)) {
                static char ev[MAX_PATH + 16];
                char full[MAX_PATH];
                if (!GetFullPathNameA(argv[k], sizeof full, full, 0)) snprintf(full, sizeof full, "%s", argv[k]);
                snprintf(ev, sizeof ev, "RT_CAMERA=%s", full);
                _putenv(ev);
            } else argv[n++] = argv[k];
        }
        g_argc = argc = n;
    }

    /* data root: <exe dir>\vend unless VEND_ROOT is set */
    if (getenv("VEND_ROOT")) snprintf(rt_root, sizeof rt_root, "%s", getenv("VEND_ROOT"));
    else {
        GetModuleFileNameA(0, exe, sizeof exe);
        s = strrchr(exe, '\\');
        if (s) *s = 0;
        snprintf(rt_root, sizeof rt_root, "%s\\vend", exe);
    }
    if (_chdir(rt_root) != 0) {
        fprintf(stderr, "data directory %s not found\n", rt_root);
        return 2;
    }
    _mkdir("tmp");
    if (guest_is_game) rt_log = fopen("tmp\\rt.log", "w");
    else if (getenv("RT_TOOLLOG")) {
        char ln[128];
        snprintf(ln, sizeof ln, "tmp\\rt-%s.log", guest_name);
        rt_log = fopen(ln, "a");
    }
    rt_logf("runtime start, root=%s\n", rt_root);
    if (guest_is_game && getenv("RT_CAMERA")) rt_logf("camera source: %s\n", getenv("RT_CAMERA"));

    AddVectoredExceptionHandler(1, veh);
    rt_link_imports();
    rt_sig_init();
    rt_timers_init();
    uib_reset();
    if (!guest_is_game) SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);

    {
        if (getenv("RT_WATCH")) CreateThread(0, 0, watch_thread, (void *)(intptr_t)atoi(getenv("RT_WATCH")), 0, 0);
        HANDLE t = CreateThread(0, 256u << 20, guest_main_thread, 0, STACK_SIZE_PARAM_IS_A_RESERVATION, 0);
        if (!t) { fprintf(stderr, "cannot create guest thread\n"); return 2; }
        if (getenv("RT_PROF")) { prof_target = t; CreateThread(0, 0, prof_thread, (void *)(intptr_t)atoi(getenv("RT_PROF")), 0, 0); }
        WaitForSingleObject(t, INFINITE);
    }
    return 0;
}
