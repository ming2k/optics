import re

def strip_project_header(content):
    # Remove project(...) block
    content = re.sub(r"project\([^)]*\)\n*", "", content, count=1, flags=re.DOTALL)
    # Remove standard setup lines
    content = re.sub(r"cc = meson\.get_compiler\('c'\)\n*", "", content)
    content = re.sub(r"m_dep = cc\.find_library\('m', required : false\)\n*", "", content)
    content = re.sub(r"pkg = import\('pkgconfig'\)\n*", "", content)
    content = re.sub(r"add_project_arguments\([^)]*\)\n*", "", content, flags=re.DOTALL)
    return content

# 1. Flux
with open('libs/flux/meson.build', 'r') as f:
    flux = f.read()
flux = strip_project_header(flux)
# Remove subdirs at the end of flux
flux = re.sub(r"subdir\('libs/flux-text'\)", "", flux)
flux = re.sub(r"subdir\('libs/flux-scene-graph'\)", "", flux)
flux = re.sub(r"subdir\('examples'\)", "", flux)
flux = re.sub(r"subdir\('tests'\)", "", flux)
with open('libs/flux/meson.build', 'w') as f:
    f.write(flux)

# 2. Flux sub-libs
for p in ['flux-text', 'flux-scene-graph']:
    with open(f'libs/{p}/meson.build', 'r') as f:
        c = f.read()
    c = strip_project_header(c)
    with open(f'libs/{p}/meson.build', 'w') as f:
        f.write(c)

# 3. Lens
with open('libs/lens/meson.build', 'r') as f:
    lens = f.read()
lens = strip_project_header(lens)
# Lens has a subdir('libs/lens') which we removed because it's now flat
lens = re.sub(r"subdir\('libs/lens'\)", "", lens)
with open('libs/lens/meson.build', 'w') as f:
    f.write(lens)

# 4. Iris
with open('libs/iris/meson.build', 'r') as f:
    iris = f.read()
iris = strip_project_header(iris)
# Iris has a dependency('lens') with a fallback, which is no longer needed since it's just lens_dep defined in the same meson tree.
# We can just remove the lens_dep = dependency(...) line entirely because lens_dep is already defined!
iris = re.sub(r"lens_dep = dependency\([^)]*\)\n*", "", iris, flags=re.DOTALL)
iris = re.sub(r"subdir\('examples'\)", "", iris)
iris = re.sub(r"subdir\('tests'\)", "", iris)
with open('libs/iris/meson.build', 'w') as f:
    f.write(iris)

# Top level meson.build
root = """project('optics', ['c'],
  version : '0.2.3',
  license : 'MIT',
  meson_version : '>=1.0',
  default_options : [
    'c_std=c2x',
    'warning_level=3',
    'default_library=shared',
    'b_ndebug=if-release',
  ],
)

cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required : false)
pkg = import('pkgconfig')

add_project_arguments(
  '-D_GNU_SOURCE',
  '-Wno-unused-parameter',
  '-Wno-missing-field-initializers',
  language : 'c',
)

subdir('libs/flux')
if get_option('text')
  subdir('libs/flux-text')
endif
if get_option('scene-graph')
  subdir('libs/flux-scene-graph')
endif

subdir('libs/lens')
subdir('libs/iris')

if get_option('examples')
  subdir('examples/flux')
  subdir('examples/iris')
endif
"""
with open('meson.build', 'w') as f:
    f.write(root)

