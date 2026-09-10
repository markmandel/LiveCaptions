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

    printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
