/* spk-vad.c
 * Wraps Silero VAD.
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
#include <stdlib.h>
#include <string.h>

#include "onnxruntime_c_api.h"
#include "spk-vad.h"

// v5 carries its two recurrent states in one (2, batch, 128) tensor
#define SPK_VAD_STATE_DIM 128
#define SPK_VAD_STATE_SIZE (2 * SPK_VAD_STATE_DIM)

struct spk_vad_i {
    const OrtApi *ort;
    OrtEnv *env;
    OrtSessionOptions *options;
    OrtSession *session;
    OrtMemoryInfo *memory_info;
    OrtAllocator *allocator;

    char *input_names[3];
    char *output_names[2];
    size_t num_inputs;
    size_t num_outputs;

    int sample_rate;
    size_t frame_samples;

    float state[SPK_VAD_STATE_SIZE];
    int64_t sample_rate_tensor;
};


static bool ort_failed(spk_vad v, OrtStatus *status, const char *what) {
    if(status == NULL) return false;

    fprintf(stderr, "VAD: %s failed: %s\n", what, v->ort->GetErrorMessage(status));
    v->ort->ReleaseStatus(status);
    return true;
}


spk_vad spk_vad_create(const char *model_path, int sample_rate) {
    if((model_path == NULL) || (sample_rate <= 0)) return NULL;

    spk_vad v = calloc(1, sizeof(struct spk_vad_i));
    if(v == NULL) return NULL;

    v->sample_rate = sample_rate;
    v->sample_rate_tensor = sample_rate;

    // Silero's frame size is defined at 16 kHz; scale it if the model ever runs
    // at another rate so the frame still covers the same span of time
    v->frame_samples = (size_t)((SPK_VAD_FRAME_SAMPLES * (long)sample_rate) / 16000);

    v->ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if(v->ort == NULL) {
        fprintf(stderr, "VAD: ONNX Runtime is unavailable\n");
        free(v);
        return NULL;
    }

    if(ort_failed(v, v->ort->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "livecaptions-vad", &v->env), "CreateEnv")) {
        free(v);
        return NULL;
    }

    if(ort_failed(v, v->ort->CreateSessionOptions(&v->options), "CreateSessionOptions")) {
        spk_vad_free(v);
        return NULL;
    }

    // Tiny model, and it runs 31 times a second next to realtime ASR. One
    // thread avoids paying more in scheduling than the inference itself costs.
    v->ort->SetIntraOpNumThreads(v->options, 1);
    v->ort->SetInterOpNumThreads(v->options, 1);

    if(ort_failed(v, v->ort->CreateSession(v->env, model_path, v->options, &v->session), "CreateSession")) {
        fprintf(stderr, "VAD: could not load model %s\n", model_path);
        spk_vad_free(v);
        return NULL;
    }

    if(ort_failed(v, v->ort->GetAllocatorWithDefaultOptions(&v->allocator), "GetAllocator")) {
        spk_vad_free(v);
        return NULL;
    }

    if(ort_failed(v, v->ort->SessionGetInputCount(v->session, &v->num_inputs), "GetInputCount")
        || ort_failed(v, v->ort->SessionGetOutputCount(v->session, &v->num_outputs), "GetOutputCount")) {
        spk_vad_free(v);
        return NULL;
    }

    // v4 splits the recurrent state into h and c and takes 1536-sample frames,
    // v5 combines them and takes 512. Only v5's three-input shape is supported;
    // anything else is rejected rather than silently mis-driven.
    if((v->num_inputs != 3) || (v->num_outputs != 2)) {
        fprintf(stderr, "VAD: %s has %zu inputs and %zu outputs, expected 3 and 2 "
                        "(is it a Silero v5 model?)\n",
                model_path, v->num_inputs, v->num_outputs);
        spk_vad_free(v);
        return NULL;
    }

    for(size_t i=0; i<v->num_inputs; i++){
        if(ort_failed(v, v->ort->SessionGetInputName(v->session, i, v->allocator, &v->input_names[i]), "GetInputName")) {
            spk_vad_free(v);
            return NULL;
        }
    }

    for(size_t i=0; i<v->num_outputs; i++){
        if(ort_failed(v, v->ort->SessionGetOutputName(v->session, i, v->allocator, &v->output_names[i]), "GetOutputName")) {
            spk_vad_free(v);
            return NULL;
        }
    }

    if(ort_failed(v, v->ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &v->memory_info), "CreateCpuMemoryInfo")) {
        spk_vad_free(v);
        return NULL;
    }

    printf("VAD: loaded %s (%zu-sample frames)\n", model_path, v->frame_samples);

    return v;
}


void spk_vad_free(spk_vad v) {
    if(v == NULL) return;

    for(size_t i=0; i<3; i++){
        if(v->input_names[i] != NULL) v->ort->AllocatorFree(v->allocator, v->input_names[i]);
    }
    for(size_t i=0; i<2; i++){
        if(v->output_names[i] != NULL) v->ort->AllocatorFree(v->allocator, v->output_names[i]);
    }

    if(v->memory_info != NULL) v->ort->ReleaseMemoryInfo(v->memory_info);
    if(v->session != NULL) v->ort->ReleaseSession(v->session);
    if(v->options != NULL) v->ort->ReleaseSessionOptions(v->options);
    if(v->env != NULL) v->ort->ReleaseEnv(v->env);

    free(v);
}


size_t spk_vad_frame_samples(spk_vad v) {
    if(v == NULL) return SPK_VAD_FRAME_SAMPLES;
    return v->frame_samples;
}


void spk_vad_reset(spk_vad v) {
    if(v == NULL) return;
    memset(v->state, 0, sizeof(v->state));
}


float spk_vad_process(spk_vad v, const float *samples) {
    if((v == NULL) || (samples == NULL)) return -1.0f;

    int64_t input_shape[2] = { 1, (int64_t)v->frame_samples };
    int64_t state_shape[3] = { 2, 1, SPK_VAD_STATE_DIM };
    int64_t sr_shape[1] = { 1 };

    OrtValue *inputs[3] = { NULL, NULL, NULL };
    OrtValue *outputs[2] = { NULL, NULL };

    bool failed = false;

    failed = failed || ort_failed(v, v->ort->CreateTensorWithDataAsOrtValue(
        v->memory_info, (void *)samples, v->frame_samples * sizeof(float),
        input_shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputs[0]), "CreateTensor(input)");

    failed = failed || ort_failed(v, v->ort->CreateTensorWithDataAsOrtValue(
        v->memory_info, v->state, sizeof(v->state),
        state_shape, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputs[1]), "CreateTensor(state)");

    failed = failed || ort_failed(v, v->ort->CreateTensorWithDataAsOrtValue(
        v->memory_info, &v->sample_rate_tensor, sizeof(v->sample_rate_tensor),
        sr_shape, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &inputs[2]), "CreateTensor(sr)");

    if(!failed) {
        failed = ort_failed(v, v->ort->Run(v->session, NULL,
                                           (const char *const *)v->input_names,
                                           (const OrtValue *const *)inputs, 3,
                                           (const char *const *)v->output_names, 2,
                                           outputs), "Run");
    }

    float probability = -1.0f;

    if(!failed && (outputs[0] != NULL)) {
        float *raw = NULL;
        if(!ort_failed(v, v->ort->GetTensorMutableData(outputs[0], (void **)&raw), "GetTensorData")) {
            probability = raw[0];
        }

        // Carry the recurrent state into the next frame
        if(outputs[1] != NULL) {
            float *next_state = NULL;
            if(!ort_failed(v, v->ort->GetTensorMutableData(outputs[1], (void **)&next_state), "GetTensorData(state)")) {
                memcpy(v->state, next_state, sizeof(v->state));
            }
        }
    }

    for(size_t i=0; i<3; i++){
        if(inputs[i] != NULL) v->ort->ReleaseValue(inputs[i]);
    }
    for(size_t i=0; i<2; i++){
        if(outputs[i] != NULL) v->ort->ReleaseValue(outputs[i]);
    }

    return probability;
}
