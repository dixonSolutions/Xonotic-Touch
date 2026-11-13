/* Resident bridge shared by the Android UI and whichever engine is active. */
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define QUEUE_CAP 32
#define COMMAND_CAP 1024
#define STATUS_CAP 32768
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static char queue[QUEUE_CAP][COMMAND_CAP];
static unsigned head, count;
static char status[STATUS_CAP];
static char permissions[STATUS_CAP];

JNIEXPORT jboolean JNICALL Java_io_github_dixonsolutions_xonotictouch_NativeBridge_enqueueBytes(JNIEnv *env, jobject self, jbyteArray command)
{
    (void)self;
    if (!command) return JNI_FALSE;
    jsize n = (*env)->GetArrayLength(env, command);
    if (n <= 0 || n >= COMMAND_CAP - 1) return JNI_FALSE;
    char bytes[COMMAND_CAP];
    (*env)->GetByteArrayRegion(env, command, 0, n, (jbyte *)bytes);
    if ((*env)->ExceptionCheck(env) || memchr(bytes, 0, (size_t)n)) return JNI_FALSE;
    int ok = 0;
    pthread_mutex_lock(&mutex);
    if (count < QUEUE_CAP) {
        memcpy(queue[(head + count) % QUEUE_CAP], bytes, (size_t)n);
        queue[(head + count) % QUEUE_CAP][n] = '\n';
        queue[(head + count) % QUEUE_CAP][n + 1] = 0;
        count++;
        ok = 1;
    }
    pthread_mutex_unlock(&mutex);
    return ok ? JNI_TRUE : JNI_FALSE;
}

static jbyteArray copy_status(JNIEnv *env, const char *source)
{
    char copy[STATUS_CAP];
    pthread_mutex_lock(&mutex);
    memcpy(copy, source, sizeof copy);
    pthread_mutex_unlock(&mutex);
    jsize n = (jsize)strlen(copy);
    jbyteArray result = (*env)->NewByteArray(env, n);
    if (result) (*env)->SetByteArrayRegion(env, result, 0, n, (const jbyte *)copy);
    return result;
}
JNIEXPORT jbyteArray JNICALL Java_io_github_dixonsolutions_xonotictouch_NativeBridge_statusBytes(JNIEnv *env, jobject self)
{
    (void)self;
    return copy_status(env, status);
}
JNIEXPORT jbyteArray JNICALL Java_io_github_dixonsolutions_xonotictouch_NativeBridge_permissionsBytes(JNIEnv *env, jobject self)
{
    (void)self;
    return copy_status(env, permissions);
}

int nativeui_pop(char *out, size_t cap)
{
    int ok = 0;
    pthread_mutex_lock(&mutex);
    if (count) {
        snprintf(out, cap, "%s", queue[head]);
        head = (head + 1) % QUEUE_CAP;
        count--;
        ok = 1;
    }
    pthread_mutex_unlock(&mutex);
    return ok;
}
void nativeui_publish(const char *s)
{
    pthread_mutex_lock(&mutex);
    snprintf(status, sizeof status, "%s", s ? s : "");
    pthread_mutex_unlock(&mutex);
}

void nativeui_publish_permissions(const char *s)
{
    pthread_mutex_lock(&mutex);
    snprintf(permissions, sizeof permissions, "%s", s ? s : "");
    pthread_mutex_unlock(&mutex);
}

/* The engine has put its first frame on screen (VID_Finish): the Android
 * activity lifts the boot picture it kept over the SDL surface until then. */
static volatile int presented;
void nativeui_presented(void)
{
    presented = 1;
}
JNIEXPORT jboolean JNICALL Java_io_github_dixonsolutions_xonotictouch_NativeBridge_presented(JNIEnv *env, jobject self)
{
    (void)env;
    (void)self;
    return presented ? JNI_TRUE : JNI_FALSE;
}

/* ---------------------------------------------------------------------------
 * Front-end bridge (protocol 1, docs/FRONTEND_BRIDGE.md).
 *
 * The engine thread hands JSON snapshots per topic and events to
 * nativeui_fe_put (through touch_frontend.c's sink). This keeps the newest
 * snapshot of each topic and a bounded queue of events, and wakes the UI
 * thread once (NativeBridge.onFrontendDirty) until it has taken them. Events
 * (and the UI bridge's messages, which must not be coalesced) queue in order;
 * past FE_EVENTS an "overflow" event tells the UI to ask for fresh state
 * (NativeBridge.feTake), so a burst of changes costs one wake-up. The UI's
 * heartbeat and a load's Cancel are plain timestamps/flags the engine reads
 * without waiting for its command queue, which a map load does not drain.
 */
#include <stdlib.h>
#include <time.h>

#define FE_VERSION 1
#define FE_TOPICS 16
#define FE_EVENTS 256
static struct { char name[16]; char *json; int fresh; } fe_topics[FE_TOPICS];
static char *fe_events[FE_EVENTS];
static unsigned fe_nevents;
static int fe_overflow; /* events were dropped: the UI asks for fresh snapshots */
static int fe_wake_pending;
static long long fe_beat_ms = -1;
static volatile int fe_cancel;
static JavaVM *fe_vm;
static jclass fe_class;
static jmethodID fe_dirty;

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
    JNIEnv *env;
    (void)reserved;
    fe_vm = vm;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK)
        return JNI_VERSION_1_6;
    /* Resolved here, on a Java thread whose class loader knows the app: the
     * engine thread's FindClass would only see the system's classes. A dex
     * without the callback (older code) leaves it NULL: no wake-ups. */
    jclass cls = (*env)->FindClass(env, "io/github/dixonsolutions/xonotictouch/NativeBridge");
    if (cls) {
        fe_dirty = (*env)->GetStaticMethodID(env, cls, "onFrontendDirty", "()V");
        if (fe_dirty)
            fe_class = (jclass)(*env)->NewGlobalRef(env, cls);
    }
    if ((*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);
    return JNI_VERSION_1_6;
}

int nativeui_fe_version(void)
{
    return FE_VERSION;
}

static void fe_wake(void)
{
    JNIEnv *env;
    int attached = 0;
    if (!fe_vm || !fe_class || !fe_dirty)
        return;
    if ((*fe_vm)->GetEnv(fe_vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*fe_vm)->AttachCurrentThread(fe_vm, (void *)&env, NULL) != JNI_OK)
            return;
        attached = 1;
    }
    (*env)->CallStaticVoidMethod(env, fe_class, fe_dirty);
    if ((*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);
    if (attached)
        (*fe_vm)->DetachCurrentThread(fe_vm);
}

void nativeui_fe_put(const char *topic, const char *json, int event)
{
    int wake = 0;
    char *copy;
    if (!topic)
        return;
    if (event) {
        /* queued with its topic: "event" for events, "ui" for the UI bridge's messages */
        size_t n = strlen(topic) + strlen(json ? json : "{}") + 2;
        copy = (char *)malloc(n);
        if (copy)
            snprintf(copy, n, "%s\x1f%s", topic, json ? json : "{}");
    } else
        copy = strdup(json ? json : "{}");
    if (!copy)
        return;
    pthread_mutex_lock(&mutex);
    if (event) {
        if (fe_nevents < FE_EVENTS)
            fe_events[fe_nevents++] = copy;
        else {
            free(copy); /* nobody is taking them: the oldest stand */
            fe_overflow = 1;
        }
    } else {
        int i, slot = -1;
        for (i = 0; i < FE_TOPICS; i++) {
            if (fe_topics[i].json && !strcmp(fe_topics[i].name, topic)) { slot = i; break; }
            if (!fe_topics[i].json && slot < 0) slot = i;
        }
        if (slot >= 0) {
            free(fe_topics[slot].json);
            snprintf(fe_topics[slot].name, sizeof fe_topics[slot].name, "%s", topic);
            fe_topics[slot].json = copy;
            fe_topics[slot].fresh = 1;
        } else
            free(copy);
    }
    if (!fe_wake_pending) {
        fe_wake_pending = 1;
        wake = 1;
    }
    pthread_mutex_unlock(&mutex);
    if (wake)
        fe_wake();
}

long long nativeui_fe_beat_age_ms(void)
{
    long long beat;
    pthread_mutex_lock(&mutex);
    beat = fe_beat_ms;
    pthread_mutex_unlock(&mutex);
    return beat < 0 ? -1 : now_ms() - beat;
}

int nativeui_fe_cancel_requested(void)
{
    int c = fe_cancel;
    fe_cancel = 0;
    return c;
}

/* Records of "topic\x1fjson", separated by \x1e: every topic that changed
 * since the last take (all of them with all != 0), then the queued messages
 * in order ("event\x1fjson", "ui\x1fjson"). */
JNIEXPORT jbyteArray JNICALL Java_io_github_dixonsolutions_xonotictouch_NativeBridge_feTakeBytes(JNIEnv *env, jobject self, jboolean all)
{
    size_t n = 0, cap = 1;
    char *out;
    unsigned i;
    (void)self;
    pthread_mutex_lock(&mutex);
    for (i = 0; i < FE_TOPICS; i++)
        if (fe_topics[i].json && (all || fe_topics[i].fresh))
            cap += strlen(fe_topics[i].name) + strlen(fe_topics[i].json) + 2;
    for (i = 0; i < fe_nevents; i++)
        cap += strlen(fe_events[i]) + 8;
    cap += 48;
    out = (char *)malloc(cap);
    if (out) {
        for (i = 0; i < FE_TOPICS; i++)
            if (fe_topics[i].json && (all || fe_topics[i].fresh)) {
                n += (size_t)sprintf(out + n, "%s\x1f%s\x1e", fe_topics[i].name, fe_topics[i].json);
                fe_topics[i].fresh = 0;
            }
        for (i = 0; i < fe_nevents; i++) {
            n += (size_t)sprintf(out + n, "%s\x1e", fe_events[i]);
            free(fe_events[i]);
        }
        fe_nevents = 0;
        if (fe_overflow)
            n += (size_t)sprintf(out + n, "event\x1f{\"type\":\"overflow\"}\x1e");
        fe_overflow = 0;
    }
    fe_wake_pending = 0;
    pthread_mutex_unlock(&mutex);
    if (!out)
        return NULL;
    jbyteArray result = (*env)->NewByteArray(env, (jsize)n);
    if (result) (*env)->SetByteArrayRegion(env, result, 0, (jsize)n, (const jbyte *)out);
    free(out);
    return result;
}

JNIEXPORT void JNICALL Java_io_github_dixonsolutions_xonotictouch_NativeBridge_feBeat(JNIEnv *env, jobject self)
{
    (void)env;
    (void)self;
    pthread_mutex_lock(&mutex);
    fe_beat_ms = now_ms();
    pthread_mutex_unlock(&mutex);
}

JNIEXPORT void JNICALL Java_io_github_dixonsolutions_xonotictouch_NativeBridge_feCancel(JNIEnv *env, jobject self)
{
    (void)env;
    (void)self;
    fe_cancel = 1;
}

JNIEXPORT jint JNICALL Java_io_github_dixonsolutions_xonotictouch_NativeBridge_feVersion(JNIEnv *env, jobject self)
{
    (void)env;
    (void)self;
    return FE_VERSION;
}
