project(
  'sync-kgraph',
  'c',
  version: '0.1.0',
  default_options: [
    # c2x, not c23: meson only learned 'c23' in 1.4, and this project declares
    # meson_version >=1.3.0. Newer meson still accepts c2x, and both select the
    # same standard, so c2x is the portable spelling.
    'c_std=c2x',
    'warning_level=3',
    'werror=true',
    'b_lundef=false',
  ],
  meson_version: '>=1.3.0',
)

cc = meson.get_compiler('c')

common_args = [
  '-D_POSIX_C_SOURCE=200809L',
  '-Wpedantic',
  '-Wconversion',
  '-Wsign-conversion',
  '-Wshadow',
  '-Wstrict-prototypes',
  '-Wmissing-prototypes',
  '-Wno-unsafe-buffer-usage',
]

foreach arg : common_args
  if cc.has_argument(arg)
    add_project_arguments(arg, language: 'c')
  endif
endforeach

inc = include_directories('include')

sync_sources = files('src/sync.c')

sync_lib = static_library(
  'sync_kgraph',
  sync_sources,
  include_directories: inc,
  install: true,
)

sync_dep = declare_dependency(
  include_directories: inc,
  link_with: sync_lib,
)

install_headers('include/sync_kgraph/sync.h', subdir: 'sync_kgraph')

sync_cli = executable(
  'sync-kgraph-cli',
  'src/sync_cli.c',
  dependencies: sync_dep,
  install: true,
)

test_core = executable(
  'test_core',
  'tests/test_core.c',
  dependencies: sync_dep,
)

test('core', test_core)

valgrind = find_program('valgrind', required: false)
if valgrind.found()
  add_test_setup(
    'valgrind',
    exe_wrapper: [
      valgrind,
      '--leak-check=full',
      '--show-leak-kinds=all',
      '--errors-for-leak-kinds=all',
      '--track-origins=yes',
      '--error-exitcode=99',
    ],
    timeout_multiplier: 10,
  )
endif

memgraph_feature = get_option('memgraph')
memgraph_include_dir = get_option('memgraph_include_dir')
memgraph_args = []
has_memgraph = false

if memgraph_include_dir != ''
  memgraph_args = ['-I' + memgraph_include_dir]
  has_memgraph = cc.has_header('mg_procedure.h', args: memgraph_args)
else
  has_memgraph = cc.has_header('mg_procedure.h')
endif

if memgraph_feature.enabled() and not has_memgraph
  error('Memgraph module requested, but mg_procedure.h was not found.')
endif

if has_memgraph and not memgraph_feature.disabled()
  module_link_args = []
  if host_machine.system() == 'darwin'
    module_link_args += ['-Wl,-undefined,dynamic_lookup']
  endif

  shared_module(
    'sync',
    'src/memgraph/sync_module.c',
    dependencies: sync_dep,
    c_args: memgraph_args,
    name_prefix: '',
    link_args: module_link_args,
    install: true,
    install_dir: get_option('libdir') / 'memgraph' / 'query_modules',
  )
endif

install_data(
  'cypher/install_schema.cypher',
  'cypher/triggers_mark_dirty.cypher',
  'cypher/uninstall.cypher',
  install_dir: get_option('datadir') / 'sync-kgraph' / 'cypher',
)

install_data(
  'views/sync_automata.gss',
  install_dir: get_option('datadir') / 'sync-kgraph' / 'views',
)

install_subdir(
  'examples/office',
  install_dir: get_option('datadir') / 'sync-kgraph' / 'examples',
)
