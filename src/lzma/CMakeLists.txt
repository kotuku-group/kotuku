# LZMA module
#
# Registers the LZMAStream class, a decode-only CompressedStream subclass for raw LZMA1 streams.  The decoder is the
# vendored LZMA SDK reference implementation under lzmasdk/ (see lzmasdk/README.md), compiled directly into the module
# in the same manner as src/mp3 vendors minimp3.

set (MOD lzma)
set (INC_MOD_LZMA TRUE PARENT_SCOPE)

idl_gen ("${MOD}.tdl" NAME ${MOD}_defs
   OUTPUT "${INCLUDE_OUTPUT}/modules/${MOD}.h")

add_library (${MOD})
set_module_defaults (${MOD} "Lzma")

target_sources (${MOD} PRIVATE
   "lzma.cpp"
   "lzmasdk/LzmaDec.c")

target_include_directories (${MOD} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/lzmasdk")

flute_test (lzma_decompress "tests/test_lzma.tiri")
