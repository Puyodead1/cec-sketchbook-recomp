/* Sound. The game plays its .wav files by vfork()+execvp("wavplay", "-a", [-e] -b|-f, file...) in
 * snd_play() (0x809E88A) and keeps the child's pid: effects in 0x80BE04C, music in 0x80BE050. It later
 * polls that pid with waitpid() to see whether a sound has finished and kill()s it to cut a sound short.
 * Here each play request is a host thread feeding waveOut; it is registered as a child (odd pid, never a
 * Windows pid) whose handle is signalled when playback ends, so waitpid()/kill() keep working. */
#include "rt.h"
#include <mmsystem.h>

int rt_kid_add(int pid, HANDLE h);   /* proc.c */

#define MAXSND 16
typedef struct {
    int pid;
    HANDLE done;               /* manual-reset event: playback finished */
    volatile LONG stop;
    int nfiles;
    char files[24][MAX_PATH];
} Snd;
static Snd *snds[MAXSND];
static CRITICAL_SECTION scs;
static int sinit, next_pid = 900001;

static int load_wav(const char *path, WAVEFORMATEX *wf, u8 **data, DWORD *len) {
    FILE *f = fopen(path, "rb");
    u8 hdr[12], ch[8];
    int have_fmt = 0;
    *data = 0;
    if (!f) return -1;
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) { fclose(f); return -1; }
    while (fread(ch, 1, 8, f) == 8) {
        DWORD n = *(DWORD *)(ch + 4);
        if (!memcmp(ch, "fmt ", 4)) {
            u8 fb[40] = { 0 };
            if (fread(fb, 1, n < sizeof fb ? n : sizeof fb, f) < 16) break;
            if (n > sizeof fb) fseek(f, (long)(n - sizeof fb), SEEK_CUR);
            memset(wf, 0, sizeof *wf);
            wf->wFormatTag = *(WORD *)fb;
            wf->nChannels = *(WORD *)(fb + 2);
            wf->nSamplesPerSec = *(DWORD *)(fb + 4);
            wf->nAvgBytesPerSec = *(DWORD *)(fb + 8);
            wf->nBlockAlign = *(WORD *)(fb + 12);
            wf->wBitsPerSample = *(WORD *)(fb + 14);
            have_fmt = 1;
        } else if (!memcmp(ch, "data", 4) && have_fmt) {
            *data = (u8 *)malloc(n ? n : 1);
            *len = (DWORD)fread(*data, 1, n, f);   /* a truncated file plays what is there */
            fclose(f);
            return wf->wFormatTag == WAVE_FORMAT_PCM ? 0 : -1;
        } else fseek(f, (long)(n + (n & 1)), SEEK_CUR);
        if (n & 1 && !memcmp(ch, "fmt ", 4)) fseek(f, 1, SEEK_CUR);
    }
    fclose(f);
    return -1;
}

static void play_file(Snd *s, const char *path) {
    WAVEFORMATEX wf;
    WAVEHDR wh;
    HWAVEOUT wo;
    HANDLE ev;
    u8 *data;
    DWORD len = 0;
    if (load_wav(path, &wf, &data, &len)) { rt_logf("snd: cannot play %s\n", path); free(data); return; }
    rt_logf("snd: %s (%lu Hz, %u ch, %u bit, %lu ms)\n", path, wf.nSamplesPerSec, wf.nChannels, wf.wBitsPerSample,
            wf.nAvgBytesPerSec ? (unsigned long)((u64)len * 1000 / wf.nAvgBytesPerSec) : 0ul);
    ev = CreateEventA(0, FALSE, FALSE, 0);
    if (waveOutOpen(&wo, WAVE_MAPPER, &wf, (DWORD_PTR)ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        rt_logf("snd: waveOutOpen failed for %s\n", path);
        CloseHandle(ev); free(data); return;
    }
    memset(&wh, 0, sizeof wh);
    wh.lpData = (LPSTR)data;
    wh.dwBufferLength = len;
    waveOutPrepareHeader(wo, &wh, sizeof wh);
    waveOutWrite(wo, &wh, sizeof wh);
    while (!(wh.dwFlags & WHDR_DONE)) {
        if (s->stop) { waveOutReset(wo); break; }
        WaitForSingleObject(ev, 20);
    }
    while (!(wh.dwFlags & WHDR_DONE)) Sleep(1);
    waveOutUnprepareHeader(wo, &wh, sizeof wh);
    waveOutClose(wo);
    CloseHandle(ev);
    free(data);
}

static DWORD WINAPI snd_thread(void *arg) {
    Snd *s = (Snd *)arg;
    int i;
    for (i = 0; i < s->nfiles && !s->stop; i++) play_file(s, s->files[i]);
    EnterCriticalSection(&scs);
    for (i = 0; i < MAXSND; i++) if (snds[i] == s) snds[i] = 0;
    LeaveCriticalSection(&scs);
    SetEvent(s->done);   /* the child table owns the handle from here on */
    free(s);
    return 0;
}

/* kill() on a sound pid: stop it. 0 = was a sound */
int rt_snd_kill(int pid) {
    int i, r = -1;
    if (!sinit) return -1;
    EnterCriticalSection(&scs);
    for (i = 0; i < MAXSND; i++) if (snds[i] && snds[i]->pid == pid) { snds[i]->stop = 1; r = 0; }
    LeaveCriticalSection(&scs);
    return r;
}

static int snd_start(u32 files, int n) {
    Snd *s = (Snd *)calloc(1, sizeof(Snd));
    HANDLE dup;
    int i, slot;
    if (!sinit) { sinit = 1; InitializeCriticalSection(&scs); }
    for (i = 0; i < n && s->nfiles < 24; i++) {
        const char *g = (const char *)(uintptr_t)M32(files + 4 * i);
        if (g && !rt_map_path(g, s->files[s->nfiles], MAX_PATH)) s->nfiles++;
    }
    EnterCriticalSection(&scs);
    for (slot = 0; slot < MAXSND && snds[slot]; slot++) ;
    if (slot == MAXSND) { LeaveCriticalSection(&scs); free(s); return -1; }
    s->pid = next_pid; next_pid += 2;
    s->done = CreateEventA(0, TRUE, FALSE, 0);
    snds[slot] = s;
    LeaveCriticalSection(&scs);
    DuplicateHandle(GetCurrentProcess(), s->done, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CloseHandle(s->done);
    s->done = dup;
    if (rt_kid_add(s->pid, dup)) { /* no room in the child table: play anyway, untracked */ }
    i = s->pid;
    CloseHandle(CreateThread(0, 0, snd_thread, s, 0, 0));
    return i;
}

/* snd_play(char **files, int n, int music) */
void ovr_snd_play(CPU *c) {
    u32 files = ARG(0), music = ARG(2);
    int n = (int)ARG(1), pid;
    if (!M32(0x80BE044)) RET(0);   /* sound off */
    if (!music) {
        if (M32(0x80BE04C)) {   /* previous effect still going: the original waits up to 2 s for it */
            u32 a = 2000;
            rt_call_guest(c, 0x809E7A6, 1, &a);
            if (c->r[0].d) RET(-1);
        }
        if (M32(0x80BE048)) {   /* hook that may take the request over */
            rt_call_guest(c, M32(0x80BE048), 0, 0);
            if (c->r[0].d) RET(0);
        }
    }
    pid = snd_start(files, n);
    if (pid < 0) RET(-1);
    M32(music ? 0x80BE050 : 0x80BE04C) = (u32)pid;
    RET(0);
}
