cmake_minimum_required(VERSION 3.16)
project(nodx_adapt VERSION 0.2.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(NODX_SANITIZE "Enable undefined-behavior sanitizer" OFF)
option(NODX_ASAN "Also enable address sanitizer (platform dependent)" OFF)
if(NODX_SANITIZE)
  add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=undefined)
endif()
if(NODX_ASAN)
  add_compile_options(-fsanitize=address)
  add_link_options(-fsanitize=address)
endif()
add_library(nodx
    core/src/calibration.cpp
    core/src/motion.cpp
    core/src/selection.cpp
    core/src/output.cpp
    core/src/system.cpp
    core/src/profile.cpp
    core/src/sensor.cpp
    core/src/gesture.cpp
    core/src/handsfree.cpp
    core/src/mapping.cpp
    core/src/clickgesture.cpp
    core/src/quickgesture.cpp
    core/src/actions.cpp
)
target_include_directories(nodx PUBLIC core/include)
target_compile_options(nodx PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(nodx_sim desktop/main.cpp)
target_link_libraries(nodx_sim PRIVATE nodx)
# Offline replay of captured raw sensor recordings through the quick-gesture recognizer.
add_executable(nodx_replay tools/replay.cpp)
target_link_libraries(nodx_replay PRIVATE nodx)
add_executable(nodx_tests tests/test_main.cpp)
target_link_libraries(nodx_tests PRIVATE nodx)
add_executable(nodx_handsfree_tests tests/test_handsfree.cpp)
target_link_libraries(nodx_handsfree_tests PRIVATE nodx)
add_executable(nodx_mapping_tests tests/test_mapping.cpp)
target_link_libraries(nodx_mapping_tests PRIVATE nodx)
add_executable(nodx_click_tests tests/test_click.cpp)
target_link_libraries(nodx_click_tests PRIVATE nodx)
add_executable(nodx_quick_tests tests/test_quick.cpp)
target_link_libraries(nodx_quick_tests PRIVATE nodx)
add_executable(nodx_actions_tests tests/test_actions.cpp)
target_link_libraries(nodx_actions_tests PRIVATE nodx)
# IMU identity, initialisation, read-back and conversion (MPU-6050 and MPU-6500 variants).
add_executable(nodx_imu_tests tests/test_imu.cpp)
target_link_libraries(nodx_imu_tests PRIVATE nodx)
# Bench-diagnostic helpers (pure logic, no Arduino): statistics and button burst grouping.
add_executable(nodx_diag_tests tests/test_diag.cpp)
target_include_directories(nodx_diag_tests PRIVATE firmware/diag)
# Host tests of the REAL firmware command path: firmware/src is compiled unmodified against stub
# Arduino/Wire/Preferences/NimBLE headers. Not a substitute for on-device testing.
foreach(variant sim hw)
    add_executable(nodx_firmware_${variant}_tests
        tests/test_firmware_commands.cpp
        firmware/src/runtime.cpp
        firmware/src/commands.cpp
        firmware/src/telemetry.cpp)
    target_include_directories(nodx_firmware_${variant}_tests PRIVATE
        tests/firmware_host/stubs firmware/src)
    target_link_libraries(nodx_firmware_${variant}_tests PRIVATE nodx)
endforeach()
target_compile_definitions(nodx_firmware_sim_tests PRIVATE NODX_SIMULATED=1)
set_source_files_properties(tests/test_firmware_commands.cpp PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wpedantic;-Werror")
enable_testing()
add_test(NAME control_invariants COMMAND nodx_tests)
add_test(NAME handsfree_invariants COMMAND nodx_handsfree_tests)
add_test(NAME bench_diag_helpers COMMAND nodx_diag_tests)
add_test(NAME mapping_invariants COMMAND nodx_mapping_tests)
add_test(NAME click_gesture_invariants COMMAND nodx_click_tests)
add_test(NAME quick_gesture_invariants COMMAND nodx_quick_tests)
add_test(NAME dwell_action_palette COMMAND nodx_actions_tests)
add_test(NAME imu_support COMMAND nodx_imu_tests)
add_test(NAME firmware_commands_simulated COMMAND nodx_firmware_sim_tests)
add_test(NAME firmware_commands_hardware_config COMMAND nodx_firmware_hw_tests)

foreach(target nodx_sim nodx_tests nodx_handsfree_tests nodx_diag_tests nodx_imu_tests)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endforeach()
