/*
 * Copyright (c) 2026 fifthBro
 * https://fifthbro.github.io
 *
 * Licensed under CC BY-NC-SA 4.0
 * https://creativecommons.org/licenses/by-nc-sa/4.0/
 * NOT FOR COMMERCIAL USE
 */

/*
 * dio_manager_preload — LD_PRELOAD hook for MH2P dio_manager (QNX 6.6 ARMv7)
 *
 * Evolution of iap2_rgd_hook.c. Targets dio_manager instead of
 * iap2connectionmanager because that's the process that runs the actual
 * CarPlay-over-WiFi iAP2 session and hosts the Location / NMEA / iAP2
 * modules. Route Guidance (0x5200/0x5201/0x5202/0x5204) must be negotiated
 * in that process's Identify, not in iap2connectionmanager's bootstrap one.
 *
 * Inject path: uses CIAP2ControlSession::deployMessage(view) with a real
 * CIAP2ControlSessionMessageView constructed via library ctor. No raw
 * CIAP2Packet + deployPacket anymore (that path caused writeToFile+0x4e
 * UAFs in CIAP2LinkPacketDeployer — we bypassed the library's lifecycle).
 *
 * Defensive: constructor checks /proc/self/comm and no-ops unless we're
 * actually inside dio_manager. Safe to leave preloaded elsewhere.
 *
 * Build:  qcc -Vgcc_ntoarmv7le -shared -fPIC -O2 -o dio_manager_preload.so \
 *             dio_manager_preload.c -lc
 * Deploy: LD_PRELOAD=/mnt/app/eso/bin/apps/dio_manager_preload.so in the
 *         "carplay" child's envs[] of smartphone_integrator.json.
 * Log:    /tmp/dio_cluster.log
 *
 * No flag file — dio_manager only starts after the user accepts CarPlay,
 * so hook activity is naturally gated by that. Process-name check is the
 * only safety net (no-ops in any non-dio_manager process).
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Luka's RG parser + PPS writer (compiled alongside via -I flags). */
#include "rgd_tlv.h"
#include "pps_writer.h"

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* Gated by env var DIO_CLUSTER_LOG (default off). Set to "1" before    */
/* loading the hook to enable. Macros at the bottom of this section     */
/* skip the call entirely (no arg eval) when disabled.                  */
/* ------------------------------------------------------------------ */
#define LOG_FILE_NAME "dio_cluster.log"
/* Same path priority as cluster_daemon.log and gal_cluster.log — external
 * media (USB/SD) preferred over /tmp so logs persist. First writable wins. */
static const char * const DIO_LOG_ROOTS[] = {
    "/fs/usb0_0", "/fs/usb1_0", "/fs/sda0", "/fs/sdb0", "/tmp", NULL
};
static FILE *g_log = NULL;
static pthread_mutex_t g_log_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_log_enabled = 0;

static void log_init_from_env(void) {
    const char *v = getenv("DIO_CLUSTER_LOG");
    g_log_enabled = (v && *v && *v != '0') ? 1 : 0;
}

static FILE *dio_open_log(void) {
    char path[256];
    for (int i = 0; DIO_LOG_ROOTS[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", DIO_LOG_ROOTS[i], LOG_FILE_NAME);
        FILE *f = fopen(path, "a");
        if (f) return f;
    }
    return NULL;
}

static void log_msg_impl(const char *fmt, ...) {
    pthread_mutex_lock(&g_log_mtx);
    if (!g_log) {
        g_log = dio_open_log();
        if (!g_log) { pthread_mutex_unlock(&g_log_mtx); return; }
        setbuf(g_log, NULL);
    }
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (tm) fprintf(g_log, "[%02d:%02d:%02d] ", tm->tm_hour, tm->tm_min, tm->tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    pthread_mutex_unlock(&g_log_mtx);
}

static void log_hex_impl(const char *label, const uint8_t *data, size_t len) {
    pthread_mutex_lock(&g_log_mtx);
    if (!g_log) { pthread_mutex_unlock(&g_log_mtx); return; }
    size_t dump = len > 512 ? 512 : len;
    fprintf(g_log, "  %s (%zu bytes): ", label, len);
    size_t i;
    for (i = 0; i < dump; i++) fprintf(g_log, "%02x ", data[i]);
    if (dump < len) fprintf(g_log, "...");
    fputc('\n', g_log);
    pthread_mutex_unlock(&g_log_mtx);
}

/* Walk TLVs inside an iAP2 control session message (40 40 LL LL MM MM ...)
 * and log each top-level TLV's {length, tag}. Helps identify which capabilities
 * the HU declares (tag 0x0006=MessagesSent, 0x0007=MessagesReceived, etc.). */
static void log_tlv_tags_impl(const char *label, const uint8_t *msg, size_t len) {
    if (!msg || len < 8) return;
    if (msg[0] != 0x40 || msg[1] != 0x40) return;
    pthread_mutex_lock(&g_log_mtx);
    if (!g_log) { pthread_mutex_unlock(&g_log_mtx); return; }
    fprintf(g_log, "  %s TLVs:", label);
    size_t off = 6;
    int count = 0;
    while (off + 4 <= len && count < 200) {
        uint16_t tlv_len = (uint16_t)((msg[off] << 8) | msg[off+1]);
        uint16_t tlv_tag = (uint16_t)((msg[off+2] << 8) | msg[off+3]);
        if (tlv_len < 4 || off + tlv_len > len) {
            fprintf(g_log, " [BAD@%zu len=%u]", off, tlv_len);
            break;
        }
        /* For tags 0x0006/0x0007 (msgid lists), show the inner 2-byte value */
        if ((tlv_tag == 0x0006 || tlv_tag == 0x0007) && tlv_len == 6) {
            uint16_t mid = (uint16_t)((msg[off+4] << 8) | msg[off+5]);
            fprintf(g_log, " {t=%04x=%04x}", tlv_tag, mid);
        } else {
            fprintf(g_log, " {t=%04x l=%u}", tlv_tag, tlv_len);
        }
        off += tlv_len;
        count++;
    }
    fputc('\n', g_log);
    pthread_mutex_unlock(&g_log_mtx);
}

/* Gating macros — args are only evaluated when g_log_enabled != 0.
 * Defined AFTER the impls so the function definitions above are not
 * subject to macro expansion. */
#define log_msg(...)                       do { if (g_log_enabled) log_msg_impl(__VA_ARGS__); } while (0)
#define log_hex(label, data, len)          do { if (g_log_enabled) log_hex_impl((label), (data), (len)); } while (0)
#define log_tlv_tags(label, msg, len)      do { if (g_log_enabled) log_tlv_tags_impl((label), (msg), (len)); } while (0)

/* ------------------------------------------------------------------ */
/* iAP2 message IDs of interest                                        */
/* ------------------------------------------------------------------ */
#define IAP2_MSG_IDENTIFY               0x1D00
#define IAP2_MSG_IDENTIFY_ACCEPTED      0x1D02
#define IAP2_MSG_IDENTIFY_REJECTED      0x1D03
#define IAP2_MSG_RG_START               0x5200
#define IAP2_MSG_RG_UPDATE              0x5201
#define IAP2_MSG_RG_MANEUVER            0x5202
#define IAP2_MSG_RG_STOP                0x5203
#define IAP2_MSG_RG_LANE                0x5204

static const char *msgid_name(uint16_t id) {
    switch (id) {
        case IAP2_MSG_IDENTIFY:          return "Identify";
        case IAP2_MSG_IDENTIFY_ACCEPTED: return "IdentifyAccepted";
        case IAP2_MSG_IDENTIFY_REJECTED: return "IdentifyRejected";
        case IAP2_MSG_RG_START:          return "RG_Start";
        case IAP2_MSG_RG_UPDATE:         return "RG_Update";
        case IAP2_MSG_RG_MANEUVER:       return "RG_Maneuver";
        case IAP2_MSG_RG_STOP:           return "RG_Stop";
        case IAP2_MSG_RG_LANE:           return "RG_Lane";
        default:                         return NULL;
    }
}

/* ------------------------------------------------------------------ */
/* No runtime gate. LD_PRELOAD is only wired for dio_manager via       */
/* smartphone_integrator.json's "carplay" child envs[], so the hook    */
/* only loads there in the first place. And dio_manager itself only    */
/* runs after the user accepts CarPlay, which is the natural trigger.  */
/* ------------------------------------------------------------------ */
static int is_enabled(void) { return 1; }

/* ------------------------------------------------------------------ */
/* Counters                                                            */
/* ------------------------------------------------------------------ */
static volatile uint32_t g_deploy_count = 0;
static volatile uint32_t g_ident_count = 0;
static volatile uint32_t g_session_count = 0;

/* ------------------------------------------------------------------ */
/* Real function pointers                                              */
/* ------------------------------------------------------------------ */

/*
 * iap2::CIAP2ControlSession::deployMessage(const iap2::CIAP2ControlSessionMessageView&)
 * Mangled: _ZN4iap219CIAP2ControlSession13deployMessageERKNS_30CIAP2ControlSessionMessageViewE
 *
 * This dispatches incoming messages to registered modules.
 * Messages without a module (like 0x5201) are silently dropped.
 * We hook to observe ALL incoming messages.
 */
typedef int (*fn_deployMessage_t)(void *self, const void *msgView);
static fn_deployMessage_t real_deployMessage = NULL;

/*
 * iap2::CIAP2Link::sendToSession(const iap2::CIAP2PacketView&)
 * Mangled: _ZN4iap29CIAP2Link13sendToSessionERKNS_15CIAP2PacketViewE
 *
 * ALL incoming iAP2 packets pass through here after link-layer processing.
 * Hooked minimally to capture CIAP2Link instance for injection.
 */

/*
 * iap2::CIAP2ControlSessionModuleIdent::identificationInformation(
 *     const iap2::messages::identification::CMessageParameterIdentificationInformation&)
 * Mangled: _ZN4iap230CIAP2ControlSessionModuleIdent25identificationInformationERKNS_8messages14identification42CMessageParameterIdentificationInformationE
 *
 * Called to build the outgoing Identify message. We hook to log its content
 * and later patch it to include Route Guidance capability.
 */
typedef int (*fn_identInfo_t)(void *self, const void *identParam);
static fn_identInfo_t real_identInfo = NULL;

/*
 * iap2::CIAP2ControlSessionModuleIdent::process(const iap2::CIAP2ControlSessionMessageView&)
 * Mangled: _ZN4iap230CIAP2ControlSessionModuleIdent7processERKNS_30CIAP2ControlSessionMessageViewE
 *
 * Processes incoming Identify response (accepted/rejected).
 */
typedef int (*fn_identProcess_t)(void *self, const void *msgView);
static fn_identProcess_t real_identProcess = NULL;

/* CIAP2ControlSession::process hook REMOVED in v14 — it was firing on every
 * incoming control-session packet in a hot path and contributed to the
 * writeToFile use-after-free crash during teardown. We already see every
 * incoming packet via the CIAP2Link::sendToSession hook, which is upstream
 * of process() anyway, so this hook added no new diagnostic value. */

/* ------------------------------------------------------------------ */
/* Route Guidance Component TLV builder (from luka-dev rgd_tlv.c)      */
/* ------------------------------------------------------------------ */

#define IDENT_TLV_RG_COMPONENT   0x001E
#define RGD_COMPONENT_ID         0x0010

/* ------------------------------------------------------------------ */
/* PPS bridge — hook side: parse RG, publish to /pps/...               */
/* Java side (CarPlayClusterIntegration) subscribes and pushes BAP.    */
/* ------------------------------------------------------------------ */
/* Plain file in /tmp — matches the convention for our cluster IPC on MH2P
 * (mirror FIFO is /tmp/cluster_ctl). Java polls the same path. */
#define PPS_RG_PATH   "/tmp/cluster_cp.state"
#define PPS_RG_OBJECT "cluster_cp"

static pps_handle_t *g_pps = NULL;
static pthread_mutex_t g_pps_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Latest-known route state. 0x5201 updates multiple fields; 0x5202 with
 * index=0 updates the current maneuver. We cache and republish all fields
 * together so Java sees a coherent snapshot. */
static struct {
    int     have_update;       /* got at least one 0x5201 */
    uint8_t route_state;
    uint8_t distance_units;          /* TLV 0x0009 — TOTAL trip (0=km,1=mi,2=m,3=yd,4=ft) */
    uint8_t dist_to_maneuver_units;  /* TLV 0x000C — NEXT TURN (same encoding) */
    uint8_t lane_guidance_showing;   /* TLV 0x0012 — 0=hide lanes, 1=show */
    uint8_t source_supports_rg;      /* TLV 0x0014 */
    uint32_t distance_remaining;     /* TLV 0x0007 — total trip, in units above */
    uint32_t dist_to_maneuver;       /* TLV 0x000A — distance to next turn (live-updating) */
    uint64_t time_remaining;
    uint64_t eta;
    char    destination[256];
    char    current_road[256];
    char    source_name[64];

    /* Maneuver list from 0x5201 TLV 0x000D — iAP indexes of upcoming maneuvers
     * in order. maneuver_list[0] is the CURRENT next turn. iPhone sends the
     * full list of 0x5202 maneuvers once upfront, then advances the list in
     * 0x5201 updates as the driver passes each turn. */
    uint16_t maneuver_list[MAX_MANEUVER_LIST];
    uint16_t maneuver_list_count;
    int      have_maneuver_list;   /* at least one 0x5201 with list */

    /* Active lane-guidance set, signalled in 0x5201 TLV 0x0010
     * (LaneGuidanceIndex). This is the lookup key into g_lane_sets[] for
     * what the cluster should currently display — NOT the maneuver iap_idx
     * and not the MAN_TLV_LINKED_LANE_GUIDANCE link from 5202 (which is
     * never present in observed Apple/Google traffic). Apple/Google update
     * this index as the route progresses to point at the lane set whose
     * lanes apply right now. 0xFFFF = no current lane set. */
    uint16_t lane_guidance_index;
} g_rg_state;

/* Slot cache — LRU-replaced maneuver slots keyed by iAP index.
 * Mirrors Luka's slot_cache, sized for BAP's 6-slot MaxGuidanceManeuverStorageCapacity. */
/* Cache capacity. Apple re-broadcasts slot[cur] every tick AND injects
 * phantom indices (6/7/8) around reroutes — with only 6 slots the LRU
 * evicts upcoming turns before ML advances to them, stranding the cluster
 * on the last known maneuver. 16 gives headroom for a typical route. */
#define MAN_SLOT_COUNT 16
/* BAP spec renders at most 6 upcoming maneuvers; cap PPS emission regardless
 * of cache size so Java's MAX_SLOTS=6 array stays in sync. */
#define MAN_SLOT_PPS_MAX 6
typedef struct {
    uint16_t iap_index;          /* 0xFFFF = empty */
    uint32_t last_seq;           /* LRU ordering */
    uint8_t  type;
    uint8_t  driving_side;
    uint8_t  junction_type;
    int16_t  exit_angle;
    int16_t  junction_angles[MAX_MANEUVER_LIST];
    uint16_t junction_angle_count;
    uint32_t distance_between;
    char     description[256];
    char     after_road[256];
    char     exit_info[RGD_EXIT_INFO_MAX + 1];
} man_slot_t;

static man_slot_t g_slots[MAN_SLOT_COUNT];
static uint32_t   g_slot_seq_counter = 0;

/* Lane-set cache, parallel to slot cache. Apple/Google send 0x5204
 * messages keyed by `LaneGuidanceIndex` and use the same numeric space as
 * iap_idx (verified 2026-04-26: a burst of 5204s with indices 0..5
 * arrived BEFORE the matching 5202s with iap_idx 0..5 — so the linkage is
 * implicit, MAN_TLV_LINKED_LANE_GUIDANCE was not present in any 5202).
 * We cache lane sets keyed by lane_guidance_index and look up by current
 * maneuver's iap_idx at publish time. */
#define LANE_SET_COUNT 16
typedef struct {
    uint16_t lg_index;             /* 0xFFFF = empty */
    uint32_t last_seq;
    uint8_t  lane_count;
    uint16_t lane_pos[MAX_LANE_GUIDANCE];
    int16_t  lane_dir[MAX_LANE_GUIDANCE];
    uint8_t  lane_status[MAX_LANE_GUIDANCE];
    int16_t  lane_angles[MAX_LANE_GUIDANCE][MAX_LANE_ANGLES];
    uint8_t  lane_angle_count[MAX_LANE_GUIDANCE];
} lane_set_t;

static lane_set_t g_lane_sets[LANE_SET_COUNT];
static uint32_t   g_lane_seq_counter = 0;

static void slots_init_once(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    int i;
    for (i = 0; i < MAN_SLOT_COUNT; i++) g_slots[i].iap_index = 0xFFFF;
    for (i = 0; i < LANE_SET_COUNT; i++) g_lane_sets[i].lg_index = 0xFFFF;
    g_rg_state.lane_guidance_index = 0xFFFF;
}

static int lane_set_find(uint16_t lg_idx) {
    int i;
    for (i = 0; i < LANE_SET_COUNT; i++)
        if (g_lane_sets[i].lg_index == lg_idx) return i;
    return -1;
}

static int lane_set_allocate(uint16_t lg_idx) {
    int existing = lane_set_find(lg_idx);
    if (existing >= 0) return existing;
    int i, oldest = 0;
    for (i = 0; i < LANE_SET_COUNT; i++) {
        if (g_lane_sets[i].lg_index == 0xFFFF) {
            g_lane_sets[i].lg_index = lg_idx;
            return i;
        }
        if (g_lane_sets[i].last_seq < g_lane_sets[oldest].last_seq) oldest = i;
    }
    memset(&g_lane_sets[oldest], 0, sizeof(g_lane_sets[oldest]));
    g_lane_sets[oldest].lg_index = lg_idx;
    return oldest;
}

/* Find the slot that currently holds iap_idx, or -1 if none. */
static int slot_find(uint16_t iap_idx) {
    int i;
    for (i = 0; i < MAN_SLOT_COUNT; i++)
        if (g_slots[i].iap_index == iap_idx) return i;
    return -1;
}

/* True if iap_idx is referenced by the current maneuver_list (upcoming turn).
 * Used to pin live entries against LRU eviction — Apple's re-broadcast pattern
 * otherwise rotates past-turn slots to the top and evicts the ones ML is
 * about to advance to. */
static int slot_pinned_by_ml(uint16_t iap_idx) {
    uint16_t i;
    if (!g_rg_state.have_maneuver_list) return 0;
    for (i = 0; i < g_rg_state.maneuver_list_count; i++)
        if (g_rg_state.maneuver_list[i] == iap_idx) return 1;
    return 0;
}

/* Allocate a slot for iap_idx (reuse existing or LRU-evict).
 * Eviction refuses slots whose index is in the current maneuver_list —
 * those are upcoming turns the driver has not passed yet. */
static int slot_allocate(uint16_t iap_idx) {
    int existing = slot_find(iap_idx);
    if (existing >= 0) return existing;
    /* find empty */
    int i;
    int oldest = -1;
    for (i = 0; i < MAN_SLOT_COUNT; i++) {
        if (g_slots[i].iap_index == 0xFFFF) {
            g_slots[i].iap_index = iap_idx;
            return i;
        }
        if (slot_pinned_by_ml(g_slots[i].iap_index)) continue;
        if (oldest < 0 || g_slots[i].last_seq < g_slots[oldest].last_seq) oldest = i;
    }
    /* All unpinned slots exhausted — fall back to global LRU so we still
     * make progress (shouldn't happen with 16 slots and a normal route). */
    if (oldest < 0) {
        oldest = 0;
        for (i = 1; i < MAN_SLOT_COUNT; i++)
            if (g_slots[i].last_seq < g_slots[oldest].last_seq) oldest = i;
    }
    memset(&g_slots[oldest], 0, sizeof(g_slots[oldest]));
    g_slots[oldest].iap_index = iap_idx;
    return oldest;
}

/* Build a CSV from int16_t array: "a,b,c". Returns bytes written (excl NUL). */
static size_t angles_to_csv(const int16_t *vals, uint16_t n, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return 0;
    size_t off = 0; out[0] = '\0';
    for (uint16_t i = 0; i < n && off + 8 < out_sz; i++) {
        int written = snprintf(out + off, out_sz - off, "%s%d", i > 0 ? "," : "", (int)vals[i]);
        if (written > 0) off += (size_t)written;
    }
    return off;
}

static void pps_publish_state(void) {
    if (!g_pps) return;
    pthread_mutex_lock(&g_pps_mtx);
    pps_begin(g_pps);
    pps_write_header(g_pps);
    pps_write_bool(g_pps, "session_active", g_rg_state.have_update);
    if (g_rg_state.have_update) {
        pps_write_int(g_pps, "route_state", g_rg_state.route_state);
        pps_write_string(g_pps, "destination", g_rg_state.destination);
        pps_write_string(g_pps, "current_road", g_rg_state.current_road);
        pps_write_string(g_pps, "source_name", g_rg_state.source_name);
        pps_write_uint(g_pps, "distance_remaining", g_rg_state.distance_remaining);
        pps_write_uint(g_pps, "time_remaining", g_rg_state.time_remaining);
        pps_write_uint(g_pps, "eta", g_rg_state.eta);
        pps_write_int(g_pps, "distance_units", g_rg_state.distance_units);
        pps_write_int(g_pps, "source_supports_rg", g_rg_state.source_supports_rg);
        pps_write_uint(g_pps, "dist_to_maneuver", g_rg_state.dist_to_maneuver);
        pps_write_int(g_pps, "dist_to_maneuver_units", g_rg_state.dist_to_maneuver_units);
        pps_write_int(g_pps, "lane_guidance_showing", g_rg_state.lane_guidance_showing);
    }
    /* Stage 2 — emit slot array + current-slot pointer.
     * Slot ordering: walk the maneuver_list in order so Java gets slots
     * in turn order (slot_0 = next turn, slot_1 = one after, ...). If no
     * maneuver_list yet, just dump populated slots in their native order. */
    int emitted = 0;
    uint16_t i;
    char key[40];
    int current_slot_out = -1;   /* which emitted slot is "current next turn" */

    if (g_rg_state.have_maneuver_list) {
        for (i = 0; i < g_rg_state.maneuver_list_count && emitted < MAN_SLOT_PPS_MAX; i++) {
            int si = slot_find(g_rg_state.maneuver_list[i]);
            if (si < 0) continue;     /* list ref before 0x5202 arrived */
            man_slot_t *s = &g_slots[si];

            if (emitted == 0) current_slot_out = emitted;

            snprintf(key, sizeof(key), "slot_%d_iap_idx", emitted);
            pps_write_int(g_pps, key, s->iap_index);
            snprintf(key, sizeof(key), "slot_%d_type", emitted);
            pps_write_int(g_pps, key, s->type);
            snprintf(key, sizeof(key), "slot_%d_description", emitted);
            pps_write_string(g_pps, key, s->description);
            snprintf(key, sizeof(key), "slot_%d_road", emitted);
            pps_write_string(g_pps, key, s->after_road);
            snprintf(key, sizeof(key), "slot_%d_distance", emitted);
            pps_write_uint(g_pps, key, s->distance_between);
            snprintf(key, sizeof(key), "slot_%d_driving_side", emitted);
            pps_write_int(g_pps, key, s->driving_side);
            snprintf(key, sizeof(key), "slot_%d_junction_type", emitted);
            pps_write_int(g_pps, key, s->junction_type);
            snprintf(key, sizeof(key), "slot_%d_exit_angle", emitted);
            pps_write_int(g_pps, key, s->exit_angle);
            snprintf(key, sizeof(key), "slot_%d_exit_info", emitted);
            pps_write_string(g_pps, key, s->exit_info);
            {
                char csv[256];
                angles_to_csv(s->junction_angles, s->junction_angle_count, csv, sizeof(csv));
                snprintf(key, sizeof(key), "slot_%d_junction_angles", emitted);
                pps_write_string(g_pps, key, csv);
            }
            emitted++;
        }
    }
    pps_write_int(g_pps, "slot_count", emitted);
    pps_write_int(g_pps, "current_slot", current_slot_out);
    /* Stage 3: lane guidance. The cluster should display the lane set
     * Apple/Google explicitly point at via 0x5201 TLV 0x0010
     * (LaneGuidanceIndex), NOT the lane set whose lg_idx happens to equal
     * the current maneuver's iap_idx. Verified 2026-04-27 against direct
     * user observation: at iap_idx=3 (Scotland Rd) Apple sent 5201.lgi=0,
     * the iPhone screen showed lg_idx=0's content — 5202 / 5204 indices
     * are independent number spaces, the 5201 explicitly binds them. */
    {
        int lane_set_idx = -1;
        if (g_rg_state.lane_guidance_index != 0xFFFF) {
            lane_set_idx = lane_set_find(g_rg_state.lane_guidance_index);
        }
        if (lane_set_idx >= 0 && g_lane_sets[lane_set_idx].lane_count > 0) {
            lane_set_t *ls = &g_lane_sets[lane_set_idx];
            pps_write_bool(g_pps, "lane_active", 1);
            pps_write_int(g_pps, "lane_count", ls->lane_count);
            char key[32];
            uint8_t i;
            for (i = 0; i < ls->lane_count && i < MAX_LANE_GUIDANCE; i++) {
                snprintf(key, sizeof(key), "lane_%u_pos", (unsigned)i);
                pps_write_int(g_pps, key, ls->lane_pos[i]);
                snprintf(key, sizeof(key), "lane_%u_dir", (unsigned)i);
                pps_write_int(g_pps, key, ls->lane_dir[i]);
                snprintf(key, sizeof(key), "lane_%u_status", (unsigned)i);
                pps_write_int(g_pps, key, ls->lane_status[i]);
                {
                    char csv[256];
                    angles_to_csv(ls->lane_angles[i],
                                  ls->lane_angle_count[i],
                                  csv, sizeof(csv));
                    snprintf(key, sizeof(key), "lane_%u_angles", (unsigned)i);
                    pps_write_string(g_pps, key, csv);
                }
            }
        } else {
            pps_write_bool(g_pps, "lane_active", 0);
        }
    }
    pps_end(g_pps);
    pthread_mutex_unlock(&g_pps_mtx);
}

/* Called from sendToSession when we see 0x5201 RouteGuidanceUpdate.
 * buf/len is the full payload "40 40 LL LL 52 01 <TLVs>"; rgd_parse_update
 * expects TLV bytes only, so we skip the 6-byte message header. */
static void rg_on_update(const uint8_t *msg_payload, size_t msg_len) {
    if (msg_len < 8) return;
    rgd_update_t u;
    memset(&u, 0, sizeof(u));
    rgd_parse_update(msg_payload + 6, msg_len - 6, &u);

    g_rg_state.have_update = 1;
    if (u.present & RGD_UPD_ROUTE_STATE)        g_rg_state.route_state        = u.route_state;
    if (u.present & RGD_UPD_DISTANCE_REMAINING) g_rg_state.distance_remaining = u.distance_remaining;
    if (u.present & RGD_UPD_TIME_REMAINING)     g_rg_state.time_remaining     = u.time_remaining;
    if (u.present & RGD_UPD_ETA)                g_rg_state.eta                = u.eta;
    if (u.present & RGD_UPD_DISTANCE_UNITS)     g_rg_state.distance_units     = u.distance_units;
    if (u.present & RGD_UPD_SOURCE_SUPPORTS_RG) g_rg_state.source_supports_rg = u.source_supports_route_guidance;
    if (u.present & RGD_UPD_DIST_TO_MANEUVER)     g_rg_state.dist_to_maneuver       = u.dist_to_maneuver;
    if (u.present & RGD_UPD_DIST_TO_MANEUVER_UNI) g_rg_state.dist_to_maneuver_units = u.dist_to_maneuver_units;
    if (u.present & RGD_UPD_LANE_SHOWING)         g_rg_state.lane_guidance_showing  = u.lane_guidance_showing;
    if (u.present & RGD_UPD_LANE_INDEX)            g_rg_state.lane_guidance_index    = u.lane_guidance_index;

    /* Track the maneuver_list — which iAP indexes are the upcoming turns, in order.
     * list[0] is the CURRENT next turn. As the driver passes maneuvers, iPhone
     * advances the list (drops the head). */
    if (u.present & RGD_UPD_MANEUVER_LIST) {
        uint16_t n = u.maneuver_list_count;
        if (n > MAX_MANEUVER_LIST) n = MAX_MANEUVER_LIST;
        memcpy(g_rg_state.maneuver_list, u.maneuver_list, (size_t)n * sizeof(uint16_t));
        g_rg_state.maneuver_list_count = n;
        g_rg_state.have_maneuver_list = 1;
    }
    if (u.present & RGD_UPD_DESTINATION) {
        strncpy(g_rg_state.destination, u.destination, sizeof(g_rg_state.destination) - 1);
        g_rg_state.destination[sizeof(g_rg_state.destination) - 1] = '\0';
    }
    if (u.present & RGD_UPD_CURRENT_ROAD) {
        strncpy(g_rg_state.current_road, u.current_road, sizeof(g_rg_state.current_road) - 1);
        g_rg_state.current_road[sizeof(g_rg_state.current_road) - 1] = '\0';
    }
    if (u.present & RGD_UPD_SOURCE_NAME) {
        strncpy(g_rg_state.source_name, u.source_name, sizeof(g_rg_state.source_name) - 1);
        g_rg_state.source_name[sizeof(g_rg_state.source_name) - 1] = '\0';
    }
    pps_publish_state();
}

/* Called on 0x5202 RouteGuidanceManeuverUpdate.
 * Stage 2: cache every maneuver by iAP index in a slot. pps_publish_state()
 * then publishes all populated slots + which slot is the current turn based
 * on the 0x5201 maneuver_list. */
static void rg_on_maneuver(const uint8_t *msg_payload, size_t msg_len) {
    if (msg_len < 8) return;
    slots_init_once();
    rgd_maneuver_t m;
    memset(&m, 0, sizeof(m));
    rgd_parse_maneuver(msg_payload + 6, msg_len - 6, &m);
    if (!(m.present & RGD_MAN_INDEX)) return;

    int si = slot_allocate(m.index);
    if (si < 0) return;
    man_slot_t *s = &g_slots[si];
    s->last_seq = ++g_slot_seq_counter;
    if (m.present & RGD_MAN_TYPE)             s->type             = m.maneuver_type;
    if (m.present & RGD_MAN_DRIVING_SIDE)     s->driving_side     = m.driving_side;
    if (m.present & RGD_MAN_JUNCTION_TYPE)    s->junction_type    = m.junction_type;
    if (m.present & RGD_MAN_EXIT_ANGLE)       s->exit_angle       = m.exit_angle;
    if (m.present & RGD_MAN_DISTANCE_BETWEEN) s->distance_between = m.distance_between;
    if (m.present & RGD_MAN_DESCRIPTION) {
        strncpy(s->description, m.description, sizeof(s->description) - 1);
        s->description[sizeof(s->description) - 1] = '\0';
    }
    if (m.present & RGD_MAN_AFTER_ROAD) {
        strncpy(s->after_road, m.after_road_name, sizeof(s->after_road) - 1);
        s->after_road[sizeof(s->after_road) - 1] = '\0';
    }
    if (m.present & RGD_MAN_JUNCTION_ANGLES) {
        uint16_t nj = m.junction_angle_count;
        if (nj > MAX_MANEUVER_LIST) nj = MAX_MANEUVER_LIST;
        memcpy(s->junction_angles, m.junction_angles, (size_t)nj * sizeof(int16_t));
        s->junction_angle_count = nj;
    } else {
        s->junction_angle_count = 0;
    }
    if (m.present & RGD_MAN_EXIT_INFO_STR) {
        strncpy(s->exit_info, m.exit_info_str, sizeof(s->exit_info) - 1);
        s->exit_info[sizeof(s->exit_info) - 1] = '\0';
    } else {
        s->exit_info[0] = '\0';
    }
    pps_publish_state();
}

/* Called on 0x5204 RouteGuidanceLaneGuidanceInformation.
 * Stage 3: cache lanes BY lane_guidance_index. Apple/Google use that index
 * as an implicit reference to the matching maneuver iap_idx (no
 * MAN_TLV_LINKED_LANE_GUIDANCE on 5202s in observed runs), so storing the
 * latest 5204 globally collapsed all upcoming maneuvers' lane data onto
 * the last-received one. pps_publish_state now looks up the *current*
 * maneuver's iap_idx in g_lane_sets[]. */
static void rg_on_lane_guidance(const uint8_t *msg_payload, size_t msg_len) {
    if (msg_len < 8) return;
    slots_init_once();
    rgd_lane_guidance_t lg;
    memset(&lg, 0, sizeof(lg));
    rgd_parse_lane_guidance(msg_payload + 6, msg_len - 6, &lg);

    /* Empty lane set for this index means "this maneuver has no lanes" —
     * still record it so we can report lane_active=false when current. */
    int si = lane_set_allocate(lg.lane_guidance_index);
    if (si < 0) { pps_publish_state(); return; }
    lane_set_t *ls = &g_lane_sets[si];
    ls->last_seq = ++g_lane_seq_counter;
    uint8_t n = lg.lane_count;
    if (n > MAX_LANE_GUIDANCE) n = MAX_LANE_GUIDANCE;
    ls->lane_count = n;
    uint8_t i;
    for (i = 0; i < n; i++) {
        ls->lane_pos[i]    = lg.lanes[i].position;
        ls->lane_dir[i]    = lg.lanes[i].direction;
        ls->lane_status[i] = lg.lanes[i].status;
        uint8_t ac = lg.lanes[i].angle_count;
        if (ac > MAX_LANE_ANGLES) ac = MAX_LANE_ANGLES;
        ls->lane_angle_count[i] = ac;
        for (uint8_t k = 0; k < ac; k++)
            ls->lane_angles[i][k] = lg.lanes[i].angles[k];
    }
    pps_publish_state();
}

static void rg_pps_init(void) {
    pps_config_t cfg = PPS_CONFIG_DEFAULT;
    cfg.path          = PPS_RG_PATH;
    cfg.object_name   = PPS_RG_OBJECT;
    cfg.mode          = PPS_MODE_TEXT;
    cfg.create_dirs   = true;
    cfg.keep_open     = true;
    cfg.truncate_on_write = true;
    g_pps = pps_open(&cfg);
    if (g_pps) {
        log_msg("PPS: opened %s", PPS_RG_PATH);
    } else {
        log_msg("PPS: FAILED to open %s", PPS_RG_PATH);
    }
}

/* write_be16 / read_be16 come from Luka's common.h (via rgd_tlv.h). */

static size_t build_rg_component_tlv(uint8_t *out, size_t max_len);

/* Rebuild the Identify message, extending existing 0x0006/0x0007 TLVs in place
 * with extra msgids. Apple rejects duplicate occurrences of 0x0006 or 0x0007
 * (test 20260419_194655: 0x1D03 payload {tag=0006}{tag=0007}), so we must
 * merge into the existing TLVs rather than append new ones.
 *
 * Extends 0x0006 with: 0x5200, 0x5203 (accessory-sent RG msgs)
 * Extends 0x0007 with: 0x5201, 0x5202, 0x5204 (accessory-received RG msgs)
 *
 * in/in_len: original 40 40 LL LL 1D 01 <tlvs> message.
 * out/out_max: destination buffer.
 * Returns new message length (including 40 40 header and updated length field),
 * or 0 on failure. */
/* v15: drop 0xFFFB/0xFFFA/0xFFFC from Identify patch.
 * iap2connectionmanager has no Location module registered. Advertising
 * Location caused phone to send 0xFFFA StartLocationInformation which the
 * library then couldn't route (no handler) and SIGSEGV'd in the control
 * session dispatch path. Location must only be advertised in a process
 * that actually has the Location module wired (dio_manager). */
static const uint16_t RG_SENT_MSGIDS[] = { 0x5200, 0x5203 };
static const uint16_t RG_RECV_MSGIDS[] = { 0x5201, 0x5202, 0x5204 };

static size_t rebuild_identify_with_rg(const uint8_t *in, size_t in_len,
                                       uint8_t *out, size_t out_max) {
    if (in_len < 6 || out_max < in_len + 256) return 0;
    if (in[0] != 0x40 || in[1] != 0x40) return 0;

    /* Copy 40 40 LL LL MM MM header (length fixed up at end). */
    memcpy(out, in, 6);
    size_t oo = 6;
    size_t io = 6;

    while (io + 4 <= in_len) {
        uint16_t tlv_len = (uint16_t)((in[io] << 8) | in[io+1]);
        uint16_t tlv_tag = (uint16_t)((in[io+2] << 8) | in[io+3]);
        if (tlv_len < 4 || io + tlv_len > in_len) return 0;

        if (tlv_tag == 0x0006 || tlv_tag == 0x0007) {
            const uint16_t *extra = (tlv_tag == 0x0006) ? RG_SENT_MSGIDS : RG_RECV_MSGIDS;
            size_t extra_n = (tlv_tag == 0x0006) ? (sizeof(RG_SENT_MSGIDS)/2)
                                                 : (sizeof(RG_RECV_MSGIDS)/2);
            size_t new_tlv_len = tlv_len + 2 * extra_n;
            if (oo + new_tlv_len > out_max) return 0;
            write_be16(out + oo, (uint16_t)new_tlv_len);
            write_be16(out + oo + 2, tlv_tag);
            /* Copy original msgid payload (skip 4-byte TLV header). */
            memcpy(out + oo + 4, in + io + 4, tlv_len - 4);
            /* Append new msgids (big-endian). */
            size_t eoff = oo + tlv_len;
            size_t i;
            for (i = 0; i < extra_n; i++) {
                write_be16(out + eoff + 2*i, extra[i]);
            }
            oo += new_tlv_len;
        } else {
            if (oo + tlv_len > out_max) return 0;
            memcpy(out + oo, in + io, tlv_len);
            oo += tlv_len;
        }
        io += tlv_len;
    }

    /* Append 0x001E RouteGuidanceDisplayComponent TLV. */
    size_t rg_len = build_rg_component_tlv(out + oo, out_max - oo);
    if (rg_len == 0) return 0;
    oo += rg_len;

    /* Fix up top-level length field (bytes 2-3). */
    write_be16(out + 2, (uint16_t)oo);
    return oo;
}

/*
 * Build the RouteGuidanceDisplayComponent TLV for Identify patching.
 * Matches MHI3 libesoiap2.so layout (9 inner TLVs).
 * Returns bytes written, or 0 on failure.
 */
static size_t build_rg_component_tlv(uint8_t *out, size_t max_len) {
    const char *name = "RouteGuidanceDisplayComponent";
    size_t name_len = strlen(name) + 1;  /* include null terminator */
    size_t inner_len = 6 + (4 + name_len) + 6*7;  /* id + name + 7 uint16 TLVs */
    size_t outer_len = 4 + inner_len;

    if (max_len < outer_len) return 0;

    size_t off = 0;
    /* Outer header */
    write_be16(out + off, (uint16_t)outer_len); off += 2;
    write_be16(out + off, IDENT_TLV_RG_COMPONENT); off += 2;

    /* 0x0000 Identifier */
    write_be16(out + off, 6); off += 2;
    write_be16(out + off, 0x0000); off += 2;
    write_be16(out + off, RGD_COMPONENT_ID); off += 2;

    /* 0x0001 Name */
    write_be16(out + off, (uint16_t)(4 + name_len)); off += 2;
    write_be16(out + off, 0x0001); off += 2;
    memcpy(out + off, name, name_len); off += name_len;

    /* 0x0002 MaxCurrentRoadNameLength = 256 */
    write_be16(out + off, 6); off += 2;
    write_be16(out + off, 0x0002); off += 2;
    write_be16(out + off, 0x0100); off += 2;

    /* 0x0003 MaxDestinationNameLength = 256 */
    write_be16(out + off, 6); off += 2;
    write_be16(out + off, 0x0003); off += 2;
    write_be16(out + off, 0x0100); off += 2;

    /* 0x0004 MaxAfterManeuverRoadNameLength = 256 */
    write_be16(out + off, 6); off += 2;
    write_be16(out + off, 0x0004); off += 2;
    write_be16(out + off, 0x0100); off += 2;

    /* 0x0005 MaxManeuverDescriptionLength = 256 */
    write_be16(out + off, 6); off += 2;
    write_be16(out + off, 0x0005); off += 2;
    write_be16(out + off, 0x0100); off += 2;

    /* 0x0006 MaxGuidanceManeuverStorageCapacity = 6 */
    write_be16(out + off, 6); off += 2;
    write_be16(out + off, 0x0006); off += 2;
    write_be16(out + off, 0x0006); off += 2;

    /* 0x0007 MaxLaneGuidanceDescriptionLength = 256 */
    write_be16(out + off, 6); off += 2;
    write_be16(out + off, 0x0007); off += 2;
    write_be16(out + off, 0x0100); off += 2;

    /* 0x0008 MaxLaneGuidanceStorageCapacity = 6 */
    write_be16(out + off, 6); off += 2;
    write_be16(out + off, 0x0008); off += 2;
    write_be16(out + off, 0x0006); off += 2;

    return off;
}

/* Patched identify buffer — persists for the session */
static uint8_t *g_patched_ident = NULL;
static size_t   g_patched_ident_len = 0;

/* Captured pointers for injection */
static void *g_ident_module = NULL;
static void *g_link_instance = NULL;  /* CIAP2Link* captured from sendToSession */
static uint8_t g_session_id = 1;     /* Control session id=1. Observed in every incoming
                                        packet. Using 0 causes phone to drop our packet.
                                        Safe here because we inject only post-IdentifyAccepted
                                        when link_state==3 (deployCustomPacket's session!=0
                                        branch requires link_state==3 or crashes). */
static volatile int g_identify_accepted = 0;
static volatile int g_rg_requested = 0;

/*
 * CIAP2Link::sendToSession — hook to capture CIAP2Link instance.
 */
typedef int (*fn_sendToSession_t)(void *self, const void *packetView);
static fn_sendToSession_t real_sendToSession = NULL;

/*
 * CIAP2Packet constructor from std::vector<uint8_t>&
 * and related methods for building outgoing packets.
 */
typedef void (*fn_packet_ctor_vec_t)(void *self, const void *vec);
typedef void (*fn_packet_set_session_t)(void *self, uint8_t sid);
typedef void (*fn_packet_update_cksum_t)(void *self);
typedef void (*fn_packet_dtor_t)(void *self);
typedef int  (*fn_deploy_packet_t)(void *link, void *packet, uint8_t sid);

/* dio_manager_preload: the clean inject path uses the library's own
 * CIAP2ControlSessionMessageView + CIAP2ControlSession::deployMessage. */
typedef void (*fn_msgview_ctor_iter_t)(void *self, const uint8_t *begin, const uint8_t *end);

static fn_packet_ctor_vec_t     fn_packet_ctor = NULL;
static fn_packet_set_session_t  fn_packet_set_session = NULL;
static fn_packet_update_cksum_t fn_packet_update_cksum = NULL;
static fn_packet_dtor_t         fn_packet_dtor = NULL;
static fn_deploy_packet_t       fn_deploy_packet = NULL;
static fn_msgview_ctor_iter_t   fn_msgview_ctor_iter = NULL;

/* CIAP2ControlSession* captured from the deployMessage hook (self arg).
 * We use this to call real_deployMessage(session, &our_view) when injecting
 * 0x5200 via the proper library path. */
static void *g_control_session = NULL;

/*
 * Build 0x5200 (StartRouteGuidanceUpdate) raw iAP2 frame.
 * Contains: component ID + presence TLVs for sourceName, sourceSupportsRG, exitInfo.
 */
static size_t build_start_rg_frame(uint8_t *out, size_t max_len) {
    if (max_len < 40) return 0;

    /* Build payload TLVs */
    uint8_t payload[32];
    size_t poff = 0;

    /* 0x0000 ComponentIdentifier (uint16) */
    write_be16(payload + poff, 6); poff += 2;
    write_be16(payload + poff, 0x0000); poff += 2;
    write_be16(payload + poff, RGD_COMPONENT_ID); poff += 2;

    /* 0x0001 SourceName (presence TLV, empty) */
    write_be16(payload + poff, 4); poff += 2;
    write_be16(payload + poff, 0x0001); poff += 2;

    /* 0x0002 SourceSupportsRouteGuidance (presence TLV) */
    write_be16(payload + poff, 4); poff += 2;
    write_be16(payload + poff, 0x0002); poff += 2;

    /* 0x0003 SupportsExitInfo (presence TLV) */
    write_be16(payload + poff, 4); poff += 2;
    write_be16(payload + poff, 0x0003); poff += 2;

    /* Build frame: marker + length + msgid + payload */
    size_t frame_len = 6 + poff;
    if (max_len < frame_len) return 0;

    out[0] = 0x40;
    out[1] = 0x40;
    write_be16(out + 2, (uint16_t)frame_len);
    write_be16(out + 4, IAP2_MSG_RG_START);  /* 0x5200 */
    memcpy(out + 6, payload, poff);

    return frame_len;
}

/*
 * Inject 0x5200 via CIAP2ControlSession::deployMessage(MessageView).
 *
 * Proper library send path. deployMessage internally:
 *   1. view.begin()/end() → iterators over our bytes
 *   2. CIAP2Packet(begin, end) → lib-allocated + copied buffer
 *   3. IIAP2Session::deploy(packet) → CIAP2Link::deployPacket → wire
 * All lifecycle owned by the library. No raw CIAP2Packet on our side.
 *
 * MessageView object layout (from disasm of ctor at libesoiap2+0x325d0):
 *   [0]: vtable (set by ctor from global)
 *   [4]: begin (iterator, plain pointer in Dinkumware)
 *   [8]: end   (iterator, plain pointer)
 * Total 12 bytes, allocated on stack.
 */
static void inject_start_rg(void) {
    if (!g_control_session) {
        log_msg("INJECT: no control session captured yet");
        return;
    }
    if (!real_deployMessage || !fn_msgview_ctor_iter) {
        log_msg("INJECT: symbols missing — deployMessage=%p msgview_ctor=%p",
                (void*)real_deployMessage, (void*)fn_msgview_ctor_iter);
        return;
    }

    /* Build the control-session message bytes on our stack.
     * Lives only during this call; library copies them into a heap buffer
     * inside the deployMessage path via CIAP2Packet::setPayload. */
    uint8_t frame[64];
    size_t frame_len = build_start_rg_frame(frame, sizeof(frame));
    if (frame_len == 0) {
        log_msg("INJECT: build_start_rg_frame failed");
        return;
    }

    log_msg("INJECT: 0x5200 via deployMessage (session=%p len=%zu)",
            g_control_session, frame_len);
    log_hex("0x5200 frame", frame, frame_len);

    /* Construct a real MessageView via the library ctor.
     * Dinkumware const_iterator<uint8_t> is a 4-byte pointer wrapper,
     * so we pass raw uint8_t* — verified by disassembly of the ctor which
     * does [view+4]=r1, [view+8]=r2 directly. */
    uint8_t view[16];   /* 12 actual bytes + slack */
    memset(view, 0, sizeof(view));
    fn_msgview_ctor_iter(view, frame, frame + frame_len);

    /* Call the library's proper deploy path. All packet lifecycle is
     * library-owned from here; we don't touch CIAP2Packet at all. */
    int ret = real_deployMessage(g_control_session, view);
    log_msg("INJECT: deployMessage returned %d", ret);

    /* MessageView has a trivial destructor (no heap), stack cleanup is fine. */
    g_rg_requested = 1;
}

/* ------------------------------------------------------------------ */
/* CIAP2ControlSessionMessageView — layout from disassembly            */
/* ------------------------------------------------------------------ */

/*
 * CIAP2ControlSessionMessageView (from libesoiap2.so disassembly):
 *   [0] = vtable
 *   [4] = begin ptr (raw iAP2 control session message bytes)
 *   [8] = end ptr
 *
 * Raw message format: 0x40 0x40 LEN_HI LEN_LO MSGID_HI MSGID_LO [payload...]
 */
static uint16_t get_msgid(const void *msgView) {
    if (!msgView) return 0;
    const uint32_t *view = (const uint32_t *)msgView;
    const uint8_t *begin = (const uint8_t *)view[1];  /* offset +4 */
    const uint8_t *end   = (const uint8_t *)view[2];  /* offset +8 */
    if (!begin || !end || end <= begin) return 0;
    size_t len = (size_t)(end - begin);
    if (len < 6) return 0;
    /* Verify iAP2 marker */
    if (begin[0] != 0x40 || begin[1] != 0x40) {
        /* No marker — msgid might be at offset 0 directly */
        return (uint16_t)((begin[0] << 8) | begin[1]);
    }
    return (uint16_t)((begin[4] << 8) | begin[5]);
}

static void dump_raw(const char *label, const void *msgView) {
    if (!msgView) return;
    const uint32_t *view = (const uint32_t *)msgView;
    const uint8_t *begin = (const uint8_t *)view[1];
    const uint8_t *end   = (const uint8_t *)view[2];
    if (!begin || !end || end <= begin) return;
    size_t len = (size_t)(end - begin);
    char tag[32];
    snprintf(tag, sizeof(tag), "%s raw", label);
    log_hex(tag, begin, len);
}

/* ------------------------------------------------------------------ */
/* Hooked: CIAP2ControlSession::deployMessage                         */
/* ------------------------------------------------------------------ */
int _ZN4iap219CIAP2ControlSession13deployMessageERKNS_30CIAP2ControlSessionMessageViewE(
    void *self, const void *msgView)
{
    if (!real_deployMessage) {
        real_deployMessage = (fn_deployMessage_t)dlsym(RTLD_NEXT,
            "_ZN4iap219CIAP2ControlSession13deployMessageERKNS_30CIAP2ControlSessionMessageViewE");
    }
    if (!is_enabled()) return real_deployMessage ? real_deployMessage(self, msgView) : -1;

    /* Capture the CIAP2ControlSession* so we can drive our own deployMessage
     * call later. It's the "this" pointer of every outgoing control-session
     * message — stable for the lifetime of the session. */
    if (!g_control_session) {
        g_control_session = self;
        log_msg("deployMessage: captured CIAP2ControlSession=%p", self);
    }

    uint32_t n = ++g_deploy_count;
    uint16_t msgid = get_msgid(msgView);
    const char *name = msgid_name(msgid);
    int do_log = (n <= 20 || name != NULL || (n % 500) == 0);

    if (do_log) {
        if (name)
            log_msg("deployMessage[%u]: msgid=0x%04x (%s)", n, msgid, name);
        else
            log_msg("deployMessage[%u]: msgid=0x%04x", n, msgid);
    }

    /* Dump first few messages and any 0x52xx for analysis */
    if (n <= 5 || (msgid >= 0x5200 && msgid <= 0x52FF)) {
        dump_raw("deploy", msgView);
    }

    /* Inject 0x5200 after first outgoing 0xFFFB LocationInformation from
     * the HU. That's the same trigger Luka uses on MIB2Q — it means the
     * iAP2 session is fully established, phone has subscribed to Location,
     * and HU is actively streaming GPS. In dio_manager we see 0xFFFB ~1 Hz
     * starting right after phone's 0xFFFA request. (The old 0x5703 trigger
     * was for iap2connectionmanager's wired-CarPlay bootstrap, never fires
     * in dio_manager's wireless-CarPlay session.) */
    if (msgid == 0xFFFB && g_identify_accepted && !g_rg_requested) {
        inject_start_rg();
    }

    /* Patch Identify response (0x1D01) — merge RG msgids and add RG component.
     * Patch every time: iPhone runs a full auth+identify cycle multiple times
     * per session (observed ~47s apart). Each cycle resets phone's view of
     * our capabilities, so we must re-patch. */
    if (msgid == 0x1D01) {
        uint32_t *view = (uint32_t *)msgView;
        uint8_t *begin = (uint8_t *)view[1];
        uint8_t *end   = (uint8_t *)view[2];

        /* Free previous patched buffer before allocating new one */
        if (g_patched_ident) {
            free(g_patched_ident);
            g_patched_ident = NULL;
            g_patched_ident_len = 0;
        }

        if (begin && end && end > begin) {
            size_t orig_len = (size_t)(end - begin);

            log_tlv_tags("IDENTIFY orig", begin, orig_len);

            size_t out_cap = orig_len + 512;
            g_patched_ident = (uint8_t *)malloc(out_cap);
            if (g_patched_ident) {
                size_t new_len = rebuild_identify_with_rg(begin, orig_len,
                                                          g_patched_ident, out_cap);
                if (new_len > 0) {
                    g_patched_ident_len = new_len;
                    view[1] = (uint32_t)g_patched_ident;
                    view[2] = (uint32_t)(g_patched_ident + new_len);

                    log_msg("IDENTIFY PATCHED: %zu -> %zu bytes (merged 0006/0007 + 001E)",
                            orig_len, new_len);
                    log_tlv_tags("IDENTIFY new", g_patched_ident, new_len);
                    log_hex("IDENTIFY new raw", g_patched_ident, new_len);
                } else {
                    log_msg("IDENTIFY REBUILD FAILED: orig_len=%zu", orig_len);
                    free(g_patched_ident);
                    g_patched_ident = NULL;
                }
            } else {
                log_msg("IDENTIFY PATCH FAILED: malloc(%zu)", out_cap);
            }
        }
    }

    if (!real_deployMessage) return -1;
    return real_deployMessage(self, msgView);
}

/* ------------------------------------------------------------------ */
/* Hooked: CIAP2ControlSessionModuleIdent::identificationInformation  */
/* ------------------------------------------------------------------ */
int _ZN4iap230CIAP2ControlSessionModuleIdent25identificationInformationERKNS_8messages14identification42CMessageParameterIdentificationInformationE(
    void *self, const void *identParam)
{
    if (!real_identInfo) {
        real_identInfo = (fn_identInfo_t)dlsym(RTLD_NEXT,
            "_ZN4iap230CIAP2ControlSessionModuleIdent25identificationInformationERKNS_8messages14identification42CMessageParameterIdentificationInformationE");
    }
    if (!is_enabled()) return real_identInfo ? real_identInfo(self, identParam) : -1;

    uint32_t n = ++g_ident_count;
    g_ident_module = self;  /* capture for injection later */
    log_msg("identificationInformation[%u]: self=%p param=%p", n, self, identParam);

    /* Dump the ident param struct to understand its layout */
    if (identParam) {
        const uint32_t *words = (const uint32_t *)identParam;
        int i;
        log_msg("  identParam words:");
        for (i = 0; i < 32; i += 4)
            log_msg("    [%2d..%2d] %08x %08x %08x %08x",
                    i, i+3, words[i], words[i+1], words[i+2], words[i+3]);
    }

    if (!real_identInfo) return -1;
    int ret = real_identInfo(self, identParam);
    log_msg("identificationInformation[%u]: ret=%d", n, ret);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Hooked: CIAP2Link::sendToSession — capture link instance            */
/* ------------------------------------------------------------------ */
int _ZN4iap29CIAP2Link13sendToSessionERKNS_15CIAP2PacketViewE(
    void *self, const void *packetView)
{
    if (!real_sendToSession) {
        real_sendToSession = (fn_sendToSession_t)dlsym(RTLD_NEXT,
            "_ZN4iap29CIAP2Link13sendToSessionERKNS_15CIAP2PacketViewE");
    }

    /* Always capture link instance — even when disabled */
    if (self && !g_link_instance) {
        g_link_instance = self;
        if (is_enabled())
            log_msg("sendToSession: captured CIAP2Link=%p", self);
    }

    /* Log INCOMING message ids — capture iPhone's responses to our injected 0x5200.
     * packetView layout: [0]=vtable, [4]=data ptr.
     * Packet buffer: [0..1]=FF 5A start, [4]=ctrl byte, [7]=sessionId, [9..]=payload.
     * Payload starts with iAP2 ControlSessionMessage: 40 40 LL LL MM MM ... */
    static uint32_t sts_count = 0;
    if (is_enabled() && packetView) {
        const uint32_t *pv = (const uint32_t *)packetView;
        const uint8_t *data = (const uint8_t *)pv[1];
        uint32_t n = ++sts_count;
        if (data) {
            uint8_t sid = data[7];
            /* Payload starts at data[9]. Check for 40 40 marker then msgid at +4. */
            uint16_t in_msgid = 0;
            if (data[9] == 0x40 && data[10] == 0x40) {
                in_msgid = (uint16_t)((data[13] << 8) | data[14]);
            }
            const char *in_name = msgid_name(in_msgid);
            /* Always log 0x52xx (RG) + first 30 + periodic sampling. */
            int do_log = (n <= 30) || (in_msgid >= 0x5200 && in_msgid <= 0x52FF) || (n % 200) == 0;
            if (do_log) {
                if (in_name)
                    log_msg("sendToSession[%u]: sid=%u msgid=0x%04x (%s)", n, sid, in_msgid, in_name);
                else
                    log_msg("sendToSession[%u]: sid=%u msgid=0x%04x", n, sid, in_msgid);
            }
            /* Payload dumping policy:
             *   - RG messages (0x5200..0x52FF): ALWAYS dump — these are the
             *     live turn-by-turn data we want to parse for the cluster.
             *   - 0x4E**, 0x5702, 0xFFFA: first-seen only — setup chatter,
             *     content doesn't change across occurrences; one dump is
             *     enough to know what's in them.
             * Skip the first-seen gate for RG so ongoing updates flow into
             * the log. */
            const uint8_t *payload = data + 9;
            uint16_t plen = (uint16_t)((payload[2] << 8) | payload[3]);
            int is_rg    = (in_msgid >= 0x5200 && in_msgid <= 0x52FF);
            int is_setup = ((in_msgid & 0xFF00) == 0x4E00)
                        || in_msgid == 0x5702
                        || in_msgid == 0xFFFA;
            if (is_rg) {
                if (plen >= 6 && plen <= 2048) {
                    log_hex("  in_payload (RG)", payload, plen);
                    /* Parse + publish to PPS. 0x5201/0x5202 (Stage 1/2),
                     * 0x5204 (Stage 3 — lane guidance). */
                    if (in_msgid == 0x5201)      rg_on_update(payload, plen);
                    else if (in_msgid == 0x5202) rg_on_maneuver(payload, plen);
                    else if (in_msgid == 0x5204) rg_on_lane_guidance(payload, plen);
                }
            } else if (is_setup) {
                static uint8_t seen_bitmap[8192];
                uint32_t idx = (uint32_t)in_msgid;
                uint8_t mask = (uint8_t)(1u << (idx & 7));
                if (!(seen_bitmap[idx >> 3] & mask)) {
                    seen_bitmap[idx >> 3] |= mask;
                    if (plen >= 6 && plen <= 2048) {
                        log_hex("  in_payload (first-seen)", payload, plen);
                    }
                }
            }
        }
    }
    /* Keep g_session_id = 0 — session=1 crashes deployCustomPacket */

    if (!real_sendToSession) return -1;
    return real_sendToSession(self, packetView);
}

/* ------------------------------------------------------------------ */
/* Hooked: CIAP2ControlSessionModuleIdent::process                    */
/* ------------------------------------------------------------------ */
int _ZN4iap230CIAP2ControlSessionModuleIdent7processERKNS_30CIAP2ControlSessionMessageViewE(
    void *self, const void *msgView)
{
    if (!real_identProcess) {
        real_identProcess = (fn_identProcess_t)dlsym(RTLD_NEXT,
            "_ZN4iap230CIAP2ControlSessionModuleIdent7processERKNS_30CIAP2ControlSessionMessageViewE");
    }
    if (!is_enabled()) return real_identProcess ? real_identProcess(self, msgView) : -1;

    uint16_t msgid = get_msgid(msgView);
    log_msg("Ident::process: msgid=0x%04x (%s)", msgid,
            msgid == IAP2_MSG_IDENTIFY_ACCEPTED ? "ACCEPTED" :
            msgid == IAP2_MSG_IDENTIFY_REJECTED ? "REJECTED" : "?");
    dump_raw("ident", msgView);

    if (!real_identProcess) return -1;
    int ret = real_identProcess(self, msgView);

    /* After Identify accepted, mark state. Actual 0x5200 injection is deferred
     * until HU sends its 0x5703 WiFi config response — that's the "session
     * fully set up" signal on MH2P (after phone finishes sending 0x4e0c/5702/
     * 4e0d/4e0e/4e09). MIB2Q uses 0xFFFB for the same purpose; MH2P doesn't
     * emit 0xFFFB so 0x5703 outgoing is our equivalent. */
    if (msgid == IAP2_MSG_IDENTIFY_ACCEPTED) {
        g_identify_accepted = 1;
        g_rg_requested = 0;  /* re-arm for re-auth cycles */
        if (!g_ident_module) g_ident_module = self;
    }

    return ret;
}

/* v14: CIAP2ControlSession::process hook removed. See declaration comment. */

/* ------------------------------------------------------------------ */
/* Constructor / Destructor                                            */
/* ------------------------------------------------------------------ */
__attribute__((constructor))
static void on_load(void) {
    /* Resolve env-var gate before any log call below. */
    log_init_from_env();

    /* Clean deployMessage-based inject path (proper library API). */
    fn_msgview_ctor_iter = (fn_msgview_ctor_iter_t)dlsym(RTLD_DEFAULT,
        "_ZN4iap230CIAP2ControlSessionMessageViewC1ESt22_Vector_const_iteratorISt11_Vector_valISt13_Simple_typesIhEEES6_");

    /* Legacy raw-packet path kept resolved for diagnostic fallbacks only;
     * not used in the new inject_start_rg(). */
    fn_deploy_packet = (fn_deploy_packet_t)dlsym(RTLD_DEFAULT,
        "_ZN4iap29CIAP2Link12deployPacketENS_11CIAP2PacketEh");
    fn_packet_ctor = (fn_packet_ctor_vec_t)dlsym(RTLD_DEFAULT,
        "_ZN4iap211CIAP2PacketC1ERKSt6vectorIhSaIhEE");
    fn_packet_set_session = (fn_packet_set_session_t)dlsym(RTLD_DEFAULT,
        "_ZN4iap211CIAP2Packet12setSessionIdEh");
    fn_packet_update_cksum = (fn_packet_update_cksum_t)dlsym(RTLD_DEFAULT,
        "_ZN4iap211CIAP2Packet20updateHeaderChecksumEv");
    fn_packet_dtor = (fn_packet_dtor_t)dlsym(RTLD_DEFAULT,
        "_ZN4iap211CIAP2PacketD1Ev");

    log_msg("=== dio_manager_preload v1 loaded pid=%d ===", (int)getpid());
    rg_pps_init();
    log_msg("  MessageView ctor = %p  (inject path via deployMessage)", (void*)fn_msgview_ctor_iter);
    log_msg("  deployPacket = %p  (legacy, unused)", (void*)fn_deploy_packet);
    log_msg("  CIAP2Packet ctor = %p  setSession = %p  updateCksum = %p  dtor = %p",
            (void*)fn_packet_ctor, (void*)fn_packet_set_session,
            (void*)fn_packet_update_cksum, (void*)fn_packet_dtor);
}

__attribute__((destructor))
static void on_unload(void) {
    log_msg("=== iAP2 RGD Hook v1 unloaded (deploy=%u ident=%u) ===",
            g_deploy_count, g_ident_count);
    if (g_log) { fclose(g_log); g_log = NULL; }
}
