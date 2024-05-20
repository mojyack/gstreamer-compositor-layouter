project('gst-grid-aligner', 'cpp', version : '0.0', default_options : ['warning_level=3', 'cpp_std=c++20'])
add_global_arguments('-Wfatal-errors', language: 'cpp')

gstreamer_dep = dependency('gstreamer-1.0', version : ['>=1.20'])

deps = [
  gstreamer_dep,
]

src = files(
  'src/example.cpp',
  'src/compositor-layouter.cpp',
  'src/gutil/pipeline-helper.cpp',
)

executable('example', src,
        dependencies: deps,
)
