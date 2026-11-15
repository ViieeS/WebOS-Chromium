// cnrun: the app's bin/run (appinfo "main"). Starts Chrome (bin/run.sh) as a child and stays its parent as the
// hardware video service.
//
// Why a C program: luna-service2 grants the app's bus names (com.webos.media.client.* ...) only to the executable
// named in the app's role, which is bin/run. A shell script there shows up as /bin/sh, and Chrome's exe is
// browser/chrome, so neither may talk to LG's media server. This process can, so it drives LG's NDL DirectMedia
// (hardware decode of H.264/HEVC/VP9/AV1, picture on the TV's video plane under Chrome's window).
//
// Protocol on the Unix stream socket $CN_MEDIA_SOCK (one message = cn_hdr + payload, host byte order):
//   OPEN  (renderer)  cn_open: codec, size, audio rate        -> unload old stream, load new one
//   VIDEO (renderer)  int64 pts_ms + one access unit          -> NDL_DirectVideoPlay
//   AUDIO (renderer)  int64 pts_ms + S16LE stereo PCM         -> NDL_DirectAudioPlay
//   CLOSE (renderer)  token                                    -> unload if it is the current stream
//   GEOM  (viz)       token + video rect in window pixels      -> place the video plane (own exported window)
//   WINID (browser)   window id Chrome exported from its own window surface -> used for SetWindowId; LSM shows
//                     a video window only when it belongs to the app window (ours alone stayed black)
//   VSIZE (to browser) uint32 w,h of the stream on every OPEN, so the browser sets the plane's source region
//   NOHW  (to renderer) zero length: an NDL call failed, hardware video is off for the rest of this session (webOS 4 only)
// pts_ms is the renderer's presentation time in ms since its OPEN. The renderer paces sending (small lead).
// webOS 4 has NDL API 1 (no Load, no SetWindowId, no pts): ndl_open_old() and stream_open_old() below.
//
// NDL order that works on webOS 10: SetWindowId(exported window) BEFORE Init(appId), then
// Load with video + audio info. Types from webosbrew/webos-userland (Unlicense); LG's library is dlopen()ed.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <gnu/libc-version.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdbool.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "webos-foreign-client-protocol.h"

enum { CN_OPEN = 1, CN_VIDEO = 2, CN_AUDIO = 3, CN_CLOSE = 4, CN_GEOM = 5, CN_SRC = 6, CN_WINID = 7, CN_VSIZE = 8, CN_NOHW = 9 };
struct cn_hdr { uint32_t type, len; };
struct cn_open { uint64_t token[2]; int32_t codec, width, height, audio_rate, audio_channels; };
struct cn_geom { uint64_t token[2]; int32_t x, y, w, h; };

typedef struct {
  struct { int width, height, type, unknown1; } video;
  union {
    int type;
    struct { int type, unknown1; const char *format, *layout, *channelMode; int sampleRate; } pcm;
    char padding[32];
  } audio;
} NDL_DATA_INFO;  // NDL_DIRECTMEDIA_DATA_INFO_T, API v2 (webOS 5+)

// NDL DirectMedia API 1 (webOS 4): types from webosbrew/webos-userland (Unlicense), include/libndl-media/
// NDL_directmedia_types.h and NDL_directmedia_v1.h. Field order and enum
// values as in those headers. Enum fields are plain ints (the same size on this ABI).
typedef struct { int width, height, source; } NDL_DIRECTVIDEO_DATA_INFO_T;  // source: 1 = H.264 (the only one)
typedef struct {
  unsigned int number_of_channel, bit_per_sample;
  int no_delay_mode, no_delay_upper_time, no_delay_lower_time, channel, source, frequency;
} NDL_DIRECTAUDIO_DATA_INFO_T;  // no_delay_mode 1 = custom, channel 0 = main, source 1 = PCM, frequency: api1_rate()

static FILE *lg;
static void L(const char *f, ...) {
  if (!lg) return;
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  fprintf(lg, "%5ld.%03ld ", (long)t.tv_sec % 100000, t.tv_nsec / 1000000);
  va_list a; va_start(a, f); vfprintf(lg, f, a); va_end(a); fputc('\n', lg); fflush(lg);
}

// ---- NDL (dlopen) ----
static int (*ndl_init)(const char *), (*ndl_setwin)(const char *), (*ndl_unload)(void), (*ndl_quit)(void);
static int (*ndl_load)(NDL_DATA_INFO *, void (*)(int, long long, const char *));
static int (*ndl_vplay)(void *, unsigned, long long), (*ndl_aplay)(void *, unsigned, long long);
static const char *(*ndl_err)(void);
static int (*ndl_vbuflen)(int *), (*ndl_appstate)(int), (*ndl_setarea)(int, int, int, int);
// NDL API 1 entry points, filled by ndl_open_old(). Signatures: NDL_directmedia_v1.h and NDL_directmedia_common.h
// (webosbrew/webos-userland). NDL_DirectMediaInit takes (app_id, release callback) in API 1.
static int (*ndl1_init)(const char *, void *), (*ndl1_vopen)(NDL_DIRECTVIDEO_DATA_INFO_T *), (*ndl1_vclose)(void);
static int (*ndl1_vcb)(void (*)(unsigned long long)), (*ndl1_vplay)(const void *, unsigned int, unsigned long long);
static int (*ndl1_aopen)(NDL_DIRECTAUDIO_DATA_INFO_T *), (*ndl1_aclose)(void), (*ndl1_aplay)(void *, unsigned int);
static int ndl_mode;              // 0 none, 1 NDL API 1 ("old", webOS 4), 2 NDL API 2 ("new", webOS 5+)
static int old_vopen, old_aopen;  // which API 1 streams are open
static char hw_state[700];        // $tdir/hw.state: "bad" after an API 1 call failed
static int old_hw_off;            // 1: API 1 failed in this session; every later OPEN gets NOHW
static long vframes;
static int ndl_ok, inited, loaded, cur_rate;
static uint64_t cur_token[2];
static int cur_owner = -1;   // client fd that opened the current stream

// API 1 (webOS 4): every symbol is needed. A missing one turns hardware video off. Each name is looked up with dlsym
// directly, not with S(), so a miss only stops this path: if a TV lacks one, this returns 0 and the old path stays off.
static int ndl_open_old(void *h) {
#define O(v, n) do { *(void **)&v = dlsym(h, n); if (!v) { L("old NDL API: missing %s, hardware video off", n); return 0; } } while (0)
  O(ndl1_init, "NDL_DirectMediaInit"); O(ndl_quit, "NDL_DirectMediaQuit"); O(ndl_err, "NDL_DirectMediaGetError");
  O(ndl1_vopen, "NDL_DirectVideoOpen"); O(ndl1_vclose, "NDL_DirectVideoClose"); O(ndl1_vcb, "NDL_DirectVideoSetCallback");
  O(ndl1_vplay, "NDL_DirectVideoPlayWithCallback"); O(ndl_setarea, "NDL_DirectVideoSetArea");
  O(ndl1_aopen, "NDL_DirectAudioOpen"); O(ndl1_aclose, "NDL_DirectAudioClose"); O(ndl1_aplay, "NDL_DirectAudioPlay");
#undef O
  *(void **)&ndl_appstate = dlsym(h, "NDL_DirectMediaSetAppState");
  return 1;
}
// Returns the NDL API the library has (ndl_mode): 2 for API 2 (NDL_DirectMediaLoad present), 1 for API 1, 0 for none.
static int ndl_open_lib(void) {
  void *h = dlopen("libNDL_directmedia.so.1", RTLD_NOW | RTLD_GLOBAL);
  if (!h) { L("dlopen: %s", dlerror()); return 0; }
  if (!dlsym(h, "NDL_DirectMediaLoad")) {
    L("NDL_DirectMediaLoad absent: trying NDL API 1");
    return ndl_open_old(h) ? 1 : 0;
  }
#define S(v, n) *(void **)&v = dlsym(h, n); if (!v) { L("missing %s", n); return 0; }
  S(ndl_init, "NDL_DirectMediaInit") S(ndl_setwin, "NDL_DirectMediaSetWindowId") S(ndl_load, "NDL_DirectMediaLoad")
  S(ndl_unload, "NDL_DirectMediaUnload") S(ndl_quit, "NDL_DirectMediaQuit") S(ndl_vplay, "NDL_DirectVideoPlay")
  S(ndl_aplay, "NDL_DirectAudioPlay") S(ndl_err, "NDL_DirectMediaGetError")
#undef S
  *(void **)&ndl_vbuflen = dlsym(h, "NDL_DirectVideoGetRenderBufferLength");
  *(void **)&ndl_appstate = dlsym(h, "NDL_DirectMediaSetAppState");
  *(void **)&ndl_setarea = dlsym(h, "NDL_DirectVideoSetArea");
  return 2;
}
static void load_cb(int type, long long num, const char *str) { L("ndl callback type=%d num=%lld str=%s", type, num, str ? str : "-"); }

// ---- Wayland: one exported VIDEO window for the app's lifetime ----
static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_webos_foreign *foreign;
static struct wl_webos_exported *exported;
static char window_id[128];
static int geom_x, geom_y, geom_w, geom_h, src_w = 1920, src_h = 1080;

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
  if (!strcmp(iface, "wl_compositor")) comp = wl_registry_bind(r, name, &wl_compositor_interface, 1);
  else if (!strcmp(iface, "wl_webos_foreign")) foreign = wl_registry_bind(r, name, &wl_webos_foreign_interface, 1);
}
static void reg_remove_noop(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_l = { reg_global, reg_remove_noop };
static void exp_id(void *d, struct wl_webos_exported *e, const char *id, uint32_t type) {
  snprintf(window_id, sizeof window_id, "%s", id); L("exported window id %s", id);
}
static const struct wl_webos_exported_listener exp_l = { exp_id };

// ---- does the compositor accept wl_webos_exported.set_property? ----
// LG's protocol has wl_webos_foreign at version 1, but the wl_webos_exported it creates has set_property and
// set_crop_region "since 2". A new object gets its parent's version, so strictly our exported window is version 1
// and set_property is invalid. webOS 10's LSM accepts it anyway (and needs mute=off to show the picture); a
// compositor that checks versions, as libwayland-server does ("invalid method 3 (since 1 < 2)"), ends
// the client's connection: Chrome closed right after its splash. So test it once on a throwaway connection before
// Chrome starts. Result in $CN_EXPORTED_SET_PROPERTY (1/0): Chrome (patch 0025) and place_video() follow it.
static int exported_set_property = 1;
struct probe { struct wl_compositor *c; struct wl_webos_foreign *f; int id; };
static void probe_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
  struct probe *p = d;
  if (!strcmp(iface, "wl_compositor")) p->c = wl_registry_bind(r, name, &wl_compositor_interface, 1);
  else if (!strcmp(iface, "wl_webos_foreign")) p->f = wl_registry_bind(r, name, &wl_webos_foreign_interface, 1);
}
static const struct wl_registry_listener probe_reg_l = { probe_global, reg_remove_noop };
static void probe_id(void *d, struct wl_webos_exported *e, const char *id, uint32_t type) { ((struct probe *)d)->id = 1; }
static const struct wl_webos_exported_listener probe_exp_l = { probe_id };
static int probe_set_property(void) {
  struct wl_display *d = wl_display_connect(NULL);
  if (!d) { L("probe: wl_display_connect failed"); return 0; }
  struct probe p; memset(&p, 0, sizeof p);
  wl_registry_add_listener(wl_display_get_registry(d), &probe_reg_l, &p);
  int ok = 0;
  if (wl_display_roundtrip(d) >= 0 && p.c && p.f) {
    struct wl_surface *s = wl_compositor_create_surface(p.c);
    struct wl_webos_exported *e = wl_webos_foreign_export_element(p.f, s, WL_WEBOS_FOREIGN_WEBOS_EXPORTED_TYPE_VIDEO_OBJECT);
    wl_webos_exported_add_listener(e, &probe_exp_l, &p);
    for (int i = 0; i < 50 && !p.id; i++) if (wl_display_roundtrip(d) < 0) break;
    wl_webos_exported_set_property(e, "mute", "on");  // a real property; this throwaway window shows nothing
    ok = wl_display_roundtrip(d) >= 0;
    L("probe: window id %s, set_property %s%s", p.id ? "assigned" : "none", ok ? "accepted" : "refused, error ",
      ok ? "" : strerror(wl_display_get_error(d)));
    if (ok) { wl_webos_exported_destroy(e); wl_surface_destroy(s); wl_display_roundtrip(d); }
  } else {
    L("probe: no wl_compositor/wl_webos_foreign");
  }
  wl_display_disconnect(d);
  return ok;
}

static void place_video(void) {
  if (!exported) return;
  int x = geom_x, y = geom_y, w = geom_w, h = geom_h;
  if (w <= 0 || h <= 0) { x = 0; y = 0; w = 1920; h = 1080; }  // no geometry yet: full panel
  struct wl_region *s = wl_compositor_create_region(comp); wl_region_add(s, 0, 0, src_w, src_h);
  struct wl_region *d = wl_compositor_create_region(comp); wl_region_add(d, x, y, w, h);
  wl_webos_exported_set_exported_window(exported, s, d);
  // neva's WebOSForeignVideoWindow::SetVisibility(true): the video plane stays hidden until "mute" is off
  if (exported_set_property) wl_webos_exported_set_property(exported, "mute", "off");
  wl_region_destroy(s); wl_region_destroy(d);
  wl_display_flush(dpy);
  L("video plane src %dx%d -> %d,%d %dx%d", src_w, src_h, x, y, w, h);
}

static int wayland_up(void) {
  if (window_id[0]) return 1;
  if (!dpy && !(dpy = wl_display_connect(NULL))) { L("wl_display_connect failed"); return 0; }
  struct wl_registry *reg = wl_display_get_registry(dpy);
  wl_registry_add_listener(reg, &reg_l, NULL);
  wl_display_roundtrip(dpy);
  if (!comp || !foreign) { L("no wl_compositor/wl_webos_foreign"); return 0; }
  struct wl_surface *vs = wl_compositor_create_surface(comp);
  exported = wl_webos_foreign_export_element(foreign, vs, WL_WEBOS_FOREIGN_WEBOS_EXPORTED_TYPE_VIDEO_OBJECT);
  wl_webos_exported_add_listener(exported, &exp_l, NULL);
  for (int i = 0; i < 50 && !window_id[0]; i++) wl_display_roundtrip(dpy);
  if (!window_id[0]) { L("no exported window id"); return 0; }
  return 1;
}

static const char *app_id;
static char chrome_window_id[128];
static int winid_fd = -1;
static void stream_close(const char *why) {
  if (ndl_mode == 1) {
    if (old_aopen) { int r = ndl1_aclose(); L("audio close (%s) = %d", why, r); old_aopen = 0; }
    if (old_vopen) { int r = ndl1_vclose(); L("video close (%s) = %d", why, r); old_vopen = 0; }
    loaded = 0;
  } else if (loaded) { int r = ndl_unload(); L("unload (%s) = %d", why, r); loaded = 0; }
  if (inited) { int r = ndl_quit(); L("quit = %d", r); inited = 0; }
  cur_owner = -1; memset(cur_token, 0, sizeof cur_token);
}

// API 1 (webOS 4). A failed call ends hardware video for the rest of the session: CN_NOHW (zero length) goes to the renderer,
// the stream is closed, and mark_bad sets old_hw_off and writes "bad" to $tdir/hw.state. Later OPENs get NOHW too.
static void old_fail(int fd, int mark_bad) {
  struct cn_hdr h = { CN_NOHW, 0 };
  if (write(fd, &h, sizeof h) != sizeof h) L("nohw to renderer failed");
  if (mark_bad) {
    old_hw_off = 1;
    FILE *f = fopen(hw_state, "w"); if (f) { fputs("bad\n", f); fclose(f); }
  }
  L("old NDL API: hardware video off for this session (%s)", hw_state);
  stream_close("old NDL API");
}
static int old_play(int fd, int video, const void *p, unsigned int n, long long pts) {
  if (video) {
    vframes++;
    if (vframes == 1 || vframes % 1500 == 0) L("old NDL API: video frame %ld pts %lld ms", vframes, pts);
  }
  int r = video ? ndl1_vplay(p, n, (unsigned long long)pts) : ndl1_aplay((void *)p, n);
  if (!r) return 1;
  L("old NDL API: %s play = %d (%s)", video ? "video" : "audio", r, ndl_err());
  old_fail(fd, 1);
  return 0;
}
// Audio sample rate codes of API 1 (NDL_DIRECTAUDIO_SAMPLING_FREQ): 8k=2, 12k=4, 16k=5, 22.05k=6, 24k=7, 32k=8,
// 44.1k=9, 48k=10. Any other rate has no code and the old path refuses it. (API 2's table is different.)
static int api1_rate(int hz) {
  switch (hz) {
    case 8000: return 2;
    case 12000: return 4;
    case 16000: return 5;
    case 22050: return 6;
    case 24000: return 7;
    case 32000: return 8;
    case 44100: return 9;
    case 48000: return 10;
    default: return 0;
  }
}
static void old_video_cb(unsigned long long userdata) {}  // NDL calls it with the pts of each played frame; not used
// Opens video and audio with API 1 and places the picture with NDL_DirectVideoSetArea. There is no window id, no Load,
// and no pts: the renderer sends each frame at its own pts (patch 0023 Pump), so frames go in as they arrive.
static void stream_open_old(int fd, const struct cn_open *o) {
#define OLD_CHECK(what, call) do { int rc_ = (call); if (rc_) { L("old NDL API: %s = %d (%s)", what, rc_, ndl_err()); old_fail(fd, 1); return; } } while (0)
  if (o->codec != 1) {  // NDL_VIDEO_TYPE_H264 is the only codec API 1 takes; the renderer checks this too (patch 0039)
    L("old NDL API: codec %d refused, H.264 only", o->codec);
    old_fail(fd, 0);
    return;
  }
  cur_rate = o->audio_rate ? o->audio_rate : 48000;
  int af = api1_rate(cur_rate);
  if (!af) { L("old NDL API: audio rate %d has no API 1 code", cur_rate); old_fail(fd, 0); return; }
  OLD_CHECK("init", ndl1_init(app_id, NULL));
  inited = 1;
  NDL_DIRECTVIDEO_DATA_INFO_T vi = { o->width, o->height, 1 };
  OLD_CHECK("video open", ndl1_vopen(&vi));
  old_vopen = 1;
  OLD_CHECK("video callback", ndl1_vcb(old_video_cb));
  int x = 0, y = 0, w = 1920, h = 1080;
  if (geom_w > 0 && geom_h > 0) { x = geom_x; y = geom_y; w = geom_w; h = geom_h; }
  OLD_CHECK("set area", ndl_setarea(x, y, w, h));
  NDL_DIRECTAUDIO_DATA_INFO_T ai;
  memset(&ai, 0, sizeof ai);
  ai.number_of_channel = 2; ai.bit_per_sample = 16; ai.no_delay_mode = 1; ai.no_delay_upper_time = 48;
  ai.no_delay_lower_time = 16; ai.channel = 0; ai.source = 1; ai.frequency = af;
  OLD_CHECK("audio open", ndl1_aopen(&ai));
  old_aopen = 1;
#undef OLD_CHECK
  L("old NDL API: open H.264 %dx%d audio=%d, area %d,%d %dx%d", o->width, o->height, o->audio_rate, x, y, w, h);
  vframes = 0;
  loaded = 1; cur_owner = fd; memcpy(cur_token, o->token, sizeof cur_token);
  if (!o->audio_rate) cur_rate = 0;  // video only: we feed silence
  src_w = o->width; src_h = o->height;
}
static void stream_open(int fd, const struct cn_open *o) {
  stream_close("reopen");
  if (!ndl_ok) return;
  if (ndl_mode == 1 && old_hw_off) {  // hardware video is off: the renderer decodes in software
    struct cn_hdr h = { CN_NOHW, 0 };
    if (write(fd, &h, sizeof h) != sizeof h) L("nohw to renderer failed");
    return;
  }
  if (ndl_mode == 1) { stream_open_old(fd, o); return; }
  const char *wid = chrome_window_id;
  if (!wid[0]) { if (!wayland_up()) return; wid = window_id; }
  int r = ndl_setwin(wid);
  int r2 = ndl_init(app_id);
  if (r2) { L("init = %d (%s)", r2, ndl_err()); return; }
  inited = 1;
  NDL_DATA_INFO info; memset(&info, 0, sizeof info);
  info.video.type = o->codec; info.video.width = o->width; info.video.height = o->height;
  static const int rates[][2] = { {48000, 1}, {44100, 2}, {32000, 3}, {24000, 4}, {16000, 5}, {12000, 6}, {8000, 7}, {22050, 8} };
  cur_rate = o->audio_rate ? o->audio_rate : 48000;
  info.audio.pcm.type = 1; info.audio.pcm.format = "S16LE"; info.audio.pcm.layout = "interleaved";
  info.audio.pcm.channelMode = "stereo"; info.audio.pcm.sampleRate = 1;
  for (unsigned i = 0; i < sizeof rates / sizeof rates[0]; i++) if (rates[i][0] == cur_rate) info.audio.pcm.sampleRate = rates[i][1];
  int r3 = ndl_load(&info, load_cb);
  L("open codec=%d %dx%d audio=%d: setwindow=%d init=%d load=%d (%s)", o->codec, o->width, o->height,
    o->audio_rate, r, r2, r3, r3 ? ndl_err() : "");
  if (r3) { stream_close("load failed"); return; }
  vframes = 0;
  loaded = 1; cur_owner = fd; memcpy(cur_token, o->token, sizeof cur_token);
  if (!o->audio_rate) cur_rate = 0;  // video only: we feed silence
  src_w = o->width; src_h = o->height;
  if (chrome_window_id[0] && winid_fd >= 0) {
    uint32_t m[4] = { CN_VSIZE, 8, (uint32_t)o->width, (uint32_t)o->height };
    if (write(winid_fd, m, sizeof m) != sizeof m) L("vsize to browser failed");
  } else {
    place_video();
  }
}

static int vfails, afails;
static void on_msg(int fd, uint32_t type, unsigned char *p, uint32_t len) {
  static int16_t silence[1600 * 2];
  switch (type) {
  case CN_OPEN:
    if (len >= sizeof(struct cn_open)) stream_open(fd, (const struct cn_open *)p);
    break;
  case CN_VIDEO: case CN_AUDIO: {
    if (!loaded || fd != cur_owner || len < 8) break;
    long long pts; memcpy(&pts, p, 8);
    if (ndl_mode == 1) {  // API 1: no pts, and a failed call ends the session's hardware video (old_fail)
      if (old_play(fd, type == CN_VIDEO, p + 8, len - 8, pts) && type == CN_VIDEO && !cur_rate)
        old_play(fd, 0, silence, sizeof silence, pts);
      break;
    }
    int r = type == CN_VIDEO ? ndl_vplay(p + 8, len - 8, pts) : ndl_aplay(p + 8, len - 8, pts);
    if (r && (type == CN_VIDEO ? vfails++ : afails++) < 10) L("%s play = %d (%s)", type == CN_VIDEO ? "video" : "audio", r, ndl_err());
    if (type == CN_VIDEO && !cur_rate) ndl_aplay(silence, sizeof silence, pts);
    if (type == CN_VIDEO && (++vframes == 1 || vframes % 1500 == 0)) {
      int bl = -1; if (ndl_vbuflen) ndl_vbuflen(&bl);
      L("video frame %ld pts %lld size %u, render buffer %d", vframes, pts, len - 8, bl);
    }
    break;
  }
  case CN_WINID:
    if (len > 0 && len < sizeof chrome_window_id) {
      memcpy(chrome_window_id, p, len); chrome_window_id[len] = 0; winid_fd = fd;
      L("browser exported video window %s", chrome_window_id);
    }
    break;
  case CN_CLOSE:
    if (len >= 16 && !memcmp(p, cur_token, 16)) stream_close("close");
    break;
  case CN_GEOM: {
    if (len < sizeof(struct cn_geom)) break;
    const struct cn_geom *g = (const struct cn_geom *)p;
    if (g->x != geom_x || g->y != geom_y || g->w != geom_w || g->h != geom_h) {
      geom_x = g->x; geom_y = g->y; geom_w = g->w; geom_h = g->h;
      if (loaded && !chrome_window_id[0]) place_video();
      if (loaded && ndl_mode == 1 && ndl_setarea) {
        int r = ndl_setarea(geom_x, geom_y, geom_w, geom_h);
        L("set area %d,%d %dx%d = %d", geom_x, geom_y, geom_w, geom_h, r);
      }
    }
    break;
  }
  }
}

// ---- clients ----
#define MAXC 16
static struct client { int fd; unsigned char *buf; size_t n, cap; } cl[MAXC];
static void client_drop(struct client *c) {
  if (c->fd == cur_owner) stream_close("client gone");
  if (c->fd == winid_fd) { winid_fd = -1; chrome_window_id[0] = 0; }
  close(c->fd); free(c->buf); memset(c, 0, sizeof *c); c->fd = -1;
}
static void client_read(struct client *c) {
  for (;;) {
    if (c->cap - c->n < 65536) {
      size_t nc = c->cap ? c->cap * 2 : 262144;
      unsigned char *nb = realloc(c->buf, nc); if (!nb) { client_drop(c); return; }
      c->buf = nb; c->cap = nc;
    }
    ssize_t r = read(c->fd, c->buf + c->n, c->cap - c->n);
    if (r == 0) { client_drop(c); return; }
    if (r < 0) { if (errno == EINTR) continue; if (errno != EAGAIN) client_drop(c); break; }
    c->n += r;
    size_t off = 0;
    while (c->n - off >= sizeof(struct cn_hdr)) {
      struct cn_hdr h; memcpy(&h, c->buf + off, sizeof h);
      if (h.len > 64 << 20) { L("bad message len %u", h.len); client_drop(c); return; }
      if (c->n - off - sizeof h < h.len) break;
      on_msg(c->fd, h.type, c->buf + off + sizeof h, h.len);
      off += sizeof h + h.len;
    }
    memmove(c->buf, c->buf + off, c->n - off); c->n -= off;
  }
}

// ---- TV too old? ----
// The browser needs glibc 2.24 (the compat sysroot, patch 0031), which webOS 4.4 and newer ship. Older TVs have less
// glibc, and their loader then refuses browser/chrome without a word. This launcher is built against glibc 2.17
// (build.sh, zig) so it starts on every webOS, and tells the user instead of closing silently.
#define CN_GLIBC_MAJOR 2
#define CN_GLIBC_MINOR 24

static void webos_version(char *out, size_t n) {
  out[0] = 0;
  char buf[4096]; FILE *f = fopen("/var/run/nyx/os_info.json", "r");
  if (f) {
    size_t r = fread(buf, 1, sizeof buf - 1, f); buf[r] = 0; fclose(f);
    char *k = strstr(buf, "\"webos_release\"");
    if (k && (k = strchr(k + 15, '"'))) {
      k++; size_t i = 0; while (k[i] && k[i] != '"' && i + 1 < n) { out[i] = k[i]; i++; } out[i] = 0;
    }
  }
}

static void toast(const char *msg) {
  char json[1024];
  snprintf(json, sizeof json, "{\"sourceId\":\"%s\",\"message\":\"%s\"}", app_id, msg);
  static const char *tools[] = { "/usr/bin/luna-send-pub", "/usr/bin/luna-send", "/bin/luna-send-pub", "/bin/luna-send" };
  for (unsigned i = 0; i < sizeof tools / sizeof *tools; i++) {
    if (access(tools[i], X_OK)) continue;
    pid_t p = fork();
    if (p == 0) {
      int nul = open("/dev/null", O_RDWR); if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
      execl(tools[i], tools[i], "-n", "1", "luna://com.webos.notification/createToast", json, (char *)NULL);
      _exit(127);
    }
    int st = 0;
    if (p > 0 && waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 0) { L("toast sent with %s", tools[i]); return; }
    L("toast with %s failed (status %d)", tools[i], st);
  }
}

// 1 if this TV can't run the browser (and the user was told), 0 if it can.
// <app>/bin/requires (written by mkipk.py --requires) overrides the defaults for other builds, e.g. the compat
// build for webOS 4.4 and newer: lines "glibc=2.24" and "webos=webOS 4.4".
static int tv_too_old(const char *tdir, const char *bin) {
  int need_maj = CN_GLIBC_MAJOR, need_min = CN_GLIBC_MINOR;
  char need_webos[64] = "webOS 4.4";
  char rp[700]; snprintf(rp, sizeof rp, "%s/requires", bin);
  FILE *rf = fopen(rp, "r");
  if (rf) {
    char line[128];
    while (fgets(line, sizeof line, rf)) {
      line[strcspn(line, "\r\n")] = 0;
      if (!strncmp(line, "glibc=", 6)) sscanf(line + 6, "%d.%d", &need_maj, &need_min);
      else if (!strncmp(line, "webos=", 6)) snprintf(need_webos, sizeof need_webos, "%s", line + 6);
    }
    fclose(rf);
  }
  const char *g = gnu_get_libc_version();
  int maj = 0, min = 0; sscanf(g, "%d.%d", &maj, &min);
  char wv[64]; webos_version(wv, sizeof wv);
  L("compat: glibc %s (needs %d.%d), webOS %s", g, need_maj, need_min, wv[0] ? wv : "?");
  if ((maj > need_maj || (maj == need_maj && min >= need_min))) return 0;
  char msg[300];
  snprintf(msg, sizeof msg, "Chromium needs %s or newer. This TV has webOS %s, so it can't start yet.", need_webos,
           wv[0] ? wv : "an older version");
  toast(msg);
  return 1;
}

// ---- can Chrome's GPU path start on this TV? ----
// Chrome opens its EGL display with eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, ...), an EGL 1.5 function, and
// calls it without checking that the driver has it. Some Mali drivers on older TVs are EGL 1.4 only: the
// crash dump showed pc=0 with r0=0x31d8 (EGL_PLATFORM_WAYLAND_KHR), and --disable-gpu made Chrome start. Look the
// function up the way Chrome does (dlsym in libEGL.so.1, then eglGetProcAddress) in a child process, so the GPU
// driver is never loaded into this long-lived launcher. Exit code: 0 found, 1 missing, 2 no libEGL.so.1.
static int egl_platform_display_check(void) {
  pid_t p = fork();
  if (p == 0) {
    void *h = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) _exit(2);
    void *f = dlsym(h, "eglGetPlatformDisplay");
    if (!f) {
      void *(*gpa)(const char *);
      *(void **)&gpa = dlsym(h, "eglGetProcAddress");
      if (gpa) f = gpa("eglGetPlatformDisplay");
    }
    _exit(f ? 0 : 1);
  }
  int st = 0;
  if (p < 0 || waitpid(p, &st, 0) != p) return -1;
  return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

// ---- GPU probe ----
// Asks the TV's own EGL/GLES driver what it offers, instead of guessing one build at a time: entry points, client
// extensions, every way of opening a display (eglGetPlatformDisplay, ...EXT, eglGetDisplay on our wl_display and on
// EGL_DEFAULT_DISPLAY) with its eglInitialize result and error, EGL version/vendor/extensions, configs, the GL
// renderer of a test context, a 1-pixel draw into a pbuffer, and creating (not showing) a Wayland window surface.
// Every line goes to media.log as "gpuprobe: ...". It runs in a child with a time limit, logging each step before
// doing it, so a driver that hangs (the Firefox port saw this TV's Mali hang at its first frame) shows where.
// Nothing is committed to the compositor, so no window appears.
// Runs: once per boot on TVs where the check above turns the GPU off, and on every start with media.opts
// "gpuprobe"; "nogpuprobe" turns it off. Cost: ~0.1-0.5 s at that start, at most the time limit.
#define GP(fmt, ...) L("gpuprobe: " fmt, ##__VA_ARGS__)
enum {
  E_SUCCESS = 0x3000, E_BUFFER_SIZE = 0x3020, E_ALPHA = 0x3021, E_BLUE = 0x3022, E_GREEN = 0x3023, E_RED = 0x3024,
  E_DEPTH = 0x3025, E_STENCIL = 0x3026, E_CONFIG_ID = 0x3028, E_SAMPLES = 0x3031, E_SURFACE_TYPE = 0x3033,
  E_NONE = 0x3038, E_RENDERABLE = 0x3040, E_VENDOR = 0x3053, E_VERSION = 0x3054, E_EXTENSIONS = 0x3055,
  E_HEIGHT = 0x3056, E_WIDTH = 0x3057, E_CLIENT_APIS = 0x308D, E_CONTEXT_CLIENT_VERSION = 0x3098,
  E_OPENGL_ES_API = 0x30A0, E_PLATFORM_WAYLAND = 0x31D8, E_PBUFFER_BIT = 1, E_WINDOW_BIT = 4, E_ES2_BIT = 4,
  G_VENDOR = 0x1F00, G_RENDERER = 0x1F01, G_VERSION = 0x1F02, G_EXTENSIONS = 0x1F03, G_SL_VERSION = 0x8B8C,
  G_COLOR_BUFFER_BIT = 0x4000, G_RGBA = 0x1908, G_UNSIGNED_BYTE = 0x1401,
};
static void gp_file(const char *path) {
  char b[256]; FILE *f = fopen(path, "r");
  if (!f) return;
  if (fgets(b, sizeof b, f)) { b[strcspn(b, "\r\n")] = 0; GP("%s: %s", path, b); }
  fclose(f);
}
static int gl_checks(void *(*gpa)(const char *), const char *where) {
  void *gh = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
#define GF(v, n) do { *(void **)&v = gh ? dlsym(gh, n) : NULL; if (!v && gpa) *(void **)&v = gpa(n); } while (0)
  const unsigned char *(*gl_string)(unsigned); void (*gl_clear_color)(float, float, float, float), (*gl_clear)(unsigned);
  void (*gl_read)(int, int, int, int, unsigned, unsigned, void *), (*gl_finish)(void);
  GF(gl_string, "glGetString"); GF(gl_clear_color, "glClearColor"); GF(gl_clear, "glClear");
  GF(gl_read, "glReadPixels"); GF(gl_finish, "glFinish");
#undef GF
  if (gl_string) {
    const unsigned char *g;
    g = gl_string(G_VENDOR); GP("GL vendor: %s", g ? (const char *)g : "?");
    g = gl_string(G_RENDERER); GP("GL renderer: %s", g ? (const char *)g : "?");
    g = gl_string(G_VERSION); GP("GL version: %s", g ? (const char *)g : "?");
    g = gl_string(G_SL_VERSION); GP("GLSL version: %s", g ? (const char *)g : "?");
    g = gl_string(G_EXTENSIONS); GP("GL extensions: %s", g ? (const char *)g : "?");
  }
  if (gl_clear_color && gl_clear && gl_read && gl_finish) {
    unsigned char px[4] = {0};
    GP("drawing into the %s", where);
    gl_clear_color(1.0f, 0.0f, 0.0f, 1.0f); gl_clear(G_COLOR_BUFFER_BIT); gl_finish();
    gl_read(0, 0, 1, 1, G_RGBA, G_UNSIGNED_BYTE, px);
    GP("%s draw: pixel %02x%02x%02x%02x (%s)", where, px[0], px[1], px[2], px[3],
       px[0] == 0xff && px[1] == 0 && px[2] == 0 ? "OK, the GPU draws" : "WRONG");
    return px[0] == 0xff && px[1] == 0 && px[2] == 0;
  }
  return 0;
}
static int gpu_probe_child(void) {
  int wl_display_ok = 0, window_ok = 0, draw_ok = 0;
  setenv("EGL_PLATFORM", "wayland", 1);  // as run.sh exports for Chrome
  gp_file("/sys/class/misc/mali0/device/gpuinfo");
  gp_file("/proc/driver/pvr/version");
  void *h = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!h) { GP("dlopen libEGL.so.1: %s", dlerror()); return 1; }
  void *(*gpa)(const char *); *(void **)&gpa = dlsym(h, "eglGetProcAddress");
#define EF(v, n) do { *(void **)&v = dlsym(h, n); if (!v && gpa) *(void **)&v = gpa(n); } while (0)
  void *(*get_display)(void *), *(*get_pdisplay)(unsigned, void *, const void *), *(*get_pdisplay_ext)(unsigned, void *, const int *);
  unsigned (*initialize)(void *, int *, int *), (*get_configs)(void *, void **, int, int *);
  unsigned (*config_attrib)(void *, void *, int, int *), (*choose)(void *, const int *, void **, int, int *);
  unsigned (*bind_api)(unsigned), (*make_current)(void *, void *, void *, void *), (*terminate)(void *);
  unsigned (*destroy_surface)(void *, void *), (*destroy_context)(void *, void *);
  void *(*create_context)(void *, void *, void *, const int *), *(*create_pbuffer)(void *, void *, const int *);
  void *(*create_window)(void *, void *, void *, const int *);
  int (*get_error)(void); const char *(*query)(void *, int);
  EF(get_display, "eglGetDisplay"); EF(initialize, "eglInitialize"); EF(get_error, "eglGetError"); EF(query, "eglQueryString");
  EF(get_configs, "eglGetConfigs"); EF(config_attrib, "eglGetConfigAttrib"); EF(choose, "eglChooseConfig");
  EF(bind_api, "eglBindAPI"); EF(create_context, "eglCreateContext"); EF(create_pbuffer, "eglCreatePbufferSurface");
  EF(create_window, "eglCreateWindowSurface"); EF(make_current, "eglMakeCurrent"); EF(terminate, "eglTerminate");
  EF(destroy_surface, "eglDestroySurface"); EF(destroy_context, "eglDestroyContext");
  EF(get_pdisplay, "eglGetPlatformDisplay");
  *(void **)&get_pdisplay_ext = gpa ? gpa("eglGetPlatformDisplayEXT") : NULL;
  if (!get_pdisplay_ext) *(void **)&get_pdisplay_ext = dlsym(h, "eglGetPlatformDisplayEXT");
  Dl_info di;
  if (get_display && dladdr((void *)get_display, &di)) GP("eglGetDisplay is in %s", di.dli_fname ? di.dli_fname : "?");
  void *p1, *p2; EF(p1, "eglCreatePlatformWindowSurface"); EF(p2, "eglCreatePlatformWindowSurfaceEXT");
  GP("entry points: eglGetPlatformDisplay %s, eglGetPlatformDisplayEXT %s, eglCreatePlatformWindowSurface %s, ...EXT %s",
     get_pdisplay ? "yes" : "no", get_pdisplay_ext ? "yes" : "no", p1 ? "yes" : "no", p2 ? "yes" : "no");
#undef EF
  if (!get_display || !initialize || !get_error || !query) { GP("core EGL functions missing"); return 1; }
  const char *ce = query(NULL, E_EXTENSIONS);
  if (ce) GP("client extensions: %s", ce); else GP("client extensions: none (error 0x%x)", get_error());

  GP("connecting to wayland");
  struct wl_display *wd = wl_display_connect(NULL);
  struct probe pr; memset(&pr, 0, sizeof pr);
  if (wd) { wl_registry_add_listener(wl_display_get_registry(wd), &probe_reg_l, &pr); wl_display_roundtrip(wd); }
  GP("wayland %s, wl_compositor %s", wd ? "connected" : "connect FAILED", pr.c ? "yes" : "no");

  struct { const char *name; int kind; } att[] = {
    { "eglGetPlatformDisplay(WAYLAND, wl_display)", 1 }, { "eglGetPlatformDisplayEXT(WAYLAND, wl_display)", 2 },
    { "eglGetDisplay(wl_display)", 3 }, { "eglGetDisplay(EGL_DEFAULT_DISPLAY)", 4 },
  };
  void *dpy = NULL; const char *dpy_name = NULL;
  for (unsigned i = 0; i < sizeof att / sizeof *att; i++) {
    int k = att[i].kind; void *d = NULL;
    if ((k == 1 && !get_pdisplay) || (k == 2 && !get_pdisplay_ext) || (k <= 3 && !wd)) { GP("%s: skipped", att[i].name); continue; }
    GP("%s: trying", att[i].name);
    get_error();
    d = k == 1 ? get_pdisplay(E_PLATFORM_WAYLAND, wd, NULL) : k == 2 ? get_pdisplay_ext(E_PLATFORM_WAYLAND, wd, NULL)
      : get_display(k == 3 ? (void *)wd : NULL);
    int e1 = get_error();
    if (!d) { GP("%s: EGL_NO_DISPLAY (error 0x%x)", att[i].name, e1); continue; }
    int ma = 0, mi = 0; unsigned ok = initialize(d, &ma, &mi); int e2 = get_error();
    GP("%s: display %p, eglInitialize %s, EGL %d.%d (error 0x%x)", att[i].name, d, ok ? "OK" : "FAILED", ma, mi, e2);
    if (ok && !dpy) { dpy = d; dpy_name = att[i].name; wl_display_ok = k <= 3; }
  }
  if (!dpy) { GP("no display could be initialized"); if (wd) wl_display_disconnect(wd); return 1; }
  GP("using %s", dpy_name);
  const char *s;
  s = query(dpy, E_VENDOR); GP("EGL vendor: %s", s ? s : "?");
  s = query(dpy, E_VERSION); GP("EGL version: %s", s ? s : "?");
  s = query(dpy, E_CLIENT_APIS); GP("EGL client APIs: %s", s ? s : "?");
  s = query(dpy, E_EXTENSIONS); GP("EGL extensions: %s", s ? s : "?");

  int n = 0;
  if (get_configs && config_attrib && get_configs(dpy, NULL, 0, &n) && n > 0) {
    void *cf[64]; int m = n > 64 ? 64 : n;
    get_configs(dpy, cf, m, &m);
    GP("configs: %d (first %d listed: id rgba depth stencil samples surface renderable)", n, m < 16 ? m : 16);
    for (int i = 0; i < m && i < 16; i++) {
      int v[9] = {0}; static const int a[9] = { E_CONFIG_ID, E_RED, E_GREEN, E_BLUE, E_ALPHA, E_DEPTH, E_STENCIL, E_SAMPLES, E_SURFACE_TYPE };
      for (int j = 0; j < 9; j++) config_attrib(dpy, cf[i], a[j], &v[j]);
      int rt = 0; config_attrib(dpy, cf[i], E_RENDERABLE, &rt);
      GP("  config %d: %d%d%d%d d%d s%d ms%d surf 0x%x rend 0x%x", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], rt);
    }
  } else {
    GP("eglGetConfigs: none (error 0x%x)", get_error());
  }

  void *cfg = NULL; int nc = 0;
  static const int want[][13] = {
    { E_SURFACE_TYPE, E_WINDOW_BIT | E_PBUFFER_BIT, E_RENDERABLE, E_ES2_BIT, E_RED, 8, E_GREEN, 8, E_BLUE, 8, E_ALPHA, 8, E_NONE },
    { E_SURFACE_TYPE, E_WINDOW_BIT | E_PBUFFER_BIT, E_RENDERABLE, E_ES2_BIT, E_RED, 5, E_GREEN, 6, E_BLUE, 5, E_NONE },
    { E_SURFACE_TYPE, E_PBUFFER_BIT, E_RENDERABLE, E_ES2_BIT, E_NONE },
    { E_SURFACE_TYPE, E_WINDOW_BIT, E_RENDERABLE, E_ES2_BIT, E_NONE },
  };
  int wi = 0;
  for (; wi < 4 && choose; wi++) if (choose(dpy, want[wi], &cfg, 1, &nc) && nc > 0) break;
  if (!cfg || nc <= 0) { GP("eglChooseConfig: nothing for GLES2 (error 0x%x)", get_error()); goto out; }
  { int id = 0; if (config_attrib) config_attrib(dpy, cfg, E_CONFIG_ID, &id);
    GP("chosen config %d (%s)", id, wi == 0 ? "RGBA8888 window+pbuffer" : wi == 1 ? "RGB565 window+pbuffer" : wi == 2 ? "pbuffer only" : "window only"); }

  if (bind_api) bind_api(E_OPENGL_ES_API);
  static const int ctx_attr[] = { E_CONTEXT_CLIENT_VERSION, 2, E_NONE };
  GP("creating GLES2 context");
  void *ctx = create_context && make_current ? create_context(dpy, cfg, NULL, ctx_attr) : NULL;
  if (!ctx) { GP("eglCreateContext FAILED (error 0x%x)", get_error()); goto out; }
  static const int pb_attr[] = { E_WIDTH, 16, E_HEIGHT, 16, E_NONE };
  void *pb = create_pbuffer && wi < 3 ? create_pbuffer(dpy, cfg, pb_attr) : NULL;
  if (!pb && wi < 3) GP("eglCreatePbufferSurface FAILED (error 0x%x)", get_error());
  if (pb) GP("eglMakeCurrent (pbuffer)");
  if (pb && make_current(dpy, pb, pb, ctx)) {
    draw_ok = gl_checks(gpa, "pbuffer");
    make_current(dpy, NULL, NULL, NULL);
  } else if (pb) {
    GP("eglMakeCurrent FAILED (error 0x%x)", get_error());
  }
  if (pb && destroy_surface) destroy_surface(dpy, pb);

  // Window surface on a Wayland surface (what Chrome draws into). Created, never swapped or committed: no window shows.
  if (wd && pr.c && wi != 2) {
    static const char *const libs[] = { "libwayland-egl.so.1", "libwayland-egl.so", "libwayland-egl.so.0", "libEGL.so.1",
                                        "libmali.so.0", "libmali.so.1", "libmali.so" };
    void *(*wlegl_create)(void *, int, int) = NULL; void (*wlegl_destroy)(void *) = NULL; const char *from = NULL;
    for (unsigned i = 0; i < sizeof libs / sizeof *libs && !wlegl_create; i++) {
      void *lh = dlopen(libs[i], RTLD_NOW | RTLD_LOCAL);
      if (!lh) continue;
      *(void **)&wlegl_create = dlsym(lh, "wl_egl_window_create"); *(void **)&wlegl_destroy = dlsym(lh, "wl_egl_window_destroy");
      if (wlegl_create && wlegl_destroy) from = libs[i]; else wlegl_create = NULL;
    }
    if (!wlegl_create) {
      GP("wl_egl_window_create: not found");
    } else {
      struct wl_surface *ws = wl_compositor_create_surface(pr.c);
      void *ew = wlegl_create(ws, 64, 64);
      GP("wl_egl_window from %s: %s", from, ew ? "created" : "FAILED");
      if (ew) {
        void *es = create_window ? create_window(dpy, cfg, ew, NULL) : NULL;
        int e = get_error();
        GP("eglCreateWindowSurface: %s (error 0x%x)", es ? "OK" : "FAILED", e);
        if (es) {
          GP("eglMakeCurrent (window surface)");
          unsigned mc = make_current(dpy, es, es, ctx);
          GP("eglMakeCurrent (window surface): %s (error 0x%x)", mc ? "OK" : "FAILED", get_error());
          window_ok = mc != 0;
          if (mc && !pb) draw_ok = gl_checks(gpa, "window surface (not shown)");
          make_current(dpy, NULL, NULL, NULL);
          if (destroy_surface) destroy_surface(dpy, es);
        }
        wlegl_destroy(ew);
      }
      wl_surface_destroy(ws);
    }
  }
  if (destroy_context) destroy_context(dpy, ctx);
out:
  if (terminate) terminate(dpy);
  if (wd) wl_display_disconnect(wd);
  int pass = wl_display_ok && window_ok && draw_ok;
  GP("done: %s (Wayland display %s, window surface %s, test draw %s)", pass ? "PASS" : "FAIL",
     wl_display_ok ? "yes" : "no", window_ok ? "yes" : "no", draw_ok ? "yes" : "no");
  return pass ? 0 : 1;
}
// 0 pass, 1 fail, 2 timed out, 3 crashed
static int gpu_probe(int limit_ms) {
  GP("start");
  pid_t p = fork();
  if (p == 0) _exit(gpu_probe_child());
  if (p < 0) { GP("fork failed"); return 1; }
  int st = 0;
  for (int t = 0; ; t += 50) {
    pid_t r = waitpid(p, &st, WNOHANG);
    if (r == p) break;
    if (r < 0 || t >= limit_ms) {
      kill(p, SIGKILL); waitpid(p, &st, 0);
      GP("TIMED OUT after %d ms: the step logged last did not finish", limit_ms);
      return 2;
    }
    struct timespec ts = { 0, 50 * 1000000L }; nanosleep(&ts, NULL);
  }
  if (WIFSIGNALED(st)) { GP("CRASHED with signal %d in the step logged last", WTERMSIG(st)); return 3; }
  return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 1;
}

// ---- GPU verdict files ----
// gpuprobe.result in /tmp: this boot's probe verdict (a slow or hanging driver costs that time once per boot).
// gpu.state in <app>/data (else /tmp): "bad" after Chrome crashed soon after an automatic GPU-on start; the GPU then
// stays off on this TV. Both start with this launcher's build id, so a new app version probes and tries again.
static const char cn_build_id[] = __DATE__ " " __TIME__;
static char gpu_state[700];
static int auto_gpu;
static int read_verdict(const char *path, char *out, size_t n) {
  char line[128]; FILE *f = fopen(path, "r"); int ok = 0;
  if (!f) return 0;
  if (fgets(line, sizeof line, f)) {
    line[strcspn(line, "\n")] = 0;
    if (!strcmp(line, cn_build_id) && fgets(out, n, f)) { out[strcspn(out, "\n")] = 0; ok = 1; }
  }
  fclose(f);
  return ok;
}
static int write_verdict(const char *path, const char *v) {
  FILE *f = fopen(path, "w");
  if (!f) return 0;
  fprintf(f, "%s\n%s\n", cn_build_id, v);
  return fclose(f) == 0;
}


// SCREEN SAVER GUARD. webOS starts its screen saver ~2-3 min after the last key press, also during a cast. tvpower
// only honours answers sent on the same luna connection that registered (as Kodi does), with clientName = the app id
// and the request's timestamp. This child does that: one luna handle (libhelpers if present,
// else its own: anonymous, then a com.webos.media.client.* name), subscribe, and on every request answer
// ack false while $tdir/playstate (patch 0049: playing media sessions) is above 0, ack true otherwise.
// Lines go to media.log as "screensaver: ...". It ends when the launcher ends.
typedef struct LSHandle LSHandle; typedef struct LSMessage LSMessage;
typedef struct { int error_code; char *message; const char *file; int line; const char *func; char pad[64]; } SSErr;
typedef bool (*SSFilter)(LSHandle *, LSMessage *, void *);
typedef struct { SSFilter cb; void *ud; void *unknown; int multiple; int pub; unsigned long tok; } SSHCtx;  // libhelpers' HContext
static bool (*ss_reg)(const char *, LSHandle **, void *), (*ss_attach)(LSHandle *, void *, void *);
static bool (*ss_call)(LSHandle *, const char *, const char *, SSFilter, void *, unsigned long *, void *);
static bool (*ss_err_init)(void *); static void (*ss_err_free)(void *); static const char *(*ss_payload)(LSMessage *);
static int (*ss_hcall)(const char *, const char *, SSHCtx *); static const char *(*ss_hmsg)(LSMessage *);
static int (*ss_hunreg)(SSHCtx *); static bool (*ss_cancel)(LSHandle *, unsigned long, void *);
static SSHCtx *ss_sub_ctx; static unsigned long ss_sub_tok; static int ss_registered;
static LSHandle *ss_bus; static int ss_via_helper, ss_logged; static char ss_state[700], ss_client[200];
#define SSL(...) do { if (ss_logged++ < 120) L("screensaver: " __VA_ARGS__); } while (0)
static void ss_err_reset(SSErr *e) { if (ss_err_free && e->message) ss_err_free(e); memset(e, 0, sizeof *e); if (ss_err_init) ss_err_init(e); }
static const char *ss_text(LSMessage *m) { const char *p = ss_via_helper ? ss_hmsg(m) : ss_payload(m); return p ? p : ""; }
static bool ss_drop(LSHandle *h, LSMessage *m, void *u) {
  (void)h; (void)u; SSL("answer reply %.160s", ss_text(m)); return true;
}
static int ss_send(const char *uri, const char *payload, SSFilter cb, int multiple) {
  if (ss_via_helper) {
    SSHCtx *c = calloc(1, sizeof *c); if (!c) return 0;  // kept: the helper hands it back on every reply
    c->cb = cb; c->multiple = multiple; c->pub = 1;
    if (ss_hcall(uri, payload, c) != 0) { free(c); return 0; }
    if (multiple) ss_sub_ctx = c;
    return 1;
  }
  SSErr e; memset(&e, 0, sizeof e); if (ss_err_init) ss_err_init(&e); unsigned long tok = 0;
  int ok = ss_call(ss_bus, uri, payload, cb, NULL, &tok, &e);
  if (!ok) SSL("%s failed: %s", uri, e.message ? e.message : "?");
  else if (multiple) ss_sub_tok = tok;
  ss_err_reset(&e); return ok;
}
// Register only while media plays (as Kodi: Inhibit at playback start, Uninhibit at stop): webOS 4 never sends a
// request while a client is registered, so an always-on registration would also keep the screen saver away on pause.
// Playing -> register (and answer ack false); stopped or paused for 3 s -> cancel, webOS runs its normal timer.
static int ss_playing(void) {
  int playing = 0; FILE *f = fopen(ss_state, "r"); if (f) { if (fscanf(f, "%d", &playing) != 1) playing = 0; fclose(f); }
  return playing;
}
static void ss_unregister(void) {
  if (!ss_registered) return;
  if (ss_via_helper) { if (ss_sub_ctx && ss_hunreg) ss_hunreg(ss_sub_ctx); ss_sub_ctx = NULL; }  // ctx leaked on purpose
  else if (ss_sub_tok && ss_cancel) { SSErr e; memset(&e, 0, sizeof e); if (ss_err_init) ss_err_init(&e); ss_cancel(ss_bus, ss_sub_tok, &e); ss_err_reset(&e); ss_sub_tok = 0; }
  ss_registered = 0;
  SSL("unregistered (nothing playing)");
}
static bool ss_on_request(LSHandle *h, LSMessage *m, void *u) {
  (void)h; (void)u;
  const char *p = ss_text(m);
  SSL("request %.200s", p);
  const char *t = strstr(p, "\"timestamp\"");
  if (!t) return true;
  t = strchr(t + 11, ':'); if (!t) return true; t++; while (*t == ' ') t++;
  char ts[64]; size_t n = 0;
  if (*t == '"') { ts[n++] = *t++; while (*t && *t != '"' && n < sizeof ts - 2) ts[n++] = *t++; if (*t == '"') ts[n++] = '"'; }
  else while ((*t >= '0' && *t <= '9') && n < sizeof ts - 1) ts[n++] = *t++;
  ts[n] = 0; if (!n) return true;
  int playing = ss_playing();
  char resp[400];
  snprintf(resp, sizeof resp, "{\"clientName\":\"%s\",\"ack\":%s,\"timestamp\":%s}", ss_client, playing > 0 ? "false" : "true", ts);
  SSL("answer %s (playing %d)", resp, playing);
  ss_send("luna://com.webos.service.tvpower/power/responseScreenSaverRequest", resp, ss_drop, 0);
  return true;
}
static bool ss_on_register(LSHandle *h, LSMessage *m, void *u) {
  const char *p = ss_text(m);
  if (strstr(p, "\"timestamp\"")) return ss_on_request(h, m, u);
  SSL("register reply %.200s", p);
  return true;
}
static void screensaver_guard(const char *tdir) {
  signal(SIGCHLD, SIG_DFL); signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGHUP, SIG_DFL);
  pid_t parent = getppid();
  snprintf(ss_state, sizeof ss_state, "%s/playstate", tdir);
  snprintf(ss_client, sizeof ss_client, "%s", app_id);
  void *G = dlopen("libglib-2.0.so.0", RTLD_NOW);
  void *ctx = NULL; int (*iter)(void *, int) = NULL;
  void *H = dlopen("libhelpers.so.2", RTLD_NOW); if (!H) H = dlopen("libhelpers.so", RTLD_NOW);
  if (H) {
    *(void **)&ss_hcall = dlsym(H, "HLunaServiceCall"); *(void **)&ss_hmsg = dlsym(H, "HLunaServiceMessage");
    *(void **)&ss_hunreg = dlsym(H, "HUnregisterServiceCallback");
    if (ss_hcall && ss_hmsg) { ss_via_helper = 1; SSL("using libhelpers"); }
  }
  if (!ss_via_helper) {
    void *Ls = dlopen("libluna-service2.so.3", RTLD_NOW);
    if (!Ls || !G) { SSL("luna/glib not found"); _exit(0); }
    *(void **)&ss_reg = dlsym(Ls, "LSRegister"); *(void **)&ss_attach = dlsym(Ls, "LSGmainAttach");
    *(void **)&ss_call = dlsym(Ls, "LSCall"); *(void **)&ss_payload = dlsym(Ls, "LSMessageGetPayload");
    *(void **)&ss_cancel = dlsym(Ls, "LSCallCancel");
    *(void **)&ss_err_init = dlsym(Ls, "LSErrorInit"); *(void **)&ss_err_free = dlsym(Ls, "LSErrorFree");
    void *(*ctx_new)(void) = dlsym(G, "g_main_context_new"); void *(*loop_new)(void *, int) = dlsym(G, "g_main_loop_new");
    *(void **)&iter = dlsym(G, "g_main_context_iteration");
    if (!ss_reg || !ss_attach || !ss_call || !ss_payload || !ctx_new || !loop_new || !iter) { SSL("luna symbols missing"); _exit(0); }
    const char *shortid = strrchr(app_id, '.') ? strrchr(app_id, '.') + 1 : app_id;
    char name[200]; snprintf(name, sizeof name, "com.webos.media.client.%s.ss%d", shortid, (int)getpid());
    const char *tries[2] = { NULL, name };
    for (int i = 0; i < 2 && !ss_bus; i++) {
      SSErr e; memset(&e, 0, sizeof e); if (ss_err_init) ss_err_init(&e); LSHandle *hh = NULL;
      if (ss_reg(tries[i], &hh, &e) && hh) { ss_bus = hh; SSL("luna handle %s", tries[i] ? tries[i] : "(anonymous)"); }
      else SSL("register %s refused: %d %s", tries[i] ? tries[i] : "(anonymous)", e.error_code, e.message ? e.message : "?");
      ss_err_reset(&e);
    }
    if (!ss_bus) _exit(0);
    ctx = ctx_new(); void *loop = loop_new(ctx, 0);
    SSErr e; memset(&e, 0, sizeof e); if (ss_err_init) ss_err_init(&e);
    if (!ss_attach(ss_bus, loop, &e)) { SSL("attach failed: %s", e.message ? e.message : "?"); _exit(0); }
    ss_err_reset(&e);
  }
  char reg[300]; snprintf(reg, sizeof reg, "{\"subscribe\":true,\"clientName\":\"%s\"}", ss_client);
  int idle_ticks = 0, tick = 0;
  while (getppid() == parent) {
    if (iter && ctx) { for (int n = 0; n < 16 && iter(ctx, 0); n++) {} }
    if (++tick % 10 == 0) {  // once a second
      if (ss_playing() > 0) {
        idle_ticks = 0;
        if (!ss_registered) {
          if (ss_send("luna://com.webos.service.tvpower/power/registerScreenSaverRequest", reg, ss_on_register, 1)) {
            ss_registered = 1; SSL("registered (media playing)");
          }
        }
      } else if (ss_registered && ++idle_ticks >= 3) {
        ss_unregister();
      }
    }
    usleep(100 * 1000);
  }
  ss_unregister();
  _exit(0);
}
static volatile sig_atomic_t got_chld, got_term;
static void on_sig(int s) { if (s == SIGCHLD) got_chld = 1; else got_term = s; }

int main(int argc, char **argv) {
  char exe[512] = {0};
  if (readlink("/proc/self/exe", exe, sizeof exe - 1) <= 0) snprintf(exe, sizeof exe, "%s", argv[0]);
  char dir[512]; snprintf(dir, sizeof dir, "%s", exe);
  char *bin = dirname(dir);                      // <app>/bin
  char appdir[512]; snprintf(appdir, sizeof appdir, "%s", bin);
  char *ad = dirname(appdir);                    // <app>
  char tmp[512]; snprintf(tmp, sizeof tmp, "%s", ad);
  app_id = strdup(basename(tmp));
  char tdir[600], sock[700], logp[700], script[700];
  snprintf(tdir, sizeof tdir, "/tmp/%s", app_id); mkdir(tdir, 0700);
  snprintf(logp, sizeof logp, "%s/media.log", tdir); lg = fopen(logp, "w");
  snprintf(sock, sizeof sock, "%s/media.sock", tdir);
  snprintf(script, sizeof script, "%s/run.sh", bin);
  if (tv_too_old(tdir, bin)) return 1;
  // troubleshooting options: /tmp/<appid>/media.opts, words: nohw (no hardware video), gpu=on|off (run.sh),
  // gpuprobe / nogpuprobe (GPU report), noss (no screen saver guard), webgpu=on (run.sh)
  char opts[256] = {0}, optp[700]; snprintf(optp, sizeof optp, "%s/media.opts", tdir);
  FILE *of = fopen(optp, "r"); if (of) { if (!fgets(opts, sizeof opts, of)) opts[0] = 0; fclose(of); }
  if (!getenv("XDG_RUNTIME_DIR")) setenv("XDG_RUNTIME_DIR", "/tmp/xdg", 1);
  L("cnrun app=%s script=%s opts=%s", app_id, script, opts);
  // LG's NDL registers its media pipeline with $APPID; without it the pipeline had appId "" and LG's media policy
  // kept the picture muted (sound played). SAM does not set it for native apps here.
  L("env APPID=%s", getenv("APPID") ? getenv("APPID") : "(unset)");
  setenv("APPID", app_id, 1);

  int ls = -1;
  // media.opts "nohw": no hardware video (troubleshooting, or test environments without LG's media stack)
  if (!getenv("CN_NO_HWVIDEO") && !strstr(opts, "nohw") && (ndl_mode = ndl_open_lib())) {
    ndl_ok = 1;
    ls = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", sock); unlink(sock);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 8)) { L("socket %s: %s", sock, strerror(errno)); close(ls); ls = -1; }
    else setenv("CN_MEDIA_SOCK", sock, 1);
  }
  if (ls < 0) { unsetenv("CN_MEDIA_SOCK"); ndl_mode = 0; }
  else {
    // API 1 has no exported window, so the compositor probe is only for API 2
    if (ndl_mode == 2) exported_set_property = probe_set_property();
    setenv("CN_EXPORTED_SET_PROPERTY", exported_set_property ? "1" : "0", 1);
  }
  // Chromium reads this before it starts: new, old (webOS 4, NDL API 1) or none (no hardware video)
  setenv("CN_NDL_API", ndl_mode == 2 ? "new" : ndl_mode == 1 ? "old" : "none", 1);
  L("ndl api: %s", getenv("CN_NDL_API"));
  snprintf(hw_state, sizeof hw_state, "%s/hw.state", tdir);
  // A "bad" from an earlier start is kept as hw.state.prev and every start tries hardware video again; a failure turns
  // it off for the rest of that session only.
  { FILE *hf = fopen(hw_state, "r"); char hv[8] = {0};
    if (hf) {
      int bad = fgets(hv, sizeof hv, hf) && !strncmp(hv, "bad", 3); fclose(hf);
      char prev[720]; snprintf(prev, sizeof prev, "%s.prev", hw_state); rename(hw_state, prev);
      if (bad) L("hw.state: an NDL API 1 call failed at an earlier start (see lastlog.prev/media.log); trying hardware video again");
    } }
  // GPU: run.sh adds --disable-gpu when CN_GPU=off. media.opts "gpu=on" / "gpu=off" override the check (tests).
  {
    const char *why;
    int on;
    int ran_probe = 0;
    char res_path[700], v[128];
    snprintf(res_path, sizeof res_path, "%s/gpuprobe.result", tdir);
    { char d[640]; snprintf(d, sizeof d, "%s/data", ad);
      if (!access(d, W_OK)) snprintf(gpu_state, sizeof gpu_state, "%s/gpu.state", d);
      else snprintf(gpu_state, sizeof gpu_state, "%s/gpu.state", tdir); }
    if (strstr(opts, "gpu=on")) { on = 1; why = "media.opts gpu=on"; }
    else if (strstr(opts, "gpu=off")) { on = 0; why = "media.opts gpu=off"; }
    else {
      int r = egl_platform_display_check();
      on = r == 0;
      why = r == 0 ? "eglGetPlatformDisplay found" : r == 2 ? "no libEGL.so.1" : "EGL check failed";
      if (r == 1) {
        // EGL 1.4 driver (webOS 5/6 Mali): Chrome takes the patch 0033 path. GPU on only if the TV's driver passes
        // what that path needs (gpuprobe), and not after the GPU crashed Chrome at an earlier start.
        int pr;
        if (read_verdict(gpu_state, v, sizeof v) && !strncmp(v, "bad", 3)) {
          on = 0; why = "EGL 1.4 driver; Chrome crashed with the GPU on at an earlier start (gpu.state)";
        } else if (strstr(opts, "nogpuprobe")) {
          on = 0; why = "EGL 1.4 driver, GPU probe turned off (nogpuprobe)";
        } else {
          int cached = !strstr(opts, "gpuprobe") && read_verdict(res_path, v, sizeof v) && v[0] >= '0' && v[0] <= '3';
          if (cached) pr = v[0] - '0';
          else { char b[8]; pr = gpu_probe(4000); ran_probe = 1; snprintf(b, sizeof b, "%d", pr); write_verdict(res_path, b); }
          on = pr == 0; auto_gpu = on;
          why = pr == 0 ? (cached ? "EGL 1.4 driver, GPU probe passed (this boot)" : "EGL 1.4 driver, GPU probe passed")
              : pr == 1 ? (cached ? "EGL 1.4 driver, GPU probe failed (this boot)" : "EGL 1.4 driver, GPU probe failed")
              : pr == 2 ? "EGL 1.4 driver, GPU probe timed out" : "EGL 1.4 driver, GPU probe crashed";
        }
      }
    }
    setenv("CN_GPU", on ? "on" : "off", 1);
    L("gpu: %s (%s)", on ? "on" : "off", why);
    // a full report on any TV on request (media.opts "gpuprobe")
    if (!ran_probe && strstr(opts, "gpuprobe") && !strstr(opts, "nogpuprobe")) gpu_probe(4000);
  }

  struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_sig;
  sigaction(SIGCHLD, &sa, NULL); sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL); sigaction(SIGHUP, &sa, NULL);
  struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
  int term_seen = 0;
  pid_t child = fork();
  if (child == 0) {
    if (ls >= 0) close(ls);
    char **av = calloc(argc + 3, sizeof *av);
    av[0] = "sh"; av[1] = script; for (int i = 1; i < argc; i++) av[i + 1] = argv[i];
    execv("/bin/sh", av);
    _exit(127);
  }
  L("chrome wrapper pid %d, media socket %s", (int)child, ls >= 0 ? sock : "(off)");
  // screen saver guard (see screensaver_guard): media.opts "noss" turns it off
  pid_t ssg = -1;
  if (!strstr(opts, "noss")) {
    ssg = fork();
    if (ssg == 0) { if (ls >= 0) close(ls); screensaver_guard(tdir); _exit(0); }
  }
  for (int i = 0; i < MAXC; i++) cl[i].fd = -1;
  int status = 0;
  for (;;) {
    if (got_term) { kill(child, got_term); got_term = 0; term_seen = 1; }
    if (got_chld) {
      got_chld = 0;
      if (ssg > 0 && waitpid(ssg, NULL, WNOHANG) == ssg) ssg = -1;
      if (waitpid(child, &status, WNOHANG) == child) break;
    }
    struct pollfd pf[MAXC + 2]; int n = 0, wl_idx = -1, cmap[MAXC + 2];
    if (ls >= 0) { pf[n].fd = ls; pf[n].events = POLLIN; cmap[n++] = -1; }
    if (dpy) { wl_display_flush(dpy); pf[n].fd = wl_display_get_fd(dpy); pf[n].events = POLLIN; wl_idx = n; cmap[n++] = -2; }
    for (int i = 0; i < MAXC; i++) if (cl[i].fd >= 0) { pf[n].fd = cl[i].fd; pf[n].events = POLLIN; cmap[n++] = i; }
    int r = poll(pf, n, 1000);
    if (r < 0) continue;
    for (int k = 0; k < n; k++) {
      if (!pf[k].revents) continue;
      if (cmap[k] == -1) {
        int c = accept4(ls, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (c < 0) continue;
        int slot = -1; for (int i = 0; i < MAXC; i++) if (cl[i].fd < 0) { slot = i; break; }
        if (slot < 0) { close(c); continue; }
        cl[slot].fd = c;
      } else if (k == wl_idx) {
        if (wl_display_dispatch(dpy) < 0) { L("wayland connection lost"); stream_close("wayland"); wl_display_disconnect(dpy); dpy = NULL; comp = NULL; foreign = NULL; exported = NULL; window_id[0] = 0; }
      } else {
        client_read(&cl[cmap[k]]);
      }
    }
  }
  stream_close("exit");
  if (ssg > 0) { kill(ssg, SIGTERM); waitpid(ssg, NULL, 0); }
  if (ls >= 0) unlink(sock);
  L("chrome exited, status %d", status);
  // Automatic GPU-on start that ended on its own soon after start (crash or error exit, not a close by webOS):
  // next start uses the GPU off. Delete gpu.state (or install a new version) to try the GPU again.
  if (auto_gpu && !term_seen) {
    struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
    long secs = (long)(t1.tv_sec - t0.tv_sec);
    int crashed = WIFSIGNALED(status) && WTERMSIG(status) != SIGTERM && WTERMSIG(status) != SIGKILL &&
                  WTERMSIG(status) != SIGINT && WTERMSIG(status) != SIGHUP;
    int failed = WIFEXITED(status) && WEXITSTATUS(status) != 0;
    if (secs < 60 && (crashed || failed)) {
      char v2[96]; snprintf(v2, sizeof v2, "bad (status %d after %ld s)", status, secs);
      L("gpu: Chrome ended after %ld s with the GPU on; the next start uses the GPU off (%s %s)", secs, gpu_state,
        write_verdict(gpu_state, v2) ? "written" : "NOT written");
    }
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
