/* speaker-db-test.c
 * Checks that stored voices survive a save and reload, and that a store written
 * by a different embedding model is refused rather than silently mismatched.
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
#include <glib/gstdio.h>

#include "speaker-db.h"

#define DIM 8

static int failures = 0;

static void check(const char *what, bool ok) {
    printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
    if(!ok) failures++;
}

static void fill(float *centroid, float seed) {
    for(int i=0; i<DIM; i++) centroid[i] = seed + (float)i * 0.01f;
}

int main(void) {
    char *dir = g_dir_make_tmp("lcap-speakerdb-XXXXXX", NULL);
    if(dir == NULL) { fprintf(stderr, "could not make a temp dir\n"); return 2; }

    char *path = g_build_filename(dir, "speakers.bin", NULL);

    float mark[DIM], sarah[DIM];
    fill(mark, 0.5f);
    fill(sarah, -0.3f);

    uint64_t mark_id = 0, sarah_id = 0;

    printf("storing two voices\n");
    {
        speaker_db db = speaker_db_open(path, DIM);
        check("opening a store that does not exist yet works", db != NULL);
        check("a new store is empty", speaker_db_count(db) == 0);

        mark_id = speaker_db_add(db, mark, 4, 12000);
        sarah_id = speaker_db_add(db, sarah, 2, 8000);

        check("adding returns usable ids", (mark_id != SPEAKER_DB_NO_PROFILE)
                                        && (sarah_id != SPEAKER_DB_NO_PROFILE)
                                        && (mark_id != sarah_id));

        speaker_db_rename(db, mark_id, "Mark");

        check("saving succeeds", speaker_db_save(db));
        speaker_db_close(db);
    }

    printf("\nreloading\n");
    {
        speaker_db db = speaker_db_open(path, DIM);
        check("both voices came back", speaker_db_count(db) == 2);

        const struct speaker_profile *p = speaker_db_find(db, mark_id);
        check("the named voice is found by id", p != NULL);

        if(p != NULL) {
            check("the name survived", p->named && (strcmp(p->name, "Mark") == 0));
            check("the update count survived", p->updates == 4);
            check("the speech total survived", p->total_speech_ms == 12000);

            bool same = true;
            for(int i=0; i<DIM; i++) if(p->centroid[i] != mark[i]) same = false;
            check("the centroid survived exactly", same);
        }

        const struct speaker_profile *q = speaker_db_find(db, sarah_id);
        check("an unnamed voice stays unnamed", (q != NULL) && !q->named);

        speaker_db_close(db);
    }

    printf("\nforgetting one voice\n");
    {
        speaker_db db = speaker_db_open(path, DIM);
        speaker_db_forget(db, mark_id);

        check("count drops", speaker_db_count(db) == 1);
        check("the forgotten voice is gone", speaker_db_find(db, mark_id) == NULL);

        const struct speaker_profile *q = speaker_db_find(db, sarah_id);
        check("the other voice is intact", q != NULL);

        if(q != NULL) {
            // The centroids move when an earlier entry is removed, so this
            // catches the profile and its vector falling out of step
            bool same = true;
            for(int i=0; i<DIM; i++) if(q->centroid[i] != sarah[i]) same = false;
            check("its centroid still points at its own vector", same);
        }

        speaker_db_save(db);
        speaker_db_close(db);
    }

    printf("\nopening with a different embedding size\n");
    {
        // Centroids from another model live in a different space and cannot be
        // compared, so they must be discarded rather than matched against
        speaker_db db = speaker_db_open(path, DIM * 2);
        check("mismatched voices are discarded", speaker_db_count(db) == 0);
        speaker_db_close(db);
    }

    printf("\ngarbage file\n");
    {
        char *junk = g_build_filename(dir, "junk.bin", NULL);
        g_file_set_contents(junk, "not a speaker database at all", -1, NULL);

        speaker_db db = speaker_db_open(junk, DIM);
        check("a garbage file yields an empty store, not a crash",
              (db != NULL) && (speaker_db_count(db) == 0));

        speaker_db_close(db);
        g_unlink(junk);
        g_free(junk);
    }

    g_unlink(path);
    g_rmdir(dir);
    g_free(path);
    g_free(dir);

    printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
