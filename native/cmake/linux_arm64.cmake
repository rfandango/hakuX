# xbased's core on Linux arm64 (clang): the guest CPU is FEXCore (cmake/fex.cmake),
# the SDL3 desktop frontend (app/win_main.c, cmake/app_win.cmake) is shared with
# Linux x86-64. No Zydis, no x86-only compiler flags, no SEH shim.
# SDL3 is built here (static) for the APU's output and the front end.
set(NC_DEPS "${CMAKE_CURRENT_LIST_DIR}/../../../deps" CACHE PATH "native core third-party sources")

include("${CMAKE_CURRENT_LIST_DIR}/fex.cmake")

# The guest's 4 GiB live at host address 0 (core/vm.c): pointer arithmetic on
# that null base is everyday business, no optimization may assume otherwise.
# A PIE executable: it and its libraries load far above the guest's space.
add_compile_options(-fno-delete-null-pointer-checks -g1 -fPIE)
add_compile_definitions(_GNU_SOURCE)
add_link_options(-pie)
# C++'s runtime inside: the binary asks the system for glibc (and zlib) only
add_link_options(-static-libstdc++ -static-libgcc)
set(NC_LINUX_COMPAT_INC "${CMAKE_CURRENT_LIST_DIR}/../compat/linux/include")
function(nc_linux_compat)
  foreach(t ${ARGN})
    target_include_directories(${t} BEFORE PRIVATE "${NC_LINUX_COMPAT_INC}")
  endforeach()
endfunction()

# Both PC platforms' code paths (SDL audio, the props shim, ...): NC_DESKTOP
add_compile_definitions(NC_DESKTOP=1)

# On native CI the system zlib is available; for cross-compilation build from source.
if(CMAKE_CROSSCOMPILING)
  set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  add_subdirectory("${NC_DEPS}/zlib" zlib-build EXCLUDE_FROM_ALL)
  set(NC_ZLIB_LIB zlibstatic CACHE STRING "" FORCE)
  # zlib uses old-style include_directories (not INTERFACE), so propagate manually
  set(NC_ZLIB_INCLUDE_DIRS "${NC_DEPS}/zlib" "${CMAKE_BINARY_DIR}/zlib-build" CACHE STRING "" FORCE)
else()
  set(NC_ZLIB_LIB z CACHE STRING "" FORCE)
  set(NC_ZLIB_INCLUDE_DIRS "" CACHE STRING "" FORCE)
endif()

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_STATIC_PIC ON CACHE BOOL "" FORCE)
add_subdirectory("${NC_DEPS}/SDL3" sdl3 EXCLUDE_FROM_ALL)
