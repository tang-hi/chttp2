set(test_sources
  src/frame/frame_test.cpp
  src/hpack/hpack_test.cpp
  src/hpack/huffman_test.cpp
  src/byte_buffer_test.cpp
  src/flow_test.cpp
  src/http2_protocol_test.cpp
)

find_package(Catch2 REQUIRED)

foreach(file ${test_sources})
  string(REGEX REPLACE "(.*/)([a-zA-Z0-9_ ]+)(\.cpp)" "\\2" test_name ${file})
  add_executable(${test_name}_Tests ${file})
  target_compile_features(${test_name}_Tests PRIVATE cxx_std_11)
  target_link_libraries(${test_name}_Tests PRIVATE Catch2::Catch2 Catch2::Catch2WithMain chttp2)
  add_test(NAME ${test_name} COMMAND ${test_name}_Tests)
endforeach()

#
# E2E tests (requires Go)
#

if(chttp2_BUILD_E2E_TESTS)
  find_program(GO_EXECUTABLE go)
  if(NOT GO_EXECUTABLE)
    message(FATAL_ERROR "Go is required for E2E tests but was not found.")
  endif()

  set(TESTSERVER_DIR "${CMAKE_SOURCE_DIR}/test/e2e/testserver")
  set(TESTSERVER_BIN "${CMAKE_BINARY_DIR}/testserver")

  add_custom_command(
    OUTPUT ${TESTSERVER_BIN}
    COMMAND ${GO_EXECUTABLE} build -o ${TESTSERVER_BIN} .
    WORKING_DIRECTORY ${TESTSERVER_DIR}
    DEPENDS ${TESTSERVER_DIR}/main.go
    COMMENT "Building Go E2E test server"
  )
  add_custom_target(testserver DEPENDS ${TESTSERVER_BIN})

  # Helper: add an E2E test executable with common settings.
  function(add_e2e_target name source)
    add_executable(${name} ${source})
    target_compile_features(${name} PRIVATE cxx_std_11)
    target_include_directories(${name} PRIVATE
      ${CMAKE_SOURCE_DIR}/test/e2e
      ${CMAKE_SOURCE_DIR}/include
    )
    target_compile_definitions(${name} PRIVATE TESTSERVER_BIN="${TESTSERVER_BIN}")
    add_dependencies(${name} testserver)
  endfunction()

  # E2E test suite
  add_e2e_target(e2e_Tests ${CMAKE_SOURCE_DIR}/test/e2e/e2e_test.cpp)
  target_link_libraries(e2e_Tests PRIVATE Catch2::Catch2 Catch2::Catch2WithMain chttp2)
  add_test(NAME e2e COMMAND e2e_Tests)

  # Chaos gate test (long-running)
  add_e2e_target(chaos_Tests ${CMAKE_SOURCE_DIR}/test/e2e/chaos_test.cpp)
  target_compile_options(chaos_Tests PRIVATE -O2)
  target_link_libraries(chaos_Tests PRIVATE Catch2::Catch2 Catch2::Catch2WithMain chttp2)
  add_test(NAME chaos COMMAND chaos_Tests)

  # Benchmark (standalone, no Catch2)
  add_e2e_target(chttp2_bench ${CMAKE_SOURCE_DIR}/test/e2e/bench.cpp)
  target_compile_options(chttp2_bench PRIVATE -O2)
  target_link_libraries(chttp2_bench PRIVATE chttp2)
endif()

#
# Amalgamation test
#

set(AMALG_DIR "${CMAKE_SOURCE_DIR}/single_include")
if(EXISTS "${AMALG_DIR}/chttp2.hpp")
  set(amalg_test_sources "")
  foreach(file ${test_sources})
    list(APPEND amalg_test_sources "${CMAKE_SOURCE_DIR}/test/${file}")
  endforeach()

  add_executable(amalg_Tests
    ${CMAKE_SOURCE_DIR}/test/amalgamate_impl.cpp
    ${amalg_test_sources}
  )
  target_compile_features(amalg_Tests PRIVATE cxx_std_11)
  target_include_directories(amalg_Tests PRIVATE
    ${AMALG_DIR}
    ${CMAKE_SOURCE_DIR}/test/src
    ${CMAKE_SOURCE_DIR}/test/src/hpack
  )
  target_link_libraries(amalg_Tests PRIVATE
    Catch2::Catch2
    Catch2::Catch2WithMain
    OpenSSL::SSL
    OpenSSL::Crypto
  )
  add_test(NAME amalgamate COMMAND amalg_Tests)
endif()
