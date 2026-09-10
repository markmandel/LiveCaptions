/* spk-embed.c
 * Wraps the speaker embedding network (WeSpeaker CAM++).
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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "onnxruntime_c_api.h"
#include "spk-embed.h"
#include "spk-fbank.h"

// Below this there is not enough signal for a stable embedding
#define SPK_EMBED_MIN_FRAMES 25

// Ceiling on how much audio goes into one embedding. Longer windows do not help
// and would make the buffers unbounded.
#define SPK_EMBED_MAX_FRAMES 1600

struct spk_embed_i {
    const OrtApi *ort;
    OrtEnv *env;
    OrtSessionOptions *options;
    OrtSession *session;
    OrtMemoryInfo *memory_info;
    OrtAllocator *allocator;

    char *input_name;
    char *output_name;

    int dim;
    int sample_rate;

    spk_fbank fbank;

    float *feats;   // SPK_EMBED_MAX_FRAMES * SPK_FBANK_NUM_BINS
};


static bool ort_failed(spk_embed e, OrtStatus *status, const char *what) {
    if(status == NULL) return false;

    fprintf(stderr, "Speaker embedding: %s failed: %s\n",
            what, e->ort->GetErrorMessage(status));
    e->ort->ReleaseStatus(status);
    return true;
}


spk_embed spk_embed_create(const char *model_path, int sample_rate) {
    if((model_path == NULL) || (sample_rate <= 0)) return NULL;

    spk_embed e = calloc(1, sizeof(struct spk_embed_i));
    if(e == NULL) return NULL;

    e->sample_rate = sample_rate;
    e->ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);

    if(e->ort == NULL) {
        fprintf(stderr, "Speaker embedding: ONNX Runtime is unavailable\n");
        free(e);
        return NULL;
    }

    if(ort_failed(e, e->ort->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "livecaptions-spk", &e->env), "CreateEnv")) {
        free(e);
        return NULL;
    }

    if(ort_failed(e, e->ort->CreateSessionOptions(&e->options), "CreateSessionOptions")) {
        spk_embed_free(e);
        return NULL;
    }

    // One thread on purpose. This runs alongside realtime ASR, and letting ONNX
    // Runtime default to one thread per core would fight the recognizer for CPU
    // on exactly the machines that can least afford it.
    e->ort->SetIntraOpNumThreads(e->options, 1);
    e->ort->SetInterOpNumThreads(e->options, 1);
    e->ort->SetSessionGraphOptimizationLevel(e->options, ORT_ENABLE_ALL);

    if(ort_failed(e, e->ort->CreateSession(e->env, model_path, e->options, &e->session), "CreateSession")) {
        fprintf(stderr, "Speaker embedding: could not load model %s\n", model_path);
        spk_embed_free(e);
        return NULL;
    }

    if(ort_failed(e, e->ort->GetAllocatorWithDefaultOptions(&e->allocator), "GetAllocator")) {
        spk_embed_free(e);
        return NULL;
    }

    // Names are read from the graph rather than hardcoded: the models in this
    // family disagree about them, and about the embedding width
    if(ort_failed(e, e->ort->SessionGetInputName(e->session, 0, e->allocator, &e->input_name), "GetInputName")
        || ort_failed(e, e->ort->SessionGetOutputName(e->session, 0, e->allocator, &e->output_name), "GetOutputName")) {
        spk_embed_free(e);
        return NULL;
    }

    OrtTypeInfo *type_info = NULL;
    if(ort_failed(e, e->ort->SessionGetOutputTypeInfo(e->session, 0, &type_info), "GetOutputTypeInfo")) {
        spk_embed_free(e);
        return NULL;
    }

    const OrtTensorTypeAndShapeInfo *shape_info = NULL;
    e->ort->CastTypeInfoToTensorInfo(type_info, &shape_info);

    size_t num_dims = 0;
    e->ort->GetDimensionsCount(shape_info, &num_dims);

    int64_t dims[4] = { 0 };
    if(num_dims > 4) num_dims = 4;
    e->ort->GetDimensions(shape_info, dims, num_dims);

    // Shape is (batch, dim); batch is symbolic so only the last axis is useful
    e->dim = (num_dims >= 2) ? (int)dims[num_dims - 1] : 0;
    e->ort->ReleaseTypeInfo(type_info);

    if((e->dim <= 0) || (e->dim > SPK_EMBED_MAX_DIM)) {
        fprintf(stderr, "Speaker embedding: model reports an unusable embedding size %d\n", e->dim);
        spk_embed_free(e);
        return NULL;
    }

    if(ort_failed(e, e->ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &e->memory_info), "CreateCpuMemoryInfo")) {
        spk_embed_free(e);
        return NULL;
    }

    e->fbank = spk_fbank_create(sample_rate);
    e->feats = calloc((size_t)SPK_EMBED_MAX_FRAMES * SPK_FBANK_NUM_BINS, sizeof(float));

    if((e->fbank == NULL) || (e->feats == NULL)) {
        spk_embed_free(e);
        return NULL;
    }

    printf("Speaker embedding: loaded %s (%d-dim)\n", model_path, e->dim);

    return e;
}


void spk_embed_free(spk_embed e) {
    if(e == NULL) return;

    if(e->input_name != NULL) e->ort->AllocatorFree(e->allocator, e->input_name);
    if(e->output_name != NULL) e->ort->AllocatorFree(e->allocator, e->output_name);

    if(e->memory_info != NULL) e->ort->ReleaseMemoryInfo(e->memory_info);
    if(e->session != NULL) e->ort->ReleaseSession(e->session);
    if(e->options != NULL) e->ort->ReleaseSessionOptions(e->options);
    if(e->env != NULL) e->ort->ReleaseEnv(e->env);

    spk_fbank_free(e->fbank);
    free(e->feats);
    free(e);
}


int spk_embed_dim(spk_embed e) {
    if(e == NULL) return 0;
    return e->dim;
}


bool spk_embed_compute(spk_embed e,
                       const float *samples,
                       size_t num_samples,
                       float *out)
{
    if((e == NULL) || (samples == NULL) || (out == NULL)) return false;

    size_t num_frames = spk_fbank_compute(e->fbank, samples, num_samples,
                                          e->feats, SPK_EMBED_MAX_FRAMES);

    if(num_frames < SPK_EMBED_MIN_FRAMES) return false;

    spk_fbank_apply_cmn(e->feats, num_frames, SPK_FBANK_NUM_BINS);

    int64_t shape[3] = { 1, (int64_t)num_frames, SPK_FBANK_NUM_BINS };

    OrtValue *input = NULL;
    if(ort_failed(e, e->ort->CreateTensorWithDataAsOrtValue(
            e->memory_info,
            e->feats,
            num_frames * SPK_FBANK_NUM_BINS * sizeof(float),
            shape, 3,
            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
            &input), "CreateTensor")) {
        return false;
    }

    const char *input_names[1] = { e->input_name };
    const char *output_names[1] = { e->output_name };
    OrtValue *output = NULL;

    bool failed = ort_failed(e, e->ort->Run(e->session, NULL,
                                            input_names, (const OrtValue *const *)&input, 1,
                                            output_names, 1, &output), "Run");

    e->ort->ReleaseValue(input);

    if(failed || (output == NULL)) return false;

    float *raw = NULL;
    if(ort_failed(e, e->ort->GetTensorMutableData(output, (void **)&raw), "GetTensorData")) {
        e->ort->ReleaseValue(output);
        return false;
    }

    // Normalize so that a dot product between two embeddings is their cosine
    double sum_squares = 0.0;
    for(int i=0; i<e->dim; i++) sum_squares += (double)raw[i] * (double)raw[i];

    float norm = (float)sqrt(sum_squares);
    if(norm < 1e-9f) {
        e->ort->ReleaseValue(output);
        return false;
    }

    for(int i=0; i<e->dim; i++) out[i] = raw[i] / norm;

    e->ort->ReleaseValue(output);

    return true;
}


float spk_embed_similarity(const float *a, const float *b, int dim) {
    if((a == NULL) || (b == NULL)) return -1.0f;

    float sum = 0.0f;
    for(int i=0; i<dim; i++) sum += a[i] * b[i];

    return sum;
}
