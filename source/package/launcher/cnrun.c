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
// webOS 4 has NDL API 1 (no Load, no SetWindowId, no pts): ndl_open_old() and stream_open_old() below. Since 1.1.1
// webOS 4 uses LG's buffered player directly instead ("direct player", sf_*), with NDL API 1 as the fallback.
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

// ---- LG's Starfish player ----
// NDL API 2 is a wrapper around LG's Starfish media pipeline: NDL_DirectMediaLoad builds a JSON message and passes
// it to StarfishMediaAPIs::Load (libplayerAPIs), and NDL_DirectVideoPlay / AudioPlay call StarfishMediaAPIs::Feed.
// NDL always loads Starfish's game-streaming player ("DIRECTMEDIA-ES-PLAYER", lowDelayMode and isGameMode true).
// cnrun exports both functions under their mangled names (-rdynamic in build.sh), so the dynamic linker binds NDL's
// calls to cn_smp_load / cn_smp_feed below, which replace the load message and fix up the feeds.
typedef _Bool (*smp_load_fn)(void *, const char *, void (*)(int, long long, const char *));
__attribute__((visibility("default"))) _Bool cn_smp_load(void *self, const char *payload,
                                                           void (*cb)(int, long long, const char *))
    __asm__("_ZN17StarfishMediaAPIs4LoadEPKcPFvixS1_E");
// Default since 1.1.1 (media.opts "smp=game" turns it off): load LG's normal buffered player ("BUFFERSTREAM", the one LG's own
// web runtime and SS4S's Starfish backend use) instead of NDL's game-streaming "DIRECTMEDIA-ES-PLAYER". The message
// is rebuilt from NDL's (codec, size, PCM rate, window id) in the form SS4S sends (mariotaku/ss4s
// modules/webos/smp/src/smp_player.c, LGPL-3.0; only the JSON keys are taken). BUFFERSTREAM takes nanosecond
// timestamps, NDL feeds milliseconds, so cn_smp_feed below scales them. NDL's game mode showed H.264/HEVC with
// B-frames in bursts (#12, #17: 24 and 30 fps clips with B-frames stuttered, without B-frames they were smooth);
// with BUFFERSTREAM the same clips play smoothly (owner's TV, 2026-11-22). webOS 4 (NDL API 1) never gets here.
static int smp_buffer, smp_feeds_logged;
static long long smp_pts_scale = 1;
static int smp_json_int(const char *s, const char *key, int def) {
  char pat[64]; snprintf(pat, sizeof pat, "\"%s\":", key);
  const char *p = strstr(s, pat); return p ? atoi(p + strlen(pat)) : def;
}
static void smp_json_str(const char *s, const char *key, char *out, size_t n) {
  char pat[64]; snprintf(pat, sizeof pat, "\"%s\":\"", key);
  const char *p = strstr(s, pat); out[0] = 0; if (!p) return;
  p += strlen(pat); size_t i = 0; while (p[i] && p[i] != '"' && i + 1 < n) { out[i] = p[i]; i++; } out[i] = 0;
}
static char *smp_buffer_payload(const char *in) {
  char vcodec[16], acodec[16], win[64], app[128];
  const char *v = strstr(in, "\"codec\":{");
  smp_json_str(v ? v : in, "video", vcodec, sizeof vcodec); smp_json_str(v ? v : in, "audio", acodec, sizeof acodec);
  smp_json_str(in, "windowId", win, sizeof win); smp_json_str(in, "appId", app, sizeof app);
  // adaptiveStreaming max size: the largest picture the decoder is set up for. Not the first picture's size: YouTube
  // starts a stream at 426x240 and switches to 1080p in the same stream (the TV reset during that, 2026-11-22). NDL
  // sends 1920x1080 (or more for 4K streams); take the larger of that and the stream's own size.
  int w = smp_json_int(in, "videoWidth", 1920), h = smp_json_int(in, "videoHeight", 1080);
  const char *as = strstr(in, "\"adaptiveStreaming\":");
  int mw = as ? smp_json_int(as, "maxWidth", 1920) : 1920, mh = as ? smp_json_int(as, "maxHeight", 1080) : 1080;
  if (mw < 1920) mw = 1920;
  if (mh < 1080) mh = 1080;
  if (w < mw) w = mw;
  if (h < mh) h = mh;
  const char *ps = strstr(in, "\"pcmInfo\":");
  int rate = ps ? smp_json_int(ps, "sampleRate", 1) : 1;
  static const double khz[] = { 48, 48, 44.1, 32, 24, 16, 12, 8, 22.05 };
  double k = (rate >= 0 && rate <= 8) ? khz[rate] : 48;
  if (!vcodec[0]) return NULL;
  char aud[64] = "", pcm[160] = "";
  if (acodec[0]) {
    snprintf(aud, sizeof aud, ",\"audio\":\"%s\"", acodec);
    snprintf(pcm, sizeof pcm, ",\"pcmInfo\":{\"channelMode\":\"stereo\",\"format\":\"S16LE\",\"sampleRate\":%.2f,"
             "\"bitsPerSample\":16,\"layout\":\"interleaved\"}", k);
  }
  char *out = malloc(2048); if (!out) return NULL;
  snprintf(out, 2048,
    "{\"args\":[{\"mediaTransportType\":\"BUFFERSTREAM\",\"option\":{\"appId\":\"%s\",\"windowId\":\"%s\","
    "\"externalStreamingInfo\":{\"contents\":{\"codec\":{\"video\":\"%s\"%s},"
    "\"esInfo\":{\"pauseAtDecodeTime\":true,\"ptsToDecode\":0,\"seperatedPTS\":true},\"format\":\"RAW\",\"provider\":\"Chrome\""
    "%s},"
    "\"streamQualityInfo\":true,\"audioSync\":true,\"streamQualityInfoCorruptedFrame\":true,\"streamQualityInfoNonFlushable\":true,"
    "\"restartStreaming\":false,\"bufferingCtrInfo\":{\"bufferMaxLevel\":0,\"bufferMinLevel\":0,\"preBufferByte\":0,"
    "\"qBufferLevelAudio\":0,\"qBufferLevelVideo\":0,\"srcBufferLevelAudio\":{\"minimum\":1,\"maximum\":1048576},"
    "\"srcBufferLevelVideo\":{\"minimum\":1,\"maximum\":8388608}}},"
    "\"needAudio\":false,\"queryPosition\":false,"
    "\"adaptiveStreaming\":{\"audioOnly\":false,\"maxWidth\":%d,\"maxHeight\":%d,\"maxFrameRate\":60}}}]}",
    app, win, vcodec, aud, pcm, w, h);
  return out;
}
_Bool cn_smp_load(void *self, const char *payload, void (*cb)(int, long long, const char *)) {
  static smp_load_fn real;
  if (!real) {
    void *h = dlopen("libplayerAPIs.so.1", RTLD_NOW | RTLD_LOCAL);
    if (h) *(void **)&real = dlsym(h, "_ZN17StarfishMediaAPIs4LoadEPKcPFvixS1_E");
    if (!real) { L("smp: libplayerAPIs Load not found (%s)", dlerror()); return 0; }
  }
  const char *msg = payload ? payload : "";
  char *c = NULL;
  smp_feeds_logged = 0; smp_pts_scale = 1;
  // BUFFERSTREAM needs cn_smp_feed (ns timestamps). It only gets NDL's feeds when libplayerAPIs has Feed under the
  // C++11 string ABI name we export; an older library (other webOS releases) would leave the feeds in ms with a
  // BUFFERSTREAM load. Then keep NDL's own player.
  static int feed_ok = -1;
  if (feed_ok < 0) {
    void *h = dlopen("libplayerAPIs.so.1", RTLD_NOW | RTLD_LOCAL);
    feed_ok = h && dlsym(h, "_ZN17StarfishMediaAPIs4FeedB5cxx11EPKc");
    if (!feed_ok) L("smp: libplayerAPIs has no C++11 Feed, NDL's own player stays");
  }
  if (smp_buffer && feed_ok && (c = smp_buffer_payload(msg))) {
    L("smp load from NDL: %s", msg);
    msg = c; smp_pts_scale = 1000000;  // ms -> ns
  }
  L("smp load%s: %s", c ? " (BUFFERSTREAM)" : " (NDL's game player)", msg);
  _Bool r = real(self, msg, cb);
  L("smp load = %d", r);
  free(c);
  return r;
}

// StarfishMediaAPIs::Feed returns a std::string: on 32-bit ARM the caller passes the return slot first, then `this`.
// Exported like Load above, so NDL's per-frame Feed lands here: it logs the first feeds of a stream and, for
// BUFFERSTREAM, scales "pts" (ms from NDL) to ns.
typedef void (*smp_feed_fn)(void *, void *, const char *);
__attribute__((visibility("default"))) void cn_smp_feed(void *ret, void *self, const char *payload)
    __asm__("_ZN17StarfishMediaAPIs4FeedB5cxx11EPKc");
void cn_smp_feed(void *ret, void *self, const char *payload) {
  static smp_feed_fn real;
  if (!real) {
    void *h = dlopen("libplayerAPIs.so.1", RTLD_NOW | RTLD_LOCAL);
    if (h) *(void **)&real = dlsym(h, "_ZN17StarfishMediaAPIs4FeedB5cxx11EPKc");
    if (!real) { L("smp: libplayerAPIs Feed not found (%s)", dlerror()); abort(); }
  }
  char buf[256]; const char *msg = payload;
  const char *p = payload ? strstr(payload, "\"pts\":") : NULL;
  if (p && smp_pts_scale != 1) {
    long long pts = atoll(p + 6); const char *e = p + 6; while (*e == '-' || (*e >= '0' && *e <= '9')) e++;
    int n = snprintf(buf, sizeof buf, "%.*s\"pts\":%lld%s", (int)(p - payload), payload, pts * smp_pts_scale, e);
    if (n > 0 && n < (int)sizeof buf) msg = buf;
  }
  real(ret, self, msg);
  // the result is a libstdc++ std::string (C++11 ABI): its first word points at the characters
  const char *res = ret ? *(const char *const *)ret : NULL;
  // BUFFERSTREAM answers "BufferFull" when its queue is full; NDL then drops the frame (a lost reference frame
  // smears the picture). Wait and send the same frame again (up to 2 s); the launcher stops reading the socket
  // meanwhile, so the renderer waits too.
  for (int i = 0; smp_pts_scale != 1 && res && strstr(res, "BufferFull") && i < 400; i++) {
    usleep(5000);
    // destroy the old result by hand before constructing the next one in the same slot: a std::string on 32-bit
    // libstdc++ is {char *p; size_t n; char local[16]}, and p points at local unless the text is longer than 15
    // bytes ({"returnValue":"BufferFull"} is), in which case operator new allocated it (operator delete = free)
    char *old = *(char **)ret; if (old != (char *)ret + 8) free(old);
    real(ret, self, msg);
    res = *(const char *const *)ret;
  }
  static int bad_logged;
  int ok = res && strstr(res, "Ok");
  if (smp_feeds_logged < 6) { smp_feeds_logged++; L("smp feed: %s -> %s", msg, res ? res : "?"); }
  else if (!ok && bad_logged < 20) { bad_logged++; L("smp feed not ok: %s -> %s", msg, res ? res : "?"); }
}

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

// ---- LG's buffered player, driven by the launcher itself ("direct player") ----
// webOS 4's NDL (API 1) takes frames without timestamps, and its picture came in bursts of 2-3 frames with 70-100 ms
// holds (#6, #12, #31's B9). On webOS 4 the launcher therefore drives LG's buffered player itself, the way LG's own
// browser plays MSE video: StarfishMediaAPIs (libplayerAPIs) with a BUFFERSTREAM load and timestamped feeds, and
// the picture placed with LG's app switching control block (ACB, libAcbAPI), because webOS 4's compositor has no
// exported video windows. webOS 5+ get the same player through NDL (cn_smp_load above); media.opts "smp=direct"
// uses this path there too (tests), with the exported window instead of ACB.
// Where the steps come from (no code is taken from either):
// - the ACB calls and their order (create, initialize, setMediaId before Load, setSinkType + setState LOADED and
//   Play at "load completed", setDisplayWindow, setState PLAYING at the first feed, UNLOADED after Unload):
//   SS4S (mariotaku/ss4s, LGPL-3.0) modules/webos/smp/src/smp_resource_acb.c and smp_player.c, and webosbrew's
//   media guide (webOS 4 places the video with ACB). ACB types: webosbrew/webos-userland (Unlicense).
// - that the player object is larger than the public header declares, and that Unload can take a long time:
//   Wren (sostk/webos-wpe-browser, GPL-3.0) browser/lg_media_player.cpp, which drives the same player.
// The load message is cn_smp_load's webOS 5+ one, without a window id on webOS 4.
// Member functions are called as plain functions with `this` first (ARM EABI); Feed returns a std::string through a
// hidden result slot (r0), as in cn_smp_feed.
#include <pthread.h>
static struct {
  void (*ctor)(void *, const char *), (*dtor)(void *), (*feed)(void *, void *, const char *);
  const char *(*media_id)(void *);
  _Bool (*fg)(void *), (*play)(void *), (*eos)(void *), (*unload)(void *);
  _Bool (*load)(void *, const char *, void (*)(int, long long, const char *));
  long (*acb_create)(void);
  _Bool (*acb_init)(long, int, const char *, void (*)(long, long, long, long, long, const char *));
  _Bool (*acb_media_id)(long, const char *), (*acb_sink)(long, int);
  int (*acb_vdata)(long, const char *, long *), (*acb_adata)(long, const char *, long *);
  int (*acb_state)(long, int, int, long *), (*acb_window)(long, long, long, long, long, _Bool, long *);
  void (*acb_destroy)(long);
} sf;
enum { SF_OFF, SF_ACB, SF_WINDOW };
static int sf_mode;                 // how the direct player places the picture; SF_OFF: NDL as before
static int sf_bad;                  // the direct player failed in this session: later streams use NDL
static pthread_mutex_t sf_mu = PTHREAD_MUTEX_INITIALIZER;
static void *sf_api;                // the current stream's player object, NULL when none
static long sf_acb;                 // its ACB id (SF_ACB)
static int sf_audio, sf_fed, sf_feeds_logged, sf_events_logged;
static volatile int sf_ready, sf_acb_playing;  // "load completed" seen; ACB told PLAYING (only after LOADED: ACB
                                               // answers "Invalid State" to PLAYING before it, Beanjar webOS 4.4.3)
static unsigned char *sf_blocks[4]; // player objects; never freed (a late callback may still refer to one)
static int sf_next_block;

// Looks up everything the direct player needs; any missing symbol leaves it off. acb: also libAcbAPI.
static int sf_setup(int acb) {
  void *p = dlopen("libplayerAPIs.so.1", RTLD_NOW | RTLD_GLOBAL);
  if (!p) { L("direct player: %s", dlerror()); return 0; }
  // dlsym on the library's handle, not RTLD_DEFAULT: the launcher exports Load and Feed itself (cn_smp_load/feed)
#define F(v, n) do { *(void **)&sf.v = dlsym(p, n); if (!sf.v) { L("direct player: no %s", n); return 0; } } while (0)
  F(ctor, "_ZN17StarfishMediaAPIsC1EPKc"); F(dtor, "_ZN17StarfishMediaAPIsD1Ev");
  F(media_id, "_ZN17StarfishMediaAPIs10getMediaIDEv"); F(fg, "_ZN17StarfishMediaAPIs16notifyForegroundEv");
  F(load, "_ZN17StarfishMediaAPIs4LoadEPKcPFvixS1_E"); F(feed, "_ZN17StarfishMediaAPIs4FeedB5cxx11EPKc");
  F(play, "_ZN17StarfishMediaAPIs4PlayEv"); F(eos, "_ZN17StarfishMediaAPIs7pushEOSEv");
  F(unload, "_ZN17StarfishMediaAPIs6UnloadEv");
#undef F
  if (!acb) return 1;
  void *a = dlopen("libAcbAPI.so.1", RTLD_NOW | RTLD_GLOBAL);
  if (!a) { L("direct player: %s", dlerror()); return 0; }
#define A(v, n) do { *(void **)&sf.v = dlsym(a, n); if (!sf.v) { L("direct player: no %s", n); return 0; } } while (0)
  A(acb_create, "AcbAPI_create"); A(acb_init, "AcbAPI_initialize"); A(acb_media_id, "AcbAPI_setMediaId");
  A(acb_sink, "AcbAPI_setSinkType"); A(acb_state, "AcbAPI_setState"); A(acb_window, "AcbAPI_setDisplayWindow");
  A(acb_destroy, "AcbAPI_destroy"); A(acb_vdata, "AcbAPI_setMediaVideoData");
#undef A
  *(void **)&sf.acb_adata = dlsym(a, "AcbAPI_setMediaAudioData");  // not on every firmware
  return 1;
}

// Calls Feed and copies its answer. The std::string result (32-bit libstdc++, C++11 ABI) is {char *p; size_t n;
// char local[16]}: p points at local for short text, else at memory from operator new (operator delete = free).
static void sf_call_feed(void *api, const char *json, char *out, size_t n) {
  void *slot[6] = {0};
  sf.feed(slot, api, json);
  const char *s = slot[0];
  snprintf(out, n, "%s", s ? s : "");
  if (s && s != (const char *)slot + 8) free((void *)s);
}

enum { SF_ACB_FOREGROUND = 1, SF_PLAYSTATE_UNLOADED = 0, SF_PLAYSTATE_LOADED = 1, SF_PLAYSTATE_PLAYING = 2 };
static void sf_acb_cb(long id, long task, long ev, long app, long play, const char *reply) {
  if (sf_events_logged < 40) L("acb event %ld app %ld play %ld: %s", ev, app, play, reply ? reply : "-");
}
static void sf_place(void) {
  if (sf_mode != SF_ACB || !sf_acb) return;
  int x = geom_x, y = geom_y, w = geom_w, h = geom_h;
  if (w <= 0 || h <= 0) { x = 0; y = 0; w = 1920; h = 1080; }
  long task = 0;
  int r = sf.acb_window(sf_acb, x, y, w, h, x == 0 && y == 0 && w >= 1920 && h >= 1080, &task);
  L("direct player: display window %d,%d %dx%d = %d", x, y, w, h, r);
}
// Player events (LG's thread). Event numbers: PF_EVENT_T in webosbrew/webos-userland StarfishMediaAPIs.h.
static void sf_event(int type, long long num, const char *str) {
  pthread_mutex_lock(&sf_mu);
  void *api = sf_api; long acb = sf_acb;
  pthread_mutex_unlock(&sf_mu);
  if (type != 0x0 && type != 0x2c && type != 0x2d && type != 0x2e && type != 0x26 && type != 0x27 && type != 0x31 &&
      sf_events_logged < 40) {
    sf_events_logged++;
    L("direct player: event 0x%x %lld %.200s", type, num, str ? str : "");
  }
  if (!api) return;
  long task = 0;
  switch (type) {
  case 0x16:  // load completed: the player is ready, start it
    if (acb) { sf.acb_sink(acb, 0); sf.acb_state(acb, SF_ACB_FOREGROUND, SF_PLAYSTATE_LOADED, &task); }
    L("direct player: load completed, play = %d", sf.play(api));
    sf_ready = 1;
    if (acb && sf_fed > 0 && !sf_acb_playing) {
      sf_acb_playing = 1;
      L("direct player: ACB playing = %d", sf.acb_state(acb, SF_ACB_FOREGROUND, SF_PLAYSTATE_PLAYING, &task));
    }
    break;
  case 0x4:  // video info: ACB needs it to set up the picture
    if (acb && str) sf.acb_vdata(acb, str, NULL);
    break;
  case 0x7:  // audio info
    if (acb && str && sf.acb_adata) sf.acb_adata(acb, str, NULL);
    break;
  case 0x12: case 0x13:
    L("direct player: error %lld %.200s", num, str ? str : "");
    break;
  }
}

// Opens a stream on the direct player. 0: it could not (the caller falls back to NDL).
static int sf_open(const struct cn_open *o, const char *window) {
  static const char *names[] = { NULL, "H264", "H265", "VP9", "AV1" };  // cn_open codec (patch 0023 NdlCodec)
  const char *codec = o->codec >= 1 && o->codec <= 4 ? names[o->codec] : NULL;
  if (!codec) { L("direct player: codec %d unknown", o->codec); return 0; }
  if (!sf_blocks[sf_next_block] && !(sf_blocks[sf_next_block] = calloc(1, 64 * 1024))) return 0;
  void *api = sf_blocks[sf_next_block]; sf_next_block = (sf_next_block + 1) % 4;
  memset(api, 0, 64 * 1024);
  sf.ctor(api, NULL);
  long acb = 0;
  if (sf_mode == SF_ACB) {
    acb = sf.acb_create();
    if (!acb || !sf.acb_init(acb, 10 /* PLAYER_TYPE_MSE */, app_id, sf_acb_cb)) {
      L("direct player: ACB create/initialize failed (%ld)", acb);
      if (acb) sf.acb_destroy(acb);
      sf.dtor(api);
      return 0;
    }
  }
  sf_events_logged = 0; sf_feeds_logged = 0; sf_fed = 0; sf_ready = 0; sf_acb_playing = 0;
  sf_audio = o->audio_rate > 0;
  pthread_mutex_lock(&sf_mu); sf_api = api; sf_acb = acb; pthread_mutex_unlock(&sf_mu);
  sf.fg(api);
  const char *mid = sf.media_id(api);
  if (acb) sf.acb_media_id(acb, mid ? mid : "");
  int w = o->width > 1920 ? o->width : 1920, h = o->height > 1080 ? o->height : 1080;  // see smp_buffer_payload
  char win[160] = "", aud[48] = "", pcm[200] = "";
  if (window && window[0]) snprintf(win, sizeof win, ",\"windowId\":\"%s\"", window);
  if (sf_audio) {
    snprintf(aud, sizeof aud, ",\"audio\":\"PCM\"");
    snprintf(pcm, sizeof pcm, ",\"pcmInfo\":{\"channelMode\":\"stereo\",\"format\":\"S16LE\",\"sampleRate\":%.2f,"
             "\"bitsPerSample\":16,\"layout\":\"interleaved\"}", o->audio_rate / 1000.0);
  }
  char msg[2048];
  snprintf(msg, sizeof msg,
    "{\"args\":[{\"mediaTransportType\":\"BUFFERSTREAM\",\"option\":{\"appId\":\"%s\"%s,"
    "\"externalStreamingInfo\":{\"contents\":{\"codec\":{\"video\":\"%s\"%s},"
    "\"esInfo\":{\"pauseAtDecodeTime\":true,\"ptsToDecode\":0,\"seperatedPTS\":true},\"format\":\"RAW\",\"provider\":\"Chrome\"%s},"
    "\"streamQualityInfo\":true,\"audioSync\":%s,\"streamQualityInfoCorruptedFrame\":true,\"streamQualityInfoNonFlushable\":true,"
    "\"restartStreaming\":false,\"bufferingCtrInfo\":{\"bufferMaxLevel\":0,\"bufferMinLevel\":0,\"preBufferByte\":0,"
    "\"qBufferLevelAudio\":0,\"qBufferLevelVideo\":0,\"srcBufferLevelAudio\":{\"minimum\":1,\"maximum\":1048576},"
    "\"srcBufferLevelVideo\":{\"minimum\":1,\"maximum\":8388608}}},"
    "\"needAudio\":false,\"queryPosition\":false,"
    "\"adaptiveStreaming\":{\"audioOnly\":false,\"maxWidth\":%d,\"maxHeight\":%d,\"maxFrameRate\":60}}}]}",
    app_id, win, codec, aud, pcm, sf_audio ? "true" : "false", w, h);
  L("direct player (%s): load %s", sf_mode == SF_ACB ? "ACB" : "window", msg);
  _Bool r = sf.load(api, msg, sf_event);
  L("direct player: load = %d, media id %s", r, mid ? mid : "?");
  if (!r) {
    pthread_mutex_lock(&sf_mu); sf_api = NULL; sf_acb = 0; pthread_mutex_unlock(&sf_mu);
    if (acb) sf.acb_destroy(acb);
    sf.dtor(api);
    return 0;
  }
  sf_place();
  return 1;
}

// Feeds one access unit (video) or PCM block (audio); pts in ms since the stream's OPEN, the player wants ns.
static void sf_send(int video, const void *p, unsigned int n, long long pts_ms) {
  void *api = sf_api; if (!api) return;
  if (!video && !sf_audio) return;
  char json[160], res[128];
  snprintf(json, sizeof json, "{\"bufferAddr\":\"%p\",\"bufferSize\":%u,\"pts\":%lld,\"esData\":%d}",
           p, n, pts_ms * 1000000LL, video ? 1 : 2);
  sf_call_feed(api, json, res, sizeof res);
  // a full queue: wait and send the same unit again (up to 2 s); the socket is not read meanwhile, so the renderer
  // waits too (as cn_smp_feed does on webOS 5+)
  for (int i = 0; strstr(res, "BufferFull") && i < 400; i++) { usleep(5000); sf_call_feed(api, json, res, sizeof res); }
  int ok = strstr(res, "Ok") != NULL;
  if (sf_feeds_logged < 6 || (!ok && sf_feeds_logged < 30)) { sf_feeds_logged++; L("direct player: feed %s -> %s", json, res); }
  if (ok && video) sf_fed++;
  if (ok && video && sf_acb && sf_ready && !sf_acb_playing) {
    sf_acb_playing = 1;
    long task = 0; int r = sf.acb_state(sf_acb, SF_ACB_FOREGROUND, SF_PLAYSTATE_PLAYING, &task);
    L("direct player: ACB playing = %d", r);
  }
  if (ok && video && (sf_fed == 1 || sf_fed % 1500 == 0)) L("direct player: video frame %d pts %lld ms", sf_fed, pts_ms);
}

// Unload can take long (Wren's notes) and must not hold up the next stream for good: it runs on its own thread,
// and the caller waits at most 3 s for it.
struct sf_gone { void *api; long acb; int fed; volatile int done; };
static void *sf_unload_thread(void *arg) {
  struct sf_gone *g = arg;
  if (g->fed) sf.eos(g->api);
  _Bool r = sf.unload(g->api);
  sf.dtor(g->api);
  if (g->acb) { long task = 0; sf.acb_state(g->acb, SF_ACB_FOREGROUND, SF_PLAYSTATE_UNLOADED, &task); sf.acb_destroy(g->acb); }
  L("direct player: unload = %d after %d frames", r, g->fed);
  g->done = 1;
  return NULL;
}
static void sf_close(const char *why) {
  pthread_mutex_lock(&sf_mu);
  void *api = sf_api; long acb = sf_acb; sf_api = NULL; sf_acb = 0;
  pthread_mutex_unlock(&sf_mu);
  if (!api) return;
  struct sf_gone *g = calloc(1, sizeof *g);  // freed by whoever finishes last; on a timeout it is kept (small)
  if (!g) return;
  g->api = api; g->acb = acb; g->fed = sf_fed;
  L("direct player: close (%s)", why);
  pthread_t t;
  if (pthread_create(&t, NULL, sf_unload_thread, g)) { sf_unload_thread(g); free(g); return; }
  pthread_detach(t);
  for (int i = 0; i < 300 && !g->done; i++) usleep(10000);
  if (g->done) free(g); else L("direct player: unload still running after 3 s, going on");
}

static void stream_close(const char *why) {
  if (sf_api) { sf_close(why); loaded = 0; cur_owner = -1; memset(cur_token, 0, sizeof cur_token); return; }
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
  if (sf_mode != SF_OFF && !sf_bad) {
    const char *wid = NULL;
    if (sf_mode == SF_WINDOW) { wid = chrome_window_id; if (!wid[0]) { if (!wayland_up()) return; wid = window_id; } }
    if (sf_open(o, wid)) {
      vframes = 0; loaded = 1; cur_owner = fd; memcpy(cur_token, o->token, sizeof cur_token);
      cur_rate = o->audio_rate; src_w = o->width; src_h = o->height;
      if (sf_mode == SF_WINDOW) {
        if (chrome_window_id[0] && winid_fd >= 0) {
          uint32_t m[4] = { CN_VSIZE, 8, (uint32_t)o->width, (uint32_t)o->height };
          if (write(winid_fd, m, sizeof m) != sizeof m) L("vsize to browser failed");
        } else {
          place_video();
        }
      }
      return;
    }
    sf_bad = 1;
    L("direct player failed: NDL from now on in this session");
  }
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
    if (sf_api) { sf_send(type == CN_VIDEO, p + 8, len - 8, pts); break; }
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
      if (loaded && sf_api && sf_mode == SF_ACB) sf_place();
      else if (loaded && !chrome_window_id[0]) place_video();
      if (loaded && !sf_api && ndl_mode == 1 && ndl_setarea) {
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
// SIGUSR1 / SIGUSR2 come from Chrome (patch 0057, which finds us through CN_LAUNCHER_PID): show "Press Back again to
// exit", and end the app (Chrome quit; if it is still running 3 s later it gets SIGTERM, after 8 s SIGKILL).
static volatile sig_atomic_t got_chld, got_term, got_toast, got_quit;
static void on_sig(int s) {
  if (s == SIGCHLD) got_chld = 1;
  else if (s == SIGUSR1) got_toast = 1;
  else if (s == SIGUSR2) got_quit = 1;
  else got_term = s;
}

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
  // gpuprobe / nogpuprobe (GPU report), noss (no screen saver guard), smp=game (NDL's game-streaming player), smp=ndl
  // (webOS 4: NDL API 1 instead of the direct player), smp=direct (webOS 5+: the direct player), webgpu=on, omnibox=lite (run.sh)
  char opts[256] = {0}, optp[700]; snprintf(optp, sizeof optp, "%s/media.opts", tdir);
  // the whole file (run.sh reads its options from any line), newlines shown as spaces in the log
  FILE *of = fopen(optp, "r");
  if (of) { size_t n = fread(opts, 1, sizeof opts - 1, of); opts[n] = 0; fclose(of); for (char *c = opts; *c; c++) if (*c == '\n') *c = ' '; }
  smp_buffer = strstr(opts, "smp=game") == NULL;  // default since 1.1.1; media.opts "smp=game" = NDL's own
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
    // the direct player: by default on webOS 4 (NDL API 1), with ACB; media.opts "smp=ndl" keeps NDL there.
    // On webOS 5+ only with media.opts "smp=direct" (tests), with the exported window.
    if (ndl_mode == 1 && !strstr(opts, "smp=ndl")) sf_mode = sf_setup(1) ? SF_ACB : SF_OFF;
    else if (ndl_mode == 2 && strstr(opts, "smp=direct")) sf_mode = sf_setup(0) ? SF_WINDOW : SF_OFF;
    L("direct player: %s", sf_mode == SF_ACB ? "on (ACB)" : sf_mode == SF_WINDOW ? "on (exported window)" : "off");
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
  sigaction(SIGUSR1, &sa, NULL); sigaction(SIGUSR2, &sa, NULL);
  { char pid[16]; snprintf(pid, sizeof pid, "%d", (int)getpid()); setenv("CN_LAUNCHER_PID", pid, 1); }
  struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
  int term_seen = 0;
  struct timespec quit_t = {0, 0}; int quit_step = 0;  // 1 = SIGTERM sent, 2 = SIGKILL sent
  pid_t toast_pid = -1;
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
    if (got_toast) {
      got_toast = 0;
      if (toast_pid < 0) {  // in a child: luna-send must not hold up the media socket
        toast_pid = fork();
        if (toast_pid == 0) { if (ls >= 0) close(ls); toast("Press Back again to exit"); _exit(0); }
      }
    }
    if (got_quit) {
      got_quit = 0;
      if (!quit_t.tv_sec) { clock_gettime(CLOCK_MONOTONIC, &quit_t); L("chrome is quitting"); }
    }
    if (quit_t.tv_sec) {
      struct timespec n; clock_gettime(CLOCK_MONOTONIC, &n);
      long ms = (n.tv_sec - quit_t.tv_sec) * 1000 + (n.tv_nsec - quit_t.tv_nsec) / 1000000;
      if (ms >= 8000 && quit_step < 2) { L("chrome still running %ld ms after quitting: SIGKILL", ms); kill(child, SIGKILL); quit_step = 2; }
      else if (ms >= 3000 && quit_step < 1) { L("chrome still running %ld ms after quitting: SIGTERM", ms); kill(child, SIGTERM); quit_step = 1; term_seen = 1; }
    }
    if (got_chld) {
      got_chld = 0;
      if (ssg > 0 && waitpid(ssg, NULL, WNOHANG) == ssg) ssg = -1;
      if (toast_pid > 0 && waitpid(toast_pid, NULL, WNOHANG) == toast_pid) toast_pid = -1;
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
