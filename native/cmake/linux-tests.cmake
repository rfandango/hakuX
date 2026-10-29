add_executable(linux-smoke "${NC_REPO}/tests/native/linux/smoke.c" "${NC_APU_OUT_NULL}")
target_link_libraries(linux-smoke PRIVATE hakux_native)
add_test(NAME linux-smoke COMMAND linux-smoke)
set_tests_properties(linux-smoke PROPERTIES TIMEOUT 15)
add_executable(audio-smoke "${NC_REPO}/tests/native/linux/audio.c" "${NC_APU_OUT_NULL}")
target_include_directories(audio-smoke PRIVATE "${NC_REPO}")
target_link_libraries(audio-smoke PRIVATE hakux_native)
add_test(NAME audio-smoke COMMAND audio-smoke)
set_tests_properties(audio-smoke PROPERTIES TIMEOUT 10)
# The USB stack driven as the guest's would: enumeration, input, rumble, and
# controllers plugged in and pulled out.
add_executable(usb-smoke "${NC_REPO}/tests/native/usb/usb_test.c" "${NC_REPO}/native/dev/usb.c"
                         "${NC_REPO}/native/dev/usb_hub.c" "${NC_REPO}/native/dev/xid.c")
target_include_directories(usb-smoke PRIVATE "${NC_REPO}")
target_link_libraries(usb-smoke PRIVATE Threads::Threads)
add_test(NAME usb-smoke COMMAND usb-smoke)
set_tests_properties(usb-smoke PROPERTIES TIMEOUT 10)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME linux-boundaries
         COMMAND "${Python3_EXECUTABLE}" "${NC_REPO}/tests/native/linux/check.py" "$<TARGET_FILE_DIR:xboxrun>")
set_tests_properties(linux-boundaries PROPERTIES TIMEOUT 60)
if(TARGET hakux_libretro)
  # The APU made again in one process (a relaunch), its EP's samples through
  # the libretro output.
  add_executable(apu-reinit-smoke "${NC_REPO}/tests/native/linux/apu_reinit.c"
                                  "${NC_REPO}/native/libretro/audio.c")
  target_link_libraries(apu-reinit-smoke PRIVATE hakux_native)
  add_test(NAME apu-reinit-smoke COMMAND apu-reinit-smoke)
  set_tests_properties(apu-reinit-smoke PROPERTIES TIMEOUT 10)
  # The core through a minimal frontend: exports, info and a synthetic title.
  add_executable(libretro-harness "${NC_REPO}/tests/native/libretro/harness.c"
                                  "${NC_REPO}/tests/native/libretro/vk.c")
  target_include_directories(libretro-harness PRIVATE "${NC_REPO}" "${NC_DEPS}/Vulkan-Headers/include"
                             "${NC_REPO}/native/libretro")
  target_link_libraries(libretro-harness PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
  add_test(NAME libretro-smoke
           COMMAND "${Python3_EXECUTABLE}" "${NC_REPO}/tests/native/libretro/check.py"
                   "$<TARGET_FILE:libretro-harness>" "$<TARGET_FILE:hakux_libretro>")
  set_tests_properties(libretro-smoke PROPERTIES TIMEOUT 120)
endif()
