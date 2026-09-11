/* line-gen-test.c
 * Exercises line_generator the way asrproc drives it, with no display involved,
 * so speaker labelling can be checked without launching the interface.
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
#include <string.h>
#include <pango/pangocairo.h>

#include "line-gen.h"

static AprilToken make_token(const char *text) {
    AprilToken t;
    memset(&t, 0, sizeof(t));
    t.token = text;
    t.logprob = -0.2f;
    t.flags = (text[0] == ' ') ? APRIL_TOKEN_FLAG_WORD_BOUNDARY_BIT : 0;
    return t;
}

static int failures = 0;

static void check_no_duplicate(const char *what, const char *text, const char *phrase) {
    const char *first = strstr(text, phrase);
    const char *second = first ? strstr(first + 1, phrase) : NULL;

    if(second != NULL) {
        printf("  FAIL %s: \"%s\" appears more than once\n", what, phrase);
        failures++;
    } else if(first == NULL) {
        printf("  FAIL %s: \"%s\" is missing entirely\n", what, phrase);
        failures++;
    } else {
        printf("  ok   %s: \"%s\" appears exactly once\n", what, phrase);
    }
}

static void check_contains(const char *what, const char *text, const char *phrase) {
    if(strstr(text, phrase) == NULL) {
        printf("  FAIL %s: expected to find \"%s\"\n", what, phrase);
        failures++;
    } else {
        printf("  ok   %s: found \"%s\"\n", what, phrase);
    }
}

int main(void) {
    // A font map gives a usable PangoLayout with no windowing system at all
    PangoFontMap *font_map = pango_cairo_font_map_get_default();
    PangoContext *context = pango_font_map_create_context(font_map);

    struct line_generator lg;
    line_generator_init(&lg);

    lg.layout = pango_layout_new(context);
    lg.max_text_width = 100000; // wide, so nothing wraps and the test stays readable
    lg.is_english = true;

    AprilToken tokens[8];
    tokens[0] = make_token(" Well");
    tokens[1] = make_token(",");
    tokens[2] = make_token(" first");
    tokens[3] = make_token(" of");
    tokens[4] = make_token(" all");
    tokens[5] = make_token(" welcome");
    tokens[6] = make_token(" everybody");

    printf("case 1: label arrives part way through an utterance\n");

    // Partial results arrive, still unattributed
    line_generator_update(&lg, 3, tokens);
    line_generator_update(&lg, 5, tokens);

    // The diarizer works out who it is, one second in
    line_generator_set_speaker(&lg, 0, "Speaker 1");

    // More of the same utterance arrives
    line_generator_update(&lg, 7, tokens);

    const char *out = line_generator_get_plaintext(&lg);
    printf("--- plaintext ---\n%s\n-----------------\n", out);

    check_no_duplicate("mid-utterance label", out, "first of all");
    check_contains("mid-utterance label", out, "Speaker 1:");

    printf("\ncase 2: same speaker again must not repeat the name\n");
    line_generator_finalize(&lg);
    line_generator_set_speaker(&lg, 0, "Speaker 1");
    line_generator_update(&lg, 3, tokens);

    out = line_generator_get_plaintext(&lg);
    check_no_duplicate("repeat speaker", out, "Speaker 1:");

    printf("\ncase 3: a different speaker does get named\n");
    line_generator_finalize(&lg);
    line_generator_set_speaker(&lg, 1, "Speaker 2");
    line_generator_update(&lg, 3, tokens);

    out = line_generator_get_plaintext(&lg);
    printf("--- plaintext ---\n%s\n-----------------\n", out);
    check_contains("speaker change", out, "Speaker 2:");

    printf("\ncase 4: renaming rewrites captions already on screen\n");
    {
        struct line_generator lg2;
        line_generator_init(&lg2);
        lg2.layout = pango_layout_new(context);
        lg2.max_text_width = 100000;
        lg2.is_english = true;

        line_generator_set_speaker(&lg2, 0, "Speaker 1");
        line_generator_update(&lg2, 7, tokens);
        line_generator_finalize(&lg2);

        // A longer name has to push the words along without losing any
        check_contains("before rename", line_generator_get_plaintext(&lg2), "Speaker 1:");

        bool changed = line_generator_rename_speaker(&lg2, 0, "Alexandra");
        check_contains("rename reported a change", changed ? "yes" : "", "yes");

        const char *after = line_generator_get_plaintext(&lg2);
        printf("--- plaintext ---\n%s\n-----------------\n", after);

        check_contains("the new name is shown", after, "Alexandra:");
        if(strstr(after, "Speaker 1:") != NULL) {
            printf("  FAIL long rename: the old name is still there\n");
            failures++;
        } else {
            printf("  ok   long rename: the old name is gone\n");
        }
        check_no_duplicate("long rename", after, "welcome");

        // And a shorter one has to pull them back without truncating anything
        line_generator_rename_speaker(&lg2, 0, "Jo");
        after = line_generator_get_plaintext(&lg2);

        check_contains("the shorter name is shown", after, "Jo:");
        check_no_duplicate("short rename", after, "welcome");
        check_contains("the words survived both renames", after, "first of all");

        // The window hit-tests clicks against these, so they have to describe
        // where the name actually landed in the text Pango lays out
        const char *plain = line_generator_get_plaintext(&lg2);

        struct line_speaker_span spans[AC_LINE_COUNT];
        size_t num_spans = line_generator_get_speaker_spans(&lg2, spans, G_N_ELEMENTS(spans));

        check_contains("a span was recorded", num_spans > 0 ? "yes" : "", "yes");

        if(num_spans > 0) {
            bool sane = (spans[0].name_end > spans[0].line_start)
                     && (spans[0].name_end <= strlen(plain))
                     && (spans[0].speaker_id == 0);
            check_contains("the span is within the text", sane ? "yes" : "", "yes");

            // "Jo: " is what the span should cover
            char covered[64] = { 0 };
            size_t len = spans[0].name_end - spans[0].line_start;
            if(len < sizeof(covered)) memcpy(covered, plain + spans[0].line_start, len);

            printf("  span covers: \"%s\"\n", covered);
            check_contains("the span covers the name", covered, "Jo:");
        }

        if(strstr(plain, "span ") != NULL) {
            printf("  FAIL markup leaks into the plain text stream\n");
            failures++;
        } else {
            printf("  ok   markup is stripped from the plain text stream\n");
        }
    }

    printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
