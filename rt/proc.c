/* Child processes. The game runs its helper programs (tgascale, voronoi, ...) with
 * vfork()+execvp() inside run_in_child(). Those helpers are recompiled to <name>.exe next to
 * their ELF in /vend/bin, so a guest exec becomes CreateProcess of the matching .exe. */
#include "rt.h"
#include <ctype.h>
#include <math.h>

#define MAXCHILD 64
static struct { int pid; HANDLE h; } kids[MAXCHILD];
static CRITICAL_SECTION kcs;
static int kinit;

static void kids_init(void) {
    if (kinit) return;
    kinit = 1;
    InitializeCriticalSection(&kcs);
    SetEnvironmentVariableA("VEND_ROOT", rt_root);   /* children find the same data root */
}

static int file_exists(const char *p) {
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* guest program name -> host .exe path; 0 ok */
static int resolve(const char *file, int search, char *out, size_t n) {
    static const char *dirs[] = { ".", "/vend/bin", "/bin", "/usr/bin", 0 };
    char g[512], h[MAX_PATH];
    int i;
    if (strchr(file, '/') || !search) {
        if (rt_map_path(file, h, sizeof h)) return -1;
        snprintf(out, n, "%s.exe", h);
        return file_exists(out) ? 0 : -1;
    }
    for (i = 0; dirs[i]; i++) {
        snprintf(g, sizeof g, "%s/%s", dirs[i], file);
        if (rt_map_path(g, h, sizeof h)) continue;
        snprintf(out, n, "%s.exe", h);
        if (file_exists(out)) return 0;
    }
    return -1;
}

/* MSVCRT command-line quoting */
static void append_arg(char *cl, size_t n, const char *a) {
    size_t L = strlen(cl);
    int quote = !*a || strpbrk(a, " \t\"") != 0, bs = 0;
    if (L && L + 1 < n) cl[L++] = ' ';
    if (quote && L + 1 < n) cl[L++] = '"';
    for (; *a && L + 4 < n; a++) {
        if (*a == '\\') { bs++; cl[L++] = '\\'; continue; }
        if (*a == '"') { while (bs-- >= 0 && L + 2 < n) cl[L++] = '\\'; }
        bs = 0;
        cl[L++] = *a;
    }
    if (quote) { while (bs-- > 0 && L + 2 < n) cl[L++] = '\\'; if (L + 1 < n) cl[L++] = '"'; }
    cl[L] = 0;
}

/* returns pid, or -1 (errno ENOENT) */
static int spawn(const char *file, u32 gargv, int search) {
    char exe[MAX_PATH], cl[8192];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    int i;
    kids_init();
    if (resolve(file, search, exe, sizeof exe)) {
        rt_logf("exec %s: no recompiled program\n", file);
        return -1;
    }
    cl[0] = 0;
    append_arg(cl, sizeof cl, exe);
    for (i = 1; gargv && M32(gargv + 4 * i); i++) append_arg(cl, sizeof cl, (const char *)(uintptr_t)M32(gargv + 4 * i));
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    fflush(0);
    if (!CreateProcessA(exe, cl, 0, 0, TRUE, 0, 0, 0, &si, &pi)) {
        rt_logf("exec %s: CreateProcess failed (%lu)\n", exe, GetLastError());
        return -1;
    }
    CloseHandle(pi.hThread);
    rt_logf("exec %s -> pid %lu\n", cl, pi.dwProcessId);
    EnterCriticalSection(&kcs);
    for (i = 0; i < MAXCHILD; i++)
        if (!kids[i].h) { kids[i].pid = (int)pi.dwProcessId; kids[i].h = pi.hProcess; break; }
    LeaveCriticalSection(&kcs);
    if (i == MAXCHILD) CloseHandle(pi.hProcess);
    return (int)pi.dwProcessId;
}

/* a non-process child (snd.c): h is signalled when it is over; the table closes h when reaped. 0 ok */
int rt_kid_add(int pid, HANDLE h) {
    int i;
    kids_init();
    EnterCriticalSection(&kcs);
    for (i = 0; i < MAXCHILD; i++)
        if (!kids[i].h) { kids[i].pid = pid; kids[i].h = h; break; }
    LeaveCriticalSection(&kcs);
    return i == MAXCHILD ? -1 : 0;
}

/* wait for pid (-1: any). returns pid, 0 (WNOHANG, still running) or -1 (no such child) */
static int reap(CPU *c, int pid, int nohang, int *code) {
    for (;;) {
        int i, found = 0;
        EnterCriticalSection(&kcs);
        for (i = 0; i < MAXCHILD; i++) {
            DWORD ec;
            if (!kids[i].h || (pid > 0 && kids[i].pid != pid)) continue;
            found = 1;
            if (WaitForSingleObject(kids[i].h, 0) == WAIT_OBJECT_0) {
                int p = kids[i].pid;
                if (!GetExitCodeProcess(kids[i].h, &ec)) ec = 0;   /* sound child: an event */
                CloseHandle(kids[i].h);
                kids[i].h = 0;
                LeaveCriticalSection(&kcs);
                *code = (int)ec;
                return p;
            }
        }
        LeaveCriticalSection(&kcs);
        if (!found) return -1;
        if (nohang) return 0;
        if (rt_wait_ms(c, 5)) return -2;   /* interrupted by a signal */
    }
}

/* Linux wait status from a Windows exit code */
static u32 wstatus(int code) {
    if ((u32)code >= 0xC0000000u) return 11;   /* crashed: "killed by SIGSEGV" */
    return ((u32)code & 0xff) << 8;
}

DECL_IMP(waitpid) {
    int pid = (s32)ARG(0), code = 0, r;
    u32 st = ARG(1), opt = ARG(2);
    if (!kinit) ERR(10);
    r = reap(c, pid, opt & 1, &code);
    if (r == -1) ERR(10);
    if (r == -2) ERR(4);
    if (r > 0 && st) M32(st) = wstatus(code);
    RET(r);
}
DECL_IMP(wait) {
    int code = 0, r;
    u32 st = ARG(0);
    if (!kinit) ERR(10);
    r = reap(c, -1, 0, &code);
    if (r == -1) ERR(10);
    if (r == -2) ERR(4);
    if (st) M32(st) = wstatus(code);
    RET(r);
}

static const char *plt_name(u32 a) {
    int i;
    for (i = 0; i < guest_got_n; i++)
        if (guest_got[i].stub == a) return guest_got[i].name;
    return 0;
}

/* run_in_child(mode, fn, a, b): mode 0 = run fn(a,b) in a child and wait (returns exit code),
 * 1 = run it in the background (returns 0), 2 = run fn(a,b) in this process (an exec replaces us) */
void ovr_run_child(CPU *c) {
    int mode = (int)ARG(0), pid, code = 0, r;
    u32 fn = ARG(1), a = ARG(2), b = ARG(3);
    const char *name = plt_name(fn);
    int search;
    if (!name || (strcmp(name, "execvp") && strcmp(name, "execv"))) {
        rt_logf("run_in_child: unsupported child function %08x (%s)\n", fn, name ? name : "?");
        RET(mode == 1 ? 0 : -1);
    }
    search = !strcmp(name, "execvp");
    pid = spawn((const char *)(uintptr_t)a, b, search);
    if (mode == 1) RET(0);
    if (pid < 0) {
        if (mode == 2) RET(-1);
        RET(255);   /* child: exec failed -> _exit(-1) */
    }
    while ((r = reap(c, pid, 0, &code)) == -2) rt_deliver(c);
    if (mode == 2) rt_exit(c, code);
    if ((u32)code >= 0xC0000000u) {
        rt_logf("child %d crashed (%08x)\n", pid, (u32)code);
        RET(-2);
    }
    RET(code & 0xff);
}
DECL_IMP(__gmon_start__) { RET_VOID(); }

/* qsort with a guest comparator: merge sort (stable, O(n log n) guest calls) */
static void msort(CPU *c, u8 *b, u8 *tmp, u32 n, u32 sz, u32 cmp) {
    u32 h, i, j, k;
    if (n < 2) return;
    h = n / 2;
    msort(c, b, tmp, h, sz, cmp);
    msort(c, b + h * sz, tmp, n - h, sz, cmp);
    i = 0; j = h; k = 0;
    while (i < h && j < n) {
        u32 args[2] = { (u32)(uintptr_t)(b + i * sz), (u32)(uintptr_t)(b + j * sz) };
        rt_call_guest(c, cmp, 2, args);
        if ((s32)c->r[0].d <= 0) memcpy(tmp + k++ * sz, b + i++ * sz, sz);
        else memcpy(tmp + k++ * sz, b + j++ * sz, sz);
    }
    while (i < h) memcpy(tmp + k++ * sz, b + i++ * sz, sz);
    while (j < n) memcpy(tmp + k++ * sz, b + j++ * sz, sz);
    memcpy(b, tmp, (size_t)n * sz);
}
DECL_IMP(qsort) {
    u8 *b = (u8 *)ARGP(0);
    u32 n = ARG(1), sz = ARG(2), cmp = ARG(3);
    u8 *tmp = (u8 *)malloc((size_t)n * sz + 1);
    msort(c, b, tmp, n, sz, cmp);
    free(tmp);
    RET_VOID();
}

/* ---------------- odds and ends the helper programs import */
DECL_IMP(fileno) {
    FILE *f = (FILE *)ARGP(0);
    int h = _fileno(f), i;
    if (f == stdin) RET(0);
    if (f == stdout) RET(1);
    if (f == stderr) RET(2);
    for (i = 3; i < MAXFD; i++)
        if (rt_fds[i].kind == FD_HOST && rt_fds[i].hfd == h) RET(i);
    i = rt_fd_alloc(3);
    if (i < 0) ERR(24);
    rt_fds[i].kind = FD_HOST; rt_fds[i].hfd = h; rt_fds[i].flags = 2;
    RET(i);
}
DECL_IMP(tmpfile) {
    static volatile LONG seq;
    char p[MAX_PATH];
    snprintf(p, sizeof p, "tmp\tf%lu_%ld.tmp", GetCurrentProcessId(), InterlockedIncrement(&seq));
    RET(fopen(p, "w+bD"));
}
DECL_IMP(scanf) {
    extern int rt_fwd_scanf(FILE *f, const char *str, const char *fmt0, const u32 *a);
    RET(rt_fwd_scanf(stdin, 0, ARGS(0), (const u32 *)(uintptr_t)(c->r[4].d + 8)));
}
static double argd(CPU *c, int i) { double d; memcpy(&d, (void *)(uintptr_t)(c->r[4].d + 4 + 4 * i), 8); return d; }
DECL_IMP(pow) { FPUSH((long double)pow(argd(c, 0), argd(c, 2))); RET_VOID(); }
DECL_IMP(sqrt) { FPUSH((long double)sqrt(argd(c, 0))); RET_VOID(); }
