#include <android/log.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Eclipse writes its log to stdout/stderr, which Android discards. Forward both to logcat (tag "Eclipse"). */
static int log_pipe[2];

static void *log_pump(void *arg) {
    (void) arg;
    char buf[1024];
    size_t used = 0;
    ssize_t n;
    while ((n = read(log_pipe[0], buf + used, sizeof(buf) - 1 - used)) > 0) {
        used += (size_t) n;
        buf[used] = '\0';
        char *start = buf, *nl;
        while ((nl = strchr(start, '\n')) != NULL) {
            *nl = '\0';
            __android_log_write(ANDROID_LOG_INFO, "Eclipse", start);
            start = nl + 1;
        }
        used = strlen(start);
        memmove(buf, start, used + 1);
        if (used >= sizeof(buf) - 1) {
            __android_log_write(ANDROID_LOG_INFO, "Eclipse", buf);
            used = 0;
        }
    }
    return NULL;
}

void android_stdio_to_logcat(void) {
    static int done = 0;
    if (done) {
        return;
    }
    done = 1;
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (pipe(log_pipe) != 0) {
        return;
    }
    dup2(log_pipe[1], STDOUT_FILENO);
    dup2(log_pipe[1], STDERR_FILENO);
    pthread_t thread;
    if (pthread_create(&thread, NULL, log_pump, NULL) == 0) {
        pthread_detach(thread);
    }
}
