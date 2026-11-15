/*
 * Parallel-port protection dongle emulator for v8mnu22b (Innovative Foto vend app).
 *
 * The dongle is a 64 x 16-bit serial EEPROM (93C46-style, 6 address bits) driven by
 * bit-banging the LPT data/status ports, wrapped in a login/challenge API
 * (sub_806B630 -> sub_806B860 jump table, cmds 0..7).  Rather than trap in/out
 * instructions, this LD_PRELOAD patches the two word-level primitives:
 *     sub_806B4B0(port, addr)        read word
 *     sub_806B5D0(port, addr, val)   write word
 * so the game's own login / challenge / permission logic runs unchanged against an
 * in-memory EEPROM image, and overrides ioperm() so port probing "succeeds".
 *
 * Built -nostdlib (raw syscalls only) so it loads under the chroot's glibc 2.4.
 *
 * EEPROM layout (what the game needs):
 *   w2, w3      nonzero (dispatch through a table whose only bad entry is index 0)
 *   w6:w7       function pointer the game stores (sub_804BA0A) and later calls
 *   w10:w11     ^ CRC32(string w42..49) == 0x3BBF1547  -> main_init returns 0x8BE2D272,
 *               which main() turns into a call to the real main loop at 0x804D045
 *   w12:w13     ^ CRC32(...) == 0x86BC372C            -> "protection block" check
 *   w14         nonzero
 *   w42..49     16-char string (empty -> CRC 0)
 *   w60, w61    seed for the login passwords; solved at load time so that the game's
 *               login (0xEB36, 0xC9F4, 0x57E2) is accepted
 *   w63         0xA000 (presence signature)
 * Writes are persisted to /vend/dongle.img.
 */
typedef unsigned short u16;
typedef unsigned int u32;

#define RD_FN   0x806B4B0
#define WR_FN   0x806B5D0
#define LOGIN_FN 0x806B8F0
#define AF2 (*(volatile u16 *)0x80C1AF2)
#define AF4 (*(volatile u16 *)0x80C1AF4)
#define AF6 (*(volatile u16 *)0x80C1AF6)

static u16 ee[64];
static int logfd = -1;
static const char IMG[] = "/vend/dongle.img";
static const char LOG[] = "/vend/tmp/dongle.log";

static inline int sc3(int n, int a, int b, int c) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}
#define SYS_read 3
#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_lseek 19
#define SYS_mprotect 125

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void put(const char *s) { if (logfd >= 0) sc3(SYS_write, logfd, (int)s, slen(s)); }
static void puthex(u32 v, int digits) {
    char b[9]; int i;
    for (i = digits - 1; i >= 0; i--) { b[i] = "0123456789abcdef"[v & 15]; v >>= 4; }
    b[digits] = 0; put(b);
}

static int quiet_read(int a) { return a == 63 || a == 60 || a == 61 || a == 62; }

static void save(void) {
    int fd = sc3(SYS_open, (int)IMG, 01 | 0100 | 01000, 0644);  /* O_WRONLY|O_CREAT|O_TRUNC */
    if (fd >= 0) { sc3(SYS_write, fd, (int)ee, sizeof ee); sc3(SYS_close, fd, 0, 0); }
}

static int solving;

int __attribute__((cdecl)) dongle_read(int port, int addr) {
    (void)port;
    int v = ee[addr & 63];
    if (!solving && !quiet_read(addr & 63)) {
        put("["); puthex(sc3(20 /*getpid*/, 0, 0, 0), 4); put("] R w"); puthex(addr & 63, 2);
        put(" = "); puthex(v, 4); put(" from "); puthex((u32)__builtin_return_address(0), 8); put("\n");
    }
    return v;
}

int __attribute__((cdecl)) dongle_write(int port, int addr, int val) {
    (void)port;
    ee[addr & 63] = (u16)val;
    put("W w"); puthex(addr & 63, 2); put(" = "); puthex((u16)val, 4); put("\n");
    save();
    return 0;
}

/* game calls ioperm(port, 3, on) before touching the LPT port */
int ioperm(unsigned long from, unsigned long num, int on) { (void)from; (void)num; (void)on; return 0; }

static void hook(u32 at, void *to) {
    u32 page = at & ~0xFFFu;
    sc3(SYS_mprotect, page, 0x2000, 7);
    unsigned char *p = (unsigned char *)at;
    int rel = (int)((u32)to - (at + 5));
    p[0] = 0xE9;
    p[1] = rel; p[2] = rel >> 8; p[3] = rel >> 16; p[4] = rel >> 24;
}

#define PRN_INIT_FN 0x80675FE
int __attribute__((cdecl)) prn_init_stub(void) {
    int i;
    *(volatile u32 *)0x80B9EE4 = 1;   /* prn inited */
    *(volatile u32 *)0x80B9EE8 = 1;   /* primary printer = 1 (prn_check returns -1/-2 -> EXIT if 0) */
    *(volatile u32 *)0x80B9EF0 = 1;   /* printers online */
    for (i = 1; i <= 8; i++) ((volatile u16 *)0x80B9EFE)[i] = 0;   /* no ports: skip driver status polls */
    put("prn_init stubbed\n");
    return 0;
}

/* prn_status(pr) (sub_8067CCB): 0 = ready, else drives "OUT OF PAPER (P4)" etc. */
#define PRN_STATUS_FN 0x8067CCB
int __attribute__((cdecl)) prn_status_stub(int pr) { (void)pr; return 0; }

/* dict.db string key.  The real one is hashed from the 16-char string in dongle words 42..49,
 * which we don't have; the 16 derived bytes were recovered from dict.db by known plaintext
 * (dongle/dictkey.py).  Replaces sub_8083C33(keystring), its only caller is main_init. */
#define DICTKEY_FN 0x8083C33
static const unsigned char dict_key[16] = {
    0x5c, 0x31, 0x9a, 0x8e, 0x73, 0xef, 0x77, 0x76, 0x4c, 0x56, 0x5d, 0x87, 0x8c, 0xc9, 0x18, 0xfd };
int __attribute__((cdecl)) dictkey_stub(const char *s) {
    int i;
    (void)s;
    ((void (*)(void))0x8082DBA)();                 /* flush dictionary cache, as original */
    for (i = 0; i < 32; i++) ((volatile unsigned char *)0x80BC100)[i] = i < 16 ? dict_key[i] : 0;
    *(volatile u32 *)0x80BC054 = 0x80BC100;
    put("dict key installed\n");
    return 0;
}

typedef int (*login_t)(int, int, int, int);

static int solve_login(void) {
    login_t login = (login_t)LOGIN_FN;
    u32 e2, e0;
    solving = 1;
    for (e2 = 0; e2 < 0x10000; e2++) {
        ee[61] = e2; ee[60] = 0;
        login(0x378, 0, 0, 0);
        if (AF4 != 0xC9F4) continue;
        for (e0 = 0; e0 < 0x10000; e0++) {
            ee[60] = e0;
            login(0x378, 0, 0, 0);
            if (AF2 == 0xEB36) {
                if (AF6 == 0x57E2) { solving = 0; return 1; }
                break;  /* AF6 depends only on AF2 and w61 */
            }
        }
    }
    solving = 0;
    return 0;
}

static void defaults(void) {
    int i;
    for (i = 0; i < 64; i++) ee[i] = 0;
    ee[2] = 0x0001;
    ee[3] = 0x0348;
    ee[5] = 0x012C;
    ee[6] = 0xBA0A; ee[7] = 0x0804;   /* sub_804BA0A */
    ee[8] = 0x0100;
    ee[10] = 0x3BBF; ee[11] = 0x1547;
    ee[12] = 0x86BC; ee[13] = 0x372C;
    ee[14] = 0x0001;
    ee[62] = 0x0001;
    ee[63] = 0xA000;
}

__attribute__((constructor)) static void init(void) {
    logfd = sc3(SYS_open, (int)LOG, 01 | 0100 | 02000, 0644);  /* O_WRONLY|O_CREAT|O_APPEND */
    /* only act inside the game binary */
    if (*(volatile u32 *)RD_FN != 0x56E58955u) {  /* push ebp; mov ebp,esp; push esi */ put("dongle: not v8mnu22b, inactive\n"); return; }
    int fd = sc3(SYS_open, (int)IMG, 0, 0), loaded = 0;
    if (fd >= 0) { loaded = sc3(SYS_read, fd, (int)ee, sizeof ee) == sizeof ee; sc3(SYS_close, fd, 0, 0); }
    if (!loaded) defaults();
    hook(RD_FN, dongle_read);
    hook(WR_FN, dongle_write);
    /* no printer hardware: prn_init() (sub_80675FE) -> mark inited, return 0 */
    hook(PRN_INIT_FN, prn_init_stub);
    hook(DICTKEY_FN, dictkey_stub);
    hook(PRN_STATUS_FN, prn_status_stub);
    if (!loaded) {
        if (solve_login()) { put("dongle: solved login seed w60="); puthex(ee[60], 4); put(" w61="); puthex(ee[61], 4); put("\n"); }
        else put("dongle: FAILED to solve login seed\n");
        *(volatile u32 *)0x80C1ACC = 0;   /* clear the error code the trial logins left */
        save();
    }
    put(loaded ? "dongle: loaded /vend/dongle.img\n" : "dongle: fresh image\n");
}
