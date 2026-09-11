/* line-gen.c
 * This file contains the implementation for line_generator
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
#include <stdbool.h>
#include <april_api.h>

#include <adwaita.h>

#include "line-gen.h"
#include "profanity-filter.h"
#include "common.h"

void token_capitalizer_init(struct token_capitalizer *tc) {
    tc->is_english = true;
    tc->previous_was_period = true;
    tc->finished_at_period = false;
    tc->force_next_cap = false;
}

bool token_capitalizer_next(struct token_capitalizer *tc, const char *token, int flags, const char *subsequent_token, int subsequent_flags) {
    if((flags & APRIL_TOKEN_FLAG_SENTENCE_END_BIT) != 0){
        tc->previous_was_period = true;
        return false;
    }

    if(tc->force_next_cap){
        tc->force_next_cap = false;
        return true;
    }

    if((tc->previous_was_period) && (flags & APRIL_TOKEN_FLAG_WORD_BOUNDARY_BIT)){
        // If bare space token, capitalize the subsequent token since space can't
        // be capitalized.
        if((token[0] == ' ') && (token[1] == 0)){
            tc->force_next_cap = true;
        }

        tc->previous_was_period = false;
        return true;
    }

    // English-specific behavior: capitalize 'I'
    // TODO: A better way of capitalizing I and names and places
    if(tc->is_english) {
        if((token[0] == ' ') && (token[1] == 'I') && (token[2] == 0)){
            if(subsequent_token != NULL){
                if (((subsequent_flags & (APRIL_TOKEN_FLAG_WORD_BOUNDARY_BIT | APRIL_TOKEN_FLAG_SENTENCE_END_BIT)) != 0) || (subsequent_token[0] == '\'')){
                    return true;
                }
            }else{
                return true;
            }
        }
    }

    return false;
}

void token_capitalizer_finish(struct token_capitalizer *tc){
    tc->finished_at_period = tc->previous_was_period;
    tc->previous_was_period = false;
    tc->force_next_cap = false;
}

void token_capitalizer_rewind(struct token_capitalizer *tc){
    tc->previous_was_period = tc->finished_at_period;
    tc->force_next_cap = false;
}


#define REL_LINE_IDX(HEAD, IDX) (4*AC_LINE_COUNT + (HEAD) + (IDX)) % AC_LINE_COUNT

static GSettings *settings = NULL;

void line_generator_init(struct line_generator *lg) {
    for(int i=0; i<AC_LINE_COUNT; i++){
        lg->active_start_of_lines[i] = -1;

        lg->lines[i].start_head = 0;
        lg->lines[i].start_len = 0;
        lg->lines[i].speaker_id = LINE_SPEAKER_UNKNOWN;
        lg->lines[i].prefix_len = 0;
        lg->lines[i].prefix_width = 0;
    }

    lg->current_line = 0;
    lg->active_start_of_lines[0] = 0;
    lg->current_speaker_id = LINE_SPEAKER_UNKNOWN;

    if(settings == NULL) settings = g_settings_new("net.sapples.LiveCaptions");

    token_capitalizer_init(&lg->tcap);
}

static int line_generator_get_text_width(struct line_generator *lg, const char *text){
    pango_layout_set_width(lg->layout, -1);

    int width, height;
    pango_layout_set_text(lg->layout, text, strlen(text));
    pango_layout_get_size(lg->layout, &width, &height);

    return width / PANGO_SCALE;
}

#define MAX_TOKEN_SCRATCH 72
void line_generator_update(struct line_generator *lg, size_t num_tokens, const AprilToken *tokens) {
    // Add capitalization information
    static bool should_capitalize[1024];

    token_capitalizer_rewind(&lg->tcap);
    for(size_t i=0; i<num_tokens; i++){
        if((i+1) < num_tokens) {
            should_capitalize[i] = token_capitalizer_next(&lg->tcap, tokens[i].token, tokens[i].flags, tokens[i+1].token, tokens[i+1].flags);
        }else{
            should_capitalize[i] = token_capitalizer_next(&lg->tcap, tokens[i].token, tokens[i].flags, NULL, 0);
        }
    }

    bool use_fade = g_settings_get_boolean(settings, "fade-text");

    bool filter_slurs = g_settings_get_boolean(settings, "filter-slurs");
    bool filter_profanity = g_settings_get_boolean(settings, "filter-profanity");

    FilterMode filter_mode = filter_profanity ? FILTER_PROFANITY : (filter_slurs ? FILTER_SLURS : FILTER_NONE);

    bool use_lowercase = !g_settings_get_boolean(settings, "text-uppercase");
    char token_scratch[MAX_TOKEN_SCRATCH] = { 0 };

    for(size_t i=0; i<AC_LINE_COUNT; i++){
        if(lg->active_start_of_lines[i] == -1) continue;
        size_t start_of_line = lg->active_start_of_lines[i];

        struct line *curr = &lg->lines[i];

        // reset for writing
        curr->text[curr->start_head] = '\0';
        curr->head = curr->start_head;
        curr->len = curr->start_len;

        if(num_tokens == 0) continue;

        if(start_of_line >= num_tokens) {
            if(i == lg->current_line) {
                // oops... turns out our text isn't long enough for the new line
                // backtrack to the previous line
                lg->active_start_of_lines[lg->current_line] = -1;
                lg->current_line = REL_LINE_IDX(lg->current_line, -1);
                return line_generator_update(lg, num_tokens, tokens);
            } else {
                continue;
            }
        }


        ssize_t end = lg->active_start_of_lines[REL_LINE_IDX(i, 1)];
        if((end == -1) || (i == lg->current_line)) end = num_tokens;

        // print line
        for(size_t j=start_of_line; j<((size_t)end);) {
            size_t skipahead = 1;
            const char *token = tokens[j].token;

            bool should_be_capitalized = should_capitalize[j];

            if(use_lowercase){
                char *out = token_scratch;
                const char *p = tokens[j].token;
                gunichar c;
                while (*p) {
                    c = g_utf8_get_char_validated(p, -1);
                    if(c == ((gunichar)-2)) {
                        printf("gunichar -2 \n");
                        break;
                    }else if(c == ((gunichar)-1)) {
                        printf("gunichar -1 \n");
                        break;
                    }

                    c = g_unichar_tolower(c);

                    if(should_be_capitalized){
                        gunichar c1 = g_unichar_toupper(c);
                        if(c != c1){
                            c = c1;
                            should_be_capitalized = false;
                        }
                    }

                    out += g_unichar_to_utf8(c, out);
                    if((out + 6) >= (token_scratch + MAX_TOKEN_SCRATCH)){
                        printf("Unicode too big for token scratch!\n");
                        break;
                    }

                    p = g_utf8_next_char(p);
                }

                *out = '\0';

                token = token_scratch;
            }

            // filter current word, if applicable
            if((filter_mode > FILTER_NONE) && (tokens[j].flags & APRIL_TOKEN_FLAG_WORD_BOUNDARY_BIT)) {
                size_t skip = get_filter_skip(tokens, j, num_tokens, filter_mode);
                if(skip > 0) {
                    skipahead = skip;
                    token = SWEAR_REPLACEMENT;
                }
            }

            // skip if line is too long to safely write
            bool must_break = (curr->head > (AC_LINE_MAX - 256));
            if(must_break){
                printf("Must linebreak, but not active line. Leaving incomplete line...\n");
                break;
            }

            // break line if too long
            if(i == lg->current_line){
                curr->len += line_generator_get_text_width(lg, token);
                if(curr->len >= lg->max_text_width) {
                    size_t tgt_brk = j;
                    // find previous word boundary
                    while((!(tokens[tgt_brk].flags & APRIL_TOKEN_FLAG_WORD_BOUNDARY_BIT)) && (tgt_brk > start_of_line)) tgt_brk--;

                    // if we backtracked all the way to the start of line, just give up and break here
                    // unless this line has starting text
                    if((tgt_brk == start_of_line) && (curr->start_head == 0)) tgt_brk = j;

                    // line break
                    lg->current_line = REL_LINE_IDX(lg->current_line, 1);
                    lg->active_start_of_lines[lg->current_line] = tgt_brk;
                    lg->lines[lg->current_line].start_head = 0;
                    lg->lines[lg->current_line].start_len = 0;
                    return line_generator_update(lg, num_tokens, tokens);
                }
            }

            // write the actual line
            int alpha = (int)((tokens[j].logprob + 2.0) / 8.0 * 65536.0);
            alpha /= 2.0;
            alpha += 32768;
            if(alpha < 10000) alpha = 10000;
            if(alpha > 65535) alpha = 65535;

            if(use_fade)
                curr->head += sprintf(&curr->text[curr->head], "<span fgalpha=\"%d\">%s</span>", alpha, token);
            else
                curr->head += sprintf(&curr->text[curr->head], "%s", token);
            
            g_assert(curr->head < AC_LINE_MAX);

            j += skipahead;
        }
    }
}

void line_generator_finalize(struct line_generator *lg) {
    // reset active
    for(size_t i=0; i<AC_LINE_COUNT; i++) lg->active_start_of_lines[i] = -1;

    // freeze the current line thus far
    lg->lines[lg->current_line].start_head = lg->lines[lg->current_line].head;
    lg->lines[lg->current_line].start_len = lg->lines[lg->current_line].len;

    token_capitalizer_finish(&lg->tcap);

    // set new line to start at 0
    lg->active_start_of_lines[lg->current_line] = 0;
}

void line_generator_break(struct line_generator *lg) {
    // Drop anything the outgoing line was still in the middle of rendering.
    //
    // Those words came from the active token array, which the new line takes
    // over and renders again from the start. Only lines that are still active
    // get truncated in line_generator_update, and this one is about to stop
    // being active, so without this the same words stay on both lines.
    //
    // Text that was finalized sits below start_head and is left untouched.
    struct line *outgoing = &lg->lines[lg->current_line];
    outgoing->text[outgoing->start_head] = '\0';
    outgoing->head = outgoing->start_head;
    outgoing->len = outgoing->start_len;

    // insert new line
    lg->current_line = REL_LINE_IDX(lg->current_line, 1);

    // reset active
    for(int i=0; i<AC_LINE_COUNT; i++) lg->active_start_of_lines[i] = -1;

    // set new line to start at 0
    lg->active_start_of_lines[lg->current_line] = 0;

    // clear new line
    lg->lines[lg->current_line].text[0] = '\0';
    lg->lines[lg->current_line].head = 0;
    lg->lines[lg->current_line].len = 0;
    lg->lines[lg->current_line].start_head = 0;
    lg->lines[lg->current_line].start_len = 0;
    lg->lines[lg->current_line].speaker_id = LINE_SPEAKER_UNKNOWN;
    lg->lines[lg->current_line].prefix_len = 0;
    lg->lines[lg->current_line].prefix_width = 0;
}

// Distinguishable at a glance and legible on both light and dark backgrounds,
// which matters because the caption window can be made transparent over anything
static const char *speaker_colors[] = {
    "#62a0ea", // blue
    "#8ff0a4", // green
    "#f9f06b", // yellow
    "#ffbe6f", // orange
    "#dc8add", // purple
    "#99c1f1", // light blue
    "#f66151", // red
    "#cdab8f", // brown
};

const char *line_generator_speaker_color(int32_t speaker_id) {
    if(speaker_id < 0) return "#ffffff";

    size_t count = sizeof(speaker_colors) / sizeof(speaker_colors[0]);
    return speaker_colors[(size_t)speaker_id % count];
}

// Writes "Name: " at the front of a line and freezes it in place
static void write_speaker_prefix(struct line_generator *lg,
                                 struct line *curr,
                                 int32_t speaker_id,
                                 const char *name)
{
    // The name comes from the user, so it has to be escaped before going
    // anywhere near Pango markup
    char *escaped = g_markup_escape_text(name, -1);

    // Wrapped in a link so the name can be clicked to change it. Pango would
    // otherwise paint it in the theme's link colour with an underline, which
    // would lose the per-speaker colour, so the inner span overrides both.
    curr->head = sprintf(curr->text,
                         "<a href=\"" LINE_SPEAKER_URI_PREFIX "%d\">"
                         "<span foreground=\"%s\" weight=\"bold\" underline=\"none\">%s:</span>"
                         "</a> ",
                         speaker_id, line_generator_speaker_color(speaker_id), escaped);

    g_free(escaped);

    // Freeze the prefix: line_generator_update rewrites everything from
    // start_head onwards on every token, and would otherwise erase it
    curr->start_head = curr->head;
    curr->prefix_len = curr->head;
    curr->speaker_id = speaker_id;

    // The width has to count the prefix as it appears on screen, not as markup,
    // or wrapping will run the first line past the edge of the window. The
    // layout is not available until the first recognition result has arrived.
    curr->prefix_width = 0;

    if(lg->layout != NULL) {
        char plain[LINE_SPEAKER_NAME_MAX + 4];
        g_snprintf(plain, sizeof(plain), "%s: ", name);

        curr->prefix_width = line_generator_get_text_width(lg, plain);
    }

    curr->len = curr->prefix_width;
    curr->start_len = curr->len;
}


bool line_generator_rename_speaker(struct line_generator *lg,
                                   int32_t speaker_id,
                                   const char *name)
{
    if((speaker_id == LINE_SPEAKER_UNKNOWN) || (name == NULL) || (name[0] == '\0')) return false;

    char *escaped = g_markup_escape_text(name, -1);

    char prefix[AC_LINE_MAX];
    int prefix_len = g_snprintf(prefix, sizeof(prefix),
                                "<a href=\"" LINE_SPEAKER_URI_PREFIX "%d\">"
                                "<span foreground=\"%s\" weight=\"bold\" underline=\"none\">%s:</span>"
                                "</a> ",
                                speaker_id, line_generator_speaker_color(speaker_id), escaped);

    g_free(escaped);

    if((prefix_len <= 0) || (prefix_len >= AC_LINE_MAX)) return false;

    size_t width = 0;
    if(lg->layout != NULL) {
        char plain[LINE_SPEAKER_NAME_MAX + 4];
        g_snprintf(plain, sizeof(plain), "%s: ", name);
        width = line_generator_get_text_width(lg, plain);
    }

    bool changed = false;

    for(size_t i=0; i<AC_LINE_COUNT; i++){
        struct line *curr = &lg->lines[i];

        if((curr->speaker_id != speaker_id) || (curr->prefix_len == 0)) continue;

        size_t tail = curr->head - curr->prefix_len;

        // Refuse rather than truncate somebody's words to fit a longer name
        if(((size_t)prefix_len + tail) >= AC_LINE_MAX) continue;

        // Shift what was said along to make room for the new name
        memmove(&curr->text[prefix_len], &curr->text[curr->prefix_len], tail + 1);
        memcpy(curr->text, prefix, (size_t)prefix_len);

        ssize_t byte_delta = (ssize_t)prefix_len - (ssize_t)curr->prefix_len;
        ssize_t width_delta = (ssize_t)width - (ssize_t)curr->prefix_width;

        curr->head = (size_t)((ssize_t)curr->head + byte_delta);
        curr->start_head = (size_t)((ssize_t)curr->start_head + byte_delta);
        curr->len = (size_t)((ssize_t)curr->len + width_delta);
        curr->start_len = (size_t)((ssize_t)curr->start_len + width_delta);

        curr->prefix_len = (size_t)prefix_len;
        curr->prefix_width = width;

        changed = true;
    }

    return changed;
}

void line_generator_set_speaker(struct line_generator *lg,
                                int32_t speaker_id,
                                const char *name)
{
    if(speaker_id == lg->current_speaker_id) return;

    lg->current_speaker_id = speaker_id;

    // A change of speaker always starts a new line, named or not.
    //
    // This works just as well part way through an utterance as at its start:
    // the break points the active token array at the new line, so the words
    // recognised so far move across and reflow after the name. That matters
    // because the diarizer usually needs about a second of speech before it can
    // say who is talking, by which time the utterance is already on screen.
    line_generator_break(lg);

    if((speaker_id == LINE_SPEAKER_UNKNOWN) || (name == NULL) || (name[0] == '\0')) return;

    write_speaker_prefix(lg, &lg->lines[lg->current_line], speaker_id, name);
}

void line_generator_set_text(struct line_generator *lg, GtkLabel *lbl) {
    char *head = &lg->output[0];
    *head = '\0';

    int display_count = g_settings_get_int(settings, "num-lines");
    for(int i=display_count-1; i>=0; i--) {
        struct line *curr = &lg->lines[REL_LINE_IDX(lg->current_line, -i)];
        head += sprintf(head, "%s", curr->text);

        if(i != 0) head += sprintf(head, "\n");
    }

    gtk_label_set_markup(lbl, lg->output);
}

void line_generator_set_language(struct line_generator *lg, const char* language) {
    lg->is_english = (language[0] == 'e') && (language[1] == 'n');
    lg->tcap.is_english = lg->is_english;
}

const char *line_generator_get_plaintext(struct line_generator *lg) {
    char *head = &lg->plaintext[0];
    *head = '\0';

    int display_count = g_settings_get_int(settings, "num-lines");
    for(int i=display_count-1; i>=0; i--) {
        struct line *curr = &lg->lines[REL_LINE_IDX(lg->current_line, -i)];

        // HACK: We need to remove the <span...></span> tags
        //
        // Done unconditionally rather than only when text fading is on: a
        // speaker name is also written as markup, so checking that one setting
        // is no longer enough to know whether the line contains tags.
        bool inside_markup = false;
        for(int j=0; j<curr->head; j++) {
            if(curr->text[j] == '<') {
                inside_markup = true;
                continue;
            }else if(curr->text[j] == '>') {
                inside_markup = false;
                continue;
            }else if(inside_markup) {
                continue;
            }

            head += sprintf(head, "%c", curr->text[j]);
        }

        if(i != 0) head += sprintf(head, "\n");
    }

    return &lg->plaintext[0];
}