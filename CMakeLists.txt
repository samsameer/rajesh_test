cmake_minimum_required(VERSION 3.13)
project(sensor_fusion VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

set(SF_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
                -Wmissing-prototypes -Wdouble-promotion)

add_library(sensorfusion STATIC
  src/spsc_queue.c
  src/clock.c
  src/backoff.c
  src/fusion.c
  src/sensor.c
  src/aggregator.c
  src/writer.c
)
target_include_directories(sensorfusion PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(sensorfusion PUBLIC _POSIX_C_SOURCE=200809L)
target_compile_options(sensorfusion PRIVATE ${SF_WARNINGS})
target_link_libraries(sensorfusion PUBLIC Threads::Threads m)

add_executable(sensor_fusion src/main.c)
target_compile_options(sensor_fusion PRIVATE ${SF_WARNINGS})
target_link_libraries(sensor_fusion PRIVATE sensorfusion)

add_executable(fusion_file tools/fusion_file.c)
target_compile_options(fusion_file PRIVATE ${SF_WARNINGS})
target_link_libraries(fusion_file PRIVATE sensorfusion)

enable_testing()

foreach(test_name test_fusion test_pipeline)
  add_executable(${test_name} tests/${test_name}.c)
  target_compile_options(${test_name} PRIVATE ${SF_WARNINGS})
  target_link_libraries(${test_name} PRIVATE sensorfusion)
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

add_test(NAME fusion_file_samples
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_file_tests.sh $<TARGET_FILE:fusion_file>)
