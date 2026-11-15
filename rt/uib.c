/* In-process UIB button/coin box emulator on the guest's /dev/uib fd (port of uibemu.py). */
#include "rt.h"

#define PROTO 23
#define SEED 0x3021

static CRITICAL_SECTION cs;
static u8 inbuf[256]; static int nin;
static u8 outbuf[4096]; static int nout;
static u8 last_msgid, last_cmd = 4, last_res[8];
static volatile LONG buttons;
static u8 coins[8];
static u16 nonce = 0x1234;

typedef struct { u32 a, b, c; } WH;
static void wh_init(WH *g, u32 s) { g->a = g->b = g->c = s & 0xffff; }
static u16 wh_next(WH *g) {
    g->c = (g->c * 0xAB) % 0x763D;
    g->b = (g->b * 0xAC) % 0x7663;
    g->a = (g->a * 0xAA) % 0x7673;
    return (u16)((g->a ^ g->b ^ g->c) & 0xffff);
}
static void crypt14(u8 *buf, u16 iv, int enc) {
    WH g;
    u16 chain = iv;
    int i;
    wh_init(&g, SEED);
    wh_next(&g);
    for (i = 0; i + 1 < 14; i += 2) {
        u16 k = wh_next(&g), w = (u16)((buf[i] << 8) | buf[i + 1]), o;
        if (enc) { o = (u16)(k ^ chain ^ (u16)~w); chain = o; }
        else { o = (u16)~(k ^ chain ^ w); chain = w; }
        buf[i] = o >> 8; buf[i + 1] = o & 0xff;
    }
}
static u16 checksum(const u8 *b) {
    u16 v = 0;
    int i;
    for (i = 0; i < 10; i++) v = (u16)(((v >> 1) | (v << 15)) + b[i]);
    return v;
}
static void emit(u8 msgid, u8 cmd, const u8 *payload) {
    u8 p[18];
    u16 cs16;
    memset(p, 0, sizeof p);
    nonce = (u16)(nonce * 1009);
    if (!nonce) nonce = 1;
    p[8] = msgid; p[9] = cmd;
    if (payload) memcpy(p + 10, payload, 8);
    cs16 = checksum(p + 8);
    p[6] = cs16 >> 8; p[7] = cs16 & 0xff;
    p[0] = 0x3C; p[1] = PROTO; p[2] = nonce >> 8; p[3] = nonce & 0xff;
    crypt14(p + 4, nonce, 1);
    if (nout + 18 <= (int)sizeof outbuf) { memcpy(outbuf + nout, p, 18); nout += 18; }
}

static void result_for(u8 cmd, const u8 *pl, u8 *r) {
    int i, any = 0;
    memset(r, 0, 8);
    switch (cmd) {
    case 11: r[0] = 1; r[1] = 4; r[2] = 0; r[3] = PROTO; break;
    case 12: memset(r, 0xff, 8); break;
    case 13:
        for (i = 0; i < 8; i++) any |= coins[i];
        r[0] = any ? 0x20 : 0;
        r[1] = buttons ? 1 : 0;
        break;
    case 17: memcpy(r, coins, 8); memset(coins, 0, 8); break;
    case 18: {
        LONG b = InterlockedExchange(&buttons, 0);
        r[0] = b & 0xff; r[1] = (b >> 8) & 0xff;
        break;
    }
    case 9: case 27: memcpy(r, pl, 8); break;
    }
}

static void handle(const u8 *frame) {
    u8 p[18], res[8];
    u16 cs16;
    memcpy(p, frame, 18);
    if (p[0] != 0x3C) { emit(0, 3, 0); return; }
    crypt14(p + 4, (u16)((p[2] << 8) | p[3]), 0);
    cs16 = checksum(p + 8);
    if (cs16 != (u16)((p[6] << 8) | p[7])) { rt_logf("uib: bad checksum\n"); emit(0, 3, 0); return; }
    if (getenv("UIB_TRACE")) rt_logf("uib frame msgid %d cmd %d\n", p[8], p[9]);
    if (p[9] == 4) { emit(last_msgid, 1, last_res); return; }   /* STROBE */
    result_for(p[9], p + 10, res);
    last_msgid = p[8]; last_cmd = p[9];
    memcpy(last_res, res, 8);
    if (p[9] != 13 && p[9] != 4)
        rt_logf("uib: cmd %d msgid %d -> %02x%02x%02x%02x%02x%02x%02x%02x\n", p[9], p[8],
                res[0], res[1], res[2], res[3], res[4], res[5], res[6], res[7]);
    emit(p[8], 1, 0);
}

void uib_reset(void) {
    InitializeCriticalSection(&cs);
    nonce = (u16)(GetTickCount() | 1);
}

int uib_write(const u8 *p, int n) {
    int i;
    EnterCriticalSection(&cs);
    if (getenv("UIB_TRACE")) { rt_logf("uib W %d:", n); for (i = 0; i < n && i < 18; i++) rt_logf(" %02x", p[i]); rt_logf("\n"); }
    for (i = 0; i < n; i++) {
        if (nin < (int)sizeof inbuf) inbuf[nin++] = p[i];
        if (nin == 18) { handle(inbuf); nin = 0; }
    }
    LeaveCriticalSection(&cs);
    return n;
}

int uib_read(u8 *p, int n) {
    int k;
    EnterCriticalSection(&cs);
    k = n < nout ? n : nout;
    memcpy(p, outbuf, k);
    memmove(outbuf, outbuf + k, nout - k);
    nout -= k;
    if (getenv("UIB_TRACE")) rt_logf("uib R want %d got %d\n", n, k);
    LeaveCriticalSection(&cs);
    return k;
}

int uib_avail(void) { return nout; }

void uib_press(int n) {
    __sync_fetch_and_or(&buttons, 1L << (n - 1));
    rt_logf("uib: button %d pressed\n", n);
}

void uib_coin(int line) {
    EnterCriticalSection(&cs);
    if (coins[line - 1] < 255) coins[line - 1]++;
    LeaveCriticalSection(&cs);
    rt_logf("uib: coin on line %d\n", line);
}
