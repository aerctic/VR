cmake_minimum_required(VERSION 3.21)
project(vrlayer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include(GNUInstallDirs)

option(VRLAYER_BUILD_LAYER "Build the Vulkan capture layer" ON)
option(VRLAYER_BUILD_COMPOSITOR "Build the OpenXR compositor and tools" ON)
option(VRLAYER_BUILD_TESTS "Build tests" ON)

find_package(Threads REQUIRED)
# The layer only needs the headers (so a 32-bit layer builds without a 32-bit
# loader); the host side links the Vulkan loader.
find_path(VULKAN_HEADERS_DIR vulkan/vk_layer.h REQUIRED)
if(VRLAYER_BUILD_COMPOSITOR OR VRLAYER_BUILD_TESTS)
  find_package(Vulkan REQUIRED)
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(VRLAYER_ARCH x86_64)
else()
  set(VRLAYER_ARCH x86)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

# ---------------------------------------------------------------------------
# Shared code
# ---------------------------------------------------------------------------
add_library(vrlayer_common STATIC common/ipc.cpp common/config.cpp common/camera_math.cpp)
target_include_directories(vrlayer_common PUBLIC common)
set_target_properties(vrlayer_common PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Vulkan implicit layer (loaded into the game)
# ---------------------------------------------------------------------------
if(VRLAYER_BUILD_LAYER)
  add_library(VkLayer_vrlayer SHARED layer/layer.cpp layer/capture.cpp layer/analyzer.cpp)
  target_include_directories(VkLayer_vrlayer PRIVATE ${VULKAN_HEADERS_DIR})
  target_link_libraries(VkLayer_vrlayer PRIVATE vrlayer_common Threads::Threads)
  set_target_properties(VkLayer_vrlayer PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
  # The layer is injected into arbitrary processes (including the Steam Linux
  # Runtime container), so do not depend on the host's libstdc++.
  target_link_options(VkLayer_vrlayer PRIVATE -static-libstdc++ -static-libgcc -Wl,--no-undefined
                      -Wl,--exclude-libs,ALL)

  set(VRLAYER_LAYER_NAME "VK_LAYER_VRLAYER_capture_${VRLAYER_ARCH}")
  set(VRLAYER_MANIFEST "VkLayer_vrlayer.${VRLAYER_ARCH}.json")

  # Manifest for running straight from the build tree:
  #   XDG_DATA_HOME=<build>/devroot ENABLE_VRLAYER=1 <game>
  set(VRLAYER_LAYER_PATH "$<TARGET_FILE:VkLayer_vrlayer>")
  configure_file(layer/VkLayer_vrlayer.json.in ${CMAKE_BINARY_DIR}/manifest.dev.json.in @ONLY)
  file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/devroot/vulkan/implicit_layer.d/${VRLAYER_MANIFEST}
       INPUT ${CMAKE_BINARY_DIR}/manifest.dev.json.in)

  # Installed manifest.
  set(VRLAYER_LAYER_PATH "${CMAKE_INSTALL_FULL_LIBDIR}/vrlayer/libVkLayer_vrlayer.so")
  configure_file(layer/VkLayer_vrlayer.json.in ${CMAKE_BINARY_DIR}/${VRLAYER_MANIFEST} @ONLY)

  install(TARGETS VkLayer_vrlayer LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/vrlayer)
  install(FILES ${CMAKE_BINARY_DIR}/${VRLAYER_MANIFEST}
          DESTINATION ${CMAKE_INSTALL_DATADIR}/vulkan/implicit_layer.d)
endif()

# ---------------------------------------------------------------------------
# Shaders
# ---------------------------------------------------------------------------
find_program(GLSLANG_VALIDATOR glslangValidator)
function(vrlayer_shader target_var src var_name)
  get_filename_component(name ${src} NAME)
  set(out ${CMAKE_BINARY_DIR}/shaders/${name}.h)
  add_custom_command(OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
    COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.1 --vn ${var_name} -o ${out} ${CMAKE_SOURCE_DIR}/${src}
    DEPENDS ${CMAKE_SOURCE_DIR}/${src}
    COMMENT "Compiling ${src}")
  set(${target_var} ${${target_var}} ${out} PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# Host side: frame import + stereo synthesis (no OpenXR dependency)
# ---------------------------------------------------------------------------
if(VRLAYER_BUILD_COMPOSITOR OR VRLAYER_BUILD_TESTS)
  if(NOT GLSLANG_VALIDATOR)
    message(FATAL_ERROR "glslangValidator is required (package glslang-tools)")
  endif()
  set(HOST_SHADERS)
  vrlayer_shader(HOST_SHADERS shaders/stereo.comp kStereoCompSpv)
  vrlayer_shader(HOST_SHADERS shaders/scene.comp kSceneCompSpv)

  add_library(vrlayer_host STATIC
    host/gpu.cpp
    host/frame_server.cpp
    host/stereo.cpp
    host/settings.cpp
    host/head_mouse.cpp
    host/uinput.cpp
    host/canvas.cpp
    host/scene.cpp
    host/calibrator.cpp
    host/controls.cpp
    host/loading.cpp
    ${HOST_SHADERS})
  target_include_directories(vrlayer_host PUBLIC host ${CMAKE_BINARY_DIR}/shaders)
  target_link_libraries(vrlayer_host PUBLIC vrlayer_common Vulkan::Vulkan Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# OpenXR compositor + control tool
# ---------------------------------------------------------------------------
if(VRLAYER_BUILD_COMPOSITOR)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(OPENXR REQUIRED IMPORTED_TARGET openxr)

  add_executable(vrlayer-compositor
    compositor/main.cpp
    compositor/app.cpp
    compositor/render.cpp
    compositor/xr.cpp
    compositor/input.cpp
    compositor/hands.cpp
    compositor/vrmath.cpp)
  target_include_directories(vrlayer-compositor PRIVATE compositor)
  target_link_libraries(vrlayer-compositor PRIVATE vrlayer_host PkgConfig::OPENXR)
  target_compile_definitions(vrlayer-compositor PRIVATE VRLAYER_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/vrlayer")

  add_executable(vrlayer-ctl tools/vrlayer-ctl.cpp)
  target_link_libraries(vrlayer-ctl PRIVATE vrlayer_common)

  install(TARGETS vrlayer-compositor vrlayer-ctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(PROGRAMS tools/vrlayer-run DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(DIRECTORY profiles/ DESTINATION ${CMAKE_INSTALL_DATADIR}/vrlayer/profiles)
  install(FILES packaging/70-vrlayer-uinput.rules DESTINATION ${CMAKE_INSTALL_DATADIR}/vrlayer)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(VRLAYER_BUILD_TESTS)
  enable_testing()

  add_executable(vrlayer_unit_tests tests/unit_tests.cpp compositor/vrmath.cpp)
  target_include_directories(vrlayer_unit_tests PRIVATE compositor)
  target_link_libraries(vrlayer_unit_tests PRIVATE vrlayer_host)
  target_compile_definitions(vrlayer_unit_tests PRIVATE VRLAYER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_test(NAME unit COMMAND vrlayer_unit_tests)

  add_executable(vrlayer_render_test tests/render_test.cpp)
  target_link_libraries(vrlayer_render_test PRIVATE vrlayer_host)
  add_test(NAME render COMMAND vrlayer_render_test)
  set_tests_properties(render PROPERTIES SKIP_RETURN_CODE 77)

  if(VRLAYER_BUILD_LAYER)
    set(TEST_SHADERS)
    vrlayer_shader(TEST_SHADERS tests/shaders/fullscreen.vert kFullscreenVertSpv)
    vrlayer_shader(TEST_SHADERS tests/shaders/fullscreen.frag kFullscreenFragSpv)
    add_executable(vrlayer_test_game tests/test_game.cpp ${TEST_SHADERS})
    target_include_directories(vrlayer_test_game PRIVATE ${CMAKE_BINARY_DIR}/shaders)
    target_link_libraries(vrlayer_test_game PRIVATE Vulkan::Vulkan)

    add_executable(vrlayer_e2e_test tests/e2e_test.cpp)
    target_link_libraries(vrlayer_e2e_test PRIVATE vrlayer_host)
    add_dependencies(vrlayer_e2e_test VkLayer_vrlayer vrlayer_test_game)
    add_executable(vrlayer_e2e_camera_test tests/e2e_camera_test.cpp)
    target_link_libraries(vrlayer_e2e_camera_test PRIVATE vrlayer_host)
    add_dependencies(vrlayer_e2e_camera_test VkLayer_vrlayer vrlayer_test_game)
    foreach(transport gpu cpu)
      add_test(NAME e2e_camera_${transport}
               COMMAND vrlayer_e2e_camera_test $<TARGET_FILE:vrlayer_test_game> ${CMAKE_BINARY_DIR}/devroot ${transport})
      set_tests_properties(e2e_camera_${transport} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    endforeach()

    foreach(transport gpu cpu)
      foreach(api renderpass dynamic)
        add_test(NAME e2e_${transport}_${api}
                 COMMAND vrlayer_e2e_test $<TARGET_FILE:vrlayer_test_game> ${CMAKE_BINARY_DIR}/devroot
                         ${transport} ${api})
        set_tests_properties(e2e_${transport}_${api} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
      endforeach()
    endforeach()

    if(VRLAYER_BUILD_COMPOSITOR)
      # Full pipeline through a real OpenXR runtime (Monado's simulated HMD).
      add_test(NAME xr_smoke COMMAND ${CMAKE_SOURCE_DIR}/tests/xr_smoke.sh ${CMAKE_BINARY_DIR})
      set_tests_properties(xr_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    endif()
  endif()
endif()
