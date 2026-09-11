livecaptions_sources = [
  'main.c',
  'livecaptions-window.c',
  'livecaptions-welcome.c',
  'livecaptions-settings.c',
  'livecaptions-application.c',
  'audiocap.c',
  'audiocap-pa.c',
  'audiocap-pw.c',
  'asrproc.c',
  'diarize.c',
  'spk-fbank.c',
  'spk-embed.c',
  'spk-vad.c',
  'speaker-db.c',
  'line-gen.c',
  'profanity-filter.c',
  'window-helper.c',
  'history.c',
  'livecaptions-history-window.c',
  'dbus-interface.c'
]

cc = meson.get_compiler('c')

# The diarizer drives ONNX Runtime directly, so unlike the ASR path it needs the
# headers itself rather than reaching them through april-asr. ONNX_ROOT is how
# the Flatpak build and the README's terminal build both point at it; a distro
# install that ships a pkg-config file works without it.
onnx_root = run_command(find_program('sh'), '-c', 'printf %s "$ONNX_ROOT"', check: false).stdout().strip()

onnx_args = []
onnx_deps = []

if onnx_root != ''
  onnx_args += ['-I' + onnx_root / 'include']
  onnx_lib = cc.find_library('onnxruntime', dirs: [onnx_root / 'lib'], required: false)
  if onnx_lib.found()
    onnx_deps += onnx_lib
  endif
else
  onnx_pkg = dependency('libonnxruntime', required: false)
  if not onnx_pkg.found()
    onnx_pkg = dependency('onnxruntime', required: false)
  endif
  if onnx_pkg.found()
    onnx_deps += onnx_pkg
  else
    error('Could not find ONNX Runtime. Set ONNX_ROOT to an extracted release, ' +
          'as described in the README.')
  endif
endif

livecaptions_deps = [
  dependency('libadwaita-1', version: '>= 1.0'),
  #dependency('libpipewire-0.3', version: '>=0.3.41'),
  dependency('libpulse'),
  dependency('x11'),

  cc.find_library('m', required: false),
  april_lib
] + onnx_deps

gnome = import('gnome')

livecaptions_sources += gnome.compile_resources('livecaptions-resources',
  'livecaptions.gresource.xml',
  c_name: 'livecaptions'
)

executable('livecaptions', livecaptions_sources,
  dependencies: livecaptions_deps,
  c_args: onnx_args,
  install: true,
)

# Checks speaker labelling in the caption line generator. Runs headless, since
# Pango can lay text out through a font map with no windowing system, which is
# what makes this runnable as an ordinary test.
line_gen_test = executable('line-gen-test',
  ['line-gen-test.c', 'line-gen.c', 'profanity-filter.c'],
  dependencies: [
    dependency('libadwaita-1', version: '>= 1.0'),
    dependency('pangocairo'),
    cc.find_library('m', required: false),
    april_lib
  ],
  install: false,
)

test('line-gen', line_gen_test,
  env: ['GSETTINGS_SCHEMA_DIR=' + meson.project_build_root() / 'data'],
)

# Checks the stored voice profiles round-trip. Pure logic, so it needs neither
# a display nor the speaker models.
speaker_db_test = executable('speaker-db-test',
  ['speaker-db-test.c', 'speaker-db.c'],
  dependencies: [dependency('glib-2.0')],
  install: false,
)

test('speaker-db', speaker_db_test)

# Offline harness for the diarizer. Not installed; it exists so speaker
# segmentation can be tuned and regression-tested without the GUI.
executable('diarize-test',
  ['diarize-test.c', 'diarize.c', 'spk-fbank.c', 'spk-embed.c', 'spk-vad.c'],
  dependencies: [
    dependency('glib-2.0'),
    cc.find_library('m', required: false)
  ] + onnx_deps,
  c_args: onnx_args,
  install: false,
)
