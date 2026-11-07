/*
 * arecord - Native Android AAudio microphone recorder shim for Antigravity CLI (agy)
 *
 * Implements the ALSA arecord interface expected by `agy` (/voice, /voice live,
 * /voice dictate, and `agy mic-serve`):
 *   arecord -f S16_LE -r 16000 -c 1 -t raw -q -
 *
 * Streams raw 16-bit signed little-endian PCM directly from Android's low-latency
 * AAudio subsystem (libaaudio.so) to stdout.
 */

#include <aaudio/AAudio.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_running = 1;

static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static AAudioStream *open_input_stream(int32_t sample_rate, int32_t channels) {
    AAudioStreamBuilder *builder = NULL;
    aaudio_result_t res = AAudio_createStreamBuilder(&builder);
    if (res != AAUDIO_OK || !builder) {
        return NULL;
    }

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setSampleRate(builder, sample_rate);
    AAudioStreamBuilder_setChannelCount(builder, channels);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);

    const char *preset_env = getenv("AGY_AAUDIO_PRESET");
    if (preset_env && strcmp(preset_env, "comm") == 0) {
        AAudioStreamBuilder_setInputPreset(builder, AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION);
    } else {
        AAudioStreamBuilder_setInputPreset(builder, AAUDIO_INPUT_PRESET_VOICE_RECOGNITION);
    }

    AAudioStream *stream = NULL;
    res = AAudioStreamBuilder_openStream(builder, &stream);
    if (res != AAUDIO_OK) {
        /*
         * On Termux, if android.permission.RECORD_AUDIO has not been activated
         * for the shared com.termux UID yet, triggering `termux-microphone-record -i`
         * prompts/activates the permission via Termux:API.
         */
        int ret = system("/data/data/com.termux/files/usr/bin/termux-microphone-record -i >/dev/null 2>&1");
        (void)ret;
        res = AAudioStreamBuilder_openStream(builder, &stream);
    }

    AAudioStreamBuilder_delete(builder);
    if (res != AAUDIO_OK) {
        fprintf(stderr, "arecord (AAudio): failed to open microphone stream: %s (%d)\n",
                AAudio_convertResultToText(res), res);
        return NULL;
    }
    return stream;
}

int main(int argc, char **argv) {
    int32_t sample_rate = 16000;
    int32_t channels = 1;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--rate") == 0) && i + 1 < argc) {
            sample_rate = atoi(argv[++i]);
        } else if (strncmp(argv[i], "--rate=", 7) == 0) {
            sample_rate = atoi(argv[i] + 7);
        } else if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--channels") == 0) && i + 1 < argc) {
            channels = atoi(argv[++i]);
        } else if (strncmp(argv[i], "--channels=", 11) == 0) {
            channels = atoi(argv[i] + 11);
        } else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            printf("arecord: version 1.2.12 (Android AAudio bridge)\n");
            return 0;
        }
    }

    if (sample_rate <= 0) sample_rate = 16000;
    if (channels <= 0) channels = 1;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    AAudioStream *stream = open_input_stream(sample_rate, channels);
    if (!stream) {
        return 1;
    }

    aaudio_result_t res = AAudioStream_requestStart(stream);
    if (res != AAUDIO_OK) {
        fprintf(stderr, "arecord (AAudio): failed to start stream: %s (%d)\n",
                AAudio_convertResultToText(res), res);
        AAudioStream_close(stream);
        return 1;
    }

    /* 20ms chunks at 16kHz = 320 frames */
    const int32_t frames_per_chunk = sample_rate / 50 > 0 ? sample_rate / 50 : 320;
    const size_t bytes_per_frame = (size_t)channels * sizeof(int16_t);
    int16_t *buffer = (int16_t *)malloc((size_t)frames_per_chunk * bytes_per_frame);
    if (!buffer) {
        AAudioStream_close(stream);
        return 1;
    }

    while (g_running) {
        aaudio_result_t frames_read = AAudioStream_read(
            stream, buffer, frames_per_chunk, 200000000LL /* 200ms timeout */);

        if (frames_read > 0) {
            size_t total_bytes = (size_t)frames_read * bytes_per_frame;
            size_t offset = 0;
            const uint8_t *ptr = (const uint8_t *)buffer;

            while (offset < total_bytes && g_running) {
                ssize_t w = write(STDOUT_FILENO, ptr + offset, total_bytes - offset);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    g_running = 0;
                    break;
                }
                offset += (size_t)w;
            }
        } else if (frames_read == AAUDIO_ERROR_TIMEOUT) {
            continue;
        } else if (frames_read == AAUDIO_ERROR_DISCONNECTED) {
            /* Audio routing changed (e.g. Bluetooth headset connected/disconnected) */
            AAudioStream_close(stream);
            stream = open_input_stream(sample_rate, channels);
            if (!stream || AAudioStream_requestStart(stream) != AAUDIO_OK) {
                break;
            }
        } else if (frames_read < 0) {
            fprintf(stderr, "arecord (AAudio): read error: %s (%d)\n",
                    AAudio_convertResultToText(frames_read), frames_read);
            break;
        }
    }

    free(buffer);
    if (stream) {
        AAudioStream_requestStop(stream);
        AAudioStream_close(stream);
    }
    return 0;
}
