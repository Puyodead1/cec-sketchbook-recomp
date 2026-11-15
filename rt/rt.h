/* Runtime-internal declarations shared by the Win32 runtime sources. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"

/* argument i (0-based) of an import: esp points at the return address */
#define ARG(i) M32(c->r[4].d + 4 + 4 * (i))
#define ARGP(i) ((void *)(uintptr_t)ARG(i))
#define ARGS(i) ((char *)(uintptr_t)ARG(i))
#define ARGF64(i) MF64(c->r[4].d + 4 + 4 * (i))
/* return from an import: set eax, pop the return address */
#define RET(v) do { c->r[0].d = (u32)(v); c->r[4].d += 4; return; } while (0)
#define RET_VOID() do { c->r[4].d += 4; return; } while (0)
#define RET64(v) do { u64 _v = (u64)(v); c->r[0].d = (u32)_v; c->r[2].d = (u32)(_v >> 32); c->r[4].d += 4; return; } while (0)
#define ERR(e) do { c->err = (e); RET(-1); } while (0)

#define DECL_IMP(name) void IMP_##name(CPU *c)

extern CPU *rt_main_cpu;
extern DWORD rt_main_tid;
extern char rt_root[MAX_PATH];        /* host directory standing in for /vend */
extern FILE *rt_log;

void rt_logf(const char *fmt, ...);
void rt_fatal(CPU *c, const char *fmt, ...);
GFn rt_lookup(u32 addr);
void rt_call_guest(CPU *c, u32 addr, int n, const u32 *args);
u32 rt_host_callback(void (*fn)(CPU *c));   /* returns a fake guest address calling fn */
CPU *rt_new_cpu(void);
void rt_fpu_init(void);

/* signals */
#define GSIG_INT 2
#define GSIG_KILL 9
#define GSIG_SEGV 11
#define GSIG_PIPE 13
#define GSIG_ALRM 14
#define GSIG_TERM 15
#define GSIG_CHLD 17
void rt_raise(int sig);                  /* mark pending (async) */
int rt_wait_ms(CPU *c, DWORD ms);        /* sleep, interruptible: returns 1 if interrupted by a signal */
int rt_wait_us(CPU *c, u64 us);          /* same, microseconds (~0ull = forever) */
u64 rt_now_us(void);                     /* monotonic microseconds (performance counter) */
int rt_signal_interrupts(CPU *c);        /* nonzero if a deliverable signal is pending for c */
void rt_timers_init(void);
u32 rt_set_itimer(u32 value_us, u32 interval_us, u32 *old_value_us, u32 *old_interval_us);
void rt_sig_init(void);

/* paths / fds */
int rt_map_path(const char *guest, char *host, size_t n);   /* 0 ok, -1 not mappable (ENOENT) */
int rt_path_special(const char *guest);   /* DEVK_* or 0 */
enum { FD_FREE, FD_HOST, FD_UIB, FD_NULL, FD_TTY };
typedef struct { int kind; int hfd; int flags; } GFd;
#define MAXFD 256
extern GFd rt_fds[MAXFD];
int rt_fd_alloc(int from);
int rt_errno_from_host(int e);

/* uib */
void uib_reset(void);
int uib_write(const u8 *p, int n);
int uib_read(u8 *p, int n);
int uib_avail(void);
void uib_press(int n);
void uib_coin(int line);

/* x11 */
int x11_key_event(int down, unsigned vk, unsigned scan, unsigned ch);
void x11_cam_rect(int x, int y, int w, int h);
void x11_cam_mode(int live);
void x11_cam_frame(const u32 *px, int w, int h, int stride, int mirror, int vflip);
void x11_screen_size(int *w, int *h);

/* overrides */
void ovr_init(CPU *c);

/* libc helpers */
void rt_exit(CPU *c, int code);
