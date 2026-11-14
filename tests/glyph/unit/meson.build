# glyph_cache compiles its source file directly: the cache is a pure data
# structure with no Vulkan/FreeType/Fontconfig calls on these paths, so this
# lane runs on every build host. It still links flux for allocation
# attribution (ADR-0135).
test_glyph_cache = executable('test_glyph_cache',
  'test_glyph_cache.c',
  dependencies : [flux_dep, m_dep, optics_test_dep],
)
test('glyph_cache', test_glyph_cache, suite : 'unit')

# Version, refcount, scale/family policy, compaction and the null-tolerant
# public surface. No font shaping required.
test_glyph_lifecycle = executable('test_glyph_lifecycle',
  'test_glyph_lifecycle.c',
  dependencies : [glyph_test_dep],
)
test('glyph_lifecycle', test_glyph_lifecycle, suite : 'unit')
