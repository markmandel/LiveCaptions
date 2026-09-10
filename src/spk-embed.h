/* spk-embed.h
 * Wraps the speaker embedding network (WeSpeaker CAM++) and turns a stretch of
 * speech into a fixed-size vector that can be compared against other speakers.
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

// Generous upper bound so callers can put an embedding on the stack. CAM++
// produces 512; the other models in the same family range from 192 to 512.
#define SPK_EMBED_MAX_DIM 512

struct spk_embed_i;
typedef struct spk_embed_i * spk_embed;

// Returns NULL if the model cannot be loaded, which is not fatal: the caller
// should carry on with diarization disabled.
spk_embed spk_embed_create(const char *model_path, int sample_rate);
void spk_embed_free(spk_embed e);

// Size of the vectors this model produces
int spk_embed_dim(spk_embed e);

// Turns speech into an L2-normalized embedding, so that a dot product between
// two of them is their cosine similarity.
//
// `samples` are at int16 scale (roughly -32768..32767), matching the model's
// normalize_samples=0 metadata. `out` must hold spk_embed_dim() floats.
// Returns false if there is too little audio to be worth embedding.
bool spk_embed_compute(spk_embed e,
                       const float *samples,
                       size_t num_samples,
                       float *out);

// Cosine similarity between two normalized embeddings, in [-1, 1]
float spk_embed_similarity(const float *a, const float *b, int dim);
