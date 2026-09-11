/* diarize.h
 * This file contains declarations for diarize_state, which watches the captured
 * audio stream and works out which speaker is talking.
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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Kept numerically equal to HISTORY_SPEAKER_UNKNOWN so ids can pass between the
// diarizer and history without translation
#define DIARIZE_SPEAKER_UNKNOWN (-1)

// Cosine similarity above which two stretches of speech are treated as the same
// person, for a full-length window. Shorter windows are held to a proportionally
// lower bar; see effective_threshold in diarize.c.
//
// Measured on three seconds of speech, the same person scored 0.56 to 0.74 and
// two different people 0.24 to 0.37, so this sits between the two with room on
// either side. Raise it to tell similar voices apart, lower it if one person
// keeps being split in two.
#define DIARIZE_DEFAULT_THRESHOLD 0.50f

struct diarize_state_i;
typedef struct diarize_state_i * diarize_state;

// One stretch of speech attributed to a single speaker. Times are on the
// capture clock described above.
struct diarize_segment {
    uint64_t start_ms;
    uint64_t end_ms;
    int32_t speaker_id;
    float confidence;
};

// Called from the diarizer's worker thread when the speaker holding the floor
// changes. The handler must not block, and must not call back into diarize_*.
typedef void (*diarize_change_handler)(void *userdata, int32_t speaker_id);

// Either model path may fail to load, in which case the diarizer still runs but
// attributes nothing, and diarize_has_models returns false. Captioning is never
// blocked by a missing speaker model.
diarize_state diarize_create(int sample_rate,
                             const char *vad_model_path,
                             const char *embed_model_path);
void diarize_free(diarize_state d);

// Whether both models loaded. If false, every speaker comes back UNKNOWN.
bool diarize_has_models(diarize_state d);

// Cosine similarity required to call two stretches of speech the same person.
// Higher splits similar voices apart; lower merges them.
void diarize_set_match_threshold(diarize_state d, float threshold);

// Upper bound on distinct speakers per session. Once reached, further voices are
// folded into the closest existing speaker rather than adding more.
void diarize_set_max_speakers(diarize_state d, int max_speakers);

void diarize_set_change_handler(diarize_state d,
                                diarize_change_handler handler,
                                void *userdata);

// Feeds captured audio to the diarizer. Called from the audio capture callback,
// so it never blocks and never allocates. Audio that does not fit is dropped,
// which only costs accuracy: the diarizer's clock stays aligned regardless.
//
// Every sample handed to this function advances the diarizer's clock by one,
// which is what keeps it on the same time base as the caller's capture clock.
void diarize_push(diarize_state d, const short *pcm, size_t num_shorts);

// Returns the speaker that held the floor for the most speech during
// [start_ms, end_ms], or DIARIZE_SPEAKER_UNKNOWN if there is nothing to go on.
int32_t diarize_speaker_for_range(diarize_state d, uint64_t start_ms, uint64_t end_ms);

// The speaker currently holding the floor, or DIARIZE_SPEAKER_UNKNOWN
int32_t diarize_current_speaker(diarize_state d);

// While disabled, pushed audio is discarded and no speakers are reported.
// Toggling this is cheap, so it can follow a GSettings key directly.
void diarize_set_enabled(diarize_state d, bool enabled);
bool diarize_is_enabled(diarize_state d);

// Number of samples dropped because the worker could not keep up. Intended for
// diagnostics; a persistently rising count means the machine is too slow.
uint64_t diarize_dropped_samples(diarize_state d);

// Blocks until the worker has consumed everything pushed so far and has closed
// off any turn still in progress.
void diarize_flush(diarize_state d);

// Copies out the most recent segments, oldest first, and returns how many were
// written. Used by the offline tools and tests.
size_t diarize_get_segments(diarize_state d, struct diarize_segment *out, size_t max);


// Most distinct voices one session can track. The user's max-speakers setting
// narrows this further; this is the ceiling on the table itself.
#define DIARIZE_MAX_SPEAKERS 16

#define DIARIZE_NAME_MAX 64

// What the diarizer knows about one speaker in this session, for handing back
// to the profile store
struct diarize_speaker_info {
    int32_t id;          // speaker index within this session
    uint64_t profile_id; // stored profile it came from, or 0 if new this session
    char name[DIARIZE_NAME_MAX];
    bool named;
    uint32_t updates;
    uint64_t speech_ms;
    const float *centroid; // embedding_dim floats, valid until the next call
};

// Width of the embeddings this diarizer produces, or 0 if no model loaded
int diarize_embedding_dim(diarize_state d);

// Introduces a voice from the profile store before captioning starts, so that
// somebody named in an earlier session is recognised straight away. Must be
// called before any audio is pushed.
bool diarize_seed_speaker(diarize_state d,
                          uint64_t profile_id,
                          const char *name,
                          const float *centroid,
                          int dim,
                          uint32_t updates);

// Copies out what has been learned about each speaker, for writing back to the
// profile store. Returns how many were written.
size_t diarize_snapshot_speakers(diarize_state d,
                                 struct diarize_speaker_info *out,
                                 size_t max);

// The name for a speaker, if one is known. Returns false if it is nameless.
bool diarize_get_speaker_name(diarize_state d,
                              int32_t speaker_id,
                              char *out,
                              size_t out_size);

// Names a speaker for the rest of the session
void diarize_set_speaker_name(diarize_state d, int32_t speaker_id, const char *name);

// The stored profile a speaker corresponds to, or 0 if it is new this session
uint64_t diarize_profile_id(diarize_state d, int32_t speaker_id);

// Links a speaker to a profile that has just been written to the store, so the
// rest of the session updates that profile rather than adding another
void diarize_bind_profile(diarize_state d, int32_t speaker_id, uint64_t profile_id);

// Drops every name and profile link, for when the stored voices are deleted.
// The speakers themselves stay, so the transcript does not lose its structure
// part way through a session; they simply go back to being anonymous.
void diarize_forget_profiles(diarize_state d);
