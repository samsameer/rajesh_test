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
)
target_include_directories(sensorfusion PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(sensorfusion PUBLIC _POSIX_C_SOURCE=200809L)
target_compile_options(sensorfusion PRIVATE ${SF_WARNINGS})
target_link_libraries(sensorfusion PUBLIC Threads::Threads m)
