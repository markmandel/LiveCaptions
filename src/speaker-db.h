/* speaker-db.h
 * Stores the voices the application has heard before, so that somebody named
 * once is recognised again in later sessions.
 *
 * A profile is a speaker embedding and the name the user gave it. Embeddings
 * are derived from a person's voice, so they stay on this machine like the
 * audio they came from: the file lives under the user's data directory and is
 * never sent anywhere.
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
#include <sys/types.h>

#define SPEAKER_DB_NAME_MAX 64

// Profile ids start at 1 so that zero can mean "not stored"
#define SPEAKER_DB_NO_PROFILE 0

struct speaker_profile {
    uint64_t id;
    char name[SPEAKER_DB_NAME_MAX];
    bool named;

    // How many embeddings have been folded into the centroid, which sets how
    // readily it still moves
    uint32_t updates;

    uint64_t total_speech_ms;
    time_t last_seen;

    const float *centroid; // speaker_db_dim() floats, owned by the database
};

struct speaker_db_i;
typedef struct speaker_db_i * speaker_db;

// Path to the default profile file, under the user's data directory
const char *speaker_db_default_path(void);

// Opens the store, reading whatever is already there. Profiles recorded with a
// different embedding size cannot be compared against the current model, so
// they are discarded rather than silently mismatched.
speaker_db speaker_db_open(const char *path, int embed_dim);

// Saves if anything changed, then frees
void speaker_db_close(speaker_db db);

bool speaker_db_save(speaker_db db);

int speaker_db_dim(speaker_db db);
size_t speaker_db_count(speaker_db db);

// Profiles in storage order. The pointer is valid until the store is modified.
const struct speaker_profile *speaker_db_get(speaker_db db, size_t index);
const struct speaker_profile *speaker_db_find(speaker_db db, uint64_t id);

// Adds a voice and returns its new id, or SPEAKER_DB_NO_PROFILE if it could not
// be stored
uint64_t speaker_db_add(speaker_db db,
                        const float *centroid,
                        uint32_t updates,
                        uint64_t speech_ms);

// Replaces what is known about a voice after hearing more of it
void speaker_db_update(speaker_db db,
                       uint64_t id,
                       const float *centroid,
                       uint32_t updates,
                       uint64_t speech_ms);

// Sets the name the user chose. Passing NULL or an empty string clears it,
// putting the profile back to being nameless.
void speaker_db_rename(speaker_db db, uint64_t id, const char *name);

void speaker_db_forget(speaker_db db, uint64_t id);
void speaker_db_forget_all(speaker_db db);
