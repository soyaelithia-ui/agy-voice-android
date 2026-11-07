/*
 * aplay - Native Android AAudio speaker playback shim for Antigravity CLI (agy)
 *
 * Implements the ALSA aplay interface expected by `agy` (/voice live and
 * `agy mic-serve`):
 *   aplay -q -t raw -f S16_LE -r 24000 -c 1 --buffer-time=60000 -
 *
 * Reads raw 16-bit signed little-endian PCM from stdin and plays it directly
 * through Android's low-latency AAudio subsystem (libaaudio.so).
 */

#include <aaudio/AAudio.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_aborted = 0;

static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
    g_aborted = 1;
}

static AAudioStream *open_output_stream(int32_t sample_rate, int32_t channels) {
    AAudioStreamBuilder *builder = NULL;
    aaudio_result_t res = AAudio_createStreamBuilder(&builder);
    if (res != AAUDIO_OK || !builder) {
        return NULL;
    }

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(builder, sample_rate);
    AAudioStreamBuilder_setChannelCount(builder, channels);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_MEDIA);
    AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_SPEECH);

    AAudioStream *stream = NULL;
    res = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);

    if (res != AAUDIO_OK || !stream) {
        fprintf(stderr, "aplay (AAudio): failed to open speaker stream: %s (%d)\n",
                AAudio_convertResultToText(res), res);
        return NULL;
    }

    /* Set a tight buffer size (~60ms) so barge-in stops quickly without underruns */
    int32_t burst = AAudioStream_getFramesPerBurst(stream);
    int32_t target_frames = (sample_rate * 60) / 1000;
    if (burst > 0 && target_frames < burst * 2) {
        target_frames = burst * 2;
    }
    int32_t cap = AAudioStream_getBufferCapacityInFrames(stream);
    if (cap > 0 && target_frames > cap) {
        target_frames = cap;
    }
    if (target_frames > 0) {
        AAudioStream_setBufferSizeInFrames(stream, target_frames);
    }

    return stream;
}

int main(int argc, char **argv) {
    int32_t sample_rate = 24000;
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
            printf("aplay: version 1.2.12 (Android AAudio bridge)\n");
            return 0;
        }
    }

    if (sample_rate <= 0) sample_rate = 24000;
    if (channels <= 0) channels = 1;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    AAudioStream *stream = open_output_stream(sample_rate, channels);
    if (!stream) {
        return 1;
    }

    const size_t bytes_per_frame = (size_t)channels * sizeof(int16_t);
    const size_t max_frames = 960; /* 40ms at 24kHz */
    const size_t buf_size = max_frames * bytes_per_frame;
    uint8_t *buffer = (uint8_t *)malloc(buf_size);
    if (!buffer) {
        AAudioStream_close(stream);
        return 1;
    }

    size_t leftover = 0;
    int stream_started = 0;
    struct pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;

    while (g_running) {
        int timeout_ms = stream_started ? 200 : -1;
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (pr == 0) {
            /* No stdin data for 200ms while stream is active: pause cleanly to avoid xruns */
            if (stream_started && stream) {
                int16_t silence[240];
                memset(silence, 0, sizeof(silence));
                AAudioStream_write(stream, silence, 240 / channels, 50000000LL);
                AAudioStream_requestPause(stream);
                AAudioStream_requestFlush(stream);
                stream_started = 0;
            }
            continue;
        }

        ssize_t n = read(STDIN_FILENO, buffer + leftover, buf_size - leftover);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) {
            /* EOF on stdin */
            break;
        }

        size_t total_bytes = leftover + (size_t)n;
        size_t playable_frames = total_bytes / bytes_per_frame;
        size_t playable_bytes = playable_frames * bytes_per_frame;

        if (playable_frames > 0) {
            if (!stream_started) {
                aaudio_result_t sres = AAudioStream_requestStart(stream);
                if (sres == AAUDIO_ERROR_DISCONNECTED) {
                    AAudioStream_close(stream);
                    stream = open_output_stream(sample_rate, channels);
                    if (!stream) break;
                    sres = AAudioStream_requestStart(stream);
                }
                if (sres != AAUDIO_OK) {
                    fprintf(stderr, "aplay (AAudio): failed to start stream: %s (%d)\n",
                            AAudio_convertResultToText(sres), sres);
                    break;
                }
                stream_started = 1;
            }

            int32_t written_total = 0;
            const uint8_t *frame_ptr = buffer;
            while (written_total < (int32_t)playable_frames && g_running) {
                aaudio_result_t w = AAudioStream_write(
                    stream,
                    frame_ptr + ((size_t)written_total * bytes_per_frame),
                    (int32_t)playable_frames - written_total,
                    500000000LL /* 500ms timeout */);

                if (w > 0) {
                    written_total += w;
                } else if (w == AAUDIO_ERROR_DISCONNECTED) {
                    AAudioStream_close(stream);
                    stream = open_output_stream(sample_rate, channels);
                    if (!stream || AAudioStream_requestStart(stream) != AAUDIO_OK) {
                        g_running = 0;
                        break;
                    }
                    stream_started = 1;
                } else if (w < 0) {
                    fprintf(stderr, "aplay (AAudio): write error: %s (%d)\n",
                            AAudio_convertResultToText(w), w);
                    g_running = 0;
                    break;
                }
            }
        }

        leftover = total_bytes - playable_bytes;
        if (leftover > 0) {
            memmove(buffer, buffer + playable_bytes, leftover);
        }
    }

    free(buffer);

    if (stream) {
        if (g_aborted) {
            /* Immediate cutoff on SIGINT/SIGTERM (barge-in interruption) */
            AAudioStream_requestPause(stream);
            AAudioStream_requestFlush(stream);
        } else if (stream_started) {
            /* Normal EOF: let final ~60ms hardware buffer finish playing */
            struct timespec ts = {0, 60000000L};
            nanosleep(&ts, NULL);
            AAudioStream_requestStop(stream);
        }
        AAudioStream_close(stream);
    }

    return 0;
}
