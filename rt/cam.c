/* Camera: stands in for the booth's Brooktree (bttv) capture card.
 *
 * The game's plain-X11 video driver (-vt1) has no camera ops, so we give it the ones the bttv driver (-vt2) uses:
 * vt1's driver object gets slots +44 (capture mode), +48 (picture adjust), +52 (video window), +56 (input source)
 * pointed at the vt2 entry points, which are overridden here (vt2 itself is never used).
 *
 * The card overlays live video into the framebuffer underneath the UI; x11.c mimics that with a video buffer and
 * a per-pixel "shows video" mask (see the overlay notes there); the game's own screen reads see the composite.
 *
 * Frame source:
 *   RT_CAMERA=<image file>        a still picture (BMP/JPG/PNG/GIF via GDI+)
 *   RT_CAMERA_DEVICE=<name part>  a DirectShow camera by name, e.g. "OBS" for the OBS Virtual Camera
 *   otherwise                     the first real webcam (DirectShow), else the first camera of any kind,
 *                                 else Media Foundation
 * RT_CAMERA_MIRROR=1 mirrors the picture, RT_CAMERA_FLIP=1 flips it vertically.
 * A picture dropped on the game window replaces whatever the source is from then on (rt_cam_use_image). */
#include "rt.h"
#include <ctype.h>
__declspec(dllimport) HRESULT WINAPI CoInitializeEx(void *, DWORD);
__declspec(dllimport) void WINAPI CoTaskMemFree(void *);
__declspec(dllimport) void *WINAPI CoTaskMemAlloc(SIZE_T);

/* ---------------- frame source thread */
static volatile LONG cam_started;
static int mirror, vflip;

typedef struct { u32 *px; int w, h; } StillImg;
static StillImg *volatile dropped;   /* set: the camera shows this picture instead of its own frames */

static void push(const u32 *px, int w, int h, int stride) {
    static DWORD t0;
    static int nf, nlog;
    DWORD t = GetTickCount();
    if (dropped) return;
    x11_cam_frame(px, w, h, stride, mirror, vflip);
    if (!t0) t0 = t;
    if (++nf, t - t0 >= 10000 && nlog < 6) { rt_logf("cam: %.1f fps, %dx%d\n", nf * 1000.0 / (t - t0), w, h); nlog++; nf = 0; t0 = t; }
}

/* GDI+ flat API, loaded at runtime */
typedef struct { UINT32 ver; void *cb; BOOL nobg, noext; } GpStartIn;
typedef struct { UINT w, h; INT stride, fmt; void *scan0; UINT_PTR res; } GpData;
typedef struct { INT x, y, w, h; } GpRect;
static u32 *load_image(const char *path, int *w, int *h) {
    HMODULE m = LoadLibraryA("gdiplus.dll");
    int (WINAPI *Startup)(ULONG_PTR *, const GpStartIn *, void *);
    int (WINAPI *FromFile)(const WCHAR *, void **);
    int (WINAPI *GetW)(void *, UINT *), (WINAPI *GetH)(void *, UINT *);
    int (WINAPI *Lock)(void *, const GpRect *, UINT, INT, GpData *), (WINAPI *Unlock)(void *, GpData *);
    ULONG_PTR tok; GpStartIn in = { 1, 0, 0, 0 }; GpData d; GpRect r;
    WCHAR wp[MAX_PATH]; void *bmp = 0; UINT bw = 0, bh = 0; u32 *out = 0; UINT y;
    if (!m) return 0;
    Startup = (void *)GetProcAddress(m, "GdiplusStartup");
    FromFile = (void *)GetProcAddress(m, "GdipCreateBitmapFromFile");
    GetW = (void *)GetProcAddress(m, "GdipGetImageWidth");
    GetH = (void *)GetProcAddress(m, "GdipGetImageHeight");
    Lock = (void *)GetProcAddress(m, "GdipBitmapLockBits");
    Unlock = (void *)GetProcAddress(m, "GdipBitmapUnlockBits");
    if (!Startup || !FromFile || !GetW || !GetH || !Lock || !Unlock || Startup(&tok, &in, 0)) return 0;
    MultiByteToWideChar(CP_ACP, 0, path, -1, wp, MAX_PATH);
    if (FromFile(wp, &bmp) || !bmp) return 0;
    GetW(bmp, &bw); GetH(bmp, &bh);
    r.x = 0; r.y = 0; r.w = (INT)bw; r.h = (INT)bh;
    if (!bw || !bh || Lock(bmp, &r, 1 /* read */, 0x00022009 /* 32bppRGB */, &d)) return 0;
    out = (u32 *)malloc((size_t)bw * bh * 4);
    if (out) for (y = 0; y < bh; y++) memcpy(out + (size_t)y * bw, (u8 *)d.scan0 + (long)y * d.stride, bw * 4);
    Unlock(bmp, &d);
    *w = (int)bw; *h = (int)bh;
    return out;
}

/* Media Foundation, loaded at runtime; COM methods called through their vtable slots */
#define VT(o, i) ((*(void ***)(o))[i])
#define REL(o) do { if (o) ((ULONG (WINAPI *)(void *))VT(o, 2))(o); } while (0)
static const GUID G_SOURCE_TYPE = { 0xc60ac5fe, 0x252a, 0x478f, { 0xa0, 0xef, 0xbc, 0x8f, 0xa5, 0xf7, 0xca, 0xd3 } };
static const GUID G_VIDCAP = { 0x8ac3587a, 0x4ae7, 0x42d8, { 0x99, 0xe0, 0x0a, 0x60, 0x13, 0xee, 0xf9, 0x0f } };
static const GUID G_VIDEO_PROCESSING = { 0xfb394f3d, 0xccf1, 0x42ee, { 0xbb, 0xb3, 0xf9, 0xb8, 0x45, 0xd5, 0x68, 0x1d } };
static const GUID G_MAJOR_TYPE = { 0x48eba18e, 0xf8c9, 0x4687, { 0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f } };
static const GUID G_SUBTYPE = { 0xf7e34c9a, 0x42e8, 0x4714, { 0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5 } };
static const GUID G_VIDEO = { 0x73646976, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID G_RGB32 = { 0x00000016, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID G_FRAME_SIZE = { 0x1652c33d, 0xd6b2, 0x4012, { 0xb8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xa3, 0x7d } };
static const GUID G_DEFAULT_STRIDE = { 0x644b4e48, 0x1e02, 0x4516, { 0xb0, 0xeb, 0xc0, 0x1c, 0xa9, 0xd4, 0x9a, 0xc6 } };
static const GUID IID_MediaSource = { 0x279a808d, 0xaec7, 0x40c8, { 0x9c, 0x6b, 0xa6, 0xb4, 0x92, 0xc7, 0x8a, 0x66 } };
#define FIRST_VIDEO 0xFFFFFFFCu
typedef HRESULT (WINAPI *SetGUIDf)(void *, const GUID *, const GUID *);
typedef HRESULT (WINAPI *SetU32f)(void *, const GUID *, UINT32);
typedef HRESULT (WINAPI *GetU32f)(void *, const GUID *, UINT32 *);
typedef HRESULT (WINAPI *GetU64f)(void *, const GUID *, UINT64 *);

static const GUID G_FRAME_RATE = { 0xc459a2e8, 0x3d2c, 0x4e44, { 0xb1, 0x32, 0xfe, 0xe5, 0x15, 0x6c, 0x7b, 0xb0 } };
static const GUID GUID_NULL_ = { 0 };

/* Media Foundation source reader loop: a video file (looped, paced to its timestamps) or the first webcam */
static int mf_loop(const char *file) {
    HMODULE plat = LoadLibraryA("mfplat.dll"), mf = LoadLibraryA("mf.dll"), rw = LoadLibraryA("mfreadwrite.dll");
    HRESULT (WINAPI *MFStartup)(ULONG, DWORD);
    HRESULT (WINAPI *MFCreateAttributes)(void **, UINT32);
    HRESULT (WINAPI *MFCreateMediaType)(void **);
    HRESULT (WINAPI *MFEnumDeviceSources)(void *, void ***, UINT32 *);
    HRESULT (WINAPI *MFCreateSourceReaderFromMediaSource)(void *, void *, void **);
    HRESULT (WINAPI *MFCreateSourceReaderFromURL)(const WCHAR *, void *, void **);
    void *attr = 0, **devs = 0, *src = 0, *rd = 0, *mt = 0, *cur = 0;
    UINT32 ndev = 0, i, sstride = 0;
    UINT64 fsz = 0;
    int w, h, stride;
    LONGLONG base_ts = -1;
    DWORD base_tick = 0;
    if (!plat || !mf || !rw) { rt_logf("cam: Media Foundation not available\n"); return -1; }
    MFStartup = (void *)GetProcAddress(plat, "MFStartup");
    MFCreateAttributes = (void *)GetProcAddress(plat, "MFCreateAttributes");
    MFCreateMediaType = (void *)GetProcAddress(plat, "MFCreateMediaType");
    MFEnumDeviceSources = (void *)GetProcAddress(mf, "MFEnumDeviceSources");
    MFCreateSourceReaderFromMediaSource = (void *)GetProcAddress(rw, "MFCreateSourceReaderFromMediaSource");
    MFCreateSourceReaderFromURL = (void *)GetProcAddress(rw, "MFCreateSourceReaderFromURL");
    if (!MFStartup || !MFCreateAttributes || !MFCreateMediaType || !MFEnumDeviceSources || !MFCreateSourceReaderFromMediaSource || !MFCreateSourceReaderFromURL) return -1;
    CoInitializeEx(0, 0 /* COINIT_MULTITHREADED */);
    if (FAILED(MFStartup(0x00020070, 0))) { rt_logf("cam: MFStartup failed\n"); return -1; }
    if (FAILED(MFCreateAttributes(&attr, 1))) return -1;
    ((SetU32f)VT(attr, 21))(attr, &G_VIDEO_PROCESSING, 1);   /* let the reader decode/convert to RGB32 */
    if (file) {
        WCHAR wp[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, file, -1, wp, MAX_PATH);
        if (FAILED(MFCreateSourceReaderFromURL(wp, attr, &rd))) { rt_logf("cam: cannot open video %s\n", file); return -1; }
    } else {
        void *ea = 0;
        MFCreateAttributes(&ea, 1);
        ((SetGUIDf)VT(ea, 24))(ea, &G_SOURCE_TYPE, &G_VIDCAP);
        if (FAILED(MFEnumDeviceSources(ea, &devs, &ndev)) || !ndev) { rt_logf("cam: no Media Foundation webcam found\n"); REL(ea); return -1; }
        REL(ea);
        rt_logf("cam: %u Media Foundation webcam(s), using the first\n", ndev);
        if (FAILED(((HRESULT (WINAPI *)(void *, const GUID *, void **))VT(devs[0], 33))(devs[0], &IID_MediaSource, &src))) {
            rt_logf("cam: cannot activate webcam\n"); return -1;
        }
        for (i = 0; i < ndev; i++) REL(devs[i]);
        CoTaskMemFree(devs);
        if (FAILED(MFCreateSourceReaderFromMediaSource(src, attr, &rd))) { rt_logf("cam: cannot create source reader\n"); return -1; }
        {   /* pick a native format near 640 wide at >= 15 fps; the default is often full resolution at 5 fps */
            int best = -1;
            long long bscore = 0;
            for (i = 0; i < 200; i++) {
                void *nt = 0;
                UINT64 sz = 0, fr = 0;
                long long nw, nh, fps, score;
                if (FAILED(((HRESULT (WINAPI *)(void *, DWORD, DWORD, void **))VT(rd, 5))(rd, FIRST_VIDEO, i, &nt))) break;
                ((GetU64f)VT(nt, 8))(nt, &G_FRAME_SIZE, &sz);
                ((GetU64f)VT(nt, 8))(nt, &G_FRAME_RATE, &fr);
                nw = (long long)(sz >> 32); nh = (long long)(sz & 0xffffffff);
                fps = (fr & 0xffffffff) ? (long long)((fr >> 32) / (fr & 0xffffffff)) : 30;
                score = (fps >= 15 ? 0 : 1LL << 40) + (nw >= 640 ? 0 : 1LL << 39) + llabs(nw - 640) * 1000000 + nh * 100 - fps;
                if (nw > 0 && (best < 0 || score < bscore)) { best = (int)i; bscore = score; }
                REL(nt);
            }
            if (best >= 0) {
                void *nt = 0;
                ((HRESULT (WINAPI *)(void *, DWORD, DWORD, void **))VT(rd, 5))(rd, FIRST_VIDEO, (DWORD)best, &nt);
                ((HRESULT (WINAPI *)(void *, DWORD, DWORD *, void *))VT(rd, 7))(rd, FIRST_VIDEO, 0, nt);
                REL(nt);
            }
        }
    }
    MFCreateMediaType(&mt);
    ((SetGUIDf)VT(mt, 24))(mt, &G_MAJOR_TYPE, &G_VIDEO);
    ((SetGUIDf)VT(mt, 24))(mt, &G_SUBTYPE, &G_RGB32);
    if (FAILED(((HRESULT (WINAPI *)(void *, DWORD, DWORD *, void *))VT(rd, 7))(rd, FIRST_VIDEO, 0, mt))) {
        rt_logf("cam: source cannot deliver RGB32\n"); return -1;
    }
    REL(mt);
    ((HRESULT (WINAPI *)(void *, DWORD, void **))VT(rd, 6))(rd, FIRST_VIDEO, &cur);
    ((GetU64f)VT(cur, 8))(cur, &G_FRAME_SIZE, &fsz);
    w = (int)(fsz >> 32); h = (int)(fsz & 0xffffffff);
    stride = w * 4;
    if (SUCCEEDED(((GetU32f)VT(cur, 7))(cur, &G_DEFAULT_STRIDE, &sstride))) stride = (int)sstride;
    REL(cur);
    rt_logf("cam: %s %dx%d stride %d\n", file ? "video" : "webcam", w, h, stride);
    if (w <= 0 || h <= 0) return -1;
    for (;;) {
        DWORD si = 0, flags = 0;
        LONGLONG ts = 0;
        void *smp = 0, *buf = 0;
        BYTE *p = 0;
        DWORD maxl = 0, len = 0;
        if (FAILED(((HRESULT (WINAPI *)(void *, DWORD, DWORD, DWORD *, DWORD *, LONGLONG *, void **))VT(rd, 9))(
                rd, FIRST_VIDEO, 0, &si, &flags, &ts, &smp))) { rt_logf("cam: ReadSample failed\n"); return -1; }
        if (flags & 2 /* end of stream */) {
            BYTE pv[16];
            REL(smp);
            if (!file) return -1;
            memset(pv, 0, sizeof pv); *(USHORT *)pv = 20; /* VT_I8 0: rewind and loop */
            ((HRESULT (WINAPI *)(void *, const GUID *, void *))VT(rd, 8))(rd, &GUID_NULL_, pv);
            base_ts = -1;
            continue;
        }
        if (!smp) continue;
        if (file) {   /* play at the file's own speed */
            DWORD now = GetTickCount();
            LONGLONG due;
            if (base_ts < 0) { base_ts = ts; base_tick = now; }
            due = (ts - base_ts) / 10000;
            if (due > (LONGLONG)(now - base_tick)) Sleep((DWORD)(due - (now - base_tick)));
        }
        if (SUCCEEDED(((HRESULT (WINAPI *)(void *, void **))VT(smp, 41))(smp, &buf)) && buf) {
            if (SUCCEEDED(((HRESULT (WINAPI *)(void *, BYTE **, DWORD *, DWORD *))VT(buf, 3))(buf, &p, &maxl, &len)) && p) {
                if (len >= (DWORD)(abs(stride) * h)) {
                    if (stride < 0) push((const u32 *)(p + (size_t)(-stride) * (h - 1)), w, h, stride / 4);
                    else push((const u32 *)p, w, h, stride / 4);
                }
                ((HRESULT (WINAPI *)(void *))VT(buf, 4))(buf);
            }
            REL(buf);
        }
        REL(smp);
    }
}
/* DirectShow: sees hardware webcams and virtual cameras (OBS, etc.) alike.
   Graph: camera -> SampleGrabber (RGB32, converter inserted as needed) -> NullRenderer; frames arrive in BufferCB. */
__declspec(dllimport) HRESULT WINAPI CoCreateInstance(const GUID *, void *, DWORD, const GUID *, void **);
static const GUID CLSID_SystemDeviceEnum = { 0x62be5d10, 0x60eb, 0x11d0, { 0xbd, 0x3b, 0x00, 0xa0, 0xc9, 0x11, 0xce, 0x86 } };
static const GUID IID_ICreateDevEnum = { 0x29840822, 0x5b84, 0x11d0, { 0xbd, 0x3b, 0x00, 0xa0, 0xc9, 0x11, 0xce, 0x86 } };
static const GUID CLSID_VideoInputCat = { 0x860bb310, 0x5d01, 0x11d0, { 0xbd, 0x3b, 0x00, 0xa0, 0xc9, 0x11, 0xce, 0x86 } };
static const GUID IID_IPropertyBag_ = { 0x55272a00, 0x42cb, 0x11ce, { 0x81, 0x35, 0x00, 0xaa, 0x00, 0x4b, 0xb8, 0x51 } };
static const GUID IID_IBaseFilter_ = { 0x56a86895, 0x0ad4, 0x11ce, { 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 } };
static const GUID CLSID_FilterGraph_ = { 0xe436ebb3, 0x524f, 0x11ce, { 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 } };
static const GUID IID_IGraphBuilder_ = { 0x56a868a9, 0x0ad4, 0x11ce, { 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 } };
static const GUID CLSID_CaptureGraphBuilder2_ = { 0xbf87b6e1, 0x8c27, 0x11d0, { 0xb3, 0xf0, 0x00, 0xaa, 0x00, 0x37, 0x61, 0xc5 } };
static const GUID IID_ICaptureGraphBuilder2_ = { 0x93e5a4e0, 0x2d50, 0x11d2, { 0xab, 0xfa, 0x00, 0xa0, 0xc9, 0xc6, 0xe3, 0x8d } };
static const GUID IID_IMediaControl_ = { 0x56a868b1, 0x0ad4, 0x11ce, { 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 } };
static const GUID IID_IMediaFilter_ = { 0x56a86899, 0x0ad4, 0x11ce, { 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 } };
static const GUID CLSID_SampleGrabber_ = { 0xc1f400a0, 0x3f08, 0x11d3, { 0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37 } };
static const GUID IID_ISampleGrabber_ = { 0x6b652fff, 0x11fe, 0x4fce, { 0x92, 0xad, 0x02, 0x66, 0xb5, 0xd7, 0xc7, 0x8f } };
static const GUID IID_ISampleGrabberCB_ = { 0x0579154a, 0x2b53, 0x4994, { 0xb0, 0xd0, 0xe7, 0x73, 0x14, 0x8e, 0xff, 0x85 } };
static const GUID CLSID_NullRenderer_ = { 0xc1f400a4, 0x3f08, 0x11d3, { 0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37 } };
static const GUID MEDIASUBTYPE_RGB32_ = { 0xe436eb7e, 0x524f, 0x11ce, { 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 } };
static const GUID FORMAT_VideoInfo_ = { 0x05589f80, 0xc356, 0x11ce, { 0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a } };
static const GUID PIN_CATEGORY_CAPTURE_ = { 0xfb6c4281, 0x0353, 0x11d1, { 0x90, 0x5f, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba } };
static const GUID PIN_CATEGORY_PREVIEW_ = { 0xfb6c4282, 0x0353, 0x11d1, { 0x90, 0x5f, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba } };
static const GUID IID_IAMStreamConfig_ = { 0xc6e13340, 0x30ac, 0x11d0, { 0xa1, 0x8c, 0x00, 0xa0, 0xc9, 0x11, 0x89, 0x56 } };
static const GUID IID_IUnknown_ = { 0, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };

typedef struct {   /* AM_MEDIA_TYPE */
    GUID major, sub; BOOL fixed, temporal; ULONG ssize; GUID ftype; void *unk; ULONG cbfmt; BYTE *fmt;
} AMT;
/* VIDEOINFOHEADER: rcSource(16) rcTarget(16) bitrate(4) errrate(4) AvgTimePerFrame(8 @40) BITMAPINFOHEADER(@48) */
#define VIH_W(f) (*(LONG *)((f) + 52))
#define VIH_H(f) (*(LONG *)((f) + 56))
#define VIH_T(f) (*(LONGLONG *)((f) + 40))
static void amt_free(AMT *m) {
    if (!m) return;
    if (m->cbfmt) CoTaskMemFree(m->fmt);
    REL(m->unk);
    CoTaskMemFree(m);
}

static int ds_w, ds_h;
static HRESULT WINAPI cb_qi(void *s, const GUID *iid, void **pp) {
    if (!memcmp(iid, &IID_IUnknown_, 16) || !memcmp(iid, &IID_ISampleGrabberCB_, 16)) { *pp = s; return 0; }
    *pp = 0;
    return E_NOINTERFACE;
}
static ULONG WINAPI cb_ref(void *s) { (void)s; return 2; }
static HRESULT WINAPI cb_sample(void *s, double t, void *smp) { (void)s; (void)t; (void)smp; return 0; }
static HRESULT WINAPI cb_buffer(void *s, double t, BYTE *buf, long len) {
    static int first = 1;
    (void)s; (void)t;
    if (first) { first = 0; rt_logf("cam: first frame, %ld bytes\n", len); }
    if (buf && ds_w > 0 && ds_h > 0 && len >= ds_w * ds_h * 4)   /* bottom-up DIB */
        push((const u32 *)(buf + (size_t)ds_w * 4 * (ds_h - 1)), ds_w, ds_h, -ds_w);
    return 0;
}
static void *cb_vt[] = { (void *)cb_qi, (void *)cb_ref, (void *)cb_ref, (void *)cb_sample, (void *)cb_buffer };
static struct { void **vt; } grab_cb = { cb_vt };

static int prop_str(void *mon, const WCHAR *name, char *out, int n) {
    HRESULT (WINAPI *VariantClear_)(void *) = (void *)GetProcAddress(LoadLibraryA("oleaut32.dll"), "VariantClear");
    void *bag = 0;
    BYTE var[16];
    int ok = 0;
    if (FAILED(((HRESULT (WINAPI *)(void *, void *, void *, const GUID *, void **))VT(mon, 9))(mon, 0, 0, &IID_IPropertyBag_, &bag)) || !bag) return 0;
    memset(var, 0, sizeof var);
    if (SUCCEEDED(((HRESULT (WINAPI *)(void *, const WCHAR *, void *, void *))VT(bag, 3))(bag, name, var, 0)) && *(USHORT *)var == 8 /* VT_BSTR */) {
        if (out) WideCharToMultiByte(CP_UTF8, 0, *(WCHAR **)(var + 8), -1, out, n, 0, 0);
        ok = 1;
    }
    if (VariantClear_) VariantClear_(var);
    REL(bag);
    return ok;
}

static void pick_format(void *cgb, void *src) {
    void *cfg = 0;
    int n = 0, sz = 0, i, best = -1;
    long long bscore = 0;
    BYTE caps[256];
    if (FAILED(((HRESULT (WINAPI *)(void *, const GUID *, const GUID *, void *, const GUID *, void **))VT(cgb, 6))(
            cgb, &PIN_CATEGORY_CAPTURE_, &G_VIDEO, src, &IID_IAMStreamConfig_, &cfg)) || !cfg) return;
    ((HRESULT (WINAPI *)(void *, int *, int *))VT(cfg, 5))(cfg, &n, &sz);
    for (i = 0; i < n && sz <= (int)sizeof caps; i++) {
        AMT *m = 0;
        if (FAILED(((HRESULT (WINAPI *)(void *, int, AMT **, BYTE *))VT(cfg, 6))(cfg, i, &m, caps)) || !m) continue;
        if (!memcmp(&m->ftype, &FORMAT_VideoInfo_, 16) && m->cbfmt >= 88) {
            long w = VIH_W(m->fmt), h = labs(VIH_H(m->fmt));
            long long t = VIH_T(m->fmt), fps = t > 0 ? 10000000 / t : 30;
            /* want: >= 15 fps, at least 640 wide but as small as possible, then the highest frame rate */
            long long score = (fps >= 15 ? 0 : 1LL << 40) + (w >= 640 ? 0 : 1LL << 39) + (long long)labs(w - 640) * 1000000 + h * 100 - fps;
            if (w > 0 && h > 0 && (best < 0 || score < bscore)) { best = i; bscore = score; }
        }
        amt_free(m);
    }
    if (best >= 0) {
        AMT *m = 0;
        if (SUCCEEDED(((HRESULT (WINAPI *)(void *, int, AMT **, BYTE *))VT(cfg, 6))(cfg, best, &m, caps)) && m) {
            HRESULT hr = ((HRESULT (WINAPI *)(void *, AMT *))VT(cfg, 3))(cfg, m);
            rt_logf("cam: format %ldx%ld @ %lld fps%s\n", VIH_W(m->fmt), labs(VIH_H(m->fmt)),
                    VIH_T(m->fmt) > 0 ? 10000000 / VIH_T(m->fmt) : 0LL, FAILED(hr) ? " (camera refused, using its default)" : "");
            amt_free(m);
        }
    }
    REL(cfg);
}

static int dshow_start(const char *want) {
    void *de = 0, *em = 0, *mon = 0, *pick = 0, *g = 0, *cgb = 0, *src = 0, *gf = 0, *sg = 0, *nul = 0, *mc = 0, *mf = 0;
    char name[256], pname[256] = "";
    int hw, pick_hw = 0;
    ULONG got;
    AMT req, *cur = 0;
    HRESULT hr;
    if (FAILED(CoCreateInstance(&CLSID_SystemDeviceEnum, 0, 1, &IID_ICreateDevEnum, &de))) return -1;
    if (((HRESULT (WINAPI *)(void *, const GUID *, void **, DWORD))VT(de, 3))(de, &CLSID_VideoInputCat, &em, 0) != 0 || !em) {
        REL(de); rt_logf("cam: no DirectShow cameras\n"); return -1;
    }
    while (((HRESULT (WINAPI *)(void *, ULONG, void **, ULONG *))VT(em, 3))(em, 1, &mon, &got) == 0 && got) {
        int take;
        if (!prop_str(mon, L"FriendlyName", name, sizeof name)) strcpy(name, "?");
        hw = prop_str(mon, L"DevicePath", 0, 0);   /* real (kernel streaming) devices have one, virtual ones don't */
        rt_logf("cam: found \"%s\"%s\n", name, hw ? "" : " (virtual)");
        if (want && *want) {
            char a[256], b[256]; int k;
            for (k = 0; name[k] && k < 255; k++) a[k] = (char)tolower((unsigned char)name[k]);
            a[k] = 0;
            for (k = 0; want[k] && k < 255; k++) b[k] = (char)tolower((unsigned char)want[k]);
            b[k] = 0;
            take = !pick && strstr(a, b);
        } else take = !pick || (hw && !pick_hw);   /* default: first real webcam, else first device */
        if (take) { REL(pick); pick = mon; pick_hw = hw; strcpy(pname, name); }
        else REL(mon);
        mon = 0;
    }
    REL(em); REL(de);
    if (!pick) { rt_logf(want && *want ? "cam: no camera matching \"%s\"\n" : "cam: no DirectShow cameras\n", want); return -1; }
    rt_logf("cam: using \"%s\"\n", pname);

    hr = ((HRESULT (WINAPI *)(void *, void *, void *, const GUID *, void **))VT(pick, 8))(pick, 0, 0, &IID_IBaseFilter_, &src);
    REL(pick);
    if (FAILED(hr) || !src) { rt_logf("cam: cannot open camera (%08lx)\n", hr); return -1; }
    CoCreateInstance(&CLSID_FilterGraph_, 0, 1, &IID_IGraphBuilder_, &g);
    CoCreateInstance(&CLSID_CaptureGraphBuilder2_, 0, 1, &IID_ICaptureGraphBuilder2_, &cgb);
    CoCreateInstance(&CLSID_SampleGrabber_, 0, 1, &IID_IBaseFilter_, &gf);
    CoCreateInstance(&CLSID_NullRenderer_, 0, 1, &IID_IBaseFilter_, &nul);
    if (!g || !cgb || !gf || !nul) { rt_logf("cam: DirectShow components missing (qedit SampleGrabber?)\n"); return -1; }
    ((HRESULT (WINAPI *)(void *, void *))VT(cgb, 3))(cgb, g);
    ((HRESULT (WINAPI *)(void *, void *, const WCHAR *))VT(g, 3))(g, src, L"camera");
    ((HRESULT (WINAPI *)(void *, void *, const WCHAR *))VT(g, 3))(g, gf, L"grabber");
    ((HRESULT (WINAPI *)(void *, void *, const WCHAR *))VT(g, 3))(g, nul, L"null");
    ((HRESULT (WINAPI *)(void *, const GUID *, void **))VT(gf, 0))(gf, &IID_ISampleGrabber_, &sg);
    if (!sg) return -1;
    memset(&req, 0, sizeof req);
    req.major = G_VIDEO; req.sub = MEDIASUBTYPE_RGB32_; req.ftype = FORMAT_VideoInfo_;
    ((HRESULT (WINAPI *)(void *, AMT *))VT(sg, 4))(sg, &req);
    if (pick_hw) pick_format(cgb, src);   /* virtual cameras (OBS...) stream in their own fixed format */
    hr = ((HRESULT (WINAPI *)(void *, const GUID *, const GUID *, void *, void *, void *))VT(cgb, 7))(cgb, &PIN_CATEGORY_CAPTURE_, &G_VIDEO, src, gf, nul);
    if (FAILED(hr)) hr = ((HRESULT (WINAPI *)(void *, const GUID *, const GUID *, void *, void *, void *))VT(cgb, 7))(cgb, &PIN_CATEGORY_PREVIEW_, &G_VIDEO, src, gf, nul);
    if (FAILED(hr)) hr = ((HRESULT (WINAPI *)(void *, const GUID *, const GUID *, void *, void *, void *))VT(cgb, 7))(cgb, 0, &G_VIDEO, src, gf, nul);
    if (FAILED(hr)) { rt_logf("cam: cannot connect camera to RGB32 (%08lx)\n", hr); return -1; }
    cur = (AMT *)CoTaskMemAlloc(sizeof *cur);
    memset(cur, 0, sizeof *cur);
    if (FAILED(((HRESULT (WINAPI *)(void *, AMT *))VT(sg, 5))(sg, cur)) || cur->cbfmt < 88) { rt_logf("cam: no connected format\n"); return -1; }
    ds_w = VIH_W(cur->fmt); ds_h = VIH_H(cur->fmt);
    if (ds_h < 0) ds_h = -ds_h;   /* top-down RGB32 is not offered by the grabber in practice */
    amt_free(cur);
    rt_logf("cam: streaming %dx%d RGB32\n", ds_w, ds_h);
    ((HRESULT (WINAPI *)(void *, BOOL))VT(sg, 3))(sg, FALSE);   /* SetOneShot */
    ((HRESULT (WINAPI *)(void *, BOOL))VT(sg, 6))(sg, FALSE);   /* SetBufferSamples */
    ((HRESULT (WINAPI *)(void *, void *, long))VT(sg, 9))(sg, &grab_cb, 1);
    (void)mf;   /* keep the graph's reference clock: OBS's virtual camera paces its output against it */
    ((HRESULT (WINAPI *)(void *, const GUID *, void **))VT(g, 0))(g, &IID_IMediaControl_, &mc);
    if (!mc || FAILED(hr = ((HRESULT (WINAPI *)(void *))VT(mc, 7))(mc))) { rt_logf("cam: cannot start camera graph (%08lx)\n", hr); return -1; }
    {
        long st = -1;   /* IMediaControl::GetState(timeout, &state): 2 = running */
        HRESULT h2 = ((HRESULT (WINAPI *)(void *, long, long *))VT(mc, 9 + 1))(mc, 5000, &st);
        rt_logf("cam: graph run %08lx, state %ld (%08lx)\n", hr, st, h2);
    }
    return 0;   /* graph objects are kept alive for the life of the process */
}

static DWORD WINAPI dropped_thread(void *arg) {
    (void)arg;
    for (;;) {
        StillImg *s = dropped;
        if (s) x11_cam_frame(s->px, s->w, s->h, s->w, 0, 0);
        Sleep(100);
    }
}

/* show this picture in place of the camera; 0 = ok */
int rt_cam_use_image(const char *path) {
    static volatile LONG thread_started;
    StillImg *s = (StillImg *)calloc(1, sizeof *s);
    if (!s || !(s->px = load_image(path, &s->w, &s->h))) { free(s); rt_logf("cam: %s is not a picture\n", path); return -1; }
    InterlockedExchangePointer((void *volatile *)&dropped, s);   /* the old one is leaked: frames may still read it */
    rt_logf("cam: now showing %s (%dx%d)\n", path, s->w, s->h);
    if (!InterlockedExchange(&thread_started, 1)) CloseHandle(CreateThread(0, 0, dropped_thread, 0, 0, 0));
    return 0;
}

static DWORD WINAPI cam_thread(void *arg) {
    const char *img = getenv("RT_CAMERA"), *dev = getenv("RT_CAMERA_DEVICE");
    (void)arg;
    if (img && *img) {
        int w, h;
        u32 *px = load_image(img, &w, &h);
        if (!px) {   /* not a picture: try it as a video file */
            mf_loop(img);
            rt_logf("cam: cannot use RT_CAMERA file %s\n", img);
        } else {
            rt_logf("cam: still image %s (%dx%d)\n", img, w, h);
            for (;;) { push(px, w, h, w); Sleep(100); }
        }
    }
    CoInitializeEx(0, 0 /* COINIT_MULTITHREADED */);
    if (dshow_start(dev) == 0) { for (;;) Sleep(INFINITE); }
    if (dev && *dev) return 0;
    mf_loop(0);
    rt_logf("cam: no frame source; the video window stays black\n");
    return 0;
}

static void cam_start(void) {
    if (InterlockedExchange(&cam_started, 1)) return;
    mirror = getenv("RT_CAMERA_MIRROR") && atoi(getenv("RT_CAMERA_MIRROR"));
    vflip = getenv("RT_CAMERA_FLIP") && atoi(getenv("RT_CAMERA_FLIP"));
    CloseHandle(CreateThread(0, 0, cam_thread, 0, 0, 0));
}

/* ---------------- driver ops (guest-visible) */
#define DRV(i) M32(drv + 4 * (i))   /* driver object as u32[]: [17] live, [19] flag, [35..38] video rect */
#define LIVE 0x80BD834u             /* the bttv driver's global "live" flag */

static void set_rect(u32 drv) { x11_cam_rect((s32)DRV(35), (s32)DRV(36), (s32)DRV(37) - (s32)DRV(35), (s32)DRV(38) - (s32)DRV(36)); }

/* sub_808D630(a, b, c): vt1 driver setup; same as the original plus the camera slots */
void ovr_vt1_setup(CPU *c) {
    u32 a[4] = { ARG(0), ARG(1), ARG(2), 0 }, s;
    rt_call_guest(c, 0x808D626, 0, 0);   /* driver object */
    s = c->r[0].d;
    rt_call_guest(c, 0x808D58A, 1, &s);  /* fill vt1 ops */
    M32(s + 44) = 0x80938D3;             /* camera mode */
    M32(s + 48) = 0x8093A1F;             /* picture adjustments */
    M32(s + 52) = 0x8093A80;             /* video window */
    M32(s + 56) = 0x8093D4B;             /* input source */
    a[3] = a[2]; a[2] = a[1]; a[1] = a[0]; a[0] = s;
    rt_call_guest(c, 0x8086D71, 4, a);
    RET(c->r[0].d);
}

/* (drv, set, int *a): a[0] = 0 stop, 1 live, 2 snap, 4 freeze, 5/6 flag, 7/8 capture settings */
void ovr_cam_ctl(CPU *c) {
    u32 drv = ARG(0), a = ARG(2);
    if (!ARG(1)) { M32(a) = M32(LIVE); M32(a + 4) = 1; M32(a + 8) = 1; RET(0); }
    switch ((s32)M32(a)) {
    case -1: case 3: case 7: case 8: break;
    case 0: case 2: case 4:
        if (DRV(17)) rt_logf("cam: %s\n", M32(a) ? "freeze" : "stop");
        x11_cam_mode(0);
        DRV(17) = 0; M32(LIVE) = 0;
        break;
    case 1:
        rt_logf("cam: live\n");
        cam_start();
        set_rect(drv);
        x11_cam_mode(1);
        DRV(17) = 1; M32(LIVE) = 1;
        break;
    case 5: DRV(19) = 0; break;
    case 6: DRV(19) = 1; break;
    default: RET(-103);
    }
    RET(0);
}

/* (drv, set, int a[7]): brightness/contrast/... kept but not applied */
void ovr_cam_adj(CPU *c) {
    u32 a = ARG(2), store = 0x80BD884;
    int i;
    for (i = 0; i < 7; i++) {
        if (!ARG(1)) M32(a + 4 * i) = M32(store + 4 * i);
        else if ((s32)M32(a + 4 * i) >= 0) M32(store + 4 * i) = M32(a + 4 * i);
    }
    RET(0);
}

/* (drv, set, int a[6]): x, y, w, h, zoom num/den */
void ovr_cam_win(CPU *c) {
    u32 drv = ARG(0), a = ARG(2);
    s32 x, y, w, h, n, d, sw, sh;
    x11_screen_size(&sw, &sh);
    if (!ARG(1)) {
        w = (s32)DRV(37) - (s32)DRV(35);
        M32(a) = DRV(35); M32(a + 4) = DRV(36); M32(a + 8) = (u32)w; M32(a + 12) = DRV(38) - DRV(36);
        M32(a + 16) = 100; M32(a + 20) = (u32)(w ? 64000 / w : 0);
        RET(0);
    }
    x = (s32)M32(a); y = (s32)M32(a + 4); w = (s32)M32(a + 8); h = (s32)M32(a + 12); n = (s32)M32(a + 16); d = (s32)M32(a + 20);
    if (x < 0) x = (s32)DRV(35);
    if (y < 0) y = (s32)DRV(36);
    if (w < 0) w = (s32)DRV(37) - (s32)DRV(35);
    if (h < 0) h = (s32)DRV(38) - (s32)DRV(36);
    if (!n && !d) {
        x = sw / 2 - 320 < 0 ? 0 : sw / 2 - 320; y = sh / 2 - 240 < 0 ? 0 : sh / 2 - 240;
        w = sw > 640 ? 640 : sw; h = sh > 480 ? 480 : sh;
    } else if (n > 0 && d > 0) {
        w = (640 * n / d) & ~15; h = 480 * w / 640;
    }
    x &= ~15; y &= ~1; w &= ~15; h &= ~1;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > sw || y + h > sh) RET(-1);
    DRV(35) = (u32)x; DRV(36) = (u32)y; DRV(37) = (u32)(x + w); DRV(38) = (u32)(y + h);
    set_rect(drv);
    RET(0);
}

/* (drv, set, int a[3]): input / standard / format; one composite input */
void ovr_cam_src(CPU *c) {
    u32 a = ARG(2);
    if (!ARG(1)) { M32(a) = 1; M32(a + 4) = 0; M32(a + 8) = 0; }
    RET(0);
}
