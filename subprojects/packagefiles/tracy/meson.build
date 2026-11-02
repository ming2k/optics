project('tracy', 'cpp',
  version : '0.14.1',
  default_options : ['cpp_std=c++17'],
)

threads_dep = dependency('threads')
cpp_cc = meson.get_compiler('cpp')
dl_dep = cpp_cc.find_library('dl', required : false)

cpp_lib = cpp_cc.find_library('stdc++', required : false)
if not cpp_lib.found()
  cpp_lib = cpp_cc.find_library('c++', required : false)
endif

tracy_cpp_args = ['-DTRACY_ENABLE', '-DTRACY_EXPORTS'] + cpp_cc.get_supported_arguments(
  '-w',
  '-Wno-unused-parameter',
  '-Wno-missing-field-initializers',
  '-Wno-sign-compare',
)

host_sys = host_machine.system()
tracy_deps = [threads_dep]
if cpp_lib.found()
  tracy_deps += cpp_lib
endif
if host_sys == 'linux'
  tracy_deps += dl_dep
elif host_sys == 'windows'
  ws2_dep = cpp_cc.find_library('ws2_32', required : true)
  dbghelp_dep = cpp_cc.find_library('dbghelp', required : true)
  tracy_deps += [ws2_dep, dbghelp_dep]
endif

tracy_inc = include_directories('public')

tracy_lib = shared_library('tracy',
  'public/TracyClient.cpp',
  include_directories : tracy_inc,
  cpp_args : tracy_cpp_args,
  dependencies : tracy_deps,
  install : false,
)

tracy_dep = declare_dependency(
  include_directories : tracy_inc,
  link_with : tracy_lib,
  compile_args : ['-DTRACY_ENABLE', '-DTRACY_IMPORTS'],
  dependencies : tracy_deps,
)
