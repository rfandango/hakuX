# The desktop executable: the SDL3 front end (app/desk_main.c) on the core,
# with the presentation and frame pacing (app/present.c, app/vsync.c),
# and the dashboard (app/ui/). Linux arm64 only — no Windows build.
set(NC_APP_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/app/desk_main.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/desk_mods.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/desk_auto.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/present.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/vsync.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/ui.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/i18n.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/config.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/xbox_data.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/library.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/input_map.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/sound.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/dash.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/dash_settings.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/dash_memory.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/update.c"
)
add_executable(hakux ${NC_APP_SOURCES})
set_target_properties(hakux PROPERTIES OUTPUT_NAME kantaiX)
target_link_libraries(hakux PRIVATE hakux_native hakux_hle SDL3::SDL3)
target_include_directories(hakux PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../third_party/stb")  # public domain, vendored
# KANTAIX_VERSION is set by CI from the git tag; fall back to the in-source default for local builds.
if(NOT DEFINED KANTAIX_VERSION)
  set(KANTAIX_VERSION "1.3.0")
endif()
target_compile_definitions(hakux PRIVATE KANTAIX_VERSION="${KANTAIX_VERSION}")
target_compile_options(hakux PRIVATE -Wall -Wno-unused-parameter -Wno-unused-function)
# mods/cheats (mods/README.md) and dashboard fonts beside the executable
add_custom_command(TARGET hakux POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${NC_REPO}/mods" "$<TARGET_FILE_DIR:hakux>/mods"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/fonts" "$<TARGET_FILE_DIR:hakux>/fonts")
