cmake_minimum_required(VERSION 3.16)
project(optical_telemetry_agent VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(OTA_BUILD_TESTS "Build unit tests" ON)
set(OTA_SANITIZER "" CACHE STRING "Sanitizer to enable: address, thread or undefined")

if(OTA_SANITIZER)
  message(STATUS "Building with -fsanitize=${OTA_SANITIZER}")
  add_compile_options(-fsanitize=${OTA_SANITIZER} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${OTA_SANITIZER})
endif()

find_package(Threads REQUIRED)

add_library(ota_core
  src/types.cpp
  src/transceiver.cpp
  src/alarm_engine.cpp
  src/latency_histogram.cpp
  src/agent.cpp
)
target_include_directories(ota_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ota_core PUBLIC Threads::Threads)
target_compile_options(ota_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)

add_executable(ota-agent src/main.cpp)
target_link_libraries(ota-agent PRIVATE ota_core)
target_compile_options(ota-agent PRIVATE -Wall -Wextra -Wpedantic)

if(OTA_BUILD_TESTS)
  enable_testing()
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
      googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()

  add_executable(ota_tests
    tests/test_bounded_queue.cpp
    tests/test_alarm_engine.cpp
    tests/test_transceiver.cpp
    tests/test_latency_histogram.cpp
    tests/test_agent.cpp
  )
  target_link_libraries(ota_tests PRIVATE ota_core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(ota_tests DISCOVERY_TIMEOUT 60)
endif()
