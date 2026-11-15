/* Signals, interval timers and __clone threads, emulated on Win32.
 * Signals are delivered to the main guest thread synchronously at SIGCHK points
 * (function entries, loop back-edges) and inside interruptible blocking calls. */
#include "rt.h"
#include <mmsystem.h>

volatile int rt_sigpending;
static volatile LONG pending;       /* bitmask of pending signals 1..31 */
static u64 blocked;                 /* main thread mask */
static struct { u32 handler, flags; u64 mask; } act[65];
static HANDLE wake_evt;             /* set whenever a signal is raised */

u64 rt_sigmask_get(void) { return blocked; }
void rt_sigmask_set(u64 m) { blocked = m & ~((1ull << GSIG_KILL) | (1ull << 19)); if (pending) rt_sigpending = 1; }

void rt_raise(int sig) {
    if (sig <= 0 || sig > 31) return;
    __sync_fetch_and_or(&pending, 1L << sig);
    rt_sigpending = 1;
    SetEvent(wake_evt);
}

int rt_signal_interrupts(CPU *c) {
    if (!c->is_main) return 0;
    return (pending & ~(LONG)blocked) != 0;
}

static void default_action(CPU *c, int sig) {
    switch (sig) {
    case GSIG_CHLD: case 18: case 23: case 28: return;   /* ignore */
    default:
        rt_logf("signal %d with default action: terminating\n", sig);
        rt_exit(c, 128 + sig);
    }
}

void rt_deliver(CPU *c) {
    int sig;
    if (!c->is_main) return;
    for (;;) {
        LONG p = pending & ~(LONG)blocked;
        if (!p) { if (!pending) rt_sigpending = 0; return; }
        for (sig = 1; sig < 32; sig++) if (p & (1L << sig)) break;
        __sync_fetch_and_and(&pending, ~(1L << sig));
        if (!pending) rt_sigpending = 0;
        {
            u32 h = act[sig].handler;
            if (h == 1) continue;                  /* SIG_IGN */
            if (h == 0) { default_action(c, sig); continue; }
            {
                Reg save[8];
                f80 sst[8];
                u32 stop = c->top; u16 sfcw = c->fcw, sfsw = c->fsw;
                u64 omask = blocked;
                u32 args[3];
                static u8 siginfo[128], uctx[512];
                memcpy(save, c->r, sizeof save);
                memcpy(sst, c->st, sizeof sst);
                blocked |= act[sig].mask;
                if (!(act[sig].flags & 0x40000000u)) blocked |= 1ull << sig;   /* !SA_NODEFER */
                if (act[sig].flags & 0x80000000u) act[sig].handler = 0;        /* SA_RESETHAND */
                args[0] = sig;
                memset(siginfo, 0, sizeof siginfo);
                *(u32 *)siginfo = sig;
                args[1] = (u32)(uintptr_t)siginfo;
                args[2] = (u32)(uintptr_t)uctx;
                c->r[4].d -= 128;   /* red zone, like a kernel signal frame */
                rt_call_guest(c, h, (act[sig].flags & 4) ? 3 : 1, args);
                memcpy(c->r, save, sizeof save);
                memcpy(c->st, sst, sizeof sst);
                c->top = stop; c->fcw = sfcw; c->fsw = sfsw;
                blocked = omask;
            }
        }
    }
}

static u64 now_us(void);

/* sleep up to us microseconds (~0 = forever); returns 1 if a signal for c arrived (caller returns EINTR, delivery
   at next SIGCHK). Timed with the performance counter: GetTickCount only moves in 15.6 ms steps, which turned
   every short usleep/select into a 15.6 ms one and ran the game ~3x slow. */
static int wait_us(CPU *c, u64 us);
/* RT_SLEEPLOG: total wall time spent waiting, per guest call site of the import (bit 31: not the main thread) */
int rt_wait_us(CPU *c, u64 us) {
    static u32 site[64]; static u64 tot[64], cnt[64], last;
    static int on = -1;
    u64 t0; int r, i; u32 s;
    if (on < 0) on = getenv("RT_SLEEPLOG") != 0;
    if (!on) return wait_us(c, us);
    s = (M32(c->r[4].d) & 0x7fffffffu) | (c->is_main ? 0 : 0x80000000u);
    t0 = now_us(); r = wait_us(c, us);
    for (i = 0; i < 64 && site[i] && site[i] != s; i++) ;
    if (i < 64) { site[i] = s; tot[i] += now_us() - t0; cnt[i]++; }
    if (now_us() - last > 5000000) {
        last = now_us(); rt_logf("WAITSTAT\n");
        for (i = 0; i < 64 && site[i]; i++) rt_logf("  %08x %llu ms %llu calls\n", site[i], tot[i] / 1000, cnt[i]);
    }
    return r;
}
/* Per-thread high-resolution waitable timer (Windows 10 1803+). Plain Sleep/WaitForSingleObject timeouts are
   not reliably 1 ms on Windows 11 even with timeBeginPeriod(1): the game's msleep(4) per pen step took ~10.7 ms,
   so the attract drawing ran ~2.5x slow. */
static __thread HANDLE hr_timer;
static __thread int hr_tried;
static HANDLE get_hr_timer(void) {
    if (!hr_tried) {
        typedef HANDLE (WINAPI *CWTEx)(void *, const wchar_t *, DWORD, DWORD);
        CWTEx f = (CWTEx)GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateWaitableTimerExW");
        hr_tried = 1;
        if (f) hr_timer = f(0, 0, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, 0x1F0003 /* TIMER_ALL_ACCESS */);
    }
    return hr_timer;
}

static int wait_us(CPU *c, u64 us) {
    u64 end = us == ~0ull ? ~0ull : now_us() + us;
    int waited = 0;
    HANDLE tm = get_hr_timer();
    for (;;) {
        u64 n, left;
        DWORD ms;
        if (rt_signal_interrupts(c)) return 1;
        n = now_us();
        if (n >= end && (waited || !us)) return 0;
        left = n >= end ? 0 : end - n;
        if (tm) {
            LARGE_INTEGER due;
            HANDLE hs[2];
            if (waited && left < 20) return 0;
            if (left > 50000) left = 50000;
            if (left < 50) left = 50;
            due.QuadPart = -(LONGLONG)(left * 10);
            if (SetWaitableTimer(tm, &due, 0, 0, 0, FALSE)) {
                hs[0] = tm; hs[1] = wake_evt;
                WaitForMultipleObjects(c->is_main ? 2 : 1, hs, FALSE, INFINITE);
                waited = 1;
                continue;
            }
        }
        ms = left >= 50000 ? 50 : (DWORD)(left / 1000);
        if (!ms && !waited) ms = 1;   /* sub-millisecond request: one short wait, like the kernel would */
        else if (!ms) return 0;
        if (c->is_main) WaitForSingleObject(wake_evt, ms);
        else Sleep(ms);
        waited = 1;
    }
}
int rt_wait_ms(CPU *c, DWORD ms) { return rt_wait_us(c, ms == INFINITE ? ~0ull : (u64)ms * 1000); }
u64 rt_now_us(void) { return now_us(); }

void rt_sig_init(void) {
    wake_evt = CreateEventA(0, FALSE, FALSE, 0);
    timeBeginPeriod(1);   /* 1 ms scheduler tick for the whole process */
}

static BOOL WINAPI ctrl_handler(DWORD t) {
    if (t == CTRL_C_EVENT || t == CTRL_BREAK_EVENT) { rt_raise(GSIG_INT); return TRUE; }
    if (t == CTRL_CLOSE_EVENT) { rt_raise(GSIG_TERM); Sleep(2000); return FALSE; }
    return FALSE;
}

/* ---------------- ITIMER_REAL (shared with alarm) */
static CRITICAL_SECTION tm_cs;
static HANDLE tm_evt;
static u64 tm_deadline_us;     /* 0 = disarmed */
static u32 tm_interval_us;

static u64 now_us(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (u64)(t.QuadPart / f.QuadPart) * 1000000ull + (u64)(t.QuadPart % f.QuadPart) * 1000000ull / (u64)f.QuadPart;
}

static DWORD WINAPI timer_thread(void *arg) {
    (void)arg;
    timeBeginPeriod(1);
    for (;;) {
        DWORD wait = INFINITE;
        EnterCriticalSection(&tm_cs);
        if (tm_deadline_us) {
            u64 n = now_us();
            if (n >= tm_deadline_us) {
                rt_raise(GSIG_ALRM);
                if (tm_interval_us) {
                    tm_deadline_us += tm_interval_us;
                    if (tm_deadline_us < n) tm_deadline_us = n + tm_interval_us;
                } else tm_deadline_us = 0;
            }
            if (tm_deadline_us) {
                u64 d = (tm_deadline_us - now_us()) / 1000;
                wait = d > 1000 ? 1000 : (DWORD)d;
            }
        }
        LeaveCriticalSection(&tm_cs);
        WaitForSingleObject(tm_evt, wait);
    }
    return 0;
}

void rt_timers_init(void) {
    InitializeCriticalSection(&tm_cs);
    tm_evt = CreateEventA(0, FALSE, FALSE, 0);
    CreateThread(0, 0, timer_thread, 0, 0, 0);
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
}

u32 rt_set_itimer(u32 value_us, u32 interval_us, u32 *old_value_us, u32 *old_interval_us) {
    EnterCriticalSection(&tm_cs);
    if (old_value_us) {
        u64 n = now_us();
        *old_value_us = tm_deadline_us ? (u32)(tm_deadline_us > n ? tm_deadline_us - n : 1) : 0;
    }
    if (old_interval_us) *old_interval_us = tm_interval_us;
    tm_deadline_us = value_us ? now_us() + value_us : 0;
    tm_interval_us = interval_us;
    LeaveCriticalSection(&tm_cs);
    SetEvent(tm_evt);
    return 0;
}

/* ---------------- imports */
DECL_IMP(signal) {
    int sig = ARG(0);
    u32 old;
    if (sig <= 0 || sig > 64) ERR(22);
    old = act[sig].handler;
    act[sig].handler = ARG(1);
    act[sig].flags = 0x10000000u | 0x80000000u * 0;   /* bsd signal(): SA_RESTART */
    act[sig].mask = 0;
    RET(old);
}

/* glibc struct sigaction (i386): handler, sa_mask[32 words], sa_flags, sa_restorer */
DECL_IMP(sigaction) {
    int sig = ARG(0);
    u32 na = ARG(1), oa = ARG(2);
    if (sig <= 0 || sig > 64) ERR(22);
    if (oa) {
        memset((void *)(uintptr_t)oa, 0, 140);
        M32(oa) = act[sig].handler;
        M32(oa + 4) = (u32)act[sig].mask; M32(oa + 8) = (u32)(act[sig].mask >> 32);
        M32(oa + 132) = act[sig].flags;
    }
    if (na) {
        act[sig].handler = M32(na);
        act[sig].mask = M32(na + 4) | ((u64)M32(na + 8) << 32);
        act[sig].flags = M32(na + 132);
    }
    RET(0);
}

DECL_IMP(sigprocmask) {
    int how = ARG(0);
    u32 set = ARG(1), old = ARG(2);
    u64 m = blocked;
    if (old) { memset((void *)(uintptr_t)old, 0, 128); M32(old) = (u32)m; M32(old + 4) = (u32)(m >> 32); }
    if (set && c->is_main) {
        u64 s = M32(set) | ((u64)M32(set + 4) << 32);
        if (how == 0) m |= s; else if (how == 1) m &= ~s; else m = s;
        rt_sigmask_set(m);
    }
    RET(0);
}

DECL_IMP(sigemptyset) { memset(ARGP(0), 0, 128); RET(0); }
DECL_IMP(sigaddset) {
    int s = ARG(1) - 1;
    if (s < 0 || s >= 1024) ERR(22);
    ((u32 *)ARGP(0))[s >> 5] |= 1u << (s & 31);
    RET(0);
}
DECL_IMP(sigismember) {
    int s = ARG(1) - 1;
    if (s < 0 || s >= 1024) ERR(22);
    RET((((u32 *)ARGP(0))[s >> 5] >> (s & 31)) & 1);
}

DECL_IMP(alarm) {
    u32 ov = 0;
    rt_set_itimer(ARG(0) * 1000000u, 0, &ov, 0);
    RET((ov + 999999) / 1000000);
}

/* setitimer(which, new, old): struct itimerval { timeval interval; timeval value; } */
DECL_IMP(setitimer) {
    u32 nv = ARG(1), ov = ARG(2), v = 0, iv = 0, ovv, oiv;
    if (ARG(0) != 0) ERR(22);
    if (nv) {
        iv = M32(nv) * 1000000u + M32(nv + 4);
        v = M32(nv + 8) * 1000000u + M32(nv + 12);
    }
    rt_set_itimer(v, iv, &ovv, &oiv);
    if (ov) {
        M32(ov) = oiv / 1000000u; M32(ov + 4) = oiv % 1000000u;
        M32(ov + 8) = ovv / 1000000u; M32(ov + 12) = ovv % 1000000u;
    }
    RET(0);
}

DECL_IMP(pause) {
    rt_wait_ms(c, INFINITE);
    ERR(4);
}

/* ---------------- threads (__clone with CLONE_VM) */
typedef struct { u32 fn, stack, arg; HANDLE h; int tid; volatile int live; } GThread;
#define NTHR 16
static GThread thr[NTHR];
static volatile LONG next_tid = 2000;

static DWORD WINAPI thread_main(void *p) {
    GThread *t = (GThread *)p;
    CPU *c = rt_new_cpu();
    u32 a = t->arg;
    rt_fpu_init();
    c->tid = t->tid;
    c->r[4].d = t->stack;
    rt_logf("thread %d start fn=%08x\n", t->tid, t->fn);
    rt_call_guest(c, t->fn, 1, &a);
    rt_logf("thread %d returned %d\n", t->tid, c->r[0].d);
    t->live = 0;
    return 0;
}

void rt_thread_exit(CPU *c, int code) {
    int i;
    for (i = 0; i < NTHR; i++)
        if (thr[i].live && thr[i].tid == c->tid) { thr[i].live = 0; break; }
    rt_logf("thread %d _exit(%d)\n", c->tid, code);
    ExitThread(code);
}

int rt_is_thread(int pid) {
    int i;
    for (i = 0; i < NTHR; i++) if (thr[i].live && thr[i].tid == pid) return i + 1;
    return 0;
}

int rt_kill_thread(int pid, int sig) {
    int k = rt_is_thread(pid);
    if (!k) return -1;
    if (sig == 0) return 0;
    if (sig == GSIG_KILL || sig == GSIG_TERM || sig == 1 || sig == 2) {
        rt_logf("killing thread %d (sig %d)\n", pid, sig);
        TerminateThread(thr[k - 1].h, 0);
        thr[k - 1].live = 0;
    }
    return 0;
}

DECL_IMP(__clone) {
    int i;
    GThread *t = 0;
    for (i = 0; i < NTHR; i++) if (!thr[i].live) { t = &thr[i]; break; }
    if (!t) ERR(11);
    t->fn = ARG(0); t->stack = ARG(1); t->arg = ARG(3);
    t->tid = (int)InterlockedIncrement(&next_tid);
    t->live = 1;
    rt_logf("__clone fn=%08x stack=%08x flags=%x arg=%08x -> tid %d\n", t->fn, t->stack, ARG(2), t->arg, t->tid);
    t->h = CreateThread(0, 1 << 20, thread_main, t, 0, 0);
    RET(t->tid);
}
