/*
 * Copyright (c) 2026 fifthBro
 * https://fifthbro.github.io
 *
 * Licensed under CC BY-NC-SA 4.0
 * https://creativecommons.org/licenses/by-nc-sa/4.0/
 * NOT FOR COMMERCIAL USE
 */

/*
 * aa_cluster_mirror.c
 *
 * Single-process AA→cluster mirror.
 *
 * Replaces the aa_display_reader + aa_pixmap_test two-process pipeline.
 * No shared memory. One process holds two Screen contexts:
 *
 *   ctx_mgr  (DISPLAY_MANAGER_CONTEXT) — screen_read_display on HMI display
 *   ctx_app  (APPLICATION_CONTEXT)     — EGL window on cluster display 33
 *
 * Loop (capture=display): screen_read_display → pixmap ptr → glTexSubImage2D → eglSwapBuffers
 * Loop (capture=shmem):   map /gal_vc_frame_shm → poll sequence → glTexSubImage2D → eglSwapBuffers
 *
 * Build:
 *   qcc -Vgcc_ntoarmv7le -Wall -O2 -std=gnu99 \
 *       -o ../../out/aa_cluster_mirror \
 *       aa_cluster_mirror.c \
 *       -lscreen -lEGL -lc
 *
 * NOTE: do NOT link -lGLESv2. GL functions are loaded via eglGetProcAddress.
 * Linking -lGLESv2 pulls in the SDK stub (.so.1) instead of the real Nvidia
 * driver (.so.2), causing SIGSEGV. The GLES2 header is included only for
 * GL type definitions (GLuint, GLenum, etc.) — it does not force linking.
 */

#include <screen/screen.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <zlib.h>                 /* canim files are zlib-compressed */
#include <dlfcn.h>                /* h264 mode: dlopen NvMedia/parser */
#include "cluster_h264_shm.h"     /* h264 mode: shared SHM struct + magic */
#include "boot_canim_data.h"      /* embedded boot splash canim (fifthBro) */
#include <sys/neutrino.h>
#include <sys/syspage.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <signal.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>

/* SCREEN_FORMAT_YV12 — used in h264 mode for the decode window */
#define SCREEN_FORMAT_YV12  13

/* -------- CPVC shmem (legacy capture=shmem path; superseded by capture=h264) --
 * Kept compiling so existing capture=shmem invocations don't crash, but the
 * gal0 hook with AA_HOOK_DECODE=0 no longer fills /cluster_shm — practical
 * use is gone. Will be removed in a follow-up cleanup. */
#define CPVC_SHM_NAME   "/cluster_shm"
#define CPVC_MAGIC      0x43505643u
#define CPVC_SLOTS      3
#define CPVC_SLOT_BYTES (1280 * 720 * 4)
typedef struct {
    volatile uint32_t magic;
    volatile uint32_t version;
    volatile uint32_t width;
    volatile uint32_t height;
    volatile uint32_t stride;
    volatile uint32_t format;
    volatile uint32_t sequence;
    volatile uint32_t current_slot;
    volatile uint32_t frame_ready;
    volatile uint32_t _pad;
    uint8_t           data[CPVC_SLOTS * CPVC_SLOT_BYTES];
} cpvc_shm_t;

/* Default cluster display size — auto-detected at startup from displayable 33
 * via QNX screen API; the hardcoded values below are only the fallback used
 * if the auto-detection fails. 1280x860 = MH2P cluster. Macan is ~540x480.
 * Can also be explicitly overridden with xres=/yres= args. */
#define DISP_W_DEFAULT  1280
#define DISP_H_DEFAULT   860
#define DISP_ID_TARGET  33
static int DISP_W = DISP_W_DEFAULT;
static int DISP_H = DISP_H_DEFAULT;

#define SCREEN_DISPLAY_MANAGER_CONTEXT  8

/* Query the actual pixel dimensions of displayable 33 via the QNX screen API.
 * Returns 0 on success (sets *out_w, *out_h), non-zero on failure (caller
 * should fall back to defaults / explicit args).
 *
 * Enumerates all displays in the application context, picks the one whose
 * integer SCREEN_PROPERTY_ID matches DISP_ID_TARGET (=33 on MH2P/Macan),
 * then reads its SCREEN_PROPERTY_SIZE. */
/* Enumerate physical displays and pick the cluster.
 * On Porsche MH2P there are two displays: HMI (widest, e.g. 1920×720) and
 * cluster (narrower, e.g. 1280×860 Cayenne / ~540×480 Macan). The cluster
 * is whichever has a smaller width than the widest.
 *
 * target_id is honored if SCREEN_PROPERTY_ID happens to match (rarely does
 * on this hardware — display IDs are physical indices, not displayable IDs).
 * Otherwise fall back to the non-widest heuristic. Returns 0 on success.
 *
 * Diagnostic: logs every display seen via printf so the daemon log shows
 * exactly what the hardware reports — easier to debug if detection picks
 * the wrong one on a new car. */
static int detect_disp_size(int target_id, int* out_w, int* out_h) {
    screen_context_t ctx = NULL;
    if (screen_create_context(&ctx, SCREEN_APPLICATION_CONTEXT) != 0)
        return -1;

    int n_displays = 0;
    if (screen_get_context_property_iv(ctx, SCREEN_PROPERTY_DISPLAY_COUNT,
                                       &n_displays) != 0 || n_displays <= 0) {
        screen_destroy_context(ctx);
        return -2;
    }

    screen_display_t* displays =
        (screen_display_t*)malloc((size_t)n_displays * sizeof(screen_display_t));
    if (!displays) {
        screen_destroy_context(ctx);
        return -3;
    }
    if (screen_get_context_property_pv(ctx, SCREEN_PROPERTY_DISPLAYS,
                                       (void**)displays) != 0) {
        free(displays);
        screen_destroy_context(ctx);
        return -4;
    }

    int hmi_w = 0;
    int by_id_w = 0, by_id_h = 0;
    for (int i = 0; i < n_displays; i++) {
        int id = -1;
        int size[2] = { 0, 0 };
        screen_get_display_property_iv(displays[i], SCREEN_PROPERTY_ID,   &id);
        screen_get_display_property_iv(displays[i], SCREEN_PROPERTY_SIZE, size);
        printf("    detect_disp: disp[%d] id=%d size=%dx%d\n", i, id, size[0], size[1]);
        if (size[0] > hmi_w) hmi_w = size[0];
        if (id == target_id && size[0] >= 64 && size[1] >= 64) {
            by_id_w = size[0]; by_id_h = size[1];
        }
    }

    int found = -5;
    if (by_id_w > 0 && by_id_h > 0) {
        *out_w = by_id_w;
        *out_h = by_id_h;
        found = 0;
    } else {
        /* No display matched target_id (typical on MH2P). Pick the first
         * non-widest display with sane dimensions — that's the cluster. */
        for (int i = 0; i < n_displays; i++) {
            int size[2] = { 0, 0 };
            screen_get_display_property_iv(displays[i], SCREEN_PROPERTY_SIZE, size);
            if (size[0] < 64 || size[1] < 64) continue;
            if (size[0] == hmi_w) continue;
            *out_w = size[0];
            *out_h = size[1];
            found = 0;
            break;
        }
    }

    free(displays);
    screen_destroy_context(ctx);
    return found;
}

static uint64_t cc_to_us(uint64_t d) {
    return d * 1000000ULL / SYSPAGE_ENTRY(qtime)->cycles_per_sec;
}

/* -------- GL via eglGetProcAddress (never link -lGLESv2 directly) -------- */
typedef void   (*PFN_Viewport)(GLint,GLint,GLsizei,GLsizei);
typedef void   (*PFN_ClearColor)(GLfloat,GLfloat,GLfloat,GLfloat);
typedef void   (*PFN_Clear)(GLbitfield);
typedef GLuint (*PFN_CreateShader)(GLenum);
typedef void   (*PFN_ShaderSource)(GLuint,GLsizei,const GLchar**,const GLint*);
typedef void   (*PFN_CompileShader)(GLuint);
typedef GLuint (*PFN_CreateProgram)(void);
typedef void   (*PFN_AttachShader)(GLuint,GLuint);
typedef void   (*PFN_LinkProgram)(GLuint);
typedef void   (*PFN_UseProgram)(GLuint);
typedef void   (*PFN_GenTextures)(GLsizei,GLuint*);
typedef void   (*PFN_BindTexture)(GLenum,GLuint);
typedef void   (*PFN_TexParameteri)(GLenum,GLenum,GLint);
typedef void   (*PFN_TexImage2D)(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*);
typedef void   (*PFN_TexSubImage2D)(GLenum,GLint,GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,const void*);
typedef GLint  (*PFN_GetAttribLocation)(GLuint,const GLchar*);
typedef void   (*PFN_EnableVertexAttribArray)(GLuint);
typedef void   (*PFN_VertexAttribPointer)(GLuint,GLint,GLenum,GLboolean,GLsizei,const void*);
typedef void   (*PFN_DrawArrays)(GLenum,GLint,GLsizei);
typedef GLint  (*PFN_GetUniformLocation)(GLuint,const GLchar*);
typedef void   (*PFN_Uniform1i)(GLint,GLint);
typedef void   (*PFN_ActiveTexture)(GLenum);

static PFN_Viewport              gl_Viewport;
static PFN_ClearColor            gl_ClearColor;
static PFN_Clear                 gl_Clear;
static PFN_CreateShader          gl_CreateShader;
static PFN_ShaderSource          gl_ShaderSource;
static PFN_CompileShader         gl_CompileShader;
static PFN_CreateProgram         gl_CreateProgram;
static PFN_AttachShader          gl_AttachShader;
static PFN_LinkProgram           gl_LinkProgram;
static PFN_UseProgram            gl_UseProgram;
static PFN_GenTextures           gl_GenTextures;
static PFN_BindTexture           gl_BindTexture;
static PFN_TexParameteri         gl_TexParameteri;
static PFN_TexImage2D            gl_TexImage2D;
static PFN_TexSubImage2D         gl_TexSubImage2D;
static PFN_GetAttribLocation     gl_GetAttribLocation;
static PFN_EnableVertexAttribArray gl_EnableVertexAttribArray;
static PFN_VertexAttribPointer   gl_VertexAttribPointer;
static PFN_DrawArrays            gl_DrawArrays;
static PFN_GetUniformLocation    gl_GetUniformLocation;
static PFN_Uniform1i             gl_Uniform1i;
static PFN_ActiveTexture         gl_ActiveTexture;

static void load_gl(void) {
#define L(n) gl_##n = (PFN_##n)eglGetProcAddress("gl" #n)
    L(Viewport); L(ClearColor); L(Clear);
    L(CreateShader); L(ShaderSource); L(CompileShader);
    L(CreateProgram); L(AttachShader); L(LinkProgram); L(UseProgram);
    L(GenTextures); L(BindTexture); L(TexParameteri);
    L(TexImage2D); L(TexSubImage2D);
    L(GetAttribLocation); L(EnableVertexAttribArray); L(VertexAttribPointer);
    L(DrawArrays); L(GetUniformLocation); L(Uniform1i); L(ActiveTexture);
#undef L
}

/* -------- Shaders -------- */
static const char *vs_src =
    "attribute vec2 pos;\n"
    "attribute vec2 uv;\n"
    "varying vec2 v_uv;\n"
    "void main() { gl_Position = vec4(pos,0,1); v_uv = uv; }\n";

/* screen_read_display delivers BGRA into an RGBX pixmap — swap R/B */
static const char *fs_src =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D tex;\n"
    "void main() { vec4 c = texture2D(tex,v_uv); gl_FragColor=vec4(c.b,c.g,c.r,1.0); }\n";

/* YV12 shader: 3 GL_LUMINANCE textures (Y full-res, V half-res, U half-res).
 * BT.601 limited-range YCbCr → RGB. */
static const char *fs_yuv_src =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D tex_y;\n"
    "uniform sampler2D tex_v;\n"
    "uniform sampler2D tex_u;\n"
    "void main() {\n"
    "    float y = texture2D(tex_y, v_uv).r - 0.0625;\n"
    "    float v = texture2D(tex_v, v_uv).r - 0.5;\n"
    "    float u = texture2D(tex_u, v_uv).r - 0.5;\n"
    "    float r = clamp(y * 1.164 + v * 1.596,           0.0, 1.0);\n"
    "    float g = clamp(y * 1.164 - v * 0.813 - u * 0.391, 0.0, 1.0);\n"
    "    float b = clamp(y * 1.164 + u * 2.018,           0.0, 1.0);\n"
    "    gl_FragColor = vec4(r, g, b, 1.0);\n"
    "}\n";

static void build_quad_xy(float *v, float qx, float qy, float u0, float u1, float v0, float v1) {
    float q[24] = {
        -qx,  qy, u0, v0,
        -qx, -qy, u0, v1,
         qx, -qy, u1, v1,
        -qx,  qy, u0, v0,
         qx, -qy, u1, v1,
         qx,  qy, u1, v0,
    };
    int i; for (i=0;i<24;i++) v[i]=q[i];
}
static void build_quad(float *v, float qy, float u0, float u1, float v0, float v1) {
    build_quad_xy(v, 1.0f, qy, u0, u1, v0, v1);
}

/* forward declarations */
static float get_arg_f(int argc, char **argv, const char *key, float def);
static const char *get_arg_s(int argc, char **argv, const char *key, const char *def);

/* -------- Watchdog -------- */

static int process_running(const char *name) {
    char cmd[128];
    char buf[64];
    snprintf(cmd, sizeof(cmd), "pidin | grep ' %s$' 2>/dev/null", name);
    FILE *f = popen(cmd, "r");
    if (!f) return 0;
    int found = (fgets(buf, sizeof(buf), f) != NULL);
    pclose(f);
    return found;
}

static int watchdog_run(int argc, char **argv) {
    int settle = 3;
    int poll   = 2;
    int i;

    /* parse watchdog-specific args */
    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "settle=", 7) == 0) settle = atoi(argv[i] + 7);
        if (strncmp(argv[i], "poll=",   5) == 0) poll   = atoi(argv[i] + 5);
    }

    /* build child argv: same as ours but without "watch" */
    char **cargv = malloc(((size_t)argc + 1) * sizeof(char *));
    int    cargc = 0;
    for (i = 0; i < argc; i++) {
        if (i > 0 && strcmp(argv[i], "watch") == 0) continue;
        cargv[cargc++] = argv[i];
    }
    cargv[cargc] = NULL;

    printf("[watchdog] started  monitor=gal  settle=%ds poll=%ds\n", settle, poll);
    printf("[watchdog] mirror cmd: ");
    for (i = 0; i < cargc; i++) printf("%s ", cargv[i]);
    printf("\n");

    while (1) {
        /* wait for gal */
        printf("[watchdog] waiting for gal...\n");
        while (!process_running("gal")) sleep(poll);

        printf("[watchdog] gal detected, settling %ds\n", settle);
        sleep(settle);
        if (!process_running("gal")) {
            printf("[watchdog] gal gone during settle\n");
            continue;
        }

        /* fork mirror child */
        pid_t pid = fork();
        if (pid == 0) {
            execv(cargv[0], cargv);
            fprintf(stderr, "[watchdog] execv failed errno=%d\n", errno);
            exit(1);
        }
        if (pid < 0) {
            fprintf(stderr, "[watchdog] fork failed errno=%d\n", errno);
            sleep(poll);
            continue;
        }
        printf("[watchdog] mirror started pid=%d\n", (int)pid);

        /* monitor: exit if gal dies or child exits */
        while (1) {
            sleep(poll);

            /* check if child is still alive */
            int status;
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid) {
                printf("[watchdog] mirror exited (status=%d), restarting loop\n",
                       WEXITSTATUS(status));
                pid = -1;
                break;
            }

            /* check if gal is still running */
            if (!process_running("gal")) {
                printf("[watchdog] gal gone, killing mirror pid=%d\n", (int)pid);
                kill(pid, SIGTERM);
                waitpid(pid, NULL, 0);
                pid = -1;
                break;
            }
        }

        printf("[watchdog] sleeping %ds before restart loop\n", poll);
        sleep(poll);
    }

    return 0;
}

/* -------- Daemon (control file polled every 1s) -------- */
/*
 * /tmp doesn't support mkfifo (errno=89 ENOSYS on QNX tmpfs).
 * Use a plain file instead: Java writes "start [args]\n" or "stop\n",
 * daemon polls every second, reads+truncates, acts on the command.
 */

#define DAEMON_CTL "/tmp/cluster_ctl"

static pid_t daemon_start_mirror(const char *self, const char *cmdline) {
    char buf[512];
    strncpy(buf, cmdline, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *targv[64];
    int   targc = 0;
    targv[targc++] = (char *)self;

    char *tok = strtok(buf, " \t\r\n");
    while (tok && targc < 63) {
        targv[targc++] = tok;
        tok = strtok(NULL, " \t\r\n");
    }
    targv[targc] = NULL;

    pid_t pid = fork();
    if (pid == 0) {
        execv(self, targv);
        fprintf(stderr, "[daemon] execv failed errno=%d\n", errno);
        exit(1);
    }
    return pid;
}

/* Daemon is a dumb command dispatcher. Java owns lifecycle state.
 *
 * Command set:
 *   prepare [args] — kill any child, spawn fresh with `prepare` arg appended
 *   start   [args] — kill any child, spawn fresh
 *   resume         — SIGUSR1 to child (no-op if no child). Promotes h264
 *                    PREPARE→RENDERING or PAUSED→RENDERING. No spawn.
 *   pause          — SIGUSR2 to child (no-op if no child). h264 child handles
 *                    it as RENDERING→PAUSED; mirror child has no handler so
 *                    kernel kills it (mirror has no useful warm state).
 *   stop           — SIGTERM child.
 *
 * Daemon does NOT track prepared vs rendering vs paused state — those live in
 * the Java integration. Daemon just executes what Java tells it. */

static int daemon_run(int argc, char **argv) {
    int i;
    int poll_ms = (int)(get_arg_f(argc, argv, "poll", 2.0f) * 1000);

    /* default mirror args: everything after "daemon", excluding daemon control args.
     * No xres/yres injection — each child runs its own detect_disp_size in
     * main(), same as direct invocation, which is the path empirically known
     * to work with the displaymanager. */
    char default_args[512] = "";
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "daemon") == 0) continue;
        if (strncmp(argv[i], "poll=", 5) == 0) continue;
        if (strlen(default_args) > 0) strncat(default_args, " ", sizeof(default_args) - strlen(default_args) - 1);
        strncat(default_args, argv[i], sizeof(default_args) - strlen(default_args) - 1);
    }

    /* create empty control file (0666 so Java can write to it) */
    FILE *init = fopen(DAEMON_CTL, "w");
    if (!init) {
        fprintf(stderr, "[daemon] cannot create %s errno=%d\n", DAEMON_CTL, errno);
        return 1;
    }
    fclose(init);
    chmod(DAEMON_CTL, 0666);

    printf("[daemon] polling %s every %dms\n", DAEMON_CTL, poll_ms);
    printf("[daemon] default args: '%s'\n", default_args);
    printf("[daemon] commands: prepare [args]  start [args]  resume [args]  pause  stop\n");

    pid_t       child_pid = -1;
    pid_t       splash_pid = -1;  /* canim_splash (Porsche) when h264 is dead */
    const char *self      = argv[0];

    /* Daemon comes up silent: native maps owns disp 33 by default.
     * On first `prepare`/`start` we spawn the h264 child in STAGE_PREPARE
     * (decoder warming, post_win invisible). Java's `resume` then drives
     * the in-child boot splash blit and the PREPARE→RENDERING transition.
     * Subsequent prepare/start commands DO NOT respawn — the warm child
     * stays alive across Java's flow and is only killed on `stop`. */

    while (1) {
        usleep(poll_ms * 1000);

        /* reap child if it exited on its own */
        if (child_pid > 0) {
            int status;
            if (waitpid(child_pid, &status, WNOHANG) == child_pid) {
                printf("[daemon] child exited on its own (status=%d)\n", WEXITSTATUS(status));
                child_pid = -1;
            }
        }

        /* reap splash if it exited */
        if (splash_pid > 0) {
            int status;
            if (waitpid(splash_pid, &status, WNOHANG) == splash_pid) {
                splash_pid = -1;
            }
        }

        /* read control file */
        FILE *f = fopen(DAEMON_CTL, "r");
        if (!f) continue;
        char line[512];
        int got = (fgets(line, sizeof(line), f) != NULL);
        fclose(f);

        if (!got) continue;

        /* strip trailing whitespace */
        char *end = line + strlen(line) - 1;
        while (end >= line && (*end == '\n' || *end == '\r' || *end == ' ')) *end-- = '\0';
        if (strlen(line) == 0) continue;

        /* truncate the file so we don't re-process the same command */
        FILE *clr = fopen(DAEMON_CTL, "w");
        if (clr) fclose(clr);

        printf("[daemon] cmd: '%s' (pid=%d)\n", line, (int)child_pid);

        /* ── STOP: SIGTERM child, then spawn canim splash. Only spawn canim
         *           if there was actually a child to stop — otherwise we'd
         *           steal disp 33 from native at boot when no session ever
         *           ran. */
        if (strncmp(line, "stop", 4) == 0) {
            int had_child = (child_pid > 0);
            if (had_child) {
                printf("[daemon] stop: SIGTERM pid=%d\n", (int)child_pid);
                kill(child_pid, SIGTERM);
                waitpid(child_pid, NULL, 0);
                child_pid = -1;
            } else {
                printf("[daemon] stop: no child (canim suppressed)\n");
            }
            if (had_child && splash_pid <= 0) {
                splash_pid = daemon_start_mirror(self, "capture=canim_splash");
                printf("[daemon] stop: canim_splash spawned pid=%d\n", (int)splash_pid);
            }

        /* ── PAUSE: SIGUSR2 to h264 child. NO canim_splash spawn —
         *           the h264 child paints canim itself in STAGE_PAUSED so
         *           disp 33 stays bound to the h264 post_win throughout.
         *           Spawning a second window here would steal the
         *           displaymanager binding and prevent recovery on resume. */
        } else if (strncmp(line, "pause", 5) == 0) {
            if (child_pid > 0) {
                printf("[daemon] pause: SIGUSR2 pid=%d\n", (int)child_pid);
                kill(child_pid, SIGUSR2);
            } else {
                printf("[daemon] pause: no child\n");
            }

        /* ── RESUME: SIGUSR1 (h264 PREPARE/PAUSED → RENDERING) ─────── */
        } else if (strncmp(line, "resume", 6) == 0) {
            /* Kill splash first so it releases disp 33 before h264 child
             * window becomes visible again. */
            if (splash_pid > 0) {
                kill(splash_pid, SIGTERM);
                waitpid(splash_pid, NULL, 0);
                splash_pid = -1;
            }
            if (child_pid > 0) {
                printf("[daemon] resume: SIGUSR1 pid=%d\n", (int)child_pid);
                kill(child_pid, SIGUSR1);
            } else {
                printf("[daemon] resume: no child\n");
            }

        /* ── PREPARE: spawn h264 child in PREPARE stage if not already up.
         *           If a child already exists, leave it alone — it's warm and
         *           waiting for resume. The boot splash and disp 33 binding
         *           are entirely in-child (apply_stage_signals on resume blits
         *           boot_pixmap to post_win). */
        } else if (strncmp(line, "prepare", 7) == 0) {
            const char *extra = line + 7;
            while (*extra == ' ') extra++;
            char cmdline[512];
            int n;
            if (*extra && *default_args)
                n = snprintf(cmdline, sizeof(cmdline), "%s %s prepare", default_args, extra);
            else if (*extra)
                n = snprintf(cmdline, sizeof(cmdline), "%s prepare", extra);
            else
                n = snprintf(cmdline, sizeof(cmdline), "%s prepare", default_args);
            if (n < 0 || n >= (int)sizeof(cmdline)) cmdline[sizeof(cmdline)-1] = '\0';

            /* Release disp 33 from any canim_splash before the real child claims it. */
            if (splash_pid > 0) {
                kill(splash_pid, SIGTERM);
                waitpid(splash_pid, NULL, 0);
                splash_pid = -1;
            }
            if (child_pid > 0) {
                printf("[daemon] prepare: child already warm pid=%d, leaving in PREPARE\n",
                       (int)child_pid);
            } else {
                child_pid = daemon_start_mirror(self, cmdline);
                printf("[daemon] prepare: pid=%d args='%s'\n", (int)child_pid, cmdline);
            }

        /* ── START: direct spawn (no PREPARE arg, no SIGUSR1). Used by the
         *           CarPlay/mirror path which has no PREPARE/RENDERING
         *           distinction. The h264 path uses the explicit
         *           prepare+resume sequence from Java instead. */
        } else if (strncmp(line, "start", 5) == 0) {
            const char *extra = line + 5;
            while (*extra == ' ') extra++;
            char cmdline[512];
            int n;
            if (*extra && *default_args)
                n = snprintf(cmdline, sizeof(cmdline), "%s %s", default_args, extra);
            else if (*extra)
                n = snprintf(cmdline, sizeof(cmdline), "%s", extra);
            else
                n = snprintf(cmdline, sizeof(cmdline), "%s", default_args);
            if (n < 0 || n >= (int)sizeof(cmdline)) cmdline[sizeof(cmdline)-1] = '\0';

            if (splash_pid > 0) {
                kill(splash_pid, SIGTERM);
                waitpid(splash_pid, NULL, 0);
                splash_pid = -1;
            }
            if (child_pid > 0) {
                kill(child_pid, SIGTERM);
                waitpid(child_pid, NULL, 0);
                child_pid = -1;
            }
            child_pid = daemon_start_mirror(self, cmdline);
            printf("[daemon] start: pid=%d args='%s'\n", (int)child_pid, cmdline);

        } else {
            printf("[daemon] unknown command '%s' (use: prepare/start [args] / resume / pause / stop)\n", line);
        }
    }

    return 0;
}

/* parse "key=value" args */
static float get_arg_f(int argc, char **argv, const char *key, float def) {
    int i;
    size_t klen = strlen(key);
    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], key, klen) == 0 && argv[i][klen] == '=')
            return (float)atof(argv[i] + klen + 1);
    }
    return def;
}

static const char *get_arg_s(int argc, char **argv, const char *key, const char *def) {
    int i;
    size_t klen = strlen(key);
    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], key, klen) == 0 && argv[i][klen] == '=')
            return argv[i] + klen + 1;
    }
    return def;
}

/* Non-static so aa_cluster_decoder.c (linked into the same binary)
 * can declare it `extern` and gate its own LOG calls on the same flag. */
int g_verbose = 0;
#define LOG(...) do { if (g_verbose) printf(__VA_ARGS__); } while(0)

/* -------- verbose=2: redirect stdout to rotating log file -------- */
#define LOG_FILENAME   "cluster_daemon.log"
#define LOG_MAX_BYTES  (1024 * 1024)   /* 1 MB — truncate and restart */

static void log_open(void) {
    static const char *fs_roots[] = {
        "/fs/usb0_0", "/fs/usb1_0", "/fs/sda0", "/fs/sdb0", "/tmp", NULL
    };
    char path[256];
    int i;
    for (i = 0; fs_roots[i]; i++) {
        snprintf(path, sizeof(path), "%s/" LOG_FILENAME, fs_roots[i]);

        /* probe writability */
        FILE *f = fopen(path, "a");
        if (!f) continue;

        /* truncate if already >= 1MB */
        fseek(f, 0, SEEK_END);
        if (ftell(f) >= LOG_MAX_BYTES) {
            fclose(f);
            f = fopen(path, "w");
            if (!f) continue;
        }

        dup2(fileno(f), STDOUT_FILENO);
        dup2(STDOUT_FILENO, STDERR_FILENO);
        fclose(f);
        setvbuf(stdout, NULL, _IOLBF, 0);
        printf("[log] %s\n", path);
        return;
    }
    fprintf(stderr, "[log] no writable fs found, falling back to console\n");
}

/* Forward declaration — h264 mode is defined in cluster_mirror_h264.c, which
 * wraps the proven aa_cluster_decoder.c (v5). Reads raw H.264 from
 * /dev/shmem/cluster_h264_shm (filled by gal0 hook with AA_HOOK_DECODE=0),
 * NvMedia decodes, screen_blit YV12 → RGBA into the cluster window's buffer,
 * screen_post. No GL/EGL — VIC HW does the YUV→RGB conversion. */
int run_h264_mode(int argc, char **argv);
int run_test_mode(int argc, char **argv);
int run_idle_logo(int argc, char **argv, int default_duration_s);
int run_canim_splash(int argc, char **argv, int default_duration_s);

/* Forward declarations for canim helper used by run_h264_mode (defined later
 * in the file alongside run_canim_splash). */
#define CANIM_DEFAULT_PATH "/mnt/app/eso/hmi/splashscreen/splashscreen_000.canim"
static int canim_load_into_buf(const char* path,
                               uint8_t* dst, int dst_stride,
                               int dst_w, int dst_h,
                               int swap_rb);
static int canim_load_from_mem(const uint8_t* zbuf, size_t zlen,
                               uint8_t* dst, int dst_stride,
                               int dst_w, int dst_h,
                               int swap_rb);
static uint64_t now_us(void);

int main(int argc, char **argv) {
    int rc, i;

    /* Parse verbose= early so log_open() can take effect before any LOG calls. */
    g_verbose = (int)get_arg_f(argc, argv, "verbose", 0.0f);
    if (g_verbose >= 2) log_open();

    /* watchdog / daemon dispatch BEFORE any screen API calls. Daemon process
     * must not create a screen context (empirically: doing so registers the
     * daemon as a displaymanager client and interferes with subsequent
     * child-process windows claiming disp 33). Resolution detection moved
     * below so it only runs in render-mode children. */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "watch") == 0)
            return watchdog_run(argc, argv);
        if (strcmp(argv[i], "daemon") == 0)
            return daemon_run(argc, argv);
    }

    /* Resolve cluster display resolution. Runs in render-mode processes only
     * (h264, mirror, test, idle_logo, canim_splash, …). Daemon path returned
     * above without touching the screen API. */
    float xres_arg = get_arg_f(argc, argv, "xres", -1.0f);
    float yres_arg = get_arg_f(argc, argv, "yres", -1.0f);
    if (xres_arg >= 64.0f && yres_arg >= 64.0f) {
        DISP_W = (int)xres_arg;
        DISP_H = (int)yres_arg;
        LOG("display size: %dx%d (from args, auto-detect skipped)\n", DISP_W, DISP_H);
    } else {
        int auto_w = 0, auto_h = 0;
        int auto_ok = (detect_disp_size(DISP_ID_TARGET, &auto_w, &auto_h) == 0);
        DISP_W = (xres_arg >= 64.0f) ? (int)xres_arg
               : (auto_ok ? auto_w : DISP_W_DEFAULT);
        DISP_H = (yres_arg >= 64.0f) ? (int)yres_arg
               : (auto_ok ? auto_h : DISP_H_DEFAULT);
        LOG("display size: %dx%d (xres_arg=%.0f yres_arg=%.0f auto=%s %dx%d)\n",
            DISP_W, DISP_H, xres_arg, yres_arg,
            auto_ok ? "ok" : "fail", auto_w, auto_h);
    }

    /* capture= source selector:
     *   display (default) — screen_read_display from HMI display, GL render
     *   h264              — read raw H.264 from /dev/shmem/cluster_h264_shm,
     *                       NvMedia decode, screen_blit to cluster, no GL
     *   test              — render a static calibration pattern to displayable
     *                       33 (concentric inset rectangles + tick marks) for
     *                       bezel-shape measurement. No external input. */
    const char *capture = get_arg_s(argc, argv, "capture", "display");
    if (strcmp(capture, "h264") == 0) {
        return run_h264_mode(argc, argv);
    }
    if (strcmp(capture, "test") == 0) {
        return run_test_mode(argc, argv);
    }
    if (strcmp(capture, "idle_logo") == 0) {
        /* idle_logo: indefinite, exits on SIGTERM from daemon. */
        return run_idle_logo(argc, argv, 0);
    }
    if (strcmp(capture, "boot_splash") == 0) {
        /* boot_splash: fifthBro credit, owns disp 33 from daemon start until
         * Java sends the first `start`/`prepare`. Runs indefinitely — exits
         * only on SIGTERM from daemon when a real renderer is spawned. */
        return run_idle_logo(argc, argv, 0);
    }
    if (strcmp(capture, "canim_splash") == 0) {
        /* Direct invocation, no fallback. Useful for manual testing. */
        return run_canim_splash(argc, argv, 0);
    }
    int use_shmem = (strcmp(capture, "shmem") == 0);

    /* fifthBro splash before mirror takes disp 33. Fork a short-lived child
     * that runs the same binary in `capture=boot_splash duration_s=2`
     * (= run_idle_logo with 2s sleep), then waitpid so our mirror init
     * doesn't race the splash for disp 33. Skips if Java passed
     * skip_boot_splash=1 (reserved for future use; not set today). */
    if ((int)get_arg_f(argc, argv, "skip_boot_splash", 0.0f) == 0) {
        pid_t bp = fork();
        if (bp == 0) {
            char *bargv[] = {
                argv[0], "verbose=2", "capture=boot_splash", "duration_s=2", NULL
            };
            execv(argv[0], bargv);
            _exit(1);
        } else if (bp > 0) {
            waitpid(bp, NULL, 0);
        }
    }

    /* mode: accept both positional ("fill") and key=value ("mode=fill") */
    const char *mode = "letter";
    if (argc > 1 && strchr(argv[1], '=') == NULL)
        mode = argv[1];
    else
        mode = get_arg_s(argc, argv, "mode", "letter");

    /* zoomX/Y: UV scale factor > 1.0 crops edges and magnifies centre
     * panX/Y:  UV shift, positive = image moves left/up (shows more right/bottom) */
    float zoomX = get_arg_f(argc, argv, "zoomX", 1.0f);
    float zoomY = get_arg_f(argc, argv, "zoomY", 1.0f);
    float panX  = get_arg_f(argc, argv, "panX",  0.0f);
    float panY  = get_arg_f(argc, argv, "panY",  0.0f);
    if (zoomX < 0.1f) zoomX = 0.1f;
    if (zoomY < 0.1f) zoomY = 0.1f;
    /* DISP_W/H were resolved at the top of main() (auto-detect + xres=/yres= override).
     * verbose / log_open were also parsed before the dispatch above. */

    LOG("+--------------------------------------------------+\n");
    LOG("  aa_cluster_mirror  capture=%s\n", capture);
    LOG("  mode: %s  zoomX: %.3f  zoomY: %.3f  panX: %.3f  panY: %.3f\n", mode, zoomX, zoomY, panX, panY);
    LOG("+--------------------------------------------------+\n\n");

    /* ------------------------------------------------------------------ */
    /* [1] DISPLAY_MANAGER_CONTEXT — for screen_read_display              */
    /* ------------------------------------------------------------------ */
    screen_context_t ctx_mgr = NULL;
    rc = screen_create_context(&ctx_mgr, SCREEN_DISPLAY_MANAGER_CONTEXT);
    if (rc != 0) {
        LOG("[1] DISPLAY_MANAGER_CONTEXT failed errno=%d, trying APPLICATION_CONTEXT\n", errno);
        rc = screen_create_context(&ctx_mgr, SCREEN_APPLICATION_CONTEXT);
        if (rc != 0) { LOG("[!] context creation failed\n"); return 1; }
        LOG("[1] ctx_mgr=APPLICATION_CONTEXT (fallback) %p\n\n", (void*)ctx_mgr);
    } else {
        LOG("[1] ctx_mgr=DISPLAY_MANAGER_CONTEXT %p\n\n", (void*)ctx_mgr);
    }


    /* ------------------------------------------------------------------ */
    /* [2] Find HMI display (1920×720)                                    */
    /* ------------------------------------------------------------------ */
    int ndisplays = 0;
    screen_get_context_property_iv(ctx_mgr, SCREEN_PROPERTY_DISPLAY_COUNT, &ndisplays);
    LOG("[2] display_count=%d\n", ndisplays);
    if (ndisplays <= 0) { LOG("[!] no displays\n"); return 1; }

    screen_display_t *displays = calloc(ndisplays, sizeof(screen_display_t));
    screen_get_context_property_pv(ctx_mgr, SCREEN_PROPERTY_DISPLAYS, (void**)displays);

    int sel=0, sel_w=0, sel_h=0;
    int hmi_w_max = 0;
    int cluster_w_auto = 0, cluster_h_auto = 0;
    for (i = 0; i < ndisplays; i++) {
        int id=-1, sz[2]={0,0};
        screen_get_display_property_iv(displays[i], SCREEN_PROPERTY_ID,   &id);
        screen_get_display_property_iv(displays[i], SCREEN_PROPERTY_SIZE, sz);
        LOG("    disp[%d]: id=%d  size=%dx%d\n", i, id, sz[0], sz[1]);
        if (sz[0] > hmi_w_max) hmi_w_max = sz[0];
        if (sel_h == 0 || sz[1] == 720) { sel=i; sel_w=sz[0]; sel_h=sz[1]; }
    }
    /* Pick cluster: first non-zero display that isn't the widest (HMI). */
    for (i = 0; i < ndisplays; i++) {
        int sz[2] = {0,0};
        screen_get_display_property_iv(displays[i], SCREEN_PROPERTY_SIZE, sz);
        if (sz[0] == 0 || sz[1] == 0) continue;
        if (sz[0] == hmi_w_max) continue;
        cluster_w_auto = sz[0]; cluster_h_auto = sz[1];
        break;
    }
    /* Only apply auto if user didn't override with xres=/yres=. */
    if (cluster_w_auto > 0 && DISP_W == DISP_W_DEFAULT) DISP_W = cluster_w_auto;
    if (cluster_h_auto > 0 && DISP_H == DISP_H_DEFAULT) DISP_H = cluster_h_auto;
    LOG("    cluster: auto=%dx%d, using=%dx%d\n", cluster_w_auto, cluster_h_auto, DISP_W, DISP_H);

    screen_display_t hmi_disp = displays[sel];
    int cap_w = (sel_w > 0 && sel_w <= 1920) ? sel_w : 1920;
    int cap_h = (sel_h > 0 && sel_h <= 1080) ? sel_h : 720;
    LOG("    => disp[%d] %dx%d  cap=%dx%d\n\n", sel, sel_w, sel_h, cap_w, cap_h);

    /* ------------------------------------------------------------------ */
    /* [3] Capture pixmap (RGBX8888, READ|WRITE)                          */
    /* ------------------------------------------------------------------ */
    int fmt   = SCREEN_FORMAT_RGBX8888;
    int usage = SCREEN_USAGE_READ | SCREEN_USAGE_WRITE;
    int bsz[2] = { cap_w, cap_h };

    screen_pixmap_t  cap_pixmap = NULL;
    screen_buffer_t  cap_buf    = NULL;
    void            *cap_ptr    = NULL;
    int              cap_stride = 0;

    rc = screen_create_pixmap(&cap_pixmap, ctx_mgr);
    LOG("[3] create_pixmap rc=%d pix=%p\n", rc, (void*)cap_pixmap);
    if (rc) { LOG("[!] failed errno=%d\n", errno); return 1; }

    screen_set_pixmap_property_iv(cap_pixmap, SCREEN_PROPERTY_FORMAT,      &fmt);
    screen_set_pixmap_property_iv(cap_pixmap, SCREEN_PROPERTY_USAGE,       &usage);
    screen_set_pixmap_property_iv(cap_pixmap, SCREEN_PROPERTY_BUFFER_SIZE, bsz);

    rc = screen_create_pixmap_buffer(cap_pixmap);
    if (rc) { LOG("[!] create_pixmap_buffer failed errno=%d\n", errno); return 1; }

    screen_get_pixmap_property_pv(cap_pixmap, SCREEN_PROPERTY_RENDER_BUFFERS, (void**)&cap_buf);
    screen_get_buffer_property_pv(cap_buf, SCREEN_PROPERTY_POINTER, &cap_ptr);
    screen_get_buffer_property_iv(cap_buf, SCREEN_PROPERTY_STRIDE,  &cap_stride);
    LOG("    ptr=%p  stride=%d\n\n", cap_ptr, cap_stride);
    if (!cap_ptr) { LOG("[!] pixmap ptr is NULL\n"); return 1; }

    /* ------------------------------------------------------------------ */
    /* [4] APPLICATION_CONTEXT — cluster window on display 33             */
    /* ------------------------------------------------------------------ */
    screen_context_t ctx_app = NULL;
    rc = screen_create_context(&ctx_app, SCREEN_APPLICATION_CONTEXT);
    LOG("[4] ctx_app=APPLICATION_CONTEXT rc=%d %p\n", rc, (void*)ctx_app);
    if (rc) { LOG("[!] failed errno=%d\n", errno); return 1; }

    screen_window_t cl_win = NULL;
    rc = screen_create_window(&cl_win, ctx_app);
    if (rc) { LOG("[!] create_window failed\n"); return 1; }

    screen_set_window_property_cv(cl_win, SCREEN_PROPERTY_ID_STRING, 2, "33");
    int cl_size[2] = { DISP_W, DISP_H };
    screen_set_window_property_iv(cl_win, SCREEN_PROPERTY_SIZE,        cl_size);
    screen_set_window_property_iv(cl_win, SCREEN_PROPERTY_BUFFER_SIZE, cl_size);
    int cl_fmt   = SCREEN_FORMAT_RGBX8888;
    int cl_usage = SCREEN_USAGE_OPENGL_ES2;
    int cl_vis   = 0;
    screen_set_window_property_iv(cl_win, SCREEN_PROPERTY_FORMAT,  &cl_fmt);
    screen_set_window_property_iv(cl_win, SCREEN_PROPERTY_USAGE,   &cl_usage);
    screen_set_window_property_iv(cl_win, SCREEN_PROPERTY_VISIBLE, &cl_vis);
    screen_flush_context(ctx_app, 0);
    rc = screen_create_window_buffers(cl_win, 2);
    if (rc) { LOG("[!] create_window_buffers failed\n"); return 1; }
    LOG("    cluster window created\n\n");

    /* ------------------------------------------------------------------ */
    /* [5] EGL on cluster window                                          */
    /* ------------------------------------------------------------------ */
    EGLDisplay egl_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(egl_dpy, NULL, NULL);
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };
    EGLConfig egl_cfg; EGLint ncfg;
    eglChooseConfig(egl_dpy, cfg_attr, &egl_cfg, 1, &ncfg);

    EGLSurface egl_surf = eglCreateWindowSurface(egl_dpy, egl_cfg,
                                                 (EGLNativeWindowType)cl_win, NULL);
    if (egl_surf == EGL_NO_SURFACE) {
        LOG("[!] eglCreateWindowSurface failed 0x%x\n", eglGetError());
        return 1;
    }
    EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext egl_ctx = eglCreateContext(egl_dpy, egl_cfg, EGL_NO_CONTEXT, ctx_attr);
    if (egl_ctx == EGL_NO_CONTEXT) { LOG("[!] eglCreateContext failed\n"); return 1; }
    eglMakeCurrent(egl_dpy, egl_surf, egl_surf, egl_ctx);

    /* drain any EGL errors before loading GL */
    while (eglGetError() != EGL_SUCCESS) {}
    load_gl();
    LOG("[5] EGL ready\n\n");

    /* ------------------------------------------------------------------ */
    /* [6] GL texture (cap_w × cap_h)                                     */
    /* ------------------------------------------------------------------ */

    /* ------------------------------------------------------------------ */
    /* shmem setup (capture=shmem only)                                   */
    /* ------------------------------------------------------------------ */
    cpvc_shm_t *g_shm = NULL;
    uint32_t    shm_last_seq = 0;

    if (use_shmem) {
        LOG("[6] capture=shmem: opening %s...\n", CPVC_SHM_NAME);
        int sfd = shm_open(CPVC_SHM_NAME, O_RDONLY, 0);
        if (sfd < 0) { LOG("[!] shm_open failed errno=%d\n", errno); return 1; }
        size_t shm_sz = sizeof(cpvc_shm_t);
        g_shm = (cpvc_shm_t *)mmap(NULL, shm_sz, PROT_READ, MAP_SHARED, sfd, 0);
        close(sfd);
        if (g_shm == MAP_FAILED) { LOG("[!] mmap failed errno=%d\n", errno); return 1; }

        /* wait for hook to write first frame */
        int attempts;
        for (attempts = 0; attempts < 100; attempts++) {
            if (g_shm->magic == CPVC_MAGIC && g_shm->frame_ready) break;
            LOG("    waiting for shmem frame... attempt=%d\n", attempts);
            usleep(200000);
        }
        if (g_shm->magic != CPVC_MAGIC || !g_shm->frame_ready) {
            LOG("[!] shmem never became ready\n"); return 1;
        }
        /* override cap dimensions from shmem */
        cap_w = (int)g_shm->width;
        cap_h = (int)g_shm->height;
        shm_last_seq = g_shm->sequence;
        LOG("    shmem ready: %dx%d fmt=%u seq=%u\n\n",
            cap_w, cap_h, g_shm->format, shm_last_seq);
    }

    /* Detect YV12 shmem format */
    int shm_yuv = use_shmem && (g_shm->format == SCREEN_FORMAT_YV12);

    /* Do initial capture to have real pixels for glTexImage2D
     * (glTexImage2D(NULL) crashes on Tegra K1) */
    if (!use_shmem) {
        LOG("[6] Initial capture (display)...\n");
        int attempts;
        for (attempts = 0; attempts < 50; attempts++) {
            rc = screen_read_display(hmi_disp, cap_buf, 0, NULL, 0);
            if (rc == 0) break;
            LOG("    waiting for display... attempt=%d errno=%d\n", attempts, errno);
            usleep(200000);
        }
        if (rc != 0) { LOG("[!] screen_read_display never succeeded\n"); return 1; }
        LOG("    first capture ok  ptr=%p stride=%d\n", cap_ptr, cap_stride);
    }

    /* If stride is tight, pass ptr directly. Otherwise copy to tight buffer first. */
    uint8_t *upload_buf = NULL;
    int tight = use_shmem ? 1 : (cap_stride == cap_w * 4);
    if (!tight) {
        upload_buf = malloc((size_t)cap_w * (size_t)cap_h * 4);
        if (!upload_buf) { LOG("[!] malloc failed\n"); return 1; }
        int r;
        for (r = 0; r < cap_h; r++)
            memcpy(upload_buf + (size_t)r * (size_t)cap_w * 4,
                   (uint8_t*)cap_ptr + (size_t)r * (size_t)cap_stride,
                   (size_t)cap_w * 4);
    }
    uint32_t init_slot = (use_shmem && g_shm->version >= 2)
        ? g_shm->current_slot : 0;
    const void *px = use_shmem ? (const void *)(g_shm->data + (size_t)init_slot * CPVC_SLOT_BYTES)
                                : (tight ? cap_ptr : upload_buf);

    /* YV12 SHM: 3 GL_LUMINANCE textures. Offsets in data[]:
     *   Y: [0 .. cap_w*cap_h)  (1920×720)
     *   V: [cap_w*cap_h .. cap_w*cap_h + (cap_w/2)*(cap_h/2))  (960×360)
     *   U: [cap_w*cap_h + (cap_w/2)*(cap_h/2) .. )              (960×360) */
    GLuint tex_y = 0, tex_v = 0, tex_u = 0;
    if (shm_yuv) {
        int uv_w = cap_w / 2, uv_h = cap_h / 2;
        const uint8_t *y_ptr = g_shm->data + (size_t)init_slot * CPVC_SLOT_BYTES;
        const uint8_t *v_ptr = y_ptr + (size_t)cap_w * cap_h;
        const uint8_t *u_ptr = v_ptr + (size_t)uv_w * uv_h;

        gl_GenTextures(1, &tex_y);
        gl_ActiveTexture(GL_TEXTURE0);
        gl_BindTexture(GL_TEXTURE_2D, tex_y);
        gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl_TexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, cap_w, cap_h, 0,
                      GL_LUMINANCE, GL_UNSIGNED_BYTE, y_ptr);

        gl_GenTextures(1, &tex_v);
        gl_ActiveTexture(GL_TEXTURE1);
        gl_BindTexture(GL_TEXTURE_2D, tex_v);
        gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl_TexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, uv_w, uv_h, 0,
                      GL_LUMINANCE, GL_UNSIGNED_BYTE, v_ptr);

        gl_GenTextures(1, &tex_u);
        gl_ActiveTexture(GL_TEXTURE2);
        gl_BindTexture(GL_TEXTURE_2D, tex_u);
        gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl_TexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, uv_w, uv_h, 0,
                      GL_LUMINANCE, GL_UNSIGNED_BYTE, u_ptr);

        LOG("    YV12 textures Y=%u V=%u U=%u  Y:%dx%d UV:%dx%d\n\n",
            tex_y, tex_v, tex_u, cap_w, cap_h, uv_w, uv_h);
    } else {
        GLuint tex;
        gl_GenTextures(1, &tex);
        gl_ActiveTexture(GL_TEXTURE0);
        gl_BindTexture(GL_TEXTURE_2D, tex);
        gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, cap_w, cap_h, 0,
                      GL_RGBA, GL_UNSIGNED_BYTE, px);
        tex_y = tex;
        LOG("    texture %dx%d uploaded\n\n", cap_w, cap_h);
    }

    /* ------------------------------------------------------------------ */
    /* [7] Shader + quad                                                  */
    /* ------------------------------------------------------------------ */
    const char *frag = shm_yuv ? fs_yuv_src : fs_src;
    GLuint vs = gl_CreateShader(GL_VERTEX_SHADER);
    gl_ShaderSource(vs, 1, &vs_src, NULL); gl_CompileShader(vs);
    GLuint fs = gl_CreateShader(GL_FRAGMENT_SHADER);
    gl_ShaderSource(fs, 1, &frag, NULL); gl_CompileShader(fs);
    GLuint prog = gl_CreateProgram();
    gl_AttachShader(prog, vs); gl_AttachShader(prog, fs);
    gl_LinkProgram(prog); gl_UseProgram(prog);
    if (shm_yuv) {
        gl_Uniform1i(gl_GetUniformLocation(prog, "tex_y"), 0);
        gl_Uniform1i(gl_GetUniformLocation(prog, "tex_v"), 1);
        gl_Uniform1i(gl_GetUniformLocation(prog, "tex_u"), 2);
    } else {
        gl_Uniform1i(gl_GetUniformLocation(prog, "tex"), 0);
    }

    float src_ar   = (float)cap_w / (float)cap_h;
    float disp_ar  = (float)DISP_W / (float)DISP_H;
    float qx = 1.0f;
    float qy = disp_ar / src_ar;
    float u0 = 0.0f, u1 = 1.0f;
    float v0 = 0.0f, v1 = 1.0f;
    if (strcmp(mode, "fill") == 0) {
        qy = 1.0f;
        float vis = disp_ar / src_ar;
        u0 = (1.0f - vis) * 0.5f; u1 = 1.0f - u0;
    } else if (strcmp(mode, "stretch") == 0) {
        qy = 1.0f;
    } else if (strcmp(mode, "native") == 0) {
        /* 1:1 pixel mapping — source cap_w × cap_h drawn at native pixel
         * size centered on display, with black borders around. */
        qx = (float)cap_w / (float)DISP_W;
        qy = (float)cap_h / (float)DISP_H;
        if (qx > 1.0f) qx = 1.0f;
        if (qy > 1.0f) qy = 1.0f;
    }

    /* Apply zoomX: shrink U range around centre */
    {
        float centre = (u0 + u1) * 0.5f;
        float half   = (u1 - u0) * 0.5f / zoomX;
        u0 = centre - half;
        u1 = centre + half;
    }
    /* Apply zoomY: shrink V range around centre */
    {
        float centre = (v0 + v1) * 0.5f;
        float half   = (v1 - v0) * 0.5f / zoomY;
        v0 = centre - half;
        v1 = centre + half;
    }

    /* Apply panX/panY: shift UV ranges (positive = image moves left/up) */
    { float s = panX * (u1 - u0); u0 += s; u1 += s; }
    { float s = panY * (v1 - v0); v0 += s; v1 += s; }

    /* No UV clamping — GL_CLAMP_TO_EDGE handles out-of-range values. */

    float verts[24];
    build_quad_xy(verts, qx, qy, u0, u1, v0, v1);
    LOG("[7] quad: qy=%.4f u0=%.4f u1=%.4f v0=%.4f v1=%.4f  src=%dx%d disp=%dx%d"
           "  zoomX=%.3f zoomY=%.3f panX=%.3f panY=%.3f\n\n",
           qy, u0, u1, v0, v1, cap_w, cap_h, DISP_W, DISP_H, zoomX, zoomY, panX, panY);

    GLint pos_loc = gl_GetAttribLocation(prog, "pos");
    GLint uv_loc  = gl_GetAttribLocation(prog, "uv");
    gl_EnableVertexAttribArray(pos_loc);
    gl_EnableVertexAttribArray(uv_loc);
    gl_VertexAttribPointer(pos_loc, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), verts);
    gl_VertexAttribPointer(uv_loc,  2, GL_FLOAT, GL_FALSE, 4*sizeof(float), verts+2);

    cl_vis = 1;
    screen_set_window_property_iv(cl_win, SCREEN_PROPERTY_VISIBLE, &cl_vis);
    screen_flush_context(ctx_app, 0);

    /* ------------------------------------------------------------------ */
    /* [8] Render loop                                                     */
    /* ------------------------------------------------------------------ */
    LOG("[8] Running (Ctrl-C to stop)...\n");

    uint32_t render_fps = 0, cap_fps = 0;
    time_t   fps_ts = time(NULL);
    uint32_t cap_fails = 0;

    while (1) {
        uint64_t t0 = ClockCycles();
        uint64_t t_tex0 = 0, t_tex1 = 0, t_draw0, t_draw1, t_swap0, t_swap1;
        int got_frame = 0;

        if (use_shmem) {
            /* poll shmem for new frame */
            uint32_t seq = g_shm->sequence;
            if (g_shm->frame_ready && seq != shm_last_seq) {
                uint32_t missed = seq - shm_last_seq - 1;
                if (missed > 0) LOG("  [!] missed %u shmem frames (seq %u->%u)\n",
                                    missed, shm_last_seq, seq);
                shm_last_seq = seq;
                t_tex0 = ClockCycles();
                /* v2 ring: read from current_slot. v1 falls back to slot 0. */
                uint32_t slot = (g_shm->version >= 2)
                    ? __atomic_load_n(&g_shm->current_slot, __ATOMIC_ACQUIRE)
                    : 0;
                const uint8_t *base = g_shm->data + (size_t)slot * CPVC_SLOT_BYTES;
                if (shm_yuv) {
                    int uv_w = cap_w / 2, uv_h = cap_h / 2;
                    const uint8_t *y_p = base;
                    const uint8_t *v_p = y_p + (size_t)cap_w * cap_h;
                    const uint8_t *u_p = v_p + (size_t)uv_w * uv_h;
                    gl_ActiveTexture(GL_TEXTURE0);
                    gl_BindTexture(GL_TEXTURE_2D, tex_y);
                    gl_TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cap_w, cap_h,
                                     GL_LUMINANCE, GL_UNSIGNED_BYTE, y_p);
                    gl_ActiveTexture(GL_TEXTURE1);
                    gl_BindTexture(GL_TEXTURE_2D, tex_v);
                    gl_TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uv_w, uv_h,
                                     GL_LUMINANCE, GL_UNSIGNED_BYTE, v_p);
                    gl_ActiveTexture(GL_TEXTURE2);
                    gl_BindTexture(GL_TEXTURE_2D, tex_u);
                    gl_TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uv_w, uv_h,
                                     GL_LUMINANCE, GL_UNSIGNED_BYTE, u_p);
                } else {
                    gl_ActiveTexture(GL_TEXTURE0);
                    gl_BindTexture(GL_TEXTURE_2D, tex_y);
                    gl_TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cap_w, cap_h,
                                     GL_RGBA, GL_UNSIGNED_BYTE, base);
                }
                t_tex1 = ClockCycles();
                cap_fps++;
                got_frame = 1;
                cap_fails = 0;
            } else {
                cap_fails++;
                /* No new frame. Phone may legitimately send nothing for many
                 * seconds when the map is static — don't exit, just idle.
                 *
                 * For the first few polls fall through to draw+swap so the
                 * initial frame (uploaded before the loop) hits the window.
                 * After that, go into cheap-idle: sleep longer and skip the
                 * draw+swap entirely. Compositor keeps displaying the last
                 * posted frame, so the cluster stays lit without burning
                 * CPU/GPU redrawing identical pixels at 50 Hz. */
                if (cap_fails > 5) {
                    usleep(100000); /* 100 ms idle poll */
                    continue;
                }
                usleep(20000);
            }
        } else {
            rc = screen_read_display(hmi_disp, cap_buf, 0, NULL, 0);
            if (rc == 0) {
                cap_fails = 0;
                if (!tight) {
                    int r;
                    for (r = 0; r < cap_h; r++)
                        memcpy(upload_buf + (size_t)r * (size_t)cap_w * 4,
                               (uint8_t*)cap_ptr + (size_t)r * (size_t)cap_stride,
                               (size_t)cap_w * 4);
                }
                gl_ActiveTexture(GL_TEXTURE0);
                gl_BindTexture(GL_TEXTURE_2D, tex_y);
                t_tex0 = ClockCycles();
                gl_TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cap_w, cap_h,
                                 GL_RGBA, GL_UNSIGNED_BYTE, px);
                t_tex1 = ClockCycles();
                cap_fps++;
                got_frame = 1;
            } else {
                cap_fails++;
                if (cap_fails <= 5 || cap_fails % 100 == 0)
                    LOG("  [!] screen_read_display rc=%d errno=%d fails=%u\n",
                           rc, errno, cap_fails);
                if (cap_fails > 150) { LOG("  [!] too many failures, exiting\n"); break; }
                usleep(20000);
            }
        }
        (void)got_frame;

        gl_Viewport(0, 0, DISP_W, DISP_H);
        gl_ClearColor(0, 0, 0, 1);
        gl_Clear(GL_COLOR_BUFFER_BIT);
        gl_ActiveTexture(GL_TEXTURE0);
        gl_BindTexture(GL_TEXTURE_2D, tex_y);
        if (shm_yuv) {
            gl_ActiveTexture(GL_TEXTURE1);
            gl_BindTexture(GL_TEXTURE_2D, tex_v);
            gl_ActiveTexture(GL_TEXTURE2);
            gl_BindTexture(GL_TEXTURE_2D, tex_u);
        }
        gl_VertexAttribPointer(pos_loc, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), verts);
        gl_VertexAttribPointer(uv_loc,  2, GL_FLOAT, GL_FALSE, 4*sizeof(float), verts+2);
        t_draw0 = ClockCycles();
        gl_DrawArrays(GL_TRIANGLES, 0, 6);
        t_draw1 = ClockCycles();
        t_swap0 = ClockCycles();
        eglSwapBuffers(egl_dpy, egl_surf);
        t_swap1 = ClockCycles();
        render_fps++;

        uint64_t t1 = ClockCycles();
        time_t now = time(NULL);
        if (now != fps_ts) {
            LOG("  render=%u fps  cap=%u fps"
                   "  rd=%lluus  tex=%lluus  draw=%lluus  swap=%lluus\n",
                   render_fps, cap_fps,
                   (unsigned long long)cc_to_us(t1 - t0),
                   (unsigned long long)cc_to_us(t_tex1 - t_tex0),
                   (unsigned long long)cc_to_us(t_draw1 - t_draw0),
                   (unsigned long long)cc_to_us(t_swap1 - t_swap0));
            render_fps = 0; cap_fps = 0; fps_ts = now;
        }
    }

    return 0;
}
/* Args populated by run_h264_mode (or the STANDALONE main below). Read
 * inside cb_display_picture to compute SCREEN_BLIT_* attribs from
 * mode / zoom / pan. */
struct h264_args {
    const char* mode;     /* letter, fill, stretch, native */
    float zoomX, zoomY;
    float panX, panY;
    int   xres, yres;     /* cluster window (output) dimensions */
};
static struct h264_args g_h264_args = {
    .mode = "letter", .zoomX = 1.0f, .zoomY = 1.0f,
    .panX = 0.0f, .panY = 0.0f, .xres = 0, .yres = 0,
};

/* Three-stage render state for daemon-controlled h264 lifecycle:
 *   STAGE_PREPARE   — decoder running, NOT posting to displayable 33.
 *                     post_win created with VISIBLE=0 so other apps own 33.
 *   STAGE_RENDERING — full pipeline: blit + screen_post_window per decoded frame.
 *   STAGE_PAUSED    — like PREPARE: decoder runs, post_win VISIBLE=0, no posts.
 *
 * SIGUSR1 → request promote to RENDERING (from PREPARE or PAUSED).
 * SIGUSR2 → request pause (RENDERING → PAUSED).
 * SIGTERM → exit.
 *
 * Whether Java actually asks h264 to pause is its own decision — daemon just
 * forwards signals. Today we keep h264 rendering through HMI focus changes
 * (cluster channel is independent of HMI video focus); pause path exists for
 * when/if we change that.
 *
 * Default state (no `prepare` arg) is RENDERING — backward-compatible with
 * direct CLI invocation (`cluster capture=h264 ...`). */
typedef enum { STAGE_PREPARE = 0, STAGE_RENDERING = 1, STAGE_PAUSED = 2 } render_stage_t;
static volatile sig_atomic_t g_render_stage = STAGE_RENDERING;
static volatile sig_atomic_t g_resume_requested = 0;
static volatile sig_atomic_t g_pause_requested  = 0;

static void on_sigusr1(int sig) { (void)sig; g_resume_requested = 1; }
static void on_sigusr2(int sig) { (void)sig; g_pause_requested  = 1; }


/* DISP_W / DISP_H are runtime statics declared at file top (line ~80).
 * Auto-detected from displayable 33 at startup, overridable via xres=/yres=.
 * Earlier this section had its own `#define DISP_W 1280` / `DISP_H 860`
 * which shadowed the statics for all code from here to EOF — removed. */
#define MAX_SURFACES 8

/* ------------------------------------------------------------------ */
/*  NvMedia function types (all loaded via dlsym)                     */
/* ------------------------------------------------------------------ */
typedef void* (*fn_device_create)(void);
typedef void  (*fn_device_destroy)(void*);
typedef void* (*fn_decoder_create)(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
typedef void  (*fn_destroy)(void*);
typedef int   (*fn_decoder_render)(void*,void*,void*,uint32_t,void*);
typedef void  (*fn_decoder_set_attr)(void*,uint32_t,void*);
typedef void* (*fn_mixer_create)(void*,uint32_t,uint32_t,float,
               uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
typedef int   (*fn_mixer_render)(void*,void*,uint32_t,void*,uint32_t,uint32_t,uint32_t);
typedef void* (*fn_surface_create)(void*,uint32_t,uint32_t,uint32_t);
typedef int   (*fn_surface_wait)(void*,uint32_t);
typedef int   (*fn_surface_lock)(void*,void*);
typedef void  (*fn_surface_unlock)(void*);
typedef void* (*fn_parser_create)(void*);
typedef int   (*fn_parser_set_attr)(void*,uint32_t,uint32_t,void*);
typedef int   (*fn_parser_parse)(void*,void*);
typedef void  (*fn_parser_flush)(void*);
typedef void  (*fn_parser_destroy)(void*);
typedef int   (*fn_sibling_create)(void*,void*,void**);
/* NvMedia2D — alternate to NvMedia mixer for YV12→RGBA conversion. */
typedef void* (*fn_2d_create)(void*);
typedef void  (*fn_2d_destroy)(void*);
typedef int   (*fn_2d_blit)(void*, void*, void*, void*, void*, void*);
typedef int   (*fn_2d_blit_ext)(void*, void*, void*, void*, void*, void*);

static struct {
    fn_device_create    DeviceCreate;
    fn_device_destroy   DeviceDestroy;
    fn_decoder_create   DecoderCreate;
    fn_destroy          DecoderDestroy;
    fn_decoder_render   DecoderRender;
    fn_decoder_set_attr DecoderSetAttr;
    fn_mixer_create     MixerCreate;
    fn_destroy          MixerDestroy;
    fn_mixer_render     MixerRender;
    fn_surface_create   SurfaceCreate;
    fn_destroy          SurfaceDestroy;
    fn_surface_wait     SurfaceWait;
    fn_surface_lock     SurfaceLock;
    fn_surface_unlock   SurfaceUnlock;
    fn_parser_create    ParserCreate;
    fn_parser_set_attr  ParserSetAttr;
    fn_parser_parse     ParserParse;
    fn_parser_flush     ParserFlush;
    fn_parser_destroy   ParserDestroy;
    fn_sibling_create   SiblingCreate;
    fn_2d_create        TwoDCreate;
    fn_2d_destroy       TwoDDestroy;
    fn_2d_blit          TwoDBlit;
    fn_2d_blit_ext      TwoDBlitExt;
} nv;

static int load_nvmedia(void) {
    void* h1 = dlopen("libnvmedia.so", RTLD_NOW);
    void* h2 = dlopen("libnvparser.so", RTLD_NOW);
    if (!h1) { LOG("dlopen libnvmedia.so failed: %s\n", dlerror()); return -1; }
    if (!h2) { LOG("dlopen libnvparser.so failed: %s\n", dlerror()); return -1; }

    nv.DeviceCreate  = (fn_device_create)dlsym(h1, "NvMediaDeviceCreate");
    nv.DeviceDestroy = (fn_device_destroy)dlsym(h1, "NvMediaDeviceDestroy");
    nv.DecoderCreate = (fn_decoder_create)dlsym(h1, "NvMediaVideoDecoderCreateEx");
    nv.DecoderDestroy= (fn_destroy)dlsym(h1, "NvMediaVideoDecoderDestroy");
    nv.DecoderRender = (fn_decoder_render)dlsym(h1, "NvMediaVideoDecoderRender");
    nv.DecoderSetAttr= (fn_decoder_set_attr)dlsym(h1, "NvMediaVideoDecoderSetAttributes");
    nv.MixerCreate   = (fn_mixer_create)dlsym(h1, "NvMediaVideoMixerCreate");
    nv.MixerDestroy  = (fn_destroy)dlsym(h1, "NvMediaVideoMixerDestroy");
    nv.MixerRender   = (fn_mixer_render)dlsym(h1, "NvMediaVideoMixerRenderSurface");
    nv.SurfaceCreate = (fn_surface_create)dlsym(h1, "NvMediaVideoSurfaceCreate");
    nv.SurfaceDestroy= (fn_destroy)dlsym(h1, "NvMediaVideoSurfaceDestroy");
    nv.SurfaceWait   = (fn_surface_wait)dlsym(h1, "NvMediaVideoSurfaceWaitForCompletion");
    nv.SurfaceLock   = (fn_surface_lock)dlsym(h1, "NvMediaVideoSurfaceLock");
    nv.SurfaceUnlock = (fn_surface_unlock)dlsym(h1, "NvMediaVideoSurfaceUnlock");
    nv.SiblingCreate = (fn_sibling_create)dlsym(h1, "NvxScreenCreateNvMediaVideoSurfaceSibling");
    nv.TwoDCreate    = (fn_2d_create)dlsym(h1, "NvMedia2DCreate");
    nv.TwoDDestroy   = (fn_2d_destroy)dlsym(h1, "NvMedia2DDestroy");
    nv.TwoDBlit      = (fn_2d_blit)dlsym(h1, "NvMedia2DBlit");
    nv.TwoDBlitExt   = (fn_2d_blit_ext)dlsym(h1, "NvMedia2DBlitExt");
    nv.ParserCreate  = (fn_parser_create)dlsym(h2, "video_parser_create");
    nv.ParserSetAttr = (fn_parser_set_attr)dlsym(h2, "video_parser_set_attribute");
    nv.ParserParse   = (fn_parser_parse)dlsym(h2, "video_parser_parse");
    nv.ParserFlush   = (fn_parser_flush)dlsym(h2, "video_parser_flush");
    nv.ParserDestroy = (fn_parser_destroy)dlsym(h2, "video_parser_destroy");

    if (!nv.DeviceCreate || !nv.DecoderCreate || !nv.ParserCreate || !nv.ParserParse) {
        LOG("critical NvMedia symbols missing\n");
        return -1;
    }
    LOG("NvMedia loaded OK\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Decoder state                                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    void* device;
    void* decoder;
    void* mixer;
    void* nvm2d;            /* NvMedia2D handle for variant 2 (-DUSE_NVMEDIA2D) */
    void* surfaces[MAX_SURFACES];
    int   surf_refcount[MAX_SURFACES];
    int   num_surfaces;
    int   width;
    int   height;
    int   frames_decoded;
    int   frames_displayed;
    int   frame_counter;      /* monotonic counter written to pCurrPic[5] per gal */
    /* NvMedia post buffers — used as decode targets via siblings (gal0 pattern).
     * 8 surfaces gives the H.264 DPB enough headroom; with 2 the parser
     * clobbers references every frame for P-slice decode. */
    screen_context_t scr_ctx;
    screen_window_t  post_win;
    screen_buffer_t  post_bufs[MAX_SURFACES];
    void*  scr_siblings[MAX_SURFACES];   /* RGBA siblings (mixer dest) */
    int    post_buf_idx;
    /* OPTION B-PRIME v1: separate YV12 window for decode targets so the
     * decoder writes via the sibling path (sets TVMR +8 flag correctly). */
    screen_window_t  decode_win;
    screen_buffer_t  decode_bufs[MAX_SURFACES];
    void*  decode_siblings[MAX_SURFACES];  /* YV12 siblings (decode targets) */
    /* Read pixmap for screen_read_window (CPU-readable RGBA) */
    screen_pixmap_t  read_pix;
    screen_buffer_t  read_buf;
    void*  read_ptr;
    int    read_stride;
    /* EGL display on display 33 */
    screen_window_t  egl_win;
    EGLDisplay egl_dpy;
    EGLSurface egl_surf;
    EGLContext egl_ctx;
    GLuint tex_y, tex_v, tex_u;
    GLuint prog;
    GLint  pos_loc, uv_loc;
    int    scr_ready;
    /* canim splash buffer loaded once at h264 child startup; staged here
     * and later memcpy'd into canim_pixmap in setup_screen. NULL if load
     * failed — pause then leaves the last decoded frame frozen. */
    uint8_t* canim_buf;
    /* Pixmap holding the canim splash at post_win buffer dimensions
     * (g_dec.width × g_dec.height). Filled once in setup_screen from
     * canim_buf, then on each pause we screen_blit it into post_bufs[pidx]
     * and screen_post_window — same blit mechanism the mixer path uses.
     * Single window on disp 33, no second-window competition. */
    screen_pixmap_t canim_pixmap;
    screen_buffer_t canim_pix_buf;
    /* boot_pixmap: fifthBro splash at post_win dimensions. Loaded once at
     * setup_screen from the embedded boot_canim_data. Blitted to post_buf
     * on STAGE_PREPARE → RENDERING transition so the post_win has visible
     * content when VISIBLE flips to 1, instead of a blank/black frame. */
    screen_pixmap_t boot_pixmap;
    screen_buffer_t boot_pix_buf;
} DecoderState;

static DecoderState g_dec = {0};

/* ------------------------------------------------------------------ */
/*  Screen display setup (display 33)                                 */
/* ------------------------------------------------------------------ */
static int setup_screen(int width, int height) {
    int rc;
    /* Block daemon signals (SIGUSR1=resume, SIGUSR2=pause) for the duration
     * of this function. QNX 6.6 has no SA_RESTART, so without this a signal
     * arriving mid-screen-call (e.g. resume sent right after prepare while
     * we're still creating window buffers) returns EINTR and we fail with
     * scr_ready=0 — disp 33 never comes up. The signal stays pending and
     * fires immediately after we unblock at the end of setup. */
    sigset_t sig_mask, old_sig_mask;
    sigemptyset(&sig_mask);
    sigaddset(&sig_mask, SIGUSR1);
    sigaddset(&sig_mask, SIGUSR2);
    sigprocmask(SIG_BLOCK, &sig_mask, &old_sig_mask);
    rc = screen_create_context(&g_dec.scr_ctx, 0 /* APPLICATION_CONTEXT */);
    if (rc) {
        LOG("screen_create_context failed\n");
        sigprocmask(SIG_SETMASK, &old_sig_mask, NULL);
        return -1;
    }

    /* --- Post window: replicate DINT setup exactly (same as gal's video window) --- */
    {
        typedef int (*fn_cwt)(screen_window_t*, screen_context_t, int);
        fn_cwt cwt = (fn_cwt)dlsym(RTLD_DEFAULT, "screen_create_window_type");
        if (cwt) rc = cwt(&g_dec.post_win, g_dec.scr_ctx, 0);
        else     rc = screen_create_window(&g_dec.post_win, g_dec.scr_ctx);
        if (rc) {
            LOG("create post_win failed rc=%d\n", rc);
            sigprocmask(SIG_SETMASK, &old_sig_mask, NULL);
            return -1;
        }
    }
    /* OPTION B-PRIME v1: decode_win = YV12 non-displayable window for decode
     * sibling targets. Decoder writes via sibling path so TVMR +8 flag is set.
     * Then mixer reads from decode_siblings → writes to post_win's RGBA
     * siblings → screen_post to cluster. */
    {
        typedef int (*fn_cwt)(screen_window_t*, screen_context_t, int);
        fn_cwt cwt = (fn_cwt)dlsym(RTLD_DEFAULT, "screen_create_window_type");
        if (cwt) rc = cwt(&g_dec.decode_win, g_dec.scr_ctx, 0);
        else     rc = screen_create_window(&g_dec.decode_win, g_dec.scr_ctx);
        if (rc) {
            LOG("create decode_win failed rc=%d\n", rc);
            sigprocmask(SIG_SETMASK, &old_sig_mask, NULL);
            return -1;
        }
    }
    int bh_yv12 = ((height * 3 / 2) + 63) & ~63;
    int dec_buffer_size[2] = { width, bh_yv12 };  /* YV12 storage incl. chroma planes */
    int dec_visible_size[2] = { width, height };  /* visible Y-plane dims */
    int dec_fmt = 13;          /* YV12 */
    int dec_usage = 0x480;     /* gal0's exact usage */
    screen_set_window_property_iv(g_dec.decode_win, 0x28, dec_visible_size); /* SIZE = visible */
    screen_set_window_property_iv(g_dec.decode_win, 0x05, dec_buffer_size);  /* BUFFER_SIZE = storage */
    screen_set_window_property_iv(g_dec.decode_win, 0x0e, &dec_fmt);
    screen_set_window_property_iv(g_dec.decode_win, 0x30, &dec_usage);
    /* No ID_STRING / VISIBLE — this window is not displayed, only for decode buffers */
    /* Retry on EINTR — daemon may send SIGUSR1 (resume) right after spawning,
     * and QNX 6.6 has no SA_RESTART so a long-running screen syscall can
     * return -1 errno=4 even with sigprocmask blocking (apparently the
     * block doesn't fully suppress EINTR here). */
    do {
        rc = screen_create_window_buffers(g_dec.decode_win, MAX_SURFACES);
    } while (rc < 0 && errno == EINTR);
    if (rc) {
        LOG("create decode_win buffers failed rc=%d errno=%d\n", rc, errno);
        sigprocmask(SIG_SETMASK, &old_sig_mask, NULL);
        return -1;
    }
    screen_get_window_property_pv(g_dec.decode_win, SCREEN_PROPERTY_RENDER_BUFFERS,
                                  (void**)g_dec.decode_bufs);
    LOG("  decode_win YV12: %dx%d (bh=%d) bufs[0]=%p\n", width, height, bh_yv12, g_dec.decode_bufs[0]);
    if (nv.SiblingCreate && g_dec.device) {
        for (int i = 0; i < MAX_SURFACES; i++) {
            int sr = nv.SiblingCreate(g_dec.device, g_dec.decode_bufs[i], &g_dec.decode_siblings[i]);
            if (sr != 0) { LOG("  decode_sibling[%d] FAILED rc=%d\n", i, sr); g_dec.decode_siblings[i] = NULL; }
        }
        LOG("  decode_siblings[0..%d]=%p..%p\n", MAX_SURFACES-1,
            g_dec.decode_siblings[0], g_dec.decode_siblings[MAX_SURFACES-1]);
    }

    /* post_win = RGBX8888 displayable. RGBA siblings will be mixer outputs. */
    /* post_win sized to the detected (or argv-overridden) cluster panel. The
     * H.264 stream may be smaller than the panel (e.g. 480p stream → 1280×860
     * panel); our blit upscales it via `mode=stretch` so the panel is always
     * fully covered. xres=/yres= argv override the autodetected DISP_W/DISP_H. */
    int win_size[2] = { DISP_W, DISP_H };
    int pfmt = SCREEN_FORMAT_RGBX8888;       /* 8 — matches Option A egl_win that bound */
#ifdef USE_SCREEN_BLIT
    /* v3: post_win must accept compositor writes (screen_blit destination)
     * AND remain a cluster displayable. NATIVE+WRITE for blit dest, OPENGL_ES2
     * preserved so id_string="33" binding still succeeds. */
    int pusage = SCREEN_USAGE_NATIVE | SCREEN_USAGE_WRITE | SCREEN_USAGE_OPENGL_ES2;
#else
    /* OPENGL_ES2 only — keep post_win clean for the NvMedia mixer's HW write
     * path. Canim is painted by a SEPARATE window (canim_win, created below)
     * which carries the WRITE flag; post_win never sees a CPU memcpy. */
    int pusage = SCREEN_USAGE_OPENGL_ES2;
#endif
    /* In PREPARE stage we create post_win invisible so we don't claim
     * displayable 33 yet; daemon will flip VISIBLE=1 on resume. After the
     * first PREPARE→RENDERING flip, VISIBLE stays 1 for the session — pause
     * just screen_blits canim into the next post_buf and posts it (single
     * window owns disp 33 throughout, no second-window competition). */
    int vis = (g_render_stage == STAGE_PREPARE) ? 0 : 1;
    screen_set_window_property_iv(g_dec.post_win, 0x28, win_size);   /* SIZE */
    screen_set_window_property_iv(g_dec.post_win, 0x05, win_size);   /* BUFFER_SIZE = same as SIZE for RGBA */
    screen_set_window_property_iv(g_dec.post_win, 0x0e, &pfmt);      /* FORMAT=RGBX8888 */
    screen_set_window_property_iv(g_dec.post_win, 0x30, &pusage);    /* USAGE=OPENGL_ES2 */
    /* OPTION B: post_win IS the cluster displayable. Bind to id_string="33"
     * and make visible so screen_post_window directly displays YV12 on
     * cluster (compositor handles YV12→RGB on the display controller).
     *
     * NOTE: NO screen_manage_window call — that puts the window into
     * libdisplayinit's "managed" namespace and seems to bypass the normal
     * displayable id_string routing. The egl_win that worked under Option A
     * also did NOT call screen_manage_window. */
    int id_rc = screen_set_window_property_cv(g_dec.post_win,
                                               SCREEN_PROPERTY_ID_STRING, 2, "33");
    int vis_rc = screen_set_window_property_iv(g_dec.post_win,
                                                SCREEN_PROPERTY_VISIBLE, &vis);
    /* Flush so property changes are visible to displaymanager BEFORE we
     * allocate buffers (matches the egl_win pattern). */
    screen_flush_context(g_dec.scr_ctx, 0);
    /* Retry on EINTR — see decode_win buffers comment above. */
    do {
        rc = screen_create_window_buffers(g_dec.post_win, MAX_SURFACES);
    } while (rc < 0 && errno == EINTR);
    if (rc) {
        LOG("create post_win buffers failed rc=%d errno=%d\n", rc, errno);
        sigprocmask(SIG_SETMASK, &old_sig_mask, NULL);
        return -1;
    }
    screen_get_window_property_pv(g_dec.post_win, SCREEN_PROPERTY_RENDER_BUFFERS,
                                  (void**)g_dec.post_bufs);
    /* Verify ID_STRING and VISIBLE actually took effect — read back. */
    char idbuf[16] = {0};
    int got_vis = 0;
    screen_get_window_property_cv(g_dec.post_win, SCREEN_PROPERTY_ID_STRING,
                                   sizeof(idbuf), idbuf);
    screen_get_window_property_iv(g_dec.post_win, SCREEN_PROPERTY_VISIBLE, &got_vis);
    LOG("  post_win: %dx%d fmt=%d usage=0x%x set_id_rc=%d set_vis_rc=%d "
        "got_id=\"%s\" got_visible=%d (n=%d bufs=[%p,%p])\n",
        width, height, pfmt, pusage,
        id_rc, vis_rc, idbuf, got_vis, MAX_SURFACES,
        g_dec.post_bufs[0], g_dec.post_bufs[1]);

    /* --- canim_pixmap: CPU-writable pixmap holding the Porsche splash at
     * post_win buffer dimensions. On pause, screen_blit copies it into the
     * next post_buf — same screen API mechanism the mixer-output path uses
     * (USE_SCREEN_BLIT) every frame. Single window (post_win) owns disp 33
     * throughout; no second-window competition, no VISIBLE flip, no z-order. */
    if (g_dec.canim_buf) {
        int prc = screen_create_pixmap(&g_dec.canim_pixmap, g_dec.scr_ctx);
        if (prc) {
            LOG("  canim_pixmap create failed rc=%d errno=%d — splash disabled\n",
                prc, errno);
            g_dec.canim_pixmap = NULL;
        } else {
            int pix_fmt = SCREEN_FORMAT_RGBX8888;
            int pix_usage = SCREEN_USAGE_WRITE | SCREEN_USAGE_NATIVE;
            int pix_size[2] = { width, height };
            screen_set_pixmap_property_iv(g_dec.canim_pixmap, SCREEN_PROPERTY_FORMAT,      &pix_fmt);
            screen_set_pixmap_property_iv(g_dec.canim_pixmap, SCREEN_PROPERTY_USAGE,       &pix_usage);
            screen_set_pixmap_property_iv(g_dec.canim_pixmap, SCREEN_PROPERTY_BUFFER_SIZE, pix_size);
            int pbrc;
            do {
                pbrc = screen_create_pixmap_buffer(g_dec.canim_pixmap);
            } while (pbrc < 0 && errno == EINTR);
            if (pbrc) {
                LOG("  canim_pixmap buffer failed rc=%d errno=%d — splash disabled\n",
                    pbrc, errno);
                screen_destroy_pixmap(g_dec.canim_pixmap);
                g_dec.canim_pixmap = NULL;
            } else {
                g_dec.canim_pix_buf = NULL;
                screen_get_pixmap_property_pv(g_dec.canim_pixmap,
                                              SCREEN_PROPERTY_RENDER_BUFFERS,
                                              (void**)&g_dec.canim_pix_buf);
                void* pix_ptr = NULL;
                int pix_stride = 0;
                screen_get_buffer_property_pv(g_dec.canim_pix_buf,
                                              SCREEN_PROPERTY_POINTER, &pix_ptr);
                screen_get_buffer_property_iv(g_dec.canim_pix_buf,
                                              SCREEN_PROPERTY_STRIDE, &pix_stride);
                /* Re-render the canim into the pixmap at post_buf dimensions.
                 * canim_load_into_buf handles centering + crop/pad — does not
                 * need to match the staging canim_buf (DISP_W × DISP_H).
                 * swap_rb=0 → RGBA byte order, same as the mixer's RGBA
                 * sibling output (screen_blit is identity-blit between
                 * RGBX8888 buffers, no R/B swap). */
                if (pix_ptr && pix_stride > 0) {
                    int lr = canim_load_into_buf(CANIM_DEFAULT_PATH, pix_ptr,
                                                 pix_stride, width, height, 0);
                    LOG("  canim_pixmap: %dx%d stride=%d ptr=%p load_rc=%d\n",
                        width, height, pix_stride, pix_ptr, lr);
                    if (lr != 0) {
                        screen_destroy_pixmap(g_dec.canim_pixmap);
                        g_dec.canim_pixmap = NULL;
                        g_dec.canim_pix_buf = NULL;
                    }
                } else {
                    LOG("  canim_pixmap: %dx%d ptr=%p stride=%d — no CPU access, "
                        "splash disabled\n", width, height, pix_ptr, pix_stride);
                    screen_destroy_pixmap(g_dec.canim_pixmap);
                    g_dec.canim_pixmap = NULL;
                    g_dec.canim_pix_buf = NULL;
                }
            }
        }
    } else {
        LOG("  canim_pixmap: skipped (canim_buf not loaded — splash disabled)\n");
    }

    /* --- boot_pixmap: fifthBro splash, same layout as canim_pixmap. Loaded
     * from the embedded boot_canim_data byte array (no FS read). Used by
     * apply_stage_signals on PREPARE→RENDERING so the resume transition has
     * a visible posted frame instead of blank.
     */
    {
        int prc = screen_create_pixmap(&g_dec.boot_pixmap, g_dec.scr_ctx);
        if (prc) {
            LOG("  boot_pixmap create failed rc=%d errno=%d — boot splash disabled\n",
                prc, errno);
            g_dec.boot_pixmap = NULL;
        } else {
            int pix_fmt = SCREEN_FORMAT_RGBX8888;
            int pix_usage = SCREEN_USAGE_WRITE | SCREEN_USAGE_NATIVE;
            int pix_size[2] = { width, height };
            screen_set_pixmap_property_iv(g_dec.boot_pixmap, SCREEN_PROPERTY_FORMAT,      &pix_fmt);
            screen_set_pixmap_property_iv(g_dec.boot_pixmap, SCREEN_PROPERTY_USAGE,       &pix_usage);
            screen_set_pixmap_property_iv(g_dec.boot_pixmap, SCREEN_PROPERTY_BUFFER_SIZE, pix_size);
            int pbrc;
            do {
                pbrc = screen_create_pixmap_buffer(g_dec.boot_pixmap);
            } while (pbrc < 0 && errno == EINTR);
            if (pbrc) {
                LOG("  boot_pixmap buffer failed rc=%d errno=%d — boot splash disabled\n",
                    pbrc, errno);
                screen_destroy_pixmap(g_dec.boot_pixmap);
                g_dec.boot_pixmap = NULL;
            } else {
                g_dec.boot_pix_buf = NULL;
                screen_get_pixmap_property_pv(g_dec.boot_pixmap,
                                              SCREEN_PROPERTY_RENDER_BUFFERS,
                                              (void**)&g_dec.boot_pix_buf);
                void* pix_ptr = NULL;
                int pix_stride = 0;
                screen_get_buffer_property_pv(g_dec.boot_pix_buf,
                                              SCREEN_PROPERTY_POINTER, &pix_ptr);
                screen_get_buffer_property_iv(g_dec.boot_pix_buf,
                                              SCREEN_PROPERTY_STRIDE, &pix_stride);
                if (pix_ptr && pix_stride > 0) {
                    int lr = canim_load_from_mem(boot_canim_data, boot_canim_data_len,
                                                 pix_ptr, pix_stride, width, height, 0);
                    LOG("  boot_pixmap: %dx%d stride=%d ptr=%p load_rc=%d\n",
                        width, height, pix_stride, pix_ptr, lr);
                    if (lr != 0) {
                        screen_destroy_pixmap(g_dec.boot_pixmap);
                        g_dec.boot_pixmap = NULL;
                        g_dec.boot_pix_buf = NULL;
                    }
                } else {
                    LOG("  boot_pixmap: %dx%d ptr=%p stride=%d — no CPU access, "
                        "boot splash disabled\n", width, height, pix_ptr, pix_stride);
                    screen_destroy_pixmap(g_dec.boot_pixmap);
                    g_dec.boot_pixmap = NULL;
                    g_dec.boot_pix_buf = NULL;
                }
            }
        }
    }

    /* NvxScreen siblings for post buffers — these will be the decoder's
     * write targets (gal0 pattern). Decoder writes YV12 directly into the
     * post_buf's GPU memory; no mixer copy step needed. */
    if (nv.SiblingCreate && g_dec.device) {
        int i;
        for (i = 0; i < MAX_SURFACES; i++) {
            int sr = nv.SiblingCreate(g_dec.device, g_dec.post_bufs[i], &g_dec.scr_siblings[i]);
            LOG("  sibling[%d]: rc=%d nv_surf=%p\n", i, sr, g_dec.scr_siblings[i]);
            if (sr != 0) g_dec.scr_siblings[i] = NULL;
        }
    }

#if 0 /* OPTION B: read_pix + egl_win + EGL + GL all unused — post_win renders YV12 directly. */
    /* Read pixmap: screen_read_window target (CPU-readable RGBA) */
    rc = screen_create_pixmap(&g_dec.read_pix, g_dec.scr_ctx);
    if (rc) { LOG("create read_pix failed\n"); return -1; }
    int rfmt = SCREEN_FORMAT_RGBX8888;
    int rusage = SCREEN_USAGE_READ | SCREEN_USAGE_WRITE;
    screen_set_pixmap_property_iv(g_dec.read_pix, SCREEN_PROPERTY_FORMAT, &rfmt);
    screen_set_pixmap_property_iv(g_dec.read_pix, SCREEN_PROPERTY_USAGE, &rusage);
    screen_set_pixmap_property_iv(g_dec.read_pix, SCREEN_PROPERTY_BUFFER_SIZE, pbsize);
    rc = screen_create_pixmap_buffer(g_dec.read_pix);
    if (rc) { LOG("create read_pix buffer failed\n"); return -1; }
    screen_get_pixmap_property_pv(g_dec.read_pix, SCREEN_PROPERTY_RENDER_BUFFERS,
                                  (void**)&g_dec.read_buf);
    screen_get_buffer_property_pv(g_dec.read_buf, SCREEN_PROPERTY_POINTER,
                                  (void**)&g_dec.read_ptr);
    screen_get_buffer_property_iv(g_dec.read_buf, SCREEN_PROPERTY_STRIDE,
                                  &g_dec.read_stride);
    LOG("  read_pix: %dx%d RGBA ptr=%p stride=%d\n",
        width, height, g_dec.read_ptr, g_dec.read_stride);

    /* --- EGL window on display 33 (same setup as aa_cluster_mirror) --- */
    rc = screen_create_window(&g_dec.egl_win, g_dec.scr_ctx);
    if (rc) { LOG("create egl_win failed\n"); return -1; }
    screen_set_window_property_cv(g_dec.egl_win, SCREEN_PROPERTY_ID_STRING, 2, "33");
    int esize[2] = { DISP_W, DISP_H };
    int efmt = SCREEN_FORMAT_RGBX8888;
    int eusage = SCREEN_USAGE_OPENGL_ES2;
    int evis = 1;
    screen_set_window_property_iv(g_dec.egl_win, SCREEN_PROPERTY_SIZE, esize);
    screen_set_window_property_iv(g_dec.egl_win, SCREEN_PROPERTY_BUFFER_SIZE, esize);
    screen_set_window_property_iv(g_dec.egl_win, SCREEN_PROPERTY_FORMAT, &efmt);
    screen_set_window_property_iv(g_dec.egl_win, SCREEN_PROPERTY_USAGE, &eusage);
    screen_set_window_property_iv(g_dec.egl_win, SCREEN_PROPERTY_VISIBLE, &evis);
    screen_flush_context(g_dec.scr_ctx, 0);
    rc = screen_create_window_buffers(g_dec.egl_win, 2);
    if (rc) { LOG("create egl_win buffers failed\n"); return -1; }

    /* EGL init */
    g_dec.egl_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(g_dec.egl_dpy, NULL, NULL);
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };
    EGLConfig egl_cfg; EGLint ncfg;
    eglChooseConfig(g_dec.egl_dpy, cfg_attr, &egl_cfg, 1, &ncfg);
    g_dec.egl_surf = eglCreateWindowSurface(g_dec.egl_dpy, egl_cfg,
                                             (EGLNativeWindowType)g_dec.egl_win, NULL);
    if (g_dec.egl_surf == EGL_NO_SURFACE) {
        LOG("eglCreateWindowSurface failed 0x%x\n", eglGetError()); return -1;
    }
    EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    g_dec.egl_ctx = eglCreateContext(g_dec.egl_dpy, egl_cfg, EGL_NO_CONTEXT, ctx_attr);
    if (g_dec.egl_ctx == EGL_NO_CONTEXT) { LOG("eglCreateContext failed\n"); return -1; }
    eglMakeCurrent(g_dec.egl_dpy, g_dec.egl_surf, g_dec.egl_surf, g_dec.egl_ctx);
    while (eglGetError() != EGL_SUCCESS) {}
    load_gl();
    LOG("  EGL ready\n");

    /* GL shader — YV12 → RGB (from aa_cluster_mirror2.c) */
    GLuint vs = gl_CreateShader(0x8B31); /* GL_VERTEX_SHADER */
    gl_ShaderSource(vs, 1, &vs_src, NULL);
    gl_CompileShader(vs);
    GLuint fs = gl_CreateShader(0x8B30); /* GL_FRAGMENT_SHADER */
    gl_ShaderSource(fs, 1, &fs_src, NULL);
    gl_CompileShader(fs);
    g_dec.prog = gl_CreateProgram();
    gl_AttachShader(g_dec.prog, vs);
    gl_AttachShader(g_dec.prog, fs);
    gl_LinkProgram(g_dec.prog);
    gl_UseProgram(g_dec.prog);
    g_dec.pos_loc = gl_GetAttribLocation(g_dec.prog, "pos");
    g_dec.uv_loc = gl_GetAttribLocation(g_dec.prog, "uv");
    gl_EnableVertexAttribArray(g_dec.pos_loc);
    gl_EnableVertexAttribArray(g_dec.uv_loc);
    gl_Uniform1i(gl_GetUniformLocation(g_dec.prog, "tex_y"), 0);
    gl_Uniform1i(gl_GetUniformLocation(g_dec.prog, "tex_v"), 1);
    gl_Uniform1i(gl_GetUniformLocation(g_dec.prog, "tex_u"), 2);

    /* tex_y is now used as a single RGBA texture (BGRA from screen_read_window;
     * shader does R↔B swap). Sized at read_pix's actual row stride width
     * (e.g. 832 for an 800-wide buffer with 3328-byte stride) so that
     * glTexSubImage2D can upload directly from the strided source — no
     * row-by-row pack memcpy needed. UV at draw time clamps to width/tex_w
     * to crop the padding columns. */
    int tex_w = g_dec.read_stride / 4;
    int uv_w = width / 2, uv_h = height / 2;
    void* zero_y = calloc((size_t)tex_w * (size_t)height, 4);
    void* zero_uv = calloc(uv_w * uv_h, 1);

    gl_GenTextures(1, &g_dec.tex_y);
    gl_ActiveTexture(0x84C0);
    gl_BindTexture(GL_TEXTURE_2D, g_dec.tex_y);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
    gl_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex_w, height, 0,
                  GL_RGBA, GL_UNSIGNED_BYTE, zero_y);

    gl_GenTextures(1, &g_dec.tex_v);
    gl_ActiveTexture(0x84C1);
    gl_BindTexture(GL_TEXTURE_2D, g_dec.tex_v);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
    gl_TexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, uv_w, uv_h, 0,
                  GL_LUMINANCE, GL_UNSIGNED_BYTE, zero_uv);

    gl_GenTextures(1, &g_dec.tex_u);
    gl_ActiveTexture(0x84C2);
    gl_BindTexture(GL_TEXTURE_2D, g_dec.tex_u);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
    gl_TexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, uv_w, uv_h, 0,
                  GL_LUMINANCE, GL_UNSIGNED_BYTE, zero_uv);

    free(zero_y); free(zero_uv);
    while (gl_GetError() != 0) {}
    LOG("  YV12 textures Y=%u V=%u U=%u (%dx%d / %dx%d)\n",
        g_dec.tex_y, g_dec.tex_v, g_dec.tex_u, width, height, uv_w, uv_h);
#endif /* OPTION B */

    g_dec.scr_ready = 1;
    LOG("screen setup complete (Option B: direct YV12 post to displayable 33)\n");
    sigprocmask(SIG_SETMASK, &old_sig_mask, NULL);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Parser callbacks                                                  */
/* ------------------------------------------------------------------ */

/* BeginSequence: SPS parsed → create decoder + mixer + surfaces */
static int cb_begin_sequence(void* user, int* si) {
    if (!si) return 0;
    int w = si[10], h = si[0xb], dpb = si[5];
    int bs_size = si[0x16];
    if (bs_size < 0x80000) bs_size = 0x100000;
    int nbuf = dpb + 4; if (nbuf < 4) nbuf = 4; if (nbuf > MAX_SURFACES) nbuf = MAX_SURFACES;

    LOG("BeginSequence: %dx%d dpb=%d bs_size=0x%x\n", w, h, dpb, bs_size);
    g_dec.width = w;
    g_dec.height = h;

    /* Setup screen display FIRST — siblings must exist before we use them
     * as decoder targets (gal0 sibling-as-target pattern). */
    if (!g_dec.scr_ready) setup_screen(w, h);

    /* Create decoder */
    if (!g_dec.decoder && nv.DecoderCreate) {
        g_dec.decoder = nv.DecoderCreate(0, (uint32_t)w, (uint32_t)h,
                                          (uint32_t)dpb, (uint32_t)bs_size, (uint32_t)nbuf, 0);
        LOG("  decoder=%p\n", g_dec.decoder);
        if (g_dec.decoder && nv.DecoderSetAttr) {
            int dpb_val = dpb;
            nv.DecoderSetAttr(g_dec.decoder, 1, &dpb_val);
        }
    }

    /* Mixer no longer used (siblings-as-target mode), but keep create for
     * possible fallback. */
    if (!g_dec.mixer && nv.MixerCreate && g_dec.device) {
        float aspect = (float)w / (float)h;
        g_dec.mixer = nv.MixerCreate(g_dec.device, (uint32_t)w, (uint32_t)h, aspect,
                                      0,0,0,0,0,0,0,0, 0x20000, 0);
        LOG("  mixer=%p (unused; siblings are decode targets)\n", g_dec.mixer);
    }

    /* OPTION B-PRIME v1: decode_siblings (YV12, from decode_win) as decoder
     * targets. These DO get the TVMR +8 flag set after decode (proven in
     * Option B basic config). Mixer will read these and render YV12 → RGBA
     * into post_win's siblings. */
    g_dec.num_surfaces = MAX_SURFACES;
    for (int i = 0; i < MAX_SURFACES; i++) {
        g_dec.surfaces[i] = g_dec.decode_siblings[i];
        g_dec.surf_refcount[i] = 0;
        LOG("  surface[%d]=%p (YV12 decode sibling)\n", i, g_dec.surfaces[i]);
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

    *(uint32_t*)(pi +  0) = *(const uint32_t*)(pd + 200);
    *(uint32_t*)(pi +  4) = *(const uint32_t*)(pd + 0xcc);
    *(uint32_t*)(pi +  8) = *(const uint32_t*)(pd + 0x24);

    *(uint16_t*)(pi + 12) = *(const uint16_t*)(pd + 0x2c);
    *(uint16_t*)(pi + 14) = *(const uint16_t*)(pd + 0xc4);

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

    *(uint32_t*)(pi + 0x2c) = *(const uint32_t*)(pd + 0xd8);
    *(uint32_t*)(pi + 0x30) = *(const uint32_t*)(pd + 0xdc);

    pi[0x34] = *(const uint8_t*)(pd + 0xd0);
    pi[0x35] = *(const uint8_t*)(pd + 0xd1);

    memcpy(pi + 0x36, pd + 0x6a8, 0x60);
    memcpy(pi + 0x96, pd + 0x708, 0x80);

    *(uint32_t*)(pi + 0x2d8) = *(const uint32_t*)(pd + 0x44);
    *(uint32_t*)(pi + 0x2dc) = *(const uint32_t*)(pd + 0x48);

    /* Reference picture loop: 16 entries × 7 words. */
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

/* DecodePicture: parser prepared picture_info, decode the frame */
static int cb_decode_picture(void* user, void* pic_info) {
    if (!pic_info || !g_dec.decoder || !nv.DecoderRender) return 1;

    /* pic_info+0x08 = pCurrPic, pCurrPic+0x08 = target surface */
    int* pi = (int*)pic_info;
    int* curr_pic = (int*)pi[2];
    if (!curr_pic) return 1;
    void* target_surface = (void*)curr_pic[2];
    if (!target_surface) return 1;

    /* gal writes a frame counter to pCurrPic[5] before DecoderRender
     * (see gal.c:169320-169321). TVMR may dereference through pCurrPic. */
    g_dec.frame_counter++;
    curr_pic[5] = g_dec.frame_counter;

    /* arg5 is the bitstream descriptor {pBitstreamData, nBitstreamDataLen}
     * from pd[14] / pd[13] (see gal.c:169314-169315). MUST be static — TVMR
     * may read it asynchronously after this returns. */
    static struct { void* pBitstreamData; uint32_t nBitstreamDataLen; uint32_t pad[6]; }
        bs_desc __attribute__((aligned(8)));
    bs_desc.pBitstreamData    = (void*)(uintptr_t)pi[14];
    bs_desc.nBitstreamDataLen = (uint32_t)pi[13];

    /* Translate parser output to NvMedia pic_info (per gal::nvSetParamsH264) */
    static uint8_t nv_pi[1024] __attribute__((aligned(8)));
    cl_translate_h264_pd(pic_info, nv_pi);

    int rc = nv.DecoderRender(g_dec.decoder, target_surface, nv_pi, 1, &bs_desc);
    g_dec.frames_decoded++;
    if (g_dec.frames_decoded <= 5 || (g_dec.frames_decoded % 100) == 0) {
        LOG("DecodePicture: rc=%d surface=%p frame=%d bs_len=%u\n",
            rc, target_surface, g_dec.frames_decoded, bs_desc.nBitstreamDataLen);
    }
    return 1;
}

/* Apply pause/resume signals from daemon. On pause we screen_blit the canim
 * pixmap into the next post_buf and screen_post_window it — same mechanism
 * the mixer path uses every frame in USE_SCREEN_BLIT. Single window owns
 * disp 33 throughout; no second window, no VISIBLE flip, no z-order. Resume
 * just sets the stage — the mixer's next decoded frame overwrites canim.
 *
 * Called from BOTH cb_display_picture (so transitions happen mid-decode) AND
 * the main decode loop's idle path (so transitions also fire when no new
 * H.264 data is arriving — the pause case, where the phone has stopped
 * sending frames and cb_display_picture is no longer invoked). */
static void apply_stage_signals(void) {
    /* Gate the entire resume processing on scr_ready. If SIGUSR1 arrives
     * before setup_screen completes (e.g. during the SHM-wait at startup),
     * we must NOT flip g_render_stage here — setup_screen needs to see
     * STAGE_PREPARE to create post_win with VISIBLE=0. The signal stays
     * latched (g_resume_requested) and is processed on the next call after
     * scr_ready becomes true. */
    if (g_resume_requested && g_dec.scr_ready) {
        g_resume_requested = 0;
        if (g_render_stage == STAGE_PREPARE) {
            /* Blit boot_pixmap into the next post_buf and post it BEFORE
             * flipping VISIBLE=1. Without a posted buffer the compositor
             * shows blank when VISIBLE becomes 1; the splash is the bridge
             * while the decoder warms up and produces real frames. */
            int br = -1;
            if (g_dec.boot_pixmap && g_dec.boot_pix_buf) {
                int pidx = g_dec.post_buf_idx;
                g_dec.post_buf_idx = (pidx + 1) % MAX_SURFACES;
                int blit_attribs[] = { SCREEN_BLIT_END };
                br = screen_blit(g_dec.scr_ctx, g_dec.post_bufs[pidx],
                                 g_dec.boot_pix_buf, blit_attribs);
                int dirty[4] = { 0, 0, DISP_W, DISP_H };
                screen_post_window(g_dec.post_win, g_dec.post_bufs[pidx],
                                   1, dirty, 0);
            }
            int vis = 1;
            screen_set_window_property_iv(g_dec.post_win, SCREEN_PROPERTY_VISIBLE, &vis);
            screen_flush_context(g_dec.scr_ctx, 0);
            LOG("h264: resume → boot splash (blit rc=%d), decoder warms in background\n", br);
        }
        g_render_stage = STAGE_RENDERING;
        LOG("h264: resume → RENDERING (frame=%d)\n", g_dec.frames_displayed);
    }
    if (g_pause_requested) {
        g_pause_requested = 0;
        if (g_dec.scr_ready && g_dec.canim_pixmap && g_dec.canim_pix_buf) {
            int pidx = g_dec.post_buf_idx;
            g_dec.post_buf_idx = (pidx + 1) % MAX_SURFACES;
            int blit_attribs[] = { SCREEN_BLIT_END };
            int br = screen_blit(g_dec.scr_ctx, g_dec.post_bufs[pidx],
                                 g_dec.canim_pix_buf, blit_attribs);
            int dirty[4] = { 0, 0, DISP_W, DISP_H };
            screen_post_window(g_dec.post_win, g_dec.post_bufs[pidx],
                               1, dirty, 0);
            screen_flush_context(g_dec.scr_ctx, 0);
            LOG("h264: pause → PAUSED, canim blit rc=%d posted to pidx=%d "
                "(frame=%d)\n", br, pidx, g_dec.frames_displayed);
        } else {
            LOG("h264: pause → PAUSED, no canim (scr_ready=%d pixmap=%p buf=%p) "
                "(frame=%d)\n", g_dec.scr_ready, (void*)g_dec.canim_pixmap,
                (void*)g_dec.canim_pix_buf, g_dec.frames_displayed);
        }
        g_render_stage = STAGE_PAUSED;
    }
}

/* DisplayPicture: decoded frame ready →
 *   1. MixerRender into DINT sibling (YV12)
 *   2. Read YV12 planes directly from post_buf CPU pointer
 *   3. Upload Y/V/U to 3 GL textures
 *   4. YUV→RGB shader → eglSwapBuffers → display 33 */
static int cb_display_picture(void* user, void* disp_info) {
    g_dec.frames_displayed++;

    apply_stage_signals();

    if (!disp_info || !g_dec.scr_ready) return 1;

    int* di = (int*)disp_info;
    void* yuv_surface = (void*)di[2];
    if (!yuv_surface) return 1;

    /* OPTION B-PRIME: NvMedia mixer YV12 (yuv_surface) → RGBA (post sibling).
     * yuv_surface is a separate NvMediaVideoSurface (decoder target).
     * post sibling is the RGBA destination, paired with post_buf for screen_post. */
    int pidx = g_dec.post_buf_idx;
    g_dec.post_buf_idx = (pidx + 1) % MAX_SURFACES;
    void* dst_sibling = g_dec.scr_siblings[pidx];

    if (!dst_sibling || !g_dec.mixer || !nv.MixerRender) {
        if (nv.SurfaceWait) nv.SurfaceWait(yuv_surface, 0);
        if (g_dec.frames_displayed <= 3)
            LOG("DisplayPicture: frame=%d missing dst_sibling/mixer/MixerRender\n",
                g_dec.frames_displayed);
        return 1;
    }

    struct timespec ts0, ts1, ts2, ts3;
    clock_gettime(CLOCK_MONOTONIC, &ts0);

    /* Wait for decoder before mixer reads */
    if (nv.SurfaceWait) nv.SurfaceWait(yuv_surface, 0);
    clock_gettime(CLOCK_MONOTONIC, &ts1);

#ifdef USE_SCREEN_BLIT
    /* v3: compositor screen_blit YV12 (decode_buf) → RGBA (post_buf).
     * Explicit src/dst rects so blit operates on the visible 1280×720 Y area
     * only; the YV12 buffer is taller (1088 rows) for chroma packing and
     * default rect would stretch the whole thing. */
    int didx = -1;
    for (int i = 0; i < MAX_SURFACES; i++) {
        if (g_dec.decode_siblings[i] == yuv_surface) { didx = i; break; }
    }
    if (didx < 0) didx = pidx;
    /* Compute src/dst rects from g_h264_args (mode / zoom / pan / xres / yres).
     * Source = visible Y-plane area (g_dec.width × g_dec.height) inside the
     * larger YV12 buffer. Destination = post_win render buffer (xres × yres,
     * defaulting to the source size if not set). */
    int sx = 0, sy = 0, sw = g_dec.width, sh = g_dec.height;
    /* Default dst rect to the displayable size (post_win is DISP-sized). */
    int dst_w = (g_h264_args.xres > 0) ? g_h264_args.xres : DISP_W;
    int dst_h = (g_h264_args.yres > 0) ? g_h264_args.yres : DISP_H;
    int dx = 0, dy = 0, dw = dst_w, dh = dst_h;
    {
        float src_ar = (float)g_dec.width / (float)g_dec.height;
        float dst_ar = (float)dst_w / (float)dst_h;
        const char* m = g_h264_args.mode ? g_h264_args.mode : "letter";
        if (strcmp(m, "stretch") == 0) {
            /* dst = full, src = full — HW scales to fill */
        } else if (strcmp(m, "fill") == 0) {
            /* preserve aspect, crop source to fill dst */
            if (src_ar > dst_ar) {
                int new_sw = (int)((float)g_dec.height * dst_ar);
                sx = (g_dec.width - new_sw) / 2; sw = new_sw;
            } else {
                int new_sh = (int)((float)g_dec.width / dst_ar);
                sy = (g_dec.height - new_sh) / 2; sh = new_sh;
            }
        } else if (strcmp(m, "native") == 0) {
            /* 1:1 pixel mapping centered, with letterbox/pillarbox */
            int nw = g_dec.width  < dst_w ? g_dec.width  : dst_w;
            int nh = g_dec.height < dst_h ? g_dec.height : dst_h;
            dx = (dst_w - nw) / 2; dy = (dst_h - nh) / 2; dw = nw; dh = nh;
            sw = nw; sh = nh;
            sx = (g_dec.width - sw) / 2; sy = (g_dec.height - sh) / 2;
        } else {
            /* letter (default): preserve aspect, fit dst, letterbox/pillarbox */
            if (src_ar > dst_ar) {
                int new_dh = (int)((float)dst_w / src_ar);
                dy = (dst_h - new_dh) / 2; dh = new_dh;
            } else {
                int new_dw = (int)((float)dst_h * src_ar);
                dx = (dst_w - new_dw) / 2; dw = new_dw;
            }
        }
        /* zoom: shrink src around its center (after mode applied) */
        if (g_h264_args.zoomX > 0.0f && g_h264_args.zoomX != 1.0f) {
            int cx = sx + sw / 2; int new_sw = (int)((float)sw / g_h264_args.zoomX);
            sx = cx - new_sw / 2; sw = new_sw;
        }
        if (g_h264_args.zoomY > 0.0f && g_h264_args.zoomY != 1.0f) {
            int cy = sy + sh / 2; int new_sh = (int)((float)sh / g_h264_args.zoomY);
            sy = cy - new_sh / 2; sh = new_sh;
        }
        /* pan: shift src origin (positive = image moves left/up) */
        sx += (int)(g_h264_args.panX * (float)sw);
        sy += (int)(g_h264_args.panY * (float)sh);
        /* clamp src to the visible Y-plane area */
        if (sx < 0) sx = 0; if (sy < 0) sy = 0;
        if (sx + sw > g_dec.width)  sw = g_dec.width  - sx;
        if (sy + sh > g_dec.height) sh = g_dec.height - sy;
        if (sw < 1) sw = 1; if (sh < 1) sh = 1;
    }
    int blit_attribs[] = {
        SCREEN_BLIT_SOURCE_X,         sx,
        SCREEN_BLIT_SOURCE_Y,         sy,
        SCREEN_BLIT_SOURCE_WIDTH,     sw,
        SCREEN_BLIT_SOURCE_HEIGHT,    sh,
        SCREEN_BLIT_DESTINATION_X,    dx,
        SCREEN_BLIT_DESTINATION_Y,    dy,
        SCREEN_BLIT_DESTINATION_WIDTH, dw,
        SCREEN_BLIT_DESTINATION_HEIGHT, dh,
        SCREEN_BLIT_END
    };
    int mr = screen_blit(g_dec.scr_ctx, g_dec.post_bufs[pidx],
                         g_dec.decode_bufs[didx], blit_attribs);
#elif defined(USE_NVMEDIA2D)
    /* NvMedia2DBlit: YV12 src → RGBA dst. NULL rects = full surface, NULL params = defaults. */
    int mr = -1;
    if (nv.TwoDBlit && g_dec.nvm2d) {
        mr = nv.TwoDBlit(g_dec.nvm2d, dst_sibling, NULL, yuv_surface, NULL, NULL);
    } else if (nv.TwoDBlitExt && g_dec.nvm2d) {
        mr = nv.TwoDBlitExt(g_dec.nvm2d, dst_sibling, NULL, yuv_surface, NULL, NULL);
    }
    if (nv.SurfaceWait) nv.SurfaceWait(dst_sibling, 0);
#else
    /* Mixer: YV12 (yuv_surface) → RGBA (dst_sibling) */
    uint32_t video_desc[7] = {0};
    video_desc[0] = 0x20000;
    video_desc[2] = (uint32_t)yuv_surface;
    int mr = nv.MixerRender(g_dec.mixer, dst_sibling, 0, video_desc, 0, 0, 0);
    if (nv.SurfaceWait) nv.SurfaceWait(dst_sibling, 0);
#endif
    clock_gettime(CLOCK_MONOTONIC, &ts2);

    /* Post the RGBA buffer to cluster displayable, but ONLY if rendering.
     * In PREPARE/PAUSED we still blit (so the latest frame is in post_bufs[pidx]
     * ready to be posted on the next resume) but skip the post itself, leaving
     * displayable 33 free for splash or whatever else owns it. */
    int dirty[4] = { 0, 0, DISP_W, DISP_H };
    if (g_render_stage == STAGE_RENDERING) {
        screen_post_window(g_dec.post_win, g_dec.post_bufs[pidx], 1, dirty, 0);
        screen_flush_context(g_dec.scr_ctx, 0);
    }
    clock_gettime(CLOCK_MONOTONIC, &ts3);

    if (g_dec.frames_displayed <= 3 || (g_dec.frames_displayed % 100) == 0) {
        long us_wait  = (ts1.tv_sec-ts0.tv_sec)*1000000L + (ts1.tv_nsec-ts0.tv_nsec)/1000L;
        long us_mix   = (ts2.tv_sec-ts1.tv_sec)*1000000L + (ts2.tv_nsec-ts1.tv_nsec)/1000L;
        long us_post  = (ts3.tv_sec-ts2.tv_sec)*1000000L + (ts3.tv_nsec-ts2.tv_nsec)/1000L;
        long us_total = (ts3.tv_sec-ts0.tv_sec)*1000000L + (ts3.tv_nsec-ts0.tv_nsec)/1000L;
        LOG("frame=%d wait=%ldus mixer=%ldus(rc=%d) post+flush=%ldus total=%ldus pidx=%d\n",
            g_dec.frames_displayed, us_wait, us_mix, mr, us_post, us_total, pidx);
    }

    return 1;
}

/* AllocPictureBuffer: return a free surface */
static int cb_alloc_picture(void* user, void** pic_buf) {
    if (!pic_buf) return 0;
    /* Diagnostic: log first 12 alloc calls' state, then every 100th if still
     * starved. Surface-leak symptom is "no free surfaces" right after
     * BeginSequence even though refcounts should be fresh — log shows whether
     * num_surfaces was set, refcounts, and surfaces[] pointers. */
    static int alloc_dbg = 0;
    alloc_dbg++;
    if (alloc_dbg <= 12 || (alloc_dbg % 100) == 0) {
        char rcbuf[64]; int o = 0;
        for (int i = 0; i < g_dec.num_surfaces && o < 60; i++)
            o += snprintf(rcbuf + o, sizeof(rcbuf) - o, "%d%s",
                          g_dec.surf_refcount[i],
                          i == g_dec.num_surfaces - 1 ? "" : ",");
        rcbuf[o] = '\0';
        LOG("alloc #%d: num_surfaces=%d rc=[%s] s[0]=%p s[%d]=%p\n",
            alloc_dbg, g_dec.num_surfaces, rcbuf,
            g_dec.surfaces[0],
            g_dec.num_surfaces > 0 ? g_dec.num_surfaces - 1 : 0,
            g_dec.num_surfaces > 0 ? g_dec.surfaces[g_dec.num_surfaces - 1] : NULL);
    }
    int i;
    for (i = 0; i < g_dec.num_surfaces; i++) {
        if (g_dec.surf_refcount[i] == 0 && g_dec.surfaces[i]) {
            /* Build a mini picture buffer struct:
             * [0]=index, [1]=0, [2]=surface_ptr, [3]=0, [4]=refcount, [5]=0 */
            static int pic_bufs[MAX_SURFACES][6];
            pic_bufs[i][0] = i;
            pic_bufs[i][1] = 0;
            pic_bufs[i][2] = (int)g_dec.surfaces[i];
            pic_bufs[i][3] = 0;
            pic_bufs[i][4] = 1; /* refcount = 1 */
            pic_bufs[i][5] = g_dec.frames_decoded;
            g_dec.surf_refcount[i] = 1;
            *pic_buf = &pic_bufs[i][0];
            return 1;
        }
    }
    /* Failure path: dump full state so we can see WHY no surface was free. */
    {
        char rcbuf[80]; int o = 0;
        for (int i = 0; i < MAX_SURFACES && o < 70; i++)
            o += snprintf(rcbuf + o, sizeof(rcbuf) - o, "%d%s",
                          g_dec.surf_refcount[i], i == MAX_SURFACES - 1 ? "" : ",");
        rcbuf[o] = '\0';
        LOG("AllocPictureBuffer: no free surfaces! (call #%d) num=%d rc=[%s] s[0]=%p s[7]=%p\n",
            alloc_dbg, g_dec.num_surfaces, rcbuf,
            g_dec.surfaces[0], g_dec.surfaces[7]);
    }
    return 0;
}

static int cb_release(void* user, void* pic) {
    /* Diagnostic: log first 16 + every 200th. Shows refcount transitions so
     * we can spot leak / imbalance with cb_alloc and cb_addref. */
    static int rel_dbg = 0;
    rel_dbg++;
    if (!pic) {
        if (rel_dbg <= 16) LOG("release #%d: pic=NULL\n", rel_dbg);
        return 1;
    }
    int* p = (int*)pic;
    int idx = p[0];
    if (idx >= 0 && idx < MAX_SURFACES) {
        int rc_was = g_dec.surf_refcount[idx];
        if (g_dec.surf_refcount[idx] > 0) g_dec.surf_refcount[idx]--;
        if (p[4] > 0) p[4]--;
        if (rel_dbg <= 16 || (rel_dbg % 200) == 0)
            LOG("release #%d: idx=%d rc %d->%d (p4=%d)\n",
                rel_dbg, idx, rc_was, g_dec.surf_refcount[idx], p[4]);
    } else {
        if (rel_dbg <= 16) LOG("release #%d: BAD idx=%d\n", rel_dbg, idx);
    }
    return 1;
    /* Decrement, don't zero. The parser may call release multiple times
     * for the same picture (once per outstanding reference). The surface
     * is only truly free when refcount hits 0. Zeroing unconditionally
     * destroyed reference frames the decoder still needed for P-slice
     * decode → corrupted output (proven 2026-04-26 in aa_navimg_hook). */
}

static int cb_addref(void* user, void* pic) {
    static int addref_dbg = 0;
    addref_dbg++;
    if (!pic) {
        if (addref_dbg <= 16) LOG("addref #%d: pic=NULL\n", addref_dbg);
        return 1;
    }
    int* p = (int*)pic;
    p[4]++;
    int idx = p[0];
    int rc_was = (idx >= 0 && idx < MAX_SURFACES) ? g_dec.surf_refcount[idx] : -1;
    if (idx >= 0 && idx < MAX_SURFACES)
        g_dec.surf_refcount[idx]++;
    if (addref_dbg <= 16 || (addref_dbg % 200) == 0)
        LOG("addref #%d: idx=%d rc %d->%d (p4=%d)\n",
            addref_dbg, idx, rc_was,
            (idx >= 0 && idx < MAX_SURFACES) ? g_dec.surf_refcount[idx] : -1,
            p[4]);
    return 1;
}

static int cb_stub(void* user, void* p) { return 0; }

/* ------------------------------------------------------------------ */
/*  SHM reader                                                        */
/* ------------------------------------------------------------------ */
static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* ------------------------------------------------------------------ */
/*  Entry point — h264 capture mode                                   */
/*                                                                    */
/*  When linked into cluster_mirror, called as run_h264_mode(argc,    */
/*  argv) from cluster_mirror.c's main() after capture=h264 dispatch. */
/*  When built standalone (-DSTANDALONE), the wrapper at the bottom   */
/*  defines main and provides a g_verbose definition.                 */
/* ------------------------------------------------------------------ */

/* get_arg_f / get_arg_s already defined earlier in this file (used by both
 * display and h264 modes — same arg names + defaults across modes). */

int run_h264_mode(int argc, char** argv) {
    int dump_mode = (argc > 1 && strcmp(argv[1], "dump") == 0);

    /* Daemon two-stage support: bare `prepare` arg starts the process in
     * STAGE_PREPARE — decoder runs but post_win is invisible (does not claim
     * displayable 33). Daemon sends SIGUSR1 to flip to STAGE_RENDERING.
     * Without the flag the process starts in STAGE_RENDERING (backward-
     * compatible with manual `cluster capture=h264 ...` invocation). */
    {
        int prepare_mode = 0;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "prepare") == 0) {
                prepare_mode = 1;
                break;
            }
        }
        if (prepare_mode) {
            g_render_stage = STAGE_PREPARE;
        }
        signal(SIGUSR1, on_sigusr1);
        signal(SIGUSR2, on_sigusr2);
        LOG("h264 stage=%s (SIGUSR1=resume SIGUSR2=pause SIGTERM=exit)\n",
            prepare_mode ? "PREPARE" : "RENDERING");
    }

    /* Pull mode/zoom/pan/xres/yres from argv into g_h264_args so
     * cb_display_picture's blit-rect computation honors them. Same arg
     * names as cluster_mirror.c's display mode. */
    g_h264_args.mode  = get_arg_s(argc, argv, "mode", "letter");
    g_h264_args.zoomX = get_arg_f(argc, argv, "zoomX", 1.0f);
    g_h264_args.zoomY = get_arg_f(argc, argv, "zoomY", 1.0f);
    g_h264_args.panX  = get_arg_f(argc, argv, "panX",  0.0f);
    g_h264_args.panY  = get_arg_f(argc, argv, "panY",  0.0f);
    g_h264_args.xres  = (int)get_arg_f(argc, argv, "xres", 0.0f);
    g_h264_args.yres  = (int)get_arg_f(argc, argv, "yres", 0.0f);
    if (g_h264_args.zoomX < 0.1f) g_h264_args.zoomX = 0.1f;
    if (g_h264_args.zoomY < 0.1f) g_h264_args.zoomY = 0.1f;

    LOG("aa_cluster_decoder: %s mode (mode=%s zoomX=%.3f zoomY=%.3f panX=%.3f panY=%.3f xres=%d yres=%d)\n",
        dump_mode ? "dump" : "decode",
        g_h264_args.mode, g_h264_args.zoomX, g_h264_args.zoomY,
        g_h264_args.panX, g_h264_args.panY, g_h264_args.xres, g_h264_args.yres);

    /* Open SHM */
    LOG("waiting for SHM %s...\n", CLUSTER_SHM_NAME);
    int sfd = -1;
    while (sfd < 0) { sfd = shm_open(CLUSTER_SHM_NAME, O_RDWR, 0); usleep(500000); }
    cluster_h264_shm_t* shm = (cluster_h264_shm_t*)mmap(NULL, sizeof(*shm),
        PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);
    close(sfd);
    if (shm == MAP_FAILED) { perror("mmap"); return 1; }

    while (shm->magic != CLUSTER_SHM_MAGIC) usleep(100000);
    LOG("SHM active\n");

    /* Signal to gal_cluster.so that a new reader has attached. gal_cluster
     * polls this bit and, when set, sends 0x8002 Stop + 0x8001 Start to
     * cluster ch=14 to force the phone's encoder to emit a fresh SPS+PPS+IDR.
     * Without this, a mid-stream cluster restart sees only P-slices and the
     * H.264 parser never calls BeginSequence — setup_screen never runs and
     * disp 33 never gets claimed. gal_cluster clears the bit after firing. */
    shm->flags |= CLUSTER_SHM_FLAG_NEED_IDR;

    /* Dump mode */
    if (dump_mode) {
        int out_fd = open("/tmp/cluster_from_shm.h264", O_WRONLY|O_CREAT|O_TRUNC, 0644);
        uint32_t read_pos = shm->write_pos;
        uint32_t last_total = shm->total_bytes;
        int dump_bytes = 0;
        uint64_t t0 = now_us();
        LOG("dumping...\n");
        while (dump_bytes < 4*1024*1024) {
            uint32_t total = shm->total_bytes;
            if (total == last_total) { usleep(1000); if (dump_bytes>0 && now_us()-t0>10000000ULL) break; continue; }
            t0 = now_us();
            uint32_t wp = shm->write_pos;
            uint32_t avail = (wp >= read_pos) ? wp-read_pos : CLUSTER_RING_SIZE-read_pos+wp;
            if (avail > 0 && out_fd >= 0) {
                if (read_pos+avail <= CLUSTER_RING_SIZE) { write(out_fd, shm->ring+read_pos, avail); }
                else { uint32_t f=CLUSTER_RING_SIZE-read_pos; write(out_fd,shm->ring+read_pos,f); write(out_fd,shm->ring,avail-f); }
                read_pos = (read_pos+avail) % CLUSTER_RING_SIZE;
                dump_bytes += avail;
            }
            last_total = total;
            if ((dump_bytes%(100*1024))<1000) LOG("  %d KB\n", dump_bytes/1024);
        }
        close(out_fd);
        LOG("done: %d bytes\n", dump_bytes);
        return 0;
    }

    /* ---- Decode mode ---- */
    if (load_nvmedia() < 0) return 1;

    /* Create NvMedia device */
    g_dec.device = nv.DeviceCreate();
    if (!g_dec.device) { LOG("NvMediaDeviceCreate failed\n"); return 1; }
    LOG("device=%p\n", g_dec.device);
#ifdef USE_NVMEDIA2D
    if (nv.TwoDCreate) {
        g_dec.nvm2d = nv.TwoDCreate(g_dec.device);
        LOG("NvMedia2D instance=%p\n", g_dec.nvm2d);
    } else {
        LOG("ERROR: NvMedia2DCreate symbol not loaded — variant 2 won't work\n");
    }
#endif

    /* Create parser */
    /* Parser reads callbacks from user_data + 0x28 (like gal's renderer).
     * Create a fake renderer object with callbacks at the right offset. */
    static uint8_t fake_renderer[512] = {0};
    void** cbs = (void**)(fake_renderer + 0x28);
    cbs[0] = (void*)cb_begin_sequence;
    cbs[1] = (void*)cb_decode_picture;
    cbs[2] = (void*)cb_display_picture;
    cbs[3] = (void*)cb_stub;
    cbs[4] = (void*)cb_alloc_picture;
    cbs[5] = (void*)cb_release;
    cbs[6] = (void*)cb_addref;
    { int i; for (i=7;i<15;i++) cbs[i]=(void*)cb_stub; }

    /* Store device handle at offset 0xD0 (like gal's renderer) */
    *(void**)(fake_renderer + 0xD0) = g_dec.device;

    uint32_t params[6] = {0};
    params[0] = (uint32_t)(fake_renderer + 0x28);  /* callbacks AT user_data+0x28 */
    params[1] = (uint32_t)fake_renderer;             /* user_data */
    params[4] = 4; /* codec = H.264 */

    void* parser = nv.ParserCreate(params);
    if (!parser) { LOG("video_parser_create FAILED\n"); return 1; }
    LOG("parser=%p\n", parser);

    float fps = 30.0f;
    nv.ParserSetAttr(parser, 9, 4, &fps);

    /* Load canim splash into a CPU buffer once. h264 child paints it into
     * post_buf on STAGE_PAUSED so disp 33 stays owned by post_win throughout
     * the session — no separate canim_splash process competing for the
     * displaymanager binding. RGBA byte order (no R/B swap) because post_buf
     * goes directly to screen_post_window without a shader. */
    g_dec.canim_buf = (uint8_t*)malloc((size_t)DISP_W * DISP_H * 4);
    if (g_dec.canim_buf) {
        if (canim_load_into_buf(CANIM_DEFAULT_PATH, g_dec.canim_buf,
                                DISP_W * 4, DISP_W, DISP_H, 0) != 0) {
            LOG("h264: canim load failed — STAGE_PAUSED will show last frame instead\n");
            free(g_dec.canim_buf);
            g_dec.canim_buf = NULL;
        } else {
            LOG("h264: canim splash loaded for STAGE_PAUSED overlay\n");
        }
    }

    /* Main decode loop: read from SHM, feed to parser. Start from CURRENT
     * write position so we get fresh data with valid NAL boundaries. */
    uint32_t read_pos = shm->write_pos;
    uint32_t last_total = shm->total_bytes;
    int feed_count = 0;
    uint64_t t0 = now_us();
    (void)t0;

    LOG("decode loop started (wp=%u total=%u, waiting for NEW H.264 data)...\n",
        read_pos, last_total);

    while (1) {
        /* Apply pause/resume signals here too — when AA is paused, the phone
         * stops sending H.264 frames, so the decoder's cb_display_picture
         * stops firing and pause handling stuck inside that callback would
         * never run. Polling here ensures canim still gets painted on pause
         * even with no new frames in flight. */
        apply_stage_signals();
        uint32_t total = shm->total_bytes;
        if (total == last_total) {
            usleep(1000);
            continue;
        }

        uint32_t wp = shm->write_pos;
        uint32_t avail;
        if (wp >= read_pos)
            avail = wp - read_pos;
        else
            avail = CLUSTER_RING_SIZE - read_pos + wp;

        if (avail == 0) { last_total = total; continue; }

        /* Read into contiguous buffer */
        uint8_t* chunk = (uint8_t*)malloc(avail);
        if (!chunk) { last_total = total; continue; }

        if (read_pos + avail <= CLUSTER_RING_SIZE) {
            memcpy(chunk, shm->ring + read_pos, avail);
        } else {
            uint32_t first = CLUSTER_RING_SIZE - read_pos;
            memcpy(chunk, shm->ring + read_pos, first);
            memcpy(chunk + first, shm->ring, avail - first);
        }
        read_pos = (read_pos + avail) % CLUSTER_RING_SIZE;
        last_total = total;

        /* Feed directly to parser — it handles NAL discovery internally.
         * Log first chunks and any NAL start codes we see for diagnostics. */
        if (feed_count < 10 || (feed_count % 500) == 0) {
            LOG("feed %d: %u bytes, first16=[%02x %02x %02x %02x %02x %02x %02x %02x"
                " %02x %02x %02x %02x %02x %02x %02x %02x]\n",
                feed_count + 1, avail,
                avail>0?chunk[0]:0, avail>1?chunk[1]:0,
                avail>2?chunk[2]:0, avail>3?chunk[3]:0,
                avail>4?chunk[4]:0, avail>5?chunk[5]:0,
                avail>6?chunk[6]:0, avail>7?chunk[7]:0,
                avail>8?chunk[8]:0, avail>9?chunk[9]:0,
                avail>10?chunk[10]:0, avail>11?chunk[11]:0,
                avail>12?chunk[12]:0, avail>13?chunk[13]:0,
                avail>14?chunk[14]:0, avail>15?chunk[15]:0);
            /* Scan for NAL start codes (both 3-byte and 4-byte) */
            uint32_t ni, nal_count = 0;
            for (ni = 0; ni + 3 < avail && nal_count < 20; ni++) {
                if (chunk[ni]==0 && chunk[ni+1]==0) {
                    int is4 = (ni + 4 < avail && chunk[ni+2]==0 && chunk[ni+3]==1);
                    int is3 = (!is4 && chunk[ni+2]==1);
                    if (is4 || is3) {
                        int off = is4 ? 4 : 3;
                        uint8_t nal_byte = chunk[ni+off];
                        uint8_t nal_type = nal_byte & 0x1f;
                        const char* names[] = {"?","SLICE","A","B","C","IDR","SEI","SPS","PPS","AUD"};
                        const char* name = nal_type < 10 ? names[nal_type] : "?";
                        LOG("  NAL at +%u (%d-byte sc): type=%u (%s) byte=0x%02x\n",
                            ni, is4?4:3, nal_type, name, nal_byte);
                        nal_count++;
                        ni += off; /* skip past start code */
                    }
                }
            }
            if (nal_count == 0) LOG("  NO NAL start codes found in %u bytes!\n", avail);
        }

        uint32_t parse_data[9] = {0};
        parse_data[0] = (uint32_t)chunk;
        parse_data[1] = avail;

        int pr = nv.ParserParse(parser, parse_data);
        feed_count++;

        /* Periodic rate snapshot — log every 250 frames decoded (so it fires
         * regardless of whether feeds are big-and-rare or small-and-frequent).
         * Tells us if we're skipping frames vs being rate-limited by the
         * phone via TCP backpressure. */
        static int next_log_frames = 250;
        if (feed_count <= 10 || g_dec.frames_decoded >= next_log_frames) {
            static uint64_t last_t = 0;
            static uint32_t last_bytes = 0;
            static int last_dec = 0, last_disp = 0;
            uint64_t now = now_us();
            if (last_t == 0) {
                last_t = now; last_bytes = total;
                last_dec = g_dec.frames_decoded; last_disp = g_dec.frames_displayed;
                LOG("parse rc=%d, decoded=%d displayed=%d (feed %d) ring_avail=%u\n",
                    pr, g_dec.frames_decoded, g_dec.frames_displayed, feed_count, avail);
            } else {
                double secs = (double)(now - last_t) / 1e6;
                if (secs > 0.0) {
                    uint32_t dbytes = total - last_bytes;
                    int ddec  = g_dec.frames_decoded  - last_dec;
                    int ddisp = g_dec.frames_displayed - last_disp;
                    double prod_kbps = (double)dbytes * 8.0 / secs / 1000.0;
                    double dec_fps  = (double)ddec  / secs;
                    double disp_fps = (double)ddisp / secs;
                    LOG("parse rc=%d, decoded=%d displayed=%d (feed %d) ring_avail=%u "
                        "producer=%.1fkbps decode_fps=%.1f display_fps=%.1f%s\n",
                        pr, g_dec.frames_decoded, g_dec.frames_displayed, feed_count, avail,
                        prod_kbps, dec_fps, disp_fps,
                        avail > (CLUSTER_RING_SIZE * 3 / 4) ? " RING_NEAR_FULL!" : "");
                    last_t = now; last_bytes = total;
                    last_dec = g_dec.frames_decoded; last_disp = g_dec.frames_displayed;
                }
            }
            if (g_dec.frames_decoded >= next_log_frames) next_log_frames += 250;
        }
        free(chunk);
    }

    return 0;
}

/* ====================================================================
 *  TEST MODE — render a calibration pattern to displayable 33
 *
 *  Concentric inset rectangles (each in a distinct color) plus tick marks
 *  along the top and left edges. User looks at the cluster and identifies
 *  the outermost color that's fully visible (not clipped by the bezel) →
 *  that color's inset = a safe stable_content_insets value.
 *
 *  Usage:
 *    cluster capture=test                → 1280×860, default
 *    cluster capture=test xres=540 yres=480
 *    cluster capture=test step=10        → finer ticks (default 25 px)
 *
 *  Doesn't need NvMedia, doesn't need external input. Just allocates an
 *  RGBX8888 window on display 33, fills it, posts once, sleeps.
 * ==================================================================== */

/* Pixel write in BGR byte order so the R/B-swap fragment shader (fs_src,
 * which is written for screen_read_display's BGRA output) produces the
 * intended (r,g,b) on screen. Callers pass logical RGB. */
static inline void tm_set_px(uint8_t* p, int stride, int x, int y, int w, int h,
                             uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    uint8_t* px = p + y * stride + x * 4;
    px[0] = b; px[1] = g; px[2] = r; px[3] = 0xFF;
}

static void tm_rect_outline(uint8_t* p, int stride, int w, int h,
                            int x0, int y0, int rw, int rh,
                            uint8_t r, uint8_t g, uint8_t b) {
    for (int x = x0; x < x0 + rw; x++) {
        tm_set_px(p, stride, x, y0,           w, h, r, g, b);
        tm_set_px(p, stride, x, y0 + rh - 1,  w, h, r, g, b);
    }
    for (int y = y0; y < y0 + rh; y++) {
        tm_set_px(p, stride, x0,          y, w, h, r, g, b);
        tm_set_px(p, stride, x0 + rw - 1, y, w, h, r, g, b);
    }
}

static void tm_fill_rect(uint8_t* p, int stride, int w, int h,
                         int x0, int y0, int rw, int rh,
                         uint8_t r, uint8_t g, uint8_t b) {
    for (int y = y0; y < y0 + rh; y++)
        for (int x = x0; x < x0 + rw; x++)
            tm_set_px(p, stride, x, y, w, h, r, g, b);
}

/* 5x7 ASCII bitmap font for the idle splash / boot splash modes.
 * Column-major: 5 bytes per glyph, bit 0 = top row, bit 6 = bottom row of
 * 7-row cell. Char width is 5, so use 6-px advance for 1-px gap between chars.
 *
 * Only glyphs used in the current splash strings are defined; unmapped chars
 * render as the placeholder rectangle in tm_draw_char. Easy to extend. */
typedef struct { uint8_t ch; uint8_t col[5]; } font5x7_glyph_t;
static const font5x7_glyph_t FONT5X7[] = {
    { ' ',  {0x00,0x00,0x00,0x00,0x00} },
    { '(',  {0x00,0x00,0x1C,0x22,0x41} },
    { ')',  {0x41,0x22,0x1C,0x00,0x00} },
    { '.',  {0x00,0x60,0x60,0x00,0x00} },
    { '/',  {0x40,0x30,0x08,0x06,0x01} },
    { '0',  {0x3E,0x41,0x41,0x41,0x3E} },
    { '2',  {0x42,0x61,0x51,0x49,0x46} },
    { '6',  {0x3C,0x4A,0x49,0x49,0x31} },
    { 'B',  {0x7F,0x49,0x49,0x49,0x36} },
    { 'b',  {0x7F,0x48,0x48,0x48,0x78} },
    { 'c',  {0x38,0x44,0x44,0x44,0x20} },
    { 'f',  {0x08,0x7E,0x09,0x01,0x01} },
    { 'g',  {0x0C,0x52,0x52,0x52,0x3C} },
    { 'h',  {0x7F,0x08,0x04,0x04,0x78} },
    { 'i',  {0x00,0x44,0x7D,0x40,0x00} },
    { 'o',  {0x38,0x44,0x44,0x44,0x38} },
    { 'r',  {0x7C,0x08,0x04,0x04,0x08} },
    { 't',  {0x04,0x3F,0x44,0x40,0x20} },
};

static const uint8_t* font5x7_lookup(char c) {
    for (size_t i = 0; i < sizeof(FONT5X7)/sizeof(FONT5X7[0]); i++)
        if (FONT5X7[i].ch == (uint8_t)c) return FONT5X7[i].col;
    return NULL;  /* caller draws a placeholder rect */
}

/* Draw one glyph at logical (x,y). Each font pixel becomes a scale*scale
 * block. 1-px right-side gap is the caller's responsibility (x-advance =
 * 6*scale). Bit 0 of column byte = top row, bit 6 = bottom row (rows 0..6). */
static void tm_draw_char(uint8_t* p, int stride, int w, int h,
                         int x, int y, char c, int scale,
                         uint8_t r, uint8_t g, uint8_t b) {
    const uint8_t* glyph = font5x7_lookup(c);
    if (!glyph) {
        tm_rect_outline(p, stride, w, h, x, y, 5*scale, 7*scale, r, g, b);
        return;
    }
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1u << row)) {
                tm_fill_rect(p, stride, w, h,
                             x + col*scale, y + row*scale,
                             scale, scale, r, g, b);
            }
        }
    }
}

static int tm_text_width(const char* str, int scale) {
    int n = (int)strlen(str);
    if (n <= 0) return 0;
    return n*5*scale + (n-1)*scale;  /* 5 px wide + 1 px gap per char */
}

static void tm_draw_text(uint8_t* p, int stride, int w, int h,
                         int x, int y, const char* str, int scale,
                         uint8_t r, uint8_t g, uint8_t b) {
    int cx = x;
    for (const char* s = str; *s; s++) {
        tm_draw_char(p, stride, w, h, cx, y, *s, scale, r, g, b);
        cx += 6*scale;  /* 5 px glyph + 1 px gap */
    }
}

int run_test_mode(int argc, char **argv) {
    int rc;
    int xres = (int)get_arg_f(argc, argv, "xres", (float)DISP_W);
    int yres = (int)get_arg_f(argc, argv, "yres", (float)DISP_H);
    int step = (int)get_arg_f(argc, argv, "step", 25.0f);
    /* zoom/pan applied as UV manipulation on the GL quad — same semantics
     * as capture=display mirror. Use this to dial in per-car values
     * visually (set xres/yres to the h264 source size, e.g. 1280×720, then
     * tune zoom/pan until the cluster framing looks right). */
    float zoomX = get_arg_f(argc, argv, "zoomX", 1.0f);
    float zoomY = get_arg_f(argc, argv, "zoomY", 1.0f);
    float panX  = get_arg_f(argc, argv, "panX",  0.0f);
    float panY  = get_arg_f(argc, argv, "panY",  0.0f);
    if (zoomX < 0.1f) zoomX = 0.1f;
    if (zoomY < 0.1f) zoomY = 0.1f;
    if (xres < 64) xres = DISP_W;
    if (yres < 64) yres = DISP_H;
    if (step < 5)  step = 5;

    LOG("test pattern %dx%d step=%dpx zoomX=%.3f zoomY=%.3f panX=%.3f panY=%.3f "
        "(display %dx%d)\n",
        xres, yres, step, zoomX, zoomY, panX, panY, DISP_W, DISP_H);

    /* ------------------------------------------------------------------ */
    /* APPLICATION_CONTEXT — pixmap (CPU-writable) + window (GL on disp 33)*/
    /* ------------------------------------------------------------------ */
    screen_context_t ctx_app = NULL;
    rc = screen_create_context(&ctx_app, SCREEN_APPLICATION_CONTEXT);
    if (rc) { LOG("[!] create_context rc=%d errno=%d\n", rc, errno); return 1; }

    /* Pixmap: CPU-writable RGBX8888, will be uploaded to a GL texture */
    int fmt   = SCREEN_FORMAT_RGBX8888;
    int usage = SCREEN_USAGE_READ | SCREEN_USAGE_WRITE;
    int bsz[2] = { xres, yres };

    screen_pixmap_t cap_pixmap = NULL;
    rc = screen_create_pixmap(&cap_pixmap, ctx_app);
    if (rc) { LOG("[!] create_pixmap rc=%d\n", rc); return 1; }
    screen_set_pixmap_property_iv(cap_pixmap, SCREEN_PROPERTY_FORMAT,      &fmt);
    screen_set_pixmap_property_iv(cap_pixmap, SCREEN_PROPERTY_USAGE,       &usage);
    screen_set_pixmap_property_iv(cap_pixmap, SCREEN_PROPERTY_BUFFER_SIZE, bsz);
    rc = screen_create_pixmap_buffer(cap_pixmap);
    if (rc) { LOG("[!] create_pixmap_buffer rc=%d errno=%d\n", rc, errno); return 1; }

    screen_buffer_t cap_buf = NULL;
    void *cap_ptr = NULL;
    int   cap_stride = 0;
    screen_get_pixmap_property_pv(cap_pixmap, SCREEN_PROPERTY_RENDER_BUFFERS, (void**)&cap_buf);
    screen_get_buffer_property_pv(cap_buf, SCREEN_PROPERTY_POINTER, &cap_ptr);
    screen_get_buffer_property_iv(cap_buf, SCREEN_PROPERTY_STRIDE,  &cap_stride);
    if (!cap_ptr) { LOG("[!] pixmap pointer NULL\n"); return 1; }
    LOG("pixmap: ptr=%p stride=%d (tight=%d)\n", cap_ptr, cap_stride, cap_stride == xres * 4);

    /* Cluster window on display 33 — must use OPENGL_ES2 to bind to disp 33 */
    screen_window_t cl_win = NULL;
    rc = screen_create_window(&cl_win, ctx_app);
    if (rc) { LOG("[!] create_window rc=%d\n", rc); return 1; }
    screen_set_window_property_cv(cl_win, SCREEN_PROPERTY_ID_STRING, 2, "33");
    int cl_size[2] = { DISP_W, DISP_H };
    int cl_fmt   = SCREEN_FORMAT_RGBX8888;
    int cl_usage = SCREEN_USAGE_OPENGL_ES2;
    int cl_vis   = 0;
    screen_set_window_property_iv(cl_win, SCREEN_PROPERTY_SIZE,        cl_size);
    screen_set_window_property_iv(cl_win, SCREEN_PROPERTY_BUFFER_SIZE, cl_size);
    screen_set_window_property_iv(cl_win, SCREEN_PROPERTY_FORMAT,      &cl_fmt);
    screen_set_window_property_iv(cl_win, SCREEN_PROPERTY_USAGE,       &cl_usage);
    screen_set_window_property_iv(cl_win, SCREEN_PROPERTY_VISIBLE,     &cl_vis);
    screen_flush_context(ctx_app, 0);
    rc = screen_create_window_buffers(cl_win, 2);
    if (rc) { LOG("[!] create_window_buffers rc=%d errno=%d\n", rc, errno); return 1; }

    /* CPU-fill the pixmap with the test pattern (via tm_set_px which writes
     * BGR to compensate for the R/B swap fragment shader). */
    uint8_t* p   = (uint8_t*)cap_ptr;
    int      stride = cap_stride;

    /* Background: dark gray */
    tm_fill_rect(p, stride, xres, yres, 0, 0, xres, yres, 0x20, 0x20, 0x20);

    /* Outermost edge: white outline at (0,0,W,H). Almost always partially
     * clipped by bezel — this is your reference for "screen edge". */
    tm_rect_outline(p, stride, xres, yres, 0, 0, xres, yres, 0xFF, 0xFF, 0xFF);

    /* Concentric inset rectangles, each in a distinct color.
     * Outer rings packed at 25 px steps for bezel calibration; inner rings
     * extended at 50 px steps so the whole panel is covered visually.
     * Each ring drawn 2 px thick to survive thumbnail down-scaling. */
    struct { int inset; uint8_t r, g, b; const char* name; } rings[] = {
        {  25, 0xFF, 0x00, 0x00, "RED" },
        {  50, 0xFF, 0x80, 0x00, "ORANGE" },
        {  75, 0xFF, 0xFF, 0x00, "YELLOW" },
        { 100, 0x00, 0xFF, 0x00, "GREEN" },
        { 125, 0x00, 0xFF, 0xFF, "CYAN" },
        { 150, 0x00, 0x80, 0xFF, "BLUE" },
        { 200, 0xFF, 0x00, 0xFF, "MAGENTA" },
        { 250, 0xFF, 0x40, 0x80, "PINK" },
        { 300, 0x80, 0xFF, 0x80, "LIME" },
        { 350, 0x40, 0x80, 0xFF, "AZURE" },
        { 400, 0xFF, 0xC0, 0x40, "AMBER" },
    };
    int num_rings = (int)(sizeof(rings) / sizeof(rings[0]));
    LOG("Rings (color → inset px from edge):\n");
    for (int i = 0; i < num_rings; i++) {
        int m = rings[i].inset;
        if (m * 2 + 32 > xres || m * 2 + 32 > yres) {
            LOG("  %-7s = %d px (skipped, doesn't fit)\n", rings[i].name, m);
            continue;
        }
        /* 2 px thick outline (m and m+1) so it survives image down-scaling */
        tm_rect_outline(p, stride, xres, yres,
                        m, m, xres - 2*m, yres - 2*m,
                        rings[i].r, rings[i].g, rings[i].b);
        tm_rect_outline(p, stride, xres, yres,
                        m + 1, m + 1, xres - 2*(m + 1), yres - 2*(m + 1),
                        rings[i].r, rings[i].g, rings[i].b);
        LOG("  %-7s = %d px\n", rings[i].name, m);
    }

    /* Center crosshair (60 px wide, white) */
    int cx = xres / 2, cy = yres / 2;
    for (int x = cx - 30; x <= cx + 30; x++)
        tm_set_px(p, stride, x, cy, xres, yres, 0xFF, 0xFF, 0xFF);
    for (int y = cy - 30; y <= cy + 30; y++)
        tm_set_px(p, stride, cx, y, xres, yres, 0xFF, 0xFF, 0xFF);

    /* Tick marks along top edge (3 px wide, 8 px tall, alternating colors).
     * Major ticks (every 100 px) longer + magenta; minor ticks every `step`
     * yellow. */
    for (int x = 0; x < xres; x += step) {
        int major = (x % 100 == 0);
        int height = major ? 16 : 8;
        uint8_t r = major ? 0xFF : 0xFF;
        uint8_t g = major ? 0x00 : 0xFF;
        uint8_t b = major ? 0xFF : 0x00;
        tm_fill_rect(p, stride, xres, yres, x, 0, 3, height, r, g, b);
    }

    /* Tick marks along left edge */
    for (int y = 0; y < yres; y += step) {
        int major = (y % 100 == 0);
        int width = major ? 16 : 8;
        uint8_t r = major ? 0xFF : 0xFF;
        uint8_t g = major ? 0x00 : 0xFF;
        uint8_t b = major ? 0xFF : 0x00;
        tm_fill_rect(p, stride, xres, yres, 0, y, width, 3, r, g, b);
    }

    /* Tick marks along bottom edge */
    for (int x = 0; x < xres; x += step) {
        int major = (x % 100 == 0);
        int height = major ? 16 : 8;
        uint8_t r = major ? 0xFF : 0xFF;
        uint8_t g = major ? 0x00 : 0xFF;
        uint8_t b = major ? 0xFF : 0x00;
        tm_fill_rect(p, stride, xres, yres, x, yres - height, 3, height, r, g, b);
    }

    /* Tick marks along right edge */
    for (int y = 0; y < yres; y += step) {
        int major = (y % 100 == 0);
        int width = major ? 16 : 8;
        uint8_t r = major ? 0xFF : 0xFF;
        uint8_t g = major ? 0x00 : 0xFF;
        uint8_t b = major ? 0xFF : 0x00;
        tm_fill_rect(p, stride, xres, yres, xres - width, y, width, 3, r, g, b);
    }

    /* ------------------------------------------------------------------ */
    /* EGL on cluster window + GL upload of the CPU-filled pixmap         */
    /* ------------------------------------------------------------------ */
    EGLDisplay egl_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(egl_dpy, NULL, NULL);
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };
    EGLConfig egl_cfg; EGLint ncfg;
    eglChooseConfig(egl_dpy, cfg_attr, &egl_cfg, 1, &ncfg);

    EGLSurface egl_surf = eglCreateWindowSurface(egl_dpy, egl_cfg,
                                                 (EGLNativeWindowType)cl_win, NULL);
    if (egl_surf == EGL_NO_SURFACE) {
        LOG("[!] eglCreateWindowSurface failed 0x%x\n", eglGetError());
        return 1;
    }
    EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext egl_ctx = eglCreateContext(egl_dpy, egl_cfg, EGL_NO_CONTEXT, ctx_attr);
    if (egl_ctx == EGL_NO_CONTEXT) { LOG("[!] eglCreateContext failed\n"); return 1; }
    eglMakeCurrent(egl_dpy, egl_surf, egl_surf, egl_ctx);

    while (eglGetError() != EGL_SUCCESS) {}
    load_gl();

    /* If pixmap stride isn't tight, copy to a tight buffer for upload */
    uint8_t *upload_buf = NULL;
    const void *px = p;
    if (stride != xres * 4) {
        upload_buf = malloc((size_t)xres * (size_t)yres * 4);
        if (!upload_buf) { LOG("[!] malloc failed\n"); return 1; }
        for (int r = 0; r < yres; r++)
            memcpy(upload_buf + (size_t)r * xres * 4,
                   p + (size_t)r * stride,
                   (size_t)xres * 4);
        px = upload_buf;
    }

    GLuint tex = 0;
    gl_GenTextures(1, &tex);
    gl_ActiveTexture(GL_TEXTURE0);
    gl_BindTexture(GL_TEXTURE_2D, tex);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    gl_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, xres, yres, 0,
                  GL_RGBA, GL_UNSIGNED_BYTE, px);

    /* Shaders: same vs_src + fs_src as mirror (fs_src does R/B swap, which
     * matches the BGR byte order written by tm_set_px). */
    GLuint vs = gl_CreateShader(GL_VERTEX_SHADER);
    gl_ShaderSource(vs, 1, &vs_src, NULL); gl_CompileShader(vs);
    GLuint fs = gl_CreateShader(GL_FRAGMENT_SHADER);
    gl_ShaderSource(fs, 1, &fs_src, NULL); gl_CompileShader(fs);
    GLuint prog = gl_CreateProgram();
    gl_AttachShader(prog, vs); gl_AttachShader(prog, fs);
    gl_LinkProgram(prog); gl_UseProgram(prog);
    gl_Uniform1i(gl_GetUniformLocation(prog, "tex"), 0);

    /* UV range starts at full (0,0)-(1,1) and gets shrunk/shifted by
     * zoom/pan — same math as the mirror (capture=display) path so test-mode
     * tuning numbers transfer directly to h264 (when xres/yres match the
     * h264 source dims). */
    float u0 = 0.0f, u1 = 1.0f, v0 = 0.0f, v1 = 1.0f;
    /* Apply zoomX: shrink U range around centre (>1 crops edges, magnifies). */
    {
        float centre = (u0 + u1) * 0.5f;
        float half   = (u1 - u0) * 0.5f / zoomX;
        u0 = centre - half;
        u1 = centre + half;
    }
    /* Apply zoomY: shrink V range around centre. */
    {
        float centre = (v0 + v1) * 0.5f;
        float half   = (v1 - v0) * 0.5f / zoomY;
        v0 = centre - half;
        v1 = centre + half;
    }
    /* Apply panX/panY: shift UV ranges (positive = image moves left/up). */
    { float s = panX * (u1 - u0); u0 += s; u1 += s; }
    { float s = panY * (v1 - v0); v0 += s; v1 += s; }

    float verts[24];
    build_quad_xy(verts, 1.0f, 1.0f, u0, u1, v0, v1);
    LOG("test quad: u0=%.4f u1=%.4f v0=%.4f v1=%.4f (zoomX=%.3f zoomY=%.3f "
        "panX=%.3f panY=%.3f)\n", u0, u1, v0, v1, zoomX, zoomY, panX, panY);
    GLint pos_loc = gl_GetAttribLocation(prog, "pos");
    GLint uv_loc  = gl_GetAttribLocation(prog, "uv");
    gl_EnableVertexAttribArray(pos_loc);
    gl_EnableVertexAttribArray(uv_loc);
    gl_VertexAttribPointer(pos_loc, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), verts);
    gl_VertexAttribPointer(uv_loc,  2, GL_FLOAT, GL_FALSE, 4*sizeof(float), verts+2);

    cl_vis = 1;
    screen_set_window_property_iv(cl_win, SCREEN_PROPERTY_VISIBLE, &cl_vis);
    screen_flush_context(ctx_app, 0);

    gl_Viewport(0, 0, DISP_W, DISP_H);
    gl_ClearColor(0.f, 0.f, 0.f, 1.f);
    gl_Clear(GL_COLOR_BUFFER_BIT);
    gl_DrawArrays(GL_TRIANGLES, 0, 6);
    eglSwapBuffers(egl_dpy, egl_surf);

    /* Re-draw a few times to make sure the window comes up on display 33
     * (some compositor states require a couple of buffer flips before the
     * window shows). */
    for (int i = 0; i < 3; i++) {
        gl_Clear(GL_COLOR_BUFFER_BIT);
        gl_DrawArrays(GL_TRIANGLES, 0, 6);
        eglSwapBuffers(egl_dpy, egl_surf);
    }

    LOG("Test pattern posted to displayable 33. Sleeping. SIGTERM/Ctrl-C to exit.\n");

    while (1) sleep(60);
    return 0;
}

/* Idle splash / boot splash mode. Posts a centered two-line message on
 * displayable 33 over a near-black background. Used by the daemon to own
 * disp 33 when no real renderer (h264/mirror) is running, so the cluster
 * doesn't show a stale frozen frame. Also used as a one-shot boot splash.
 *
 * Args:
 *   duration_s=N   exit after N seconds (0 = idle indefinitely, wait SIGTERM)
 *   line1=...      first line text (default: "(c) 2026 fifthBro")
 *   line2=...      second line text (default: "fifthbro.github.io")
 *
 * Display: pixmap CPU-filled (same path as run_test_mode) → GL texture →
 * fragment shader R/B swap → screen window on disp 33. Fonts are the 5x7
 * bitmap font defined above. No external assets, no decoder.            */
int run_idle_logo(int argc, char **argv, int default_duration_s) {
    int rc;
    int xres = DISP_W, yres = DISP_H;
    int duration_s = (int)get_arg_f(argc, argv, "duration_s", (float)default_duration_s);
    int blank = (int)get_arg_f(argc, argv, "blank", 0.0f);
    const char* line1 = get_arg_s(argc, argv, "line1", "(c) 2026 fifthBro");
    const char* line2 = get_arg_s(argc, argv, "line2", "fifthbro.github.io");

    LOG("idle_logo: %dx%d duration_s=%d line1='%s' line2='%s'\n",
        xres, yres, duration_s, line1, line2);

    screen_context_t ctx_app = NULL;
    rc = screen_create_context(&ctx_app, SCREEN_APPLICATION_CONTEXT);
    if (rc) { LOG("[!] idle_logo create_context rc=%d errno=%d\n", rc, errno); return 1; }

    int fmt   = SCREEN_FORMAT_RGBX8888;
    int usage = SCREEN_USAGE_READ | SCREEN_USAGE_WRITE;
    int bsz[2] = { xres, yres };

    screen_pixmap_t cap_pixmap = NULL;
    rc = screen_create_pixmap(&cap_pixmap, ctx_app);
    if (rc) { LOG("[!] idle_logo create_pixmap rc=%d\n", rc); return 1; }
    screen_set_pixmap_property_iv(cap_pixmap, SCREEN_PROPERTY_FORMAT,      &fmt);
    screen_set_pixmap_property_iv(cap_pixmap, SCREEN_PROPERTY_USAGE,       &usage);
    screen_set_pixmap_property_iv(cap_pixmap, SCREEN_PROPERTY_BUFFER_SIZE, bsz);
    rc = screen_create_pixmap_buffer(cap_pixmap);
    if (rc) { LOG("[!] idle_logo create_pixmap_buffer rc=%d errno=%d\n", rc, errno); return 1; }

    screen_buffer_t cap_buf = NULL;
    void *cap_ptr = NULL;
    int   cap_stride = 0;
    screen_get_pixmap_property_pv(cap_pixmap, SCREEN_PROPERTY_RENDER_BUFFERS, (void**)&cap_buf);
    screen_get_buffer_property_pv(cap_buf, SCREEN_PROPERTY_POINTER, &cap_ptr);
    screen_get_buffer_property_iv(cap_buf, SCREEN_PROPERTY_STRIDE,  &cap_stride);
    if (!cap_ptr) { LOG("[!] idle_logo pixmap pointer NULL\n"); return 1; }

    screen_window_t cl_win = NULL;
    rc = screen_create_window(&cl_win, ctx_app);
    if (rc) { LOG("[!] idle_logo create_window rc=%d\n", rc); return 1; }
    screen_set_window_property_cv(cl_win, SCREEN_PROPERTY_ID_STRING, 2, "33");
    int cl_size[2] = { DISP_W, DISP_H };
    int cl_fmt   = SCREEN_FORMAT_RGBX8888;
    int cl_usage = SCREEN_USAGE_OPENGL_ES2;
    int cl_vis   = 0;
    screen_set_window_property_iv(cl_win, SCREEN_PROPERTY_SIZE,        cl_size);
    screen_set_window_property_iv(cl_win, SCREEN_PROPERTY_BUFFER_SIZE, cl_size);
    screen_set_window_property_iv(cl_win, SCREEN_PROPERTY_FORMAT,      &cl_fmt);
    screen_set_window_property_iv(cl_win, SCREEN_PROPERTY_USAGE,       &cl_usage);
    screen_set_window_property_iv(cl_win, SCREEN_PROPERTY_VISIBLE,     &cl_vis);
    screen_flush_context(ctx_app, 0);
    rc = screen_create_window_buffers(cl_win, 2);
    if (rc) { LOG("[!] idle_logo create_window_buffers rc=%d errno=%d\n", rc, errno); return 1; }

    /* Fill from the embedded boot canim (fifthBro logo). canim_load_from_mem
     * black-fills the pixmap first, then center-crops the embedded image
     * into it. If blank=1 or canim decode fails, fall back to a CPU-drawn
     * text credit so something still shows.
     *
     * swap_rb=1: GL fragment shader does R↔B swap, so the texture data
     * must be stored as BGR-ordered bytes for it to appear correct on screen
     * (same convention as the test pattern's tm_set_px). */
    uint8_t* p = (uint8_t*)cap_ptr;
    int stride = cap_stride;
    int canim_ok = 0;

    if (!blank) {
        int cr = canim_load_from_mem(boot_canim_data, boot_canim_data_len,
                                     p, stride, xres, yres, 1);
        if (cr == 0) {
            canim_ok = 1;
            LOG("idle_logo: embedded boot canim loaded OK\n");
        } else {
            LOG("idle_logo: embedded canim load rc=%d, falling back to text\n", cr);
        }
    }

    if (!canim_ok) {
        if (blank) {
            tm_fill_rect(p, stride, xres, yres, 0, 0, xres, yres, 0x00, 0x00, 0x00);
        } else {
            tm_fill_rect(p, stride, xres, yres, 0, 0, xres, yres, 0x0A, 0x0A, 0x0A);
            int accent_y = yres / 2;
            int accent_w = xres / 3;
            int accent_x = (xres - accent_w) / 2;
            tm_fill_rect(p, stride, xres, yres, accent_x, accent_y, accent_w, 1,
                         0x30, 0x30, 0x30);
            int s1 = 6;
            int line1_w = tm_text_width(line1, s1);
            int line1_x = (xres - line1_w) / 2;
            int line1_y = accent_y - 7*s1 - 18;
            tm_draw_text(p, stride, xres, yres, line1_x, line1_y, line1, s1,
                         0xE0, 0xE0, 0xE0);
            int s2 = 4;
            int line2_w = tm_text_width(line2, s2);
            int line2_x = (xres - line2_w) / 2;
            int line2_y = accent_y + 18;
            tm_draw_text(p, stride, xres, yres, line2_x, line2_y, line2, s2,
                         0x80, 0x80, 0x80);
        }
    }

    /* EGL + GL upload — same path as run_test_mode. */
    EGLDisplay egl_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(egl_dpy, NULL, NULL);
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };
    EGLConfig egl_cfg; EGLint ncfg;
    eglChooseConfig(egl_dpy, cfg_attr, &egl_cfg, 1, &ncfg);

    EGLSurface egl_surf = eglCreateWindowSurface(egl_dpy, egl_cfg,
                                                 (EGLNativeWindowType)cl_win, NULL);
    if (egl_surf == EGL_NO_SURFACE) {
        LOG("[!] idle_logo eglCreateWindowSurface failed 0x%x\n", eglGetError());
        return 1;
    }
    EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext egl_ctx = eglCreateContext(egl_dpy, egl_cfg, EGL_NO_CONTEXT, ctx_attr);
    if (egl_ctx == EGL_NO_CONTEXT) { LOG("[!] idle_logo eglCreateContext failed\n"); return 1; }
    eglMakeCurrent(egl_dpy, egl_surf, egl_surf, egl_ctx);

    while (eglGetError() != EGL_SUCCESS) {}
    load_gl();

    uint8_t *upload_buf = NULL;
    const void *px = p;
    if (stride != xres * 4) {
        upload_buf = malloc((size_t)xres * (size_t)yres * 4);
        if (!upload_buf) { LOG("[!] idle_logo malloc failed\n"); return 1; }
        for (int r = 0; r < yres; r++)
            memcpy(upload_buf + (size_t)r * xres * 4,
                   p + (size_t)r * stride,
                   (size_t)xres * 4);
        px = upload_buf;
    }

    GLuint tex = 0;
    gl_GenTextures(1, &tex);
    gl_ActiveTexture(GL_TEXTURE0);
    gl_BindTexture(GL_TEXTURE_2D, tex);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    gl_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, xres, yres, 0,
                  GL_RGBA, GL_UNSIGNED_BYTE, px);

    GLuint vs = gl_CreateShader(GL_VERTEX_SHADER);
    gl_ShaderSource(vs, 1, &vs_src, NULL); gl_CompileShader(vs);
    GLuint fs = gl_CreateShader(GL_FRAGMENT_SHADER);
    gl_ShaderSource(fs, 1, &fs_src, NULL); gl_CompileShader(fs);
    GLuint prog = gl_CreateProgram();
    gl_AttachShader(prog, vs); gl_AttachShader(prog, fs);
    gl_LinkProgram(prog); gl_UseProgram(prog);
    gl_Uniform1i(gl_GetUniformLocation(prog, "tex"), 0);

    float verts[24];
    build_quad_xy(verts, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
    GLint pos_loc = gl_GetAttribLocation(prog, "pos");
    GLint uv_loc  = gl_GetAttribLocation(prog, "uv");
    gl_EnableVertexAttribArray(pos_loc);
    gl_EnableVertexAttribArray(uv_loc);
    gl_VertexAttribPointer(pos_loc, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), verts);
    gl_VertexAttribPointer(uv_loc,  2, GL_FLOAT, GL_FALSE, 4*sizeof(float), verts+2);

    cl_vis = 1;
    screen_set_window_property_iv(cl_win, SCREEN_PROPERTY_VISIBLE, &cl_vis);
    screen_flush_context(ctx_app, 0);

    gl_Viewport(0, 0, DISP_W, DISP_H);
    gl_ClearColor(0.f, 0.f, 0.f, 1.f);
    for (int i = 0; i < 3; i++) {
        gl_Clear(GL_COLOR_BUFFER_BIT);
        gl_DrawArrays(GL_TRIANGLES, 0, 6);
        eglSwapBuffers(egl_dpy, egl_surf);
    }

    LOG("idle_logo: posted to displayable 33.\n");

    if (duration_s > 0) {
        sleep(duration_s);
        LOG("idle_logo: duration_s=%d elapsed, exiting.\n", duration_s);
    } else {
        while (1) sleep(60);  /* SIGTERM from daemon exits us */
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * canim splash — reads the on-target Porsche .canim file, decompresses
 * (zlib), parses the format, and posts the first contained image to
 * displayable 33. Source image is 1920x720 RGB; we center-crop horizontally
 * to 1280 columns and center-pad vertically with black to fit the 1280x860
 * cluster window.
 *
 * Format (from jilleb/mib2-toolbox extract-canim.py):
 *   - whole file = zlib-compressed stream
 *   - decompressed:
 *       0x00  8 bytes   magic "ANIM1   "
 *       0x08  uint32    stage_width
 *       0x0C  uint32    stage_height
 *       0x10  uint32    cmdblock_len
 *       0x14  uint32    brand
 *       0x18  start of 32-byte command blocks
 *               uint32  cmd_code   (0x11 = image)
 *               uint32  img_num
 *               uint32  img_width
 *               uint32  img_height
 *               uint32  bytes_per_pixel    (3 = RGB, 4 = RGBA)
 *               uint32  data_start         (offset within payload)
 *               (8 bytes trailing)
 *   - pixel payload begins at offset 0x20 + cmdblock_len + data_start.
 *
 * Some files start their first cmd block at offset 0x18 (24); others put
 * it at 24 + 0x78 = 144 — the script tries both. We do the same.
 *
 * Returns 0 on success, non-zero if the file is missing or parse fails
 * (caller can then fall back to the text splash). */

/* CANIM_DEFAULT_PATH defined near the forward declarations at the top of the file. */

/* Parse a canim file and blit its first image (center-cropped + vertically
 * padded) into a destination buffer. Returns 0 on success, non-zero on any
 * parse/IO failure. Destination is dst_w × dst_h pixels, dst_stride bytes
 * per row, 4 bytes per pixel.
 *
 *   swap_rb=1 → write BGRA (for the GL R/B-swap shader path used by
 *               run_canim_splash and run_idle_logo).
 *   swap_rb=0 → write RGBA (for direct screen_post_window paths, e.g.
 *               the h264 child painting canim itself during STAGE_PAUSED).
 *
 * The destination is filled with black outside the image's center-crop area. */
/* Shared canim decode: takes a zlib-compressed buffer (the raw file bytes
 * for file-loaded canims, or the embedded array for the boot logo). */
static int canim_load_from_mem(const uint8_t* zbuf, size_t zlen,
                               uint8_t* dst, int dst_stride,
                               int dst_w, int dst_h,
                               int swap_rb) {
    if (!zbuf || zlen == 0) return 2;
    size_t cap = 16*1024*1024;
    uint8_t* raw = (uint8_t*)malloc(cap);
    if (!raw) return 6;
    uLongf raw_len = (uLongf)cap;
    int zr = uncompress(raw, &raw_len, zbuf, (uLong)zlen);
    if (zr != Z_OK) { free(raw); return 7; }

    if (raw_len < 0x18 || memcmp(raw, "ANIM1", 5) != 0) { free(raw); return 8; }
    uint32_t cmdblock_len;
    memcpy(&cmdblock_len, raw + 0x10, 4);

    uint32_t cmd_code = 0, img_w = 0, img_h = 0, bpp = 0, data_start = 0;
    size_t cmd_off = 0;
    for (size_t try_off = 24; try_off < 24 + 240 && try_off + 32 <= raw_len; try_off += 120) {
        memcpy(&cmd_code, raw + try_off, 4);
        if (cmd_code == 0x11) { cmd_off = try_off; break; }
    }
    if (cmd_code != 0x11) { free(raw); return 9; }
    memcpy(&img_w,      raw + cmd_off + 8,  4);
    memcpy(&img_h,      raw + cmd_off + 12, 4);
    memcpy(&bpp,        raw + cmd_off + 16, 4);
    memcpy(&data_start, raw + cmd_off + 20, 4);
    if ((bpp != 3 && bpp != 4) || img_w == 0 || img_h == 0) { free(raw); return 10; }
    size_t pix_off = 0x20 + (size_t)cmdblock_len + (size_t)data_start;
    size_t pix_len = (size_t)img_w * (size_t)img_h * (size_t)bpp;
    if (pix_off + pix_len > raw_len) { free(raw); return 11; }

    /* Black-fill dst first (covers the pad area). */
    for (int row = 0; row < dst_h; row++) {
        memset(dst + (size_t)row * dst_stride, 0, (size_t)dst_w * 4);
    }

    /* Center-crop horizontally, center-pad vertically. */
    int src_w = (int)img_w, src_h = (int)img_h;
    int crop_w = src_w < dst_w ? src_w : dst_w;
    int crop_h = src_h < dst_h ? src_h : dst_h;
    int src_x0 = (src_w - crop_w) / 2;
    int src_y0 = (src_h - crop_h) / 2;
    int dst_x0 = (dst_w - crop_w) / 2;
    int dst_y0 = (dst_h - crop_h) / 2;
    int src_stride = src_w * (int)bpp;

    for (int row = 0; row < crop_h; row++) {
        const uint8_t* src = raw + pix_off + (size_t)(src_y0 + row) * src_stride
                                          + (size_t)src_x0 * bpp;
        uint8_t* d = dst + (size_t)(dst_y0 + row) * dst_stride + (size_t)dst_x0 * 4;
        for (int col = 0; col < crop_w; col++) {
            uint8_t sr = src[0], sg = src[1], sb = src[2];
            if (swap_rb) {
                d[0] = sb; d[1] = sg; d[2] = sr; d[3] = 0xFF;
            } else {
                d[0] = sr; d[1] = sg; d[2] = sb; d[3] = 0xFF;
            }
            src += bpp;
            d += 4;
        }
    }

    free(raw);
    LOG("canim_load: %ux%u → dst %dx%d (swap_rb=%d) OK\n", img_w, img_h, dst_w, dst_h, swap_rb);
    return 0;
}

/* File-loaded variant — slurp the .canim into memory, hand off to
 * canim_load_from_mem for parse+decompress. */
static int canim_load_into_buf(const char* path,
                               uint8_t* dst, int dst_stride,
                               int dst_w, int dst_h,
                               int swap_rb) {
    FILE* f = fopen(path, "rb");
    if (!f) { LOG("canim_load: open %s failed errno=%d\n", path, errno); return 2; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz <= 0 || fsz > 16*1024*1024) { fclose(f); return 3; }
    uint8_t* zbuf = (uint8_t*)malloc((size_t)fsz);
    if (!zbuf) { fclose(f); return 4; }
    if (fread(zbuf, 1, (size_t)fsz, f) != (size_t)fsz) { free(zbuf); fclose(f); return 5; }
    fclose(f);
    int rc = canim_load_from_mem(zbuf, (size_t)fsz, dst, dst_stride, dst_w, dst_h, swap_rb);
    free(zbuf);
    return rc;
}

int run_canim_splash(int argc, char **argv, int default_duration_s) {
    int rc;
    int xres = DISP_W, yres = DISP_H;
    int duration_s = (int)get_arg_f(argc, argv, "duration_s", (float)default_duration_s);
    const char* path = get_arg_s(argc, argv, "path", CANIM_DEFAULT_PATH);

    LOG("canim_splash: %dx%d duration_s=%d path=%s\n", xres, yres, duration_s, path);

    /* ---- Standard EGL + pixmap setup ---- */
    screen_context_t ctx_app = NULL;
    rc = screen_create_context(&ctx_app, SCREEN_APPLICATION_CONTEXT);
    if (rc) return 12;

    int fmt   = SCREEN_FORMAT_RGBX8888;
    int usage = SCREEN_USAGE_READ | SCREEN_USAGE_WRITE;
    int bsz[2] = { xres, yres };

    screen_pixmap_t cap_pixmap = NULL;
    if (screen_create_pixmap(&cap_pixmap, ctx_app)) return 13;
    screen_set_pixmap_property_iv(cap_pixmap, SCREEN_PROPERTY_FORMAT,      &fmt);
    screen_set_pixmap_property_iv(cap_pixmap, SCREEN_PROPERTY_USAGE,       &usage);
    screen_set_pixmap_property_iv(cap_pixmap, SCREEN_PROPERTY_BUFFER_SIZE, bsz);
    if (screen_create_pixmap_buffer(cap_pixmap)) return 14;

    screen_buffer_t cap_buf = NULL;
    void *cap_ptr = NULL;
    int   cap_stride = 0;
    screen_get_pixmap_property_pv(cap_pixmap, SCREEN_PROPERTY_RENDER_BUFFERS, (void**)&cap_buf);
    screen_get_buffer_property_pv(cap_buf, SCREEN_PROPERTY_POINTER, &cap_ptr);
    screen_get_buffer_property_iv(cap_buf, SCREEN_PROPERTY_STRIDE,  &cap_stride);
    if (!cap_ptr) return 15;

    screen_window_t cl_win = NULL;
    if (screen_create_window(&cl_win, ctx_app)) return 16;
    screen_set_window_property_cv(cl_win, SCREEN_PROPERTY_ID_STRING, 2, "33");
    int cl_size[2] = { DISP_W, DISP_H };
    int cl_fmt   = SCREEN_FORMAT_RGBX8888;
    int cl_usage = SCREEN_USAGE_OPENGL_ES2;
    int cl_vis   = 0;
    screen_set_window_property_iv(cl_win, SCREEN_PROPERTY_SIZE,        cl_size);
    screen_set_window_property_iv(cl_win, SCREEN_PROPERTY_BUFFER_SIZE, cl_size);
    screen_set_window_property_iv(cl_win, SCREEN_PROPERTY_FORMAT,      &cl_fmt);
    screen_set_window_property_iv(cl_win, SCREEN_PROPERTY_USAGE,       &cl_usage);
    screen_set_window_property_iv(cl_win, SCREEN_PROPERTY_VISIBLE,     &cl_vis);
    screen_flush_context(ctx_app, 0);
    if (screen_create_window_buffers(cl_win, 2)) return 17;

    /* Load canim into the pixmap via the shared helper. swap_rb=1 because
     * the GL shader (fs_src) does an R/B swap, so pixmap bytes are stored
     * as BGRA to come out as RGB on screen. */
    if (canim_load_into_buf(path, (uint8_t*)cap_ptr, cap_stride, xres, yres, 1) != 0) {
        LOG("canim_splash: canim_load_into_buf failed\n");
        return 18;
    }

    /* ---- EGL + GL upload + draw, same path as run_idle_logo ---- */
    EGLDisplay egl_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(egl_dpy, NULL, NULL);
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };
    EGLConfig egl_cfg; EGLint ncfg;
    eglChooseConfig(egl_dpy, cfg_attr, &egl_cfg, 1, &ncfg);

    EGLSurface egl_surf = eglCreateWindowSurface(egl_dpy, egl_cfg,
                                                 (EGLNativeWindowType)cl_win, NULL);
    if (egl_surf == EGL_NO_SURFACE) return 18;
    EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext egl_ctx = eglCreateContext(egl_dpy, egl_cfg, EGL_NO_CONTEXT, ctx_attr);
    if (egl_ctx == EGL_NO_CONTEXT) return 19;
    eglMakeCurrent(egl_dpy, egl_surf, egl_surf, egl_ctx);

    while (eglGetError() != EGL_SUCCESS) {}
    load_gl();

    uint8_t *upload_buf = NULL;
    const void *px = cap_ptr;
    if (cap_stride != xres * 4) {
        upload_buf = malloc((size_t)xres * (size_t)yres * 4);
        if (!upload_buf) return 20;
        for (int r = 0; r < yres; r++)
            memcpy(upload_buf + (size_t)r * xres * 4,
                   (uint8_t*)cap_ptr + (size_t)r * cap_stride,
                   (size_t)xres * 4);
        px = upload_buf;
    }

    GLuint tex = 0;
    gl_GenTextures(1, &tex);
    gl_ActiveTexture(GL_TEXTURE0);
    gl_BindTexture(GL_TEXTURE_2D, tex);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    gl_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, xres, yres, 0,
                  GL_RGBA, GL_UNSIGNED_BYTE, px);

    GLuint vs = gl_CreateShader(GL_VERTEX_SHADER);
    gl_ShaderSource(vs, 1, &vs_src, NULL); gl_CompileShader(vs);
    GLuint fs2 = gl_CreateShader(GL_FRAGMENT_SHADER);
    gl_ShaderSource(fs2, 1, &fs_src, NULL); gl_CompileShader(fs2);
    GLuint prog = gl_CreateProgram();
    gl_AttachShader(prog, vs); gl_AttachShader(prog, fs2);
    gl_LinkProgram(prog); gl_UseProgram(prog);
    gl_Uniform1i(gl_GetUniformLocation(prog, "tex"), 0);

    float verts[24];
    build_quad_xy(verts, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
    GLint pos_loc = gl_GetAttribLocation(prog, "pos");
    GLint uv_loc  = gl_GetAttribLocation(prog, "uv");
    gl_EnableVertexAttribArray(pos_loc);
    gl_EnableVertexAttribArray(uv_loc);
    gl_VertexAttribPointer(pos_loc, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), verts);
    gl_VertexAttribPointer(uv_loc,  2, GL_FLOAT, GL_FALSE, 4*sizeof(float), verts+2);

    cl_vis = 1;
    screen_set_window_property_iv(cl_win, SCREEN_PROPERTY_VISIBLE, &cl_vis);
    screen_flush_context(ctx_app, 0);

    gl_Viewport(0, 0, DISP_W, DISP_H);
    gl_ClearColor(0.f, 0.f, 0.f, 1.f);
    for (int i = 0; i < 3; i++) {
        gl_Clear(GL_COLOR_BUFFER_BIT);
        gl_DrawArrays(GL_TRIANGLES, 0, 6);
        eglSwapBuffers(egl_dpy, egl_surf);
    }

    LOG("canim_splash: posted to displayable 33.\n");

    if (duration_s > 0) {
        sleep(duration_s);
        LOG("canim_splash: duration_s=%d elapsed, exiting.\n", duration_s);
    } else {
        while (1) sleep(60);
    }
    return 0;
}

/* When built as the standalone aa_cluster_decoder binary (-DSTANDALONE),
 * define g_verbose ourselves and provide a small main() wrapper.
 * When linked into cluster_mirror, this section is omitted and
 * cluster_mirror.c provides g_verbose + main + dispatch. */
