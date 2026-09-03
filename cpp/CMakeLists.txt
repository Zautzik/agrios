cmake_minimum_required(VERSION 3.16)
project(agrios_bridge CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin"))
  message(FATAL_ERROR
    "agrios_bridge uses POSIX shared memory (shm_open/mmap) and is only "
    "meaningful on Linux/macOS -- it targets the same Linux boxes and "
    "Raspberry Pi / Jetson boards the rest of the project runs on. Build "
    "under WSL, a Linux container, or a real Linux/macOS host.")
endif()

add_compile_options(-Wall -Wextra)

find_package(Threads REQUIRED)

# Shared library exposing the plain-C ABI Python's ctypes bridge calls into.
add_library(agrios_bridge SHARED
  src/bridge_c_api.cpp
)
target_include_directories(agrios_bridge PUBLIC include)
target_link_libraries(agrios_bridge PUBLIC rt Threads::Threads)

# Demo: the "C++ motor control" consumer.
add_executable(motor_control_consumer src/motor_control_consumer.cpp)
target_link_libraries(motor_control_consumer PRIVATE agrios_bridge)

# Correctness test for the lock-free algorithm itself (no shared memory
# involved -- two threads in one process). Run manually with TSan for real
# signal; see the comment at the top of the test file.
add_executable(spsc_ring_buffer_test tests/spsc_ring_buffer_test.cpp)
target_include_directories(spsc_ring_buffer_test PRIVATE include)
target_link_libraries(spsc_ring_buffer_test PRIVATE Threads::Threads)

enable_testing()
add_test(NAME spsc_ring_buffer_test COMMAND spsc_ring_buffer_test)
