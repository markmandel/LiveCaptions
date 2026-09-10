/* asrproc.c
 * This file implements asr_thread which takes in audio, passes it to aprilasr,
 * and passes the output to line_generator to update the GtkLabel.
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
#include <errno.h>
#include <signal.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/mman.h>
#include <glib.h>
#include <time.h>

#include <stdbool.h>
#include <april_api.h>

#include "asrproc.h"
#include "line-gen.h"
#include "livecaptions-window.h"
#include "livecaptions-application.h"
#include "history.h"
#include "diarize.h"
#include "common.h"

// Shortest gap between two diarizer-driven flushes. A speaker boundary that
// jitters must not be able to chop the transcript into fragments.
#define SPEAKER_FLUSH_MIN_INTERVAL_MS 600

struct asr_thread_i {
    volatile size_t sound_counter;
    size_t silence_counter;

    GThread * thread_id;

    struct line_generator line;

    GMutex text_mutex;
    char text_buffer[32768];

    AprilASRModel model;
    AprilASRSession session;

    LiveCaptionsWindow *window;

    size_t layout_counter;

    volatile bool text_stream_active;
    volatile bool pause;

    volatile time_t last_silence_time;

    volatile bool ending;

    bool errored;

    // Counts every sample handed to asr_thread_enqueue_audio, including the
    // silent ones that never reach the ASR engine. AprilToken.time_ms cannot be
    // used for this: it only advances on audio that was actually fed, and gets
    // warped when april-asr speeds audio up to keep pace.
    _Atomic uint64_t capture_samples;

    diarize_state diarize;

    // Capture-clock position of the last turn boundary. Recognition results
    // arrive well after the audio that produced them, so an utterance is dated
    // from the previous boundary rather than from when its callback fired.
    // Any silence caught in that window is harmless: it contributes no speech
    // to the tally.
    uint64_t last_boundary_ms;

    // Who the utterance being recognised belongs to, fixed when it started
    int32_t utterance_speaker;
    bool utterance_open;

    uint64_t last_speaker_flush_ms;
};


static gboolean main_thread_update_label(void *userdata);

static GSettings *settings = NULL;

static void *run_asr_thread(void *userdata){
    asr_thread data = (asr_thread)userdata;

    while(!data->ending){
        sleep(1);

        if(data->last_silence_time == 0) continue;

        time_t current_time = time(NULL);

        if(difftime(current_time, data->last_silence_time) >= g_settings_get_double(settings, "silence-clear-timeout")) {
            g_mutex_lock(&data->text_mutex);
            data->last_silence_time = 0;
            for(int i=1; i<AC_LINE_COUNT; i++) line_generator_break(&data->line);
            g_mutex_unlock(&data->text_mutex);
            g_idle_add(main_thread_update_label, data);
        }
    }

    //sleep(40);

    //if(data->sound_counter < 512) {
    //    gtk_label_set_text(data->label, "[No audio is being received. If you are playing audio,\nmost likely the audio recording isn't working]");
    //}

    return NULL;
}

// Current position of the capture clock, in milliseconds
static uint64_t asr_capture_ms(asr_thread data) {
    if(data->model == NULL) return 0;

    uint64_t samples = data->capture_samples;
    return (samples * 1000ull) / (uint64_t)aam_get_sample_rate(data->model);
}

// Called from the diarizer's worker thread when a different speaker takes the
// floor. Flushing forces april-asr to finalize immediately, which is what makes
// transcript boundaries line up with speaker boundaries instead of with the
// engine's own 2.2s silence heuristic.
static void on_speaker_changed(void *userdata, int32_t speaker_id) {
    asr_thread data = userdata;

    (void)speaker_id;

    if((data->window == NULL) || data->pause) return;
    if(data->session == NULL) return;

    uint64_t now_ms = asr_capture_ms(data);

    if((data->last_speaker_flush_ms != 0)
        && ((now_ms - data->last_speaker_flush_ms) < SPEAKER_FLUSH_MIN_INTERVAL_MS)) return;

    data->last_speaker_flush_ms = now_ms;

    aas_flush(data->session);
}

// Puts the speaker's name on the line being written. Must be called with
// text_mutex held, since it writes into the line generator.
//
// The name is only emitted when the speaker actually changes; line_generator
// takes care of that. An unknown speaker is left alone rather than reset,
// because a momentary gap in attribution should not break the line and then
// re-announce the same person on the other side of it.
static void apply_speaker_label(asr_thread data, int32_t speaker) {
    if(!g_settings_get_boolean(settings, "diarization")) return;
    if(speaker == HISTORY_SPEAKER_UNKNOWN) return;

    // Registering first means an unnamed speaker still gets its "Speaker N"
    // placeholder, and a renamed one picks up the name the user chose
    history_register_speaker(speaker);

    char label[LINE_SPEAKER_NAME_MAX];
    if(!history_speaker_label(get_history_session(0), speaker, label, sizeof(label))) return;

    line_generator_set_speaker(&data->line, speaker, label);
}

static gboolean main_thread_update_label(void *userdata){
    asr_thread data = userdata;

    if((data->window == NULL) || (data->pause)) return G_SOURCE_REMOVE;

    g_mutex_lock(&data->text_mutex);
    line_generator_set_text(&data->line, data->window->label);
    
    if(data->text_stream_active) {
        LiveCaptionsApplication *application = LIVECAPTIONS_APPLICATION(gtk_window_get_application(GTK_WINDOW(data->window)));
        livecaptions_application_stream_text(application, line_generator_get_plaintext(&data->line));
    }

    g_mutex_unlock(&data->text_mutex);

    return G_SOURCE_REMOVE;
}

static void april_result_handler(void* userdata, AprilResultType result, size_t count, const AprilToken* tokens) {
    asr_thread data = userdata;
    if((data->window == NULL) || (data->pause)) return;

    switch(result) {
        case APRIL_RESULT_RECOGNITION_PARTIAL:
        case APRIL_RESULT_RECOGNITION_FINAL:
        {
            g_mutex_lock(&data->text_mutex);
            data->last_silence_time = 0;

            if((data->layout_counter != data->window->font_layout_counter) || (data->line.layout == NULL)) {
                if(data->line.layout != NULL) g_object_unref(data->line.layout);

                data->line.layout = pango_layout_copy(data->window->font_layout);
                data->line.max_text_width = data->window->max_text_width;

                data->layout_counter = data->window->font_layout_counter;
            }

            if(!data->utterance_open) {
                data->utterance_open = true;
                data->utterance_speaker = HISTORY_SPEAKER_UNKNOWN;
            }

            // The diarizer needs about a second of speech before it can name
            // anyone, so an utterance often starts before the answer exists.
            // Keep asking until it does, then label the line already being
            // written rather than waiting for the next one. Attribution is
            // fixed once found, and reused for the history entry so the screen
            // and the transcript cannot disagree.
            //
            // A speaker changing mid-utterance is handled separately: the
            // diarizer forces a flush, ending this utterance and starting one.
            if(data->utterance_speaker == HISTORY_SPEAKER_UNKNOWN) {
                int32_t speaker = diarize_speaker_for_range(data->diarize,
                                                            data->last_boundary_ms,
                                                            asr_capture_ms(data));

                if(speaker == HISTORY_SPEAKER_UNKNOWN) {
                    speaker = diarize_current_speaker(data->diarize);
                }

                if(speaker != HISTORY_SPEAKER_UNKNOWN) {
                    data->utterance_speaker = speaker;
                    apply_speaker_label(data, speaker);
                }
            }

            line_generator_update(&data->line, count, tokens);
            if(result == APRIL_RESULT_RECOGNITION_FINAL) {
                line_generator_finalize(&data->line);
                commit_tokens_to_current_history(tokens, count, data->utterance_speaker);

                data->last_boundary_ms = asr_capture_ms(data);
                data->utterance_open = false;
            }

            g_mutex_unlock(&data->text_mutex);
            g_idle_add(main_thread_update_label, data);
            break;
        }

        case APRIL_RESULT_ERROR_CANT_KEEP_UP: {
            livecaptions_window_warn_slow(data->window);
            break;
        }

        case APRIL_RESULT_SILENCE: {
            g_mutex_lock(&data->text_mutex);
            data->last_silence_time = time(NULL);
            data->last_boundary_ms = asr_capture_ms(data);
            data->utterance_open = false;

            line_generator_break(&data->line);
            save_silence_to_history();

            g_mutex_unlock(&data->text_mutex);
            g_idle_add(main_thread_update_label, data);
            break;
        }
    }
}

void asr_thread_enqueue_audio(asr_thread thread, short *data, size_t num_shorts) {
    if((thread->window == NULL) || thread->pause) return;
    if((thread->session == NULL) || (thread->model == NULL)) return;

    // The diarizer sees the audio before the silence gate below, so that its
    // clock stays continuous and matches capture_samples sample for sample
    thread->capture_samples += num_shorts;
    diarize_push(thread->diarize, data, num_shorts);

    bool found_nonzero = false;
    for(size_t i=0; i<num_shorts; i++){
        if((data[i] > 16) || (data[i] < -16)){
            found_nonzero = true;
            break;
        }
    }

    thread->silence_counter = found_nonzero ? 0 : (thread->silence_counter + num_shorts);

    if(thread->silence_counter >= 24000){
        thread->silence_counter = 24000;
        return aas_flush(thread->session);
    }
    
    thread->sound_counter += num_shorts;
    aas_feed_pcm16(thread->session, data, num_shorts); // TODO?
}

gpointer asr_thread_get_model(asr_thread thread) {
    return thread->model;
}

gpointer asr_thread_get_session(asr_thread thread) {
    return thread->session;
}

void asr_thread_pause(asr_thread thread, bool pause) {
    thread->pause = pause;
}

void asr_thread_set_text_stream_active(asr_thread thread, bool active) {
    thread->text_stream_active = active;
}

int asr_thread_samplerate(asr_thread thread) {
    return aam_get_sample_rate(thread->model);
}

asr_thread create_asr_thread(const char *model_path){
    asr_thread data = calloc(1, sizeof(struct asr_thread_i));

    if(settings == NULL) settings = g_settings_new("net.sapples.LiveCaptions");

    line_generator_init(&data->line);

    if(!asr_thread_update_model(data, model_path)){
        char *model_default = GET_MODEL_PATH();
        if(!asr_thread_update_model(data, model_default)) {
            return NULL;
        }

        GSettings *settings = g_settings_new("net.sapples.LiveCaptions");
        g_settings_set_string(settings, "active-model", model_default);
        g_object_unref(G_OBJECT(settings));
    }

    g_mutex_init(&data->text_mutex);

    // The model is loaded by this point, so its sample rate is known. Capture
    // runs at that same rate, so the diarizer and the capture clock agree.
    data->diarize = diarize_create(aam_get_sample_rate(data->model),
                                   GET_VAD_MODEL_PATH(),
                                   GET_SPEAKER_MODEL_PATH());
    if(data->diarize != NULL) {
        diarize_set_change_handler(data->diarize, on_speaker_changed, data);
        diarize_set_match_threshold(data->diarize,
            (float)g_settings_get_double(settings, "speaker-similarity-threshold"));
        diarize_set_max_speakers(data->diarize,
            g_settings_get_int(settings, "max-speakers"));
        diarize_set_enabled(data->diarize, g_settings_get_boolean(settings, "diarization"));
    }

    data->thread_id = g_thread_new("lcap-audiothread", run_asr_thread, data);

    data->text_stream_active = false;
    data->utterance_speaker = HISTORY_SPEAKER_UNKNOWN;

    return data;
}

void asr_thread_set_diarization(asr_thread thread, bool enabled) {
    if((thread == NULL) || (thread->diarize == NULL)) return;

    diarize_set_enabled(thread->diarize, enabled);
}

void asr_thread_set_speaker_threshold(asr_thread thread, double threshold) {
    if((thread == NULL) || (thread->diarize == NULL)) return;

    diarize_set_match_threshold(thread->diarize, (float)threshold);
}

void asr_thread_set_max_speakers(asr_thread thread, int max_speakers) {
    if((thread == NULL) || (thread->diarize == NULL)) return;

    diarize_set_max_speakers(thread->diarize, max_speakers);
}

bool asr_thread_update_model(asr_thread data, const char *model_path) {
    // Freeing model frees token list, which may be being accessed during
    // line generation
    g_mutex_lock(&data->text_mutex);

    data->pause = true;

    AprilASRModel old_model = data->model;
    AprilASRSession old_session = data->session;

    data->model = NULL;
    data->session = NULL;

    if(old_session != NULL)
        aas_free(old_session);

    if(old_model != NULL)
        aam_free(old_model);


    AprilConfig config = {
        .handler = april_result_handler,
        .flags = APRIL_CONFIG_FLAG_ASYNC_RT_BIT,
        .userdata = data
    };

    AprilASRModel new_model = aam_create_model(model_path);
    if(new_model == NULL) {
        printf("Loading model %s failed!\n", model_path);
        data->errored = true;
        g_mutex_unlock(&data->text_mutex);
        return false;
    }

    {
        printf("\n-- Model metadata --\n");
        printf("Name: %s\n", aam_get_name(new_model));
        char *description = (char*)aam_get_description(new_model);
        for(int i=0; description[i]; i++){
            if((description[i] == ' ') && (description[i+1] == 'D') && (description[i+2] == 'i')){
                description[i] = 0;
                break;
            }
        }
        printf("Description: %s\n", description);
        printf("Language: %s\n", aam_get_language(new_model));
        printf("-- --\n\n");
    }

    line_generator_set_language(&data->line, aam_get_language(new_model));

    AprilASRSession new_session = aas_create_session(new_model, config);
    if(new_session == NULL) {
        printf("Creating session %s failed!\n", model_path);
        data->errored = true;
        g_mutex_unlock(&data->text_mutex);
        return false;
    }

    data->model = new_model;
    data->session = new_session;

    data->errored = false;
    data->ending = false;
    data->pause = false;

    line_generator_finalize(&data->line);

    g_mutex_unlock(&data->text_mutex);

    return true;
}

bool asr_thread_is_errored(asr_thread thread) {
    return thread->errored;
}

void asr_thread_set_main_window(asr_thread thread, LiveCaptionsWindow *window) {
    thread->window = window;
}

void asr_thread_flush(asr_thread thread) {
    if(thread->session == NULL) return;
    aas_flush(thread->session);
}

void free_asr_thread(asr_thread thread) {
    thread->ending = true;

    // Stopped before the session is freed, since the diarizer's change handler
    // calls aas_flush on it
    diarize_free(thread->diarize);
    thread->diarize = NULL;

    g_mutex_lock(&thread->text_mutex);

    g_thread_join(thread->thread_id);

    if(thread->session != NULL)
        aas_free(thread->session);
    
    if(thread->model != NULL)
        aam_free(thread->model);

    g_thread_unref(thread->thread_id); // ?

    free(thread);
}
