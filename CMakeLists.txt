cmake_minimum_required(VERSION 3.20)
project(Nyx VERSION 0.1.0 LANGUAGES CXX RC)
option(NYX_BUILD_CLI "Build the command-line tool" OFF)
option(NYX_BUILD_TESTS "Build local test executables" OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(NOT WIN32)
  message(FATAL_ERROR "This release uses Windows WIC and BCrypt")
endif()
add_library(csystem_core
    src/core.cpp
    src/legacy.cpp
    src/opcodes.cpp
    src/binary.cpp
    src/script.cpp
    src/images.cpp
    src/image_profiles.cpp
    src/audio.cpp
    src/video.cpp
    src/project.cpp
    src/scenario_project.cpp
    src/progress.cpp
    src/documents.cpp
    src/standalone.cpp
    src/references.cpp
    src/graphics.cpp
    src/page_parameters.cpp
)
target_include_directories(csystem_core PUBLIC
    "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
configure_file(resources/app.manifest app.manifest COPYONLY)
target_compile_definitions(csystem_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_link_libraries(csystem_core PUBLIC bcrypt ole32 windowscodecs uuid)
if(MSVC)
  add_compile_options(/W4 /utf-8)
  target_compile_options(csystem_core PRIVATE /W4 /utf-8)
  target_link_options(csystem_core PUBLIC /STACK:8388608)
else()
  target_compile_options(csystem_core PRIVATE -Wall -Wextra -Wpedantic)
  target_link_options(csystem_core PUBLIC -static -Wl,--stack,8388608)
endif()
add_executable(CsystemTool WIN32 src/gui.cpp resources/windows.rc)
set_target_properties(CsystemTool PROPERTIES OUTPUT_NAME Nyx)
target_link_libraries(CsystemTool PRIVATE csystem_core comctl32 shell32 dwmapi uxtheme advapi32)
if(MSVC)
  target_link_options(CsystemTool PRIVATE /MANIFEST:NO)
endif()
if(MINGW)
  target_link_options(CsystemTool PRIVATE -municode)
endif()

if(NYX_BUILD_CLI)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
    message(FATAL_ERROR "NYX_BUILD_CLI requires the local src/main.cpp file")
  endif()
  add_executable(csystem-cli src/main.cpp)
  target_link_libraries(csystem-cli PRIVATE csystem_core)
  if(MINGW)
    target_link_options(csystem-cli PRIVATE -municode)
  endif()
  install(TARGETS csystem-cli RUNTIME DESTINATION .)
endif()

enable_testing()
add_test(NAME gui_startup COMMAND CsystemTool --smoke-test)

if(NYX_BUILD_TESTS)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/tests.cpp")
    message(FATAL_ERROR "NYX_BUILD_TESTS requires the local tests directory")
  endif()
  add_executable(csystem_tests tests/tests.cpp)
  target_link_libraries(csystem_tests PRIVATE csystem_core)
  add_test(NAME codecs_and_compiler COMMAND csystem_tests)

  add_executable(csystem_scenario_tests tests/scenario_project.cpp)
  target_link_libraries(csystem_scenario_tests PRIVATE csystem_core)
  add_test(NAME partial_export_and_compile COMMAND csystem_scenario_tests)

  add_executable(csystem_image_wrapper_tests tests/image_wrappers.cpp)
  target_link_libraries(csystem_image_wrapper_tests PRIVATE csystem_core)
  add_test(NAME image_wrapper_export_and_compile COMMAND csystem_image_wrapper_tests)

  add_executable(csystem_jpeg_tests tests/jpeg.cpp)
  target_link_libraries(csystem_jpeg_tests PRIVATE csystem_core)
  add_test(NAME jpeg_export_and_compile COMMAND csystem_jpeg_tests)

  add_executable(csystem_archive_storage_tests tests/archive_storage.cpp)
  target_link_libraries(csystem_archive_storage_tests PRIVATE csystem_core)
  add_test(NAME archive_storage_export_and_compile COMMAND csystem_archive_storage_tests)

  add_executable(csystem_image_profile_tests tests/image_profiles.cpp)
  target_link_libraries(csystem_image_profile_tests PRIVATE csystem_core)
  add_test(NAME experimental_image_profiles COMMAND csystem_image_profile_tests)

  add_executable(csystem_integration EXCLUDE_FROM_ALL tests/integration.cpp)
  target_link_libraries(csystem_integration PRIVATE csystem_core)
  add_executable(csystem_page_parameters EXCLUDE_FROM_ALL tests/page_parameters.cpp)
  target_link_libraries(csystem_page_parameters PRIVATE csystem_core)
  add_executable(csystem_game_roundtrip EXCLUDE_FROM_ALL tests/game_roundtrip.cpp)
  target_link_libraries(csystem_game_roundtrip PRIVATE csystem_core)
  add_executable(csystem_scenario_roundtrip EXCLUDE_FROM_ALL tests/scenario_roundtrip.cpp)
  target_link_libraries(csystem_scenario_roundtrip PRIVATE csystem_core)
  add_executable(csystem_image_roundtrip EXCLUDE_FROM_ALL tests/image_roundtrip.cpp)
  target_link_libraries(csystem_image_roundtrip PRIVATE csystem_core)
  add_executable(csystem_image_orientation EXCLUDE_FROM_ALL tests/image_orientation.cpp)
  target_link_libraries(csystem_image_orientation PRIVATE csystem_core)
  add_executable(csystem_image_variants EXCLUDE_FROM_ALL tests/image_variants.cpp)
  target_link_libraries(csystem_image_variants PRIVATE csystem_core)
  if(MINGW)
    target_link_options(csystem_integration PRIVATE -municode)
    target_link_options(csystem_page_parameters PRIVATE -municode)
    target_link_options(csystem_game_roundtrip PRIVATE -municode)
    target_link_options(csystem_scenario_roundtrip PRIVATE -municode)
    target_link_options(csystem_image_roundtrip PRIVATE -municode)
    target_link_options(csystem_image_orientation PRIVATE -municode)
    target_link_options(csystem_image_variants PRIVATE -municode)
  endif()
endif()

install(TARGETS CsystemTool RUNTIME DESTINATION .)
install(FILES README.md DESTINATION .)
