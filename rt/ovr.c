/* Replacements for guest functions that talk to hardware we don't have:
 * the parallel-port EEPROM dongle, the photo printers, and the dict.db key derived from the dongle. */
#include "rt.h"

static u16 ee[64];
static int solving;
static const char IMG[] = "dongle.img";   /* relative to /vend */

static void ee_save(void) {
    FILE *f = fopen(IMG, "wb");
    if (f) { fwrite(ee, sizeof ee, 1, f); fclose(f); }
}

/* sub_806B4B0(port, addr) -> word */
void ovr_dongle_read(CPU *c) {
    int a = ARG(1) & 63;
    if (!solving && a < 60) rt_logf("dongle R w%02x = %04x\n", a, ee[a]);
    RET(ee[a]);
}

/* sub_806B5D0(port, addr, val) */
void ovr_dongle_write(CPU *c) {
    int a = ARG(1) & 63;
    ee[a] = (u16)ARG(2);
    rt_logf("dongle W w%02x = %04x\n", a, ee[a]);
    ee_save();
    RET(0);
}

/* prn_init(): no printer hardware */
void ovr_prn_init(CPU *c) {
    int i;
    M32(0x80B9EE4) = 1;   /* prn inited */
    M32(0x80B9EE8) = 1;   /* primary printer = 1 */
    M32(0x80B9EF0) = 1;   /* printers online */
    /* word_80B9EFE[i] is printer i's type (nonzero = installed); the real prn_init clears it for
       printers that fail to come up. Only printer 1 exists here. It must stay nonzero, or every
       ready-wait on it fails ("wait error/timeout"), it gets removed, and the next print reports
       "out of paper" and exits. */
    if (!M16(0x80B9F00)) M16(0x80B9F00) = 1;
    for (i = 2; i <= 8; i++) M16(0x80B9EFE + 2 * i) = 0;
    RET(0);
}

/* prn_status(pr): 0 = ready */
void ovr_prn_status(CPU *c) { RET(0); }

/* uprn print-server stand-in: the game normally forks /vend/bin/uprn and talks
 * "cmd args\n" / "<int> ...\n" over pipes. There is no printer, so answer locally. */
void ovr_uprn_open(CPU *c) { M32(0x80BAE6C) = 1; RET(0); }
void ovr_vsend_cmd(CPU *c) {
    const char *cmd = ARGS(0);
    char *reply = (char *)ARGP(3);
    int n = (int)ARG(4), v = 0;
    if (!strcmp(cmd, "apiversion")) v = 1002;   /* <=1002: no spool, no prn_errno queries */
    if (!strcmp(cmd, "print_generate")) {
        /* "%d %s %s": render <src> into the printer-ready <dst>; prn_generate() then renames dst over src.
           No printer to render for, so dst is a plain copy of src. */
        u32 va = ARG(2);
        char hs[MAX_PATH], hd[MAX_PATH];
        const char *src = (const char *)(uintptr_t)M32(va + 4), *dst = (const char *)(uintptr_t)M32(va + 8);
        if (rt_map_path(src, hs, sizeof hs) || rt_map_path(dst, hd, sizeof hd) || !CopyFileA(hs, hd, FALSE)) {
            rt_logf("uprn: print_generate %s -> %s failed\n", src, dst);
            v = -1;
        } else {
            extern void rt_save_print(const char *host_src);
            rt_save_print(hs);   /* the finished print goes to prints\ next to the exe */
        }
    }
    if (reply && n > 0) snprintf(reply, n, "%d;", v);
    rt_logf("uprn: %s -> %d\n", cmd, v);
    RET(v);
}

/* dict.db key (recovered by known plaintext, see dongle/dictkey.py) */
static const u8 dict_key[16] = { 0x5c, 0x31, 0x9a, 0x8e, 0x73, 0xef, 0x77, 0x76, 0x4c, 0x56, 0x5d, 0x87, 0x8c, 0xc9, 0x18, 0xfd };
void ovr_dict_key(CPU *c) {
    int i;
    rt_call_guest(c, 0x8082DBA, 0, 0);   /* flush dictionary cache, as the original does */
    for (i = 0; i < 32; i++) M8(0x80BC100 + i) = i < 16 ? dict_key[i] : 0;
    M32(0x80BC054) = 0x80BC100;
    rt_logf("dict key installed\n");
    RET(0);
}

#define LOGIN_FN 0x806B8F0
static int solve_login(CPU *c) {
    u32 e2, e0;
    u32 args[4] = { 0x378, 0, 0, 0 };
    solving = 1;
    for (e2 = 0; e2 < 0x10000; e2++) {
        ee[61] = (u16)e2; ee[60] = 0;
        rt_call_guest(c, LOGIN_FN, 4, args);
        if (M16(0x80C1AF4) != 0xC9F4) continue;
        for (e0 = 0; e0 < 0x10000; e0++) {
            ee[60] = (u16)e0;
            rt_call_guest(c, LOGIN_FN, 4, args);
            if (M16(0x80C1AF2) == 0xEB36) {
                if (M16(0x80C1AF6) == 0x57E2) { solving = 0; return 1; }
                break;
            }
        }
    }
    solving = 0;
    return 0;
}

static void defaults(void) {
    memset(ee, 0, sizeof ee);
    ee[2] = 0x0001; ee[3] = 0x0348; ee[5] = 0x012C;
    ee[6] = 0xBA0A; ee[7] = 0x0804;
    ee[8] = 0x0100;
    ee[10] = 0x3BBF; ee[11] = 0x1547;
    ee[12] = 0x86BC; ee[13] = 0x372C;
    ee[14] = 0x0001; ee[62] = 0x0001; ee[63] = 0xA000;
}

void ovr_init(CPU *c) {
    FILE *f = fopen(IMG, "rb");
    int loaded = 0;
    if (f) { loaded = fread(ee, sizeof ee, 1, f) == 1; fclose(f); }
    if (!loaded) {
        defaults();
        if (solve_login(c)) rt_logf("dongle: solved login seed w60=%04x w61=%04x\n", ee[60], ee[61]);
        else rt_logf("dongle: FAILED to solve login seed\n");
        M32(0x80C1ACC) = 0;
        ee_save();
    }
    rt_logf(loaded ? "dongle: loaded image\n" : "dongle: fresh image\n");
}
