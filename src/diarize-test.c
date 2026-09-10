/* diarize-test.c
 * A small harness that runs a WAV file through the diarizer and prints the
 * speaker turns it found. Lets thresholds be tuned and regressions caught
 * without launching the GUI.
 *
 * Usage: diarize-test <file.wav> [--realtime] [--threshold N]
 *
 * Model paths come from VAD_MODEL_PATH and SPEAKER_MODEL_PATH, the same
 * variables the app itself honours.
 *
 * The file must be 16-bit mono PCM at the model's sample rate (16 kHz). To
 * convert something else:
 *   ffmpeg -i input.mp4 -ac 1 -ar 16000 -c:a pcm_s16le output.wav
 *
 * Copyright 2022 abb128
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include "diarize.h"
#include "common.h"

#define MAX_REPORTED_SEGMENTS 512

struct wav {
    short *samples;
    size_t count;
    int sample_rate;
    int channels;
};

static bool read_u32(FILE *f, uint32_t *out) {
    return fread(out, sizeof(*out), 1, f) == 1;
}

static bool read_u16(FILE *f, uint16_t *out) {
    return fread(out, sizeof(*out), 1, f) == 1;
}

// Minimal RIFF/WAVE reader: walks the chunk list looking for "fmt " and "data",
// which is enough for anything ffmpeg or sox produces.
static bool load_wav(const char *path, struct wav *out) {
    FILE *f = fopen(path, "rb");
    if(f == NULL) {
        fprintf(stderr, "Could not open %s\n", path);
        return false;
    }

    char riff[4], wave[4];
    uint32_t riff_size;

    if((fread(riff, 4, 1, f) != 1) || !read_u32(f, &riff_size) || (fread(wave, 4, 1, f) != 1)) {
        fprintf(stderr, "%s is too short to be a WAV file\n", path);
        fclose(f);
        return false;
    }

    if((memcmp(riff, "RIFF", 4) != 0) || (memcmp(wave, "WAVE", 4) != 0)) {
        fprintf(stderr, "%s is not a RIFF/WAVE file\n", path);
        fclose(f);
        return false;
    }

    bool have_fmt = false;
    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t sample_rate = 0;

    while(true) {
        char chunk_id[4];
        uint32_t chunk_size;

        if((fread(chunk_id, 4, 1, f) != 1) || !read_u32(f, &chunk_size)) break;

        if(memcmp(chunk_id, "fmt ", 4) == 0) {
            uint16_t block_align;
            uint32_t byte_rate;

            if(!read_u16(f, &format) || !read_u16(f, &channels)
                || !read_u32(f, &sample_rate) || !read_u32(f, &byte_rate)
                || !read_u16(f, &block_align) || !read_u16(f, &bits)) {
                fprintf(stderr, "%s has a truncated fmt chunk\n", path);
                fclose(f);
                return false;
            }

            have_fmt = true;

            // Skip any extension bytes beyond the 16 just read
            if(chunk_size > 16) fseek(f, (long)(chunk_size - 16), SEEK_CUR);
        } else if(memcmp(chunk_id, "data", 4) == 0) {
            if(!have_fmt) {
                fprintf(stderr, "%s has a data chunk before its fmt chunk\n", path);
                fclose(f);
                return false;
            }

            if((format != 1) || (bits != 16)) {
                fprintf(stderr, "%s must be 16-bit PCM (found format %u, %u-bit)\n",
                        path, format, bits);
                fclose(f);
                return false;
            }

            out->count = chunk_size / sizeof(short);
            out->samples = malloc(chunk_size);

            if(out->samples == NULL) {
                fprintf(stderr, "Out of memory reading %s\n", path);
                fclose(f);
                return false;
            }

            if(fread(out->samples, 1, chunk_size, f) != chunk_size) {
                fprintf(stderr, "%s has a truncated data chunk\n", path);
                free(out->samples);
                fclose(f);
                return false;
            }

            out->sample_rate = (int)sample_rate;
            out->channels = (int)channels;

            fclose(f);
            return true;
        } else {
            // Chunks are word-aligned, so an odd size carries a pad byte
            fseek(f, (long)(chunk_size + (chunk_size & 1)), SEEK_CUR);
        }
    }

    fprintf(stderr, "%s has no data chunk\n", path);
    fclose(f);
    return false;
}

// Mixes an interleaved multi-channel buffer down to mono in place, matching what
// the capture layer would have delivered
static void downmix_to_mono(struct wav *w) {
    if(w->channels <= 1) return;

    size_t frames = w->count / (size_t)w->channels;

    for(size_t i=0; i<frames; i++){
        int sum = 0;
        for(int c=0; c<w->channels; c++) sum += w->samples[i * (size_t)w->channels + c];
        w->samples[i] = (short)(sum / w->channels);
    }

    w->count = frames;
    w->channels = 1;

    printf("(down-mixed to mono)\n");
}

int main(int argc, char **argv) {
    if(argc < 2) {
        fprintf(stderr, "usage: %s <file.wav> [--realtime]\n", argv[0]);
        return 1;
    }

    bool realtime = false;
    bool probe = false;
    float threshold = DIARIZE_DEFAULT_THRESHOLD;

    for(int i=2; i<argc; i++){
        if(strcmp(argv[i], "--realtime") == 0) realtime = true;
        // Probing only means anything at real speed: pushed faster than that,
        // the queries ask about audio the worker has not reached yet and
        // everything comes back unattributed for reasons the app never hits
        else if(strcmp(argv[i], "--probe") == 0) { probe = true; realtime = true; }
        else if((strcmp(argv[i], "--threshold") == 0) && ((i + 1) < argc)) {
            threshold = (float)atof(argv[++i]);
        }
    }

    struct wav w = { 0 };
    if(!load_wav(argv[1], &w)) return 1;

    downmix_to_mono(&w);

    printf("%s: %zu samples, %d Hz, %.2f seconds\n",
           argv[1], w.count, w.sample_rate,
           (double)w.count / (double)w.sample_rate);

    diarize_state d = diarize_create(w.sample_rate,
                                     GET_VAD_MODEL_PATH(),
                                     GET_SPEAKER_MODEL_PATH());
    if(d == NULL) {
        fprintf(stderr, "Could not create the diarizer\n");
        free(w.samples);
        return 1;
    }

    if(!diarize_has_models(d)) {
        fprintf(stderr, "Models did not load. Set VAD_MODEL_PATH and SPEAKER_MODEL_PATH.\n");
        diarize_free(d);
        free(w.samples);
        return 1;
    }

    diarize_set_match_threshold(d, threshold);
    diarize_set_enabled(d, true);

    printf("threshold: %.2f\n", threshold);

    // Feed in fragments the same size the PulseAudio backend delivers, so that
    // buffering behaves the way it does in the real app
    size_t chunk = (size_t)(w.sample_rate / 20);
    gint64 started = g_get_monotonic_time();

    // Mirrors what the ASR thread does: ask who was speaking over the stretch
    // since the last question, while audio is still arriving. This is the path
    // that matters in the running application, and it is not covered by simply
    // reading the timeline once everything has been flushed.
    uint64_t last_probe_ms = 0;
    uint64_t pushed_ms = 0;

    if(probe) printf("\nlive attribution, as the ASR thread would see it:\n");

    for(size_t offset=0; offset<w.count; offset += chunk){
        size_t n = w.count - offset;
        if(n > chunk) n = chunk;

        diarize_push(d, &w.samples[offset], n);

        pushed_ms = ((uint64_t)(offset + n) * 1000ull) / (uint64_t)w.sample_rate;

        if(probe && ((pushed_ms - last_probe_ms) >= 1000)) {
            // Give the worker a moment to catch up, as it would in real time
            if(!realtime) g_usleep(30000);

            int32_t speaker = diarize_speaker_for_range(d, last_probe_ms, pushed_ms);

            char answer[32];
            if(speaker == DIARIZE_SPEAKER_UNKNOWN) g_strlcpy(answer, "unattributed", sizeof(answer));
            else g_snprintf(answer, sizeof(answer), "speaker %d", speaker + 1);

            printf("  [%6.2f - %6.2f] -> %s\n",
                   (double)last_probe_ms / 1000.0,
                   (double)pushed_ms / 1000.0,
                   answer);

            last_probe_ms = pushed_ms;
        }

        if(realtime) g_usleep(50000);
    }

    diarize_flush(d);

    gint64 elapsed_us = g_get_monotonic_time() - started;
    double audio_seconds = (double)w.count / (double)w.sample_rate;

    struct diarize_segment segments[MAX_REPORTED_SEGMENTS];
    size_t num_segments = diarize_get_segments(d, segments, MAX_REPORTED_SEGMENTS);

    printf("\n%zu segment(s):\n", num_segments);

    uint64_t total_speech_ms = 0;
    for(size_t i=0; i<num_segments; i++){
        const struct diarize_segment *s = &segments[i];

        total_speech_ms += (s->end_ms - s->start_ms);

        printf("  [%7.2f - %7.2f] %5.2fs  ",
               (double)s->start_ms / 1000.0,
               (double)s->end_ms / 1000.0,
               (double)(s->end_ms - s->start_ms) / 1000.0);

        if(s->speaker_id == DIARIZE_SPEAKER_UNKNOWN) printf("speaker unknown\n");
        else printf("speaker %d\n", s->speaker_id + 1);
    }

    printf("\nspeech: %.2fs of %.2fs (%.1f%%)\n",
           (double)total_speech_ms / 1000.0,
           audio_seconds,
           100.0 * (double)total_speech_ms / (audio_seconds * 1000.0));

    printf("dropped samples: %" G_GUINT64_FORMAT "\n", diarize_dropped_samples(d));

    if(!realtime) {
        printf("processed %.2fs of audio in %.3fs (%.0fx realtime)\n",
               audio_seconds,
               (double)elapsed_us / 1e6,
               audio_seconds / ((double)elapsed_us / 1e6));
    }

    diarize_free(d);
    free(w.samples);

    return 0;
}
