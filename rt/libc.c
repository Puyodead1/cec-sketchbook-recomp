/* glibc 2.4 (i386) imports implemented on msvcrt/Win32.  Guest pointers == host pointers. */
#include "rt.h"
#include <io.h>
#include <fcntl.h>
#include <errno.h>
#include <direct.h>
#include <time.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <ctype.h>

int __mingw_vfprintf(FILE *, const char *, va_list);
int __mingw_vsnprintf(char *, size_t, const char *, va_list);

#define GVA(i) ((va_list)(uintptr_t)(c->r[4].d + 4 + 4 * (i)))

/* ---------------- errno */
int rt_errno_from_host(int e) {
    switch (e) {
    case 36: return 35;   /* EDEADLK */
    case 38: return 36;   /* ENAMETOOLONG */
    case 39: return 37;   /* ENOLCK */
    case 40: return 38;   /* ENOSYS */
    case 41: return 39;   /* ENOTEMPTY */
    default: return e;
    }
}
#define HERR() do { c->err = rt_errno_from_host(errno); RET(-1); } while (0)

/* RT_FILELOG=1: log every guest file open (debugging missing assets) */
static int filelog(void) { static int on = -1; if (on < 0) on = getenv("RT_FILELOG") != 0; return on; }

DECL_IMP(__errno_location) { RET(&c->err); }

/* ---------------- process start / exit */
typedef struct { u32 fn, arg; } AtExit;
static AtExit atexits[64];
static int natexit;
static volatile int exiting;

void rt_thread_exit(CPU *c, int code);

void rt_exit(CPU *c, int code) {
    if (!exiting) {
        exiting = 1;
        while (natexit > 0) {
            AtExit a = atexits[--natexit];
            rt_call_guest(c, a.fn, 1, &a.arg);
        }
    }
    fflush(0);
    rt_logf("exit(%d) from guest fn %08x\n", code, c->cur);
    ExitProcess(code);
}

DECL_IMP(__cxa_atexit) {
    if (natexit < 64) { atexits[natexit].fn = ARG(0); atexits[natexit].arg = ARG(1); natexit++; }
    RET(0);
}

DECL_IMP(exit) { rt_exit(c, (int)ARG(0)); }
DECL_IMP(_exit) {
    if (!c->is_main) rt_thread_exit(c, ARG(0));
    fflush(0);
    rt_logf("_exit(%d) from guest fn %08x\n", ARG(0), c->cur);
    ExitProcess(ARG(0));
}

DECL_IMP(__libc_start_main) {
    u32 gmain = ARG(0), argc = ARG(1), argv = ARG(2), init = ARG(3);
    u32 args[3];
    args[0] = argc; args[1] = argv; args[2] = argv + 4 * (argc + 1);
    if (guest_is_game) ovr_init(c);
    if (init) rt_call_guest(c, init, 3, args);
    rt_logf("calling main(%d)\n", argc);
    rt_call_guest(c, gmain, 3, args);
    rt_exit(c, (int)c->r[0].d);
}

/* ---------------- environment */
static char *envtab[64] = { "HOME=/vend", "TMPDIR=/vend/tmp", "PATH=.:/vend/bin:/bin:/usr/bin:/usr/X11R6/bin",
                            "DISPLAY=:0", "TERM=linux", "LANG=C", 0 };
static char *env_get(const char *n) {
    size_t L = strlen(n);
    int i;
    for (i = 0; envtab[i]; i++) if (!strncmp(envtab[i], n, L) && envtab[i][L] == '=') return envtab[i] + L + 1;
    return 0;
}
DECL_IMP(getenv) { RET(env_get(ARGS(0))); }
DECL_IMP(setenv) {
    const char *n = ARGS(0), *v = ARGS(1);
    size_t L = strlen(n);
    int i;
    char *s;
    for (i = 0; envtab[i]; i++) if (!strncmp(envtab[i], n, L) && envtab[i][L] == '=') break;
    if (envtab[i] && !ARG(2)) RET(0);
    if (i >= 63) ERR(12);
    s = malloc(L + strlen(v) + 2);
    sprintf(s, "%s=%s", n, v);
    envtab[i] = s;
    RET(0);
}

/* ---------------- memory */
/* Guest heap. Guest blocks share the CRT heap with the runtime (host code frees some of them, e.g. XImage data),
 * so there is no header in front of a block. Instead every block gets PAD spare bytes behind it, as glibc's
 * rounding and chunk slack give it on the real machine: the original code writes a little past some buffers,
 * which on Windows tore up CRT heap/locale data and crashed later in unrelated places. The pad is filled with a
 * canary and blocks are tracked in a side table, so when the guest frees or reallocs a block that was written
 * past its end, rt.log names the code that allocated it. */
#define PAD 64
#define HT_BITS 19
#define HT_N (1u << HT_BITS)
typedef struct { u32 p, n, site; } HEnt;
static HEnt *ht;
static volatile LONG ht_lock;
static u32 ht_live;
static void hlock(void) { while (InterlockedExchange(&ht_lock, 1)) Sleep(0); }
static void hunlock(void) { InterlockedExchange(&ht_lock, 0); }
static u32 hslot(u32 p) { return (p * 2654435761u) >> (32 - HT_BITS); }
static void canary_set(u8 *p, u32 n) { memset(p + n, 0xFD, PAD); }
static int canary_bad(const HEnt *e) {
    const u8 *q = (const u8 *)(uintptr_t)e->p + e->n;
    int i;
    for (i = 0; i < PAD; i++) if (q[i] != 0xFD) return i + 1;
    return 0;
}
static void canary_report(HEnt *e, int at, CPU *c, const char *when) {
    rt_logf("heap: guest block %08x size %u (allocated from %08x) overrun by >=%d bytes, seen at %s in guest fn %08x\n",
            e->p, e->n, e->site, at, when, c ? c->cur : 0);
}
static void ht_add(u32 p, u32 n, u32 site) {
    u32 i;
    if (!ht) ht = (HEnt *)calloc(HT_N, sizeof(HEnt));
    if (!ht || ht_live > HT_N - HT_N / 8) return;
    for (i = hslot(p); ht[i].p && ht[i].p != p; i = (i + 1) & (HT_N - 1)) ;
    if (!ht[i].p) ht_live++;
    ht[i].p = p; ht[i].n = n; ht[i].site = site;
}
static HEnt *ht_find(u32 p) {
    u32 i;
    if (!ht || !p) return 0;
    for (i = hslot(p); ht[i].p; i = (i + 1) & (HT_N - 1)) if (ht[i].p == p) return &ht[i];
    return 0;
}
static void ht_del(HEnt *e) {   /* backward-shift deletion */
    u32 i = (u32)(e - ht), j = i, k;
    for (;;) {
        j = (j + 1) & (HT_N - 1);
        if (!ht[j].p) break;
        k = hslot(ht[j].p);
        if ((j > i && (k <= i || k > j)) || (j < i && (k <= i && k > j))) { ht[i] = ht[j]; i = j; }
    }
    ht[i].p = 0; ht_live--;
}
/* RT_GUARDHEAP=1 (debugging): guest blocks of 1 KB and up end flush against a no-access page, so a write past
 * the end faults right there and the crash report names the guest function doing it. */
static int guardheap(void) { static int on = -1; if (on < 0) on = getenv("RT_GUARDHEAP") != 0; return on; }
#define GUARD_MIN 1024
#define GSLACK 32   /* bytes between block end and guard page: the original reads (not writes) a few bytes past some
                       buffers (e.g. 08089e0f's signature compare). Writes into the slack are caught by its canary. */
static u32 gend(u32 n) { return ((n + 7) & ~7u) + GSLACK; }
static u8 *guard_alloc(u32 n) {
    u32 body = (gend(n) + 4095) & ~4095u;
    u8 *base = (u8 *)VirtualAlloc(0, body + 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE), *p;
    DWORD old;
    if (!base) return 0;
    VirtualProtect(base + body, 4096, PAGE_NOACCESS, &old);
    p = base + body - gend(n);
    memset(p + n, 0xFD, gend(n) - n);
    return p;
}
static int is_guard(u32 p, u32 n) { return guardheap() && n >= GUARD_MIN && (((p + gend(n)) & 4095) == 0); }
static int guard_canary_bad(const HEnt *e) {
    const u8 *q = (const u8 *)(uintptr_t)e->p;
    u32 i;
    for (i = e->n; i < gend(e->n); i++) if (q[i] != 0xFD) return (int)(i - e->n) + 1;
    return 0;
}

static void *galloc(CPU *c, u32 n, int zero) {
    u8 *p;
    if (n > 0x7ff00000u) return 0;   /* e.g. malloc(lseek(...) == -1): NULL, as glibc; n + PAD must not wrap */
    if (guardheap() && n >= GUARD_MIN) {
        if (!(p = guard_alloc(n))) return 0;   /* VirtualAlloc memory is already zeroed (block body) */
    } else {
        p = zero ? (u8 *)calloc(1, n + PAD) : (u8 *)malloc(n + PAD);
        if (!p) return 0;
        canary_set(p, n);
    }
    hlock();
    ht_add((u32)(uintptr_t)p, n, M32(c->r[4].d));
    hunlock();
    return p;
}
/* drop a guest block from the table (checking its canary); returns its size, or -1 if not a tracked block */
static s64 gcheck_drop(CPU *c, void *p, const char *when, int *guard) {
    HEnt *e;
    int at;
    s64 n = -1;
    *guard = 0;
    hlock();
    if ((e = ht_find((u32)(uintptr_t)p))) {
        n = e->n;
        if (is_guard(e->p, e->n)) { *guard = 1; if ((at = guard_canary_bad(e))) canary_report(e, at, c, when); }
        else if ((at = canary_bad(e))) canary_report(e, at, c, when);
        ht_del(e);
    }
    hunlock();
    return n;
}
/* free a pointer the guest owns (also used by XFree) */
void rt_gfree(CPU *c, void *p) {
    int guard;
    if (!p) return;
    gcheck_drop(c, p, "free", &guard);
    if (guard) {
        MEMORY_BASIC_INFORMATION mi;
        VirtualQuery(p, &mi, sizeof mi);
        VirtualFree(mi.AllocationBase, 0, MEM_RELEASE);
    } else free(p);
}
DECL_IMP(malloc) { RET(galloc(c, ARG(0) ? ARG(0) : 1, 0)); }
DECL_IMP(calloc) { u64 n = (u64)ARG(0) * ARG(1); RET(n > 0x7fffffff ? 0 : galloc(c, n ? (u32)n : 1, 1)); }
DECL_IMP(realloc) {
    void *o = ARGP(0), *p;
    u32 n = ARG(1) ? ARG(1) : 1;
    int guard;
    s64 old;
    if (!o) RET(galloc(c, n, 0));
    if (n > 0x7ff00000u) RET(0);
    if (guardheap()) {   /* debug mode: always move, so either side can be a guard block */
        hlock(); { HEnt *e = ht_find((u32)(uintptr_t)o); old = e ? (s64)e->n : -1; } hunlock();
        if (old >= 0) {
            if (!(p = galloc(c, n, 0))) RET(0);
            memcpy(p, o, (size_t)(old < n ? old : n));
            rt_gfree(c, o);
            RET(p);
        }
    }
    gcheck_drop(c, o, "realloc", &guard);
    p = realloc(o, n + PAD);
    if (!p) RET(0);
    canary_set((u8 *)p, n);
    hlock(); ht_add((u32)(uintptr_t)p, n, M32(c->r[4].d)); hunlock();
    RET(p);
}
DECL_IMP(free) { rt_gfree(c, ARGP(0)); RET_VOID(); }

/* The game's malloc wrapper (0x809F240). At startup the game allocates the two buffers that save the top and bottom
 * 85-row banners of the screen (0x80B814C/0x80B8150) as width*bpp*85 for the video mode current at the time
 * (640x480). The print stage switches to 800x600 and then saves the banners at 800 px per row: 27 KB past the end of
 * each buffer, which wrecked the Windows heap and crashed the game shortly after "printing". Those two get room for
 * any mode (up to 1600 px at 32 bpp); every other allocation is unchanged. */
void ovr_gmalloc(CPU *c) {
    u32 n = ARG(0), ret = M32(c->r[4].d);
    if ((ret == 0x804F636 || ret == 0x804F65B) && n < 1600u * 4 * 85) n = 1600u * 4 * 85;
    RET(galloc(c, n ? n : 1, 0));
}

typedef struct { u32 base, len; int va; } MapEnt;
static MapEnt maps[64];
DECL_IMP(mmap) {
    u32 len = ARG(1), flags = ARG(3);
    int fd = (int)ARG(4);
    u32 off = ARG(5);
    void *p;
    int i;
    if (flags & 0x20) {   /* MAP_ANONYMOUS */
        p = VirtualAlloc(0, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!p) ERR(12);
    } else {
        if (fd < 0 || fd >= MAXFD || rt_fds[fd].kind != FD_HOST) ERR(19);
        p = VirtualAlloc(0, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!p) ERR(12);
        _lseek(rt_fds[fd].hfd, off, SEEK_SET);
        _read(rt_fds[fd].hfd, p, len);
    }
    for (i = 0; i < 64; i++) if (!maps[i].base) { maps[i].base = (u32)(uintptr_t)p; maps[i].len = len; break; }
    RET(p);
}
DECL_IMP(munmap) {
    int i;
    for (i = 0; i < 64; i++) if (maps[i].base == ARG(0)) { VirtualFree(ARGP(0), 0, MEM_RELEASE); maps[i].base = 0; break; }
    RET(0);
}
DECL_IMP(mprotect) { RET(0); }
DECL_IMP(getpagesize) { RET(4096); }
DECL_IMP(sysconf) {
    switch (ARG(0)) {
    case 2: RET(100);      /* _SC_CLK_TCK */
    case 30: RET(4096);    /* _SC_PAGESIZE */
    case 4: RET(1024);     /* _SC_OPEN_MAX */
    case 84: RET(1);       /* _SC_NPROCESSORS_ONLN */
    default: ERR(22);
    }
}

/* ---------------- strings */
DECL_IMP(memcpy) { RET(memcpy(ARGP(0), ARGP(1), ARG(2))); }
DECL_IMP(memmove) { RET(memmove(ARGP(0), ARGP(1), ARG(2))); }
DECL_IMP(memset) { RET(memset(ARGP(0), (int)ARG(1), ARG(2))); }
DECL_IMP(strcat) { RET(strcat(ARGS(0), ARGS(1))); }
DECL_IMP(strchr) { RET(strchr(ARGS(0), (int)ARG(1))); }
DECL_IMP(strrchr) { RET(strrchr(ARGS(0), (int)ARG(1))); }
DECL_IMP(strcmp) { RET(strcmp(ARGS(0), ARGS(1))); }
DECL_IMP(strncmp) { RET(strncmp(ARGS(0), ARGS(1), ARG(2))); }
DECL_IMP(strcpy) { RET(strcpy(ARGS(0), ARGS(1))); }
DECL_IMP(strncpy) { RET(strncpy(ARGS(0), ARGS(1), ARG(2))); }
DECL_IMP(strcspn) { RET(strcspn(ARGS(0), ARGS(1))); }
DECL_IMP(strspn) { RET(strspn(ARGS(0), ARGS(1))); }
DECL_IMP(strpbrk) { RET(strpbrk(ARGS(0), ARGS(1))); }
DECL_IMP(strstr) { RET(strstr(ARGS(0), ARGS(1))); }
DECL_IMP(strcasecmp) { RET(_stricmp(ARGS(0), ARGS(1))); }
DECL_IMP(strncasecmp) { RET(_strnicmp(ARGS(0), ARGS(1), ARG(2))); }
DECL_IMP(atoi) { RET(atoi(ARGS(0))); }
DECL_IMP(__strtol_internal) { RET(strtol(ARGS(0), (char **)ARGP(1), (int)ARG(2))); }
DECL_IMP(__strtoul_internal) { RET(strtoul(ARGS(0), (char **)ARGP(1), (int)ARG(2))); }
DECL_IMP(__strtod_internal) {
    double d = strtod(ARGS(0), (char **)ARGP(1));
    FPUSH((f80)d);
    RET_VOID();
}
static const char *lin_strerror(int e) {
    switch (e) {
    case 35: return "Resource deadlock avoided";
    case 36: return "File name too long";
    case 37: return "No locks available";
    case 38: return "Function not implemented";
    case 39: return "Directory not empty";
    case 110: return "Connection timed out";
    case 111: return "Connection refused";
    default: return e < 35 ? strerror(e) : "Unknown error";
    }
}
DECL_IMP(strerror) { RET(lin_strerror((int)ARG(0))); }
DECL_IMP(perror) {
    const char *s = ARGS(0);
    if (s && *s) fprintf(stderr, "%s: %s\n", s, lin_strerror(c->err));
    else fprintf(stderr, "%s\n", lin_strerror(c->err));
    RET_VOID();
}

/* glibc random(): TYPE_3 additive feedback generator */
static s32 rtab[34 + 310];
static int rf, rr;
static s32 rstate[31];
static void g_srandom(u32 seed) {
    int i;
    s32 word;
    if (seed == 0) seed = 1;
    rstate[0] = seed;
    word = seed;
    for (i = 1; i < 31; i++) {
        s32 hi = word / 127773, lo = word % 127773;
        word = 16807 * lo - 2836 * hi;
        if (word < 0) word += 2147483647;
        rstate[i] = word;
    }
    rf = 3; rr = 0;
    for (i = 0; i < 310; i++) {
        rstate[rf] += rstate[rr];
        if (++rf >= 31) rf = 0;
        if (++rr >= 31) rr = 0;
    }
    (void)rtab;
}
static s32 g_random(void) {
    static int inited;
    s32 r;
    if (!inited) { inited = 1; g_srandom(1); }
    rstate[rf] += rstate[rr];
    r = (u32)rstate[rf] >> 1;
    if (++rf >= 31) rf = 0;
    if (++rr >= 31) rr = 0;
    return r;
}
DECL_IMP(srand) { g_srandom(ARG(0)); RET_VOID(); }
DECL_IMP(rand) { RET(g_random()); }
DECL_IMP(random) { RET(g_random()); }

/* ---------------- ctype (glibc tables, indexable -128..255) */
static u16 ctype_b[384];
static s32 ctype_lo[384], ctype_up[384];
static u32 p_ctype_b, p_ctype_lo, p_ctype_up;
static void ctype_init(void) {
    int i;
    for (i = -128; i < 256; i++) {
        int ch = i & 0xff, k = i + 128;
        u16 m = 0;
        if (ch < 128) {
            if (isupper(ch)) m |= 0x100;
            if (islower(ch)) m |= 0x200;
            if (isalpha(ch)) m |= 0x400;
            if (isdigit(ch)) m |= 0x800;
            if (isxdigit(ch)) m |= 0x1000;
            if (isspace(ch)) m |= 0x2000;
            if (isprint(ch)) m |= 0x4000;
            if (isgraph(ch)) m |= 0x8000;
            if (ch == ' ' || ch == '\t') m |= 0x1;
            if (iscntrl(ch)) m |= 0x2;
            if (ispunct(ch)) m |= 0x4;
            if (isalnum(ch)) m |= 0x8;
        }
        ctype_b[k] = i == -1 ? 0 : m;
        ctype_lo[k] = (ch < 128 && i >= 0) ? tolower(ch) : i;
        ctype_up[k] = (ch < 128 && i >= 0) ? toupper(ch) : i;
    }
    p_ctype_b = (u32)(uintptr_t)(ctype_b + 128);
    p_ctype_lo = (u32)(uintptr_t)(ctype_lo + 128);
    p_ctype_up = (u32)(uintptr_t)(ctype_up + 128);
}
DECL_IMP(__ctype_b_loc) { RET(&p_ctype_b); }
DECL_IMP(__ctype_tolower_loc) { RET(&p_ctype_lo); }
DECL_IMP(__ctype_toupper_loc) { RET(&p_ctype_up); }

/* ---------------- paths */
int rt_map_path(const char *g, char *h, size_t n) {
    char *p;
    if (!g) return -1;
    if (!strncmp(g, "/vend/", 6)) { g += 6; while (*g == '/') g++; }   /* "/vend//lang" is /vend/lang */
    else if (!strcmp(g, "/vend")) g = ".";
    else if (g[0] == '/') {
        if (!strncmp(g, "/dev/", 5) || !strncmp(g, "/proc/", 6) || !strncmp(g, "/sys/", 5)) return -1;
        snprintf(h, n, "%s\\_sys%s", rt_root, g);
        goto fix;
    }
    snprintf(h, n, "%s", *g ? g : ".");
fix:
    for (p = h; *p; p++) if (*p == '/') *p = '\\';
    /* collapse doubled separators (game uses "/vend/tmp//x") */
    {
        char *r = h, *w = h;
        while (*r) { *w = *r; if (*r == '\\') while (r[1] == '\\') r++; r++; w++; }
        *w = 0;
    }
    return 0;
}

/* ---------------- fds */
GFd rt_fds[MAXFD];
static u8 termios_store[MAXFD][60];

int rt_fd_alloc(int from) {
    int i;
    for (i = from; i < MAXFD; i++) if (rt_fds[i].kind == FD_FREE) return i;
    return -1;
}

static int gfd_ok(int fd) { return fd >= 0 && fd < MAXFD && rt_fds[fd].kind != FD_FREE; }

DECL_IMP(open) {
    const char *path = ARGS(0);
    u32 fl = ARG(1);
    int mode = (int)ARG(2), hfl = _O_BINARY, fd, h;
    char hp[MAX_PATH];
    fd = rt_fd_alloc(0);
    if (fd < 0) ERR(24);
    if (!strcmp(path, "/dev/uib")) {
        rt_fds[fd].kind = FD_UIB; rt_fds[fd].flags = fl;
        rt_logf("open /dev/uib -> %d\n", fd);
        RET(fd);
    }
    if (!strcmp(path, "/dev/null") || !strcmp(path, "/dev/zero")) { rt_fds[fd].kind = FD_NULL; RET(fd); }
    if (!strcmp(path, "/dev/tty") || !strcmp(path, "/dev/console") || (!strncmp(path, "/dev/tty", 8) && (!path[8] || isdigit((u8)path[8])))) { rt_fds[fd].kind = FD_TTY; RET(fd); }
    if (rt_map_path(path, hp, sizeof hp)) { rt_logf("open %s: ENOENT (unmapped)\n", path); ERR(2); }
    switch (fl & 3) { case 0: hfl |= _O_RDONLY; break; case 1: hfl |= _O_WRONLY; break; default: hfl |= _O_RDWR; }
    if (fl & 0x40) hfl |= _O_CREAT;
    if ((fl & 0x80) && (fl & 0x40)) hfl |= _O_EXCL;
    if (fl & 0x200) hfl |= _O_TRUNC;
    if (fl & 0x400) hfl |= _O_APPEND;
    h = _open(hp, hfl, _S_IREAD | _S_IWRITE);
    (void)mode;
    if (h < 0) {
        int e = errno;
        /* opening a directory read-only is legal on Linux */
        DWORD a = GetFileAttributesA(hp);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && (fl & 3) == 0) { rt_fds[fd].kind = FD_NULL; RET(fd); }
        if (filelog()) rt_logf("FILE open %s: FAIL\n", path);
        c->err = rt_errno_from_host(e);
        RET(-1);
    }
    if (filelog()) rt_logf("FILE open %s\n", path);
    rt_fds[fd].kind = FD_HOST; rt_fds[fd].hfd = h; rt_fds[fd].flags = fl;
    RET(fd);
}

DECL_IMP(close) {
    int fd = (int)ARG(0);
    if (!gfd_ok(fd)) ERR(9);
    if (rt_fds[fd].kind == FD_HOST && fd > 2) _close(rt_fds[fd].hfd);
    rt_fds[fd].kind = FD_FREE;
    RET(0);
}

DECL_IMP(read) {
    int fd = (int)ARG(0), r;
    u32 n = ARG(2);
    if (!gfd_ok(fd)) ERR(9);
    switch (rt_fds[fd].kind) {
    case FD_HOST:
        if (fd == 0) {
            /* the game polls its controlling tty (raw, VMIN=0): never block on our stdin */
            HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
            DWORD t = GetFileType(h), avail = 0;
            if (t == FILE_TYPE_CHAR) RET(0);
            if (t == FILE_TYPE_PIPE) {
                if (!PeekNamedPipe(h, 0, 0, 0, &avail, 0) || !avail) RET(0);
                if (n > avail) n = avail;
            }
        }
        r = _read(rt_fds[fd].hfd, ARGP(1), n); if (r < 0) HERR(); RET(r);
    case FD_UIB: RET(uib_read((u8 *)ARGP(1), n));
    case FD_NULL: RET(0);
    default: RET(0);
    }
}

DECL_IMP(write) {
    int fd = (int)ARG(0), r;
    u32 n = ARG(2);
    if (!gfd_ok(fd)) ERR(9);
    switch (rt_fds[fd].kind) {
    case FD_HOST:
        r = _write(rt_fds[fd].hfd, ARGP(1), n);
        if (r < 0) HERR();
        if (fd <= 2) rt_logf("[fd%d] %.*s", fd, (int)n, ARGS(1));
        RET(r);
    case FD_UIB: RET(uib_write((const u8 *)ARGP(1), n));
    default: RET(n);
    }
}

DECL_IMP(lseek) {
    int fd = (int)ARG(0);
    long r;
    if (!gfd_ok(fd)) ERR(9);
    if (rt_fds[fd].kind != FD_HOST) ERR(29);
    r = _lseek(rt_fds[fd].hfd, (long)ARG(1), (int)ARG(2));
    if (r < 0) HERR();
    RET(r);
}

DECL_IMP(dup) {
    int fd = (int)ARG(0), n;
    if (!gfd_ok(fd)) ERR(9);
    n = rt_fd_alloc(0);
    if (n < 0) ERR(24);
    rt_fds[n] = rt_fds[fd];
    if (rt_fds[fd].kind == FD_HOST) rt_fds[n].hfd = _dup(rt_fds[fd].hfd);
    RET(n);
}
DECL_IMP(dup2) {
    int fd = (int)ARG(0), n = (int)ARG(1);
    if (!gfd_ok(fd) || n < 0 || n >= MAXFD) ERR(9);
    if (fd == n) RET(n);
    if (rt_fds[n].kind == FD_HOST && n > 2) _close(rt_fds[n].hfd);
    rt_fds[n] = rt_fds[fd];
    if (rt_fds[fd].kind == FD_HOST) {
        if (n <= 2) { _dup2(rt_fds[fd].hfd, n); rt_fds[n].hfd = n; }
        else rt_fds[n].hfd = _dup(rt_fds[fd].hfd);
    }
    RET(n);
}
DECL_IMP(pipe) {
    int h[2], a, b;
    if (_pipe(h, 4096, _O_BINARY) < 0) HERR();
    a = rt_fd_alloc(0); rt_fds[a].kind = FD_HOST; rt_fds[a].hfd = h[0];
    b = rt_fd_alloc(0); rt_fds[b].kind = FD_HOST; rt_fds[b].hfd = h[1];
    M32(ARG(0)) = a; M32(ARG(0) + 4) = b;
    RET(0);
}
DECL_IMP(fcntl) {
    int fd = (int)ARG(0);
    if (!gfd_ok(fd)) ERR(9);
    switch (ARG(1)) {
    case 1: RET(0);                       /* F_GETFD */
    case 2: RET(0);                       /* F_SETFD */
    case 3: RET(rt_fds[fd].flags);        /* F_GETFL */
    case 4: rt_fds[fd].flags = (rt_fds[fd].flags & 3) | (ARG(2) & ~3u); RET(0);
    default: RET(0);
    }
}
DECL_IMP(flock) { RET(0); }
DECL_IMP(fsync) {
    int fd = (int)ARG(0);
    if (gfd_ok(fd) && rt_fds[fd].kind == FD_HOST) _commit(rt_fds[fd].hfd);
    RET(0);
}
DECL_IMP(sync) { RET_VOID(); }
DECL_IMP(ftruncate) {
    int fd = (int)ARG(0);
    if (!gfd_ok(fd) || rt_fds[fd].kind != FD_HOST) ERR(9);
    if (_chsize(rt_fds[fd].hfd, (long)ARG(1)) < 0) HERR();
    RET(0);
}

DECL_IMP(ioctl) {
    int fd = (int)ARG(0);
    u32 req = ARG(1);
    if (!gfd_ok(fd)) ERR(9);
    if (req == 0x541B) {   /* FIONREAD */
        M32(ARG(2)) = rt_fds[fd].kind == FD_UIB ? (u32)uib_avail() : 0;
        RET(0);
    }
    if (req == 0x5401) { memcpy(ARGP(2), termios_store[fd], 36); RET(0); }   /* TCGETS */
    if (rt_fds[fd].kind == FD_HOST && fd > 2) ERR(25);
    RET(0);
}

/* termios: c_iflag c_oflag c_cflag c_lflag c_line c_cc[32] (pad) c_ispeed c_ospeed */
DECL_IMP(tcgetattr) {
    int fd = (int)ARG(0);
    if (!gfd_ok(fd)) ERR(9);
    memcpy(ARGP(1), termios_store[fd], 60);
    RET(0);
}
DECL_IMP(tcsetattr) {
    int fd = (int)ARG(0);
    if (!gfd_ok(fd)) ERR(9);
    memcpy(termios_store[fd], ARGP(2), 60);
    RET(0);
}
DECL_IMP(cfmakeraw) {
    u8 *t = (u8 *)ARGP(0);
    u32 *f = (u32 *)t;
    f[0] &= ~(0001u | 0002u | 0010u | 0040u | 0100u | 0200u | 0400u | 02000u);
    f[1] &= ~0001u;
    f[3] &= ~(0010u | 0100u | 0002u | 0001u | 0100000u);
    f[2] &= ~(0060u | 0400u);
    f[2] |= 0060u;
    t[17 + 6] = 1;   /* VMIN */
    t[17 + 5] = 0;   /* VTIME */
    RET_VOID();
}
DECL_IMP(cfsetispeed) { M32(ARG(0) + 52) = ARG(1); RET(0); }
DECL_IMP(cfsetospeed) { M32(ARG(0) + 56) = ARG(1); M32(ARG(0) + 8) = (M32(ARG(0) + 8) & ~0x100Fu) | ARG(1); RET(0); }
DECL_IMP(tcdrain) { RET(0); }
DECL_IMP(tcflush) {
    int fd = (int)ARG(0);
    if (gfd_ok(fd) && rt_fds[fd].kind == FD_UIB && ARG(1) != 1) { u8 junk[256]; while (uib_avail()) uib_read(junk, sizeof junk); }
    RET(0);
}
DECL_IMP(ttyname) { static char tn[] = "/dev/tty1"; RET(tn); }

/* select(nfds, r, w, e, timeout) on Linux fd_set (1024 bits) */
DECL_IMP(select) {
    int nfds = (int)ARG(0), fd, n;
    u32 *rs = (u32 *)ARGP(1), *ws = (u32 *)ARGP(2), *es = (u32 *)ARGP(3);
    u32 tv = ARG(4);
    u32 rin[32], win[32];
    u64 lim = ~0ull, start = rt_now_us();
    if (rs) memcpy(rin, rs, 128); else memset(rin, 0, 128);
    if (ws) memcpy(win, ws, 128); else memset(win, 0, 128);
    if (tv) lim = (u64)M32(tv) * 1000000 + M32(tv + 4);
    for (;;) {
        n = 0;
        if (rs) memset(rs, 0, 128);
        if (ws) memset(ws, 0, 128);
        if (es) memset(es, 0, 128);
        for (fd = 0; fd < nfds && fd < MAXFD; fd++) {
            int rd = (rin[fd >> 5] >> (fd & 31)) & 1, wr = (win[fd >> 5] >> (fd & 31)) & 1;
            int kind = rt_fds[fd].kind, rr = 0, ww = 0;
            if (rd) rr = kind == FD_UIB ? uib_avail() > 0 : kind == FD_HOST ? fd != 0 : kind == FD_NULL;
            if (wr) ww = kind != FD_FREE;
            if (rr) { rs[fd >> 5] |= 1u << (fd & 31); n++; }
            if (ww) { ws[fd >> 5] |= 1u << (fd & 31); n++; }
        }
        if (n) RET(n);
        {
            u64 el = rt_now_us() - start;
            if (lim != ~0ull && el >= lim) {
                if (tv) { M32(tv) = 0; M32(tv + 4) = 0; }
                RET(0);
            }
            if (rt_wait_us(c, lim == ~0ull ? 5000 : (lim - el < 5000 ? lim - el : 5000))) ERR(4);
        }
    }
}

/* ---------------- files by path */
static void fill_stat(u32 b, int isdir, u32 size, u32 mtime, u32 atime, u32 ctime) {
    memset((void *)(uintptr_t)b, 0, 88);
    M32(b + 0) = 0x801;
    M32(b + 12) = 1;
    M32(b + 16) = isdir ? 040755 : 0100644;
    M32(b + 20) = 1;
    M32(b + 44) = size;
    M32(b + 48) = 4096;
    M32(b + 52) = (size + 511) / 512;
    M32(b + 56) = atime; M32(b + 64) = mtime; M32(b + 72) = ctime;
}
static u32 ft2unix(FILETIME f) {
    u64 t = ((u64)f.dwHighDateTime << 32) | f.dwLowDateTime;
    return (u32)((t - 116444736000000000ull) / 10000000ull);
}
DECL_IMP(__xstat) {
    char hp[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA d;
    const char *g = ARGS(1);
    if (!strcmp(g, "/dev/uib") || !strcmp(g, "/dev/null")) { fill_stat(ARG(2), 0, 0, 0, 0, 0); M32(ARG(2) + 16) = 020666; RET(0); }
    if (rt_map_path(g, hp, sizeof hp)) { if (filelog()) rt_logf("FILE stat %s: unmapped\n", g); ERR(2); }
    if (!GetFileAttributesExA(hp, GetFileExInfoStandard, &d)) { if (filelog()) rt_logf("FILE stat %s (%s): FAIL\n", g, hp); ERR(2); }
    if (filelog()) rt_logf("FILE stat %s (%s)\n", g, hp);
    fill_stat(ARG(2), (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, d.nFileSizeLow,
              ft2unix(d.ftLastWriteTime), ft2unix(d.ftLastAccessTime), ft2unix(d.ftLastWriteTime));
    RET(0);
}
DECL_IMP(__fxstat) {
    int fd = (int)ARG(1);
    struct _stat st;
    if (!gfd_ok(fd)) ERR(9);
    if (rt_fds[fd].kind != FD_HOST) { fill_stat(ARG(2), 0, 0, 0, 0, 0); M32(ARG(2) + 16) = 020666; RET(0); }
    if (_fstat(rt_fds[fd].hfd, &st) < 0) HERR();
    fill_stat(ARG(2), (st.st_mode & _S_IFDIR) != 0, (u32)st.st_size, (u32)st.st_mtime, (u32)st.st_atime, (u32)st.st_ctime);
    RET(0);
}
DECL_IMP(access) {
    char hp[MAX_PATH];
    const char *g = ARGS(0);
    if (!strcmp(g, "/dev/uib")) RET(0);
    if (rt_map_path(g, hp, sizeof hp)) ERR(2);
    if (GetFileAttributesA(hp) == INVALID_FILE_ATTRIBUTES) { if (filelog()) rt_logf("FILE access %s: FAIL\n", g); ERR(2); }
    RET(0);
}
DECL_IMP(mkdir) {
    char hp[MAX_PATH];
    if (rt_map_path(ARGS(0), hp, sizeof hp)) ERR(2);
    if (_mkdir(hp) < 0) HERR();
    RET(0);
}
DECL_IMP(chmod) { RET(0); }
DECL_IMP(remove) {
    char hp[MAX_PATH];
    if (rt_map_path(ARGS(0), hp, sizeof hp)) ERR(2);
    if (DeleteFileA(hp)) RET(0);
    if (RemoveDirectoryA(hp)) RET(0);
    ERR(2);
}
DECL_IMP(rename) {
    char a[MAX_PATH], b[MAX_PATH];
    if (rt_map_path(ARGS(0), a, sizeof a) || rt_map_path(ARGS(1), b, sizeof b)) ERR(2);
    if (!MoveFileExA(a, b, MOVEFILE_REPLACE_EXISTING)) ERR(2);
    RET(0);
}
DECL_IMP(mkstemp) {
    char *t = ARGS(0), hp[MAX_PATH];
    size_t L = strlen(t);
    int k, fd, h;
    if (L < 6) ERR(22);
    for (k = 0; k < 100; k++) {
        int i;
        u32 r = GetTickCount() * 2654435761u + k * 40503u + GetCurrentThreadId();
        for (i = 0; i < 6; i++) { t[L - 6 + i] = "abcdefghijklmnopqrstuvwxyz0123456789"[r % 36]; r = r / 36 + 7 * i + k; }
        if (rt_map_path(t, hp, sizeof hp)) ERR(2);
        h = _open(hp, _O_RDWR | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
        if (h >= 0) {
            fd = rt_fd_alloc(0);
            rt_fds[fd].kind = FD_HOST; rt_fds[fd].hfd = h; rt_fds[fd].flags = 2;
            RET(fd);
        }
    }
    ERR(17);
}

/* ---------------- stdio (guest FILE* == msvcrt FILE*) */
static FILE *gfopen(CPU *c, const char *path, const char *mode) {
    char hp[MAX_PATH], m[16];
    FILE *f;
    size_t L = strlen(mode);
    if (L > 10) L = 10;
    memcpy(m, mode, L); m[L] = 0;
    if (!strchr(m, 'b')) strcat(m, "b");
    if (!strcmp(path, "/dev/null")) strcpy(hp, "NUL");
    else if (rt_map_path(path, hp, sizeof hp)) { c->err = 2; return 0; }
    f = fopen(hp, m);
    if (!f) c->err = rt_errno_from_host(errno);
    if (filelog()) rt_logf("FILE fopen %s %s%s\n", path, mode, f ? "" : ": FAIL");
    return f;
}
DECL_IMP(fopen) {
    FILE *f = gfopen(c, ARGS(0), ARGS(1));
    RET(f);
}
DECL_IMP(fdopen) {
    int fd = (int)ARG(0);
    char m[16];
    if (!gfd_ok(fd) || rt_fds[fd].kind != FD_HOST) ERR(9);
    snprintf(m, sizeof m, "%s", ARGS(1));
    if (!strchr(m, 'b')) strcat(m, "b");
    RET(_fdopen(rt_fds[fd].hfd, m));
}
DECL_IMP(fclose) { RET(fclose((FILE *)ARGP(0))); }
DECL_IMP(fflush) { RET(fflush((FILE *)ARGP(0))); }
DECL_IMP(fgets) { RET(fgets(ARGS(0), (int)ARG(1), (FILE *)ARGP(2))); }
DECL_IMP(fputc) { RET(fputc((int)ARG(0), (FILE *)ARGP(1))); }
DECL_IMP(fputs) { RET(fputs(ARGS(0), (FILE *)ARGP(1))); }
DECL_IMP(puts) { int r = puts(ARGS(0)); rt_logf("[puts] %s\n", ARGS(0)); RET(r); }
DECL_IMP(fread) { RET(fread(ARGP(0), ARG(1), ARG(2), (FILE *)ARGP(3))); }
DECL_IMP(fwrite) { RET(fwrite(ARGP(0), ARG(1), ARG(2), (FILE *)ARGP(3))); }
DECL_IMP(fseek) { RET(fseek((FILE *)ARGP(0), (long)ARG(1), (int)ARG(2))); }
DECL_IMP(rewind) { rewind((FILE *)ARGP(0)); RET_VOID(); }
DECL_IMP(setvbuf) {
    int mode = (int)ARG(2);
    size_t sz = ARG(3);
    int hm = mode == 2 ? _IONBF : _IOFBF;
    if (hm != _IONBF && sz < 2) sz = 4096;
    RET(setvbuf((FILE *)ARGP(0), mode == 2 ? 0 : ARGS(1), hm, sz));
}

static void log_out(FILE *f, const char *fmt, va_list ap) {
    if (f == stdout || f == stderr) {
        char b[1024];
        __mingw_vsnprintf(b, sizeof b, fmt, ap);
        rt_logf("[%s] %s", f == stdout ? "out" : "err", b);
    }
}
DECL_IMP(printf) { log_out(stdout, ARGS(0), GVA(1)); RET(__mingw_vfprintf(stdout, ARGS(0), GVA(1))); }
DECL_IMP(fprintf) { log_out((FILE *)ARGP(0), ARGS(1), GVA(2)); RET(__mingw_vfprintf((FILE *)ARGP(0), ARGS(1), GVA(2))); }
DECL_IMP(vfprintf) { log_out((FILE *)ARGP(0), ARGS(1), (va_list)ARGP(2)); RET(__mingw_vfprintf((FILE *)ARGP(0), ARGS(1), (va_list)ARGP(2))); }
DECL_IMP(sprintf) { RET(__mingw_vsnprintf(ARGS(0), 0x7fffffff, ARGS(1), GVA(2))); }
DECL_IMP(snprintf) { RET(__mingw_vsnprintf(ARGS(0), ARG(1), ARGS(2), GVA(3))); }
DECL_IMP(vsnprintf) { RET(__mingw_vsnprintf(ARGS(0), ARG(1), ARGS(2), (va_list)ARGP(3))); }
/* scanf family: forward up to 32 pointer args to msvcrt (all scanf args are pointers) */
int rt_fwd_scanf(FILE *f, const char *str, const char *fmt0, const u32 *a) {
    char fmt[1024], *w = fmt;
    const char *r = fmt0;
    while (*r && w < fmt + sizeof fmt - 4) {
        if (r[0] == 'l' && r[1] == 'l') { memcpy(w, "I64", 3); w += 3; r += 2; continue; }
        *w++ = *r++;
    }
    *w = 0;
#define A8(k) (void *)(uintptr_t)a[k], (void *)(uintptr_t)a[k + 1], (void *)(uintptr_t)a[k + 2], (void *)(uintptr_t)a[k + 3], \
              (void *)(uintptr_t)a[k + 4], (void *)(uintptr_t)a[k + 5], (void *)(uintptr_t)a[k + 6], (void *)(uintptr_t)a[k + 7]
    if (f) return fscanf(f, fmt, A8(0), A8(8), A8(16), A8(24));
    return sscanf(str, fmt, A8(0), A8(8), A8(16), A8(24));
#undef A8
}
DECL_IMP(sscanf) { RET(rt_fwd_scanf(0, ARGS(0), ARGS(1), (const u32 *)(uintptr_t)(c->r[4].d + 12))); }
DECL_IMP(fscanf) { RET(rt_fwd_scanf((FILE *)ARGP(0), 0, ARGS(1), (const u32 *)(uintptr_t)(c->r[4].d + 12))); }
DECL_IMP(vfscanf) { RET(rt_fwd_scanf((FILE *)ARGP(0), 0, ARGS(1), (const u32 *)ARGP(2))); }

/* ---------------- time */
static void unix_now(u32 *sec, u32 *usec) {
    static void (WINAPI *precise)(FILETIME *);
    static int looked;
    FILETIME f;
    u64 t;
    if (!looked) { precise = (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetSystemTimePreciseAsFileTime"); looked = 1; }
    if (precise) precise(&f);   /* microsecond resolution; the plain call only moves every 15.6 ms */
    else GetSystemTimeAsFileTime(&f);
    t = (((u64)f.dwHighDateTime << 32) | f.dwLowDateTime) - 116444736000000000ull;
    *sec = (u32)(t / 10000000ull);
    if (usec) *usec = (u32)((t % 10000000ull) / 10);
}
DECL_IMP(time) {
    u32 s;
    unix_now(&s, 0);
    if (ARG(0)) M32(ARG(0)) = s;
    RET(s);
}
DECL_IMP(gettimeofday) {
    u32 s, us;
    unix_now(&s, &us);
    if (ARG(0)) { M32(ARG(0)) = s; M32(ARG(0) + 4) = us; }
    if (ARG(1)) { M32(ARG(1)) = 0; M32(ARG(1) + 4) = 0; }
    RET(0);
}
/* glibc struct tm: 9 ints, long tm_gmtoff, const char *tm_zone */
static u32 gtm_buf[11];
static char tzn[2][8] = { "LOC", "LOC" };
static void tm_to_guest(u32 *g, const struct tm *t, long gmtoff) {
    g[0] = t->tm_sec; g[1] = t->tm_min; g[2] = t->tm_hour; g[3] = t->tm_mday; g[4] = t->tm_mon;
    g[5] = t->tm_year; g[6] = t->tm_wday; g[7] = t->tm_yday; g[8] = t->tm_isdst;
    g[9] = (u32)gmtoff;
    g[10] = (u32)(uintptr_t)tzn[t->tm_isdst > 0];
}
static void tm_from_guest(struct tm *t, const u32 *g) {
    memset(t, 0, sizeof *t);
    t->tm_sec = g[0]; t->tm_min = g[1]; t->tm_hour = g[2]; t->tm_mday = g[3]; t->tm_mon = g[4];
    t->tm_year = g[5]; t->tm_wday = g[6]; t->tm_yday = g[7]; t->tm_isdst = g[8];
}
DECL_IMP(localtime) {
    time_t t = (time_t)M32(ARG(0));
    struct tm *r = localtime(&t);
    if (!r) RET(0);
    tm_to_guest(gtm_buf, r, -_timezone + (r->tm_isdst > 0 ? 3600 : 0));
    RET(gtm_buf);
}
DECL_IMP(gmtime) {
    time_t t = (time_t)M32(ARG(0));
    struct tm *r = gmtime(&t);
    if (!r) RET(0);
    tm_to_guest(gtm_buf, r, 0);
    RET(gtm_buf);
}
DECL_IMP(mktime) {
    struct tm t;
    time_t r;
    u32 *g = (u32 *)ARGP(0);
    tm_from_guest(&t, g);
    r = mktime(&t);
    if (r != (time_t)-1) tm_to_guest(g, &t, -_timezone + (t.tm_isdst > 0 ? 3600 : 0));
    RET((u32)r);
}
DECL_IMP(strftime) {
    char *out = ARGS(0), fmt2[512], *w = fmt2;
    size_t max = ARG(1);
    const char *f = ARGS(2);
    struct tm t;
    tm_from_guest(&t, (u32 *)ARGP(3));
    /* expand glibc-only conversions msvcrt lacks */
    while (*f && w < fmt2 + sizeof fmt2 - 16) {
        if (f[0] == '%' && f[1]) {
            char x = f[1];
            const char *rep = 0;
            char tmp[16];
            switch (x) {
            case 'T': rep = "%H:%M:%S"; break;
            case 'D': rep = "%m/%d/%y"; break;
            case 'R': rep = "%H:%M"; break;
            case 'F': rep = "%Y-%m-%d"; break;
            case 'h': rep = "%b"; break;
            case 'r': rep = "%I:%M:%S %p"; break;
            case 'n': rep = "\n"; break;
            case 't': rep = "\t"; break;
            case 'e': snprintf(tmp, sizeof tmp, "%2d", t.tm_mday); rep = tmp; break;
            case 'k': snprintf(tmp, sizeof tmp, "%2d", t.tm_hour); rep = tmp; break;
            case 'l': snprintf(tmp, sizeof tmp, "%2d", t.tm_hour % 12 ? t.tm_hour % 12 : 12); rep = tmp; break;
            case 'C': snprintf(tmp, sizeof tmp, "%02d", (t.tm_year + 1900) / 100); rep = tmp; break;
            case 'P': rep = t.tm_hour < 12 ? "am" : "pm"; break;
            case 's': { struct tm t2 = t; snprintf(tmp, sizeof tmp, "%ld", (long)mktime(&t2)); rep = tmp; break; }
            case '-': case '_': case '0': case 'E': case 'O':   /* flags: drop */
                f++; continue;
            }
            if (rep) {
                size_t L = strlen(rep);
                if (w + L >= fmt2 + sizeof fmt2 - 1) break;
                memcpy(w, rep, L); w += L; f += 2; continue;
            }
            *w++ = *f++; *w++ = *f++;
            continue;
        }
        *w++ = *f++;
    }
    *w = 0;
    RET(strftime(out, max, fmt2, &t));
}

DECL_IMP(sleep) {
    u32 s = ARG(0);
    DWORD st = GetTickCount();
    if (rt_wait_ms(c, s * 1000)) {
        u32 el = (GetTickCount() - st) / 1000;
        RET(s > el ? s - el : 0);
    }
    RET(0);
}
DECL_IMP(usleep) {
    if (rt_wait_us(c, ARG(0))) ERR(4);
    RET(0);
}

/* ---------------- process */
int rt_is_thread(int pid);
int rt_kill_thread(int pid, int sig);
#define GPID 1000
DECL_IMP(getpid) { RET(c->is_main ? GPID : c->tid); }
DECL_IMP(getpgrp) { RET(GPID); }
DECL_IMP(setpgid) { RET(0); }
DECL_IMP(geteuid) { RET(0); }
DECL_IMP(getpriority) { RET(0); }
DECL_IMP(setpriority) { RET(0); }
DECL_IMP(nice) { RET(0); }
DECL_IMP(iopl) { ERR(1); }
DECL_IMP(ioperm) { RET(0); }
DECL_IMP(kill) {
    int pid = (int)ARG(0), sig = (int)ARG(1);
    if (pid == GPID || pid == 0 || pid == -GPID) { if (sig) rt_raise(sig); RET(0); }
    { extern int rt_snd_kill(int pid); int sp = pid < 0 ? -pid : pid;   /* the game signals the player's process group: kill(-pid, SIGINT) */
      if ((sp & 1) && sig != 0 && sig != 18 && rt_snd_kill(sp) == 0) RET(0);
      if ((sp & 1) && sp >= 900001) { if (sig == 0 || sig == 18) RET(0); } }
    if (rt_kill_thread(pid, sig) == 0) RET(0);
    ERR(3);
}
DECL_IMP(raise) {
    rt_raise((int)ARG(0));
    rt_deliver(c);
    RET(0);
}

DECL_IMP(fork) { rt_logf("fork() from %08x: not supported\n", c->cur); ERR(11); }
DECL_IMP(vfork) { rt_logf("vfork() from %08x: not supported\n", c->cur); ERR(11); }
DECL_IMP(execl) { rt_logf("execl(%s)\n", ARGS(0)); ERR(2); }
DECL_IMP(execlp) { rt_logf("execlp(%s)\n", ARGS(0)); ERR(2); }
DECL_IMP(execv) { rt_logf("execv(%s)\n", ARGS(0)); ERR(2); }
DECL_IMP(execvp) { rt_logf("execvp(%s)\n", ARGS(0)); ERR(2); }

static void rm_glob(const char *gpath) {
    char hp[MAX_PATH], dir[MAX_PATH], *s;
    WIN32_FIND_DATAA fd;
    HANDLE h;
    if (rt_map_path(gpath, hp, sizeof hp)) return;
    strcpy(dir, hp);
    s = strrchr(dir, '\\');
    if (s) s[1] = 0; else dir[0] = 0;
    h = FindFirstFileA(hp, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char full[MAX_PATH];
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        snprintf(full, sizeof full, "%s%s", dir, fd.cFileName);
        DeleteFileA(full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
static void mkdir_p(const char *gpath) {
    char hp[MAX_PATH], *p;
    if (rt_map_path(gpath, hp, sizeof hp)) return;
    for (p = hp + 1; *p; p++) if (*p == '\\') { *p = 0; _mkdir(hp); *p = '\\'; }
    _mkdir(hp);
}
DECL_IMP(system) {
    const char *cmd = ARGS(0);
    char buf[1024], *tok;
    if (!cmd) RET(0);
    rt_logf("system(\"%s\")\n", cmd);
    snprintf(buf, sizeof buf, "%s", cmd);
    tok = strtok(buf, " \t");
    if (tok && !strcmp(tok, "mkdir")) {
        while ((tok = strtok(0, " \t"))) if (tok[0] != '-') mkdir_p(tok);
        RET(0);
    }
    if (tok && !strcmp(tok, "rm")) {
        while ((tok = strtok(0, " \t"))) if (tok[0] != '-') rm_glob(tok);
        RET(0);
    }
    RET(0);
}

DECL_IMP(dlopen) { rt_logf("dlopen(%s)\n", ARGS(0) ? ARGS(0) : "(null)"); RET(0); }
DECL_IMP(dlsym) { RET(0); }
DECL_IMP(dlclose) { RET(0); }

void rt_longjmp(CPU *c, u32 env, u32 val);
DECL_IMP(longjmp) { rt_longjmp(c, ARG(0), ARG(1)); }
DECL_IMP(siglongjmp) { rt_longjmp(c, ARG(0), ARG(1)); }
/* reached only via function pointers; direct calls are inlined by the recompiler */
DECL_IMP(_setjmp) { rt_fatal(c, "_setjmp called indirectly"); }
DECL_IMP(__sigsetjmp) { rt_fatal(c, "__sigsetjmp called indirectly"); }

/* ---------------- linking */
static const char *xdga_name = "XFree86-DGA";
void rt_link_imports(void) {
    int i, j;
    ctype_init();
    for (i = 0; i < guest_got_n; i++) if (guest_got[i].stub) M32(guest_got[i].got) = guest_got[i].stub;
    for (i = 0; i < guest_dataimp_n; i++) {
        const DataImp *d = &guest_dataimp[i];
        if (d->type == 5) {   /* R_386_COPY */
            if (!strcmp(d->name, "stdout")) M32(d->addr) = (u32)(uintptr_t)stdout;
            else if (!strcmp(d->name, "stderr")) M32(d->addr) = (u32)(uintptr_t)stderr;
            else if (!strcmp(d->name, "stdin")) M32(d->addr) = (u32)(uintptr_t)stdin;
        }
    }
    for (i = 0; i < guest_dataimp_n; i++) {
        const DataImp *d = &guest_dataimp[i];
        if (d->type != 6) continue;   /* R_386_GLOB_DAT */
        M32(d->addr) = 0;
        for (j = 0; j < guest_dataimp_n; j++)
            if (guest_dataimp[j].type == 5 && !strcmp(guest_dataimp[j].name, d->name)) M32(d->addr) = guest_dataimp[j].addr;
        if (!strcmp(d->name, "xdga_extension_name")) M32(d->addr) = (u32)(uintptr_t)&xdga_name;
    }
    for (i = 0; i < MAXFD; i++) rt_fds[i].kind = FD_FREE;
    for (i = 0; i < 3; i++) { rt_fds[i].kind = FD_HOST; rt_fds[i].hfd = i; }
    for (i = 0; i < MAXFD; i++) {
        u32 *t = (u32 *)termios_store[i];
        t[0] = 0x500; t[1] = 0x5; t[2] = 0xbf; t[3] = 0x8a3b;
        termios_store[i][17 + 6] = 1;
    }
}
