# Live Captions

![Screenshot of the application](https://github.com/abb128/LiveCaptions/blob/main/screenshot.png?raw=true)

Live Captions is an application that provides live captioning for the Linux desktop.

<p align="center">
    <a href='https://flathub.org/apps/details/net.sapples.LiveCaptions'>
        <img width='240' alt='Download on Flathub' src='https://flathub.org/assets/badges/flathub-badge-en.png'/>
    </a>
</p>

Join the [Discord Chat](https://discord.gg/QWaJHxWjUM) if you're interested in keeping in touch.

Only the English language is supported currently. Other languages may produce gibberish or a bad phonetic translation.

Features:
* Simple interface
* Caption desktop/mic audio locally, audio is never sent anywhere
* Does not rely on any proprietary services/libraries
* Adjust font, font size, and text casing
* Optional token-level confidence text fading

Running this requires a somewhat-decent CPU that can perform realtime captioning, especially if you want to be doing other tasks (such as video decode) while running Live Captions. It has been tested working on:
* Intel i7-2670QM (2011)
* Intel i7-7820HQ (2017)
* Intel i5-8265U (2018)
* AMD Ryzen 5 1600 (2017)
* Steam Deck

GPU is not required or used.

## Speaker identification
Live Captions labels who is speaking, turning a wall of text into an attributed
transcript. It is on by default; turn off "Identify Speakers" in the settings to
save a little CPU.

This uses two extra models, which run entirely on your machine like the ASR model,
and which the Flatpak build bundles for you. Nothing is ever sent anywhere.

If you are building from the terminal, fetch them yourself and point the
application at them:
```
$ wget https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/silero_vad_v5.onnx
$ wget -O wespeaker_en_voxceleb_CAMPP_LM.onnx \
    'https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models/wespeaker_en_voxceleb_CAM%2B%2B_LM.onnx'

$ export VAD_MODEL_PATH=`pwd`/silero_vad_v5.onnx
$ export SPEAKER_MODEL_PATH=`pwd`/wespeaker_en_voxceleb_CAMPP_LM.onnx
```

Verify them before use:
```
6b99cbfd39246b6706f98ec13c7c50c6b299181f2474fa05cbc8046acc274396  silero_vad_v5.onnx
e197af7e9d473030cf486b3124149a19bf37014d0e4485e4c70c483b0ec10cb2  wespeaker_en_voxceleb_CAMPP_LM.onnx
```

If the models are missing the application still captions as usual, it just does
not attribute anything.

`src/diarize-test` is a small offline harness for checking how well speakers are
separated on a given recording, without launching the interface:
```
$ ./_build/src/diarize-test recording.wav
```
It takes 16-bit mono WAV. Set `LIVECAPTIONS_DIARIZE_DEBUG=1` to see each decision.

## Accuracy
The live captions may not be accurate. It may make mistakes, including when it comes to numbers. Please do not rely on the results for anything critical or important.

More models may be trained and released in the future with better and more robust accuracy.

## Library
This application is built using [aprilasr](https://github.com/abb128/april-asr), a new library for realtime speech recognition.

## Model credit
Thanks to Fangjun Kuang for the [pretrained model](https://huggingface.co/csukuangfj/icefall-asr-librispeech-lstm-transducer-stateless2-2022-09-03/tree/main), and thanks to the [icefall](https://github.com/k2-fsa/icefall) contributors for creating the model recipes.

# Building
You must make sure to do a recursive clone to get dependencies:
```
$ git clone --recursive https://github.com/abb128/LiveCaptions.git
```

If you forgot, you can initialize submodules like so:
```
$ git submodule update --init --recursive
```

## Option 1: Building with GNOME Builder (easy)
You can build this easily with GNOME Builder. After cloning, open the project directory in GNOME Builder, download the SDK if it asks you, and click the play button to build and run.

If you are using Flatpak GNOME Builder and experience issues running this (for example, some cryptic X Window System error), please try using your distro's native packaged version of GNOME Builder instead of Flatpak (e.g. `sudo apt install gnome-builder`).

## Option 2: Building from the terminal (not as easy)
First you must [download ONNXRuntime v1.30.0 (Linux)](https://github.com/microsoft/onnxruntime/releases/download/v1.30.0/onnxruntime-linux-x64-1.30.0.tgz) or [ONNXRuntime v1.30.0 (Mac OS arm64)](https://github.com/microsoft/onnxruntime/releases/download/v1.30.0/onnxruntime-osx-arm64-1.30.0.tgz), extract it somewhere, and set the environment variables to point to it.

### Make ONNX available

Linux:
```
$ export ONNX_ROOT=/path/to/onnxruntime-linux-x64-1.30.0/
$ export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/path/to/onnxruntime-linux-x64-1.30.0/lib
```

Alternatively you should also be able to locally build and install ONNXRuntime, in which case that step shouldn't be necessary.


Mac OS:
```
$ export ONNX_ROOT=/path/to/onnxruntime-osx-arm64-1.30.0/
$ cp /path/to/onnxruntime-osx-arm64-1.30.0/lib/libonnxruntime.1.30.0.dylib /usr/local/lib/libonnxruntime.1.30.0.dylib
$ cp /path/to/onnxruntime-osx-arm64-1.30.0/lib/libonnxruntime.dylib /usr/local/lib/libonnxruntime.dylib
```

### Other dependencies
You will also need the following prerequisites:
```
pulseaudio
libadwaita
meson
ninja
```

If you're on macOS, you may have to run the following command for Pulseaudio to be registered as a background service:
```
brew services restart pulseaudio
```

and to stop it, just change `restart` for stop.


### Set up the build

To set up a build, run these commands:
```
$ meson setup builddir --buildtype release
$ meson devenv -C builddir
```


### Build and run

Now you can build the application. Run the following:
```
$ ninja
```

Before being able to run the app, you must also download the model and export `APRIL_MODEL_PATH` to where the model is. For example:
```
$ wget https://april.sapples.net/april-english-dev-01110_en.april
$ export APRIL_MODEL_PATH=`pwd`/april-english-dev-01110_en.april
```

You should now be able to run the app with:
```
$ src/livecaptions
```

If you're on MacOS, now, go to security settings and confirm the prompt which asks to allow `libonnxruntime`, then, the application should work as intended.
