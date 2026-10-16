cmake_minimum_required(VERSION 3.20)
project(shaped-haven-client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Ogre3D 14.6.1 SDK root. Override with -DOGRE_SDK_DIR=<path>.
set(OGRE_SDK_DIR "D:/src/ogre3d" CACHE PATH "Path to the Ogre3D SDK root")
find_package(OGRE 14.6.1 REQUIRED CONFIG PATHS "${OGRE_SDK_DIR}/CMake" NO_DEFAULT_PATH)

set(HAVEN_SOURCES
  src/entity_frame.cpp
  src/game_session.cpp
  src/hud.cpp
  src/hud_model.cpp
  src/hud_text.cpp
  src/input_state.cpp
  src/interpolation.cpp
  src/item_catalog.cpp
  src/mesh_factory.cpp
  src/net_client.cpp
  src/prediction.cpp
  src/protocol.cpp
  src/renderer.cpp
  src/run_events.cpp
  src/snapshot_state.cpp)

add_executable(shaped-haven-client src/main.cpp ${HAVEN_SOURCES})

target_include_directories(shaped-haven-client PRIVATE ${OGRE_INCLUDE_DIRS} src)
target_link_libraries(shaped-haven-client PRIVATE
  OgreMain
  OgreBites
  OgreOverlay
  OgreRTShaderSystem
  RenderSystem_GL3Plus
  Codec_STBI
  ws2_32)

# The SDK ships SDL2.dll and the Ogre DLLs, but not the SDL headers, so the
# render system plugin is installed directly and the RTShaderSystem's shader
# headers are located from the SDK media directory.
target_compile_definitions(shaped-haven-client PRIVATE
  OGRE_MEDIA_DIR_STRING="${OGRE_MEDIA_DIR}"
  OGRE_PLUGIN_DIR_STRING="${OGRE_PLUGIN_DIR}")

set_target_properties(shaped-haven-client PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ---------------------------------------------------------------------------
# Tests: the prediction / interpolation / protocol logic is Ogre-free.
# ---------------------------------------------------------------------------
enable_testing()

add_executable(shaped-haven-client-tests tests/test_logic.cpp
  src/entity_frame.cpp
  src/hud_model.cpp
  src/hud_text.cpp
  src/input_state.cpp
  src/interpolation.cpp
  src/item_catalog.cpp
  src/prediction.cpp
  src/protocol.cpp
  src/run_events.cpp
  src/snapshot_state.cpp)

target_include_directories(shaped-haven-client-tests PRIVATE src)

add_test(NAME logic COMMAND shaped-haven-client-tests)

# The tests load their fixtures from `tests/data/` by relative path, so they have
# to run from the source directory rather than from the build tree ctest
# defaults to.
set_tests_properties(logic PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
