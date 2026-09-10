/* history.c
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


#include <time.h>
#include <adwaita.h>
#include <glib/gi18n.h>
#include "history.h"

static struct history_session active_session = { 0 };
static struct past_history_sessions past_sessions = { 0 };

char default_history_file_v[1024] = { 0 };
char *default_history_file = NULL;

static GSettings *settings = NULL;
void history_init(void){
    // set timestamp for current session, etc
    active_session.timestamp = time(NULL);
    active_session.entries_count = 0;
    active_session.entries = NULL;
    active_session.speakers_count = 0;
    active_session.speakers = NULL;

    past_sessions.num_sessions = 0;
    past_sessions.sessions = NULL;

    default_history_file = default_history_file_v;

    const char *data_dir = g_get_user_data_dir();
    sprintf(default_history_file_v, "%s/live-captions-history.bin", data_dir);

    printf("Save file: %s\n", default_history_file);

    if(settings == NULL) settings = g_settings_new("net.sapples.LiveCaptions");
}


static struct history_entry *allocate_new_entry(size_t tokens_count) {
    active_session.entries_count += 1;
    active_session.entries = realloc(active_session.entries,
        active_session.entries_count * sizeof(struct history_entry));

    struct history_entry *entry = &active_session.entries[active_session.entries_count - 1];

    entry->tokens_count = tokens_count;
    entry->speaker_id = HISTORY_SPEAKER_UNKNOWN;

    if(tokens_count > 0)
        entry->tokens = calloc(tokens_count, sizeof(struct history_token));
    else
        entry->tokens = NULL;

    return entry;
}


static struct history_speaker *find_speaker_mut(struct history_session *session,
                                                int32_t speaker_id)
{
    if(speaker_id == HISTORY_SPEAKER_UNKNOWN) return NULL;

    for(size_t i=0; i<session->speakers_count; i++){
        if(session->speakers[i].id == speaker_id) return &session->speakers[i];
    }

    return NULL;
}

void history_register_speaker(int32_t speaker_id) {
    if(speaker_id == HISTORY_SPEAKER_UNKNOWN) return;
    if(find_speaker_mut(&active_session, speaker_id) != NULL) return;

    active_session.speakers_count += 1;
    active_session.speakers = realloc(active_session.speakers,
        active_session.speakers_count * sizeof(struct history_speaker));

    struct history_speaker *speaker = &active_session.speakers[active_session.speakers_count - 1];

    speaker->id = speaker_id;
    speaker->name[0] = '\0';
    speaker->named = false;
}

void history_set_speaker_name(int32_t speaker_id, const char *name) {
    history_register_speaker(speaker_id);

    struct history_speaker *speaker = find_speaker_mut(&active_session, speaker_id);
    if(speaker == NULL) return;

    if((name == NULL) || (name[0] == '\0')){
        speaker->name[0] = '\0';
        speaker->named = false;
        return;
    }

    g_strlcpy(speaker->name, name, HISTORY_SPEAKER_NAME_MAX);
    speaker->named = true;
}

const struct history_speaker *history_find_speaker(const struct history_session *session,
                                                   int32_t speaker_id)
{
    return find_speaker_mut((struct history_session *)session, speaker_id);
}

bool history_speaker_label(const struct history_session *session,
                           int32_t speaker_id,
                           char *buf,
                           size_t buf_size)
{
    if(speaker_id == HISTORY_SPEAKER_UNKNOWN) return false;

    const struct history_speaker *speaker = history_find_speaker(session, speaker_id);
    if((speaker != NULL) && speaker->named && (speaker->name[0] != '\0')){
        g_strlcpy(buf, speaker->name, buf_size);
        return true;
    }

    // Placeholder labels are 1-based so the first speaker reads "Speaker 1"
    g_snprintf(buf, buf_size, _("Speaker %d"), speaker_id + 1);
    return true;
}

void commit_tokens_to_current_history(const AprilToken *tokens,
                                      size_t tokens_count,
                                      int32_t speaker_id)
{
    history_register_speaker(speaker_id);

    struct history_entry *entry = allocate_new_entry(tokens_count);

    entry->timestamp = time(NULL);
    entry->speaker_id = speaker_id;

    for(size_t i=0; i<tokens_count; i++){
        struct history_token *token = &entry->tokens[i];

        if(strlen(tokens[i].token) >= HISTORY_TOKEN_MAX_CHARS){
            printf("Token %s is too long! (%zu)\n", tokens[i].token, strlen(tokens[i].token));
            g_assert(false);
        }

        strcpy(&token->token[0], tokens[i].token);
        token->logprob = tokens[i].logprob;
        token->flags   = tokens[i].flags;
    }
}

void save_silence_to_history(void){
    struct history_entry *entry = allocate_new_entry(0);
    entry->timestamp = time(NULL);
}


// -- Serialization --
//
// Version 1 files have no header at all; they open with a native size_t session
// count followed by raw fwrite'd structs. Version 2 opens with HISTORY_MAGIC and
// uses fixed-width fields throughout, so that adding the speaker table did not
// silently corrupt existing files. v1 files are still read, and get rewritten as
// v2 the next time history is saved.

#define HISTORY_MAGIC "LCAP"
#define HISTORY_MAGIC_LEN 4
#define HISTORY_VERSION 2

// Guards against a truncated or corrupt file turning a garbage length into a
// huge calloc. All three are far above anything a real session produces.
#define HISTORY_MAX_SESSIONS (1024u * 1024u)
#define HISTORY_MAX_ENTRIES_PER_SESSION (1024u * 1024u)
#define HISTORY_MAX_TOKENS_PER_ENTRY 4096u
#define HISTORY_MAX_SPEAKERS_PER_SESSION 4096u

#define WRITE_POD(f, value) fwrite(&(value), sizeof(value), 1, (f))

// Every read is checked; a short read aborts parsing of that session
#define READ_POD(f, value) do {                                \
        if(fread(&(value), sizeof(value), 1, (f)) != 1)        \
            return false;                                      \
    } while(0)


static void write_session_to_file(FILE *f, const struct history_session *session) {
    int64_t timestamp = (int64_t)session->timestamp;
    WRITE_POD(f, timestamp);

    uint32_t speakers_count = (uint32_t)session->speakers_count;
    WRITE_POD(f, speakers_count);

    for(size_t i=0; i<session->speakers_count; i++){
        const struct history_speaker *speaker = &session->speakers[i];

        int32_t id = speaker->id;
        uint8_t named = speaker->named ? 1 : 0;

        WRITE_POD(f, id);
        fwrite(speaker->name, HISTORY_SPEAKER_NAME_MAX, 1, f);
        WRITE_POD(f, named);
    }

    uint64_t entries_count = (uint64_t)session->entries_count;
    WRITE_POD(f, entries_count);

    for(size_t i=0; i<session->entries_count; i++){
        const struct history_entry *entry = &session->entries[i];

        int64_t entry_timestamp = (int64_t)entry->timestamp;
        int32_t speaker_id = entry->speaker_id;
        uint64_t tokens_count = (uint64_t)entry->tokens_count;

        WRITE_POD(f, entry_timestamp);
        WRITE_POD(f, speaker_id);
        WRITE_POD(f, tokens_count);

        for(size_t j=0; j<entry->tokens_count; j++){
            const struct history_token *token = &entry->tokens[j];

            uint32_t flags = (uint32_t)token->flags;

            fwrite(token->token, HISTORY_TOKEN_MAX_CHARS, 1, f);
            WRITE_POD(f, token->logprob);
            WRITE_POD(f, flags);
        }
    }
}

void save_current_history(const char *path){
    FILE *f = fopen(path, "w");

    if(f == NULL) {
        printf("Could not open %s for writing\n", path);
        return;
    }

    bool write_active_session = active_session.entries_count > 0;
    write_active_session = write_active_session && g_settings_get_boolean(settings, "save-history");

    fwrite(HISTORY_MAGIC, HISTORY_MAGIC_LEN, 1, f);

    uint32_t version = HISTORY_VERSION;
    WRITE_POD(f, version);

    uint64_t num_sessions_to_write = past_sessions.num_sessions + (write_active_session ? 1 : 0);
    WRITE_POD(f, num_sessions_to_write);

    for(size_t i=0; i<past_sessions.num_sessions; i++){
        write_session_to_file(f, &past_sessions.sessions[i]);
    }

    if(write_active_session)
        write_session_to_file(f, &active_session);

    fclose(f);
}


static bool read_session_v2(FILE *f, struct history_session *session) {
    int64_t timestamp = 0;
    READ_POD(f, timestamp);
    session->timestamp = (time_t)timestamp;

    uint32_t speakers_count = 0;
    READ_POD(f, speakers_count);
    if(speakers_count > HISTORY_MAX_SPEAKERS_PER_SESSION) return false;

    session->speakers_count = speakers_count;
    session->speakers = (speakers_count > 0)
        ? calloc(speakers_count, sizeof(struct history_speaker))
        : NULL;

    for(size_t i=0; i<session->speakers_count; i++){
        struct history_speaker *speaker = &session->speakers[i];

        uint8_t named = 0;

        READ_POD(f, speaker->id);
        if(fread(speaker->name, HISTORY_SPEAKER_NAME_MAX, 1, f) != 1) return false;
        READ_POD(f, named);

        // A corrupt or hand-edited name must not leave an unterminated string
        speaker->name[HISTORY_SPEAKER_NAME_MAX - 1] = '\0';
        speaker->named = named != 0;
    }

    uint64_t entries_count = 0;
    READ_POD(f, entries_count);
    if(entries_count > HISTORY_MAX_ENTRIES_PER_SESSION) return false;

    session->entries_count = entries_count;
    session->entries = (entries_count > 0)
        ? calloc(entries_count, sizeof(struct history_entry))
        : NULL;

    for(size_t i=0; i<session->entries_count; i++){
        struct history_entry *entry = &session->entries[i];

        int64_t entry_timestamp = 0;
        uint64_t tokens_count = 0;

        READ_POD(f, entry_timestamp);
        READ_POD(f, entry->speaker_id);
        READ_POD(f, tokens_count);

        if(tokens_count > HISTORY_MAX_TOKENS_PER_ENTRY) return false;

        entry->timestamp = (time_t)entry_timestamp;
        entry->tokens_count = tokens_count;

        if(tokens_count == 0){
            entry->tokens = NULL;
            continue;
        }

        entry->tokens = calloc(tokens_count, sizeof(struct history_token));

        for(size_t j=0; j<entry->tokens_count; j++){
            struct history_token *token = &entry->tokens[j];

            uint32_t flags = 0;

            if(fread(token->token, HISTORY_TOKEN_MAX_CHARS, 1, f) != 1) return false;
            READ_POD(f, token->logprob);
            READ_POD(f, flags);

            token->token[HISTORY_TOKEN_MAX_CHARS - 1] = '\0';
            token->flags = (AprilTokenFlagBits)flags;
        }
    }

    return true;
}


// Legacy reader. Matches the original raw-struct layout exactly, including the
// native size_t/time_t widths, so files written by older builds still load.
static bool read_session_v1(FILE *f, struct history_session *session) {
    READ_POD(f, session->timestamp);
    READ_POD(f, session->entries_count);

    if(session->entries_count > HISTORY_MAX_ENTRIES_PER_SESSION) return false;

    // v1 had no speaker information, so everything stays unattributed
    session->speakers_count = 0;
    session->speakers = NULL;

    session->entries = (session->entries_count > 0)
        ? calloc(session->entries_count, sizeof(struct history_entry))
        : NULL;

    for(size_t i=0; i<session->entries_count; i++){
        struct history_entry *entry = &session->entries[i];

        READ_POD(f, entry->timestamp);
        READ_POD(f, entry->tokens_count);

        if(entry->tokens_count > HISTORY_MAX_TOKENS_PER_ENTRY) return false;

        entry->speaker_id = HISTORY_SPEAKER_UNKNOWN;

        if(entry->tokens_count == 0){
            entry->tokens = NULL;
            continue;
        }

        entry->tokens = calloc(
            entry->tokens_count,
            sizeof(struct history_token)
        );

        for(size_t j=0; j<entry->tokens_count; j++){
            struct history_token *token = &entry->tokens[j];
            if(fread(token, sizeof(struct history_token), 1, f) != 1) return false;
            token->token[HISTORY_TOKEN_MAX_CHARS - 1] = '\0';
        }
    }

    return true;
}

void load_history_from(const char *path){
    FILE *f = fopen(path, "r");

    if(f == NULL) {
        printf("fopen %s returned NULL\n", path);
        return;
    }

    char magic[HISTORY_MAGIC_LEN];
    bool is_v2 = (fread(magic, HISTORY_MAGIC_LEN, 1, f) == 1)
              && (memcmp(magic, HISTORY_MAGIC, HISTORY_MAGIC_LEN) == 0);

    uint64_t num_sessions_in_file = 0;

    if(is_v2) {
        uint32_t version = 0;
        if(fread(&version, sizeof(version), 1, f) != 1){
            fclose(f);
            return;
        }

        if(version > HISTORY_VERSION) {
            printf("History file %s is version %u, this build understands up to %u. "
                   "Not loading it, so that it does not get overwritten.\n",
                   path, version, HISTORY_VERSION);
            fclose(f);
            return;
        }

        if(fread(&num_sessions_in_file, sizeof(num_sessions_in_file), 1, f) != 1){
            fclose(f);
            return;
        }
    } else {
        // No magic, so this is a v1 file whose first field is the session count
        rewind(f);

        size_t v1_count = 0;
        if(fread(&v1_count, sizeof(v1_count), 1, f) != 1){
            fclose(f);
            return;
        }

        num_sessions_in_file = v1_count;
        printf("Migrating history file %s from version 1\n", path);
    }

    if(num_sessions_in_file > HISTORY_MAX_SESSIONS) {
        printf("History file %s claims %" G_GUINT64_FORMAT " sessions, refusing to load it\n",
               path, num_sessions_in_file);
        fclose(f);
        return;
    }

    if(num_sessions_in_file == 0) {
        fclose(f);
        return;
    }

    past_sessions.sessions = calloc(num_sessions_in_file, sizeof(struct history_session));
    past_sessions.num_sessions = 0;

    for(size_t i=0; i<num_sessions_in_file; i++){
        bool ok = is_v2 ? read_session_v2(f, &past_sessions.sessions[i])
                        : read_session_v1(f, &past_sessions.sessions[i]);

        if(!ok) {
            // Keep every session that parsed cleanly and drop the truncated tail
            printf("History file %s is truncated or corrupt at session %zu, "
                   "keeping the %zu session(s) before it\n",
                   path, i, past_sessions.num_sessions);
            break;
        }

        past_sessions.num_sessions += 1;
    }

    fclose(f);
}


static void export_session_into_text(FILE *f, const struct history_session *session) {
    char time_buff[512];

    struct tm *tm = localtime(&session->timestamp);
    strftime(time_buff, 512, "%F | %H:%M", tm);

    fprintf(f, "    -[ %s ]-    ", time_buff);

    for(size_t i=0; i<session->entries_count; i++){
        struct history_entry *entry = &session->entries[i];

        tm = localtime_r(&entry->timestamp, tm);
        strftime(time_buff, 512, "%T", tm);

        fprintf(f, "\n(%s) - ", time_buff);

        char speaker_label[HISTORY_SPEAKER_NAME_MAX];
        if(history_speaker_label(session, entry->speaker_id,
                                 speaker_label, sizeof(speaker_label))) {
            fprintf(f, "%s: ", speaker_label);
        }

        for(size_t j=0; j<entry->tokens_count; j++){
            fprintf(f, "%s", entry->tokens[j].token);
        }
    }

    fprintf(f, "\n\n");
}

void export_history_as_text(const char *path) {
    // TODO: Apply current settings for filtering, capitalization, etc
    
    FILE *f = fopen(path, "w");
    g_assert(f != NULL);

    for(size_t i=0; i<past_sessions.num_sessions; i++){
        if(past_sessions.sessions[i].entries_count == 0) continue;

        export_session_into_text(f, &past_sessions.sessions[i]);
    }

    if(active_session.entries_count > 0)
        export_session_into_text(f, &active_session);

    fclose(f);
}

const struct history_session *get_history_session(size_t idx) {
    if(idx == 0) return &active_session;

    ssize_t i = ((ssize_t)past_sessions.num_sessions - (ssize_t)idx);
    if(i < 0) return NULL;

    return &past_sessions.sessions[i];
}


static void free_session_contents(struct history_session *session) {
    for(size_t i=0; i<session->entries_count; i++){
        free(session->entries[i].tokens);
    }

    free(session->entries);
    free(session->speakers);

    session->entries = NULL;
    session->entries_count = 0;
    session->speakers = NULL;
    session->speakers_count = 0;
}

void erase_all_history(void){
    for(size_t i=0; i<past_sessions.num_sessions; i++){
        free_session_contents(&past_sessions.sessions[i]);
    }
    free(past_sessions.sessions);

    free_session_contents(&active_session);

    active_session.timestamp = time(NULL);

    past_sessions.num_sessions = 0;
    past_sessions.sessions = NULL;

    save_current_history(default_history_file);
}