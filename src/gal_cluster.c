/*
 * Copyright (c) 2026 fifthBro
 * https://fifthbro.github.io
 *
 * Licensed under CC BY-NC-SA 4.0
 * https://creativecommons.org/licenses/by-nc-sa/4.0/
 * NOT FOR COMMERCIAL USE
 */

/*
 * MH2P Android Auto Navigation Image Hook (LD_PRELOAD)
 *
 * Forces NavigationStatusService type=2 (full rendered map images)
 * in the ServiceDiscoveryResponse sent to the phone.
 *
 * MH2P currently advertises type=1 (small turn icons) → gets PNGs in /tmp.
 * This hook rewrites the protobuf wire format to claim type=2 with
 * configurable image dimensions, causing the phone to send full cluster
 * map images instead.
 *
 * Architecture:
 *   LD_PRELOAD into gal process → intercepts libautoreceiver.so functions
 *
 *   PRIMARY HOOK: MessageRouter::populateServiceDiscoveryResponse()
 *     Called via PLT (non-virtual) → GUARANTEED to fire on MH2P.
 *     Serializes the ServiceDiscoveryResponse, rewrites NavigationStatusService
 *     type=1→2 + ImageOptions, parses back. This is the authoritative path.
 *
 *   SECONDARY HOOKS: NavigationStatusEndpoint::addDiscoveryInfo() etc.
 *     May NOT fire on MH2P because gal::CNavigationStatusImpl overrides
 *     these via vtable dispatch (bypasses PLT). Kept for logging/diagnostics
 *     and compatibility with platforms that don't override.
 *
 * Ported from MHI2 aa_navimg_hook.c — same libautoreceiver.so interface,
 * same object offsets, same protobuf field numbers.
 *
 * Build (QNX 6.6, ARMv7):
 *   qcc -Vgcc_ntoarmv7le -shared -fPIC -O2 -std=gnu99 \
 *       -o aa_navimg_hook.so aa_navimg_hook.c -lc
 *
 * Run:
 *   LD_PRELOAD=/fs/sda0/mapview/aa_navimg_hook.so gal0
 *
 * Env vars:
 *   AA_NAVIMG_WIDTH=800      Image width (default 800)
 *   AA_NAVIMG_HEIGHT=480     Image height (default 480)
 *   AA_NAVIMG_DEPTH=32       Color depth bits (default 32)
 *   AA_NAVIMG_MIN_MS=50      Min frame interval ms (default 50)
 *   AA_NAVIMG_DUMP_EVERY=30  Dump every Nth image (default 30)
 *   GAL_CLUSTER_LOG_ALL=1      Enable logging to /tmp/gal_cluster.log (default 0 = file not created at all)
 *   GAL_CLUSTER_LOG_DETAILS=1  Log event metadata (default 0)
 *   GAL_CLUSTER_SD_LOG=1       Log ServiceDiscoveryResponse + dump aa_sdresp_*.bin (default 0)
 *   GAL_CLUSTER_NAV_HEXDUMP=1  Hex-dump raw protobuf bytes of nav events (default 0)
 *   GAL_CLUSTER_INSET_TOP      stable_content_insets.top in codec-frame px (default 0)
 *   GAL_CLUSTER_INSET_LEFT     stable_content_insets.left in codec-frame px (default 0)
 *   GAL_CLUSTER_INSET_BOTTOM   stable_content_insets.bottom in codec-frame px (default 0)
 *   GAL_CLUSTER_INSET_RIGHT    stable_content_insets.right in codec-frame px (default 0)
 *   (GAL_CLUSTER_KEEPALIVE_MS removed — self-drive thread is now controlled
 *    solely by GAL_CLUSTER_FAKE_MAIN_PROJECTED, hardcoded 33ms interval.)
 *   GAL_CLUSTER_RATE_LOG=1     Per-second per-channel outgoing byte rates to gal_cluster.log.
 *                              Used to identify which channels go silent at HMI focus changes.
 *   GAL_CLUSTER_MIRROR_FOCUS=1 Mirror gal0's real VIDEO_FOCUS state to cluster ch=14 instead of
 *                              hardcoding PROJECTED. Default 0 (hardcoded). With 1, cluster
 *                              follows main: NATIVE during HMI off-AA → phone idles cleanly →
 *                              recovers on HMI return. Cost: cluster blanks during off-AA.
 *   GAL_CLUSTER_FAKE_MAIN_PROJECTED=1
 *                              Rewrite gal0's outgoing ch=1 0x8008 NATIVE → PROJECTED so phone
 *                              never sees main HMI lose focus. Goal: keep cluster encoder running
 *                              at full rate during HMI off-AA so cluster shows real map content.
 *                              Risk: phone-side behaviors keyed off main focus may misbehave.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <screen/screen.h>
#include "cluster_h264_shm.h"

#define LOG_PATH "/tmp/gal_cluster.log"
#define DUMP_DIR "/tmp"
#define MAX_SERIALIZE_BYTES (2 * 1024 * 1024)

typedef void (*HandleNextTurnFunc)(void* self, const void* event);
typedef void (*AddDiscoveryFunc)(void* self, void* resp);
typedef int (*StartFunc)(void* self);
typedef int (*StopFunc)(void* self);
typedef void (*HandleNavStatusFunc)(void* self, const void* status);
typedef void (*HandleNavDistanceFunc)(void* self, const void* dist);
typedef int (*ByteSizeFunc)(const void* msg);    /* protobuf 2.x: int ByteSize() */
typedef size_t (*ByteSizeLongFunc)(const void* msg); /* protobuf 3.x: size_t ByteSizeLong() */
typedef void (*PopulateSDRespFunc)(void* self, void* resp);
typedef int (*MessageLite_SerializeToArray)(const void* msg, void* data, int size);
typedef int (*MessageLite_ParseFromArray)(void* msg, const void* data, int size);
typedef int (*MergeFunc)(void* self, void* cis);
typedef void (*SerializeWithCachedSizesFunc)(const void* self, void* coded_output_stream);
typedef void (*COS_WriteRawFunc)(void* cos, const void* data, int size);
typedef int (*HandleChannelOpenReqFunc)(void* self, uint8_t channel, const void* req);
typedef int (*SSL_write_func)(void* ssl, const void* data, int len);

static SSL_write_func real_SSL_write = NULL;
static HandleNextTurnFunc real_handle = NULL;
static AddDiscoveryFunc real_add_discovery = NULL;
static StartFunc real_start = NULL;
static StopFunc real_stop = NULL;
static HandleNavStatusFunc real_handle_status = NULL;
static HandleNavDistanceFunc real_handle_distance = NULL;
static PopulateSDRespFunc real_populate_sd = NULL;
static ByteSizeFunc real_byte_size = NULL;
static ByteSizeFunc real_navstatus_size = NULL;
static ByteSizeFunc real_navdist_size = NULL;
static ByteSizeFunc real_sd_size = NULL;
static ByteSizeFunc real_navfocusreq_size = NULL;
static MessageLite_SerializeToArray real_serialize_to_array = NULL;
static MessageLite_ParseFromArray real_parse_from_array = NULL;
static MergeFunc real_navfocusreq_merge = NULL;
static SerializeWithCachedSizesFunc real_sd_serialize_cached = NULL;
static COS_WriteRawFunc real_cos_write_raw = NULL;
static HandleChannelOpenReqFunc real_handle_channel_open = NULL;

/* Pre-built cluster service bytes (built once in constructor) */
static uint8_t* g_cluster_service_bytes = NULL;
static size_t g_cluster_service_len = 0;
static int g_cluster_enabled = -1; /* cached from env */
static volatile int g_cluster_ch14_opened = 0; /* set by dummy_on_channel_opened */
static volatile int g_inflate_sdresp = 0; /* flag: use modified SD response */
static uint8_t* g_modified_sdresp = NULL;  /* complete modified SD response bytes */
static size_t g_modified_sdresp_len = 0;
static uint8_t* g_original_sdresp = NULL;  /* original SD response for SSL_write matching */
static size_t g_original_sdresp_len = 0;
static volatile int g_cluster_needs_ack = 0; /* set by dummy_cluster_route, sent by queueOutgoing */
static volatile uint16_t g_cluster_ack_type = 0;
static cluster_h264_shm_t* g_cluster_shm = NULL;

/* stable_content_insets sent in VideoConfiguration.ui_config.f3.
 * Phone uses these to keep critical UI (turn arrows, distance text, ETA bar)
 * inside the safe rectangle when laying out widgets. Decorative pixels (map
 * background, route line) still draw to the full encoded frame and get
 * eaten silently by the cluster bezel.
 * Values are in encoded-frame pixels (codec_resolution sized). All zero =
 * disabled (byte-identical to prior hardcoded blob). */
static int g_inset_top    = 0;
static int g_inset_left   = 0;
static int g_inset_bottom = 0;
static int g_inset_right  = 0;

/* Per-channel outgoing byte rate logging.
 * When GAL_CLUSTER_RATE_LOG=1, queueOutgoing accumulates bytes per channel
 * and dumps a single line every second showing per-channel byte/s rates.
 * Used to see which channels go silent when HMI focus moves off AA, so we
 * can correlate cluster decay with what gal0 stops sending.
 * Off by default — high log volume. */
static int       g_rate_log = 0;
#define RATE_LOG_NUM_CHANNELS 32
static uint64_t  g_rate_bytes[RATE_LOG_NUM_CHANNELS];     /* outgoing */
static uint64_t  g_rate_in_bytes[RATE_LOG_NUM_CHANNELS];  /* incoming (cluster ch=14 only currently) */
static uint64_t  g_rate_last_log_ms = 0;
static uint64_t  g_rate_session_start_ms = 0;

/* Per-(channel, msg_type) counter so we can see WHAT messages flow on each
 * channel during the HMI focus transition. Reset every second alongside
 * the byte counters; entries themselves persist for table reuse. */
#define RATE_LOG_TYPES_MAX 128
static struct {
    uint8_t  ch;
    uint16_t type;
    uint32_t count;
} g_rate_types[RATE_LOG_TYPES_MAX];
static int g_rate_types_n = 0;

/* Self-driven 0x8004 thread. Two modes:
 *
 *  GAL_CLUSTER_KEEPALIVE_MS=N (default 0): fires 0x8004 on ch=14 only at
 *      N-ms interval while HMI is NATIVE. Keeps cluster channel alive but
 *      ch=1 goes natural — phone sees inconsistent state. Empirically
 *      keeps phone sending low-rate frames on ch=14 (stale-but-flowing).
 *
 *  GAL_CLUSTER_FAKE_MAIN_PROJECTED=1: fires 0x8004 on BOTH ch=1 and ch=14
 *      at 33ms while HMI is NATIVE, alongside the ch=1 0x8008 rewrite.
 *      Comprehensive fake — phone sees consistent "AA active" state. */
#define FAKE_MAIN_INTERVAL_MS 33
static int          g_keepalive_ms = 0;
static pthread_t    g_keepalive_tid = 0;
static volatile int g_keepalive_running = 0;

/* HMI video-focus state, set from gal0's 0x8008 on ch=1 (PROJECTED=on AA,
 * NATIVE=off AA). Used to gate the keep-alive thread (fires only when ch=1
 * is silent so ch=14 outgoing stays at constant ~30Hz). */
static volatile int g_hmi_focus_projected = 1;  /* assume on at session start */

/* When 1, mirror gal0's real focus state byte to ch=14 0x8008 (NATIVE during
 * HMI off-AA, PROJECTED during on-AA). When 0 (default), hardcode PROJECTED
 * regardless of gal0 — the original behavior.
 *
 * Theory: hardcoded PROJECTED creates an inconsistent protocol state (main
 * NATIVE + cluster PROJECTED) that the phone's encoder state machine can't
 * recover from. Mirroring real state lets phone idle both channels cleanly
 * during HMI off-AA and resume both when HMI returns. Trade-off: cluster
 * goes blank during HMI off-AA period (vs hardcoded which traps in frozen
 * frame state — same visual result, but mirroring recovers on return). */
static int g_mirror_focus = 0;

/* When 1, intercept gal0's outgoing ch=1 0x8008 messages and rewrite the
 * state byte to PROJECTED before forwarding to phone. Phone never sees main
 * HMI lose focus, so it should keep both main and cluster encoders running
 * at full rate even when user switches HMI to Radio/Settings.
 *
 * Risk: gal0's internal state diverges from what phone sees. Other phone-
 * side behaviors keyed off main video focus (audio routing, notifications,
 * etc) may misbehave because phone thinks AA is always foreground.
 *
 * Default 0 (off) — opt-in experimental. */
static int g_fake_main_projected = 0;

/* When 1, on HMI focus return to AA (ch=1 0x8008 NATIVE→PROJECTED transition),
 * fire a fresh restart sequence on ch=14: MEDIA_SETUP_RESPONSE (0x8003) +
 * VIDEO_FOCUS_NOTIFICATION (0x8001 PROJECTED). Goal: re-assert cluster's
 * claim on bandwidth so phone doesn't deprioritize it when main video resumes
 * streaming. Paired with KEEPALIVE_MS — only meaningful if cluster was kept
 * alive during off-AA and is now demoted by phone-side encoder priority. */
static int g_restart_on_return = 0;

/* When HMI returns to AA (NATIVE→PROJECTED on ch=1), the restart-on-return
 * logic sends 0x8001 NATIVE on ch=14 immediately and schedules 0x8001
 * PROJECTED for later. This counter is in keep-alive ticks (33ms each).
 * Decremented each tick; when it hits 1, fires PROJECTED. 500ms gives phone
 * enough time to register NATIVE state (deallocate cluster encoder) before
 * we ask for resume — short toggles get coalesced and phone never re-keys. */
#define RESTART_PROJECTED_DELAY_TICKS 15  /* ~500ms at 33ms keep-alive interval */
static volatile int g_pending_projected_ticks = 0;

/* When 1, on HMI focus return to AA (ch=1 0x8008 NATIVE→PROJECTED), fire a
 * true protocol-level Stop+Start on ch=14: 0x8002 Stop (empty payload, 2 bytes
 * `80 02`) immediately, then 0x8001 Start with minimal proto (`80 01 08 00
 * 10 00` — Start{f1=0, f2=0}) after GAL_CLUSTER_STOPSTART_DELAY_MS so phone
 * has time to process the Stop. Distinct from RESTART_ON_RETURN which sent
 * Start+Start with non-zero field-1 values and was empirically proven not to
 * recover (session 7, 2026-05-15 log: bandwidth collapsed to 3-4fps after
 * Start+Start fire at HMI return).
 *
 * Per libautoreceiver disasm: Stop has no fields, Start::IsInitialized
 * requires both bits 0 and 1 of has_fields. Minimal payload with both fields
 * present should pass that gate. */
static int g_stopstart_on_return = 0;
static int g_stopstart_delay_ms = 200;
#define STOPSTART_DEFAULT_DELAY_MS 200
static volatile int g_pending_start_ticks = 0;


/* NvMedia function pointers (resolved in constructor, used by decoder process) */
typedef void* (*fn_nv_generic)(void);
static struct {
    fn_nv_generic ParserCreate;
    fn_nv_generic ParserSetAttr;
    fn_nv_generic ParserParse;
    fn_nv_generic ParserDestroy;
    fn_nv_generic DeviceCreate;
    fn_nv_generic DecoderCreateEx;
    fn_nv_generic DecoderDestroy;
    fn_nv_generic DecoderRender;
    fn_nv_generic DecoderSetAttr;
    fn_nv_generic MixerCreate;
    fn_nv_generic MixerDestroy;
    fn_nv_generic MixerRender;
    fn_nv_generic SurfaceCreate;
    fn_nv_generic SurfaceDestroy;
    fn_nv_generic SurfaceWait;
    fn_nv_generic SiblingCreate;
    fn_nv_generic SurfaceLock;
    fn_nv_generic SurfaceUnlock;
} g_nv = {0};


/* Forward declaration */
static void log_line(const char* fmt, ...);

/* ------------------------------------------------------------------ */
/*  In-hook cluster H.264 decoder (runs inside gal's process)         */
/* ------------------------------------------------------------------ */
#define CL_MAX_SURFACES 8
#define CL_SHM_NAME "/cluster_shm"
#define CL_SHM_MAGIC 0x43505643u  /* "CPVC" — same as mirror2 expects */
#define CL_SHM_SLOTS 3                /* ring slots — eliminates frame tearing */
#define CL_SHM_SLOT_BYTES (1280 * 720 * 4)  /* sized for 720p RGBX — phone
                                             * now sends 1280×720 video after
                                             * we advertise codec_res=2 on the
                                             * cluster service. cluster_mirror's
                                             * data[] is also SLOTS×SLOT_BYTES
                                             * so no out-of-bounds this time. */

/* version 2: 3-slot ring. version 1 single-slot. */
typedef struct {
    volatile uint32_t magic;
    volatile uint32_t version;
    volatile uint32_t width;
    volatile uint32_t height;
    volatile uint32_t stride;
    volatile uint32_t format;
    volatile uint32_t sequence;
    volatile uint32_t current_slot;   /* index of most recently-written slot */
    volatile uint32_t frame_ready;
    volatile uint32_t _pad;
    uint8_t data[CL_SHM_SLOTS * CL_SHM_SLOT_BYTES];
} cluster_decoded_shm_t;

static struct {
    void* device;
    void* decoder;
    void* mixer;
    void* parser;
    void* surfaces[CL_MAX_SURFACES];
    int   surf_refcount[CL_MAX_SURFACES];
    int   num_surfaces;
    int   width, height;
    int   frames_decoded, frames_displayed;
    int   frame_counter;      /* monotonic counter written to pCurrPic[5] per gal */
    /* DINT post window */
    screen_context_t scr_ctx;
    screen_window_t  post_win;
    screen_buffer_t  post_bufs[CL_MAX_SURFACES];
    void*  siblings[CL_MAX_SURFACES];
    int    post_idx;
    /* Separate APPLICATION_CONTEXT for the read_pix. The old gal_vc_frame_hook
     * achieved ~120-160 MB/s pixmap memcpy with this; using gal's video-pipeline
     * context (g_cl.scr_ctx) gives us only ~80 MB/s — likely uncached memory
     * attributes inherited from gal's context. Splitting it gives the read
     * pixmap cached normal RAM. */
    screen_context_t read_ctx;
    screen_pixmap_t  read_pix;
    screen_buffer_t  read_buf;
    void*  read_ptr;
    int    read_stride;
    /* Debug flag — set via GAL_CLUSTER_DEBUG env. Gates /tmp/cluster_decoded.raw
     * and /tmp/cluster_stream.h264 file dumps. Default off in production. */
    int    debug;
    /* In-hook decode flag — set via GAL_CLUSTER_DECODE env. When 0 (default), the
     * worker thread, NvMedia decoder, screen_post/read pipeline, and
     * /dev/shmem/cluster_shm fill are all SKIPPED. Use 1 only for the
     * legacy production cluster_mirror path. The standalone aa_cluster_decoder
     * does its own decode from /dev/shmem/cluster_h264_shm and doesn't need
     * the hook to decode. */
    int    decode_in_hook;
    /* SHM for decoded RGBA frames */
    cluster_decoded_shm_t* shm;
    int    ready;
    /* Worker thread + H.264 byte ring (decouple decode from AA protocol thread) */
    pthread_t      worker_tid;
    int            worker_started;
    pthread_mutex_t ring_mx;
    pthread_cond_t  ring_cv;
    uint8_t*       ring;              /* 2 MB H.264 byte ring */
    uint32_t       ring_size;
    volatile uint32_t ring_rd;
    volatile uint32_t ring_wr;
} g_cl = {0};
#define CL_RING_SIZE (2 * 1024 * 1024)

/* Logging flags — moved up here from later in the file because cl_display_picture
 * (defined further down) gates per-frame timing on g_log_all. Initialized from
 * env in hook_init constructor. */
static int g_log_all = 0;
static int g_log_details = 0;
static int g_log_sd = 0;
static int g_nav_hexdump = 0;

static int cl_setup_screen(int w, int h);
static int cl_begin_sequence(void* user, int* si);
static int cl_decode_picture(void* user, void* pic_info);
static int cl_display_picture(void* user, void* disp_info);
static int cl_alloc_picture(void* user, void** pic_buf);
static int cl_release(void* user, void* pic);
static int cl_addref(void* user, void* pic);
static int cl_stub(void* user, void* p) { return 0; }

static int cl_begin_sequence(void* user, int* si) {
    if (!si) return 0;
    int w = si[10], h = si[0xb], dpb = si[5];
    int bs_size = si[0x16];
    if (bs_size < 0x80000) bs_size = 0x100000;
    int nbuf = dpb + 4; if (nbuf < 4) nbuf = 4; if (nbuf > CL_MAX_SURFACES) nbuf = CL_MAX_SURFACES;

    log_line("[gal_cluster] BeginSequence: %dx%d dpb=%d\n", w, h, dpb);
    /* Dump first 256 bytes of sequence info — this is where SPS fields live
     * (chroma_format_idc, num_ref_frames, log2_max_frame_num, scaling_lists
     * if present, etc). We need these to fill NvMediaPictureInfoH264. */
    {
        uint8_t* sib = (uint8_t*)si;
        for (int row = 0; row < 256; row += 32) {
            log_line("[gal_cluster] si+%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n", row,
                     *(uint32_t*)(sib+row+0),  *(uint32_t*)(sib+row+4),
                     *(uint32_t*)(sib+row+8),  *(uint32_t*)(sib+row+12),
                     *(uint32_t*)(sib+row+16), *(uint32_t*)(sib+row+20),
                     *(uint32_t*)(sib+row+24), *(uint32_t*)(sib+row+28));
        }
    }
    g_cl.width = w; g_cl.height = h;

    typedef void* (*fn_dec_create)(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
    typedef void  (*fn_dec_set_attr)(void*,uint32_t,void*);
    typedef void* (*fn_mix_create)(void*,uint32_t,uint32_t,float,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
    typedef void* (*fn_surf_create)(void*,uint32_t,uint32_t,uint32_t);

    /* Must set up screen (post_win + siblings) BEFORE decoder/mixer,
     * because we now use siblings directly as decode targets (gal's FLAG=0 mode)
     * instead of separate NvMediaVideoSurface objects. */
    if (!g_cl.ready) cl_setup_screen(w, h);

    if (!g_cl.decoder && g_nv.DecoderCreateEx) {
        g_cl.decoder = ((fn_dec_create)g_nv.DecoderCreateEx)(0, w, h, dpb, bs_size, nbuf, 0);
        log_line("[gal_cluster]   decoder=%p\n", g_cl.decoder);
        if (g_cl.decoder && g_nv.DecoderSetAttr) {
            int dpb_val = dpb;
            ((fn_dec_set_attr)g_nv.DecoderSetAttr)(g_cl.decoder, 1, &dpb_val);
        }
    }
    /* Mixer no longer needed in sibling-as-target mode, but leave setup in case
     * we fall back. gal's display callback (FLAG=0) skips mixer entirely. */
    if (!g_cl.mixer && g_nv.MixerCreate && g_cl.device) {
        float aspect = (float)w / (float)h;
        g_cl.mixer = ((fn_mix_create)g_nv.MixerCreate)(g_cl.device, w, h, aspect,
                       0,0,0,0,0,0,0,0, 0x20000, 0);
        log_line("[gal_cluster]   mixer=%p\n", g_cl.mixer);
    }
    /* Use the CL_MAX_SURFACES siblings (created from post_win buffers in
     * cl_setup_screen) as decode targets. With 2 siblings the H.264 DPB
     * gets clobbered every frame — phone sends ref_idc=1 P-slices that ARE
     * references for the next frame, so we need headroom for 2-3 active
     * references plus the current decode target. 8 gives plenty. */
    g_cl.num_surfaces = CL_MAX_SURFACES;
    for (int i = 0; i < CL_MAX_SURFACES; i++) {
        g_cl.surfaces[i] = g_cl.siblings[i];
        g_cl.surf_refcount[i] = 0;
    }
    return nbuf;
}

/* Port of gal::nvSetParamsH264 (gal.c FUN_00115eb0 @ 167431).
 * Translates NvParser's picture-descriptor struct (pd) into the NvMedia
 * H.264 pic_info layout that NvMediaVideoDecoderRender expects.
 * Without this translation TVMRVideoDecoderRender rejects the input. */
static void cl_translate_h264_pd(const void* parser_pd, void* nv_pic_info_out) {
    const uint8_t* pd = (const uint8_t*)parser_pd;
    uint8_t* pi = (uint8_t*)nv_pic_info_out;

    memset(pi, 0, 0x2e0);

    /* Words 0..2 */
    *(uint32_t*)(pi +  0) = *(const uint32_t*)(pd + 200);    /* pi[0]: cmd/flags from pd[50] */
    *(uint32_t*)(pi +  4) = *(const uint32_t*)(pd + 0xcc);   /* pi[1] */
    *(uint32_t*)(pi +  8) = *(const uint32_t*)(pd + 0x24);   /* pi[2]: pCurrPic (NvMediaVideoSurface*) */

    /* Shorts */
    *(uint16_t*)(pi + 12) = *(const uint16_t*)(pd + 0x2c);
    *(uint16_t*)(pi + 14) = *(const uint16_t*)(pd + 0xc4);

    /* Packed bytes 0x10..0x28 */
    pi[0x10] = *(const uint8_t*)(pd + 0x0c);
    pi[0x11] = *(const uint8_t*)(pd + 0x10);
    pi[0x12] = *(const uint8_t*)(pd + 0x90);
    pi[0x13] = *(const uint8_t*)(pd + 0xbe);
    pi[0x14] = *(const uint8_t*)(pd + 0xbf);
    pi[0x15] = *(const uint8_t*)(pd + 0xac);
    pi[0x16] = *(const uint8_t*)(pd + 0xb0);
    pi[0x17] = *(const uint8_t*)(pd + 0x88);
    pi[0x18] = *(const uint8_t*)(pd + 0xbd);
    pi[0x19] = *(const uint8_t*)(pd + 0xc2);
    pi[0x1a] = *(const uint8_t*)(pd + 0xc3);
    pi[0x1b] = *(const uint8_t*)(pd + 0xb4);
    pi[0x1c] = *(const uint8_t*)(pd + 0xa4);
    pi[0x1d] = *(const uint8_t*)(pd + 0xa8);
    pi[0x1e] = *(const uint8_t*)(pd + 0x78);
    pi[0x1f] = *(const uint8_t*)(pd + 0x7c);
    pi[0x20] = *(const uint8_t*)(pd + 0x80);
    pi[0x21] = *(const uint8_t*)(pd + 0x84);
    pi[0x22] = *(const uint8_t*)(pd + 0x8c);
    pi[0x23] = *(const uint8_t*)(pd + 0xc0);
    pi[0x24] = *(const uint8_t*)(pd + 0xc1);
    pi[0x25] = *(const uint8_t*)(pd + 0xbc);
    pi[0x26] = *(const uint8_t*)(pd + 0xb8);
    pi[0x27] = *(const uint8_t*)(pd + 0xd2);
    pi[0x28] = *(const uint8_t*)(pd + 0xd3);

    /* Words 0xb, 0xc */
    *(uint32_t*)(pi + 0x2c) = *(const uint32_t*)(pd + 0xd8);
    *(uint32_t*)(pi + 0x30) = *(const uint32_t*)(pd + 0xdc);

    /* Bytes 0x34, 0x35 */
    pi[0x34] = *(const uint8_t*)(pd + 0xd0);
    pi[0x35] = *(const uint8_t*)(pd + 0xd1);

    /* SPS/PPS scaling lists */
    memcpy(pi + 0x36, pd + 0x6a8, 0x60);
    memcpy(pi + 0x96, pd + 0x708, 0x80);

    /* Words 0xb6, 0xb7 at byte 0x2d8, 0x2dc */
    *(uint32_t*)(pi + 0x2d8) = *(const uint32_t*)(pd + 0x44);
    *(uint32_t*)(pi + 0x2dc) = *(const uint32_t*)(pd + 0x48);

    /* Reference picture loop: 16 entries × 7 words.
     * First ref at word 0x46 (byte 0x118); each iter advances pi by 7 words, pd by 0x20 bytes. */
    uint8_t* rpi = pi;
    const uint8_t* rpd = pd;
    for (int i = 0; i < 16; i++) {
        int32_t  ref_obj = *(const int32_t*)(rpd + 0xe8);
        uint32_t flags   = *(const uint32_t*)(rpd + 0xfc);

        *(uint16_t*)(rpi + 0x4c*4) = (uint16_t)*(const uint32_t*)(rpd + 0xec);
        *(uint32_t*)(rpi + 0x47*4) = *(const uint32_t*)(rpd + 0xf0);
        *(uint32_t*)(rpi + 0x4a*4) = *(const uint32_t*)(rpd + 0x100);
        *(uint32_t*)(rpi + 0x48*4) = flags & 1u;
        *(uint32_t*)(rpi + 0x49*4) = (flags << 30) >> 31;
        *(uint32_t*)(rpi + 0x4b*4) = *(const uint32_t*)(rpd + 0x104);

        int32_t ref_handle = 0;
        if (ref_obj != 0) ref_handle = *(const int32_t*)(ref_obj + 8);
        *(uint32_t*)(rpi + 0x46*4) = (uint32_t)ref_handle;

        rpi += 7 * 4;
        rpd += 0x20;
    }
}

static int cl_decode_picture(void* user, void* pic_info) {
    if (!pic_info || !g_cl.decoder || !g_nv.DecoderRender) return 1;
    typedef int (*fn_dec_render)(void*,void*,void*,uint32_t,void*);
    int* pi = (int*)pic_info;
    int* curr_pic = (int*)pi[2];
    if (!curr_pic) return 1;
    void* target = (void*)curr_pic[2];
    if (!target) return 1;

    /* Fix #2: gal writes a frame counter to pCurrPic[5] (byte 0x14) before
     * DecoderRender — see gal.c:169320-169321. TVMR may dereference through
     * pCurrPic looking for it. We never set it → could be a missing pointer. */
    g_cl.frame_counter++;
    curr_pic[5] = g_cl.frame_counter;

    /* Fix #1: arg5 is the bitstream descriptor {pBitstreamData, nBitstreamDataLen}
     * from pd[0xe], pd[0xd] (see gal.c:169314-169315). MUST be static — TVMR
     * may read it asynchronously after cl_decode_picture returns, so it can't
     * live on our stack. */
    static struct { void* pBitstreamData; uint32_t nBitstreamDataLen; uint32_t pad[6]; }
        bs_desc __attribute__((aligned(8)));
    bs_desc.pBitstreamData   = (void*)(uintptr_t)pi[14];
    bs_desc.nBitstreamDataLen = (uint32_t)pi[13];

    /* Translate parser output to NvMedia pic_info (per gal::nvSetParamsH264) */
    static uint8_t nv_pi[1024] __attribute__((aligned(8)));
    cl_translate_h264_pd(pic_info, nv_pi);

    if (g_cl.frames_decoded == 0) {
        uint32_t* dec = (uint32_t*)g_cl.decoder;
        uint32_t* surf = (uint32_t*)target;
        log_line("[gal_cluster] DEC decoder=%p [0]=%08x [1]=%08x valid=%d\n",
                 g_cl.decoder, dec[0], dec[1], (int)(dec[5] & 0xff));
        log_line("[gal_cluster] DEC surface=%p fmt=%u w=%u h=%u tvmr=%p\n",
                 target, surf[0], surf[1], surf[2], (void*)surf[4]);
        /* Dump pd at multiple ranges — FUN_00115eb0 reads at pd+0x6a8 (SPS
         * scaling lists, 0x60 bytes), pd+0x708 (PPS scaling lists, 0x80 bytes),
         * and pd+0xe8+16*0x20 (reference picture table). Need to see if those
         * regions are populated. */
        uint8_t* pdb = (uint8_t*)pic_info;
        log_line("[gal_cluster] --- pd dump: head 0x140 bytes ---\n");
        for (int row = 0; row < 0x140; row += 32) {
            log_line("[gal_cluster] pd+%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n", row,
                     *(uint32_t*)(pdb+row+0),  *(uint32_t*)(pdb+row+4),
                     *(uint32_t*)(pdb+row+8),  *(uint32_t*)(pdb+row+12),
                     *(uint32_t*)(pdb+row+16), *(uint32_t*)(pdb+row+20),
                     *(uint32_t*)(pdb+row+24), *(uint32_t*)(pdb+row+28));
        }
        log_line("[gal_cluster] --- pd dump: ref table 0xe8..0x2e8 ---\n");
        for (int row = 0xe0; row < 0x2f0; row += 32) {
            log_line("[gal_cluster] pd+%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n", row,
                     *(uint32_t*)(pdb+row+0),  *(uint32_t*)(pdb+row+4),
                     *(uint32_t*)(pdb+row+8),  *(uint32_t*)(pdb+row+12),
                     *(uint32_t*)(pdb+row+16), *(uint32_t*)(pdb+row+20),
                     *(uint32_t*)(pdb+row+24), *(uint32_t*)(pdb+row+28));
        }
        log_line("[gal_cluster] --- pd dump: SPS/PPS scaling lists 0x6a0..0x790 ---\n");
        for (int row = 0x6a0; row < 0x790; row += 32) {
            log_line("[gal_cluster] pd+%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n", row,
                     *(uint32_t*)(pdb+row+0),  *(uint32_t*)(pdb+row+4),
                     *(uint32_t*)(pdb+row+8),  *(uint32_t*)(pdb+row+12),
                     *(uint32_t*)(pdb+row+16), *(uint32_t*)(pdb+row+20),
                     *(uint32_t*)(pdb+row+24), *(uint32_t*)(pdb+row+28));
        }
        uint32_t* npi = (uint32_t*)nv_pi;
        log_line("[gal_cluster] DEC nv_pi [0]=%08x [1]=%08x [2]=%08x [0xb]=%08x [0xc]=%08x [0xb6]=%08x\n",
                 npi[0], npi[1], npi[2], npi[0xb], npi[0xc], npi[0xb6]);
    }

    int dec_rc = ((fn_dec_render)g_nv.DecoderRender)(g_cl.decoder, target, nv_pi, 1, &bs_desc);
    g_cl.frames_decoded++;
    if (g_cl.frames_decoded <= 3)
        log_line("[gal_cluster] DecoderRender rc=%d bs_ptr=%p bs_len=%u frame#=%d\n",
                 dec_rc, bs_desc.pBitstreamData, bs_desc.nBitstreamDataLen,
                 g_cl.frame_counter);
    return 1;
}

static int cl_display_picture(void* user, void* disp_info) {
    g_cl.frames_displayed++;
    if (!disp_info || !g_cl.ready) return 1;

    int* di = (int*)disp_info;
    void* yuv = (void*)di[2];      /* sibling (= decoded surface) */
    if (!yuv) return 1;

    typedef int (*fn_mix_render)(void*,void*,uint32_t,void*,uint32_t,uint32_t,uint32_t);
    typedef int (*fn_surf_wait)(void*,uint32_t);

    /* yuv points to one of g_cl.siblings[0..N-1]. Find its index so we post
     * the matching post_buf (shares GPU memory with the sibling). */
    int pidx = -1;
    for (int i = 0; i < g_cl.num_surfaces; i++) {
        if (g_cl.siblings[i] == yuv) { pidx = i; break; }
    }
    if (pidx < 0) pidx = g_cl.post_idx;

    /* Wait for async decode to finish on the yuv surface before reading it. */
    if (g_nv.SurfaceWait) ((fn_surf_wait)g_nv.SurfaceWait)(yuv, 0);

    /* Full pipeline path: post → flush(WAIT_IDLE) → screen_read_window → memcpy SHM.
     * cluster_mirror reads SHM and renders to display 33. With the DPB fix in
     * place (num_surfaces=8, decrement-clamp release semantics) the decoder
     * produces clean YUV that screen_read_window's CSC converts to RGBA8888.
     * Per-frame cost dominated by the ~46 ms uncached-pixmap → SHM memcpy. */
    /* Timestamps only collected when logging is enabled — saves 5 syscalls
     * per frame when LOG_ALL=0 (production). */
    struct timespec ts0 = {0}, ts1 = {0}, ts2 = {0}, ts3 = {0}, ts4 = {0};
    if (g_log_all) clock_gettime(CLOCK_MONOTONIC, &ts0);

    int dirty[4] = { 0, 0, g_cl.width, g_cl.height };
    screen_post_window(g_cl.post_win, g_cl.post_bufs[pidx], 1, dirty, 0);
    g_cl.post_idx = (pidx + 1) % g_cl.num_surfaces;
    if (g_log_all) clock_gettime(CLOCK_MONOTONIC, &ts1);

    if (g_cl.scr_ctx)
        screen_flush_context(g_cl.scr_ctx, 1 /* SCREEN_WAIT_IDLE */);
    if (g_log_all) clock_gettime(CLOCK_MONOTONIC, &ts2);

    int rr = -1;
    typedef int (*fn_rw)(screen_window_t, screen_buffer_t, int, const int*, int);
    fn_rw rw = (fn_rw)dlsym(RTLD_DEFAULT, "screen_read_window");
    if (rw && g_cl.read_buf)
        rr = rw(g_cl.post_win, g_cl.read_buf, 0, NULL, 0);
    if (g_log_all) clock_gettime(CLOCK_MONOTONIC, &ts3);

    int copied = 0;
    if (rr == 0 && g_cl.read_ptr && g_cl.shm) {
        uint32_t next_slot = (g_cl.shm->current_slot + 1) % CL_SHM_SLOTS;
        uint8_t* dst = g_cl.shm->data + (size_t)next_slot * CL_SHM_SLOT_BYTES;
        int row, row_bytes = g_cl.width * 4;
        for (row = 0; row < g_cl.height; row++)
            memcpy(dst + (size_t)row * row_bytes,
                   (uint8_t*)g_cl.read_ptr + (size_t)row * g_cl.read_stride,
                   row_bytes);
        g_cl.shm->sequence++;
        __atomic_store_n(&g_cl.shm->current_slot, next_slot, __ATOMIC_RELEASE);
        g_cl.shm->frame_ready = 1;
        copied = 1;

        /* Debug-only: dump frame 100 RGBA. Gated by GAL_CLUSTER_DEBUG env var. */
        if (g_cl.debug && g_cl.frames_displayed == 100 && g_cl.read_ptr) {
            size_t total = (size_t)g_cl.width * g_cl.height * 4;
            FILE* f = fopen("/tmp/cluster_decoded.raw", "wb");
            if (f) {
                fwrite(g_cl.read_ptr, 1, total, f);
                fclose(f);
                log_line("[gal_cluster] dumped frame=100 RGBA to /tmp/cluster_decoded.raw (%zu bytes, %dx%d)\n",
                         total, g_cl.width, g_cl.height);
            }
        }
    }
    if (g_log_all) clock_gettime(CLOCK_MONOTONIC, &ts4);

    if (g_log_all && (g_cl.frames_displayed <= 3 || (g_cl.frames_displayed % 100) == 0)) {
        long us_post  = (ts1.tv_sec - ts0.tv_sec)*1000000L + (ts1.tv_nsec - ts0.tv_nsec)/1000L;
        long us_flush = (ts2.tv_sec - ts1.tv_sec)*1000000L + (ts2.tv_nsec - ts1.tv_nsec)/1000L;
        long us_read  = (ts3.tv_sec - ts2.tv_sec)*1000000L + (ts3.tv_nsec - ts2.tv_nsec)/1000L;
        long us_copy  = (ts4.tv_sec - ts3.tv_sec)*1000000L + (ts4.tv_nsec - ts3.tv_nsec)/1000L;
        long us_total = (ts4.tv_sec - ts0.tv_sec)*1000000L + (ts4.tv_nsec - ts0.tv_nsec)/1000L;
        log_line("[gal_cluster] frame=%d read_rc=%d copied=%d post=%ldus flush=%ldus read=%ldus copy=%ldus total=%ldus\n",
                 g_cl.frames_displayed, rr, copied,
                 us_post, us_flush, us_read, us_copy, us_total);
    }
    return 1;
}

static int cl_alloc_picture(void* user, void** pic_buf) {
    if (!pic_buf) return 0;
    int i;
    for (i = 0; i < g_cl.num_surfaces; i++) {
        if (g_cl.surf_refcount[i] == 0 && g_cl.surfaces[i]) {
            static int pic_bufs[CL_MAX_SURFACES][6];
            pic_bufs[i][0] = i;
            pic_bufs[i][2] = (int)g_cl.surfaces[i];
            pic_bufs[i][4] = 1;
            g_cl.surf_refcount[i] = 1;
            *pic_buf = &pic_bufs[i][0];
            return 1;
        }
    }
    return 0;
}

static int cl_release(void* user, void* pic) {
    if (!pic) return 1;
    int* p = (int*)pic;
    int idx = p[0];
    if (idx >= 0 && idx < CL_MAX_SURFACES) {
        /* Decrement, don't zero. The parser may call release multiple times
         * for the same picture (once per outstanding reference). The surface
         * is only truly free when refcount hits 0. Zeroing unconditionally
         * destroyed reference frames the decoder still needed for P-slice
         * decode → corrupted output (proven 2026-04-26). */
        if (g_cl.surf_refcount[idx] > 0) g_cl.surf_refcount[idx]--;
        if (p[4] > 0) p[4]--;
    }
    return 1;
}

static int cl_addref(void* user, void* pic) {
    if (!pic) return 1;
    int* p = (int*)pic;
    p[4]++;
    int idx = p[0];
    if (idx >= 0 && idx < CL_MAX_SURFACES) g_cl.surf_refcount[idx]++;
    return 1;
}

/* Hook screen_create_context to capture gal's context */
static int (*real_screen_create_context)(screen_context_t*, int) = NULL;
static screen_context_t g_gal_ctx = NULL;

int screen_create_context(screen_context_t *pctx, int flags) {
    if (!real_screen_create_context)
        real_screen_create_context = (int(*)(screen_context_t*, int))
            dlsym(RTLD_NEXT, "screen_create_context");
    if (!real_screen_create_context) return -1;
    int rc = real_screen_create_context(pctx, flags);
    if (rc == 0 && pctx && *pctx)
        g_gal_ctx = *pctx;
    return rc;
}

/* Hook NvMediaVideoMixerRenderSurface to capture gal's NvMediaDevice.
 * Per libnvmedia.so disassembly (NvMediaVideoMixerCreate @ 22905):
 *   mixer = malloc(0x5c); ... *(int *)(mixer + 0x2C) = device;
 * So mixer+0x2C holds the NvMediaDevice pointer gal uses.
 * TVMR hardware is exclusive to that device, so we must reuse it
 * for our own decode/mixer to avoid rc=7/rc=1 failures. */
static void* g_gal_nvdevice = NULL;
static void* g_seen_mixers[8] = {0};
static int g_seen_mixer_count = 0;
static int (*real_NvMediaVideoMixerRenderSurface)(void*, void*, unsigned, void*, unsigned, unsigned, unsigned) = NULL;

int NvMediaVideoMixerRenderSurface(void* mixer, void* outSurface, unsigned bgColor,
                                   void* primaryVideoDesc, unsigned alpha,
                                   unsigned depth, unsigned releaseList) {
    if (!real_NvMediaVideoMixerRenderSurface)
        real_NvMediaVideoMixerRenderSurface = (int(*)(void*,void*,unsigned,void*,unsigned,unsigned,unsigned))
            dlsym(RTLD_NEXT, "NvMediaVideoMixerRenderSurface");

    /* Log every UNIQUE mixer pointer we see (up to 8) so we can tell whether
     * gal is actually rendering. Our own mixer (g_cl.mixer) is one expected
     * source — anything else is gal's. */
    if (mixer) {
        int seen = 0, i;
        for (i = 0; i < g_seen_mixer_count; i++) if (g_seen_mixers[i] == mixer) { seen = 1; break; }
        if (!seen && g_seen_mixer_count < 8) {
            g_seen_mixers[g_seen_mixer_count++] = mixer;
            void* dev = *(void**)((char*)mixer + 0x2C);
            int is_ours = (mixer == g_cl.mixer);
            log_line("[gal_cluster] MixerRenderSurface mixer=%p dev=%p %s\n",
                     mixer, dev, is_ours ? "(OURS — skipped)" : "(NOT OURS — gal's)");
            /* Only capture from a mixer that is NOT ours */
            if (!is_ours && !g_gal_nvdevice && dev && (uint32_t)dev > 0x10000) {
                g_gal_nvdevice = dev;
                log_line("[gal_cluster] captured gal NvMediaDevice=%p from mixer=%p\n", dev, mixer);
            }
        }
    }

    if (!real_NvMediaVideoMixerRenderSurface) return 1;
    return real_NvMediaVideoMixerRenderSurface(mixer, outSurface, bgColor,
                                               primaryVideoDesc, alpha, depth, releaseList);
}

static int cl_setup_screen(int w, int h) {
    int rc;
    if (g_gal_ctx) {
        g_cl.scr_ctx = g_gal_ctx;
        log_line("[gal_cluster] using gal's context=%p\n", g_gal_ctx);
    } else {
        rc = screen_create_context(&g_cl.scr_ctx, 0);
        if (rc) { log_line("[gal_cluster] create_context failed\n"); return -1; }
    }

    /* DINT-style post window (FORMAT=13 YV12, USAGE=0x480 — matches gal) */
    typedef int (*fn_cwt)(screen_window_t*, screen_context_t, int);
    fn_cwt cwt = (fn_cwt)dlsym(RTLD_DEFAULT, "screen_create_window_type");
    if (cwt) rc = cwt(&g_cl.post_win, g_cl.scr_ctx, 0);
    else     rc = screen_create_window(&g_cl.post_win, g_cl.scr_ctx);
    if (rc) { log_line("[gal_cluster] create post_win failed\n"); return -1; }

    int bh = ((h * 3 / 2) + 63) & ~63;  /* padded height for YV12 layout */
    int pbsize[2] = { w, bh };          /* BUFFER_SIZE — storage height */
    int psize[2]  = { w, h };           /* SIZE — visible (Y plane) height */
    /* USAGE = 0x480 — exact match for gal HMI window. Verified empirically
     * (run 20260425_111626): readback rc=0 every frame for 4700+ frames.
     * Earlier 0x486 attempt added READ|WRITE bits to enable direct CPU mmap;
     * that gave NULL/empty UV plane because Screen handed us a CPU-mapped
     * allocation distinct from the IOMMU memory NvMedia decoder writes to. */
    int pipeline = 1, pfmt = 13, pusage = 0x480;
    screen_set_window_property_iv(g_cl.post_win, 0x33, &pipeline);
    screen_set_window_property_iv(g_cl.post_win, 0x28, pbsize);   /* BUFFER_SIZE */
    screen_set_window_property_iv(g_cl.post_win, 0x05, psize);    /* SIZE (visible) */
    screen_set_window_property_iv(g_cl.post_win, 0x0e, &pfmt);
    screen_set_window_property_iv(g_cl.post_win, 0x30, &pusage);

    /* DO NOT set CLASS="media" or ID_STRING="33" here — tested run 20260425_201606,
     * both rc=0 but output looked WORSE. ID_STRING="33" makes post_win a VISIBLE
     * layer on the cluster displayable, conflicting with cluster_mirror's own
     * output on displayable 33. We need post_win to remain off-screen (readback
     * source only). The routing-property hypothesis was wrong. */

    /* Set a unique ID_STRING so cluster_mirror (DM_CTX) can find this window
     * via window enumeration. Using non-numeric string avoids displayable
     * collision (don't want this auto-attached to displayable 33 etc). */
    typedef int (*fn_set_cv)(screen_window_t, int, int, const void*);
    fn_set_cv set_cv = (fn_set_cv)dlsym(RTLD_DEFAULT, "screen_set_window_property_cv");
    if (set_cv) {
        const char* idstr = "AA_CLUSTER_STAGING";
        int idlen = (int)strlen(idstr);
        int rc_id = set_cv(g_cl.post_win, 0x14 /* SCREEN_PROPERTY_ID_STRING */,
                           idlen, idstr);
        log_line("[gal_cluster] post_win ID_STRING=\"%s\" rc=%d\n", idstr, rc_id);
    }

    /* Register window with WindowManager (libdisplayinit.so:1391). Same magic name. */
    typedef int (*fn_mw)(screen_window_t, const char*);
    fn_mw mw = (fn_mw)dlsym(RTLD_DEFAULT, "screen_manage_window");
    if (mw) {
        int mrc = mw(g_cl.post_win, "How are you gentlemen?");
        log_line("[gal_cluster] screen_manage_window rc=%d\n", mrc);
    } else {
        log_line("[gal_cluster] screen_manage_window NOT FOUND\n");
    }

    rc = screen_create_window_buffers(g_cl.post_win, CL_MAX_SURFACES);
    if (rc) { log_line("[gal_cluster] create buffers failed\n"); return -1; }

    /* Log buffer format/stride */
    screen_get_window_property_pv(g_cl.post_win, SCREEN_PROPERTY_RENDER_BUFFERS,
                                  (void**)g_cl.post_bufs);
    {
        int bfmt=0, busage=0, bstride=0;
        screen_get_buffer_property_iv(g_cl.post_bufs[0], SCREEN_PROPERTY_FORMAT, &bfmt);
        screen_get_buffer_property_iv(g_cl.post_bufs[0], SCREEN_PROPERTY_USAGE, &busage);
        screen_get_buffer_property_iv(g_cl.post_bufs[0], SCREEN_PROPERTY_STRIDE, &bstride);
        log_line("[gal_cluster] buf: fmt=%d usage=0x%x stride=%d  (%d post_bufs total)\n",
                 bfmt, busage, bstride, CL_MAX_SURFACES);
    }

    /* Siblings — one per post_buf so the H.264 DPB has enough headroom. */
    typedef int (*fn_sib)(void*,void*,void**);
    if (g_nv.SiblingCreate && g_cl.device) {
        int i;
        for (i = 0; i < CL_MAX_SURFACES; i++) {
            ((fn_sib)g_nv.SiblingCreate)(g_cl.device, g_cl.post_bufs[i], &g_cl.siblings[i]);
            log_line("[gal_cluster] sibling[%d]=%p\n", i, g_cl.siblings[i]);
        }
    }

    /* Open /cluster_shm FIRST — we need its mmap'd pointer to back the read
     * pixmap so screen_read_window writes BGRA directly into SHM (no memcpy). */
    int sfd = shm_open(CL_SHM_NAME, O_RDWR | O_CREAT, 0666);
    if (sfd >= 0) {
        ftruncate(sfd, sizeof(cluster_decoded_shm_t));
        g_cl.shm = (cluster_decoded_shm_t*)mmap(NULL, sizeof(cluster_decoded_shm_t),
            PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);
        close(sfd);
        if (g_cl.shm == MAP_FAILED) g_cl.shm = NULL;
    }
    if (!g_cl.shm) {
        log_line("[gal_cluster] /cluster_shm mmap failed — cannot continue\n");
        return -1;
    }
    g_cl.shm->magic = CL_SHM_MAGIC;
    g_cl.shm->version = 2;
    g_cl.shm->width = w;
    g_cl.shm->height = h;
    g_cl.shm->stride = w * 4;
    g_cl.shm->format = 8;            /* SCREEN_FORMAT_RGBA8888 (BGRA bytes) */
    g_cl.shm->sequence = 0;
    g_cl.shm->current_slot = 0;      /* single-slot for now (slot 0) */
    g_cl.shm->frame_ready = 0;

    /* User-pointer buffer path is dead on this stack:
     *   - screen_read_window with user-pointer dest returns -1 (rejects id=-1
     *     buffers via internal helper).
     *   - screen_blit with user-pointer dest returns 0 but writes NOTHING.
     *     Confirmed empirically run 20260427_195257: dump is all zeros despite
     *     screen_blit success.
     * Compositor only writes to buffers it allocated itself.
     *
     * Reverted to the screen_read_window + memcpy path. Slow (~46 ms memcpy
     * dominates) but works. The bandwidth pressure on gal HMI remains the
     * known issue; a real fix needs a different bridge mechanism. */
    rc = screen_create_context(&g_cl.read_ctx, 0 /* SCREEN_APPLICATION_CONTEXT */);
    if (rc) {
        log_line("[gal_cluster] create read_ctx failed errno=%d, falling back to scr_ctx\n", errno);
        g_cl.read_ctx = g_cl.scr_ctx;
    } else {
        log_line("[gal_cluster] read_ctx=%p (separate APPLICATION_CONTEXT)\n", g_cl.read_ctx);
    }
    rc = screen_create_pixmap(&g_cl.read_pix, g_cl.read_ctx);
    if (rc) { log_line("[gal_cluster] create read_pix failed\n"); return -1; }
    int rfmt = SCREEN_FORMAT_RGBX8888;
    /* SCREEN_USAGE_NATIVE (0x08) is documented as required for
     * screen_read_window destinations (screen.h:4431, 6298). Adding it may also
     * push the kernel to allocate physically-addressable memory, which would
     * make SCREEN_PROPERTY_PHYSICAL_ADDRESS return non-zero (current value: 0
     * with READ|WRITE only). If phys becomes non-zero, cluster_mirror's
     * MAP_PHYS probe should succeed. */
    int rusage = SCREEN_USAGE_READ | SCREEN_USAGE_WRITE | SCREEN_USAGE_NATIVE;
    int rpsize[2] = { w, h };
    screen_set_pixmap_property_iv(g_cl.read_pix, SCREEN_PROPERTY_FORMAT, &rfmt);
    screen_set_pixmap_property_iv(g_cl.read_pix, SCREEN_PROPERTY_USAGE, &rusage);
    screen_set_pixmap_property_iv(g_cl.read_pix, SCREEN_PROPERTY_BUFFER_SIZE, rpsize);
    screen_create_pixmap_buffer(g_cl.read_pix);
    screen_get_pixmap_property_pv(g_cl.read_pix, SCREEN_PROPERTY_RENDER_BUFFERS,
                                  (void**)&g_cl.read_buf);
    screen_get_buffer_property_pv(g_cl.read_buf, SCREEN_PROPERTY_POINTER, &g_cl.read_ptr);
    screen_get_buffer_property_iv(g_cl.read_buf, SCREEN_PROPERTY_STRIDE, &g_cl.read_stride);
    log_line("[gal_cluster] read_pix RGBA: ptr=%p stride=%d (in read_ctx=%p) — memcpy bridge\n",
             g_cl.read_ptr, g_cl.read_stride, g_cl.read_ctx);


    g_cl.ready = 1;
    log_line("[gal_cluster] screen setup complete %dx%d\n", w, h);
    return 0;
}

static void* g_router_ptr = NULL;  /* captured MessageRouter* */

static void* cl_find_gal_device(void) {
    /* Traverse: router → endpoint[svc1] → renderer → device
     * router + (1+0x40)*4 = router + 0x104 → endpoint ptr
     * endpoint has renderer reference — need to find the offset.
     * renderer + 0xD0 = NvMedia device */
    if (!g_router_ptr) return NULL;
    uint32_t* router = (uint32_t*)g_router_ptr;
    uint32_t* endpoint = (uint32_t*)router[(1 + 0x40)]; /* svc 1 endpoint */
    if (!endpoint) { log_line("[gal_cluster] svc1 endpoint=NULL\n"); return NULL; }
    /* The endpoint object has the renderer at some offset. Try common offsets.
     * Log the endpoint to find the right one. */
    log_line("[gal_cluster] svc1 endpoint=%p [0]=%08x [1]=%08x [2]=%08x [3]=%08x [4]=%08x\n",
             endpoint, endpoint[0], endpoint[1], endpoint[2], endpoint[3], endpoint[4]);
    /* endpoint[2] is an object — scan ITS fields for the renderer (which has device at +0xD0) */
    uint32_t* obj = (uint32_t*)endpoint[2];
    if (obj && (uint32_t)obj > 0x10000) {
        log_line("[gal_cluster] obj=%p [0]=%08x [1]=%08x [2]=%08x [3]=%08x [4]=%08x [5]=%08x [6]=%08x [7]=%08x\n",
                 obj, obj[0], obj[1], obj[2], obj[3], obj[4], obj[5], obj[6], obj[7]);
        /* Scan obj fields for a pointer that has a valid device at +0xD0 */
        int i;
        for (i = 0; i <= 16; i++) {
            uint32_t* sub = (uint32_t*)obj[i];
            if (sub && (uint32_t)sub > 0x70000000 && (uint32_t)sub < 0x80000000) {
                void* dev = (void*)sub[0xD0/4];
                if (dev && (uint32_t)dev > 0x70000000 && (uint32_t)dev < 0x80000000) {
                    log_line("[gal_cluster] obj[%d]=%p → +0xD0=%p\n", i, sub, dev);
                }
            }
        }
    }
    return NULL;
}

static void cl_init_parser(void) {
    if (g_cl.parser || !g_nv.DeviceCreate || !g_nv.ParserCreate) return;

    /* Prefer gal's device (captured via NvMediaVideoMixerRenderSurface hook).
     * TVMR hardware is exclusive to whichever NvMediaDevice was created first;
     * creating a second via NvMediaDeviceCreate gives rc=7 on DecoderRender. */
    if (g_gal_nvdevice) {
        g_cl.device = g_gal_nvdevice;
        log_line("[gal_cluster] using gal's device=%p (from mixer hook)\n", g_gal_nvdevice);
    } else {
        void* gal_dev = cl_find_gal_device();
        if (gal_dev) {
            g_cl.device = gal_dev;
            log_line("[gal_cluster] using gal's device=%p (from router traversal)\n", gal_dev);
        } else {
            typedef void* (*fn_dev_create)(void);
            g_cl.device = ((fn_dev_create)g_nv.DeviceCreate)();
            if (!g_cl.device) { log_line("[gal_cluster] DeviceCreate failed\n"); return; }
            log_line("[gal_cluster] using own device=%p (FALLBACK — expect rc=7)\n", g_cl.device);
        }
    }

    static uint8_t fake_renderer[512] = {0};
    void** cbs = (void**)(fake_renderer + 0x28);
    cbs[0] = (void*)cl_begin_sequence;
    cbs[1] = (void*)cl_decode_picture;
    cbs[2] = (void*)cl_display_picture;
    cbs[3] = (void*)cl_stub;
    cbs[4] = (void*)cl_alloc_picture;
    cbs[5] = (void*)cl_release;
    cbs[6] = (void*)cl_addref;
    { int i; for (i = 7; i < 15; i++) cbs[i] = (void*)cl_stub; }
    *(void**)(fake_renderer + 0xD0) = g_cl.device;

    typedef void* (*fn_parser_create)(void*);
    typedef int (*fn_parser_set_attr)(void*,uint32_t,uint32_t,void*);
    /* Match gal::nvInitContext (gal.c:167784-167794):
     *   {callbacks, context, 0, 0x5a, codec=4(H.264), 0}
     * params[3]=0x5a is stored at parser+0x310 inside video_parser_create
     * (libnvparser.so @0x1b004). Not setting it means the parser isn't fully
     * configured and pd fields beyond word 17 stay zero. */
    uint32_t params[6] = {0};
    params[0] = (uint32_t)(fake_renderer + 0x28);  /* callbacks */
    params[1] = (uint32_t)fake_renderer;           /* context */
    params[3] = 0x5a;                               /* gal passes this */
    params[4] = 4;                                  /* H.264 */
    g_cl.parser = ((fn_parser_create)g_nv.ParserCreate)(params);
    if (!g_cl.parser) { log_line("[gal_cluster] ParserCreate failed\n"); return; }

    float fps = 30.0f;
    ((fn_parser_set_attr)g_nv.ParserSetAttr)(g_cl.parser, 9, 4, &fps);
    log_line("[gal_cluster] parser=%p device=%p\n", g_cl.parser, g_cl.device);
}

/* Worker thread: owns all NvMedia / TVMR state. Keeps thread-local hardware
 * context consistent (gal's mixer/decode also run on their own thread — running
 * from the AA protocol thread produced tvmr[2]=0 stale buffers and rc=1). */
static void* cl_worker_fn(void* arg) {
    (void)arg;
    log_line("[cl-worker] started tid=%d\n", (int)pthread_self());
    /* Init parser here, not on AA thread. */
    cl_init_parser();
    if (!g_cl.parser || !g_nv.ParserParse) {
        log_line("[cl-worker] init failed — exiting\n");
        return NULL;
    }
    typedef int (*fn_parser_parse)(void*,void*);
    fn_parser_parse fparse = (fn_parser_parse)g_nv.ParserParse;
    uint8_t scratch[64 * 1024];

    for (;;) {
        pthread_mutex_lock(&g_cl.ring_mx);
        while (g_cl.ring_rd == g_cl.ring_wr) {
            pthread_cond_wait(&g_cl.ring_cv, &g_cl.ring_mx);
        }
        /* Pull up to scratch size from the ring. Ring stores [u32 len][bytes]. */
        uint32_t rd = g_cl.ring_rd;
        uint32_t wr = g_cl.ring_wr;
        uint32_t avail = (wr >= rd) ? (wr - rd) : (g_cl.ring_size - rd + wr);
        if (avail < 4) { pthread_mutex_unlock(&g_cl.ring_mx); continue; }
        uint32_t plen;
        for (int i = 0; i < 4; i++) ((uint8_t*)&plen)[i] = g_cl.ring[(rd + i) % g_cl.ring_size];
        rd = (rd + 4) % g_cl.ring_size;
        if (plen == 0 || plen > sizeof(scratch) || plen > avail - 4) {
            /* corrupt or too big — drain */
            g_cl.ring_rd = wr;
            pthread_mutex_unlock(&g_cl.ring_mx);
            continue;
        }
        for (uint32_t i = 0; i < plen; i++) scratch[i] = g_cl.ring[(rd + i) % g_cl.ring_size];
        rd = (rd + plen) % g_cl.ring_size;
        g_cl.ring_rd = rd;
        pthread_mutex_unlock(&g_cl.ring_mx);

        uint32_t parse_data[9] = {0};
        parse_data[0] = (uint32_t)scratch;
        parse_data[1] = plen;
        fparse(g_cl.parser, parse_data);
    }
    return NULL;
}

static void cl_feed_h264(const uint8_t* data, int len) {
    if (len <= 0 || len > 512 * 1024) return;
    /* Skip the in-hook decode pipeline entirely unless explicitly enabled.
     * Default (GAL_CLUSTER_DECODE=0): worker thread never spawns, decoder is
     * never created, screen_post/read/memcpy never run, /dev/shmem/cluster_shm
     * is never written. Saves ~50 ms/frame of CPU+GPU+memcpy bandwidth on
     * gal0 when the standalone aa_cluster_decoder process is the consumer. */
    if (!g_cl.decode_in_hook) return;

    /* Lazy-start worker on first feed. */
    if (!g_cl.worker_started) {
        g_cl.ring = (uint8_t*)malloc(CL_RING_SIZE);
        if (!g_cl.ring) { log_line("[gal_cluster] ring alloc failed\n"); return; }
        g_cl.ring_size = CL_RING_SIZE;
        g_cl.ring_rd = g_cl.ring_wr = 0;
        pthread_mutex_init(&g_cl.ring_mx, NULL);
        pthread_cond_init(&g_cl.ring_cv, NULL);
        if (pthread_create(&g_cl.worker_tid, NULL, cl_worker_fn, NULL) != 0) {
            log_line("[gal_cluster] worker create failed\n");
            return;
        }
        g_cl.worker_started = 1;
        log_line("[gal_cluster] worker thread spawned\n");
    }

    /* Push [u32 len][bytes] onto ring. */
    pthread_mutex_lock(&g_cl.ring_mx);
    uint32_t rd = g_cl.ring_rd, wr = g_cl.ring_wr;
    uint32_t used = (wr >= rd) ? (wr - rd) : (g_cl.ring_size - rd + wr);
    uint32_t free_bytes = g_cl.ring_size - used - 1;
    if ((uint32_t)(len + 4) > free_bytes) {
        pthread_mutex_unlock(&g_cl.ring_mx);
        return; /* drop frame — ring full, worker lagging */
    }
    uint32_t plen = (uint32_t)len;
    for (int i = 0; i < 4; i++) g_cl.ring[(wr + i) % g_cl.ring_size] = ((uint8_t*)&plen)[i];
    wr = (wr + 4) % g_cl.ring_size;
    for (int i = 0; i < len; i++) g_cl.ring[(wr + i) % g_cl.ring_size] = data[i];
    wr = (wr + len) % g_cl.ring_size;
    g_cl.ring_wr = wr;
    pthread_cond_signal(&g_cl.ring_cv);
    pthread_mutex_unlock(&g_cl.ring_mx);
}

/* Protobuf field number constants (resolved at runtime via dlsym) */
static const int* g_image_field_number = NULL;
static const int* g_road_field_number = NULL;
static const int* g_turnside_field_number = NULL;
static const int* g_event_field_number = NULL;
static const int* g_turnangle_field_number = NULL;
static const int* g_turnnumber_field_number = NULL;
static const int* g_sd_services_field_number = NULL;
static const int* g_service_nav_field_number = NULL;
static const int* g_nav_type_field_number = NULL;
static const int* g_nav_imageopts_field_number = NULL;
static const int* g_nav_min_interval_field_number = NULL;
static const int* g_imgopt_width_field_number = NULL;
static const int* g_imgopt_height_field_number = NULL;
static const int* g_imgopt_depth_field_number = NULL;
static const int* g_navstatus_status_field_number = NULL;
static const int* g_navfocusreq_focus_field_number = NULL;

static int g_seq = 0;
static int g_frame_seq = 0;
/* g_log_all / g_log_details / g_log_sd / g_nav_hexdump declared earlier
 * (above cl_display_picture). Env init lives in hook_init. */
static int g_sd_seq = 0;
static int g_dump_every_n = 30;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t min_ms;
    uint32_t forced_type;
    int has_width;
    int has_height;
    int has_depth;
    int has_min_ms;
} NavImgConfig;

static NavImgConfig g_cfg;
static int log_fd = -1;

/*
 * ByteSize wrapper: protobuf 3.x uses ByteSizeLong() returning size_t,
 * protobuf 2.x uses ByteSize() returning int.  We try Long first.
 */
static int call_byte_size(const void* msg, ByteSizeFunc f) {
    if (!f || !msg) return -1;
    return f(msg);
}

/*
 * Resolve a ByteSize function: try ByteSizeLong first (proto3), then ByteSize (proto2).
 * Both are stored as ByteSizeFunc — ByteSizeLong returns size_t but on 32-bit ARM
 * the low word in r0 is the same as int, so the cast is safe for sizes < 2GB.
 */
static ByteSizeFunc resolve_bytesize(const char* long_sym, const char* short_sym) {
    void* p = dlsym(RTLD_DEFAULT, long_sym);
    if (p) return (ByteSizeFunc)p;
    p = dlsym(RTLD_DEFAULT, short_sym);
    return (ByteSizeFunc)p;
}

/* ------------------------------------------------------------------ */
/*  Logging                                                           */
/* ------------------------------------------------------------------ */

static void open_log(void) {
    if (log_fd >= 0) return;
    log_fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
}

static void log_line(const char* fmt, ...) {
    /* Master gate: when GAL_CLUSTER_LOG_ALL=0 (default) the hook produces NO
     * /tmp/gal_cluster.log file at all — early init banners, errors, decoder
     * state, all suppressed. Set GAL_CLUSTER_LOG_ALL=1 to enable. */
    if (!g_log_all) return;
    open_log();
    if (log_fd < 0) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) write(log_fd, buf, (size_t)n);
}

/* ------------------------------------------------------------------ */
/*  Symbol resolution                                                 */
/* ------------------------------------------------------------------ */

static void resolve_symbols(void) {
    if (!real_handle)
        real_handle = (HandleNextTurnFunc)dlsym(RTLD_NEXT,
            "_ZN24NavigationStatusEndpoint29handleNavigationNextTurnEventERK23NavigationNextTurnEvent");
    if (!real_add_discovery)
        real_add_discovery = (AddDiscoveryFunc)dlsym(RTLD_NEXT,
            "_ZN24NavigationStatusEndpoint16addDiscoveryInfoEP24ServiceDiscoveryResponse");
    if (!real_start)
        real_start = (StartFunc)dlsym(RTLD_NEXT,
            "_ZN24NavigationStatusEndpoint5startEv");
    if (!real_stop)
        real_stop = (StopFunc)dlsym(RTLD_NEXT,
            "_ZN24NavigationStatusEndpoint4stopEv");
    if (!real_handle_status)
        real_handle_status = (HandleNavStatusFunc)dlsym(RTLD_NEXT,
            "_ZN24NavigationStatusEndpoint22handleNavigationStatusERK16NavigationStatus");
    if (!real_handle_distance)
        real_handle_distance = (HandleNavDistanceFunc)dlsym(RTLD_NEXT,
            "_ZN24NavigationStatusEndpoint29handleNavigationDistanceEventERK31NavigationNextTurnDistanceEvent");
    if (!real_populate_sd)
        real_populate_sd = (PopulateSDRespFunc)dlsym(RTLD_NEXT,
            "_ZN13MessageRouter32populateServiceDiscoveryResponseEP24ServiceDiscoveryResponse");
    if (!real_byte_size)
        real_byte_size = resolve_bytesize(
            "_ZNK23NavigationNextTurnEvent12ByteSizeLongEv",
            "_ZNK23NavigationNextTurnEvent8ByteSizeEv");
    if (!real_serialize_to_array)
        real_serialize_to_array = (MessageLite_SerializeToArray)dlsym(RTLD_DEFAULT,
            "_ZNK6google8protobuf11MessageLite16SerializeToArrayEPvi");
    if (!real_parse_from_array)
        real_parse_from_array = (MessageLite_ParseFromArray)dlsym(RTLD_DEFAULT,
            "_ZN6google8protobuf11MessageLite14ParseFromArrayEPKvi");
    if (!real_navstatus_size)
        real_navstatus_size = resolve_bytesize(
            "_ZNK16NavigationStatus12ByteSizeLongEv",
            "_ZNK16NavigationStatus8ByteSizeEv");
    if (!real_navdist_size)
        real_navdist_size = resolve_bytesize(
            "_ZNK31NavigationNextTurnDistanceEvent12ByteSizeLongEv",
            "_ZNK31NavigationNextTurnDistanceEvent8ByteSizeEv");
    if (!real_sd_size)
        real_sd_size = resolve_bytesize(
            "_ZNK24ServiceDiscoveryResponse12ByteSizeLongEv",
            "_ZNK24ServiceDiscoveryResponse8ByteSizeEv");
    if (!real_navfocusreq_merge)
        real_navfocusreq_merge = (MergeFunc)dlsym(RTLD_NEXT,
            "_ZN27NavFocusRequestNotification27MergePartialFromCodedStreamEPN6google8protobuf2io16CodedInputStreamE");
    if (!real_navfocusreq_size)
        real_navfocusreq_size = resolve_bytesize(
            "_ZNK27NavFocusRequestNotification12ByteSizeLongEv",
            "_ZNK27NavFocusRequestNotification8ByteSizeEv");

    /* Protobuf field number constants — fall back to defaults if missing */
    if (!g_image_field_number)
        g_image_field_number = (const int*)dlsym(RTLD_DEFAULT,
            "_ZN23NavigationNextTurnEvent17kImageFieldNumberE");
    if (!g_road_field_number)
        g_road_field_number = (const int*)dlsym(RTLD_DEFAULT,
            "_ZN23NavigationNextTurnEvent16kRoadFieldNumberE");
    if (!g_turnside_field_number)
        g_turnside_field_number = (const int*)dlsym(RTLD_DEFAULT,
            "_ZN23NavigationNextTurnEvent20kTurnSideFieldNumberE");
    if (!g_event_field_number)
        g_event_field_number = (const int*)dlsym(RTLD_DEFAULT,
            "_ZN23NavigationNextTurnEvent17kEventFieldNumberE");
    if (!g_turnangle_field_number)
        g_turnangle_field_number = (const int*)dlsym(RTLD_DEFAULT,
            "_ZN23NavigationNextTurnEvent21kTurnAngleFieldNumberE");
    if (!g_turnnumber_field_number)
        g_turnnumber_field_number = (const int*)dlsym(RTLD_DEFAULT,
            "_ZN23NavigationNextTurnEvent22kTurnNumberFieldNumberE");
    if (!g_sd_services_field_number)
        g_sd_services_field_number = (const int*)dlsym(RTLD_DEFAULT,
            "_ZN24ServiceDiscoveryResponse20kServicesFieldNumberE");
    if (!g_service_nav_field_number)
        g_service_nav_field_number = (const int*)dlsym(RTLD_DEFAULT,
            "_ZN7Service35kNavigationStatusServiceFieldNumberE");
    if (!g_nav_type_field_number)
        g_nav_type_field_number = (const int*)dlsym(RTLD_DEFAULT,
            "_ZN23NavigationStatusService16kTypeFieldNumberE");
    if (!g_nav_imageopts_field_number)
        g_nav_imageopts_field_number = (const int*)dlsym(RTLD_DEFAULT,
            "_ZN23NavigationStatusService24kImageOptionsFieldNumberE");
    if (!g_nav_min_interval_field_number)
        g_nav_min_interval_field_number = (const int*)dlsym(RTLD_DEFAULT,
            "_ZN23NavigationStatusService29kMinimumIntervalMsFieldNumberE");
    if (!g_imgopt_width_field_number)
        g_imgopt_width_field_number = (const int*)dlsym(RTLD_DEFAULT,
            "_ZN36NavigationStatusService_ImageOptions17kWidthFieldNumberE");
    if (!g_imgopt_height_field_number)
        g_imgopt_height_field_number = (const int*)dlsym(RTLD_DEFAULT,
            "_ZN36NavigationStatusService_ImageOptions18kHeightFieldNumberE");
    if (!g_imgopt_depth_field_number)
        g_imgopt_depth_field_number = (const int*)dlsym(RTLD_DEFAULT,
            "_ZN36NavigationStatusService_ImageOptions27kColourDepthBitsFieldNumberE");
    if (!g_navstatus_status_field_number)
        g_navstatus_status_field_number = (const int*)dlsym(RTLD_DEFAULT,
            "_ZN16NavigationStatus18kStatusFieldNumberE");
    if (!g_navfocusreq_focus_field_number)
        g_navfocusreq_focus_field_number = (const int*)dlsym(RTLD_DEFAULT,
            "_ZN27NavFocusRequestNotification21kFocusTypeFieldNumberE");

    /* Serialization hooks: append cluster bytes during SD response serialization.
     * This avoids modifying the C++ object (which causes crashes). */
    if (!real_sd_serialize_cached)
        real_sd_serialize_cached = (SerializeWithCachedSizesFunc)dlsym(RTLD_NEXT,
            "_ZNK24ServiceDiscoveryResponse24SerializeWithCachedSizesEPN6google8protobuf2io17CodedOutputStreamE");
    if (!real_cos_write_raw)
        real_cos_write_raw = (COS_WriteRawFunc)dlsym(RTLD_DEFAULT,
            "_ZN6google8protobuf2io17CodedOutputStream8WriteRawEPKvi");
    if (!real_handle_channel_open)
        real_handle_channel_open = (HandleChannelOpenReqFunc)dlsym(RTLD_NEXT,
            "_ZN13MessageRouter20handleChannelOpenReqEhRK18ChannelOpenRequest");
    if (!real_SSL_write)
        real_SSL_write = (SSL_write_func)dlsym(RTLD_NEXT, "SSL_write");
}

/* ------------------------------------------------------------------ */
/*  Configuration                                                     */
/* ------------------------------------------------------------------ */

static int get_env_u32_opt(const char* name, uint32_t* out) {
    const char* v = getenv(name);
    if (!v || !v[0]) return 0;
    char* endp = NULL;
    unsigned long val = strtoul(v, &endp, 0);
    if (!endp || endp == v) return 0;
    *out = (uint32_t)val;
    return 1;
}

static uint32_t get_env_u32(const char* name, uint32_t def) {
    const char* v = getenv(name);
    if (!v || !v[0]) return def;
    char* endp = NULL;
    unsigned long val = strtoul(v, &endp, 0);
    if (!endp || endp == v) return def;
    return (uint32_t)val;
}

static void load_config(void) {
    /* Env var → hardcoded default. No size auto-detect here — phone ignores
     * our advertised resolution anyway. cluster_mirror does the detection
     * to size its own window. */
    /* DISABLED 2026-05-02: navimg patching is off (see pb_rewrite_nav_service).
     * These env vars no longer have an effect; values stay at struct defaults. */
    /* g_cfg.has_width  = get_env_u32_opt("AA_NAVIMG_WIDTH",  &g_cfg.width);  */
    /* g_cfg.has_height = get_env_u32_opt("AA_NAVIMG_HEIGHT", &g_cfg.height); */
    /* g_cfg.has_depth  = get_env_u32_opt("AA_NAVIMG_DEPTH",  &g_cfg.depth);  */
    /* g_cfg.has_min_ms = get_env_u32_opt("AA_NAVIMG_MIN_MS", &g_cfg.min_ms); */

    if (!g_cfg.has_width)  g_cfg.width = 800;
    if (!g_cfg.has_height) g_cfg.height = 480;
    if (!g_cfg.has_depth)  g_cfg.depth = 32;
    if (!g_cfg.has_min_ms) g_cfg.min_ms = 50;

    /* Type: 1=PROJECTED (phone sends images), 2=NATIVE (text only).
     * Default to 1 to keep receiving images with larger dimensions.
     * Override with AA_NAVIMG_TYPE=2 to test native mode. */
    /* DISABLED 2026-05-02: navimg patching is off. */
    /* g_cfg.forced_type = get_env_u32("AA_NAVIMG_TYPE", 1); */

    /* DISABLED 2026-05-02: nav-image dump is off. */
    /* g_dump_every_n = (int)get_env_u32("AA_NAVIMG_DUMP_EVERY", 30); */
    /* if (g_dump_every_n <= 0) g_dump_every_n = 1;                  */
}

/* ------------------------------------------------------------------ */
/*  Object field patching                                             */
/* ------------------------------------------------------------------ */


/* ------------------------------------------------------------------ */
/*  Image detection and dumping                                       */
/* ------------------------------------------------------------------ */

static const char* detect_ext(const uint8_t* data, size_t len) {
    static const uint8_t png_sig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (len >= sizeof(png_sig) && memcmp(data, png_sig, sizeof(png_sig)) == 0) return "png";
    if (len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) return "jpg";
    if (len >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0) return "webp";
    if (len >= 2 && data[0] == 'B' && data[1] == 'M') return "bmp";
    return "bin";
}

static void dump_image(const uint8_t* data, size_t len) {
    char path[256];
    const char* ext = detect_ext(data, len);
    time_t now = time(NULL);
    int seq = ++g_seq;
    int n = snprintf(path, sizeof(path), "%s/aa_navimg_%ld_%d.%s",
                     DUMP_DIR, (long)now, seq, ext);
    if (n <= 0 || (size_t)n >= sizeof(path)) return;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    write(fd, data, len);
    close(fd);
    log_line("[gal_cluster] dumped %zu bytes -> %s\n", len, path);
}

/* ------------------------------------------------------------------ */
/*  Protobuf wire format helpers                                      */
/* ------------------------------------------------------------------ */

static const uint8_t* pb_read_varint(const uint8_t* p, const uint8_t* end, uint64_t* out) {
    uint64_t v = 0;
    int shift = 0;
    while (p < end && shift < 64) {
        uint8_t b = *p++;
        v |= ((uint64_t)(b & 0x7F)) << shift;
        if ((b & 0x80) == 0) { *out = v; return p; }
        shift += 7;
    }
    return NULL;
}

typedef struct { uint8_t* data; size_t len; size_t cap; } pb_buf;

static int pb_buf_reserve(pb_buf* b, size_t extra) {
    if (b->len + extra <= b->cap) return 1;
    size_t new_cap = b->cap ? b->cap : 256;
    while (new_cap < b->len + extra) new_cap *= 2;
    uint8_t* n = (uint8_t*)realloc(b->data, new_cap);
    if (!n) return 0;
    b->data = n; b->cap = new_cap;
    return 1;
}

static int pb_buf_append(pb_buf* b, const void* data, size_t len) {
    if (!pb_buf_reserve(b, len)) return 0;
    memcpy(b->data + b->len, data, len);
    b->len += len;
    return 1;
}

static int pb_write_varint(pb_buf* b, uint64_t v) {
    uint8_t tmp[10];
    size_t n = 0;
    while (v >= 0x80) { tmp[n++] = (uint8_t)((v & 0x7F) | 0x80); v >>= 7; }
    tmp[n++] = (uint8_t)v;
    return pb_buf_append(b, tmp, n);
}

static int pb_write_key(pb_buf* b, int field, int wire) {
    return pb_write_varint(b, ((uint64_t)field << 3) | (uint64_t)wire);
}

static const uint8_t* pb_find_length_delim(const uint8_t* buf, size_t len,
                                           int field_no, size_t* out_len) {
    const uint8_t* p = buf;
    const uint8_t* end = buf + len;
    while (p < end) {
        uint64_t key = 0;
        p = pb_read_varint(p, end, &key);
        if (!p) return NULL;
        int wire = (int)(key & 0x7);
        int field = (int)(key >> 3);
        if (wire == 2) {
            uint64_t l = 0;
            p = pb_read_varint(p, end, &l);
            if (!p || p + l > end) return NULL;
            if (field == field_no) { *out_len = (size_t)l; return p; }
            p += l;
        } else if (wire == 0) {
            uint64_t tmp = 0;
            p = pb_read_varint(p, end, &tmp);
            if (!p) return NULL;
        } else if (wire == 1) {
            if (p + 8 > end) return NULL; p += 8;
        } else if (wire == 5) {
            if (p + 4 > end) return NULL; p += 4;
        } else { return NULL; }
    }
    return NULL;
}

static const uint8_t* pb_find_varint_field(const uint8_t* buf, size_t len,
                                           int field_no, uint64_t* out_val) {
    const uint8_t* p = buf;
    const uint8_t* end = buf + len;
    while (p < end) {
        uint64_t key = 0;
        p = pb_read_varint(p, end, &key);
        if (!p) return NULL;
        int wire = (int)(key & 0x7);
        int field = (int)(key >> 3);
        if (wire == 0) {
            uint64_t v = 0;
            p = pb_read_varint(p, end, &v);
            if (!p) return NULL;
            if (field == field_no) { *out_val = v; return p; }
        } else if (wire == 2) {
            uint64_t l = 0; p = pb_read_varint(p, end, &l);
            if (!p || p + l > end) return NULL; p += l;
        } else if (wire == 1) {
            if (p + 8 > end) return NULL; p += 8;
        } else if (wire == 5) {
            if (p + 4 > end) return NULL; p += 4;
        } else { return NULL; }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Protobuf rewriters                                                */
/* ------------------------------------------------------------------ */

/* Rewrite NavigationStatusService_ImageOptions: width, height, depth */
static int pb_rewrite_imageopts(const uint8_t* buf, size_t len, pb_buf* out, int* changed) {
    uint8_t dummy = 0;
    if (!buf && len == 0) { buf = &dummy; }
    else if (!buf) return 0;

    int img_w_field = g_imgopt_width_field_number ? *g_imgopt_width_field_number : 1;
    int img_h_field = g_imgopt_height_field_number ? *g_imgopt_height_field_number : 2;
    int img_d_field = g_imgopt_depth_field_number ? *g_imgopt_depth_field_number : 3;
    int saw_w = 0, saw_h = 0, saw_d = 0;

    const uint8_t* p = buf;
    const uint8_t* end = buf + len;
    while (p < end) {
        uint64_t key = 0;
        const uint8_t* q = pb_read_varint(p, end, &key);
        if (!q) return 0;
        int wire = (int)(key & 0x7);
        int field = (int)(key >> 3);
        p = q;

        if (wire == 0) {
            uint64_t v = 0;
            q = pb_read_varint(p, end, &v);
            if (!q) return 0;
            uint64_t out_v = v;
            if (field == img_w_field) { saw_w = 1; if (v != g_cfg.width) *changed = 1; out_v = g_cfg.width; }
            else if (field == img_h_field) { saw_h = 1; if (v != g_cfg.height) *changed = 1; out_v = g_cfg.height; }
            else if (field == img_d_field) { saw_d = 1; if (v != g_cfg.depth) *changed = 1; out_v = g_cfg.depth; }
            if (!pb_write_key(out, field, wire) || !pb_write_varint(out, out_v)) return 0;
            p = q;
        } else if (wire == 1) {
            if (p + 8 > end) return 0;
            if (!pb_write_key(out, field, wire) || !pb_buf_append(out, p, 8)) return 0;
            p += 8;
        } else if (wire == 5) {
            if (p + 4 > end) return 0;
            if (!pb_write_key(out, field, wire) || !pb_buf_append(out, p, 4)) return 0;
            p += 4;
        } else if (wire == 2) {
            uint64_t l = 0;
            q = pb_read_varint(p, end, &l);
            if (!q || q + l > end) return 0;
            if (!pb_write_key(out, field, wire) || !pb_write_varint(out, l) ||
                !pb_buf_append(out, q, (size_t)l)) return 0;
            p = q + l;
        } else { return 0; }
    }

    if (!saw_w) { *changed = 1; if (!pb_write_key(out, img_w_field, 0) || !pb_write_varint(out, g_cfg.width)) return 0; }
    if (!saw_h) { *changed = 1; if (!pb_write_key(out, img_h_field, 0) || !pb_write_varint(out, g_cfg.height)) return 0; }
    if (!saw_d) { *changed = 1; if (!pb_write_key(out, img_d_field, 0) || !pb_write_varint(out, g_cfg.depth)) return 0; }
    return 1;
}

/* Rewrite NavigationStatusService: type→2, min_interval, image_options */
static int pb_rewrite_nav_service(const uint8_t* buf, size_t len, pb_buf* out, int* changed) {
    /* DISABLED 2026-05-02: MH2P does not consume the turn-icon nav-image
     * stream we were patching. Pass through unchanged so the phone gets
     * whatever HMI originally advertised. Cluster MediaSinkService
     * injection is in queueOutgoing (separate path) — unaffected. */
    (void)changed;
    if (!pb_buf_append(out, buf, len)) return 0;
    return 1;
#if 0  /* original navimg patching kept for reference */
    int nav_type_field = g_nav_type_field_number ? *g_nav_type_field_number : 2;
    int nav_min_field = g_nav_min_interval_field_number ? *g_nav_min_interval_field_number : 1;
    int nav_imgopt_field = g_nav_imageopts_field_number ? *g_nav_imageopts_field_number : 3;
    int saw_type = 0, saw_min = 0, saw_imgopt = 0;

    const uint8_t* p = buf;
    const uint8_t* end = buf + len;
    while (p < end) {
        uint64_t key = 0;
        const uint8_t* q = pb_read_varint(p, end, &key);
        if (!q) return 0;
        int wire = (int)(key & 0x7);
        int field = (int)(key >> 3);
        p = q;

        if (wire == 0) {
            uint64_t v = 0;
            q = pb_read_varint(p, end, &v);
            if (!q) return 0;
            uint64_t out_v = v;
            if (field == nav_type_field) {
                saw_type = 1;
                /* Type=1 (PROJECTED) = phone sends images.
                 * Type=2 (NATIVE) = phone sends only text, no images.
                 * Keep type=1 to receive images, just change dimensions. */
                uint64_t forced_type = g_cfg.forced_type;
                if (v != forced_type) *changed = 1;
                out_v = forced_type;
            }
            else if (field == nav_min_field) { saw_min = 1; if (v != g_cfg.min_ms) *changed = 1; out_v = g_cfg.min_ms; }
            if (!pb_write_key(out, field, wire) || !pb_write_varint(out, out_v)) return 0;
            p = q;
        } else if (wire == 1) {
            if (p + 8 > end) return 0;
            if (!pb_write_key(out, field, wire) || !pb_buf_append(out, p, 8)) return 0;
            p += 8;
        } else if (wire == 5) {
            if (p + 4 > end) return 0;
            if (!pb_write_key(out, field, wire) || !pb_buf_append(out, p, 4)) return 0;
            p += 4;
        } else if (wire == 2) {
            uint64_t l = 0;
            q = pb_read_varint(p, end, &l);
            if (!q || q + l > end) return 0;
            if (field == nav_imgopt_field) {
                saw_imgopt = 1;
                pb_buf img = {0};
                int img_changed = 0;
                if (!pb_rewrite_imageopts(q, (size_t)l, &img, &img_changed)) { free(img.data); return 0; }
                if (img_changed) *changed = 1;
                if (!pb_write_key(out, field, wire) || !pb_write_varint(out, img.len) ||
                    !pb_buf_append(out, img.data, img.len)) { free(img.data); return 0; }
                free(img.data);
                p = q + l;
            } else {
                if (!pb_write_key(out, field, wire) || !pb_write_varint(out, l) ||
                    !pb_buf_append(out, q, (size_t)l)) return 0;
                p = q + l;
            }
        } else { return 0; }
    }

    if (!saw_type) { *changed = 1; if (!pb_write_key(out, nav_type_field, 0) || !pb_write_varint(out, g_cfg.forced_type)) return 0; }
    if (!saw_min) { *changed = 1; if (!pb_write_key(out, nav_min_field, 0) || !pb_write_varint(out, g_cfg.min_ms)) return 0; }
    if (!saw_imgopt) {
        pb_buf img = {0};
        int img_changed = 0;
        if (!pb_rewrite_imageopts(NULL, 0, &img, &img_changed)) { free(img.data); return 0; }
        if (!pb_write_key(out, nav_imgopt_field, 2) || !pb_write_varint(out, img.len) ||
            !pb_buf_append(out, img.data, img.len)) { free(img.data); return 0; }
        free(img.data);
        *changed = 1;
    }
    return 1;
#endif /* 0 — original navimg patching */
}

/* Rewrite Service: find NavigationStatusService sub-message and rewrite */
static int pb_rewrite_service(const uint8_t* buf, size_t len, pb_buf* out, int* changed) {
    int service_nav_field = g_service_nav_field_number ? *g_service_nav_field_number : 8;

    const uint8_t* p = buf;
    const uint8_t* end = buf + len;
    while (p < end) {
        uint64_t key = 0;
        const uint8_t* q = pb_read_varint(p, end, &key);
        if (!q) return 0;
        int wire = (int)(key & 0x7);
        int field = (int)(key >> 3);
        p = q;

        if (wire == 0) {
            uint64_t v = 0;
            q = pb_read_varint(p, end, &v);
            if (!q) return 0;
            if (!pb_write_key(out, field, wire) || !pb_write_varint(out, v)) return 0;
            p = q;
        } else if (wire == 1) {
            if (p + 8 > end) return 0;
            if (!pb_write_key(out, field, wire) || !pb_buf_append(out, p, 8)) return 0;
            p += 8;
        } else if (wire == 5) {
            if (p + 4 > end) return 0;
            if (!pb_write_key(out, field, wire) || !pb_buf_append(out, p, 4)) return 0;
            p += 4;
        } else if (wire == 2) {
            uint64_t l = 0;
            q = pb_read_varint(p, end, &l);
            if (!q || q + l > end) return 0;
            if (field == service_nav_field) {
                pb_buf nav = {0};
                int nav_changed = 0;
                if (!pb_rewrite_nav_service(q, (size_t)l, &nav, &nav_changed)) { free(nav.data); return 0; }
                if (nav_changed) *changed = 1;
                if (!pb_write_key(out, field, wire) || !pb_write_varint(out, nav.len) ||
                    !pb_buf_append(out, nav.data, nav.len)) { free(nav.data); return 0; }
                free(nav.data);
                p = q + l;
            } else {
                if (!pb_write_key(out, field, wire) || !pb_write_varint(out, l) ||
                    !pb_buf_append(out, q, (size_t)l)) return 0;
                p = q + l;
            }
        } else { return 0; }
    }
    return 1;
}

/* Rewrite entire ServiceDiscoveryResponse */
static int pb_rewrite_sdresp(const uint8_t* buf, size_t len,
                             uint8_t** out_buf, size_t* out_len, int* changed) {
    if (!buf || !out_buf || !out_len || !changed) return 0;
    int services_field = g_sd_services_field_number ? *g_sd_services_field_number : 1;

    pb_buf out = {0};
    const uint8_t* p = buf;
    const uint8_t* end = buf + len;
    while (p < end) {
        uint64_t key = 0;
        const uint8_t* q = pb_read_varint(p, end, &key);
        if (!q) { free(out.data); return 0; }
        int wire = (int)(key & 0x7);
        int field = (int)(key >> 3);
        p = q;

        if (wire == 0) {
            uint64_t v = 0;
            q = pb_read_varint(p, end, &v);
            if (!q || !pb_write_key(&out, field, wire) || !pb_write_varint(&out, v))
                { free(out.data); return 0; }
            p = q;
        } else if (wire == 1) {
            if (p + 8 > end || !pb_write_key(&out, field, wire) || !pb_buf_append(&out, p, 8))
                { free(out.data); return 0; }
            p += 8;
        } else if (wire == 5) {
            if (p + 4 > end || !pb_write_key(&out, field, wire) || !pb_buf_append(&out, p, 4))
                { free(out.data); return 0; }
            p += 4;
        } else if (wire == 2) {
            uint64_t l = 0;
            q = pb_read_varint(p, end, &l);
            if (!q || q + l > end) { free(out.data); return 0; }
            if (field == services_field) {
                pb_buf svc = {0};
                int svc_changed = 0;
                if (!pb_rewrite_service(q, (size_t)l, &svc, &svc_changed))
                    { free(svc.data); free(out.data); return 0; }
                if (svc_changed) *changed = 1;
                if (!pb_write_key(&out, field, wire) || !pb_write_varint(&out, svc.len) ||
                    !pb_buf_append(&out, svc.data, svc.len))
                    { free(svc.data); free(out.data); return 0; }
                free(svc.data);
            } else {
                if (!pb_write_key(&out, field, wire) || !pb_write_varint(&out, l) ||
                    !pb_buf_append(&out, q, (size_t)l))
                    { free(out.data); return 0; }
            }
            p = q + l;
        } else { free(out.data); return 0; }
    }

    *out_buf = out.data;
    *out_len = out.len;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  SD response logging helpers                                       */
/* ------------------------------------------------------------------ */

typedef void (*pb_len_cb)(const uint8_t* data, size_t len, void* ctx);
static void pb_for_each_len_field(const uint8_t* buf, size_t len, int field_no,
                                  pb_len_cb cb, void* ctx) {
    const uint8_t* p = buf;
    const uint8_t* end = buf + len;
    while (p < end) {
        uint64_t key = 0;
        p = pb_read_varint(p, end, &key);
        if (!p) return;
        int wire = (int)(key & 0x7);
        int field = (int)(key >> 3);
        if (wire == 2) {
            uint64_t l = 0; p = pb_read_varint(p, end, &l);
            if (!p || p + l > end) return;
            if (field == field_no && cb) cb(p, (size_t)l, ctx);
            p += l;
        } else if (wire == 0) {
            uint64_t v = 0; p = pb_read_varint(p, end, &v); if (!p) return;
        } else if (wire == 1) {
            if (p + 8 > end) return; p += 8;
        } else if (wire == 5) {
            if (p + 4 > end) return; p += 4;
        } else { return; }
    }
}

typedef struct {
    int nav_found;
    int service_nav_field, nav_type_field, nav_min_field, nav_imgopt_field;
    int img_w_field, img_h_field, img_d_field;
} SDNavCtx;

static void sd_service_cb(const uint8_t* svc, size_t svc_len, void* vctx) {
    SDNavCtx* c = (SDNavCtx*)vctx;
    size_t nav_len = 0;
    const uint8_t* nav = pb_find_length_delim(svc, svc_len, c->service_nav_field, &nav_len);
    if (!nav || nav_len == 0) return;
    c->nav_found = 1;
    uint64_t type = 0, min_ms = 0;
    pb_find_varint_field(nav, nav_len, c->nav_type_field, &type);
    pb_find_varint_field(nav, nav_len, c->nav_min_field, &min_ms);
    size_t imgopt_len = 0;
    const uint8_t* imgopt = pb_find_length_delim(nav, nav_len, c->nav_imgopt_field, &imgopt_len);
    uint64_t w = 0, h = 0, d = 0;
    if (imgopt && imgopt_len > 0) {
        pb_find_varint_field(imgopt, imgopt_len, c->img_w_field, &w);
        pb_find_varint_field(imgopt, imgopt_len, c->img_h_field, &h);
        pb_find_varint_field(imgopt, imgopt_len, c->img_d_field, &d);
    }
    log_line("[gal_cluster] sdresp nav: type=%llu min_ms=%llu imgopt=%s w=%llu h=%llu depth=%llu\n",
             (unsigned long long)type, (unsigned long long)min_ms,
             (imgopt && imgopt_len > 0) ? "yes" : "no",
             (unsigned long long)w, (unsigned long long)h, (unsigned long long)d);
}

static void log_sdresp_navstatus(const uint8_t* buf, size_t len) {
    if (!g_sd_services_field_number || !g_service_nav_field_number ||
        !g_nav_type_field_number || !g_nav_min_interval_field_number ||
        !g_nav_imageopts_field_number || !g_imgopt_width_field_number ||
        !g_imgopt_height_field_number || !g_imgopt_depth_field_number)
        return;

    SDNavCtx ctx = {0};
    ctx.service_nav_field = *g_service_nav_field_number;
    ctx.nav_type_field = *g_nav_type_field_number;
    ctx.nav_min_field = *g_nav_min_interval_field_number;
    ctx.nav_imgopt_field = *g_nav_imageopts_field_number;
    ctx.img_w_field = *g_imgopt_width_field_number;
    ctx.img_h_field = *g_imgopt_height_field_number;
    ctx.img_d_field = *g_imgopt_depth_field_number;
    pb_for_each_len_field(buf, len, *g_sd_services_field_number, sd_service_cb, &ctx);
    if (!ctx.nav_found) log_line("[gal_cluster] sdresp nav: not present\n");
}

static const char* navstatus_str(uint64_t v) {
    switch (v) {
        case 0: return "UNAVAILABLE";
        case 1: return "ACTIVE";
        case 2: return "INACTIVE";
        default: return "UNKNOWN";
    }
}

/* ------------------------------------------------------------------ */
/*  Cluster display injection into ServiceDiscoveryResponse           */
/* ------------------------------------------------------------------ */

/*
 * Build a complete Service protobuf entry for the cluster video display.
 * This gets appended to the ServiceDiscoveryResponse wire format.
 *
 * Wire format layout:
 *   ServiceDiscoveryResponse.services (field=1, wire=2) {
 *     Service.id (field=1, varint) = cluster_service_id
 *     Service.media_sink_service (field=3, wire=2) {
 *       MediaSinkService.available_type (field=1, varint) = 3 (VIDEO_H264_BP)
 *       MediaSinkService.video_configs (field=4, wire=2) {
 *         VideoConfiguration.codec_resolution (field=1, varint) = codec
 *         VideoConfiguration.frame_rate (field=2, varint) = fps
 *         VideoConfiguration.density (field=5, varint) = dpi
 *       }
 *       MediaSinkService.display_id (field=6, varint) = 1
 *       MediaSinkService.display_type (field=7, varint) = 1 (CLUSTER)
 *     }
 *   }
 *
 * Field numbers from MH2P + AA protocol spec:
 *   Service.id = 1, Service.media_sink_service = 3
 *   MediaSinkService: available_type=1, video_configs=4, display_id=6, display_type=7
 *   VideoConfiguration: codec_resolution=1, frame_rate=2, width_margin=3,
 *                       height_margin=4, density=5
 */
static int build_cluster_service(pb_buf* out, int service_id,
                                 int codec_res, int frame_rate, int dpi,
                                 int display_id, int display_type)
{
    /* Build VideoConfiguration — matching DHU format exactly.
     * DHU includes ALL fields: codec_res, frame_rate, margins, density,
     * decoder_depth, viewing_distance, pixel_aspect_ratio, real_density,
     * video_codec_type, ui_config. */
    pb_buf vc = {0};
    if (!pb_write_key(&vc, 1, 0) || !pb_write_varint(&vc, (uint64_t)codec_res)) goto fail_vc;  /* codec_resolution */
    if (!pb_write_key(&vc, 2, 0) || !pb_write_varint(&vc, (uint64_t)frame_rate)) goto fail_vc; /* frame_rate */
    if (!pb_write_key(&vc, 3, 0) || !pb_write_varint(&vc, 0)) goto fail_vc;                    /* width_margin=0 */
    if (!pb_write_key(&vc, 4, 0) || !pb_write_varint(&vc, 0)) goto fail_vc;                    /* height_margin=0 */
    if (!pb_write_key(&vc, 5, 0) || !pb_write_varint(&vc, (uint64_t)dpi)) goto fail_vc;        /* density */
    if (!pb_write_key(&vc, 6, 0) || !pb_write_varint(&vc, 0)) goto fail_vc;                    /* decoder_additional_depth=0 */
    if (!pb_write_key(&vc, 7, 0) || !pb_write_varint(&vc, 500)) goto fail_vc;                  /* viewing_distance=500 */
    if (!pb_write_key(&vc, 8, 0) || !pb_write_varint(&vc, 10000)) goto fail_vc;                /* pixel_aspect_ratio_e4=10000 */
    if (!pb_write_key(&vc, 9, 0) || !pb_write_varint(&vc, (uint64_t)dpi)) goto fail_vc;        /* real_density */
    if (!pb_write_key(&vc, 10, 0) || !pb_write_varint(&vc, 3)) goto fail_vc;                   /* video_codec_type=3 (H264_BP) */

    /* f11 (ui_config sub-message). DHU shape:
     *   {f1=margins(Insets,zero), f2=content_insets(empty),
     *    f3=stable_content_insets(empty or populated), f4=0}
     * stable_content_insets is the lever that tells the phone "keep critical
     * UI inside this rect" — turn arrows / distance text / ETA bar get laid
     * out within the inset rectangle, decorative map fills the full frame.
     * When all four GAL_CLUSTER_INSET_* env vars are unset, output is byte-
     * identical to the prior hardcoded blob. */
    {
        pb_buf uic = {0};
        /* f1 = margins — populated from env vars when set, else all-zero
         * (DHU shape). Margins is the older field; older receiver
         * implementations often honor margins where stable_content_insets
         * is a no-op. Try this first. */
        {
            pb_buf m = {0};
            int ok;
            if (g_inset_top || g_inset_left || g_inset_bottom || g_inset_right) {
                ok = pb_write_key(&m,1,0) && pb_write_varint(&m,(uint64_t)g_inset_top)    &&
                     pb_write_key(&m,2,0) && pb_write_varint(&m,(uint64_t)g_inset_left)   &&
                     pb_write_key(&m,3,0) && pb_write_varint(&m,(uint64_t)g_inset_bottom) &&
                     pb_write_key(&m,4,0) && pb_write_varint(&m,(uint64_t)g_inset_right);
            } else {
                ok = pb_write_key(&m,1,0) && pb_write_varint(&m,0) &&
                     pb_write_key(&m,2,0) && pb_write_varint(&m,0) &&
                     pb_write_key(&m,3,0) && pb_write_varint(&m,0) &&
                     pb_write_key(&m,4,0) && pb_write_varint(&m,0);
            }
            if (!ok) { free(m.data); free(uic.data); goto fail_vc; }
            if (!pb_write_key(&uic,1,2) || !pb_write_varint(&uic,m.len) ||
                !pb_buf_append(&uic,m.data,m.len)) {
                free(m.data); free(uic.data); goto fail_vc;
            }
            free(m.data);
        }
        /* f2 = content_insets (empty) */
        if (!pb_write_key(&uic,2,2) || !pb_write_varint(&uic,0)) {
            free(uic.data); goto fail_vc;
        }
        /* f3 = stable_content_insets (empty — values now go to f1 margins) */
        if (!pb_write_key(&uic,3,2) || !pb_write_varint(&uic,0)) {
            free(uic.data); goto fail_vc;
        }
        /* f4 = 0 */
        if (!pb_write_key(&uic,4,0) || !pb_write_varint(&uic,0)) {
            free(uic.data); goto fail_vc;
        }
        if (!pb_write_key(&vc,11,2) || !pb_write_varint(&vc,uic.len) ||
            !pb_buf_append(&vc,uic.data,uic.len)) {
            free(uic.data); goto fail_vc;
        }
        free(uic.data);
    }

    /* Build MediaSinkService */
    pb_buf mss = {0};
    /* available_type=3 (VIDEO_H264_BP) — MH2P's existing video service has this,
     * DHU omits it but includes video_codec_type=3 in VideoConfiguration instead.
     * Include both for maximum compatibility. */
    if (!pb_write_key(&mss, 1, 0) || !pb_write_varint(&mss, 3)) goto fail_mss;
    /* video_configs (repeated, field=4, wire=2) */
    if (!pb_write_key(&mss, 4, 2) || !pb_write_varint(&mss, vc.len) ||
        !pb_buf_append(&mss, vc.data, vc.len)) goto fail_mss;
    /* display_id (field=6, varint) */
    if (!pb_write_key(&mss, 6, 0) || !pb_write_varint(&mss, (uint64_t)display_id)) goto fail_mss;
    /* display_type (field=7, varint): 0=MAIN, 1=CLUSTER */
    if (!pb_write_key(&mss, 7, 0) || !pb_write_varint(&mss, (uint64_t)display_type)) goto fail_mss;

    /* Build Service */
    pb_buf svc = {0};
    /* Service.id (field=1, varint) */
    if (!pb_write_key(&svc, 1, 0) || !pb_write_varint(&svc, (uint64_t)service_id)) goto fail_svc;
    /* Service.media_sink_service (field=3, wire=2) */
    if (!pb_write_key(&svc, 3, 2) || !pb_write_varint(&svc, mss.len) ||
        !pb_buf_append(&svc, mss.data, mss.len)) goto fail_svc;

    /* Wrap in ServiceDiscoveryResponse.services (field=1, wire=2) */
    if (!pb_write_key(out, 1, 2) || !pb_write_varint(out, svc.len) ||
        !pb_buf_append(out, svc.data, svc.len)) goto fail_svc;

    free(vc.data); free(mss.data); free(svc.data);
    return 1;

fail_svc: free(svc.data);
fail_mss: free(mss.data);
fail_vc:  free(vc.data);
    return 0;
}

/* Also add display_type=0 (MAIN) to existing video MediaSinkService.
 * We do this by finding Service.media_sink_service sub-messages that have
 * video_configs (field=4) and appending display_type=0 field to them. */
static int pb_inject_display_type_main(const uint8_t* buf, size_t len,
                                       uint8_t** out_buf, size_t* out_len, int* changed)
{
    if (!buf || !out_buf || !out_len || !changed) return 0;

    pb_buf out = {0};
    const uint8_t* p = buf;
    const uint8_t* end = buf + len;

    while (p < end) {
        uint64_t key = 0;
        const uint8_t* q = pb_read_varint(p, end, &key);
        if (!q) { free(out.data); return 0; }
        int wire = (int)(key & 0x7);
        int field = (int)(key >> 3);
        p = q;

        if (wire == 2) {
            uint64_t l = 0;
            q = pb_read_varint(p, end, &l);
            if (!q || q + l > end) { free(out.data); return 0; }

            if (field == 1) {
                /* This is a Service entry. Check if it has media_sink_service
                 * with video_configs (indicating a video sink). */
                const uint8_t* svc_data = q;
                size_t svc_len = (size_t)l;

                /* Look for media_sink_service (field=3) inside this Service */
                size_t mss_len = 0;
                const uint8_t* mss = pb_find_length_delim(svc_data, svc_len, 3, &mss_len);

                int is_video_sink = 0;
                if (mss && mss_len > 0) {
                    /* Check if MediaSinkService has video_configs (field=4) */
                    size_t vc_len = 0;
                    if (pb_find_length_delim(mss, mss_len, 4, &vc_len))
                        is_video_sink = 1;
                }

                if (is_video_sink) {
                    /* Rebuild this Service with display_type=0 added to MediaSinkService */
                    pb_buf new_svc = {0};
                    const uint8_t* sp = svc_data;
                    const uint8_t* se = svc_data + svc_len;
                    while (sp < se) {
                        uint64_t sk = 0;
                        const uint8_t* sq = pb_read_varint(sp, se, &sk);
                        if (!sq) { free(new_svc.data); free(out.data); return 0; }
                        int sw = (int)(sk & 0x7);
                        int sf = (int)(sk >> 3);
                        sp = sq;

                        if (sw == 2) {
                            uint64_t sl = 0;
                            sq = pb_read_varint(sp, se, &sl);
                            if (!sq || sq + sl > se) { free(new_svc.data); free(out.data); return 0; }
                            if (sf == 3) {
                                /* This is media_sink_service — append display_type=0 */
                                pb_buf new_mss = {0};
                                pb_buf_append(&new_mss, sq, (size_t)sl);
                                /* display_type (field=7, varint) = 0 (MAIN) */
                                pb_write_key(&new_mss, 7, 0);
                                pb_write_varint(&new_mss, 0);
                                /* display_id (field=6, varint) = 0 */
                                pb_write_key(&new_mss, 6, 0);
                                pb_write_varint(&new_mss, 0);

                                pb_write_key(&new_svc, sf, sw);
                                pb_write_varint(&new_svc, new_mss.len);
                                pb_buf_append(&new_svc, new_mss.data, new_mss.len);
                                free(new_mss.data);
                                *changed = 1;
                            } else {
                                pb_write_key(&new_svc, sf, sw);
                                pb_write_varint(&new_svc, sl);
                                pb_buf_append(&new_svc, sq, (size_t)sl);
                            }
                            sp = sq + sl;
                        } else if (sw == 0) {
                            uint64_t sv = 0;
                            sq = pb_read_varint(sp, se, &sv);
                            if (!sq) { free(new_svc.data); free(out.data); return 0; }
                            pb_write_key(&new_svc, sf, sw);
                            pb_write_varint(&new_svc, sv);
                            sp = sq;
                        } else if (sw == 1) {
                            if (sp + 8 > se) { free(new_svc.data); free(out.data); return 0; }
                            pb_write_key(&new_svc, sf, sw);
                            pb_buf_append(&new_svc, sp, 8);
                            sp += 8;
                        } else if (sw == 5) {
                            if (sp + 4 > se) { free(new_svc.data); free(out.data); return 0; }
                            pb_write_key(&new_svc, sf, sw);
                            pb_buf_append(&new_svc, sp, 4);
                            sp += 4;
                        } else { free(new_svc.data); free(out.data); return 0; }
                    }

                    /* Write modified Service */
                    pb_write_key(&out, field, wire);
                    pb_write_varint(&out, new_svc.len);
                    pb_buf_append(&out, new_svc.data, new_svc.len);
                    free(new_svc.data);
                } else {
                    /* Not a video sink, copy as-is */
                    pb_write_key(&out, field, wire);
                    pb_write_varint(&out, l);
                    pb_buf_append(&out, q, (size_t)l);
                }
            } else {
                /* Non-service field, copy as-is */
                pb_write_key(&out, field, wire);
                pb_write_varint(&out, l);
                pb_buf_append(&out, q, (size_t)l);
            }
            p = q + l;
        } else if (wire == 0) {
            uint64_t v = 0;
            q = pb_read_varint(p, end, &v);
            if (!q) { free(out.data); return 0; }
            pb_write_key(&out, field, wire);
            pb_write_varint(&out, v);
            p = q;
        } else if (wire == 1) {
            if (p + 8 > end) { free(out.data); return 0; }
            pb_write_key(&out, field, wire);
            pb_buf_append(&out, p, 8);
            p += 8;
        } else if (wire == 5) {
            if (p + 4 > end) { free(out.data); return 0; }
            pb_write_key(&out, field, wire);
            pb_buf_append(&out, p, 4);
            p += 4;
        } else { free(out.data); return 0; }
    }

    /* Append the new cluster Service */
    if (!build_cluster_service(&out,
            20,     /* service_id — unused by gal */
            2,      /* codec_res: 2=1280x720. Capture from DHU (testing.ini
                     * HMI=1080p, cluster=720p) confirms phone accepts 720p
                     * on display_type=CLUSTER when advertised; earlier
                     * "rejected" observations were unrelated session-state
                     * bugs since fixed. Higher codec_res → phone allocates
                     * proportionally higher bitrate → Waze legible. */
            1,      /* frame_rate: 1=60fps (DHU advertises 60 on its cluster
                     * at 720p — phone sends at whatever rate it wants up
                     * to the cap, and at 60fps the bitrate budget is larger
                     * so each frame gets more bits even at lower actual fps). */
            160,    /* dpi */
            1,      /* display_id = 1 (cluster) */
            1       /* display_type = 1 (CLUSTER) */
        )) {
        free(out.data);
        return 0;
    }
    *changed = 1;

    *out_buf = out.data;
    *out_len = out.len;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Raw protobuf hex dump for nav messages — diagnostic               */
/* ------------------------------------------------------------------ */

/* Serializes a protobuf message and logs its bytes as hex. Caller passes
 * the message's ByteSize function pointer (each message type has its own).
 * Truncates to first 256 bytes so a giant nav image payload doesn't flood.
 * Off by default; enable with GAL_CLUSTER_NAV_HEXDUMP=1. */
static void nav_hexdump(const char* tag, const void* msg,
                        ByteSizeFunc size_fn) {
    if (!g_nav_hexdump || !msg || !size_fn || !real_serialize_to_array) return;
    int size = size_fn(msg);
    if (size <= 0 || size > MAX_SERIALIZE_BYTES) {
        log_line("[gal_cluster] %s: size=%d (out of range, no dump)\n", tag, size);
        return;
    }
    uint8_t* buf = (uint8_t*)malloc((size_t)size);
    if (!buf) return;
    if (!real_serialize_to_array(msg, buf, size)) { free(buf); return; }
    int n = size < 256 ? size : 256;
    /* hex line buffer: 256 bytes * 3 chars + slack = ~800 */
    char hex[800] = {0};
    int o = 0;
    for (int i = 0; i < n && o < (int)sizeof(hex) - 4; i++)
        o += snprintf(hex + o, sizeof(hex) - o, "%02x ", buf[i]);
    log_line("[gal_cluster] %s_hex bytes=%d (showing %d): %s%s\n",
             tag, size, n, hex, (n < size) ? "..." : "");
    free(buf);
}

/* ------------------------------------------------------------------ */
/*  Field extraction from NavigationNextTurnEvent                     */
/*                                                                    */
/*  Used to also dump the embedded image to disk; that path was       */
/*  disabled 2026-05-02 (MH2P doesn't consume nav images here, gal0   */
/*  still saves them to its own cache regardless). Function is now    */
/*  metadata-only.                                                    */
/* ------------------------------------------------------------------ */

static void extract_nav_event_fields(const void* event, int* out_has_image, size_t* out_img_len,
                                     int* out_total_size, char* out_road, size_t out_road_len,
                                     uint64_t* out_turnside, uint64_t* out_event,
                                     uint64_t* out_turnangle, uint64_t* out_turnnumber) {
    if (!real_byte_size || !real_serialize_to_array) return;
    int size = real_byte_size(event);
    if (out_total_size) *out_total_size = size;
    if (size <= 0 || size > MAX_SERIALIZE_BYTES) return;

    uint8_t* buf = (uint8_t*)malloc((size_t)size);
    if (!buf) return;
    if (!real_serialize_to_array(event, buf, size)) { free(buf); return; }

    int field_no = g_image_field_number ? *g_image_field_number : 4;
    size_t img_len = 0;
    const uint8_t* img = pb_find_length_delim(buf, (size_t)size, field_no, &img_len);
    if (img && img_len > 0) {
        if (out_has_image) *out_has_image = 1;
        if (out_img_len) *out_img_len = img_len;
        g_frame_seq++;
        /* DISABLED 2026-05-02: nav-image dump is off (MH2P doesn't consume them). */
        /* if (g_dump_every_n <= 1 || (g_frame_seq % g_dump_every_n) == 0)         */
        /*     dump_image(img, img_len);                                           */
    }

    if (out_road && out_road_len > 0) {
        int f = g_road_field_number ? *g_road_field_number : 1;
        size_t rl = 0;
        const uint8_t* r = pb_find_length_delim(buf, (size_t)size, f, &rl);
        if (r && rl > 0) { size_t n = rl < out_road_len-1 ? rl : out_road_len-1; memcpy(out_road, r, n); out_road[n] = '\0'; }
        else out_road[0] = '\0';
    }
    if (out_turnside) { int f = g_turnside_field_number ? *g_turnside_field_number : 2; pb_find_varint_field(buf, (size_t)size, f, out_turnside); }
    if (out_event) { int f = g_event_field_number ? *g_event_field_number : 3; pb_find_varint_field(buf, (size_t)size, f, out_event); }
    if (out_turnnumber) { int f = g_turnnumber_field_number ? *g_turnnumber_field_number : 5; pb_find_varint_field(buf, (size_t)size, f, out_turnnumber); }
    if (out_turnangle) { int f = g_turnangle_field_number ? *g_turnangle_field_number : 6; pb_find_varint_field(buf, (size_t)size, f, out_turnangle); }

    free(buf);
}

/* ------------------------------------------------------------------ */
/*  Constructor                                                       */
/* ------------------------------------------------------------------ */

__attribute__((constructor))
static void hook_init(void) {
    /* Read env BEFORE first log_line so the banner respects LOG_ALL.
     * With LOG_ALL=0 (default) the hook produces no /tmp/gal_cluster.log file. */
    g_log_all = (int)get_env_u32("GAL_CLUSTER_LOG_ALL", 0);
    g_log_details = (int)get_env_u32("GAL_CLUSTER_LOG_DETAILS", 0);
    g_log_sd = (int)get_env_u32("GAL_CLUSTER_SD_LOG", 0);
    g_nav_hexdump = (int)get_env_u32("GAL_CLUSTER_NAV_HEXDUMP", 0);
    log_line("[gal_cluster] MH2P aa_navimg_hook loaded (pid=%d)\n", (int)getpid());
    g_cl.debug = (int)get_env_u32("GAL_CLUSTER_DEBUG", 0);
    g_cl.decode_in_hook = (int)get_env_u32("GAL_CLUSTER_DECODE", 0);
    log_line("[gal_cluster] GAL_CLUSTER_DEBUG=%d (gates /tmp/cluster_decoded.raw + /tmp/cluster_stream.h264)\n",
             g_cl.debug);
    log_line("[gal_cluster] GAL_CLUSTER_DECODE=%d (=1 enables in-hook decode + /dev/shmem/cluster_shm fill; "
             "default 0 — standalone aa_cluster_decoder owns decode)\n", g_cl.decode_in_hook);

    /* stable_content_insets — see ui_config block in build_cluster_service. */
    g_inset_top    = (int)get_env_u32("GAL_CLUSTER_INSET_TOP",    0);
    g_inset_left   = (int)get_env_u32("GAL_CLUSTER_INSET_LEFT",   0);
    g_inset_bottom = (int)get_env_u32("GAL_CLUSTER_INSET_BOTTOM", 0);
    g_inset_right  = (int)get_env_u32("GAL_CLUSTER_INSET_RIGHT",  0);
    log_line("[gal_cluster] insets: top=%d left=%d bottom=%d right=%d (codec-frame px)\n",
             g_inset_top, g_inset_left, g_inset_bottom, g_inset_right);

    /* KEEPALIVE_MS — when non-zero, self-drive 0x8004 on ch=14 at this interval
     * while HMI is NATIVE. Independent of FAKE_MAIN_PROJECTED (which controls
     * the ch=1 rewrite + dual-channel self-drive). */
    g_keepalive_ms = (int)get_env_u32("GAL_CLUSTER_KEEPALIVE_MS", 0);
    log_line("[gal_cluster] GAL_CLUSTER_KEEPALIVE_MS=%d (0=disabled, else ch=14-only self-drive interval)\n", g_keepalive_ms);

    /* Per-channel outgoing byte rate logging — see g_rate_log comment. */
    g_rate_log = (int)get_env_u32("GAL_CLUSTER_RATE_LOG", 0);
    log_line("[gal_cluster] GAL_CLUSTER_RATE_LOG=%d (1=per-second per-channel byte rates)\n", g_rate_log);

    /* Mirror real focus state vs hardcode PROJECTED — see g_mirror_focus comment.
     * Default 1: tell phone the truth about cluster focus so phone properly
     * stops/resumes streaming on HMI cycle without sink/source state mismatch. */
    g_mirror_focus = (int)get_env_u32("GAL_CLUSTER_MIRROR_FOCUS", 1);
    log_line("[gal_cluster] GAL_CLUSTER_MIRROR_FOCUS=%d (0=hardcode PROJECTED, 1=mirror gal0 real state)\n", g_mirror_focus);

    /* Fake main video focus to phone — see g_fake_main_projected comment. */
    g_fake_main_projected = (int)get_env_u32("GAL_CLUSTER_FAKE_MAIN_PROJECTED", 0);
    log_line("[gal_cluster] GAL_CLUSTER_FAKE_MAIN_PROJECTED=%d (1=rewrite ch=1 0x8008 NATIVE->PROJECTED)\n", g_fake_main_projected);

    g_restart_on_return = (int)get_env_u32("GAL_CLUSTER_RESTART_ON_RETURN", 0);
    log_line("[gal_cluster] GAL_CLUSTER_RESTART_ON_RETURN=%d (1=re-fire setup on HMI return-to-AA)\n", g_restart_on_return);

    g_stopstart_on_return = (int)get_env_u32("GAL_CLUSTER_STOPSTART_ON_RETURN", 0);
    g_stopstart_delay_ms  = (int)get_env_u32("GAL_CLUSTER_STOPSTART_DELAY_MS", STOPSTART_DEFAULT_DELAY_MS);
    log_line("[gal_cluster] GAL_CLUSTER_STOPSTART_ON_RETURN=%d delay=%dms (1=fire Stop+Start on ch=14 at HMI return)\n",
             g_stopstart_on_return, g_stopstart_delay_ms);
    load_config();
    resolve_symbols();

    /* Log symbol resolution status for diagnostics */
    log_line("[gal_cluster] symbols: populate_sd=%p sd_serialize=%p cos_writeraw=%p sd_size=%p\n",
             (void*)real_populate_sd, (void*)real_sd_serialize_cached,
             (void*)real_cos_write_raw, (void*)real_sd_size);
    log_line("[gal_cluster] config: type=%u w=%u h=%u depth=%u min_ms=%u dump_every=%d\n",
             g_cfg.forced_type, g_cfg.width, g_cfg.height, g_cfg.depth, g_cfg.min_ms, g_dump_every_n);

    /* Pre-build cluster service protobuf bytes.
     * These get appended during SerializeWithCachedSizes. */
    {
        pb_buf cluster = {0};
        if (build_cluster_service(&cluster, 20, 1, 2, 160, 1, 1)) {
            g_cluster_service_bytes = cluster.data;
            g_cluster_service_len = cluster.len;
            log_line("[gal_cluster] cluster service built: %zu bytes (svc_id=20, 800x480@30, display_type=CLUSTER)\n",
                     g_cluster_service_len);
        } else {
            log_line("[gal_cluster] FAILED to build cluster service bytes\n");
        }
    }

    /* Initialize cluster H.264 SHM ring buffer */
    {
        int sfd = shm_open(CLUSTER_SHM_NAME, O_RDWR | O_CREAT, 0666);
        if (sfd >= 0) {
            ftruncate(sfd, sizeof(cluster_h264_shm_t));
            g_cluster_shm = (cluster_h264_shm_t*)mmap(NULL, sizeof(cluster_h264_shm_t),
                PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);
            close(sfd);
            if (g_cluster_shm == MAP_FAILED) {
                g_cluster_shm = NULL;
                log_line("[gal_cluster] cluster SHM: mmap failed\n");
            } else {
                g_cluster_shm->magic = CLUSTER_SHM_MAGIC;
                g_cluster_shm->write_pos = 0;
                g_cluster_shm->total_bytes = 0;
                g_cluster_shm->flags = 1;
                log_line("[gal_cluster] cluster SHM: ready (%zu bytes)\n", sizeof(cluster_h264_shm_t));
            }
        } else {
            log_line("[gal_cluster] cluster SHM: shm_open failed errno=%d\n", errno);
        }
    }

    /* Resolve NvMedia symbols (stored in g_nv for potential future use) */
    {
        g_nv.ParserCreate  = (fn_nv_generic)dlsym(RTLD_DEFAULT, "video_parser_create");
        g_nv.ParserSetAttr = (fn_nv_generic)dlsym(RTLD_DEFAULT, "video_parser_set_attribute");
        g_nv.ParserParse   = (fn_nv_generic)dlsym(RTLD_DEFAULT, "video_parser_parse");
        g_nv.ParserDestroy = (fn_nv_generic)dlsym(RTLD_DEFAULT, "video_parser_destroy");
        g_nv.DeviceCreate  = (fn_nv_generic)dlsym(RTLD_DEFAULT, "NvMediaDeviceCreate");
        g_nv.DecoderCreateEx=(fn_nv_generic)dlsym(RTLD_DEFAULT, "NvMediaVideoDecoderCreateEx");
        g_nv.DecoderDestroy= (fn_nv_generic)dlsym(RTLD_DEFAULT, "NvMediaVideoDecoderDestroy");
        g_nv.DecoderRender = (fn_nv_generic)dlsym(RTLD_DEFAULT, "NvMediaVideoDecoderRender");
        g_nv.DecoderSetAttr= (fn_nv_generic)dlsym(RTLD_DEFAULT, "NvMediaVideoDecoderSetAttributes");
        g_nv.MixerCreate   = (fn_nv_generic)dlsym(RTLD_DEFAULT, "NvMediaVideoMixerCreate");
        g_nv.MixerDestroy  = (fn_nv_generic)dlsym(RTLD_DEFAULT, "NvMediaVideoMixerDestroy");
        g_nv.MixerRender   = (fn_nv_generic)dlsym(RTLD_DEFAULT, "NvMediaVideoMixerRenderSurface");
        g_nv.SurfaceCreate = (fn_nv_generic)dlsym(RTLD_DEFAULT, "NvMediaVideoSurfaceCreate");
        g_nv.SurfaceDestroy= (fn_nv_generic)dlsym(RTLD_DEFAULT, "NvMediaVideoSurfaceDestroy");
        g_nv.SurfaceWait   = (fn_nv_generic)dlsym(RTLD_DEFAULT, "NvMediaVideoSurfaceWaitForCompletion");
        g_nv.SiblingCreate = (fn_nv_generic)dlsym(RTLD_DEFAULT, "NvxScreenCreateNvMediaVideoSurfaceSibling");
        g_nv.SurfaceLock   = (fn_nv_generic)dlsym(RTLD_DEFAULT, "NvMediaVideoSurfaceLock");
        g_nv.SurfaceUnlock = (fn_nv_generic)dlsym(RTLD_DEFAULT, "NvMediaVideoSurfaceUnlock");
        log_line("[gal_cluster] NvMedia: parser=%p parse=%p devCreate=%p decCreate=%p mixCreate=%p surfCreate=%p sibling=%p lock=%p unlock=%p\n",
                 (void*)g_nv.ParserCreate, (void*)g_nv.ParserParse,
                 (void*)g_nv.DeviceCreate, (void*)g_nv.DecoderCreateEx,
                 (void*)g_nv.MixerCreate, (void*)g_nv.SurfaceCreate,
                 (void*)g_nv.SiblingCreate,
                 (void*)g_nv.SurfaceLock, (void*)g_nv.SurfaceUnlock);
        log_line("[gal_cluster] NvMedia deferred to runtime\n");
    }
}

/* ================================================================== */
/*  HOOKED FUNCTIONS                                                  */
/* ================================================================== */

/*
 * handleNavigationNextTurnEvent — captures navigation images.
 * On MH2P, gal::CNavigationStatusImageImpl may override this via vtable,
 * so this hook might not fire. Images are still saved by gal to
 * NEXT_TURN_IMAGE_CACHE_BASE_PATH regardless.
 */
void _ZN24NavigationStatusEndpoint29handleNavigationNextTurnEventERK23NavigationNextTurnEvent(
    void* self, const void* event)
{
    resolve_symbols();
    if (event) {
        nav_hexdump("nav_event", event, real_byte_size);
        int has_img = 0;
        size_t img_len = 0;
        int total_size = 0;
        char road[128] = {0};
        uint64_t turnside = 0, ev = 0, turnangle = 0, turnnumber = 0;
        extract_nav_event_fields(event, &has_img, &img_len, &total_size,
                                 road, sizeof(road), &turnside, &ev, &turnangle, &turnnumber);
        if (g_log_all) {
            if (g_log_details)
                log_line("[gal_cluster] nav_event bytes=%d image=%s img_len=%zu road='%s' turnSide=%llu event=%llu turnAngleRaw=%llu turnNumber=%llu\n",
                         total_size, has_img ? "yes" : "no", img_len, road,
                         (unsigned long long)turnside, (unsigned long long)ev,
                         (unsigned long long)turnangle, (unsigned long long)turnnumber);
            else
                log_line("[gal_cluster] nav_event bytes=%d image=%s img_len=%zu\n",
                         total_size, has_img ? "yes" : "no", img_len);
        }
    }
    if (real_handle) real_handle(self, event);
}

int _ZN24NavigationStatusEndpoint5startEv(void* self) {
    resolve_symbols();
    log_line("[gal_cluster] nav_start\n");
    if (real_start) return real_start(self);
    return 0;
}

int _ZN24NavigationStatusEndpoint4stopEv(void* self) {
    resolve_symbols();
    log_line("[gal_cluster] nav_stop\n");
    if (real_stop) return real_stop(self);
    return 0;
}

void _ZN24NavigationStatusEndpoint22handleNavigationStatusERK16NavigationStatus(
    void* self, const void* status)
{
    resolve_symbols();
    nav_hexdump("nav_status", status, real_navstatus_size);
    int size = (status && real_navstatus_size) ? real_navstatus_size(status) : -1;
    uint64_t st = 0;
    int has_status = 0;
    if (status && size > 0 && size <= MAX_SERIALIZE_BYTES && real_serialize_to_array) {
        uint8_t* buf = (uint8_t*)malloc((size_t)size);
        if (buf) {
            if (real_serialize_to_array(status, buf, size)) {
                int f = g_navstatus_status_field_number ? *g_navstatus_status_field_number : 1;
                if (pb_find_varint_field(buf, (size_t)size, f, &st)) has_status = 1;
            }
            free(buf);
        }
    }
    if (has_status)
        log_line("[gal_cluster] nav_status bytes=%d status=%llu (%s)\n",
                 size, (unsigned long long)st, navstatus_str(st));
    else
        log_line("[gal_cluster] nav_status bytes=%d status=?\n", size);
    if (real_handle_status) real_handle_status(self, status);
}

void _ZN24NavigationStatusEndpoint29handleNavigationDistanceEventERK31NavigationNextTurnDistanceEvent(
    void* self, const void* dist)
{
    resolve_symbols();
    nav_hexdump("nav_distance", dist, real_navdist_size);
    int size = (dist && real_navdist_size) ? real_navdist_size(dist) : -1;
    log_line("[gal_cluster] nav_distance bytes=%d\n", size);
    if (real_handle_distance) real_handle_distance(self, dist);
}

/*
 * addDiscoveryInfo — patches NavigationStatusEndpoint object fields.
 * On MH2P, gal's vtable dispatch may bypass this. The protobuf rewrite
 * in populateServiceDiscoveryResponse is the authoritative path.
 */
void _ZN24NavigationStatusEndpoint16addDiscoveryInfoEP24ServiceDiscoveryResponse(
    void* self, void* resp)
{
    resolve_symbols();
    log_line("[gal_cluster] addDiscoveryInfo self=%p\n", self);
    if (real_add_discovery) real_add_discovery(self, resp);
}

/*
 * populateServiceDiscoveryResponse — THE CRITICAL HOOK.
 * Called via PLT (non-virtual), guaranteed to fire on MH2P.
 * Calls real function first, then rewrites the serialized protobuf
 * to force NavigationStatusService type=2 + ImageOptions.
 */
void _ZN13MessageRouter32populateServiceDiscoveryResponseEP24ServiceDiscoveryResponse(
    void* self, void* resp)
{
    resolve_symbols();

    /* Call the real function first — builds the entire response */
    if (real_populate_sd) real_populate_sd(self, resp);

    if (!resp || !real_sd_size || !real_serialize_to_array) {
        log_line("[gal_cluster] sdresp: missing funcs (sd_size=%p ser=%p)\n",
                 (void*)real_sd_size, (void*)real_serialize_to_array);
        return;
    }

    int size = real_sd_size(resp);
    if (size <= 0 || size > MAX_SERIALIZE_BYTES) {
        log_line("[gal_cluster] sdresp size=%d (skip)\n", size);
        return;
    }

    uint8_t* buf = (uint8_t*)malloc((size_t)size);
    if (!buf) return;

    if (!real_serialize_to_array(resp, buf, size)) {
        log_line("[gal_cluster] sdresp serialize failed\n");
        free(buf);
        return;
    }

    if (g_cluster_enabled < 0)
        g_cluster_enabled = (int)get_env_u32("GAL_CLUSTER_MERGE", 1);

    /* Build the COMPLETE modified SD response wire bytes.
     * We serialize the original, inject display_type=MAIN into existing video service,
     * append cluster service, and save the result. SerializeWithCachedSizes will
     * output these bytes instead of calling the real serialize. */
    if (g_cluster_enabled && real_sd_size && real_serialize_to_array) {
        int orig_size = real_sd_size(resp);
        if (orig_size > 0 && orig_size <= MAX_SERIALIZE_BYTES) {
            /* Save original SD response bytes for queueOutgoing matching */
            uint8_t* orig_buf = (uint8_t*)malloc((size_t)orig_size);
            if (orig_buf && real_serialize_to_array(resp, orig_buf, orig_size)) {
                if (g_original_sdresp) free(g_original_sdresp);
                g_original_sdresp = orig_buf;
                g_original_sdresp_len = (size_t)orig_size;


                /* HYBRID: MH2P original + DHU's exact cluster services + DHU fields.
                 * Uses DHU's verbatim cluster bytes (proven to work).
                 * GAL_CLUSTER_SVC=1 (default) injects cluster service; set to 0
                 * to disable injection for testing. */
                #include "dhu_sdresp_data.h"
                int add_cluster = (int)get_env_u32("GAL_CLUSTER_SVC", 1);
                size_t cluster_bytes = add_cluster ? DHU_CLUSTER_TOTAL : 0;
                size_t mod_len = (size_t)orig_size + cluster_bytes + DHU_EXTRA_FIELDS_SIZE;
                uint8_t* mod_buf = (uint8_t*)malloc(mod_len);
                if (mod_buf) {
                    size_t pos = 0;
                    memcpy(mod_buf + pos, orig_buf, (size_t)orig_size); pos += (size_t)orig_size;
                    /* GAL_CLUSTER_RES selects codec_resolution: 480→1, 720→2, 1080→3.
                     * Default 480p — keeps per-frame copy under realtime budget on Tegra K1. */
                    uint32_t res_req = get_env_u32("GAL_CLUSTER_RES", 720);
                    uint8_t codec_res = 1;
                    if (res_req == 720)  codec_res = 2;
                    else if (res_req == 1080) codec_res = 3;
                    else                      codec_res = 1;  /* 480 or anything else */
                    if (add_cluster) {
                        /* DHU's exact cluster video service (svc id=2) */
                        memcpy(mod_buf + pos, dhu_cluster_service, sizeof(dhu_cluster_service));
                        mod_buf[pos + 9] = codec_res;  /* patch codec_resolution in place */
                        pos += sizeof(dhu_cluster_service);
                        /* DHU's cluster InputSourceService (svc id=3) */
                        memcpy(mod_buf + pos, dhu_cluster_input, sizeof(dhu_cluster_input));
                        pos += sizeof(dhu_cluster_input);
                    }
                    memcpy(mod_buf + pos, dhu_field14, sizeof(dhu_field14)); pos += sizeof(dhu_field14);
                    memcpy(mod_buf + pos, dhu_field15, sizeof(dhu_field15)); pos += sizeof(dhu_field15);
                    memcpy(mod_buf + pos, dhu_field16, sizeof(dhu_field16)); pos += sizeof(dhu_field16);
                    memcpy(mod_buf + pos, dhu_field17, sizeof(dhu_field17)); pos += sizeof(dhu_field17);
                    if (g_modified_sdresp) free(g_modified_sdresp);
                    g_modified_sdresp = mod_buf;
                    g_modified_sdresp_len = mod_len;
                    log_line("[gal_cluster] sdresp: HYBRID %zu bytes (orig %d + DHU_cluster=%d[%zu] codec_res=%u(%up) + DHU_fields=%d)\n",
                             mod_len, orig_size, add_cluster, cluster_bytes,
                             codec_res, (codec_res==1?480:codec_res==2?720:1080),
                             (int)DHU_EXTRA_FIELDS_SIZE);
                }
            } else {
                free(orig_buf);
            }
        }
    }

    /* Dump and log the final response */
    if (g_log_sd) {
        char path[256];
        time_t now = time(NULL);
        int seq = ++g_sd_seq;
        int n = snprintf(path, sizeof(path), "%s/aa_sdresp_%ld_%d.bin",
                         DUMP_DIR, (long)now, seq);
        if (n > 0 && (size_t)n < sizeof(path)) {
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) { write(fd, buf, (size_t)size); close(fd); }
            log_line("[gal_cluster] sdresp dumped bytes=%d -> %s\n", size, path);
        }
        log_sdresp_navstatus(buf, (size_t)size);
    }
    free(buf);
}

/*
 * MessageRouter::queueOutgoing hook — THE KEY HOOK for cluster display injection.
 *
 * Intercepts marshalled AA messages BEFORE encryption.
 * Data format: [msg_type 2B BE][protobuf...]
 * SD response: msg_type = 6 → bytes 0x00 0x06 at offset 0.
 *
 * When we detect the SD response, we replace it with our modified version
 * (original + cluster service appended).
 *
 * No protobuf object modification. No ByteSizeLong patching. No crashes.
 */
static void (*real_queue_outgoing)(void* self, uint8_t ch, void* data, uint32_t len) = NULL;

/* Real MessageRouter::routeMessage(uchar channel, shared_ptr<IoBuffer> const&) —
 * the central incoming dispatcher in libautoreceiver. Hooked below so we can
 * see what phone is sending us on each channel during HMI transitions. */
static void (*real_routeMessage)(void* self, uint8_t channel, void* shared_ptr_ref) = NULL;

/* Per-endpoint route hooks. Same shape but include msg_type:
 *   void X::routeMessage(uchar channel, ushort msg_type, shared_ptr<IoBuffer> const&)
 * Hooking these gives us channel+type directly, no IoBuffer parsing needed. */
static void (*real_controller_routeMessage)(void*, uint8_t, uint16_t, void*) = NULL;
static void (*real_mediasinkbase_routeMessage)(void*, uint8_t, uint16_t, void*) = NULL;

/* Wrapper around real_queue_outgoing that records the byte count for the
 * rate log. Use this instead of real_queue_outgoing() everywhere so our
 * own injections (mirror, keep-alive, on-open) show up in rate stats. */
static void hook_queue_outgoing(void* self, uint8_t ch, void* data, uint32_t len);

/* Wall-clock ms (monotonic) for rate-log timestamps. */
static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

/* Accumulate bytes into per-channel counter; emit a line when 1s has passed.
 * Called from queueOutgoing. No-op when GAL_CLUSTER_RATE_LOG is 0. */
/* Per-(ch, msg_type) accumulator. Linear scan (~30 unique pairs typical). */
static void rate_log_record_type(uint8_t ch, uint16_t msg_type) {
    if (!g_rate_log) return;
    for (int i = 0; i < g_rate_types_n; i++) {
        if (g_rate_types[i].ch == ch && g_rate_types[i].type == msg_type) {
            g_rate_types[i].count++;
            return;
        }
    }
    if (g_rate_types_n < RATE_LOG_TYPES_MAX) {
        g_rate_types[g_rate_types_n].ch = ch;
        g_rate_types[g_rate_types_n].type = msg_type;
        g_rate_types[g_rate_types_n].count = 1;
        g_rate_types_n++;
    }
}

/* Record incoming bytes (called from dummy_cluster_route). Does NOT emit;
 * the periodic emit happens from rate_log_record on the outgoing path. */
static void rate_log_record_in(uint8_t ch, uint32_t len) {
    if (!g_rate_log) return;
    if (ch < RATE_LOG_NUM_CHANNELS) g_rate_in_bytes[ch] += len;
}

static void rate_log_record(uint8_t ch, const void* data, uint32_t len) {
    if (!g_rate_log) return;
    if (ch < RATE_LOG_NUM_CHANNELS) g_rate_bytes[ch] += len;
    if (len >= 2 && data) {
        const uint8_t* b = (const uint8_t*)data;
        uint16_t msg_type = ((uint16_t)b[0] << 8) | b[1];
        rate_log_record_type(ch, msg_type);
    }
    uint64_t t = now_ms();
    if (g_rate_session_start_ms == 0) g_rate_session_start_ms = t;
    if (g_rate_last_log_ms == 0) { g_rate_last_log_ms = t; return; }
    if (t - g_rate_last_log_ms < 1000) return;
    /* Emit one line per second covering both directions + per-type counts. */
    char line[2048];
    int o = 0;
    o += snprintf(line + o, sizeof(line) - o, "[gal_cluster] rate t=%llus out:",
                  (unsigned long long)((t - g_rate_session_start_ms) / 1000));
    for (int i = 0; i < RATE_LOG_NUM_CHANNELS; i++) {
        if (g_rate_bytes[i] == 0) continue;
        o += snprintf(line + o, sizeof(line) - o, " ch%d=%llu",
                      i, (unsigned long long)g_rate_bytes[i]);
        g_rate_bytes[i] = 0;
    }
    o += snprintf(line + o, sizeof(line) - o, "  in:");
    for (int i = 0; i < RATE_LOG_NUM_CHANNELS; i++) {
        if (g_rate_in_bytes[i] == 0) continue;
        o += snprintf(line + o, sizeof(line) - o, " ch%d=%llu",
                      i, (unsigned long long)g_rate_in_bytes[i]);
        g_rate_in_bytes[i] = 0;
    }
    o += snprintf(line + o, sizeof(line) - o, "  types:");
    for (int i = 0; i < g_rate_types_n && o < (int)sizeof(line) - 32; i++) {
        if (g_rate_types[i].count == 0) continue;
        o += snprintf(line + o, sizeof(line) - o, " ch%d.%04x=%u",
                      (int)g_rate_types[i].ch,
                      (unsigned)g_rate_types[i].type,
                      (unsigned)g_rate_types[i].count);
        g_rate_types[i].count = 0;
    }
    o += snprintf(line + o, sizeof(line) - o, "\n");
    log_line("%s", line);
    g_rate_last_log_ms = t;
}

/* Use this instead of real_queue_outgoing() for our own injections so they
 * are counted in the rate log alongside gal0's natural outgoing calls. */
static void hook_queue_outgoing(void* self, uint8_t ch, void* data, uint32_t len) {
    rate_log_record(ch, data, len);
    if (real_queue_outgoing) real_queue_outgoing(self, ch, data, len);
}

void _ZN13MessageRouter13queueOutgoingEhPvj(void* self, uint8_t ch, void* data, uint32_t len)
{
    if (!real_queue_outgoing)
        real_queue_outgoing = (void(*)(void*,uint8_t,void*,uint32_t))
            dlsym(RTLD_NEXT, "_ZN13MessageRouter13queueOutgoingEhPvj");

    if (g_cluster_enabled < 0)
        g_cluster_enabled = (int)get_env_u32("GAL_CLUSTER_MERGE", 1);

    /* Snoop ch=1 0x8008 to track REAL focus state (BEFORE any rewrite),
     * so g_hmi_focus_projected always reflects what gal0 thinks regardless
     * of FAKE_MAIN_PROJECTED. Used by the fake-main thread to know when to
     * fire (only when HMI internally NATIVE = gal0 silent). */
    if (ch == 1 && len >= 4 && data) {
        const uint8_t* b = (const uint8_t*)data;
        if (b[0] == 0x80 && b[1] == 0x08) {
            int new_state = (b[3] == 0x01) ? 1 : 0;
            int old_state = g_hmi_focus_projected;
            g_hmi_focus_projected = new_state;

            /* On NATIVE→PROJECTED transition (HMI returning to AA), if
             * GAL_CLUSTER_RESTART_ON_RETURN=1, fire a fresh setup sequence on
             * ch=14 to re-assert cluster's claim on bandwidth. Goal: prevent
             * phone from demoting cluster encoder when main video resumes. */
            if (g_restart_on_return && old_state == 0 && new_state == 1
                    && g_cluster_ch14_opened && real_queue_outgoing && g_router_ptr) {
                /* Wire-level cluster focus toggle on HMI return: send
                 * 0x8001 NATIVE immediately, then 0x8001 PROJECTED ~100ms
                 * later. Goal: simulate a brief cluster "pause" so phone
                 * does a fresh setup/encoder allocation for cluster instead
                 * of treating it as a low-priority continuation. */
                uint8_t* nat = (uint8_t*)malloc(6);
                if (nat) {
                    nat[0] = 0x80; nat[1] = 0x01;
                    nat[2] = 0x08; nat[3] = 0x02;  /* NATIVE */
                    nat[4] = 0x10; nat[5] = 0x01;  /* unsolicited */
                    hook_queue_outgoing(g_router_ptr, 14, nat, 6);
                }
                /* Schedule the PROJECTED follow-up after ~500ms (15 ticks
                 * at 33ms keep-alive interval). Long enough for phone to
                 * actually process the NATIVE state. */
                g_pending_projected_ticks = RESTART_PROJECTED_DELAY_TICKS;
                log_line("[gal_cluster] restart-on-return: fired 0x8001 NATIVE, pending PROJECTED in ~500ms\n");
            }

            /* STOPSTART_ON_RETURN: proper protocol-level restart on ch=14.
             * Stop (empty payload) immediately, Start (minimal proto) after
             * configured delay so phone processes Stop before Start. */
            if (g_stopstart_on_return && old_state == 0 && new_state == 1
                    && g_cluster_ch14_opened && real_queue_outgoing && g_router_ptr) {
                uint8_t* stop = (uint8_t*)malloc(2);
                if (stop) {
                    stop[0] = 0x80; stop[1] = 0x02;
                    hook_queue_outgoing(g_router_ptr, 14, stop, 2);
                }
                int delay_ms = (g_stopstart_delay_ms > 0) ? g_stopstart_delay_ms : STOPSTART_DEFAULT_DELAY_MS;
                int tick_ms = g_fake_main_projected ? FAKE_MAIN_INTERVAL_MS
                                                    : (g_keepalive_ms > 0 ? g_keepalive_ms : FAKE_MAIN_INTERVAL_MS);
                int ticks = (delay_ms + tick_ms - 1) / tick_ms;
                if (ticks < 1) ticks = 1;
                g_pending_start_ticks = ticks;
                log_line("[gal_cluster] stopstart-on-return: fired 0x8002 Stop, Start pending in ~%dms (%d ticks)\n",
                         delay_ms, ticks);
            }
        }
    }

    /* Fake main video focus: rewrite ch=1 0x8008 NATIVE→PROJECTED so phone
     * never sees main HMI lose focus. Mutates the data buffer in place; all
     * downstream handlers (rate log, mirror to ch=14) see the modified bytes,
     * which keeps cluster ch=14 0x8008 consistent with main. */
    if (g_fake_main_projected && ch == 1 && len >= 4 && data) {
        uint8_t* mut = (uint8_t*)data;
        if (mut[0] == 0x80 && mut[1] == 0x08 && mut[3] != 0x01) {
            if (g_cl.debug)
                log_line("[gal_cluster] queueOutgoing: rewrite ch=1 0x8008 state %d -> 1 (PROJECTED)\n", (int)mut[3]);
            mut[3] = 0x01;
        }
    }

    /* Rate log: count gal0's outgoing here. Our own injections call
     * hook_queue_outgoing() which also records. */
    rate_log_record(ch, data, len);

    if (g_cluster_enabled && g_original_sdresp && g_modified_sdresp && len > 2)
    {
        const uint8_t* buf = (const uint8_t*)data;

        /* Check: msg_type=6 (0x0006) at start, followed by our known SD response */
        if (buf[0] == 0x00 && buf[1] == 0x06 &&
            len >= 2 + (uint32_t)g_original_sdresp_len &&
            memcmp(buf + 2, g_original_sdresp, g_original_sdresp_len) == 0)
        {
            /* Build replacement: same msg_type + modified protobuf */
            uint32_t new_len = 2 + (uint32_t)g_modified_sdresp_len;
            uint8_t* new_buf = (uint8_t*)malloc(new_len);
            if (new_buf) {
                new_buf[0] = 0x00;
                new_buf[1] = 0x06;
                memcpy(new_buf + 2, g_modified_sdresp, g_modified_sdresp_len);

                log_line("[gal_cluster] queueOutgoing: INJECTED cluster! ch=%d orig=%u new=%u\n",
                         (int)ch, len, new_len);

                if (real_queue_outgoing)
                    real_queue_outgoing(self, ch, new_buf, new_len);
                /* DON'T free new_buf — queueOutgoing stores the pointer for
                 * async sending. Freeing here = use-after-free → Malloc Check.
                 * Leak 535 bytes once per connection — harmless. */

                /* Inject only once per connection */
                free(g_original_sdresp);
                g_original_sdresp = NULL;
                g_original_sdresp_len = 0;
                return;
            }
        }

        /* Log first few calls for diagnostics */
        {
            static int qo_log_count = 0;
            if (qo_log_count < 10) {
                log_line("[gal_cluster] queueOutgoing: ch=%d len=%u type=0x%02x%02x\n",
                         (int)ch, len, buf[0], buf[1]);
                qo_log_count++;
            }
        }
    }

    /* Forward to real queueOutgoing */
    if (real_queue_outgoing)
        real_queue_outgoing(self, ch, data, len);

    /* Cluster ACK is now sent synchronously from dummy_cluster_route so rapid
     * back-to-back cluster messages don't lose their ACKs. Deferred code removed. */

    /* Piggyback cluster messages when gal sends on main video channel (ch=1).
     * Mirror MEDIA_SETUP and VIDEO_FOCUS to cluster ch=14 (was 20 — moved to
     * 14 because phone may give better bitrate to lower svc_ids). */
    if (g_cluster_enabled && g_cluster_ch14_opened
            && len >= 2 && ch == 1 && real_queue_outgoing) {
        /* RACE FIX 2026-05-03: only mirror once ch=14 has been opened by phone.
         * Run 140440 S2 showed gal0 sometimes sends main-channel 0x8003 BEFORE
         * the phone opens ch=14 on us. Mirroring to an unopened ch=14 is silently
         * dropped/rejected; phone never sends codec config — "connected, didn't
         * run" symptom. Gate here protects ongoing 0x8002/0x8004 mirrors; the
         * one-shot 0x8003 is now driven from dummy_on_channel_opened so it always
         * fires *after* ch=14 opens. */
        const uint8_t* buf = (const uint8_t*)data;
        uint16_t msg_type = ((uint16_t)buf[0] << 8) | buf[1];

        /* 2026-05-02 NOTE: tried disabling this mirror (run 204922) on
         * suspicion it was the disconnect cause — confirmed it is LOAD-BEARING.
         * Without these mirrors the phone never starts the cluster video
         * stream past the initial mt=0x8000 sync (no codec config, no frames).
         * 0x8002/0x8003 are setup-phase messages the phone needs to see on
         * ch=14 to negotiate the cluster stream; 0x8004 may be both setup
         * and runtime status. Future work: inspect contents to mirror only
         * the setup-phase ones, not the high-rate status updates.
         *
         * 2026-05-02 (run 220501) tried dropping 0x8004 from this mirror set
         * based on DHU v4 evidence — REGRESSED START: 3/4 sessions in run
         * 214847 failed to advance past VIDEO_FOCUS, and 0/2 in run 220501.
         * 0x8004 IS load-bearing in the setup window; mirror restored. */
        if (msg_type == 0x8002 || msg_type == 0x8004) {
            /* Mirror 0x8002 (req-ack) and 0x8004 (per-frame MEDIA_START) from
             * main ch=1 to cluster ch=14. 0x8003 used to be mirrored here too,
             * but moved to dummy_on_channel_opened to avoid the race where
             * gal0 sends ch=1 0x8003 before phone opens ch=14 (run 140440 S2). */
            uint8_t* mirror = (uint8_t*)malloc(len);
            if (mirror) {
                memcpy(mirror, data, len);
                hook_queue_outgoing(self, 14, mirror, len);
                if (g_cl.debug)
                    log_line("[gal_cluster] queueOutgoing: mirrored type=0x%04x len=%u to cluster ch=14\n",
                             msg_type, len);
            }
            /* Diagnostic: dump first 30 ch=1 0x8004 payloads so we can see
             * if the bytes vary per frame (counter? timestamp?) or are
             * constant. Tells us whether keep-alive must replay variable
             * content or can use a fixed payload. Gated by GAL_CLUSTER_DEBUG. */
            if (g_cl.debug && msg_type == 0x8004) {
                static int s8004_dbg = 0;
                if (s8004_dbg < 30) {
                    s8004_dbg++;
                    char hex[200] = {0}; int o = 0;
                    int n = (int)len < 32 ? (int)len : 32;
                    for (int i = 0; i < n; i++)
                        o += snprintf(hex + o, sizeof(hex) - o, "%02x ", buf[i]);
                    log_line("[gal_cluster] 0x8004 #%d ch=1 len=%u bytes: %s\n",
                             s8004_dbg, len, hex);
                }
            }
        }
        else if (msg_type == 0x8008) {
            /* VIDEO_FOCUS on main → propagate to cluster ch=14.
             * Behavior depends on GAL_CLUSTER_MIRROR_FOCUS env var:
             *   0 (default): hardcode PROJECTED (legacy behavior).
             *   1:           mirror gal0's actual state byte. */
            uint8_t real_state = 0x01;
            if (len >= 4) {
                const uint8_t* src = (const uint8_t*)data;
                real_state = src[3];
                g_hmi_focus_projected = (real_state == 0x01) ? 1 : 0;
            }
            uint8_t* vf_buf = (uint8_t*)malloc(6);
            if (vf_buf) {
                uint8_t out_state = g_mirror_focus ? real_state : 0x01;
                vf_buf[0] = 0x80; vf_buf[1] = 0x08;
                vf_buf[2] = 0x08; vf_buf[3] = out_state;
                vf_buf[4] = 0x10; vf_buf[5] = 0x01;  /* unsolicited */
                hook_queue_outgoing(self, 14, vf_buf, 6);
                if (g_cl.debug)
                    log_line("[gal_cluster] queueOutgoing: VIDEO_FOCUS state=%d on cluster ch=14 (real=%d, mirror=%d)\n",
                             (int)out_state, (int)real_state, g_mirror_focus);
            }
        }
    }

    /* Per-message queueOutgoing trace — gated by GAL_CLUSTER_DEBUG. Two log
     * sites: short type/ch summary for first 20 messages, and per-(ch,type)
     * payload hexdump for first 64 unique pairs. Both off by default to
     * avoid log spam during video streaming (~25 mirror events/sec). */
    if (g_cl.debug) {
        {
            static int qo_diag = 0;
            if (qo_diag < 20 && len >= 2) {
                const uint8_t* b = (const uint8_t*)data;
                log_line("[gal_cluster] queueOutgoing: ch=%d len=%u type=0x%02x%02x\n",
                         (int)ch, len, b[0], b[1]);
                qo_diag++;
            }
        }
        if (len >= 2) {
            static struct { uint8_t ch; uint16_t type; } qo_seen[64];
            static int qo_seen_n = 0;
            const uint8_t* b = (const uint8_t*)data;
            uint16_t type = ((uint16_t)b[0] << 8) | b[1];
            int found = 0;
            for (int i = 0; i < qo_seen_n; i++) {
                if (qo_seen[i].ch == (uint8_t)ch && qo_seen[i].type == type) { found = 1; break; }
            }
            if (!found && qo_seen_n < 64) {
                qo_seen[qo_seen_n].ch = (uint8_t)ch;
                qo_seen[qo_seen_n].type = type;
                qo_seen_n++;
                int n = (int)len < 64 ? (int)len : 64;
                char hex[200] = {0}; int o = 0;
                for (int i = 0; i < n && o < (int)sizeof(hex) - 4; i++)
                    o += snprintf(hex + o, sizeof(hex) - o, "%02x ", b[i]);
                log_line("[gal_cluster] qo_first ch=%d type=0x%04x len=%u: %s\n",
                         (int)ch, type, len, hex);
            }
        }
    }
}

/*
 * handleChannelOpenReq hook — intercept cluster channel open requests.
 * ChannelOpenRequest: service_id at byte offset 0x18.
 * For our cluster service (id=20), return -5 (0xfffffffb) which tells
 * the caller to skip sending a response entirely. This prevents gal
 * from crashing on the unhandled channel.
 */
int _ZN13MessageRouter20handleChannelOpenReqEhRK18ChannelOpenRequest(
    void* self, uint8_t channel, const void* req)
{
    resolve_symbols();

    /* Extract service_id from ChannelOpenRequest (byte at offset 0x18) */
    uint8_t service_id = req ? *((const uint8_t*)req + 0x18) : 0xFF;

    /* Cluster channels: fake endpoint is installed in endpoint table before
     * channels are opened (in sendChannelOpenResp on first cluster channel).
     * handleChannelOpenReq will find it, call mayOpenChannel, setupMapping,
     * return 0. Normal flow then does registerChannel + notifyChannelOpened. */

    log_line("[gal_cluster] handleChannelOpenReq: svc_id=%d ch=%d -> forwarding to real handler\n",
             (int)service_id, (int)channel);

    if (real_handle_channel_open)
        return real_handle_channel_open(self, channel, req);
    return (int)0xfffffffc; /* -4: no endpoint */
}

/*
 * sendChannelOpenResp hook — accept cluster channel that gal would reject.
 * When gal sends status=-4 (no endpoint) for our cluster service,
 * we change it to status=0 (OK) so the phone can start sending video.
 */
static void (*real_send_channel_open_resp)(void* self, uint8_t ch, int status) = NULL;
static int (*real_register_channel)(void* chMgr, uint8_t ch, int8_t flag) = NULL;
static void (*real_setup_mapping)(void* router, uint8_t svc_id, uint8_t ch) = NULL;

/* Fake endpoint for cluster channels — silently consumes H.264 data.
 *
 * routeMessage vtable dispatch:
 *   vtable+0x10 (index 4): onChannelOpened(self, channel) — called by notifyChannelOpened
 *   vtable+0x14 (index 5): routeMessage(self, channel, type, data) — called for each message
 *   vtable+0x0c (index 3): mayOpenChannel(self) — called by handleChannelOpenReq
 */
static int dummy_may_open(void* self) {
    log_line("[gal_cluster] cluster endpoint: mayOpenChannel -> yes\n");
    return 1; /* allow */
}
/* Self-driven 0x8004 thread for FAKE_MAIN_PROJECTED mode.
 * When gal0 is silent on ch=1 (HMI is on NATIVE internally), this thread
 * fires the per-frame 0x8004 message on BOTH ch=1 and ch=14 at ~30Hz so
 * phone sees a continuously active main + cluster — matching the focus
 * state rewrite (NATIVE→PROJECTED on ch=1 0x8008). Only active when
 * GAL_CLUSTER_FAKE_MAIN_PROJECTED=1.
 *
 * Payload: byte-identical to gal0's natural and DHU's: 80 04 08 00 10 01. */
static void* keepalive_thread_fn(void* arg) {
    (void)arg;
    int interval_ms = g_fake_main_projected ? FAKE_MAIN_INTERVAL_MS
                                              : (g_keepalive_ms > 0 ? g_keepalive_ms : FAKE_MAIN_INTERVAL_MS);
    log_line("[gal_cluster] keepalive thread started, interval=%dms fake_main=%d keepalive_ms=%d\n",
             interval_ms, g_fake_main_projected, g_keepalive_ms);
    while (g_keepalive_running) {
        usleep((useconds_t)interval_ms * 1000);
        if (!g_keepalive_running) break;
        if (!real_queue_outgoing || !g_router_ptr) continue;

        /* RESTART_ON_RETURN follow-up: queueOutgoing scheduled the
         * PROJECTED tick countdown after firing NATIVE. Decrement each
         * tick; fire when reaches 1, then clear. Gives phone ~500ms in
         * NATIVE state to actually process it. */
        if (g_pending_projected_ticks > 0 && g_cluster_ch14_opened) {
            g_pending_projected_ticks--;
            if (g_pending_projected_ticks == 0) {
                uint8_t* prj = (uint8_t*)malloc(6);
                if (prj) {
                    prj[0] = 0x80; prj[1] = 0x01;
                    prj[2] = 0x08; prj[3] = 0x01;  /* PROJECTED */
                    prj[4] = 0x10; prj[5] = 0x01;
                    hook_queue_outgoing(g_router_ptr, 14, prj, 6);
                    log_line("[gal_cluster] restart-on-return: fired 0x8001 PROJECTED follow-up (after delay)\n");
                }
            }
        }

        /* STOPSTART_ON_RETURN follow-up: fire the Start after the Stop has had
         * time to settle on the phone side. Minimal Start payload — Start has
         * two required fields per disasm; values zero just satisfy IsInitialized. */
        if (g_pending_start_ticks > 0 && g_cluster_ch14_opened) {
            g_pending_start_ticks--;
            if (g_pending_start_ticks == 0) {
                uint8_t* st = (uint8_t*)malloc(6);
                if (st) {
                    st[0] = 0x80; st[1] = 0x01;
                    st[2] = 0x08; st[3] = 0x00;  /* Start.field1 (int32) = 0 */
                    st[4] = 0x10; st[5] = 0x00;  /* Start.field2 (uint32) = 0 */
                    hook_queue_outgoing(g_router_ptr, 14, st, 6);
                    log_line("[gal_cluster] stopstart-on-return: fired 0x8001 Start follow-up\n");
                }
            }
        }

        /* Fire only when HMI is internally NATIVE (gal0 silent on ch=1). When
         * HMI on AA, gal0's mirror handles ch=14 — no need to self-drive. */
        if (g_hmi_focus_projected) continue;
        if (!g_cluster_ch14_opened) continue;

        uint8_t bytes[6] = { 0x80, 0x04, 0x08, 0x00, 0x10, 0x01 };
        /* FAKE_MAIN_PROJECTED: dual-channel self-drive (ch=1 + ch=14).
         * KEEPALIVE_MS-only:   ch=14 self-drive. */
        if (g_fake_main_projected) {
            uint8_t* msg1 = (uint8_t*)malloc(6);
            if (msg1) {
                memcpy(msg1, bytes, 6);
                hook_queue_outgoing(g_router_ptr, 1, msg1, 6);
            }
        }
        uint8_t* msg14 = (uint8_t*)malloc(6);
        if (msg14) {
            memcpy(msg14, bytes, 6);
            hook_queue_outgoing(g_router_ptr, 14, msg14, 6);
        }
    }
    log_line("[gal_cluster] keepalive thread exiting\n");
    return NULL;
}

static void dummy_on_channel_opened(void* self, uint8_t ch) {
    log_line("[gal_cluster] cluster endpoint: onChannelOpened ch=%d\n", (int)ch);
    if (ch == 14 && !g_cluster_ch14_opened) {
        g_cluster_ch14_opened = 1;
        /* Push MEDIA_SETUP_RESPONSE on ch=14 immediately on open. Bytes copied
         * from gal0's main-channel ch=1 0x8003 — the same 12 bytes that worked
         * empirically across many sessions when the timing race didn't bite.
         * Done from here (not gal0-ch=1-mirror) so it always fires AFTER ch=14
         * is open. Race fix for run 140440 S2. */
        if (real_queue_outgoing && g_router_ptr) {
            uint8_t* setup = (uint8_t*)malloc(12);
            if (setup) {
                static const uint8_t bytes[12] = {
                    0x80, 0x03, 0x08, 0x02, 0x10, 0x08,
                    0x18, 0x00, 0x18, 0x01, 0x18, 0x02
                };
                memcpy(setup, bytes, 12);
                hook_queue_outgoing(g_router_ptr, 14, setup, 12);
                log_line("[gal_cluster] queueOutgoing: 0x8003 (12B, on-open) -> cluster ch=14\n");
            }
        }
        /* Start fake-main thread when FAKE_MAIN_PROJECTED mode is on.
         * Thread itself gates per-tick on g_hmi_focus_projected so it only
         * fires when gal0 has gone silent. Idempotent. */
        if ((g_fake_main_projected || g_keepalive_ms > 0) && !g_keepalive_running) {
            g_keepalive_running = 1;
            if (pthread_create(&g_keepalive_tid, NULL, keepalive_thread_fn, NULL) != 0) {
                log_line("[gal_cluster] keepalive: pthread_create failed errno=%d\n", errno);
                g_keepalive_running = 0;
            }
        }
    }
}

/* Hook MessageRouter::routeMessage — the central INCOMING dispatcher in
 * libautoreceiver. Called for every message phone sends to gal0 on any
 * application channel (post-TLS-decrypt, framing-parsed). Lets us see
 * exactly what phone sends back during HMI focus transitions so we can
 * identify what gal0 reacts to.
 *
 * Signature: void MessageRouter::routeMessage(uchar channel, shared_ptr<IoBuffer> const&)
 *
 * Just counts incoming per channel for now; msg_type parsing would require
 * reverse-engineering IoBuffer's internal layout (deferred). The per-second
 * rate_log emit will show how many incoming messages arrived on each channel.
 */
void _ZN13MessageRouter12routeMessageEhRK10shared_ptrI8IoBufferE(
        void* self, uint8_t channel, void* shared_ptr_ref)
{
    if (!real_routeMessage)
        real_routeMessage = (void(*)(void*, uint8_t, void*))
            dlsym(RTLD_NEXT, "_ZN13MessageRouter12routeMessageEhRK10shared_ptrI8IoBufferE");

    /* Record one incoming event per channel. */
    rate_log_record_in(channel, 1);

    /* Parse msg_type from IoBuffer. Layout from Controller::routeMessage
     * disasm: shared_ptr* + 4 = control block ptr; data = cb[0] + cb[2].
     * msg_type is the first 2 bytes (BE) of the message data. Mark incoming
     * by tagging channel with high bit so it doesn't collide with outgoing
     * in the rate log's types: section. */
    if (shared_ptr_ref) {
        intptr_t* cb = *(intptr_t**)((char*)shared_ptr_ref + 4);
        if (cb) {
            uint8_t* data = (uint8_t*)(cb[0] + cb[2]);
            if (data) {
                uint16_t msg_type = ((uint16_t)data[0] << 8) | data[1];
                rate_log_record_type((uint8_t)(channel | 0x80), msg_type);
            }
        }
    }

    if (real_routeMessage)
        real_routeMessage(self, channel, shared_ptr_ref);
}

/* Per-endpoint route hooks. These fire AFTER MessageRouter dispatches to the
 * appropriate endpoint. We get channel + msg_type cleanly. Log type counts
 * via rate_log_record_type so the types: section shows incoming as well.
 * We tag incoming types with high bit (channel | 0x80) to distinguish from
 * outgoing in the dump — TODO if it muddles the existing log, split later. */
static void log_incoming_type(uint8_t channel, uint16_t msg_type) {
    /* Use existing per-(ch,type) counter. Outgoing already populates this
     * table; incoming will mix in unless we mark differently. For now, use
     * a synthetic high-bit channel marker so they don't overlap. */
    rate_log_record_type((uint8_t)(channel | 0x80), msg_type);
}

void _ZN10Controller12routeMessageEhtRK10shared_ptrI8IoBufferE(
        void* self, uint8_t channel, uint16_t msg_type, void* shared_ptr_ref)
{
    if (!real_controller_routeMessage)
        real_controller_routeMessage = (void(*)(void*, uint8_t, uint16_t, void*))
            dlsym(RTLD_NEXT, "_ZN10Controller12routeMessageEhtRK10shared_ptrI8IoBufferE");
    log_incoming_type(channel, msg_type);
    if (real_controller_routeMessage)
        real_controller_routeMessage(self, channel, msg_type, shared_ptr_ref);
}

void _ZN13MediaSinkBase12routeMessageEhtRK10shared_ptrI8IoBufferE(
        void* self, uint8_t channel, uint16_t msg_type, void* shared_ptr_ref)
{
    if (!real_mediasinkbase_routeMessage)
        real_mediasinkbase_routeMessage = (void(*)(void*, uint8_t, uint16_t, void*))
            dlsym(RTLD_NEXT, "_ZN13MediaSinkBase12routeMessageEhtRK10shared_ptrI8IoBufferE");
    log_incoming_type(channel, msg_type);
    if (real_mediasinkbase_routeMessage)
        real_mediasinkbase_routeMessage(self, channel, msg_type, shared_ptr_ref);
}

static int dummy_cluster_route(void* self, uint32_t ch, uint16_t type, void* data) {
    static int cluster_data_count = 0;
    cluster_data_count++;
    /* Per-event log gated under GAL_CLUSTER_DEBUG (in addition to LOG_ALL master). */
    if (g_cl.debug && (cluster_data_count <= 10 || (cluster_data_count % 100) == 0)) {
        log_line("[gal_cluster] cluster data: ch=%u type=0x%04x count=%d data=%p\n",
                 ch, type, cluster_data_count, data);
    }
    /* Reply on the SAME channel the request arrived on, with the CORRECT
     * reply type per AA protocol.
     *
     *   0x8002 MEDIA_SETUP_REQ on ch=21 -> 0x8006 MEDIA_ACK on ch=21 (status=OK)
     *
     * Before this: dummy_cluster_route blindly sent MEDIA_ACK on ch=20 for
     * every incoming event. When phone opened ch=21 first and sent
     * MEDIA_SETUP_REQ on ch=21 before VIDEO_FOCUS_REQ on ch=20, our ACK
     * landed on the wrong channel → MEDIA_SETUP_REQ on ch=21 was never
     * acked → phone stalled 5-10s → watchdog killed gal. (~50% activation
     * failure.)
     *
     * Note: we DO NOT reply to 0x8000 VIDEO_FOCUS_REQ here. The
     * VIDEO_FOCUS_NOTIFICATION (0x8008) is already sent for cluster ch=20
     * by the queueOutgoing mirror path (when gal emits 0x8008 on main ch=1).
     * Sending 0x8008 twice on ch=20 is a protocol violation and phone
     * disconnects.
     *
     * CRITICAL: pass g_router_ptr (captured from MessageRouter calls) as self,
     * NOT our `self` here (which is the FAKE ENDPOINT). queueOutgoing expects
     * a MessageRouter*. Passing fake_endpoint → SIGSEGV inside gal. */
    if (real_queue_outgoing && g_router_ptr && type == 0x8002) {
        int total = 4; /* 2-byte type + 2-byte payload */
        uint8_t* msg = (uint8_t*)malloc((size_t)total);
        if (msg) {
            msg[0] = 0x80; msg[1] = 0x06;     /* MEDIA_ACK */
            msg[2] = 0x08; msg[3] = 0x00;     /* status = OK */
            hook_queue_outgoing(g_router_ptr, (uint8_t)ch, msg, total);
            if (g_cl.debug)
                log_line("[gal_cluster] cluster REPLY ch=%u: req=0x%04x -> resp=0x8006 plen=2\n",
                         ch, (unsigned)type);
        }
    }
    /* Write H.264 data to SHM:
     * msg_type=0x0001 → codec config (SPS/PPS NALs) — write raw payload
     * msg_type=0x0000 → video frame — skip 8-byte AA media header (flags+timestamp)
     * All other msg_types → skip
     * Channel 14 = cluster video. svc_id=1 was tested — phone rejected. */
    if (ch == 14 && data && g_cluster_shm) {
        uint32_t* sp = (uint32_t*)data;
        if (sp[1]) {
            uint32_t* inner = (uint32_t*)sp[1];
            uint8_t* buf = (uint8_t*)inner[0] + inner[2];
            int total = (int)(inner[3] - inner[2]);
            /* Rate log: count phone→us bytes on this channel. */
            if (total > 0) rate_log_record_in((uint8_t)ch, (uint32_t)total);
            if (total > 2 && total < 500000) {
                uint16_t msg_type = ((uint16_t)buf[0] << 8) | buf[1];
                uint8_t* payload = buf + 2; /* skip 2-byte msg_type */
                int plen = total - 2;
                if (msg_type == 0x0001) {
                    /* Codec config (SPS/PPS) — write entire payload as-is */
                } else if (msg_type == 0x0000 && plen > 8) {
                    /* Video frame — skip 8-byte AA header (4b flags + 4b timestamp) */
                    payload += 8;
                    plen -= 8;
                } else {
                    plen = 0; /* skip other msg_types */
                }
                /* DIAGNOSTIC: dump first 32 bytes of codec config (once) and first
                 * frame (once) so we can verify Annex B vs AVCC format. */
                static int dumped_config = 0, dumped_frame = 0;
                if (plen > 0 && msg_type == 0x0001 && !dumped_config) {
                    dumped_config = 1;
                    int n = plen < 32 ? plen : 32;
                    char hex[200] = {0}; int o = 0;
                    for (int i = 0; i < n; i++) o += snprintf(hex+o, sizeof(hex)-o, "%02x ", payload[i]);
                    log_line("[gal_cluster] CODEC_CONFIG plen=%d first%d: %s\n", plen, n, hex);
                }
                if (plen > 0 && msg_type == 0x0000 && !dumped_frame) {
                    dumped_frame = 1;
                    int n = plen < 32 ? plen : 32;
                    char hex[200] = {0}; int o = 0;
                    for (int i = 0; i < n; i++) o += snprintf(hex+o, sizeof(hex)-o, "%02x ", payload[i]);
                    log_line("[gal_cluster] FRAME0 plen=%d first%d: %s\n", plen, n, hex);
                }
                /* Feed to in-hook decoder */
                if (plen > 0) cl_feed_h264(payload, plen);

                if (plen > 0 && g_cluster_shm) {
                    cluster_h264_shm_t* shm = g_cluster_shm;
                    uint32_t wp = shm->write_pos;
                    int remain = CLUSTER_RING_SIZE - (int)wp;
                    if (plen <= remain) {
                        memcpy(shm->ring + wp, payload, (size_t)plen);
                    } else {
                        memcpy(shm->ring + wp, payload, (size_t)remain);
                        memcpy(shm->ring, payload + remain, (size_t)(plen - remain));
                    }
                    shm->write_pos = (wp + (uint32_t)plen) % CLUSTER_RING_SIZE;
                    shm->total_bytes += (uint32_t)plen;
                }

                /* Debug-only: write raw H.264 to a growing file in /tmp (RAM)
                 * for offline ffmpeg decode. Gated by GAL_CLUSTER_DEBUG env var.
                 * Capped at 20 MB. Format: Annex-B NAL stream. */
                if (plen > 0 && g_cl.debug) {
                    static FILE* h264_dump = NULL;
                    static size_t h264_written = 0;
                    static int h264_done = 0;
                    if (!h264_done && !h264_dump) {
                        h264_dump = fopen("/tmp/cluster_stream.h264", "wb");
                        if (!h264_dump) {
                            log_line("[gal_cluster] cluster_stream.h264 fopen failed errno=%d\n", errno);
                            h264_done = 1;
                        }
                    }
                    if (h264_dump && !h264_done) {
                        size_t cap = 20 * 1024 * 1024;
                        size_t to_write = (size_t)plen;
                        if (h264_written + to_write > cap) to_write = cap - h264_written;
                        if (to_write > 0) {
                            fwrite(payload, 1, to_write, h264_dump);
                            h264_written += to_write;
                        }
                        if (h264_written >= cap) {
                            fflush(h264_dump);
                            fclose(h264_dump);
                            h264_dump = NULL;
                            h264_done = 1;
                            log_line("[gal_cluster] cluster_stream.h264 capped at %zu bytes\n", h264_written);
                        }
                    }
                }
                if (g_cl.debug && (cluster_data_count <= 5 || (cluster_data_count % 500) == 0)) {
                    log_line("[gal_cluster] cluster shm: frame=%d mt=0x%04x plen=%d total=%u wp=%u\n",
                             cluster_data_count, msg_type, plen, g_cluster_shm->total_bytes,
                             g_cluster_shm->write_pos);
                }
            }
        }
    }
    return 0;
}
static int vtable_stub(void) { return 0; }
static void* fake_vtable[16] = {0};
static uint32_t fake_endpoint[16] = {0};

void _ZN13MessageRouter19sendChannelOpenRespEhi(void* self, uint8_t ch, int status)
{
    if (!g_router_ptr) g_router_ptr = self;
    if (!real_send_channel_open_resp)
        real_send_channel_open_resp = (void(*)(void*,uint8_t,int))
            dlsym(RTLD_NEXT, "_ZN13MessageRouter19sendChannelOpenRespEhi");
    if (!real_register_channel)
        real_register_channel = (int(*)(void*,uint8_t,int8_t))
            dlsym(RTLD_DEFAULT, "_ZN14ChannelManager15registerChannelEha");
    if (!real_setup_mapping)
        real_setup_mapping = (void(*)(void*,uint8_t,uint8_t))
            dlsym(RTLD_DEFAULT, "_ZN13MessageRouter12setupMappingEhh");

    /* Install fake endpoint on FIRST sendChannelOpenResp call (any channel).
     * This ensures the endpoint is in the table BEFORE handleChannelOpenReq
     * runs for cluster channels 20/21. */
    {
        static int endpoint_installed = 0;
        if (!endpoint_installed && g_cluster_enabled) {
            { int vi; for (vi = 0; vi < 16; vi++) fake_vtable[vi] = (void*)vtable_stub; }
            fake_vtable[3] = (void*)dummy_may_open;
            fake_vtable[4] = (void*)dummy_on_channel_opened;
            fake_vtable[5] = (void*)dummy_cluster_route;
            fake_endpoint[0] = (uint32_t)fake_vtable;
            fake_endpoint[4] = 0;

            /* Install for service 14 (cluster video) and 21 (cluster input).
             * Tested svc_id=1 — phone rejected (reserves id=1 for AudioSource),
             * gal cycled through repeat restarts. svc_id=14 is best so far. */
            uint32_t slot14 = (14 + 0x40) * 4;
            uint32_t slot21 = (21 + 0x40) * 4;
            *(uint32_t**)((uint8_t*)self + slot14) = fake_endpoint;
            *(uint32_t**)((uint8_t*)self + slot21) = fake_endpoint;
            endpoint_installed = 1;
            log_line("[gal_cluster] fake endpoint installed at router+0x%x and +0x%x for svc 14,21\n",
                     slot14, slot21);
        }
    }

    /* For cluster channels: handleChannelOpenReq will now find our fake endpoint,
     * call mayOpenChannel→1, setupMapping, return 0. The normal flow then calls
     * registerChannel, sendChannelOpenResp(0), notifyChannelOpened — all properly.
     * No special handling needed here. */
    if (status == (int)0xfffffffc || status == (int)0xfffffffb) {
        /* This shouldn't happen anymore for cluster channels (they have endpoints now).
         * But keep as safety net for any other unknown services. */
        log_line("[gal_cluster] sendChannelOpenResp: ch=%d status=%d -> ACCEPTING (safety net)\n", (int)ch, status);
        status = 0;
    } else {
        log_line("[gal_cluster] sendChannelOpenResp: ch=%d status=%d\n", (int)ch, status);
    }

    if (real_send_channel_open_resp)
        real_send_channel_open_resp(self, ch, status);
}

/* NavFocusRequestNotification hook — logs focus requests */
int _ZN27NavFocusRequestNotification27MergePartialFromCodedStreamEPN6google8protobuf2io16CodedInputStreamE(
    void* self, void* cis)
{
    resolve_symbols();
    int ok = real_navfocusreq_merge ? real_navfocusreq_merge(self, cis) : 0;
    if (ok && g_log_all && real_navfocusreq_size && real_serialize_to_array) {
        int size = real_navfocusreq_size(self);
        if (size > 0 && size <= MAX_SERIALIZE_BYTES) {
            uint8_t* buf = (uint8_t*)malloc((size_t)size);
            if (buf) {
                if (real_serialize_to_array(self, buf, size)) {
                    int f = g_navfocusreq_focus_field_number ? *g_navfocusreq_focus_field_number : 1;
                    uint64_t focus = 0;
                    if (pb_find_varint_field(buf, (size_t)size, f, &focus))
                        log_line("[gal_cluster] nav_focus_request focus=%llu\n", (unsigned long long)focus);
                    else
                        log_line("[gal_cluster] nav_focus_request focus=?\n");
                }
                free(buf);
            }
        }
    }
    return ok;
}


