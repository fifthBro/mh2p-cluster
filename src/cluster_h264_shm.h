/*
 * Copyright (c) 2026 fifthBro
 * https://fifthbro.github.io
 *
 * Licensed under CC BY-NC-SA 4.0
 * https://creativecommons.org/licenses/by-nc-sa/4.0/
 * NOT FOR COMMERCIAL USE
 */

/*
 * Shared memory ring buffer for cluster H.264 stream.
 * Writer: aa_navimg_hook.so (in gal process)
 * Reader: aa_cluster_decoder (separate process)
 */
#ifndef CLUSTER_H264_SHM_H
#define CLUSTER_H264_SHM_H

#include <stdint.h>

#define CLUSTER_SHM_NAME   "/cluster_h264_shm"
#define CLUSTER_SHM_MAGIC  0x48323634u  /* "H264" */
#define CLUSTER_RING_SIZE  (2 * 1024 * 1024) /* 2MB ring buffer */

/* SHM flags bits */
#define CLUSTER_SHM_FLAG_STREAM_ACTIVE (1u << 0)
/* Set by the reader (cluster.c) when it opens SHM at process startup.
 * gal_cluster.so polls this bit; when set, it sends 0x8002 Stop + 0x8001 Start
 * to cluster ch=14 to force the phone's encoder to emit a fresh SPS+PPS+IDR,
 * then clears the bit. Without this, a freshly-spawned cluster child that
 * attaches mid-stream sees only P-slices and never reaches BeginSequence. */
#define CLUSTER_SHM_FLAG_NEED_IDR      (1u << 1)

typedef struct {
    volatile uint32_t magic;
    volatile uint32_t write_pos;   /* writer increments, wraps at CLUSTER_RING_SIZE */
    volatile uint32_t total_bytes; /* total bytes written (monotonic) */
    volatile uint32_t flags;       /* see CLUSTER_SHM_FLAG_* */
    uint8_t ring[CLUSTER_RING_SIZE];
} cluster_h264_shm_t;

#endif
