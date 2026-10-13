/*
 * Co-op split-screen on Android: the glue that replaces fork()+exec().
 *
 * Desktop and webOS start the second Moonlight session by forking and exec'ing the client in
 * "--coop-secondary" mode. On Android the client is a shared library inside the app process, so
 * the second session runs in an Android service with its own process
 * (org.eclipse.stream.CoopSecondaryService, android:process=":coop").
 *
 * Two halves:
 *  - Coordinator side (main process): eclipse_android_coop_start()/_stop() call into the
 *    activity over JNI, and the secondary's process id is learned from a small pid file the
 *    secondary writes, because it is not our child and cannot be waitpid()'d.
 *  - Secondary side (":coop" process): nativeRun() is the JNI entry the service calls. It sets
 *    the data directory (SDL's Java glue is not initialised in that process, so SDL's own path
 *    functions cannot be used there), passes the launch parameters through the environment
 *    exactly as the fork path does, and runs the normal main() with the usual arguments.
 */
#ifdef __ANDROID__

#include <jni.h>
#include <SDL.h>
#include <SDL_system.h>

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "android_coop.h"
#include "util/path.h"

/* main() of main.c, renamed by SDL_main.h on Android. */
extern int SDL_main(int argc, char *argv[]);

static char pid_file[512];

static void pid_file_set(const char *files_dir) {
    snprintf(pid_file, sizeof(pid_file), "%s/coop-secondary.pid", files_dir);
}

JNIEXPORT jint JNICALL
Java_org_eclipse_stream_CoopSecondaryService_nativeRun(JNIEnv *env, jclass cls, jstring jfiles,
                                                       jstring jsocket, jstring jhost, jint app_id,
                                                       jint width, jint height) {
    (void) cls;
    const char *files = (*env)->GetStringUTFChars(env, jfiles, NULL);
    const char *socket = (*env)->GetStringUTFChars(env, jsocket, NULL);
    const char *host = jhost != NULL ? (*env)->GetStringUTFChars(env, jhost, NULL) : NULL;
    if (files == NULL || socket == NULL) {
        return -1;
    }
    path_android_set_files_dir(files);
    pid_file_set(files);

    /* The same launch contract the fork path uses (coop_coord_spawn_secondary_for). */
    if (host != NULL && host[0] != '\0') {
        setenv("COOP_HOST_UUID", host, 1);
    }
    if (app_id > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", (int) app_id);
        setenv("COOP_APP_ID", buf, 1);
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", (int) width);
    setenv("COOP_STREAM_WIDTH", buf, 1);
    snprintf(buf, sizeof(buf), "%d", (int) height);
    setenv("COOP_STREAM_HEIGHT", buf, 1);

    FILE *pf = fopen(pid_file, "w");
    if (pf != NULL) {
        fprintf(pf, "%d\n", (int) getpid());
        fclose(pf);
    }

    char arg0[] = "eclipse";
    char arg1[] = "--coop-secondary";
    char *arg2 = strdup(socket);
    char *argv[] = {arg0, arg1, arg2, NULL};
    const int rc = SDL_main(3, argv);

    unlink(pid_file);
    free(arg2);
    return rc;
}

/* ---- coordinator side ---------------------------------------------------------------------- */

static bool call_activity_static(const char *name, const char *signature, ...) {
    JNIEnv *env = SDL_AndroidGetJNIEnv();
    jobject activity = SDL_AndroidGetActivity();
    if (env == NULL || activity == NULL) {
        return false;
    }
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID method = (*env)->GetStaticMethodID(env, cls, name, signature);
    if (method == NULL) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, cls);
        (*env)->DeleteLocalRef(env, activity);
        return false;
    }
    va_list ap;
    va_start(ap, signature);
    (*env)->CallStaticVoidMethodV(env, cls, method, ap);
    va_end(ap);
    bool ok = true;
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        ok = false;
    }
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
    return ok;
}

static void pid_file_ensure(void) {
    if (pid_file[0] == '\0') {
        const char *files = SDL_AndroidGetInternalStoragePath();
        pid_file_set(files != NULL ? files : "/data/local/tmp");
    }
}

int eclipse_android_coop_pid(void) {
    pid_file_ensure();
    FILE *f = fopen(pid_file, "r");
    if (f == NULL) {
        return 0;
    }
    int pid = 0;
    if (fscanf(f, "%d", &pid) != 1) {
        pid = 0;
    }
    fclose(f);
    return pid > 0 ? pid : 0;
}

bool eclipse_android_coop_alive(int pid) {
    if (pid <= 0) {
        return false;
    }
    return kill((pid_t) pid, 0) == 0 || errno == EPERM;
}

int eclipse_android_coop_start(const char *socket, const char *host, int app_id, int width,
                               int height) {
    pid_file_ensure();
    unlink(pid_file); /* never trust a leftover from an earlier session */
    JNIEnv *env = SDL_AndroidGetJNIEnv();
    if (env == NULL) {
        return -1;
    }
    jstring jsocket = (*env)->NewStringUTF(env, socket);
    jstring jhost = (*env)->NewStringUTF(env, host != NULL ? host : "");
    const bool ok = call_activity_static("startCoopSecondary",
                                         "(Ljava/lang/String;Ljava/lang/String;III)V",
                                         jsocket, jhost, (jint) app_id, (jint) width, (jint) height);
    (*env)->DeleteLocalRef(env, jsocket);
    (*env)->DeleteLocalRef(env, jhost);
    if (!ok) {
        return -1;
    }
    /* The service process has to be created and the library loaded before the pid file appears. */
    for (int waited = 0; waited < 10000; waited += 50) {
        const int pid = eclipse_android_coop_pid();
        if (pid > 0 && eclipse_android_coop_alive(pid)) {
            return pid;
        }
        usleep(50 * 1000);
    }
    return -1;
}

void eclipse_android_coop_stop(void) {
    (void) call_activity_static("stopCoopSecondary", "()V");
}

#endif /* __ANDROID__ */
