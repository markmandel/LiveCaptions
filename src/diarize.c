/* diarize.c
 * This file implements diarize_state, which consumes the captured audio stream
 * on its own thread and works out which speaker is talking.
 *
 * The pipeline is: ring buffer -> Silero VAD -> accumulate a stretch of speech
 * -> CAM++ speaker embedding -> online clustering against the speakers seen so
 * far. Turn boundaries come from the VAD rather than from april-asr's 2.2s
 * silence heuristic, which is what lets a speaker change be noticed mid-sentence.
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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include "diarize.h"
#include "spk-embed.h"
#include "spk-vad.h"

// How much captured audio to keep buffered. The worker only needs a second or
// two of headroom; the rest is slack so a scheduling hiccup does not cost audio.
#define DIARIZE_RING_SECONDS 30

// Largest block the worker will pull out of the ring in one go
#define DIARIZE_PULL_MAX 4096

// How many past turns to remember for diarize_speaker_for_range. At roughly one
// segment per second of speech this covers several minutes.
#define DIARIZE_TIMELINE_MAX 512

// Silero hysteresis. Speech has to clear the higher bar to start and fall below
// the lower one to stop, so a brief dip mid-word does not end a turn.
#define DIARIZE_VAD_ONSET 0.50f
#define DIARIZE_VAD_OFFSET 0.35f
#define DIARIZE_MIN_SPEECH_MS 250

// Long enough to ride over the pauses inside a sentence. At 200ms turns were
// splitting mid-utterance, and the resulting fragments were often too short to
// embed, so that speech went unattributed entirely.
#define DIARIZE_MIN_SILENCE_MS 400

// Speech needed before an embedding is worth computing, and how much trailing
// speech goes into one
#define DIARIZE_EMBED_MIN_MS 1000
#define DIARIZE_EMBED_WINDOW_MS 3000

// Speech required before the diarizer is willing to declare a voice it has not
// heard before. A short window produces an unstable embedding, and inventing a
// speaker is the more damaging mistake: it splits one person's transcript in
// two, where a wrong match merely mislabels a line.
#define DIARIZE_NEW_SPEAKER_MIN_MS 1500

// A turn shorter than DIARIZE_EMBED_MIN_MS is still worth embedding when it ends,
// as long as there is this much of it. The result is too shaky to introduce a
// speaker or to update one, but it is a much better guide than the clock.
#define DIARIZE_SHORT_EMBED_MIN_MS 400

// Last resort for a fragment too short even for that. Deliberately tight: at a
// real handover the gap between turns is often little more than a second, so a
// generous window here starts stealing the first words of the next speaker.
#define DIARIZE_INHERIT_GAP_MS 600

// A turn longer than this gets re-checked, so that a speaker change without a
// pause is still noticed
#define DIARIZE_EMBED_INTERVAL_MS 1000

#define DIARIZE_SPEECH_BUFFER_MS 12000

#define DIARIZE_MAX_SPEAKERS 16

// The match threshold doubles as the new-speaker threshold: measured against the
// CAM++ reference clips, same-speaker pairs never fell below 0.64 and
// different-speaker pairs never rose above 0.51, so one boundary in that gap
// separates both ways and an "uncertain" band in between would only invent
// speakers out of noise.
#define DIARIZE_NEW_MARGIN 0.0f

// How far below the threshold counts as "plainly somebody else" rather than
// "probably somebody else". Different-speaker pairs measured at most 0.51, so
// anything under roughly 0.40 is not a close call.
#define DIARIZE_CLEAR_NEW_MARGIN 0.15f

// Matches above this are confident enough to fold into the speaker's centroid.
// Anything weaker is still reported, but is kept out of the model of that voice
// so a marginal match cannot drag a centroid onto the wrong person.
#define DIARIZE_UPDATE_MARGIN 0.05f

// Floor on the learning rate, so a centroid keeps adapting to a voice that
// changes over a long session instead of freezing after the first few updates
#define DIARIZE_MIN_LEARNING_RATE 0.05f


struct diarize_speaker {
    float centroid[SPK_EMBED_MAX_DIM];
    uint32_t updates;
    bool active;
};

struct diarize_state_i {
    int sample_rate;

    // -- Audio ring, single producer (capture thread) / single consumer (worker) --
    short *ring;
    size_t ring_capacity;
    _Atomic size_t head; // read index, owned by the worker
    _Atomic size_t tail; // write index, owned by the capture thread

    // Samples the capture thread had to throw away because the ring was full.
    // The worker folds these into its clock so that time never drifts.
    _Atomic uint64_t dropped_samples;
    uint64_t dropped_seen;

    // Absolute index of the next sample the worker will read, counting every
    // sample ever pushed including dropped ones. This is the diarizer's clock,
    // and it advances in lockstep with the caller's own capture clock.
    uint64_t read_abs;

    // -- Speaker timeline, written by the worker, read by the ASR thread --
    GMutex timeline_mutex;
    struct diarize_segment timeline[DIARIZE_TIMELINE_MAX];
    size_t timeline_count; // total appended, may exceed DIARIZE_TIMELINE_MAX
    _Atomic int32_t current_speaker;

    // The turn in progress, which has not been committed to the timeline yet.
    // Without this a caller asking who spoke during the last few seconds gets
    // nothing until the speaker stops talking, because segments are only
    // appended when a turn ends.
    uint64_t pending_start_ms;
    int32_t pending_speaker;

    // -- Worker --
    GThread *thread;
    _Atomic bool ending;
    _Atomic bool enabled;

    // diarize_flush waits for flush_ack to catch up with flush_request
    _Atomic uint64_t flush_request;
    _Atomic uint64_t flush_ack;

    diarize_change_handler change_handler;
    void *change_userdata;

    // -- Models. Either both load or the diarizer runs inert. --
    spk_vad vad;
    spk_embed embed;
    bool models_ok;

    // -- VAD framing --
    float *vad_frame;      // normalized to [-1, 1] for Silero
    size_t vad_frame_size;
    size_t vad_fill;

    bool in_speech;
    uint64_t speech_start_ms;
    uint64_t last_speech_ms;
    uint64_t candidate_start_ms;   // speech seen but not yet past MIN_SPEECH_MS
    uint64_t candidate_silence_ms; // silence seen but not yet past MIN_SILENCE_MS
    bool pending_speech;
    bool pending_silence;

    // -- Speech accumulation, kept at int16 scale for the embedding model --
    float *speech;
    size_t speech_capacity;
    size_t speech_fill;
    uint64_t last_embed_ms;

    // -- Speakers --
    struct diarize_speaker speakers[DIARIZE_MAX_SPEAKERS];
    size_t num_speakers;
    int embed_dim;

    float match_threshold;
    int max_speakers;

    // Where the turn currently being attributed started
    uint64_t segment_start_ms;
    int32_t segment_speaker;

    // The last turn that was actually attributed, so a fragment too short to
    // identify can inherit from it
    int32_t last_turn_speaker;
    uint64_t last_turn_end_ms;
};


// Set LIVECAPTIONS_DIARIZE_DEBUG=1 to trace every turn boundary and every
// clustering decision. Invaluable when tuning thresholds against a recording.
static bool diarize_debug(void) {
    static int cached = -1;

    if(cached < 0) {
        const char *value = getenv("LIVECAPTIONS_DIARIZE_DEBUG");
        cached = ((value != NULL) && (value[0] != '\0') && (value[0] != '0')) ? 1 : 0;
    }

    return cached == 1;
}

#define DIARIZE_LOG(...) do {                    \
        if(diarize_debug()) {                    \
            printf("[diarize] " __VA_ARGS__);    \
            fflush(stdout);                      \
        }                                        \
    } while(0)

static uint64_t samples_to_ms(diarize_state d, uint64_t samples) {
    return (samples * 1000ull) / (uint64_t)d->sample_rate;
}

static uint64_t ms_to_samples(diarize_state d, uint64_t ms) {
    return (ms * (uint64_t)d->sample_rate) / 1000ull;
}

static size_t ring_available(diarize_state d) {
    size_t tail = d->tail;
    size_t head = d->head;

    if(tail >= head) return tail - head;
    return (d->ring_capacity - head) + tail;
}


void diarize_push(diarize_state d, const short *pcm, size_t num_shorts) {
    if((d == NULL) || (pcm == NULL) || (num_shorts == 0)) return;

    if(!d->enabled) {
        // Still count the audio, so that re-enabling does not leave the
        // diarizer's clock behind the caller's
        d->dropped_samples += num_shorts;
        return;
    }

    // Leave one slot free so that a full ring is distinguishable from an empty one
    size_t free_space = (d->ring_capacity - 1) - ring_available(d);

    if(num_shorts > free_space) {
        d->dropped_samples += num_shorts;
        return;
    }

    size_t tail = d->tail;
    size_t first_chunk = d->ring_capacity - tail;
    if(first_chunk > num_shorts) first_chunk = num_shorts;

    memcpy(&d->ring[tail], pcm, first_chunk * sizeof(short));

    if(first_chunk < num_shorts) {
        memcpy(&d->ring[0], &pcm[first_chunk], (num_shorts - first_chunk) * sizeof(short));
    }

    d->tail = (tail + num_shorts) % d->ring_capacity;
}


// Publishes the turn in progress so that diarize_speaker_for_range can see it
// before it is committed. Called from the worker; the fields are read by the
// ASR thread, so they are kept under the same lock as the timeline.
static void set_pending_segment(diarize_state d, uint64_t start_ms, int32_t speaker) {
    g_mutex_lock(&d->timeline_mutex);

    d->pending_start_ms = start_ms;
    d->pending_speaker = speaker;

    g_mutex_unlock(&d->timeline_mutex);
}

static void timeline_append(diarize_state d,
                            uint64_t start_ms,
                            uint64_t end_ms,
                            int32_t speaker_id,
                            float confidence)
{
    if(end_ms <= start_ms) return;

    g_mutex_lock(&d->timeline_mutex);

    struct diarize_segment *segment = &d->timeline[d->timeline_count % DIARIZE_TIMELINE_MAX];

    segment->start_ms = start_ms;
    segment->end_ms = end_ms;
    segment->speaker_id = speaker_id;
    segment->confidence = confidence;

    d->timeline_count += 1;

    g_mutex_unlock(&d->timeline_mutex);
}


int32_t diarize_speaker_for_range(diarize_state d, uint64_t start_ms, uint64_t end_ms) {
    if((d == NULL) || !d->enabled || (end_ms <= start_ms)) return DIARIZE_SPEAKER_UNKNOWN;

    // Tally how much speech each speaker contributed to the window and take the
    // largest. Ids are small and dense, so a flat array beats a hash here.
    uint64_t totals[DIARIZE_MAX_SPEAKERS] = { 0 };

    g_mutex_lock(&d->timeline_mutex);

    size_t available = (d->timeline_count < DIARIZE_TIMELINE_MAX)
        ? d->timeline_count
        : DIARIZE_TIMELINE_MAX;

    size_t oldest = d->timeline_count - available;

    for(size_t i=0; i<available; i++){
        const struct diarize_segment *segment = &d->timeline[(oldest + i) % DIARIZE_TIMELINE_MAX];

        if((segment->speaker_id < 0) || (segment->speaker_id >= DIARIZE_MAX_SPEAKERS)) continue;

        uint64_t overlap_start = (segment->start_ms > start_ms) ? segment->start_ms : start_ms;
        uint64_t overlap_end = (segment->end_ms < end_ms) ? segment->end_ms : end_ms;

        if(overlap_end <= overlap_start) continue;

        totals[segment->speaker_id] += (overlap_end - overlap_start);
    }

    // Include the turn still in progress, which runs from its start up to now
    if((d->pending_speaker >= 0) && (d->pending_speaker < DIARIZE_MAX_SPEAKERS)) {
        uint64_t overlap_start = (d->pending_start_ms > start_ms) ? d->pending_start_ms : start_ms;

        if(end_ms > overlap_start) {
            totals[d->pending_speaker] += (end_ms - overlap_start);
        }
    }

    g_mutex_unlock(&d->timeline_mutex);

    int32_t best_id = DIARIZE_SPEAKER_UNKNOWN;
    uint64_t best_total = 0;

    for(int32_t i=0; i<DIARIZE_MAX_SPEAKERS; i++){
        if(totals[i] > best_total) {
            best_total = totals[i];
            best_id = i;
        }
    }

    return best_id;
}


int32_t diarize_current_speaker(diarize_state d) {
    if((d == NULL) || !d->enabled) return DIARIZE_SPEAKER_UNKNOWN;
    return d->current_speaker;
}

void diarize_set_enabled(diarize_state d, bool enabled) {
    if(d == NULL) return;

    if(!enabled) d->current_speaker = DIARIZE_SPEAKER_UNKNOWN;

    // in_speech belongs to the worker thread, which notices the change itself.
    // Clearing it from here would be a race.
    d->enabled = enabled;
}

bool diarize_is_enabled(diarize_state d) {
    if(d == NULL) return false;
    return d->enabled;
}

bool diarize_has_models(diarize_state d) {
    if(d == NULL) return false;
    return d->models_ok;
}

uint64_t diarize_dropped_samples(diarize_state d) {
    if(d == NULL) return 0;
    return d->dropped_samples;
}

void diarize_set_change_handler(diarize_state d,
                                diarize_change_handler handler,
                                void *userdata)
{
    if(d == NULL) return;

    d->change_handler = handler;
    d->change_userdata = userdata;
}

void diarize_set_match_threshold(diarize_state d, float threshold) {
    if(d == NULL) return;
    d->match_threshold = threshold;
}

void diarize_set_max_speakers(diarize_state d, int max_speakers) {
    if(d == NULL) return;

    if(max_speakers < 1) max_speakers = 1;
    if(max_speakers > DIARIZE_MAX_SPEAKERS) max_speakers = DIARIZE_MAX_SPEAKERS;

    d->max_speakers = max_speakers;
}

size_t diarize_get_segments(diarize_state d, struct diarize_segment *out, size_t max) {
    if((d == NULL) || (out == NULL) || (max == 0)) return 0;

    g_mutex_lock(&d->timeline_mutex);

    size_t available = (d->timeline_count < DIARIZE_TIMELINE_MAX)
        ? d->timeline_count
        : DIARIZE_TIMELINE_MAX;

    if(available > max) available = max;

    size_t oldest = d->timeline_count - available;

    for(size_t i=0; i<available; i++){
        out[i] = d->timeline[(oldest + i) % DIARIZE_TIMELINE_MAX];
    }

    g_mutex_unlock(&d->timeline_mutex);

    return available;
}

void diarize_flush(diarize_state d) {
    if((d == NULL) || (d->thread == NULL)) return;

    uint64_t want = (d->flush_request += 1);

    while((d->flush_ack < want) && !d->ending) {
        g_usleep(1000);
    }
}


// -- Clustering -----------------------------------------------------------

// Matches an embedding against the speakers heard so far, adding a new one when
// nothing is close enough. Returns the speaker id, and the similarity it scored.
static int32_t classify(diarize_state d,
                        const float *embedding,
                        uint64_t speech_ms,
                        bool allow_new,
                        bool mid_turn,
                        float *out_similarity)
{
    float best_similarity = -1.0f;
    int32_t best_id = DIARIZE_SPEAKER_UNKNOWN;

    for(size_t i=0; i<d->num_speakers; i++){
        if(!d->speakers[i].active) continue;

        float similarity = spk_embed_similarity(embedding, d->speakers[i].centroid, d->embed_dim);

        if(similarity > best_similarity) {
            best_similarity = similarity;
            best_id = (int32_t)i;
        }
    }

    *out_similarity = best_similarity;

    float new_threshold = d->match_threshold - DIARIZE_NEW_MARGIN;
    float update_threshold = d->match_threshold + DIARIZE_UPDATE_MARGIN;

    bool room_for_more = (int)d->num_speakers < d->max_speakers;

    // Two tiers. Well below the threshold the voice plainly belongs to nobody
    // known, and is accepted as new however short the window. Just below it the
    // evidence is weak, so a new speaker is only created when there was enough
    // speech to trust the embedding; otherwise the closest match is used.
    bool clearly_new = best_similarity < (new_threshold - DIARIZE_CLEAR_NEW_MARGIN);
    bool marginally_new = (best_similarity < new_threshold)
                       && (speech_ms >= DIARIZE_NEW_SPEAKER_MIN_MS);

    // Partway through somebody's turn, a marginal score is far more likely to be
    // the same person sounding a little different than a new person who started
    // talking without a pause. Only an unmistakably different voice introduces a
    // speaker here; a real handover gets its own turn at the next pause anyway.
    if(mid_turn) marginally_new = false;

    // Callers working from a scrap of audio cannot introduce anyone: the
    // embedding is only reliable enough to pick between voices already known
    if(!allow_new) {
        clearly_new = false;
        marginally_new = false;

        if(best_id == DIARIZE_SPEAKER_UNKNOWN) return DIARIZE_SPEAKER_UNKNOWN;
    }

    if((best_id == DIARIZE_SPEAKER_UNKNOWN)
        || (room_for_more && (clearly_new || marginally_new))) {
        if((best_id == DIARIZE_SPEAKER_UNKNOWN) && !room_for_more) {
            return DIARIZE_SPEAKER_UNKNOWN;
        }

        // Nobody close enough, so this is someone new
        int32_t id = (int32_t)d->num_speakers;

        struct diarize_speaker *speaker = &d->speakers[id];
        memcpy(speaker->centroid, embedding, (size_t)d->embed_dim * sizeof(float));
        speaker->updates = 1;
        speaker->active = true;

        d->num_speakers += 1;

        return id;
    }

    // Only fold confident matches into the centroid. Anything in between is
    // reported but left out of the model of that voice, so an uncertain match
    // cannot drag a centroid onto the wrong speaker.
    if(best_similarity >= update_threshold) {
        struct diarize_speaker *speaker = &d->speakers[best_id];

        float rate = 1.0f / (float)(speaker->updates + 1);
        if(rate < DIARIZE_MIN_LEARNING_RATE) rate = DIARIZE_MIN_LEARNING_RATE;

        double sum_squares = 0.0;
        for(int i=0; i<d->embed_dim; i++){
            speaker->centroid[i] = speaker->centroid[i] * (1.0f - rate) + embedding[i] * rate;
            sum_squares += (double)speaker->centroid[i] * (double)speaker->centroid[i];
        }

        // Keep centroids on the unit sphere so similarity stays a cosine
        float norm = (float)sqrt(sum_squares);
        if(norm > 1e-9f) {
            for(int i=0; i<d->embed_dim; i++) speaker->centroid[i] /= norm;
        }

        speaker->updates += 1;
    }

    return best_id;
}


// Embeds the trailing speech accumulated so far and updates who holds the floor
static void embed_and_classify(diarize_state d, uint64_t now_ms, bool allow_new) {
    if(!d->models_ok || (d->speech_fill == 0)) return;

    size_t window = ms_to_samples(d, DIARIZE_EMBED_WINDOW_MS);
    size_t count = (d->speech_fill < window) ? d->speech_fill : window;
    const float *tail = &d->speech[d->speech_fill - count];

    float embedding[SPK_EMBED_MAX_DIM];
    if(!spk_embed_compute(d->embed, tail, count, embedding)) return;

    uint64_t window_ms = samples_to_ms(d, count);

    bool mid_turn = d->segment_speaker != DIARIZE_SPEAKER_UNKNOWN;

    float similarity = -1.0f;
    int32_t speaker = classify(d, embedding, window_ms, allow_new, mid_turn, &similarity);

    DIARIZE_LOG("embed at %.2fs: window=%" G_GUINT64_FORMAT "ms best_sim=%.3f -> speaker %d "
                "(was %d, %zu known%s)\n",
                (double)now_ms / 1000.0, window_ms, (double)similarity,
                speaker, d->segment_speaker, d->num_speakers,
                allow_new ? "" : ", no new");

    d->last_embed_ms = now_ms;

    if((speaker == DIARIZE_SPEAKER_UNKNOWN) || (speaker == d->segment_speaker)) return;

    if(d->segment_speaker == DIARIZE_SPEAKER_UNKNOWN) {
        // First attribution of this turn. It applies to the whole turn, not
        // just from the moment there was finally enough speech to embed, so
        // the segment is backdated to where the speech actually started.
        d->segment_start_ms = d->speech_start_ms;
        d->segment_speaker = speaker;
        d->current_speaker = speaker;
        set_pending_segment(d, d->segment_start_ms, speaker);

        if(d->change_handler != NULL) {
            d->change_handler(d->change_userdata, speaker);
        }

        return;
    }

    // The floor changed mid-turn. Close the previous stretch and tell the
    // caller, which flushes the recognizer so the transcript breaks here too.
    timeline_append(d, d->segment_start_ms, now_ms, d->segment_speaker, similarity);

    d->segment_start_ms = now_ms;
    d->segment_speaker = speaker;
    d->current_speaker = speaker;
    set_pending_segment(d, now_ms, speaker);

    if(d->change_handler != NULL) {
        d->change_handler(d->change_userdata, speaker);
    }
}


// Closes off the current stretch of speech and records it
static void end_speech_segment(diarize_state d, uint64_t end_ms) {
    if(!d->in_speech) return;

    DIARIZE_LOG("turn ends at %.2fs (started %.2fs, %" G_GUINT64_FORMAT "ms of speech, speaker %d)\n",
                (double)end_ms / 1000.0, (double)d->speech_start_ms / 1000.0,
                samples_to_ms(d, d->speech_fill), d->segment_speaker);

    // One last look, in case the turn ended before it was ever embedded. A turn
    // below the usual minimum is still tried, but on a shorter leash: it may
    // pick between known voices, never add one.
    if((d->speech_fill > 0) && (d->segment_speaker == DIARIZE_SPEAKER_UNKNOWN)) {
        uint64_t speech_ms = samples_to_ms(d, d->speech_fill);

        if(speech_ms >= DIARIZE_EMBED_MIN_MS) embed_and_classify(d, end_ms, true);
        else if(speech_ms >= DIARIZE_SHORT_EMBED_MIN_MS) embed_and_classify(d, end_ms, false);
    }

    // Too short even for that: if it followed hard on the heels of the last
    // turn, it is almost certainly the same person continuing
    if((d->segment_speaker == DIARIZE_SPEAKER_UNKNOWN)
        && (d->last_turn_speaker != DIARIZE_SPEAKER_UNKNOWN)
        && (d->speech_start_ms >= d->last_turn_end_ms)
        && ((d->speech_start_ms - d->last_turn_end_ms) <= DIARIZE_INHERIT_GAP_MS)) {

        DIARIZE_LOG("turn too short to embed, inheriting speaker %d from %.2fs\n",
                    d->last_turn_speaker, (double)d->last_turn_end_ms / 1000.0);

        d->segment_speaker = d->last_turn_speaker;
        d->segment_start_ms = d->speech_start_ms;
    }

    if(d->segment_speaker != DIARIZE_SPEAKER_UNKNOWN) {
        timeline_append(d, d->segment_start_ms, end_ms, d->segment_speaker, 0.0f);

        d->last_turn_speaker = d->segment_speaker;
        d->last_turn_end_ms = end_ms;
    }

    // Now committed to the timeline, so it is no longer pending
    set_pending_segment(d, 0, DIARIZE_SPEAKER_UNKNOWN);

    d->in_speech = false;
    d->pending_speech = false;
    d->pending_silence = false;
    d->speech_fill = 0;
    d->segment_speaker = DIARIZE_SPEAKER_UNKNOWN;
    d->current_speaker = DIARIZE_SPEAKER_UNKNOWN;

    // The next turn is not contiguous with this one
    if(d->vad != NULL) spk_vad_reset(d->vad);
}


static void append_speech(diarize_state d, const float *frame, size_t count) {
    if(d->speech_fill + count > d->speech_capacity) {
        // Keep the most recent audio: it is what the next embedding will use
        size_t keep = d->speech_capacity / 2;
        if(keep > d->speech_fill) keep = d->speech_fill;

        memmove(d->speech, &d->speech[d->speech_fill - keep], keep * sizeof(float));
        d->speech_fill = keep;

        if(count > d->speech_capacity - d->speech_fill) return;
    }

    memcpy(&d->speech[d->speech_fill], frame, count * sizeof(float));
    d->speech_fill += count;
}


// Runs one VAD frame's worth of audio through the state machine. `frame` is the
// normalized copy for Silero, `raw` the same audio at int16 scale for embedding.
static void process_vad_frame(diarize_state d,
                              const float *frame,
                              const float *raw,
                              size_t count,
                              uint64_t frame_end_ms)
{
    float probability = spk_vad_process(d->vad, frame);
    if(probability < 0.0f) return;

    uint64_t frame_ms = samples_to_ms(d, count);
    uint64_t frame_start_ms = (frame_end_ms > frame_ms) ? (frame_end_ms - frame_ms) : 0;

    if(!d->in_speech) {
        if(probability >= DIARIZE_VAD_ONSET) {
            if(!d->pending_speech) {
                d->pending_speech = true;
                d->candidate_start_ms = frame_start_ms;
            }

            // Speech has to persist before a turn opens, so a cough or a door
            // does not start one
            if((frame_end_ms - d->candidate_start_ms) >= DIARIZE_MIN_SPEECH_MS) {
                DIARIZE_LOG("turn starts at %.2fs\n", (double)d->candidate_start_ms / 1000.0);
                d->in_speech = true;
                d->pending_speech = false;
                d->speech_start_ms = d->candidate_start_ms;
                d->segment_start_ms = d->candidate_start_ms;
                d->segment_speaker = DIARIZE_SPEAKER_UNKNOWN;
                d->last_embed_ms = d->candidate_start_ms;
                d->speech_fill = 0;
            }
        } else {
            d->pending_speech = false;
        }

        if(d->in_speech) append_speech(d, raw, count);
        return;
    }

    if(probability >= DIARIZE_VAD_OFFSET) {
        d->pending_silence = false;
        d->last_speech_ms = frame_end_ms;
        append_speech(d, raw, count);
    } else {
        if(!d->pending_silence) {
            d->pending_silence = true;
            d->candidate_silence_ms = frame_start_ms;
        }

        if((frame_end_ms - d->candidate_silence_ms) >= DIARIZE_MIN_SILENCE_MS) {
            end_speech_segment(d, d->last_speech_ms);
            return;
        }

        // Still inside the grace period, so keep the audio: it is probably a
        // pause between words rather than the end of the turn
        append_speech(d, raw, count);
    }

    // Re-check periodically so a speaker change without a pause is still caught
    uint64_t speech_ms = samples_to_ms(d, d->speech_fill);

    bool enough = speech_ms >= DIARIZE_EMBED_MIN_MS;
    bool due = (frame_end_ms - d->last_embed_ms) >= DIARIZE_EMBED_INTERVAL_MS;

    if(enough && (due || (d->segment_speaker == DIARIZE_SPEAKER_UNKNOWN))) {
        embed_and_classify(d, frame_end_ms, true);
    }
}


static void process_block(diarize_state d, const short *pcm, size_t count) {
    if(!d->models_ok) return;

    static float raw_frame[4096];

    size_t offset = 0;

    while(offset < count) {
        size_t space = d->vad_frame_size - d->vad_fill;
        size_t take = count - offset;
        if(take > space) take = space;

        for(size_t i=0; i<take; i++){
            short sample = pcm[offset + i];

            // Silero wants [-1, 1]; the embedding model wants int16 scale
            d->vad_frame[d->vad_fill + i] = (float)sample / 32768.0f;
            raw_frame[d->vad_fill + i] = (float)sample;
        }

        d->vad_fill += take;
        offset += take;

        if(d->vad_fill < d->vad_frame_size) break;

        uint64_t frame_end_ms = samples_to_ms(d, d->read_abs + offset);

        process_vad_frame(d, d->vad_frame, raw_frame, d->vad_frame_size, frame_end_ms);

        d->vad_fill = 0;
    }
}


static void *run_diarize_thread(void *userdata) {
    diarize_state d = (diarize_state)userdata;

    short block[DIARIZE_PULL_MAX];
    bool was_enabled = d->enabled;

    while(!d->ending) {
        size_t available = ring_available(d);

        if(available == 0) {
            // Everything pushed so far has been consumed, so this is the point
            // at which a pending flush can be honoured
            uint64_t requested = d->flush_request;
            if(requested > d->flush_ack) {
                end_speech_segment(d, samples_to_ms(d, d->read_abs));
                d->flush_ack = requested;
            }

            // Nothing to do. Capture delivers ~50 ms fragments, so this is a
            // short nap rather than a busy wait.
            g_usleep(20000);
            continue;
        }

        size_t to_read = (available < DIARIZE_PULL_MAX) ? available : DIARIZE_PULL_MAX;

        size_t head = d->head;
        size_t first_chunk = d->ring_capacity - head;
        if(first_chunk > to_read) first_chunk = to_read;

        memcpy(block, &d->ring[head], first_chunk * sizeof(short));

        if(first_chunk < to_read) {
            memcpy(&block[first_chunk], &d->ring[0], (to_read - first_chunk) * sizeof(short));
        }

        d->head = (head + to_read) % d->ring_capacity;

        bool enabled_now = d->enabled;

        if(enabled_now) {
            if(!was_enabled) {
                // Audio was skipped while disabled, so whatever turn was open
                // before is no longer contiguous with what follows
                d->in_speech = false;
                d->vad_fill = 0;
                d->speech_fill = 0;
                if(d->vad != NULL) spk_vad_reset(d->vad);
            }
            process_block(d, block, to_read);
        } else if(was_enabled) {
            end_speech_segment(d, samples_to_ms(d, d->read_abs));
        }

        was_enabled = enabled_now;

        d->read_abs += to_read;

        // Fold in anything the capture thread had to throw away, so that the
        // clock keeps matching the caller's even after an overrun
        uint64_t dropped_now = d->dropped_samples;
        if(dropped_now != d->dropped_seen) {
            d->read_abs += (dropped_now - d->dropped_seen);
            d->dropped_seen = dropped_now;

            // The gap means the current turn is no longer contiguous
            end_speech_segment(d, samples_to_ms(d, d->read_abs));
        }
    }

    return NULL;
}


diarize_state diarize_create(int sample_rate,
                             const char *vad_model_path,
                             const char *embed_model_path)
{
    if(sample_rate <= 0) return NULL;

    diarize_state d = calloc(1, sizeof(struct diarize_state_i));
    if(d == NULL) return NULL;

    d->sample_rate = sample_rate;
    d->ring_capacity = (size_t)sample_rate * DIARIZE_RING_SECONDS;
    d->ring = calloc(d->ring_capacity, sizeof(short));

    if(d->ring == NULL) {
        free(d);
        return NULL;
    }

    d->current_speaker = DIARIZE_SPEAKER_UNKNOWN;
    d->segment_speaker = DIARIZE_SPEAKER_UNKNOWN;
    d->last_turn_speaker = DIARIZE_SPEAKER_UNKNOWN;
    d->pending_speaker = DIARIZE_SPEAKER_UNKNOWN;
    d->enabled = false;
    d->ending = false;
    d->match_threshold = DIARIZE_DEFAULT_THRESHOLD;
    d->max_speakers = 8;

    g_mutex_init(&d->timeline_mutex);

    // A missing or broken model is not fatal. The app keeps captioning; it just
    // does not attribute anything.
    d->vad = spk_vad_create(vad_model_path, sample_rate);
    d->embed = spk_embed_create(embed_model_path, sample_rate);
    d->models_ok = (d->vad != NULL) && (d->embed != NULL);

    if(d->models_ok) {
        d->embed_dim = spk_embed_dim(d->embed);
        d->vad_frame_size = spk_vad_frame_samples(d->vad);

        if(d->vad_frame_size > DIARIZE_PULL_MAX) {
            // process_block stages a frame at a time in a fixed buffer
            fprintf(stderr, "Diarization: VAD frame of %zu samples is too large\n",
                    d->vad_frame_size);
            d->models_ok = false;
        }
    }

    if(d->models_ok) {
        d->vad_frame = calloc(d->vad_frame_size, sizeof(float));

        d->speech_capacity = ms_to_samples(d, DIARIZE_SPEECH_BUFFER_MS);
        d->speech = calloc(d->speech_capacity, sizeof(float));

        if((d->vad_frame == NULL) || (d->speech == NULL)) d->models_ok = false;
    }

    if(!d->models_ok) {
        printf("Diarization: models unavailable, speakers will not be identified\n");
    }

    d->thread = g_thread_new("lcap-diarize", run_diarize_thread, d);

    return d;
}


void diarize_free(diarize_state d) {
    if(d == NULL) return;

    d->ending = true;

    if(d->thread != NULL) {
        g_thread_join(d->thread);
        g_thread_unref(d->thread);
    }

    g_mutex_clear(&d->timeline_mutex);

    spk_vad_free(d->vad);
    spk_embed_free(d->embed);

    free(d->vad_frame);
    free(d->speech);
    free(d->ring);
    free(d);
}
