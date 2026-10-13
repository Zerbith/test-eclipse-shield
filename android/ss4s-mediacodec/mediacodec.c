/*
 * SS4S video module for Android (NVIDIA Shield / Android TV) using the NDK AMediaCodec API.
 *
 * STATUS: first draft, written without access to an Android NDK or a device. It has not been
 * compiled or run. Expect to iterate on it through the CI build and a real Shield.
 *
 * Design:
 *  - The decoder renders straight into a Surface owned by the Java side
 *    (org.eclipse.stream.EclipseActivity.getVideoSurface()), which sits BEHIND the transparent
 *    SDL surface that draws the LVGL user interface.
 *  - Eclipse passes one complete access unit per SS4S_PlayerVideoFeed() call
 *    (FRAME_START | FRAME_END). Chunked feeding is also handled by accumulating until FRAME_END.
 *  - Audio is NOT handled here; the stock `sdl` SS4S audio module is used.
 */
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/system_properties.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL2/SDL.h>

#include "ss4s/modapi.h"

#define TAG "EclipseMediaCodec"

/* Co-op feeds a 4K composite; give a busy decoder a little longer before a frame is dropped. */
#define FEED_INPUT_TIMEOUT_US 20000
#define OUTPUT_POLL_TIMEOUT_US 10000

static const SS4S_LibraryContext *LibContext = NULL;

struct SS4S_PlayerContext {
    int reserved;
};

struct SS4S_VideoInstance {
    AMediaCodec *codec;
    ANativeWindow *window;
    pthread_t outputThread;
    bool outputThreadStarted;
    atomic_bool running;

    unsigned char *frame;
    size_t frameSize;
    size_t frameCapacity;

    /* Diagnostics, written by the feed thread only (output counters by the output thread). */
    uint64_t fed, droppedBusy, droppedOversize, largestFrame;
    atomic_uint_fast64_t rendered;
    size_t inputCapacitySeen;
};

static void Log(SS4S_LogLevel level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void Log(SS4S_LogLevel level, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (LibContext != NULL && LibContext->Log != NULL) {
        LibContext->Log(level, "MediaCodec", "%s", buf);
    } else {
        __android_log_print(ANDROID_LOG_INFO, TAG, "%s", buf);
    }
}

static int64_t NowUs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

/* Ask the Java activity for the Surface the video should be rendered to. */
static ANativeWindow *AcquireWindow(void) {
    JNIEnv *env = SDL_AndroidGetJNIEnv();
    jobject activity = SDL_AndroidGetActivity();
    if (env == NULL || activity == NULL) {
        Log(SS4S_LogLevelError, "No JNI environment or activity available");
        return NULL;
    }
    ANativeWindow *window = NULL;
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID method = (*env)->GetStaticMethodID(env, cls, "getVideoSurface", "()Landroid/view/Surface;");
    if (method == NULL) {
        (*env)->ExceptionClear(env);
        Log(SS4S_LogLevelError, "getVideoSurface() not found on the activity class");
    } else {
        jobject surface = (*env)->CallStaticObjectMethod(env, cls, method);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            surface = NULL;
        }
        if (surface != NULL) {
            window = ANativeWindow_fromSurface(env, surface);
            (*env)->DeleteLocalRef(env, surface);
        }
    }
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
    return window;
}

static const char *MimeFor(SS4S_VideoCodec codec) {
    switch (codec) {
        case SS4S_VIDEO_H264:
            return "video/avc";
        case SS4S_VIDEO_H265:
            return "video/hevc";
        case SS4S_VIDEO_AV1:
            return "video/av01";
        case SS4S_VIDEO_VP9:
            return "video/x-vnd.on2.vp9";
        default:
            return NULL;
    }
}

static void *OutputThread(void *arg) {
    SS4S_VideoInstance *inst = arg;
    AMediaCodecBufferInfo info;
    while (atomic_load(&inst->running)) {
        ssize_t index = AMediaCodec_dequeueOutputBuffer(inst->codec, &info, OUTPUT_POLL_TIMEOUT_US);
        if (index >= 0) {
            /* render=true shows the frame immediately: lowest latency, no pacing. */
            AMediaCodec_releaseOutputBuffer(inst->codec, (size_t) index, true);
            atomic_fetch_add(&inst->rendered, 1);
        } else if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *format = AMediaCodec_getOutputFormat(inst->codec);
            if (format != NULL) {
                Log(SS4S_LogLevelInfo, "Output format changed: %s", AMediaFormat_toString(format));
                AMediaFormat_delete(format);
            }
        }
        /* INFO_TRY_AGAIN_LATER / OUTPUT_BUFFERS_CHANGED: just loop. */
    }
    return NULL;
}

static bool GetCapabilities(SS4S_VideoCapabilities *capabilities) {
    /* H.264 and HEVC are mandatory on every Shield. AV1 and HDR are left out until measured. */
    capabilities->codecs = SS4S_VIDEO_H264 | SS4S_VIDEO_H265;
    capabilities->transform = SS4S_VIDEO_CAP_TRANSFORM_UI_COMPOSITING;
    capabilities->maxWidth = 3840;
    capabilities->maxHeight = 2160;
    capabilities->maxFps = 60;
    capabilities->fullColorRange = false;
    capabilities->hdr = false;
    capabilities->colorSpace = SS4S_VIDEO_CAP_COLORSPACE_BT709;
    return true;
}

static SS4S_VideoOpenResult Open(const SS4S_VideoInfo *info, const SS4S_VideoExtraInfo *extraInfo,
                                 SS4S_VideoInstance **instance, SS4S_PlayerContext *context) {
    (void) extraInfo;
    (void) context;
    const char *mime = MimeFor(info->codec);
    if (mime == NULL) {
        return SS4S_VIDEO_OPEN_UNSUPPORTED_CODEC;
    }
    SS4S_VideoInstance *inst = calloc(1, sizeof(SS4S_VideoInstance));
    if (inst == NULL) {
        return SS4S_VIDEO_OPEN_ERROR;
    }
    inst->window = AcquireWindow();
    if (inst->window == NULL) {
        free(inst);
        return SS4S_VIDEO_OPEN_ERROR;
    }
    /* WHICH DECODER. Normally the platform's default (the hardware decoder). For experiments the
     * system property debug.eclipse.decoder picks another one:
     *   adb shell setprop debug.eclipse.decoder sw        -> Google's software HEVC decoder
     *   adb shell setprop debug.eclipse.decoder <name>    -> any decoder by its exact name
     *   adb shell setprop debug.eclipse.decoder default   -> back to the normal choice */
    inst->codec = NULL;
    {
        char forced[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.eclipse.decoder", forced);
        const char *want = NULL;
        if (strcmp(forced, "sw") == 0) {
            want = info->codec == SS4S_VIDEO_H265 ? "c2.android.hevc.decoder" : "c2.android.avc.decoder";
        } else if (forced[0] != '\0' && strcmp(forced, "default") != 0) {
            want = forced;
        }
        if (want != NULL) {
            inst->codec = AMediaCodec_createCodecByName(want);
            Log(inst->codec != NULL ? SS4S_LogLevelInfo : SS4S_LogLevelWarn, "Decoder %s: %s", want,
                inst->codec != NULL ? "created" : "not available, using the default");
        }
    }
    if (inst->codec == NULL) {
        inst->codec = AMediaCodec_createDecoderByType(mime);
    }
    if (inst->codec == NULL) {
        Log(SS4S_LogLevelError, "No decoder for %s", mime);
        ANativeWindow_release(inst->window);
        free(inst);
        return SS4S_VIDEO_OPEN_UNSUPPORTED_CODEC;
    }

    AMediaFormat *format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, info->width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, info->height);
    /* A big IDR (a 4K picture, or the spliced co-op composite) can exceed the decoder's default
     * input buffer, and a truncated access unit decodes as garbage. Ask for room: one byte per
     * pixel, between 1 and 8 MB. */
    {
        int64_t want = (int64_t) info->width * (int64_t) info->height;
        if (want < 1024 * 1024) want = 1024 * 1024;
        if (want > 8 * 1024 * 1024) want = 8 * 1024 * 1024;
        AMediaFormat_setInt32(format, "max-input-size", (int32_t) want);
    }
    /* Plain-string keys: the matching constants only exist in newer NDK headers. */
    AMediaFormat_setInt32(format, "low-latency", 1);
    AMediaFormat_setInt32(format, "priority", 0);
    if (info->frameRateDenominator > 0) {
        AMediaFormat_setInt32(format, "operating-rate", info->frameRateNumerator / info->frameRateDenominator);
    }

    media_status_t status = AMediaCodec_configure(inst->codec, format, inst->window, NULL, 0);
    AMediaFormat_delete(format);
    if (status != AMEDIA_OK) {
        Log(SS4S_LogLevelError, "configure failed: %d", (int) status);
        goto fail;
    }
    status = AMediaCodec_start(inst->codec);
    if (status != AMEDIA_OK) {
        Log(SS4S_LogLevelError, "start failed: %d", (int) status);
        goto fail;
    }
    atomic_store(&inst->running, true);
    if (pthread_create(&inst->outputThread, NULL, OutputThread, inst) != 0) {
        atomic_store(&inst->running, false);
        AMediaCodec_stop(inst->codec);
        goto fail;
    }
    inst->outputThreadStarted = true;
    {
        char *name = NULL;
        if (AMediaCodec_getName(inst->codec, &name) == AMEDIA_OK && name != NULL) {
            Log(SS4S_LogLevelInfo, "Opened %s %dx%d with decoder %s", SS4S_VideoCodecName(info->codec), info->width,
                info->height, name);
            AMediaCodec_releaseName(inst->codec, name);
        } else {
            Log(SS4S_LogLevelInfo, "Opened %s %dx%d", SS4S_VideoCodecName(info->codec), info->width, info->height);
        }
    }
    *instance = inst;
    return SS4S_VIDEO_OPEN_OK;

fail:
    AMediaCodec_delete(inst->codec);
    ANativeWindow_release(inst->window);
    free(inst);
    return SS4S_VIDEO_OPEN_ERROR;
}

static bool AppendFrame(SS4S_VideoInstance *inst, const unsigned char *data, size_t size) {
    if (inst->frameSize + size > inst->frameCapacity) {
        size_t newCapacity = inst->frameCapacity ? inst->frameCapacity : 256 * 1024;
        while (newCapacity < inst->frameSize + size) {
            newCapacity *= 2;
        }
        unsigned char *grown = realloc(inst->frame, newCapacity);
        if (grown == NULL) {
            return false;
        }
        inst->frame = grown;
        inst->frameCapacity = newCapacity;
    }
    memcpy(inst->frame + inst->frameSize, data, size);
    inst->frameSize += size;
    return true;
}

/* First few access units: one line per NAL unit with its type, size and leading bytes. Enough to
 * read the stream's structure (parameter sets, slices, tiles) from a log. */
static void DumpNals(uint64_t number, const unsigned char *data, size_t size) {
    size_t i = 0;
    while (i + 3 < size) {
        if (!(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)) {
            i++;
            continue;
        }
        size_t start = i + 3;
        size_t end = start;
        while (end + 2 < size && !(data[end] == 0 && data[end + 1] == 0 && (data[end + 2] == 1 || data[end + 2] == 0))) {
            end++;
        }
        if (end + 2 >= size) {
            end = size;
        }
        int type = (data[start] >> 1) & 0x3f;
        size_t show = (type >= 32 && type <= 34) ? 40 : 12;
        if (show > end - start) {
            show = end - start;
        }
        char hex[96];
        size_t pos = 0;
        for (size_t k = 0; k < show && pos + 3 < sizeof(hex); k++) {
            pos += (size_t) snprintf(hex + pos, sizeof(hex) - pos, "%02x", data[start + k]);
        }
        Log(SS4S_LogLevelInfo, "AU%llu nal type=%d len=%zu %s", (unsigned long long) number, type, end - start, hex);
        i = end;
    }
}

static SS4S_VideoFeedResult Feed(SS4S_VideoInstance *inst, const unsigned char *data, size_t size,
                                 SS4S_VideoFeedFlags flags) {
    if (flags & SS4S_VIDEO_FEED_DATA_FRAME_START) {
        inst->frameSize = 0;
    }
    if (!AppendFrame(inst, data, size)) {
        inst->frameSize = 0;
        return SS4S_VIDEO_FEED_ERROR;
    }
    bool chunked = (flags & SS4S_VIDEO_FEED_DATA_FRAME_START) && !(flags & SS4S_VIDEO_FEED_DATA_FRAME_END);
    bool middle = (flags & (SS4S_VIDEO_FEED_DATA_FRAME_START | SS4S_VIDEO_FEED_DATA_FRAME_END)) == 0 &&
                  flags != SS4S_VIDEO_FEED_DATA_NONE;
    if (chunked || middle) {
        return SS4S_VIDEO_FEED_OK; /* wait for FRAME_END */
    }

    if (inst->frameSize > inst->largestFrame) {
        inst->largestFrame = inst->frameSize;
    }
    if (inst->fed < 3) {
        DumpNals(inst->fed, inst->frame, inst->frameSize);
    }
    ssize_t index = AMediaCodec_dequeueInputBuffer(inst->codec, FEED_INPUT_TIMEOUT_US);
    if (index < 0) {
        /* Decoder is behind. Drop this frame and ask the host for a clean picture. */
        inst->droppedBusy++;
        inst->frameSize = 0;
        return SS4S_VIDEO_FEED_REQUEST_KEYFRAME;
    }
    size_t capacity = 0;
    uint8_t *buffer = AMediaCodec_getInputBuffer(inst->codec, (size_t) index, &capacity);
    if (capacity > 0) {
        inst->inputCapacitySeen = capacity;
    }
    if (buffer == NULL || capacity < inst->frameSize) {
        if (inst->droppedOversize++ < 5) {
            Log(SS4S_LogLevelError, "Access unit of %zu bytes does not fit the decoder input buffer (%zu bytes)",
                inst->frameSize, capacity);
        }
        AMediaCodec_queueInputBuffer(inst->codec, (size_t) index, 0, 0, (uint64_t) NowUs(), 0);
        inst->frameSize = 0;
        return SS4S_VIDEO_FEED_REQUEST_KEYFRAME;
    }
    memcpy(buffer, inst->frame, inst->frameSize);
    media_status_t status = AMediaCodec_queueInputBuffer(inst->codec, (size_t) index, 0, inst->frameSize,
                                                         (uint64_t) NowUs(), 0);
    inst->frameSize = 0;
    inst->fed++;
    if (inst->fed % 300 == 0) {
        Log(SS4S_LogLevelInfo,
            "stats: fed=%llu rendered=%llu droppedBusy=%llu droppedOversize=%llu largestAU=%llu inputBuf=%zu",
            (unsigned long long) inst->fed, (unsigned long long) atomic_load(&inst->rendered),
            (unsigned long long) inst->droppedBusy, (unsigned long long) inst->droppedOversize,
            (unsigned long long) inst->largestFrame, inst->inputCapacitySeen);
    }
    if (status != AMEDIA_OK) {
        Log(SS4S_LogLevelError, "queueInputBuffer failed: %d", (int) status);
    }
    return status == AMEDIA_OK ? SS4S_VIDEO_FEED_OK : SS4S_VIDEO_FEED_ERROR;
}

static bool SizeChanged(SS4S_VideoInstance *inst, int width, int height) {
    (void) inst;
    (void) width;
    (void) height;
    return true; /* The codec follows in-band SPS changes; the Surface scales the picture. */
}

static bool SetHDRInfo(SS4S_VideoInstance *inst, const SS4S_VideoHDRInfo *info) {
    (void) inst;
    (void) info;
    return false;
}

static bool SetDisplayArea(SS4S_VideoInstance *inst, const SS4S_VideoRect *src, const SS4S_VideoRect *dst) {
    (void) inst;
    (void) src;
    (void) dst;
    return false;
}

static void Close(SS4S_VideoInstance *inst) {
    if (inst == NULL) {
        return;
    }
    atomic_store(&inst->running, false);
    if (inst->outputThreadStarted) {
        pthread_join(inst->outputThread, NULL);
    }
    AMediaCodec_stop(inst->codec);
    AMediaCodec_delete(inst->codec);
    ANativeWindow_release(inst->window);
    free(inst->frame);
    free(inst);
}

static SS4S_PlayerContext *PlayerCreate(SS4S_Player *player) {
    (void) player;
    return calloc(1, sizeof(SS4S_PlayerContext));
}

static void PlayerDestroy(SS4S_PlayerContext *context) {
    free(context);
}

static const SS4S_PlayerDriver PlayerDriver = {
        .Create = PlayerCreate,
        .Destroy = PlayerDestroy,
};

static const SS4S_VideoDriver VideoDriver = {
        .GetCapabilities = GetCapabilities,
        .Open = Open,
        .Feed = Feed,
        .SizeChanged = SizeChanged,
        .SetHDRInfo = SetHDRInfo,
        .SetDisplayArea = SetDisplayArea,
        .Close = Close,
};

SS4S_EXPORTED bool SS4S_ModuleOpen_MEDIACODEC(SS4S_Module *module, const SS4S_LibraryContext *context) {
    LibContext = context;
    module->Name = "mediacodec";
    module->PlayerDriver = &PlayerDriver;
    module->VideoDriver = &VideoDriver;
    return true;
}

SS4S_EXPORTED SS4S_ModuleCheckFlag SS4S_ModuleCheck_MEDIACODEC(SS4S_ModuleCheckFlag flags) {
    return flags & SS4S_MODULE_CHECK_VIDEO;
}
