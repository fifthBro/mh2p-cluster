/*
 * Audi AA secondary-display rotary sender.
 *
 * This helper deliberately interposes NO GAL/libautoreceiver symbols.  It is
 * loaded after the exact stable cc3e388 gal_cluster.so and calls that hook's
 * already-exported queueOutgoingUnencrypted implementation.  The stable hook
 * stores its live MessageRouter* at module-relative offset 0x113f0.
 *
 * Exact supported stable hook:
 *   size   82104 bytes
 *   sha256 1E173663791BC58B4D73ED138FFA574990391CB74B6C4B15AED01A9F0CA0C7A5
 *   sendChannelOpenResp            +0x0000b0ac
 *   queueOutgoingUnencrypted       +0x0000a274
 *   g_router_ptr slot              +0x000113f0
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CONTROL_FILE "/tmp/aa_cluster_rotary_ctl"
#define LOG_FILE     "/tmp/gal_cluster_rotary_nohook.log"
#define INPUT_CHANNEL 21

#define STABLE_SENDRESP_OFF  0x0000b0acU
#define STABLE_QUEUE_RAW_OFF 0x0000a274U
#define STABLE_ROUTER_OFF    0x000113f0U

typedef void (*queue_raw_fn)(void *, uint8_t, void *, uint32_t);

static uintptr_t stable_base;
static void *volatile *router_slot;
static queue_raw_fn queue_raw;
static int compatible;

static void append_log(const char *fmt, ...)
{
    char line[384];
    va_list ap;
    int n, fd;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
    fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) return;
    (void)write(fd, line, (size_t)n);
    close(fd);
}

static size_t put_varint(uint8_t *out, uint64_t value)
{
    size_t n = 0;
    do {
        uint8_t b = (uint8_t)(value & 0x7fU);
        value >>= 7;
        if (value) b |= 0x80U;
        out[n++] = b;
    } while (value);
    return n;
}

static uint64_t monotonic_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000ULL
         + (uint64_t)ts.tv_nsec / 1000ULL;
}

static int resolve_stable(void)
{
    void *sendresp = dlsym(RTLD_DEFAULT,
        "_ZN13MessageRouter19sendChannelOpenRespEhi");
    void *raw = dlsym(RTLD_DEFAULT,
        "_ZN13MessageRouter24queueOutgoingUnencryptedEhPvj");
    uintptr_t send_addr;
    uintptr_t raw_addr;

    if (!sendresp || !raw) {
        append_log("disabled: stable symbols missing send=%p raw=%p\n",
                   sendresp, raw);
        return 0;
    }

    /* Function pointers may carry the Thumb bit. */
    send_addr = ((uintptr_t)sendresp) & ~(uintptr_t)1U;
    raw_addr = ((uintptr_t)raw) & ~(uintptr_t)1U;
    stable_base = send_addr - STABLE_SENDRESP_OFF;

    /* Refuse to operate with any hook other than the analyzed cc3e388 ELF. */
    if (raw_addr != stable_base + STABLE_QUEUE_RAW_OFF) {
        append_log("disabled: incompatible offsets base=%p send=%p raw=%p expected=%p\n",
                   (void *)stable_base, sendresp, raw,
                   (void *)(stable_base + STABLE_QUEUE_RAW_OFF));
        return 0;
    }

    router_slot = (void *volatile *)(stable_base + STABLE_ROUTER_OFF);
    queue_raw = (queue_raw_fn)raw;
    append_log("compatible: base=%p router_slot=%p queue_raw=%p\n",
               (void *)stable_base, (void *)router_slot, raw);
    return 1;
}

/* InputEventIndication (0x8001):
 *   field 1: timestamp_us
 *   field 6: RelativeInputEvents
 *     field 1: RelativeInputEvent
 *       field 1: scan_code = 65536 (ROTARY_CONTROLLER)
 *       field 2: delta = +1/-1
 */
static int send_rotary(int delta)
{
    uint8_t event[24], events[32], msg[64];
    size_t en = 0, rn = 0, mn = 0;
    void *router;
    uint8_t *owned;

    if (!compatible || !router_slot || !queue_raw) return -1;
    router = *router_slot;
    if (!router) {
        append_log("ignored delta=%d: MessageRouter not ready\n", delta);
        return -1;
    }

    event[en++] = 0x08;
    en += put_varint(event + en, 65536U);
    event[en++] = 0x10;
    en += put_varint(event + en, (uint64_t)(int64_t)delta);

    events[rn++] = 0x0a;
    rn += put_varint(events + rn, en);
    memcpy(events + rn, event, en);
    rn += en;

    msg[mn++] = 0x80;
    msg[mn++] = 0x01;
    msg[mn++] = 0x08;
    mn += put_varint(msg + mn, monotonic_us());
    msg[mn++] = 0x32;
    mn += put_varint(msg + mn, rn);
    memcpy(msg + mn, events, rn);
    mn += rn;

    /* The router owns/frees outgoing message buffers asynchronously. */
    owned = (uint8_t *)malloc(mn);
    if (!owned) return -1;
    memcpy(owned, msg, mn);
    queue_raw(router, INPUT_CHANNEL, owned, (uint32_t)mn);
    append_log("sent delta=%d channel=%d bytes=%u router=%p\n",
               delta, INPUT_CHANNEL, (unsigned)mn, router);
    return 0;
}

static size_t build_location(uint8_t *out, unsigned x, unsigned y,
                             unsigned pointer_id)
{
    uint8_t location[24];
    size_t ln = 0, n = 0;
    location[ln++] = 0x08;
    ln += put_varint(location + ln, x);
    location[ln++] = 0x10;
    ln += put_varint(location + ln, y);
    location[ln++] = 0x18;
    ln += put_varint(location + ln, pointer_id);
    out[n++] = 0x0a;
    n += put_varint(out + n, ln);
    memcpy(out + n, location, ln);
    return n + ln;
}

/* Send one touchscreen MotionEvent to the cluster-bound input channel.
 * This is useful only when the service-discovery sidecar has advertised a
 * TouchScreenConfig for display_id=1. */
static int send_touch(unsigned count, const unsigned *x, const unsigned *y,
                      unsigned action, unsigned action_index)
{
    uint8_t touch[96], msg[128];
    size_t tn = 0, mn = 0;
    unsigned i;
    void *router;
    uint8_t *owned;

    if (!compatible || !router_slot || !queue_raw) return -1;
    router = *router_slot;
    if (!router) return -1;

    for (i = 0; i < count; ++i)
        tn += build_location(touch + tn, x[i], y[i], i + 1U);
    touch[tn++] = 0x10;
    tn += put_varint(touch + tn, action_index);
    touch[tn++] = 0x18;
    tn += put_varint(touch + tn, action);

    msg[mn++] = 0x80;
    msg[mn++] = 0x01;
    msg[mn++] = 0x08;
    mn += put_varint(msg + mn, monotonic_us());
    msg[mn++] = 0x1a; /* InputEventIndication.touch_event, field 3 */
    mn += put_varint(msg + mn, tn);
    memcpy(msg + mn, touch, tn);
    mn += tn;

    owned = (uint8_t *)malloc(mn);
    if (!owned) return -1;
    memcpy(owned, msg, mn);
    queue_raw(router, INPUT_CHANNEL, owned, (uint32_t)mn);
    return 0;
}

static int send_pinch(int zoom_in)
{
    unsigned x[2], y[2] = { 540U, 540U };
    int start_half = zoom_in ? 120 : 420;
    int end_half = zoom_in ? 420 : 120;
    int step, rc = 0;

    x[0] = (unsigned)(960 - start_half);
    x[1] = (unsigned)(960 + start_half);
    rc |= send_touch(1, x, y, 0, 0); /* DOWN */
    usleep(25000);
    rc |= send_touch(2, x, y, 5, 1); /* POINTER_DOWN */
    for (step = 1; step <= 8; ++step) {
        int half = start_half + ((end_half - start_half) * step) / 8;
        x[0] = (unsigned)(960 - half);
        x[1] = (unsigned)(960 + half);
        usleep(22000);
        rc |= send_touch(2, x, y, 2, 0); /* MOVE */
    }
    usleep(22000);
    rc |= send_touch(2, x, y, 6, 1); /* POINTER_UP */
    usleep(18000);
    rc |= send_touch(1, x, y, 1, 0); /* UP */
    append_log("sent pinch=%s channel=%d geometry=1920x1080 rc=%d\n",
               zoom_in ? "in" : "out", INPUT_CHANNEL, rc);
    return rc;
}

static void *worker(void *unused)
{
    char last_command[32] = {0};
    (void)unused;
    for (;;) {
        int fd = open(CONTROL_FILE, O_RDONLY);
        if (fd >= 0) {
            char cmd[32];
            int n = read(fd, cmd, sizeof(cmd) - 1);
            close(fd);
            if (n > 0) {
                cmd[n] = '\0';
                /* QNX may keep a just-created command file visible while the
                 * remote shell still has it open.  Execute each complete
                 * command token once, regardless of unlink semantics.  Every
                 * later command must carry a new sequence suffix, e.g.
                 * "delta 8 1", "delta -8 2". */
                if (strncmp(cmd, last_command, sizeof(last_command) - 1) != 0) {
                    int delta = 0;
                    strncpy(last_command, cmd, sizeof(last_command) - 1);
                    last_command[sizeof(last_command) - 1] = '\0';
                    if (!strncmp(cmd, "pinch_in", 8)) {
                        send_pinch(1);
                    } else if (!strncmp(cmd, "pinch_out", 9)) {
                        send_pinch(0);
                    } else if (sscanf(cmd, "delta %d", &delta) == 1) {
                        if (delta > 20) delta = 20;
                        if (delta < -20) delta = -20;
                        if (delta != 0) send_rotary(delta);
                    } else if (!strncmp(cmd, "in", 2)) {
                        send_rotary(1);
                    } else if (!strncmp(cmd, "out", 3)) {
                        send_rotary(-1);
                    } else {
                        append_log("ignored command=%s\n", cmd);
                    }
                }
            }
        }
        usleep(50000);
    }
    return NULL;
}

__attribute__((constructor, visibility("hidden")))
static void rotary_init(void)
{
    pthread_t tid;
    unlink(CONTROL_FILE);
    compatible = resolve_stable();
    if (!compatible) return;
    if (pthread_create(&tid, NULL, worker, NULL) == 0) {
        pthread_detach(tid);
        append_log("worker started pid=%d; passive until control command\n",
                   (int)getpid());
    } else {
        append_log("disabled: worker creation failed\n");
        compatible = 0;
    }
}
