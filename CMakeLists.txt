cmake_minimum_required(VERSION 3.25)
project(MooSwitcher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

find_package(Threads REQUIRED)
find_package(NDI)

add_library(moo_core STATIC
  src/core/MediaClock.cpp
  src/core/Stats.cpp
  src/engine/SwitcherCore.cpp
)
target_include_directories(moo_core PUBLIC src)
target_link_libraries(moo_core PUBLIC Threads::Threads)
target_compile_options(moo_core PRIVATE -Wall -Wextra)

if(NDI_FOUND)
  add_library(moo_ndi STATIC src/ndi/NdiLib.cpp)
  target_link_libraries(moo_ndi PUBLIC moo_core NDI::ndi)
  target_compile_options(moo_ndi PRIVATE -Wall -Wextra)

  get_filename_component(_ndi_libdir ${NDI_LIBRARY} DIRECTORY)

  foreach(tool moo-testgen moo-latmeter)
    add_executable(${tool} tools/${tool}/main.cpp)
    target_link_libraries(${tool} PRIVATE moo_ndi)
    target_include_directories(${tool} PRIVATE tools)
    target_compile_options(${tool} PRIVATE -Wall -Wextra)
    # Locate libndi.so.6 next to the SDK without requiring ldconfig.
    set_target_properties(${tool} PROPERTIES
      BUILD_RPATH ${_ndi_libdir}
      INSTALL_RPATH ${_ndi_libdir})
  endforeach()
else()
  message(WARNING "NDI SDK not found - NDI tools will not be built. "
                  "Set NDI_SDK_DIR or populate third_party/ndi.")
endif()

option(MOO_BUILD_TESTS "Build unit tests" ON)
if(MOO_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.8.0
  )
  FetchContent_MakeAvailable(Catch2)

  enable_testing()
  add_executable(moo_tests
    tests/clock_test.cpp
    tests/ring_test.cpp
    tests/switcher_core_test.cpp
    tests/strip_test.cpp
  )
  target_link_libraries(moo_tests PRIVATE moo_core Catch2::Catch2WithMain)
  target_include_directories(moo_tests PRIVATE tools)

  list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
  include(Catch)
  catch_discover_tests(moo_tests)
endif()
