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

add_executable(shaped-haven-client src/main.cpp)

target_include_directories(shaped-haven-client PRIVATE ${OGRE_INCLUDE_DIRS})
target_link_libraries(shaped-haven-client PRIVATE OgreMain)

set_target_properties(shaped-haven-client PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
