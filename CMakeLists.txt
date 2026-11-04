cmake_minimum_required(VERSION 3.10)
project(skein LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

find_package(BISON REQUIRED)
find_package(FLEX REQUIRED)

BISON_TARGET(MyParser parser.y ${CMAKE_CURRENT_BINARY_DIR}/parser.cpp)
FLEX_TARGET(MyScanner tokens.l ${CMAKE_CURRENT_BINARY_DIR}/tokens.cpp)
ADD_FLEX_BISON_DEPENDENCY(MyScanner MyParser)

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(LIBUV QUIET IMPORTED_TARGET libuv)
endif()
if(TARGET PkgConfig::LIBUV)
  set(SKEIN_LIBUV_TARGET PkgConfig::LIBUV)
else()
  find_path(LIBUV_INCLUDE_DIR uv.h)
  find_library(LIBUV_LIBRARY NAMES uv libuv)
  if(LIBUV_INCLUDE_DIR AND LIBUV_LIBRARY)
    add_library(skein_libuv UNKNOWN IMPORTED)
    set_target_properties(skein_libuv PROPERTIES
      IMPORTED_LOCATION "${LIBUV_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${LIBUV_INCLUDE_DIR}"
    )
    set(SKEIN_LIBUV_TARGET skein_libuv)
  endif()
endif()
if(NOT SKEIN_LIBUV_TARGET)
  message(FATAL_ERROR "libuv not found; install libuv development files")
endif()

if(PKG_CONFIG_FOUND)
  pkg_check_modules(PCRE2 QUIET IMPORTED_TARGET libpcre2-8)
endif()
if(TARGET PkgConfig::PCRE2)
  set(SKEIN_PCRE2_TARGET PkgConfig::PCRE2)
else()
  find_path(PCRE2_INCLUDE_DIR pcre2.h)
  find_library(PCRE2_LIBRARY NAMES pcre2-8 libpcre2-8)
  if(PCRE2_INCLUDE_DIR AND PCRE2_LIBRARY)
    add_library(skein_pcre2 UNKNOWN IMPORTED)
    set_target_properties(skein_pcre2 PROPERTIES
      IMPORTED_LOCATION "${PCRE2_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${PCRE2_INCLUDE_DIR}"
    )
    set(SKEIN_PCRE2_TARGET skein_pcre2)
  endif()
endif()
if(NOT SKEIN_PCRE2_TARGET)
  message(FATAL_ERROR
    "PCRE2 8-bit not found; install libpcre2-dev or the pcre2 package")
endif()

find_package(Boost CONFIG QUIET COMPONENTS context)
if(NOT Boost_FOUND)
  find_package(Boost REQUIRED COMPONENTS context)
endif()

add_library(skein_llhttp STATIC
  third_party/llhttp/src/llhttp.c
  third_party/llhttp/src/api.c
  third_party/llhttp/src/http.c
)
target_include_directories(skein_llhttp PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/llhttp/include
)
set_target_properties(skein_llhttp PROPERTIES
  C_STANDARD 99
  C_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE ON
)

add_library(skein_utf8proc STATIC
  third_party/utf8proc/utf8proc.c
)
target_include_directories(skein_utf8proc PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/utf8proc
)
target_compile_definitions(skein_utf8proc PUBLIC UTF8PROC_STATIC=1)
set_target_properties(skein_utf8proc PROPERTIES
  C_STANDARD 99
  C_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE ON
)

add_executable(${PROJECT_NAME}
  ${BISON_MyParser_OUTPUTS}
  ${FLEX_MyScanner_OUTPUTS}
  ast.cpp
  async_support.cpp
  builtins.cpp
  compress_builtin.cpp
  compression.cpp
  cookie.cpp
  crypto_builtin.cpp
  cycle.cpp
  diagnostics.cpp
  dict.cpp
  encoding.cpp
  env_builtin.cpp
  error.cpp
  eval.cpp
  eval_context.cpp
  event_loop.cpp
  fiber.cpp
  filetype.cpp
  format.cpp
  fs_extra.cpp
  gc.cpp
  generator.cpp
  http.cpp
  http1_parser.cpp
  io.cpp
  json.cpp
  main.cpp
  net.cpp
  package.cpp
  pattern.cpp
  process_builtin.cpp
  process_spawn.cpp
  random_builtin.cpp
  regex_builtin.cpp
  repl.cpp
  result.cpp
  scopestack.cpp
  serialization.cpp
  source_encoding.cpp
  stdio_stream.cpp
  structeval.cpp
  structtype.cpp
  symbol.cpp
  task.cpp
  time_builtin.cpp
  unicode.cpp
  unicode_ident.cpp
  valuetype.cpp
  websocket.cpp
  worker_pool.cpp
)

if(WIN32)
  target_sources(${PROJECT_NAME} PRIVATE platform_win.cpp)
  target_compile_definitions(${PROJECT_NAME} PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
  )
  if(MINGW)
    # Ensure C99 printf length modifiers such as %lld work with MinGW.
    target_compile_definitions(${PROJECT_NAME} PRIVATE
      __USE_MINGW_ANSI_STDIO=1
    )
  endif()
else()
  target_sources(${PROJECT_NAME} PRIVATE platform_posix.cpp)
endif()

# Embedded Skein standard-library packages.  Each .skein source is turned into
# a C string at build time and imported in memory by package.cpp.
set(SKEIN_EMBEDDED_DIR ${CMAKE_CURRENT_BINARY_DIR}/embedded)
set(SKEIN_EMBEDDED_SOURCES
  fsm.skein:kStdFsmSource:std_fsm_source.inc
  set.skein:kStdSetSource:std_set_source.inc
  deque.skein:kStdDequeSource:std_deque_source.inc
  heap.skein:kStdHeapSource:std_heap_source.inc
  route.skein:kStdRouteSource:std_route_source.inc
  graph.skein:kStdGraphSource:std_graph_source.inc
  arena.skein:kStdArenaSource:std_arena_source.inc
)
set(SKEIN_EMBEDDED_OUTPUTS)
foreach(entry IN LISTS SKEIN_EMBEDDED_SOURCES)
  string(REPLACE ":" ";" parts "${entry}")
  list(GET parts 0 source_name)
  list(GET parts 1 symbol)
  list(GET parts 2 output_name)
  set(source_path ${CMAKE_CURRENT_SOURCE_DIR}/lib/std/${source_name})
  set(output_path ${SKEIN_EMBEDDED_DIR}/${output_name})
  add_custom_command(
    OUTPUT ${output_path}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SKEIN_EMBEDDED_DIR}
    COMMAND ${CMAKE_COMMAND}
      -DINPUT=${source_path}
      -DOUTPUT=${output_path}
      -DSYMBOL=${symbol}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_source.cmake
    DEPENDS
      ${source_path}
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_source.cmake
    VERBATIM
  )
  list(APPEND SKEIN_EMBEDDED_OUTPUTS ${output_path})
endforeach()
add_custom_target(skein_embedded_sources DEPENDS ${SKEIN_EMBEDDED_OUTPUTS})
add_dependencies(${PROJECT_NAME} skein_embedded_sources)

target_include_directories(${PROJECT_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
  ${SKEIN_EMBEDDED_DIR}
)
target_link_libraries(${PROJECT_NAME} PRIVATE
  ${SKEIN_LIBUV_TARGET}
  ${SKEIN_PCRE2_TARGET}
  Boost::context
  skein_llhttp
  skein_utf8proc
)

option(SKEIN_ENABLE_TLS "Enable TLS support (requires OpenSSL)" ON)
if(SKEIN_ENABLE_TLS)
  find_package(OpenSSL QUIET)
  if(OpenSSL_FOUND)
    target_sources(${PROJECT_NAME} PRIVATE tls.cpp)
    target_compile_definitions(${PROJECT_NAME} PRIVATE SKEIN_ENABLE_TLS=1)
    target_link_libraries(${PROJECT_NAME} PRIVATE
      OpenSSL::SSL
      OpenSSL::Crypto
    )
  else()
    message(STATUS "OpenSSL not found; std.tls is disabled")
  endif()
endif()

option(SKEIN_ENABLE_COMPRESSION "Enable gzip/deflate/brotli response decompression" ON)
if(SKEIN_ENABLE_COMPRESSION)
  find_package(ZLIB QUIET)
  if(ZLIB_FOUND)
    target_compile_definitions(${PROJECT_NAME} PRIVATE SKEIN_ENABLE_ZLIB=1)
    target_link_libraries(${PROJECT_NAME} PRIVATE ZLIB::ZLIB)
  else()
    message(STATUS "zlib not found; gzip/deflate decompression is disabled")
  endif()
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(BROTLIDEC QUIET IMPORTED_TARGET libbrotlidec)
  endif()
  if(BROTLIDEC_FOUND)
    target_compile_definitions(${PROJECT_NAME} PRIVATE SKEIN_ENABLE_BROTLI=1)
    target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::BROTLIDEC)
  else()
    message(STATUS "brotli decoder not found; br decompression is disabled")
  endif()
endif()

option(SKEIN_ENABLE_HTTP2 "Enable the optional std.http2 module (requires nghttp2)" ON)
if(SKEIN_ENABLE_HTTP2)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(NGHTTP2 QUIET IMPORTED_TARGET libnghttp2)
  endif()
  if(NGHTTP2_FOUND)
    target_sources(${PROJECT_NAME} PRIVATE http2.cpp)
    target_compile_definitions(${PROJECT_NAME} PRIVATE SKEIN_ENABLE_HTTP2=1)
    target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::NGHTTP2)
  else()
    message(STATUS "nghttp2 not found; std.http2 is disabled")
  endif()
endif()

option(SKEIN_ENABLE_FFI "Enable the optional std.ffi module (requires libffi)" ON)
if(SKEIN_ENABLE_FFI)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(LIBFFI QUIET IMPORTED_TARGET libffi)
  endif()
  if(LIBFFI_FOUND)
    target_sources(${PROJECT_NAME} PRIVATE ffi.cpp)
    target_compile_definitions(${PROJECT_NAME} PRIVATE SKEIN_ENABLE_FFI=1)
    target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::LIBFFI)
  else()
    message(STATUS "libffi not found; std.ffi is disabled")
  endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
endif()

if(UNIX)
  target_link_libraries(${PROJECT_NAME} PRIVATE m)
endif()

if(WIN32)
  target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 shell32)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(http1_parser_test
    tests/test_http1_parser.cpp
    http1_parser.cpp
  )
  target_link_libraries(http1_parser_test PRIVATE skein_llhttp)
  target_include_directories(http1_parser_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
  )
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(http1_parser_test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME http1_parser_test COMMAND http1_parser_test)

  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    if(SKEIN_ENABLE_FFI AND LIBFFI_FOUND)
      add_library(skein_ffi_shim SHARED tests/ffi_shim.cpp)
    endif()
    set(SKEIN_TEST_COMMAND
      ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_skein.py
      $<TARGET_FILE:${PROJECT_NAME}>
    )
    if(TARGET skein_ffi_shim)
      list(APPEND SKEIN_TEST_COMMAND $<TARGET_FILE:skein_ffi_shim>)
    endif()
    add_test(NAME skein_tests COMMAND ${SKEIN_TEST_COMMAND})
    add_test(
      NAME encoding_tables_test
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_encoding_tables.py
              --check
    )
    add_test(
      NAME docs_sync_test
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_docs_sync.py
    )
    add_test(
      NAME container_bench_test
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tools/bench_containers.py
              --bin $<TARGET_FILE:${PROJECT_NAME}>
              --check
    )
    set_tests_properties(container_bench_test PROPERTIES
      TIMEOUT 180
      RUN_SERIAL TRUE
    )
  else()
    message(WARNING "Python3 not found; skein_tests will not be registered")
  endif()
endif()
