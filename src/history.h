/* history.h
 *
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

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <april_api.h>
#include <adwaita.h>

#define HISTORY_TOKEN_MAX_CHARS 32
#define HISTORY_MAX_TOKENS 256
#define HISTORY_SPEAKER_NAME_MAX 64

// Speaker id used for entries that have not been attributed to anyone, either
// because diarization is disabled or because it could not decide
#define HISTORY_SPEAKER_UNKNOWN (-1)

extern char *default_history_file;


// A single token. The token text is inline for serialization simplicity
struct history_token {
    char token[HISTORY_TOKEN_MAX_CHARS]; // should this be a dynamic array?
    float logprob;
    AprilTokenFlagBits flags;
};

// One speaker within a session. `named` distinguishes a name the user confirmed
// from the "Speaker N" placeholder, which is generated at display time
struct history_speaker {
    int32_t id;
    char name[HISTORY_SPEAKER_NAME_MAX];
    bool named;
};

// A single history entry containing a collection of tokens
// An entry consisting of 0 tokens denotes silence
struct history_entry {
    time_t timestamp;
    int32_t speaker_id;
    size_t tokens_count;
    struct history_token *tokens;
};

// A Live Captions session
struct history_session {
    time_t timestamp;
    size_t speakers_count;
    struct history_speaker *speakers;
    size_t entries_count;
    struct history_entry *entries;
};

// List of past sessions
struct past_history_sessions {
    size_t num_sessions;
    struct history_session *sessions;
};

// Use static global variables for simplicity

// Initialize history
void history_init(void);

// Every time finalized, commit to list of history_entry
// speaker_id is HISTORY_SPEAKER_UNKNOWN when the entry is unattributed
void commit_tokens_to_current_history(const AprilToken *tokens,
                                      size_t tokens_count,
                                      int32_t speaker_id);


// Puts an empty entry into history meaning silence
void save_silence_to_history(void);


// Ensures the active session has a speaker with this id, creating one if not.
// Safe to call repeatedly with an id that already exists.
void history_register_speaker(int32_t speaker_id);

// Sets the user-confirmed name for a speaker in the active session
void history_set_speaker_name(int32_t speaker_id, const char *name);

// Looks up a speaker in a session, or NULL if the session has no such speaker
const struct history_speaker *history_find_speaker(const struct history_session *session,
                                                   int32_t speaker_id);

// Writes the display label for an entry's speaker into buf ("Mark", "Speaker 2").
// Returns false if the entry is unattributed, in which case buf is untouched.
bool history_speaker_label(const struct history_session *session,
                           int32_t speaker_id,
                           char *buf,
                           size_t buf_size);

// Serialize/Deserialize list of history_entry
void save_current_history(const char *path);
void load_history_from(const char *path);

// Convert to text file
void export_history_as_text(const char *path);


// 0 returns the active session
// 1 returns the previous session
// 2 returns the one prior to the previous
// ...
// returns NULL once reached the first session
const struct history_session *get_history_session(size_t idx);


void erase_all_history(void);