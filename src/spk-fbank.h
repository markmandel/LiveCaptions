/* spk-fbank.h
 * Kaldi-compatible log-mel filterbank features for the speaker embedding model.
 *
 * Deliberately separate from april-asr's own fbank: that one is internal to the
 * submodule, is an online/streaming extractor, and its common.h collides with
 * ours on the include path. The speaker model wants whole segments at once, so
 * a small batch extractor is both simpler and independently testable.
 *
 * The defaults match what WeSpeaker trains with, which is torchaudio's
 * kaldi.fbank at 25ms/10ms, 80 mel bins, Povey window, no dither, and samples
 * kept at int16 scale rather than normalized to [-1, 1].
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

#define SPK_FBANK_NUM_BINS 80
#define SPK_FBANK_FRAME_LENGTH_MS 25
#define SPK_FBANK_FRAME_SHIFT_MS 10

struct spk_fbank_i;
typedef struct spk_fbank_i * spk_fbank;

spk_fbank spk_fbank_create(int sample_rate);
void spk_fbank_free(spk_fbank fb);

// How many frames spk_fbank_compute will produce for this many samples.
// Matches Kaldi's snip_edges=true: frames never run past the end of the audio.
size_t spk_fbank_num_frames(spk_fbank fb, size_t num_samples);

// Computes features into `out`, which must hold
// num_frames * SPK_FBANK_NUM_BINS floats. `samples` are at int16 scale, that is
// roughly -32768..32767, not normalized. Returns the number of frames written.
size_t spk_fbank_compute(spk_fbank fb,
                         const float *samples,
                         size_t num_samples,
                         float *out,
                         size_t max_frames);

// Subtracts the per-bin mean over time, in place. WeSpeaker applies this
// cepstral mean normalization to every utterance before the network sees it.
void spk_fbank_apply_cmn(float *feats, size_t num_frames, size_t num_bins);
