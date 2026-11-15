/* "Printing": there is no photo printer, so every finished print is saved as a PNG in a "prints" folder
 * next to the exe (print_YYYYMMDD_HHMMSS.png). The game's print images are TGA (8-bit grey for the sketches); they are decoded here and
 * written with GDI+. If that fails the TGA itself is copied there. */
#include "rt.h"

static u8 *load_tga(const char *path, int *pw, int *ph) {
    FILE *f = fopen(path, "rb");
    u8 h[18], *out = 0, *src = 0;
    long sz;
    int w, hgt, bpp, type, top, Bpp, i, n;
    if (!f) return 0;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 18 || fread(h, 1, 18, f) != 18) { fclose(f); return 0; }
    type = h[2]; w = h[12] | h[13] << 8; hgt = h[14] | h[15] << 8; bpp = h[16]; top = (h[17] & 0x20) != 0;
    Bpp = bpp / 8;
    if (!(((type == 2 || type == 10) && (bpp == 16 || bpp == 24 || bpp == 32)) || ((type == 3 || type == 11) && bpp == 8))
        || w <= 0 || hgt <= 0 || w > 16384 || hgt > 16384) {   /* true colour or greyscale (the sketch prints are 8-bit grey) */
        fclose(f); return 0;
    }
    fseek(f, 18 + h[0] + (h[1] ? (h[5] | h[6] << 8) * ((h[7] + 7) / 8) : 0), SEEK_SET);
    n = w * hgt;
    src = (u8 *)malloc((size_t)n * Bpp);
    out = (u8 *)malloc((size_t)n * 4);
    if (!src || !out) goto fail;
    if (type == 2 || type == 3) {
        if (fread(src, Bpp, (size_t)n, f) != (size_t)n) goto fail;
    } else {   /* RLE */
        for (i = 0; i < n;) {
            int c = fgetc(f), k, cnt;
            u8 px[4];
            if (c == EOF) goto fail;
            cnt = (c & 0x7f) + 1;
            if (i + cnt > n) cnt = n - i;
            if (c & 0x80) {
                if (fread(px, 1, Bpp, f) != (size_t)Bpp) goto fail;
                for (k = 0; k < cnt; k++) memcpy(src + (size_t)(i + k) * Bpp, px, Bpp);
            } else if (fread(src + (size_t)i * Bpp, Bpp, cnt, f) != (size_t)cnt) goto fail;
            i += cnt;
        }
    }
    fclose(f);
    for (i = 0; i < n; i++) {   /* -> top-down BGRA */
        int x = i % w, y = i / w, dy = top ? y : hgt - 1 - y;
        const u8 *s = src + (size_t)i * Bpp;
        u8 *d = out + ((size_t)dy * w + x) * 4;
        if (Bpp == 1) {
            d[0] = d[1] = d[2] = s[0];
        } else if (Bpp == 2) {
            u16 v = (u16)(s[0] | s[1] << 8);
            d[0] = (u8)((v & 31) * 255 / 31); d[1] = (u8)(((v >> 5) & 31) * 255 / 31); d[2] = (u8)(((v >> 10) & 31) * 255 / 31);
        } else { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
        d[3] = 255;
    }
    free(src);
    *pw = w; *ph = hgt;
    return out;
fail:
    fclose(f); free(src); free(out);
    return 0;
}

typedef struct { UINT32 ver; void *cb; BOOL nobg, noext; } GpStartIn;
static int save_png(const u8 *bgra, int w, int h, const char *path) {
    static const GUID png = { 0x557CF406, 0x1A04, 0x11D3, { 0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E } };
    HMODULE m = LoadLibraryA("gdiplus.dll");
    int (WINAPI *Startup)(ULONG_PTR *, const GpStartIn *, void *);
    int (WINAPI *FromScan0)(INT, INT, INT, INT, BYTE *, void **);
    int (WINAPI *Save)(void *, const WCHAR *, const GUID *, const void *);
    int (WINAPI *Dispose)(void *);
    ULONG_PTR tok; GpStartIn in = { 1, 0, 0, 0 };
    WCHAR wp[MAX_PATH];
    void *bmp = 0;
    int ok;
    if (!m) return -1;
    Startup = (void *)GetProcAddress(m, "GdiplusStartup");
    FromScan0 = (void *)GetProcAddress(m, "GdipCreateBitmapFromScan0");
    Save = (void *)GetProcAddress(m, "GdipSaveImageToFile");
    Dispose = (void *)GetProcAddress(m, "GdipDisposeImage");
    if (!Startup || !FromScan0 || !Save || !Dispose || Startup(&tok, &in, 0)) return -1;
    if (FromScan0(w, h, w * 4, 0x26200A /* PixelFormat32bppARGB */, (BYTE *)bgra, &bmp) || !bmp) return -1;
    MultiByteToWideChar(CP_ACP, 0, path, -1, wp, MAX_PATH);
    ok = Save(bmp, wp, &png, 0) == 0;
    Dispose(bmp);
    return ok ? 0 : -1;
}

/* host path of a finished print image -> prints\print_<time>.png next to the exe */
void rt_save_print(const char *host_src) {
    char dir[MAX_PATH], out[MAX_PATH], *p;
    SYSTEMTIME t;
    u8 *px;
    int w, h, k;
    GetModuleFileNameA(0, dir, sizeof dir);
    if ((p = strrchr(dir, '\\'))) *p = 0;
    strncat(dir, "\\prints", sizeof dir - strlen(dir) - 1);
    CreateDirectoryA(dir, 0);
    GetLocalTime(&t);
    for (k = 0; k < 100; k++) {   /* never overwrite an earlier print */
        snprintf(out, sizeof out, k ? "%s\\print_%04d%02d%02d_%02d%02d%02d_%d.png" : "%s\\print_%04d%02d%02d_%02d%02d%02d.png",
                 dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, k);
        if (GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES) break;
    }
    if ((px = load_tga(host_src, &w, &h)) && save_png(px, w, h, out) == 0) {
        rt_logf("print saved: %s (%dx%d)\n", out, w, h);
    } else {
        strcpy(out + strlen(out) - 4, ".tga");
        if (CopyFileA(host_src, out, TRUE)) rt_logf("print saved as TGA: %s\n", out);
        else rt_logf("print: could not save %s (%lu)\n", host_src, GetLastError());
    }
    free(px);
}
