# libchdr (NC_DEPS/libchdr, rtissera/libchdr, BSD-3-Clause): CHD disc images
# (core/chd.c). Its codecs over the system zlib, its own LZMA (with the arm64
# decoder), zstd and dr_flac. Without it the core reads ISO images only.
set(NC_CHDR "${NC_DEPS}/libchdr")
if(EXISTS "${NC_CHDR}/src/libchdr_chd.c")
  file(GLOB NC_CHDR_LZMA LIST_DIRECTORIES true "${NC_CHDR}/deps/lzma-*")
  file(GLOB NC_CHDR_ZSTD LIST_DIRECTORIES true "${NC_CHDR}/deps/zstd-*")
  add_library(nc_chdr STATIC
    "${NC_CHDR}/src/libchdr_bitstream.c"
    "${NC_CHDR}/src/libchdr_cdrom.c"
    "${NC_CHDR}/src/libchdr_chd.c"
    "${NC_CHDR}/src/libchdr_codec_avhuff.c"
    "${NC_CHDR}/src/libchdr_codec_cdfl.c"
    "${NC_CHDR}/src/libchdr_codec_cdlz.c"
    "${NC_CHDR}/src/libchdr_codec_cdzl.c"
    "${NC_CHDR}/src/libchdr_codec_cdzs.c"
    "${NC_CHDR}/src/libchdr_codec_flac.c"
    "${NC_CHDR}/src/libchdr_codec_huff.c"
    "${NC_CHDR}/src/libchdr_codec_lzma.c"
    "${NC_CHDR}/src/libchdr_codec_zlib.c"
    "${NC_CHDR}/src/libchdr_codec_zstd.c"
    "${NC_CHDR}/src/libchdr_flac.c"
    "${NC_CHDR}/src/libchdr_huffman.c"
    "${NC_CHDR_LZMA}/src/LzmaDec.c"
    "${NC_CHDR_ZSTD}/zstddeclib.c"
  )
  enable_language(ASM)
  target_sources(nc_chdr PRIVATE "${NC_CHDR_LZMA}/Asm/arm64/LzmaDecOpt.S")
  set_source_files_properties("${NC_CHDR_LZMA}/src/LzmaDec.c" PROPERTIES COMPILE_DEFINITIONS Z7_LZMA_DEC_OPT)
  target_include_directories(nc_chdr PUBLIC "${NC_CHDR}/include")
  target_compile_definitions(nc_chdr PRIVATE CHDR_SYSTEM_ZLIB)
  target_compile_options(nc_chdr PRIVATE -w)
  set_target_properties(nc_chdr PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
  target_link_libraries(nc_chdr PRIVATE z)
  target_link_libraries(hakux_native PRIVATE nc_chdr)
  target_compile_definitions(hakux_native PRIVATE NC_HAVE_CHD)
else()
  message(STATUS "libchdr not in ${NC_CHDR}: no CHD images")
endif()
