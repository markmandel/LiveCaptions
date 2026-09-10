/* spk-vad.h
 * Wraps Silero VAD, which reports how likely a short frame of audio is to be
 * speech. Used to find turn boundaries far more precisely than april-asr's
 * 2.2 second silence heuristic can.
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

// Silero v5 is trained on 512-sample frames at 16 kHz, i.e. 32 ms
#define SPK_VAD_FRAME_SAMPLES 512

struct spk_vad_i;
typedef struct spk_vad_i * spk_vad;

// Returns NULL if the model cannot be loaded, which is not fatal: the caller
// should carry on with diarization disabled.
spk_vad spk_vad_create(const char *model_path, int sample_rate);
void spk_vad_free(spk_vad v);

// Frame size this model expects, in samples
size_t spk_vad_frame_samples(spk_vad v);

// Probability that one frame of exactly spk_vad_frame_samples() samples is
// speech, in [0, 1]. Samples are normalized to [-1, 1]. Returns a negative
// value if the model could not be run.
//
// The model is stateful and assumes frames arrive in order; call spk_vad_reset
// whenever the audio is not contiguous with what came before.
float spk_vad_process(spk_vad v, const float *samples);

void spk_vad_reset(spk_vad v);
