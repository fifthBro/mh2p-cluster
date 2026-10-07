/* Read-only MH2P VC H.264 transport trace. No video bytes are persisted. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SHM_PATH "/dev/shmem/cluster_h264_shm"
#define DECODER_LOG "/tmp/cluster_daemon.log"
#define MAGIC 0x48323634u
#define LOG_LIMIT (2u * 1024u * 1024u)

typedef struct {
    volatile uint32_t magic;
    volatile uint32_t write_pos;
    volatile uint32_t total_bytes;
    volatile uint32_t flags;
    uint8_t ring[1];
} shared_t;

typedef struct {
    uint64_t bytes, samples, overrun_bytes;
    uint32_t max_sample, nal_slice, nal_idr, nal_sps, nal_pps, nal_other;
    uint32_t resets, invalid, overruns;
} counters_t;

static volatile sig_atomic_t running = 1;
static void stop_handler(int sig) { (void)sig; running = 0; }

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void rotate_if_needed(FILE **log, const char *path) {
    char old1[512], old2[512];
    long pos;
    if (!*log) return;
    pos = ftell(*log);
    if (pos < 0 || (unsigned long)pos < LOG_LIMIT) return;
    if (strlen(path) > sizeof(old1) - 3) return;
    snprintf(old1, sizeof(old1), "%s.1", path);
    snprintf(old2, sizeof(old2), "%s.2", path);
    fclose(*log);
    *log = NULL;
    remove(old2);                 /* only this tracer's oldest bounded segment */
    rename(old1, old2);
    rename(path, old1);
    *log = fopen(path, "w");
}

static void count_nal(counters_t *c, uint8_t nal) {
    switch (nal & 31u) {
    case 1: c->nal_slice++; break;
    case 5: c->nal_idr++; break;
    case 7: c->nal_sps++; break;
    case 8: c->nal_pps++; break;
    default: c->nal_other++; break;
    }
}

/* Scan Annex-B start codes across polling boundaries, without storing frames. */
static void scan_bytes(const shared_t *shm, uint32_t ring_size,
        uint32_t start, uint32_t count, uint32_t *shift, int *expect_nal,
        counters_t *interval, counters_t *lifetime) {
    uint32_t i, pos = start;
    for (i = 0; i < count; i++) {
        uint8_t b = shm->ring[pos];
        if (*expect_nal) {
            count_nal(interval, b);
            count_nal(lifetime, b);
            *expect_nal = 0;
        }
        *shift = (*shift << 8) | (uint32_t)b;
        if (*shift == 0x00000001u || (*shift & 0x00ffffffu) == 0x000001u)
            *expect_nal = 1;
        if (++pos == ring_size) pos = 0;
    }
}

static void print_status(FILE *log, uint64_t at_ms, unsigned session,
        uint32_t total, uint32_t wp, uint32_t flags,
        const counters_t *c, uint64_t since_idr_ms) {
    fprintf(log,
        "T ms=%llu session=%u total=%u wp=%u flags=%u bytes=%llu samples=%llu"
        " max_sample=%u slice=%u idr=%u sps=%u pps=%u other=%u"
        " overruns=%u lost_bytes=%llu resets=%u invalid=%u since_idr_ms=%llu\n",
        (unsigned long long)at_ms, session, total, wp, flags,
        (unsigned long long)c->bytes, (unsigned long long)c->samples,
        c->max_sample, c->nal_slice, c->nal_idr, c->nal_sps, c->nal_pps,
        c->nal_other, c->overruns, (unsigned long long)c->overrun_bytes,
        c->resets, c->invalid, (unsigned long long)since_idr_ms);
    fflush(log);
}

static void pump_decoder_log(FILE **reader, FILE *trace, uint64_t now) {
    struct stat old_st, new_st;
    char line[512];
    unsigned copied = 0;
    if (stat(DECODER_LOG, &new_st) != 0) return;
    if (!*reader) {
        *reader = fopen(DECODER_LOG, "r");
        if (!*reader) return;
        fseek(*reader, 0, SEEK_END); /* historical detail remains in original log */
        return;
    }
    if (fstat(fileno(*reader), &old_st) != 0 ||
            old_st.st_dev != new_st.st_dev || old_st.st_ino != new_st.st_ino ||
            ftell(*reader) > new_st.st_size) {
        fclose(*reader);
        *reader = fopen(DECODER_LOG, "r");
        if (!*reader) return;
        fprintf(trace, "DEC_LOG_RESET ms=%llu\n", (unsigned long long)now);
    }
    clearerr(*reader);
    while (fgets(line, sizeof(line), *reader)) {
        if ((strstr(line, "parse rc=") || strstr(line, "DecodePicture: rc=") ||
             strstr(line, "BeginSequence:") || strstr(line, "RING_NEAR_FULL") ||
             strstr(line, "decode loop started") || strstr(line, "decode failed")) &&
             copied++ < 12) {
            line[strcspn(line, "\r\n")] = 0;
            fprintf(trace, "DEC ms=%llu %s\n", (unsigned long long)now, line);
        }
    }
    fflush(trace);
}

int main(int argc, char **argv) {
    const char *log_path = "/mnt/misc1/zhook/vc_frame_trace.log";
    uint64_t deadline = 0, next_log, last_data_ms = 0, last_idr_ms = 0;
    uint32_t last_total = 0, shift = 0, ring_size = 0;
    unsigned session = 0;
    int expect_nal = 0, attached = 0, fd = -1, i;
    size_t map_size = 0;
    shared_t *shm = NULL;
    struct stat attached_st;
    FILE *log, *decoder_reader = NULL;
    counters_t interval, lifetime;
    memset(&interval, 0, sizeof(interval));
    memset(&lifetime, 0, sizeof(lifetime));
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--log") && i + 1 < argc) log_path = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            unsigned seconds = (unsigned)strtoul(argv[++i], NULL, 10);
            if (seconds > 0) deadline = monotonic_ms() + (uint64_t)seconds * 1000u;
        } else { fprintf(stderr, "usage: %s [--log PATH] [--seconds N]\n", argv[0]); return 2; }
    }
    log = fopen(log_path, "a");
    if (!log) { perror("open trace log"); return 1; }
    signal(SIGTERM, stop_handler);
    signal(SIGINT, stop_handler);
    fprintf(log, "START monotonic_ms=%llu wall_epoch=%ld pid=%ld schema=1\n",
        (unsigned long long)monotonic_ms(), (long)time(NULL), (long)getpid());
    fflush(log);
    next_log = monotonic_ms() + 1000u;
    while (running && (!deadline || monotonic_ms() < deadline)) {
        uint64_t now = monotonic_ms();
        if (!attached) {
            struct stat st;
            fd = open(SHM_PATH, O_RDONLY);
            if (fd >= 0 && fstat(fd, &st) == 0 && st.st_size > 4096) {
                map_size = (size_t)st.st_size;
                ring_size = (uint32_t)(map_size - 16u);
                shm = (shared_t *)mmap(NULL, map_size, PROT_READ, MAP_SHARED, fd, 0);
                if (shm != MAP_FAILED) {
                    attached = 1;
                    attached_st = st;
                    session++;
                    last_total = shm->total_bytes; /* observe only new bytes */
                    shift = 0;
                    expect_nal = 0;
                    fprintf(log, "ATTACH ms=%llu session=%u ring=%u magic=%08x total=%u wp=%u\n",
                        (unsigned long long)now, session, ring_size,
                        shm->magic, last_total, shm->write_pos);
                    fflush(log);
                } else shm = NULL;
            }
            if (!attached && fd >= 0) { close(fd); fd = -1; }
        }
        if (attached) {
            uint32_t total = shm->total_bytes, wp = shm->write_pos;
            uint32_t flags = shm->flags, delta;
            __sync_synchronize();
            if (shm->magic != MAGIC || wp >= ring_size) {
                interval.invalid++; lifetime.invalid++;
            } else if (total < last_total && last_total - total < 0x80000000u) {
                interval.resets++; lifetime.resets++;
                session++;
                shift = 0; expect_nal = 0;
                last_total = total;
                fprintf(log, "RESET ms=%llu session=%u total=%u wp=%u\n",
                    (unsigned long long)now, session, total, wp);
                fflush(log);
            } else if ((delta = total - last_total) != 0) {
                uint32_t readable = delta;
                uint32_t start;
                uint32_t idr_before = interval.nal_idr;
                if (delta > ring_size) {
                    interval.overruns++; lifetime.overruns++;
                    interval.overrun_bytes += delta - ring_size;
                    lifetime.overrun_bytes += delta - ring_size;
                    readable = ring_size;
                    shift = 0; expect_nal = 0;
                    fprintf(log, "OVERRUN ms=%llu session=%u delta=%u ring=%u\n",
                        (unsigned long long)now, session, delta, ring_size);
                    fflush(log);
                }
                start = (wp + ring_size - readable) % ring_size;
                scan_bytes(shm, ring_size, start, readable, &shift, &expect_nal,
                    &interval, &lifetime);
                interval.bytes += delta; lifetime.bytes += delta;
                interval.samples++; lifetime.samples++;
                if (delta > interval.max_sample) interval.max_sample = delta;
                if (delta > lifetime.max_sample) lifetime.max_sample = delta;
                if (interval.nal_idr != idr_before) last_idr_ms = now;
                last_data_ms = now;
                last_total = total;
            }
            if (now >= next_log) {
                struct stat current_st;
                pump_decoder_log(&decoder_reader, log, now);
                print_status(log, now, session, total, wp, flags, &interval,
                    last_idr_ms ? now - last_idr_ms : 0);
                if (last_data_ms && flags && now - last_data_ms > 2000u)
                    fprintf(log, "STALL ms=%llu session=%u no_data_ms=%llu\n",
                        (unsigned long long)now, session,
                        (unsigned long long)(now - last_data_ms));
                memset(&interval, 0, sizeof(interval));
                next_log = now + 1000u;
                rotate_if_needed(&log, log_path);
                if (!log) break;
                if (stat(SHM_PATH, &current_st) != 0 ||
                        current_st.st_dev != attached_st.st_dev ||
                        current_st.st_ino != attached_st.st_ino ||
                        current_st.st_size != attached_st.st_size) {
                    fprintf(log, "DETACH ms=%llu session=%u reason=shm_replaced\n",
                        (unsigned long long)now, session);
                    fflush(log);
                    munmap((void *)shm, map_size);
                    close(fd);
                    shm = NULL; fd = -1; attached = 0;
                    last_total = 0; shift = 0; expect_nal = 0;
                }
            }
        }
        usleep(20000);
    }
    if (log) {
        fprintf(log, "STOP monotonic_ms=%llu bytes=%llu idr=%u overruns=%u invalid=%u\n",
            (unsigned long long)monotonic_ms(),
            (unsigned long long)lifetime.bytes, lifetime.nal_idr,
            lifetime.overruns, lifetime.invalid);
        fclose(log);
    }
    if (shm) munmap((void *)shm, map_size);
    if (decoder_reader) fclose(decoder_reader);
    if (fd >= 0) close(fd);
    return 0;
}
