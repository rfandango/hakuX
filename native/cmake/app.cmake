# The hakuX app's native core: libhakuxcore.so, loaded by NativeCoreActivity
# next to libSDL3.so. The front end (native/app) is the only thing it exports
# (SDL_main and its JNI); everything linked in statically stays hidden.

# adrenotools (NC_DEPS/adrenotools, bylaws/libadrenotools): a custom Vulkan
# driver (Turnip) for the HLE GPU, chosen in the app's settings, and
# xboxrun's "vkdriver=". Its hook libraries (libmain_hook.so and co.) ship
# next to libhakuxcore.so.
set(NC_ADRENOTOOLS "${NC_DEPS}/adrenotools")
if(EXISTS "${NC_ADRENOTOOLS}/CMakeLists.txt")
  add_subdirectory("${NC_ADRENOTOOLS}" adrenotools EXCLUDE_FROM_ALL)
endif()
if(TARGET adrenotools)
  target_link_libraries(xboxrun PRIVATE adrenotools)
  target_compile_definitions(xboxrun PRIVATE NC_ADRENOTOOLS=1)
endif()

# SDL3 comes from NC_DEPS/SDL3 (release-3.4.18, unmodified); its Java glue
# is in android/app/src/main/java/org/libsdl/app.
set(NC_SDL3 "${NC_DEPS}/SDL3")
if(NOT EXISTS "${NC_SDL3}/CMakeLists.txt")
  message(STATUS "hakuxcore: no SDL3 at ${NC_SDL3}; the app library is not built")
  return()
endif()

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${NC_SDL3}" sdl3 EXCLUDE_FROM_ALL)

add_library(hakuxcore SHARED
  "${CMAKE_CURRENT_SOURCE_DIR}/app/app_main.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/present.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/gamepad.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/vsync.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/vklayer.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/vkdriver.c"
)
target_link_libraries(hakuxcore PRIVATE hakux_native hakux_hle SDL3::SDL3-shared android log)
if(TARGET adrenotools)
  target_link_libraries(hakuxcore PRIVATE adrenotools)
  target_compile_definitions(hakuxcore PRIVATE NC_ADRENOTOOLS=1)
  add_dependencies(hakuxcore main_hook file_redirect_hook gsl_alloc_hook kgsl_context_hook hook_impl)
endif()
target_link_options(hakuxcore PRIVATE -Wl,--exclude-libs,ALL -Wl,--gc-sections)
set_target_properties(hakuxcore PROPERTIES C_VISIBILITY_PRESET hidden)
target_compile_options(hakuxcore PRIVATE -Wall -Wno-unused-parameter)
