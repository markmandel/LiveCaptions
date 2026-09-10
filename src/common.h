/* common.h
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

#define SWEAR_REPLACEMENT " [__]"

#define LIVECAPTIONS_VERSION "0.4.3"

#define MINIMUM_BENCHMARK_RESULT (0.6)
#define GET_MODEL_PATH() (getenv("APRIL_MODEL_PATH") == NULL) ? "/app/LiveCaptions/models/aprilv0_en-us.april" : getenv("APRIL_MODEL_PATH")

// Speaker diarization models. As with the ASR model, the Flatpak build installs
// these under /app and the environment variables let a local build point
// somewhere else.
#define GET_VAD_MODEL_PATH() (getenv("VAD_MODEL_PATH") == NULL) ? "/app/LiveCaptions/models/silero_vad_v5.onnx" : getenv("VAD_MODEL_PATH")
#define GET_SPEAKER_MODEL_PATH() (getenv("SPEAKER_MODEL_PATH") == NULL) ? "/app/LiveCaptions/models/wespeaker_en_voxceleb_CAMPP_LM.onnx" : getenv("SPEAKER_MODEL_PATH")
