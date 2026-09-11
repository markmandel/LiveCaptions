/* speaker-db.c
 * Stores the voices the application has heard before.
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
#include <time.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "speaker-db.h"

#define SPEAKER_DB_MAGIC "LCSP"
#define SPEAKER_DB_MAGIC_LEN 4
#define SPEAKER_DB_VERSION 1

// Sanity bounds, so a corrupt file cannot turn a garbage length into a huge
// allocation. Both are far above what any real use produces.
#define SPEAKER_DB_MAX_PROFILES 4096
#define SPEAKER_DB_MAX_DIM 4096

struct speaker_db_i {
    char *path;
    int embed_dim;

    struct speaker_profile *profiles;
    float *centroids; // count * embed_dim, indexed alongside profiles
    size_t count;
    size_t capacity;

    uint64_t next_id;
    bool dirty;
};

static char default_path[1024] = { 0 };

const char *speaker_db_default_path(void) {
    if(default_path[0] == '\0') {
        g_snprintf(default_path, sizeof(default_path),
                   "%s/live-captions-speakers.bin", g_get_user_data_dir());
    }

    return default_path;
}


// Profiles and their centroids grow together, and the profile's centroid
// pointer has to be refreshed whenever the block moves
static void repoint_centroids(speaker_db db) {
    for(size_t i=0; i<db->count; i++){
        db->profiles[i].centroid = &db->centroids[i * (size_t)db->embed_dim];
    }
}

static bool ensure_capacity(speaker_db db, size_t needed) {
    if(needed <= db->capacity) return true;

    size_t capacity = (db->capacity == 0) ? 8 : db->capacity;
    while(capacity < needed) capacity *= 2;

    struct speaker_profile *profiles = realloc(db->profiles,
        capacity * sizeof(struct speaker_profile));
    if(profiles == NULL) return false;
    db->profiles = profiles;

    float *centroids = realloc(db->centroids,
        capacity * (size_t)db->embed_dim * sizeof(float));
    if(centroids == NULL) return false;
    db->centroids = centroids;

    db->capacity = capacity;
    repoint_centroids(db);

    return true;
}


#define READ_POD(f, value) do {                            \
        if(fread(&(value), sizeof(value), 1, (f)) != 1)    \
            return false;                                  \
    } while(0)

static bool read_profiles(speaker_db db, FILE *f) {
    char magic[SPEAKER_DB_MAGIC_LEN];
    if(fread(magic, SPEAKER_DB_MAGIC_LEN, 1, f) != 1) return false;
    if(memcmp(magic, SPEAKER_DB_MAGIC, SPEAKER_DB_MAGIC_LEN) != 0) {
        printf("Speaker profiles: %s is not a profile file\n", db->path);
        return false;
    }

    uint32_t version = 0;
    READ_POD(f, version);

    if(version > SPEAKER_DB_VERSION) {
        printf("Speaker profiles: %s is version %u, this build understands up to %u. "
               "Not loading it, so that it does not get overwritten.\n",
               db->path, version, SPEAKER_DB_VERSION);
        return false;
    }

    uint32_t stored_dim = 0;
    READ_POD(f, stored_dim);

    if(stored_dim > SPEAKER_DB_MAX_DIM) return false;

    if((int)stored_dim != db->embed_dim) {
        // Embeddings from a different model live in a different space, so these
        // centroids cannot be compared against anything the current one makes
        printf("Speaker profiles: stored voices are %u-dimensional but the model "
               "produces %d, so they cannot be matched and are being discarded\n",
               stored_dim, db->embed_dim);
        return false;
    }

    uint32_t count = 0;
    READ_POD(f, count);

    if(count > SPEAKER_DB_MAX_PROFILES) return false;
    if(!ensure_capacity(db, count)) return false;

    for(uint32_t i=0; i<count; i++){
        struct speaker_profile *profile = &db->profiles[db->count];

        uint8_t named = 0;
        int64_t last_seen = 0;

        READ_POD(f, profile->id);
        if(fread(profile->name, SPEAKER_DB_NAME_MAX, 1, f) != 1) return false;
        READ_POD(f, named);
        READ_POD(f, profile->updates);
        READ_POD(f, profile->total_speech_ms);
        READ_POD(f, last_seen);

        float *centroid = &db->centroids[db->count * (size_t)db->embed_dim];
        if(fread(centroid, sizeof(float), (size_t)db->embed_dim, f) != (size_t)db->embed_dim) {
            return false;
        }

        // A corrupt or hand-edited name must not leave an unterminated string
        profile->name[SPEAKER_DB_NAME_MAX - 1] = '\0';
        profile->named = named != 0;
        profile->last_seen = (time_t)last_seen;
        profile->centroid = centroid;

        db->count += 1;

        if(profile->id >= db->next_id) db->next_id = profile->id + 1;
    }

    return true;
}


speaker_db speaker_db_open(const char *path, int embed_dim) {
    if((path == NULL) || (embed_dim <= 0) || (embed_dim > SPEAKER_DB_MAX_DIM)) return NULL;

    speaker_db db = calloc(1, sizeof(struct speaker_db_i));
    if(db == NULL) return NULL;

    db->path = g_strdup(path);
    db->embed_dim = embed_dim;
    db->next_id = 1;

    FILE *f = fopen(path, "rb");
    if(f != NULL) {
        if(!read_profiles(db, f)) {
            // Keep whatever parsed cleanly and drop the rest, rather than
            // refusing to start
            printf("Speaker profiles: could not read all of %s, keeping %zu voice(s)\n",
                   path, db->count);
        }

        fclose(f);

        printf("Speaker profiles: %zu voice(s) known\n", db->count);
    }

    return db;
}


bool speaker_db_save(speaker_db db) {
    if(db == NULL) return false;

    // Written to a neighbouring file and renamed, so an interrupted save cannot
    // destroy the profiles that were already there
    char *temp_path = g_strdup_printf("%s.new", db->path);

    FILE *f = fopen(temp_path, "wb");
    if(f == NULL) {
        printf("Speaker profiles: could not open %s for writing\n", temp_path);
        g_free(temp_path);
        return false;
    }

    fwrite(SPEAKER_DB_MAGIC, SPEAKER_DB_MAGIC_LEN, 1, f);

    uint32_t version = SPEAKER_DB_VERSION;
    uint32_t dim = (uint32_t)db->embed_dim;
    uint32_t count = (uint32_t)db->count;

    fwrite(&version, sizeof(version), 1, f);
    fwrite(&dim, sizeof(dim), 1, f);
    fwrite(&count, sizeof(count), 1, f);

    for(size_t i=0; i<db->count; i++){
        const struct speaker_profile *profile = &db->profiles[i];

        uint8_t named = profile->named ? 1 : 0;
        int64_t last_seen = (int64_t)profile->last_seen;

        fwrite(&profile->id, sizeof(profile->id), 1, f);
        fwrite(profile->name, SPEAKER_DB_NAME_MAX, 1, f);
        fwrite(&named, sizeof(named), 1, f);
        fwrite(&profile->updates, sizeof(profile->updates), 1, f);
        fwrite(&profile->total_speech_ms, sizeof(profile->total_speech_ms), 1, f);
        fwrite(&last_seen, sizeof(last_seen), 1, f);

        fwrite(&db->centroids[i * (size_t)db->embed_dim], sizeof(float),
               (size_t)db->embed_dim, f);
    }

    bool ok = (fflush(f) == 0) && (ferror(f) == 0);
    fclose(f);

    if(ok) ok = (g_rename(temp_path, db->path) == 0);

    if(!ok) {
        printf("Speaker profiles: could not save to %s\n", db->path);
        g_unlink(temp_path);
    } else {
        db->dirty = false;
    }

    g_free(temp_path);

    return ok;
}


void speaker_db_close(speaker_db db) {
    if(db == NULL) return;

    if(db->dirty) speaker_db_save(db);

    g_free(db->path);
    free(db->profiles);
    free(db->centroids);
    free(db);
}


int speaker_db_dim(speaker_db db) {
    if(db == NULL) return 0;
    return db->embed_dim;
}

size_t speaker_db_count(speaker_db db) {
    if(db == NULL) return 0;
    return db->count;
}

const struct speaker_profile *speaker_db_get(speaker_db db, size_t index) {
    if((db == NULL) || (index >= db->count)) return NULL;
    return &db->profiles[index];
}

const struct speaker_profile *speaker_db_find(speaker_db db, uint64_t id) {
    if((db == NULL) || (id == SPEAKER_DB_NO_PROFILE)) return NULL;

    for(size_t i=0; i<db->count; i++){
        if(db->profiles[i].id == id) return &db->profiles[i];
    }

    return NULL;
}


uint64_t speaker_db_add(speaker_db db,
                        const float *centroid,
                        uint32_t updates,
                        uint64_t speech_ms)
{
    if((db == NULL) || (centroid == NULL)) return SPEAKER_DB_NO_PROFILE;
    if(db->count >= SPEAKER_DB_MAX_PROFILES) return SPEAKER_DB_NO_PROFILE;
    if(!ensure_capacity(db, db->count + 1)) return SPEAKER_DB_NO_PROFILE;

    struct speaker_profile *profile = &db->profiles[db->count];

    profile->id = db->next_id++;
    profile->name[0] = '\0';
    profile->named = false;
    profile->updates = updates;
    profile->total_speech_ms = speech_ms;
    profile->last_seen = time(NULL);

    float *stored = &db->centroids[db->count * (size_t)db->embed_dim];
    memcpy(stored, centroid, (size_t)db->embed_dim * sizeof(float));
    profile->centroid = stored;

    db->count += 1;
    db->dirty = true;

    return profile->id;
}


void speaker_db_update(speaker_db db,
                       uint64_t id,
                       const float *centroid,
                       uint32_t updates,
                       uint64_t speech_ms)
{
    if((db == NULL) || (centroid == NULL)) return;

    for(size_t i=0; i<db->count; i++){
        if(db->profiles[i].id != id) continue;

        memcpy(&db->centroids[i * (size_t)db->embed_dim], centroid,
               (size_t)db->embed_dim * sizeof(float));

        db->profiles[i].updates = updates;
        db->profiles[i].total_speech_ms = speech_ms;
        db->profiles[i].last_seen = time(NULL);
        db->dirty = true;

        return;
    }
}


void speaker_db_rename(speaker_db db, uint64_t id, const char *name) {
    if(db == NULL) return;

    for(size_t i=0; i<db->count; i++){
        if(db->profiles[i].id != id) continue;

        if((name == NULL) || (name[0] == '\0')) {
            db->profiles[i].name[0] = '\0';
            db->profiles[i].named = false;
        } else {
            g_strlcpy(db->profiles[i].name, name, SPEAKER_DB_NAME_MAX);
            db->profiles[i].named = true;
        }

        db->dirty = true;
        return;
    }
}


void speaker_db_forget(speaker_db db, uint64_t id) {
    if(db == NULL) return;

    for(size_t i=0; i<db->count; i++){
        if(db->profiles[i].id != id) continue;

        // Close the gap, keeping profiles and centroids in step
        size_t remaining = db->count - i - 1;

        if(remaining > 0) {
            memmove(&db->profiles[i], &db->profiles[i+1],
                    remaining * sizeof(struct speaker_profile));

            memmove(&db->centroids[i * (size_t)db->embed_dim],
                    &db->centroids[(i+1) * (size_t)db->embed_dim],
                    remaining * (size_t)db->embed_dim * sizeof(float));
        }

        db->count -= 1;
        db->dirty = true;

        repoint_centroids(db);
        return;
    }
}


void speaker_db_forget_all(speaker_db db) {
    if(db == NULL) return;

    db->count = 0;
    db->dirty = true;

    speaker_db_save(db);
}
