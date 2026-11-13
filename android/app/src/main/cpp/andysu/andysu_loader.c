/*
 * libandysu_xonotic_loader.so: the resident part of the Godnotic live engine swap.
 *
 * SDLActivity runs `AndySU_SDL_main` (XonoticActivity.getMainFunction) on the SDL thread instead
 * of the engine's SDL_main. This library loads the engine library (libmain[.<hash8>].so, found next
 * to this file) RTLD_LOCAL, resolves its single exported symbol andysu_engine_v1, and runs it.
 * SDL itself (libSDL2.so) stays one shared copy, so SDL's state is common to every engine copy.
 *
 * Swap (stage 2a): when the engine polls swap_requested() and sees 1, it saves its state with
 * put_state(), shuts down and returns ANDYSU_RUN_SWAP. The loader then runs the engine that was
 * pre-flighted (dlopen + ABI check) when the swap was requested, with swapped_in=1 and the saved
 * state. Old engine libraries are never dlclose()d (bionic cannot reliably unload them and the old
 * copy may still own SDL callbacks); each swap leaves one dormant mapping behind.
 * Rollback layer 2: if the new engine abandons its start (abort_start() -> longjmp), the loader
 * reloads the previous build with ANDROID_DLEXT_FORCE_LOAD (a fresh mapping of the same file,
 * because the old copy is dirty after its shutdown) and runs it with swapped_in=2.
 *
 * Also owns the JNI entry the Java side used to send to the engine (nativeInputDevices): the
 * engine library is no longer loaded by System.loadLibrary. Values are replayed into every engine.
 *
 * Swaps are requested by the app (EngineSwap.nativeRequestSwap, called from the LiveSwapHandler
 * the app registered with the AndySU host, D29) and reported back through static Java methods
 * (EngineSwap.onEngineEvent: started / failed(reason) / healthy; EngineSwap.beforeEngineStart:
 * the app unpacks the new release's data between the old engine's teardown and the new start).
 *
 * TEST HOOK (off unless the file <external files dir>/andysu-swap-hook exists): a thread polls
 * <external files dir>/andysu-swap-to once per second. Contents: line 1 = absolute path of an
 * engine .so readable by the app, line 2 optional `fail_init`. Used by the milestone 2a probe.
 */
#include <android/dlext.h>
#include <android/log.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "andysu_swap.h"

#define EVENT_TAG "AndySU-Event"
#define TAG "AndySU-Loader"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static const andysu_engine *volatile g_engine;
/* nativeInputDevices state: bit 3 = the activity has reported at all. */
static atomic_int g_input_seen;
static atomic_int g_input_bits;

static atomic_int g_swap_flag;
static char g_next_path[640];
static void *g_next_handle;
static const andysu_engine *g_next_engine;
static char g_pkg[160];
static char g_fail_reason[300];

/* Java side (EngineSwap). Absent in builds without it: everything below then does nothing. */
static JavaVM *g_vm;
static jclass g_swap_cls;
static jmethodID g_on_event, g_before_start;
enum { EV_STARTED = 1, EV_FAILED = 2, EV_HEALTHY = 3 };

static char *g_state; /* last put_state() copy */
static size_t g_state_len;
static int g_state_valid;
static int g_trial;
static jmp_buf g_jb;

/* ------------------------------------------------------------------ events and process stats */

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void emit_json(const char *json) { __android_log_write(ANDROID_LOG_INFO, EVENT_TAG, json); }

static void emitf(const char *fmt, ...) {
    char buf[1536];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    emit_json(buf);
}

typedef struct { int fds, threads; long rss_kb; int libmain_maps; char maps[512]; char kinds[160]; } stats;

static void get_stats(stats *s) {
    memset(s, 0, sizeof *s);
    DIR *d = opendir("/proc/self/fd");
    if (d) {
        struct dirent *de;
        int pk3 = 0, sock = 0, anon = 0, pipes = 0, dev = 0, other = 0;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            char lp[64], tgt[256];
            snprintf(lp, sizeof lp, "/proc/self/fd/%s", de->d_name);
            ssize_t n = readlink(lp, tgt, sizeof tgt - 1);
            if (n < 0) continue; /* the DIR's own fd */
            tgt[n] = 0;
            s->fds++;
            if (strstr(tgt, ".pk3")) pk3++;
            else if (!strncmp(tgt, "socket:", 7)) sock++;
            else if (!strncmp(tgt, "anon_inode:", 11)) anon++;
            else if (!strncmp(tgt, "pipe:", 5)) pipes++;
            else if (!strncmp(tgt, "/dev/", 5)) dev++;
            else other++;
        }
        closedir(d);
        snprintf(s->kinds, sizeof s->kinds, "pk3=%d,socket=%d,anon=%d,pipe=%d,dev=%d,other=%d", pk3, sock, anon, pipes, dev, other);
    }
    FILE *f = fopen("/proc/self/status", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "VmRSS:", 6)) s->rss_kb = atol(line + 6);
            else if (!strncmp(line, "Threads:", 8)) s->threads = atoi(line + 8);
        }
        fclose(f);
    }
    f = fopen("/proc/self/maps", "r");
    if (f) {
        char line[1024], last[200] = "";
        while (fgets(line, sizeof line, f)) {
            char *p = strstr(line, "/libmain.");
            if (!p) continue;
            p++;
            char *e = strpbrk(p, " \n");
            if (e) *e = 0;
            if (!strcmp(p, last)) continue;
            snprintf(last, sizeof last, "%s", p);
            int seen = 0; /* distinct names only */
            if (strstr(s->maps, p)) seen = 1;
            if (!seen) {
                s->libmain_maps++;
                size_t n = strlen(s->maps);
                snprintf(s->maps + n, sizeof s->maps - n, "%s%s", n ? "," : "", p);
            }
        }
        fclose(f);
    }
}

static void emit_stage(const char *event, const char *lib, const andysu_engine *e, int swapped) {
    stats s;
    get_stats(&s);
    emitf("{\"event\":\"%s\",\"pid\":%d,\"lib\":\"%s\",\"build_id\":\"%s\",\"swapped\":%d,\"fds\":%d,\"threads\":%d,"
          "\"rss_kb\":%ld,\"libmain_maps\":%d,\"maps\":\"%s\",\"fd_kinds\":\"%s\",\"t_ms\":%lld}",
          event, (int)getpid(), lib ? lib : "", e && e->build_id ? e->build_id : "", swapped, s.fds, s.threads, s.rss_kb,
          s.libmain_maps, s.maps, s.kinds, now_ms());
}

/* ------------------------------------------------------------------ Java side */

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    g_vm = vm;
    JNIEnv *env;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_VERSION_1_6;
    jclass c = (*env)->FindClass(env, "io/github/dixonsolutions/xonotictouch/EngineSwap");
    if (!c) { (*env)->ExceptionClear(env); return JNI_VERSION_1_6; }
    g_swap_cls = (*env)->NewGlobalRef(env, c);
    g_on_event = (*env)->GetStaticMethodID(env, c, "onEngineEvent", "(ILjava/lang/String;)V");
    g_before_start = (*env)->GetStaticMethodID(env, c, "beforeEngineStart", "(I)I");
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); g_on_event = NULL; g_before_start = NULL; }
    return JNI_VERSION_1_6;
}

static JNIEnv *java_env(void) {
    JNIEnv *env = NULL;
    if (!g_vm) return NULL;
    if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED)
        (*g_vm)->AttachCurrentThread(g_vm, &env, NULL);
    return env;
}

static void notify_java(int kind, const char *detail) {
    JNIEnv *env = java_env();
    if (!env || !g_on_event) return;
    jstring js = (*env)->NewStringUTF(env, detail ? detail : "");
    (*env)->CallStaticVoidMethod(env, g_swap_cls, g_on_event, (jint)kind, js);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, js);
}

/* 0 = go on. The app unpacks the data of the release that is about to run here: the new release's
 * (swapped 1) or, after an in-process rollback, the previous release's (swapped 2). */
static int java_before_start(int swapped) {
    JNIEnv *env = java_env();
    if (!env || !g_before_start) return 0;
    jint rc = (*env)->CallStaticIntMethod(env, g_swap_cls, g_before_start, (jint)swapped);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return 1; }
    return rc;
}

/* ------------------------------------------------------------------ loader API for the engine */

static int api_swap_requested(void) { return atomic_load(&g_swap_flag); }

static void api_put_state(const char *p, size_t n) {
    free(g_state);
    g_state = malloc(n + 1);
    if (!g_state) { g_state_len = 0; g_state_valid = 0; return; }
    memcpy(g_state, p, n);
    g_state[n] = 0;
    g_state_len = n;
    g_state_valid = 1;
}

static const char *api_get_state(size_t *n) {
    if (n) *n = g_state_valid ? g_state_len : 0;
    return g_state_valid ? g_state : NULL;
}

static void api_mark_healthy(void) {
    int was_trial = g_trial;
    g_trial = 0;
    if (was_trial) notify_java(EV_HEALTHY, "");
    emitf("{\"event\":\"engine_healthy\",\"pid\":%d,\"t_ms\":%lld}", (int)getpid(), now_ms());
}

/* Trial only: the new engine gives up its start (Sys_Error during Host_Init). */
static void api_abort_start(const char *why) {
    if (!g_trial) return;
    char esc[300];
    size_t j = 0;
    for (size_t i = 0; why && why[i] && j + 2 < sizeof esc; i++) {
        unsigned char c = (unsigned char)why[i];
        if (c == '"' || c == '\\') esc[j++] = '\\';
        esc[j++] = (c < 32) ? ' ' : (char)c;
    }
    esc[j] = 0;
    snprintf(g_fail_reason, sizeof g_fail_reason, "%s", esc);
    emitf("{\"event\":\"engine_start_failed\",\"pid\":%d,\"error\":\"%s\"}", (int)getpid(), esc);
    longjmp(g_jb, 1);
}

static const andysu_loader_api g_api = {
    ANDYSU_ENGINE_ABI, sizeof(andysu_loader_api), api_swap_requested, api_put_state, api_get_state,
    api_mark_healthy, emit_json, api_abort_start,
};

/* JNI: io.github.dixonsolutions.xonotictouch.XonoticActivity.nativeInputDevices (static native). */
JNIEXPORT void JNICALL Java_io_github_dixonsolutions_xonotictouch_XonoticActivity_nativeInputDevices(
    JNIEnv *env, jclass cls, jboolean touch, jboolean keyboard, jboolean mouse) {
    (void)env;
    (void)cls;
    atomic_store(&g_input_bits, (touch ? 1 : 0) | (keyboard ? 1 : 0) << 1 | (mouse ? 1 : 0) << 2);
    atomic_store(&g_input_seen, 1);
    const andysu_engine *e = g_engine;
    if (e && e->input_devices) e->input_devices(touch, keyboard, mouse);
}

/* ------------------------------------------------------------------ loading */

/* Open an engine library and check its entry. force=1: a fresh mapping even if this file is loaded. */
static const andysu_engine *open_engine(const char *path, int force, void **handle, char *err, size_t errcap) {
    void *h;
    if (force) {
        android_dlextinfo ext;
        memset(&ext, 0, sizeof ext);
        ext.flags = ANDROID_DLEXT_FORCE_LOAD;
        h = android_dlopen_ext(path, RTLD_NOW | RTLD_LOCAL, &ext);
    } else {
        h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    }
    if (!h) { snprintf(err, errcap, "dlopen: %s", dlerror()); return NULL; }
    andysu_engine_fn fn = (andysu_engine_fn)dlsym(h, ANDYSU_ENGINE_SYMBOL);
    const andysu_engine *e = fn ? fn() : NULL;
    if (!e || e->abi != ANDYSU_ENGINE_ABI || e->size < sizeof(andysu_engine) || !e->run) {
        snprintf(err, errcap, "missing or incompatible %s", ANDYSU_ENGINE_SYMBOL);
        return NULL; /* the mapping stays (no dlclose, see above) */
    }
    *handle = h;
    return e;
}

/* The engine library sits next to this file: libmain.so, or libmain.<hash8>.so after renaming. */
static int find_engine(char *out, size_t cap) {
    Dl_info di;
    if (!dladdr((void *)find_engine, &di) || !di.dli_fname) return -1;
    char dir[512];
    snprintf(dir, sizeof dir, "%s", di.dli_fname);
    char *slash = strrchr(dir, '/');
    if (!slash) return -1;
    *slash = 0;
    snprintf(out, cap, "%s/libmain.so", dir);
    if (access(out, R_OK) == 0) return 0;
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (!strncmp(e->d_name, "libmain.", 8) && n > 11 && !strcmp(e->d_name + n - 3, ".so")) {
            snprintf(out, cap, "%s/%s", dir, e->d_name);
            found = 0;
            break;
        }
    }
    closedir(d);
    return found;
}

/* ------------------------------------------------------------------ TEST HOOK: swap trigger */

static void read_pkg(void) {
    FILE *f = fopen("/proc/self/cmdline", "r");
    if (!f) return;
    size_t n = fread(g_pkg, 1, sizeof g_pkg - 1, f);
    g_pkg[n] = 0;
    fclose(f);
}

static unsigned fnv8(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned long long h = 1469598103934665603ULL;
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        for (size_t i = 0; i < n; i++) { h ^= buf[i]; h *= 1099511628211ULL; }
    fclose(f);
    return (unsigned)(h >> 32);
}

/* Copy src to the app-private dir under a unique name, read-only (external storage is noexec and
 * Android 14+ wants code files read-only). The name is libmain.<hash8>.<seq>.so. */
static int stage_copy(const char *src, char *dst, size_t cap, int seq) {
    char dir[300];
    snprintf(dir, sizeof dir, "/data/data/%s/files/andysu-swap", g_pkg);
    mkdir(dir, 0700);
    snprintf(dst, cap, "%s/libmain.%08x.%d.so", dir, fnv8(src), seq);
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    unlink(dst);
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    chmod(dst, 0444);
    return 0;
}

/* Pre-flight a new engine library (copy to a unique read-only name, dlopen, ABI check) and flag
 * the swap. The running engine is not touched. 0 = the swap is under way, else err says why not. */
static int request_swap(const char *src, int fail_init, char *err, size_t errcap) {
    static int seq;
    char path[640];
    void *h = NULL;
    const andysu_engine *e = NULL;
    if (atomic_load(&g_swap_flag)) { snprintf(err, errcap, "a swap is already pending"); return 1; }
    if (stage_copy(src, path, sizeof path, ++seq)) {
        snprintf(err, errcap, "cannot copy %s (%s)", src, strerror(errno));
        return 1;
    }
    e = open_engine(path, 0, &h, err, errcap);
    if (e && g_engine && e->java_abi != g_engine->java_abi) {
        snprintf(err, errcap, "java_abi %u != running %u", e->java_abi, g_engine->java_abi);
        e = NULL;
    }
    if (!e) return 1; /* pre-flight failed: the running engine is untouched */
    if (fail_init) setenv("ANDYSU_TEST_FAIL_INIT", "1", 1); /* TEST HOOK, read by the engine */
    else unsetenv("ANDYSU_TEST_FAIL_INIT");
    snprintf(g_next_path, sizeof g_next_path, "%s", path);
    g_next_handle = h;
    g_next_engine = e;
    emitf("{\"event\":\"engine_swap_begin\",\"pid\":%d,\"t_ms\":%lld,\"from\":\"%s\",\"to\":\"%s\",\"lib\":\"%s\",\"fail_init\":%d}",
          (int)getpid(), now_ms(), g_engine && g_engine->build_id ? g_engine->build_id : "", e->build_id ? e->build_id : "", path,
          fail_init);
    atomic_store(&g_swap_flag, 1);
    return 0;
}

/* JNI: EngineSwap.nativeRequestSwap(String enginePath): null = accepted, else the reason it was not. */
JNIEXPORT jstring JNICALL Java_io_github_dixonsolutions_xonotictouch_EngineSwap_nativeRequestSwap(JNIEnv *env, jclass cls,
                                                                                                     jstring src) {
    (void)cls;
    char err[256] = "";
    const char *s = (*env)->GetStringUTFChars(env, src, NULL);
    int rc = request_swap(s, 0, err, sizeof err);
    if (rc) emitf("{\"event\":\"engine_swap_refused\",\"pid\":%d,\"src\":\"%s\",\"error\":\"%s\"}", (int)getpid(), s, err);
    (*env)->ReleaseStringUTFChars(env, src, s);
    return rc ? (*env)->NewStringUTF(env, err) : NULL;
}

/* TEST HOOK thread (only with the flag file): see the file comment. */
static void *test_hook_thread(void *arg) {
    (void)arg;
    char trig[400];
    snprintf(trig, sizeof trig, "/storage/emulated/0/Android/data/%s/files/andysu-swap-to", g_pkg);
    int tick = 0;
    for (;;) {
        sleep(1);
        if (++tick % 5 == 0) emit_stage("proc_stats", "", g_engine, 0); /* steady-state numbers */
        FILE *f = fopen(trig, "r");
        if (!f) continue;
        char src[512] = "", flag[64] = "", err[256] = "";
        if (fgets(src, sizeof src, f)) src[strcspn(src, "\r\n")] = 0;
        if (fgets(flag, sizeof flag, f)) flag[strcspn(flag, "\r\n")] = 0;
        fclose(f);
        unlink(trig);
        if (!src[0]) continue;
        if (request_swap(src, !strcmp(flag, "fail_init"), err, sizeof err))
            emitf("{\"event\":\"engine_swap_refused\",\"pid\":%d,\"src\":\"%s\",\"error\":\"%s\"}", (int)getpid(), src, err);
    }
    return NULL;
}

/* ------------------------------------------------------------------ the loop */

/* SDLActivity: nativeRunMain(library = this file, function = "AndySU_SDL_main"). */
__attribute__((visibility("default"))) int AndySU_SDL_main(int argc, char *argv[]) {
    static int started;
    char path[640], prev_path[640] = "", err[256];
    void *handle = NULL;
    const andysu_engine *e;
    int swapped = 0;

    if (find_engine(path, sizeof path)) {
        LOGE("no libmain*.so next to the loader");
        emitf("{\"event\":\"loader_failed\",\"pid\":%d,\"error\":\"engine library not found\"}", (int)getpid());
        return 1;
    }
    e = open_engine(path, 0, &handle, err, sizeof err);
    if (!e) {
        LOGE("%s: %s", path, err);
        emitf("{\"event\":\"loader_failed\",\"pid\":%d,\"lib\":\"%s\",\"error\":\"%s\"}", (int)getpid(), path, err);
        return 1;
    }
    if (!started) {
        started = 1;
        read_pkg();
        char flagfile[400];
        snprintf(flagfile, sizeof flagfile, "/storage/emulated/0/Android/data/%s/files/andysu-swap-hook", g_pkg);
        pthread_t t;
        if (access(flagfile, F_OK) == 0 && !pthread_create(&t, NULL, test_hook_thread, NULL)) pthread_detach(t);
    }
    for (;;) {
        g_engine = e;
        g_trial = (swapped == 1);
        emitf("{\"event\":\"loader_engine_loaded\",\"pid\":%d,\"lib\":\"%s\",\"build_id\":\"%s\",\"swapped\":%d}", (int)getpid(),
              path, e->build_id ? e->build_id : "", swapped);
        emit_stage("engine_start", path, e, swapped);
        if (atomic_load(&g_input_seen) && e->input_devices) {
            int b = atomic_load(&g_input_bits);
            e->input_devices(b & 1, (b >> 1) & 1, (b >> 2) & 1);
        }
        int rc;
        if (swapped >= 1 && java_before_start(swapped)) {
            /* After a rollback this is final (prev_path is gone): the previous engine must not run on
             * the failed release's data, so it does not start; the app has asked the host to relaunch. */
            snprintf(g_fail_reason, sizeof g_fail_reason, "preparing the %s release's data failed", swapped == 1 ? "new" : "previous");
            emitf("{\"event\":\"engine_start_failed\",\"pid\":%d,\"error\":\"%s\"}", (int)getpid(), g_fail_reason);
            rc = ANDYSU_RUN_FAILED;
        } else {
            if (swapped == 1) notify_java(EV_STARTED, "");
            if (setjmp(g_jb) == 0) rc = e->run(&g_api, argc, argv, swapped);
            else rc = ANDYSU_RUN_FAILED;
        }
        g_trial = 0;
        if (rc == ANDYSU_RUN_SWAP && g_next_engine) {
            emit_stage("engine_stopped", path, e, swapped); /* after the old engine's teardown */
            /* The old library stays mapped on purpose; remember its file for a rollback. */
            snprintf(prev_path, sizeof prev_path, "%s", path);
            snprintf(path, sizeof path, "%s", g_next_path);
            handle = g_next_handle;
            e = g_next_engine;
            g_next_handle = NULL;
            g_next_engine = NULL;
            atomic_store(&g_swap_flag, 0);
            swapped = 1;
            continue;
        }
        if (rc == ANDYSU_RUN_FAILED && prev_path[0]) {
            emit_stage("engine_stopped", path, e, swapped);
            notify_java(EV_FAILED, g_fail_reason); /* the app reports failed(reason) to the AndySU host */
            unsetenv("ANDYSU_TEST_FAIL_INIT");
            atomic_store(&g_swap_flag, 0);
            /* The failed copy is poisoned: never reused (its threads may still be parked). The
             * previous build is already mapped and dirty after its shutdown, so force a fresh map. */
            e = open_engine(prev_path, 1, &handle, err, sizeof err);
            if (!e) {
                LOGE("rollback to %s failed: %s", prev_path, err);
                emitf("{\"event\":\"rollback_failed\",\"pid\":%d,\"error\":\"%s\"}", (int)getpid(), err);
                return 1;
            }
            snprintf(path, sizeof path, "%s", prev_path);
            prev_path[0] = 0;
            emitf("{\"event\":\"engine_rolled_back\",\"pid\":%d,\"lib\":\"%s\",\"build_id\":\"%s\"}", (int)getpid(), path,
                  e->build_id ? e->build_id : "");
            swapped = 2;
            continue;
        }
        emitf("{\"event\":\"loader_engine_returned\",\"pid\":%d,\"lib\":\"%s\",\"rc\":%d}", (int)getpid(), path, rc);
        return rc;
    }
}
