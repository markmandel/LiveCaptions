/* spk-fbank.c
 * Kaldi-compatible log-mel filterbank features for the speaker embedding model.
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

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "spk-fbank.h"

// Kaldi defaults for these options
#define FBANK_LOW_FREQ 20.0f
#define FBANK_PREEMPH 0.97f
#define FBANK_POVEY_EXPONENT 0.85

// One triangular mel filter, stored sparsely: Kaldi filters only touch a narrow
// run of FFT bins, so keeping the offset and the run beats a full-width row
struct mel_filter {
    int offset;      // first FFT bin this filter touches
    int count;       // how many bins
    float *weights;  // `count` weights
};

struct spk_fbank_i {
    int sample_rate;
    int frame_length;  // samples per analysis window
    int frame_shift;   // samples between windows
    int fft_size;      // frame_length rounded up to a power of two
    int num_fft_bins;  // fft_size / 2

    float *window;     // Povey window, frame_length long

    struct mel_filter filters[SPK_FBANK_NUM_BINS];
    float *filter_storage;

    // Scratch, sized for one frame
    float *real;
    float *imag;
    float *power;

    // Bit-reversal permutation and twiddles for the FFT
    int *bit_reverse;
    float *cos_table;
    float *sin_table;
};


static float mel_scale(float freq) {
    return 1127.0f * logf(1.0f + freq / 700.0f);
}

static int next_power_of_two(int n) {
    int p = 1;
    while(p < n) p <<= 1;
    return p;
}


// -- FFT ------------------------------------------------------------------
// Iterative radix-2 Cooley-Tukey. The frames are 512 points at 100 frames per
// second, so this costs well under a percent of the embedding model's own work
// and is not worth pulling in a dependency for.

static void fft_init(spk_fbank fb) {
    int n = fb->fft_size;

    fb->bit_reverse = calloc(n, sizeof(int));
    fb->cos_table = calloc(n / 2, sizeof(float));
    fb->sin_table = calloc(n / 2, sizeof(float));

    int bits = 0;
    while((1 << bits) < n) bits++;

    for(int i=0; i<n; i++){
        int reversed = 0;
        for(int b=0; b<bits; b++){
            if(i & (1 << b)) reversed |= 1 << (bits - 1 - b);
        }
        fb->bit_reverse[i] = reversed;
    }

    for(int i=0; i<n/2; i++){
        double angle = -2.0 * M_PI * (double)i / (double)n;
        fb->cos_table[i] = (float)cos(angle);
        fb->sin_table[i] = (float)sin(angle);
    }
}

static void fft_run(spk_fbank fb) {
    int n = fb->fft_size;
    float *re = fb->real;
    float *im = fb->imag;

    for(int i=0; i<n; i++){
        int j = fb->bit_reverse[i];
        if(j > i) {
            float tr = re[i]; re[i] = re[j]; re[j] = tr;
            float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }

    for(int len=2; len<=n; len <<= 1){
        int half = len / 2;
        int step = n / len;

        for(int i=0; i<n; i += len){
            for(int j=0; j<half; j++){
                float wr = fb->cos_table[j * step];
                float wi = fb->sin_table[j * step];

                int a = i + j;
                int b = i + j + half;

                float br = re[b] * wr - im[b] * wi;
                float bi = re[b] * wi + im[b] * wr;

                re[b] = re[a] - br;
                im[b] = im[a] - bi;
                re[a] += br;
                im[a] += bi;
            }
        }
    }
}


// -- Mel filterbank -------------------------------------------------------

static void build_mel_filters(spk_fbank fb) {
    float nyquist = (float)fb->sample_rate * 0.5f;
    float low_freq = FBANK_LOW_FREQ;
    float high_freq = nyquist;

    float mel_low = mel_scale(low_freq);
    float mel_high = mel_scale(high_freq);
    float mel_delta = (mel_high - mel_low) / (float)(SPK_FBANK_NUM_BINS + 1);

    float fft_bin_width = (float)fb->sample_rate / (float)fb->fft_size;

    // Worst case every filter spans every bin; allocated once and carved up
    fb->filter_storage = calloc((size_t)SPK_FBANK_NUM_BINS * (size_t)fb->num_fft_bins,
                                sizeof(float));

    float *cursor = fb->filter_storage;

    for(int bin=0; bin<SPK_FBANK_NUM_BINS; bin++){
        float left_mel = mel_low + (float)bin * mel_delta;
        float center_mel = mel_low + (float)(bin + 1) * mel_delta;
        float right_mel = mel_low + (float)(bin + 2) * mel_delta;

        int first = -1;
        int last = -1;

        // Two passes: find the span, then fill it, so the sparse rows stay tight
        for(int i=0; i<fb->num_fft_bins; i++){
            float mel = mel_scale(fft_bin_width * (float)i);
            if((mel > left_mel) && (mel < right_mel)) {
                if(first < 0) first = i;
                last = i;
            }
        }

        if(first < 0) {
            fb->filters[bin].offset = 0;
            fb->filters[bin].count = 0;
            fb->filters[bin].weights = cursor;
            continue;
        }

        fb->filters[bin].offset = first;
        fb->filters[bin].count = (last - first) + 1;
        fb->filters[bin].weights = cursor;

        for(int i=first; i<=last; i++){
            float mel = mel_scale(fft_bin_width * (float)i);
            float weight;

            if(mel <= center_mel) weight = (mel - left_mel) / (center_mel - left_mel);
            else weight = (right_mel - mel) / (right_mel - center_mel);

            cursor[i - first] = weight;
        }

        cursor += fb->filters[bin].count;
    }
}


spk_fbank spk_fbank_create(int sample_rate) {
    if(sample_rate <= 0) return NULL;

    spk_fbank fb = calloc(1, sizeof(struct spk_fbank_i));
    if(fb == NULL) return NULL;

    fb->sample_rate = sample_rate;
    fb->frame_length = (sample_rate * SPK_FBANK_FRAME_LENGTH_MS) / 1000;
    fb->frame_shift = (sample_rate * SPK_FBANK_FRAME_SHIFT_MS) / 1000;
    fb->fft_size = next_power_of_two(fb->frame_length);
    fb->num_fft_bins = fb->fft_size / 2;

    fb->window = calloc(fb->frame_length, sizeof(float));
    for(int i=0; i<fb->frame_length; i++){
        // Povey window: a Hann window raised to 0.85
        double hann = 0.5 - 0.5 * cos(2.0 * M_PI * (double)i / (double)(fb->frame_length - 1));
        fb->window[i] = (float)pow(hann, FBANK_POVEY_EXPONENT);
    }

    fb->real = calloc(fb->fft_size, sizeof(float));
    fb->imag = calloc(fb->fft_size, sizeof(float));
    fb->power = calloc(fb->num_fft_bins, sizeof(float));

    fft_init(fb);
    build_mel_filters(fb);

    return fb;
}


void spk_fbank_free(spk_fbank fb) {
    if(fb == NULL) return;

    free(fb->window);
    free(fb->filter_storage);
    free(fb->real);
    free(fb->imag);
    free(fb->power);
    free(fb->bit_reverse);
    free(fb->cos_table);
    free(fb->sin_table);
    free(fb);
}


size_t spk_fbank_num_frames(spk_fbank fb, size_t num_samples) {
    if((fb == NULL) || (num_samples < (size_t)fb->frame_length)) return 0;

    return 1 + ((num_samples - (size_t)fb->frame_length) / (size_t)fb->frame_shift);
}


size_t spk_fbank_compute(spk_fbank fb,
                         const float *samples,
                         size_t num_samples,
                         float *out,
                         size_t max_frames)
{
    if((fb == NULL) || (samples == NULL) || (out == NULL)) return 0;

    size_t num_frames = spk_fbank_num_frames(fb, num_samples);
    if(num_frames > max_frames) num_frames = max_frames;

    for(size_t frame=0; frame<num_frames; frame++){
        const float *src = &samples[frame * (size_t)fb->frame_shift];

        // Kaldi removes the DC offset per frame before anything else
        float mean = 0.0f;
        for(int i=0; i<fb->frame_length; i++) mean += src[i];
        mean /= (float)fb->frame_length;

        for(int i=0; i<fb->frame_length; i++) fb->real[i] = src[i] - mean;

        // Pre-emphasis, walked backwards so each sample still sees its
        // untouched predecessor. Kaldi treats x[-1] as equal to x[0].
        for(int i=fb->frame_length-1; i>0; i--){
            fb->real[i] -= FBANK_PREEMPH * fb->real[i-1];
        }
        fb->real[0] -= FBANK_PREEMPH * fb->real[0];

        for(int i=0; i<fb->frame_length; i++) fb->real[i] *= fb->window[i];

        memset(&fb->real[fb->frame_length], 0,
               (size_t)(fb->fft_size - fb->frame_length) * sizeof(float));
        memset(fb->imag, 0, (size_t)fb->fft_size * sizeof(float));

        fft_run(fb);

        for(int i=0; i<fb->num_fft_bins; i++){
            fb->power[i] = fb->real[i] * fb->real[i] + fb->imag[i] * fb->imag[i];
        }

        float *row = &out[frame * SPK_FBANK_NUM_BINS];

        for(int bin=0; bin<SPK_FBANK_NUM_BINS; bin++){
            const struct mel_filter *filter = &fb->filters[bin];

            float energy = 0.0f;
            for(int i=0; i<filter->count; i++){
                energy += filter->weights[i] * fb->power[filter->offset + i];
            }

            // Kaldi floors at FLT_EPSILON before taking the log, so silence
            // produces a large negative number rather than -inf
            if(energy < FLT_EPSILON) energy = FLT_EPSILON;

            row[bin] = logf(energy);
        }
    }

    return num_frames;
}


void spk_fbank_apply_cmn(float *feats, size_t num_frames, size_t num_bins) {
    if((feats == NULL) || (num_frames == 0)) return;

    for(size_t bin=0; bin<num_bins; bin++){
        double sum = 0.0;
        for(size_t frame=0; frame<num_frames; frame++){
            sum += feats[frame * num_bins + bin];
        }

        float mean = (float)(sum / (double)num_frames);

        for(size_t frame=0; frame<num_frames; frame++){
            feats[frame * num_bins + bin] -= mean;
        }
    }
}
