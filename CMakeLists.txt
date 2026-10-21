cmake_minimum_required(VERSION 3.16)
project(plagiarism_engine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)

# libclang (C API). Override with -DLIBCLANG_ROOT=/path/to/llvm if needed.
find_program(LLVM_CONFIG NAMES llvm-config HINTS ${LIBCLANG_ROOT}/bin)
if(LLVM_CONFIG)
  execute_process(COMMAND ${LLVM_CONFIG} --includedir OUTPUT_VARIABLE LLVM_INC OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND ${LLVM_CONFIG} --libdir OUTPUT_VARIABLE LLVM_LIB OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
find_path(LIBCLANG_INCLUDE_DIR clang-c/Index.h HINTS ${LIBCLANG_ROOT}/include ${LLVM_INC})
find_library(LIBCLANG_LIBRARY NAMES clang libclang HINTS ${LIBCLANG_ROOT}/lib ${LLVM_LIB})
if(NOT LIBCLANG_INCLUDE_DIR OR NOT LIBCLANG_LIBRARY)
  message(FATAL_ERROR "libclang not found (install clang / set LIBCLANG_ROOT)")
endif()
message(STATUS "libclang: ${LIBCLANG_LIBRARY}")

add_library(plag
  src/clang_tokenizer.cpp
  src/engine.cpp
  src/fingerprint.cpp
  src/index.cpp
  src/std_stub.cpp
  src/token_cache.cpp
)
target_include_directories(plag PUBLIC include ${LIBCLANG_INCLUDE_DIR})
target_link_libraries(plag PUBLIC ${LIBCLANG_LIBRARY} Threads::Threads)

add_executable(plag_cli tools/plag.cpp)
set_target_properties(plag_cli PROPERTIES OUTPUT_NAME plag)
target_link_libraries(plag_cli PRIVATE plag)

option(PLAG_BUILD_TESTS "Build unit tests" ON)
if(PLAG_BUILD_TESTS)
  find_package(GTest REQUIRED)
  enable_testing()
  add_executable(plag_tests
    tests/fingerprint_test.cpp
    tests/index_test.cpp
    tests/thread_pool_test.cpp
    tests/tokenizer_test.cpp
  )
  target_link_libraries(plag_tests PRIVATE plag GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(plag_tests)
endif()
