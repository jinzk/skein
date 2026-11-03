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
  compression.cpp
  cookie.cpp
  diagnostics.cpp
  encoding.cpp
  error.cpp
  eval.cpp
  eval_context.cpp
  event_loop.cpp
  fiber.cpp
  filetype.cpp
  format.cpp
  generator.cpp
  http.cpp
  http1_parser.cpp
  io.cpp
  json.cpp
  main.cpp
  net.cpp
  package.cpp
  repl.cpp
  result.cpp
  scopestack.cpp
  structeval.cpp
  structtype.cpp
  symbol.cpp
  task.cpp
  unicode.cpp
  unicode_ident.cpp
  valuetype.cpp
  websocket.cpp
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

target_include_directories(${PROJECT_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(${PROJECT_NAME} PRIVATE
  ${SKEIN_LIBUV_TARGET}
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
  else()
    message(WARNING "Python3 not found; skein_tests will not be registered")
  endif()
endif()
