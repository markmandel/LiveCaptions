/* line-gen.h
 * This file contains the declaration for line_generator, which generates
 * two lines based on AprilTokens from aprilasr
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

#define AC_LINE_MAX 4096
#define AC_LINE_COUNT 25

struct token_capitalizer {
    bool is_english;
    bool finished_at_period;
    bool previous_was_period;

    bool force_next_cap;
};

void token_capitalizer_init(struct token_capitalizer *tc);
bool token_capitalizer_next(struct token_capitalizer *tc, const char *token, int flags, const char *subsequent_token, int subsequent_flags);
void token_capitalizer_finish(struct token_capitalizer *tc);
void token_capitalizer_rewind(struct token_capitalizer *tc);


struct line {
    char text[AC_LINE_MAX];

    size_t start_head;
    size_t start_len;

    size_t head;
    size_t len;

    // The speaker whose name opens this line, and how many bytes of markup that
    // name takes up. Kept so a rename can rewrite lines already on screen
    // instead of only applying to whatever is said next.
    int32_t speaker_id;
    size_t prefix_len;
    size_t prefix_width;
};

// Speaker id meaning "not attributed", matching HISTORY_SPEAKER_UNKNOWN
#define LINE_SPEAKER_UNKNOWN (-1)
#define LINE_SPEAKER_NAME_MAX 64

// Speaker names are rendered as links so they can be clicked in the caption
// window. The scheme is our own, so activate-link can tell them from a real URL.
#define LINE_SPEAKER_URI_PREFIX "livecaptions-speaker:"

struct line_generator {
    size_t current_line;
    struct line lines[AC_LINE_COUNT];

    // Who the line being written belongs to, so that a change of speaker can
    // start a fresh line with their name on it
    int32_t current_speaker_id;

    // Denotes the index within the active token array at which the line starts
    // If -1, means the active tokens don't reach that line yet
    ssize_t active_start_of_lines[AC_LINE_COUNT];

    char output[AC_LINE_MAX * AC_LINE_COUNT];
    char plaintext[AC_LINE_MAX * AC_LINE_COUNT];

    PangoLayout *layout;
    int max_text_width;

    bool is_english;
    struct token_capitalizer tcap;
};

void line_generator_init(struct line_generator *lg);
void line_generator_update(struct line_generator *lg, size_t num_tokens, const AprilToken *tokens);
void line_generator_finalize(struct line_generator *lg);
void line_generator_break(struct line_generator *lg);
// Starts a new line attributed to `speaker_id`, prefixed with `name`. Does
// nothing if that speaker already holds the current line. Pass
// LINE_SPEAKER_UNKNOWN to go back to unattributed text.
void line_generator_set_speaker(struct line_generator *lg,
                                int32_t speaker_id,
                                const char *name);

// Rewrites the name on every line already showing this speaker, so a rename
// takes effect on what is on screen rather than only on what is said next.
// Returns true if any line changed.
bool line_generator_rename_speaker(struct line_generator *lg,
                                   int32_t speaker_id,
                                   const char *name);

// The colour used for a speaker's name, as a Pango colour string
const char *line_generator_speaker_color(int32_t speaker_id);

void line_generator_set_text(struct line_generator *lg, GtkLabel *lbl);
void line_generator_set_language(struct line_generator *lg, const char* language);
const char *line_generator_get_plaintext(struct line_generator *lg);