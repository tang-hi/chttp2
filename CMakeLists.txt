cmake_minimum_required(VERSION 3.15)

project(chttp2 VERSION 0.0.1 LANGUAGES CXX)

#
# Options
#

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  option(chttp2_ENABLE_UNIT_TESTING "Enable unit tests." ON)
else()
  option(chttp2_ENABLE_UNIT_TESTING "Enable unit tests." OFF)
endif()

option(chttp2_BUILD_E2E_TESTS    "Build end-to-end tests (requires Go)."  OFF)
option(chttp2_BUILD_TOOLS        "Build CLI tools (chttp2_get)."          OFF)
option(chttp2_BUILD_EXAMPLES     "Build example programs."                OFF)
option(chttp2_ENABLE_ASAN        "Enable AddressSanitizer."               OFF)
option(chttp2_ENABLE_TSAN        "Enable ThreadSanitizer."                OFF)
option(chttp2_ENABLE_DOXYGEN     "Enable Doxygen documentation."          OFF)
option(chttp2_ENABLE_CLANG_TIDY  "Enable clang-tidy."                     OFF)
option(chttp2_ENABLE_CPPCHECK    "Enable cppcheck."                       OFF)
option(chttp2_ENABLE_CODE_COVERAGE "Enable code coverage (GCC)."          OFF)
option(chttp2_WARNINGS_AS_ERRORS "Treat compiler warnings as errors."     ON)

#
# Configuration
#

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Debug")
endif()

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR "In-source builds not allowed.")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ccache
find_program(CCACHE ccache)
if(CCACHE)
  set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE})
endif()

# Sanitizers (mutually exclusive)
if(chttp2_ENABLE_ASAN AND chttp2_ENABLE_TSAN)
  message(FATAL_ERROR "ASan and TSan cannot be enabled simultaneously.")
endif()

# Static analyzers & Doxygen
include(cmake/StaticAnalyzers.cmake)
include(cmake/Doxygen.cmake)

message(STATUS "Started CMake for ${PROJECT_NAME} v${PROJECT_VERSION} [${CMAKE_BUILD_TYPE}]")

#
# Library
#

add_library(chttp2
  src/hpack/hpack.cpp
  src/hpack/table.cpp
  src/hpack/huffman.cpp
  src/client.cpp
  src/callback_dispatcher.cpp
  src/connection.cpp
  src/connection_manager.cpp
  src/flow.cpp
  src/frame_codec.cpp
  src/frame.cpp
  src/http2_session.cpp
  src/reactor.cpp
  src/request_context.cpp
  src/byte_buffer.cpp
  src/socket_fd.cpp
  src/connector.cpp
  src/stream_transport.cpp
  src/http2_protocol.cpp
  src/stream.cpp
)

add_library(chttp2::chttp2 ALIAS chttp2)

target_compile_features(chttp2 PUBLIC cxx_std_11)

target_include_directories(chttp2
  PUBLIC
    $<INSTALL_INTERFACE:include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Compiler warnings (PRIVATE — not leaked to downstream consumers)
include(cmake/CompilerWarnings.cmake)
set_project_warnings(chttp2)

# Sanitizer flags (PUBLIC so tests inherit them)
if(chttp2_ENABLE_ASAN)
  target_compile_options(chttp2 PUBLIC -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(chttp2 PUBLIC -fsanitize=address)
endif()

if(chttp2_ENABLE_TSAN)
  target_compile_options(chttp2 PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(chttp2 PUBLIC -fsanitize=thread)
endif()

# Code coverage
if(chttp2_ENABLE_CODE_COVERAGE)
  target_compile_options(chttp2 PUBLIC -O0 -g -fprofile-arcs -ftest-coverage)
  target_link_options(chttp2 PUBLIC -fprofile-arcs -ftest-coverage)
endif()

# Dependencies
find_package(OpenSSL REQUIRED)
target_link_libraries(chttp2 PUBLIC OpenSSL::SSL OpenSSL::Crypto)

set_target_properties(chttp2 PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

#
# Unit tests
#

if(chttp2_ENABLE_UNIT_TESTING)
  enable_testing()
  add_subdirectory(test)
endif()

#
# CLI tools
#

if(chttp2_BUILD_TOOLS)
  add_executable(chttp2_get tools/chttp2_get.cpp)
  target_compile_features(chttp2_get PRIVATE cxx_std_11)
  target_link_libraries(chttp2_get PRIVATE chttp2)
  set_target_properties(chttp2_get PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  )
endif()

#
# Examples
#

if(chttp2_BUILD_EXAMPLES)
  set(EXAMPLE_SOURCES
    examples/simple_get.cpp
    examples/post_json.cpp
    examples/async_requests.cpp
    examples/timeout_cancel.cpp
    examples/client_config.cpp
    examples/unix_socket.cpp
  )
  foreach(example_src ${EXAMPLE_SOURCES})
    get_filename_component(example_name ${example_src} NAME_WE)
    add_executable(${example_name} ${example_src})
    target_compile_features(${example_name} PRIVATE cxx_std_11)
    target_link_libraries(${example_name} PRIVATE chttp2)
    set_target_properties(${example_name} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
  endforeach()
endif()

#
# clang-format target
#

find_program(CLANG_FORMAT clang-format)
if(CLANG_FORMAT)
  file(GLOB_RECURSE ALL_CXX_SOURCES
    ${CMAKE_SOURCE_DIR}/src/*.cpp
    ${CMAKE_SOURCE_DIR}/include/*.hpp
  )
  add_custom_target(clang-format
    COMMAND ${CLANG_FORMAT} -i ${ALL_CXX_SOURCES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running clang-format"
  )
endif()

#
# Installation
#

include(GNUInstallDirs)

install(
  TARGETS chttp2
  EXPORT chttp2Targets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION include
)

install(
  EXPORT chttp2Targets
  FILE chttp2Targets.cmake
  NAMESPACE chttp2::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/chttp2
)

# Version header
string(TOUPPER ${PROJECT_NAME} PROJECT_NAME_UPPERCASE)
configure_file(
  ${CMAKE_CURRENT_LIST_DIR}/cmake/version.hpp.in
  include/chttp2/version.hpp
  @ONLY
)

install(
  FILES ${CMAKE_CURRENT_BINARY_DIR}/include/chttp2/version.hpp
  DESTINATION include/chttp2
)

install(
  DIRECTORY include/chttp2
  DESTINATION include
)

# Package config
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
  chttp2ConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
  ${CMAKE_CURRENT_LIST_DIR}/cmake/chttp2Config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/chttp2Config.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/chttp2
)

install(
  FILES
    ${CMAKE_CURRENT_BINARY_DIR}/chttp2Config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/chttp2ConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/chttp2
)
