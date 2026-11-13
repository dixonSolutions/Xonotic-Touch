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
