cmake_minimum_required(VERSION 3.10)
project(skein LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SKEIN_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
option(SKEIN_VM_ONLY "Build without the AST reference backend" OFF)
option(SKEIN_ENABLE_JIT "Enable the experimental asmjit-backed JIT" OFF)
if(SKEIN_VM_ONLY)
  add_compile_definitions(SKEIN_VM_ONLY=1)
endif()
if(SKEIN_SANITIZE)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(SKEIN_SANITIZE_FLAGS "-fsanitize=address,undefined -fno-omit-frame-pointer")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${SKEIN_SANITIZE_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SKEIN_SANITIZE_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS
        "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address,undefined")
    set(CMAKE_SHARED_LINKER_FLAGS
        "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address,undefined")
  else()
    message(FATAL_ERROR "SKEIN_SANITIZE requires GCC or Clang")
  endif()
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

find_package(BISON REQUIRED)
find_package(FLEX REQUIRED)

BISON_TARGET(MyParser src/interpreter/parser.y
  ${CMAKE_CURRENT_BINARY_DIR}/parser.cpp)
FLEX_TARGET(MyScanner src/interpreter/tokens.l
  ${CMAKE_CURRENT_BINARY_DIR}/tokens.cpp)
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

set(SKEIN_EMBEDDED_DIR ${CMAKE_CURRENT_BINARY_DIR}/embedded)

set(SKEIN_PLATFORM_SOURCES
  src/platform/dynamic_library.cpp
  src/platform/runtime_state.cpp
)
if(WIN32)
  list(APPEND SKEIN_PLATFORM_SOURCES src/platform/platform_win.cpp)
else()
  list(APPEND SKEIN_PLATFORM_SOURCES src/platform/platform_posix.cpp)
endif()
add_library(skein_platform STATIC ${SKEIN_PLATFORM_SOURCES})
set_target_properties(skein_platform PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(skein_platform PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
  ${CMAKE_CURRENT_BINARY_DIR}
)
if(WIN32)
  target_compile_definitions(skein_platform PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
  )
  if(MINGW)
    # Ensure C99 printf length modifiers such as %lld work with MinGW.
    target_compile_definitions(skein_platform PRIVATE
      __USE_MINGW_ANSI_STDIO=1
    )
  endif()
endif()

if(CMAKE_DL_LIBS)
  target_link_libraries(skein_platform PUBLIC ${CMAKE_DL_LIBS})
endif()

add_library(skein_runtime STATIC
  src/runtime/valuetype.cpp
  src/runtime/value_ops.cpp
  src/runtime/dict.cpp
  src/runtime/structtype.cpp
  src/runtime/symbol.cpp
  src/runtime/cycle.cpp
  src/runtime/serialization.cpp
  src/runtime/typeinfo.cpp
  src/runtime/json.cpp
  src/runtime/format.cpp
  src/runtime/encoding.cpp
  src/runtime/unicode.cpp
  src/runtime/unicode_ident.cpp
  src/runtime/stdio_stream.cpp
  src/runtime/runtime_context.cpp
  src/runtime/runtime_error.cpp
  src/runtime/binding_registry.cpp
)
set_target_properties(skein_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(skein_runtime PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime
  ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
  ${CMAKE_CURRENT_BINARY_DIR}
)
target_include_directories(skein_runtime PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/data
)
target_link_libraries(skein_runtime PUBLIC
  skein_platform
  skein_utf8proc
)

# Bytecode representation, disassembly and verification.  This target must
# stay free of AST/evaluator dependencies so serialization or a future AOT
# tool can link it without the interpreter.
add_library(skein_bytecode STATIC
  src/interpreter/bytecode.cpp
  src/interpreter/verifier.cpp
)
set_target_properties(skein_bytecode PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(skein_bytecode PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/interpreter
  ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime
  ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
  ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(skein_bytecode PUBLIC skein_runtime)

# Value/symbol/index mutation helpers and the scope/symbol tables.  These
# operate on runtime values only and must stay free of AST/evaluator
# dependencies so the VM executor can link them independently.
add_library(skein_value_runtime STATIC
  src/interpreter/value_runtime.cpp
  src/interpreter/scopestack.cpp
  src/interpreter/eval_context.cpp
  src/interpreter/control_runtime.cpp
  src/interpreter/vm_eval_runtime.cpp
  src/interpreter/gc.cpp
  src/interpreter/gc_roots.cpp
  src/interpreter/diagnostics.cpp
)
set_target_properties(skein_value_runtime PROPERTIES
  POSITION_INDEPENDENT_CODE ON)
target_include_directories(skein_value_runtime PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/interpreter
  ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime
  ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
  ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(skein_value_runtime PUBLIC skein_runtime)

# Optional JIT backend.  asmjit is a system dependency; see docs/jit-plan.md.
if(SKEIN_ENABLE_JIT)
  find_package(asmjit CONFIG REQUIRED)
  if(NOT TARGET asmjit::asmjit)
    message(FATAL_ERROR "asmjit found but asmjit::asmjit is missing")
  endif()

  add_library(skein_jit STATIC
    src/jit/jit.cpp
    src/jit/jit_helpers.cpp
  )
  set_target_properties(skein_jit PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(skein_jit PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/jit
  )
  target_link_libraries(skein_jit
    PUBLIC
      skein_bytecode
      skein_value_runtime
      skein_runtime
      skein_platform
    PRIVATE
      asmjit::asmjit
  )
  message(STATUS "JIT enabled (asmjit)")
endif()

# Bytecode executor.  Fiber/package/struct/pattern services and the evaluator
# call path are installed through vm_host.h by the interpreter, so this target
# links only bytecode/value runtime.
add_library(skein_vm STATIC
  src/interpreter/vm.cpp
  src/interpreter/vm_gc_roots.cpp
  src/interpreter/vm_host.cpp
)
set_target_properties(skein_vm PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(skein_vm PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/interpreter
  ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime
  ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
  ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(skein_vm PUBLIC
  skein_bytecode
  skein_value_runtime
  skein_runtime
)
if(SKEIN_ENABLE_JIT)
  target_link_libraries(skein_vm PUBLIC skein_jit)
  target_compile_definitions(skein_vm PRIVATE SKEIN_ENABLE_JIT=1)
endif()

add_library(skein_interpreter STATIC
  ${BISON_MyParser_OUTPUTS}
  ${FLEX_MyScanner_OUTPUTS}
  src/interpreter/ast.cpp
  src/interpreter/decorators.cpp
  src/interpreter/compiler.cpp
  src/interpreter/ast_analysis.cpp
  src/interpreter/defer_validation.cpp
  src/interpreter/vm_bridge.cpp
  src/interpreter/error_report.cpp
  src/interpreter/eval.cpp
  src/interpreter/eval_control.cpp
  src/interpreter/eval_call.cpp
  src/interpreter/eval_assign.cpp
  src/interpreter/package.cpp
  src/interpreter/std_namespace.cpp
  src/interpreter/pattern.cpp
  src/interpreter/structeval.cpp
  src/interpreter/fiber.cpp
  src/interpreter/generator.cpp
  src/interpreter/async_support.cpp
  src/interpreter/task.cpp
  src/interpreter/event_loop.cpp
  src/interpreter/repl.cpp
  src/interpreter/source_encoding.cpp
  src/interpreter/iter.cpp
)
set_target_properties(skein_interpreter PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(skein_interpreter PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/interpreter
  ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime
  ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
  ${CMAKE_CURRENT_BINARY_DIR}
  ${SKEIN_EMBEDDED_DIR}
)
target_link_libraries(skein_interpreter PUBLIC
  skein_vm
  skein_bytecode
  skein_value_runtime
  skein_runtime
  ${SKEIN_LIBUV_TARGET}
  Boost::context
)
if(SKEIN_ENABLE_JIT)
  target_compile_definitions(skein_interpreter PRIVATE SKEIN_ENABLE_JIT=1)
endif()
add_dependencies(skein_interpreter skein_embedded_sources)

add_library(skein_builtins STATIC
  src/builtins/builtins.cpp
  src/builtins/builtins_math.cpp
  src/builtins/builtins_list.cpp
  src/builtins/builtins_dict.cpp
  src/builtins/builtins_string.cpp
  src/builtins/builtins_convert.cpp
  src/builtins/builtins_file.cpp
  src/builtins/builtins_text.cpp
  src/builtins/builtins_bytes.cpp
  src/builtins/builtins_fn.cpp
  src/builtins/builtins_meta.cpp
  src/builtins/builtins_lru.cpp
  src/builtins/builtins_deque.cpp
  src/builtins/builtins_heap.cpp
  src/builtins/builtins_set.cpp
  src/builtins/compress_builtin.cpp
  src/builtins/compression.cpp
  src/builtins/cookie.cpp
  src/builtins/crypto_builtin.cpp
  src/builtins/env_builtin.cpp
  src/builtins/error.cpp
  src/builtins/filetype.cpp
  src/builtins/fs_extra.cpp
  src/builtins/http.cpp
  src/builtins/http1_parser.cpp
  src/builtins/io.cpp
  src/builtins/net.cpp
  src/builtins/process_builtin.cpp
  src/builtins/process_spawn.cpp
  src/builtins/random_builtin.cpp
  src/builtins/regex_builtin.cpp
  src/builtins/result.cpp
  src/builtins/time_builtin.cpp
  src/builtins/websocket.cpp
  src/builtins/worker_pool.cpp
  src/builtins/example_binding.cpp
  src/builtins/ffi/ffi_bindgen.cpp
)
set_target_properties(skein_builtins PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(skein_builtins PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/builtins
  ${CMAKE_CURRENT_SOURCE_DIR}/src/builtins/ffi
  ${CMAKE_CURRENT_SOURCE_DIR}/src/interpreter
  ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime
  ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
  ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(skein_builtins PUBLIC
  skein_interpreter
  skein_runtime
  skein_llhttp
  ${SKEIN_LIBUV_TARGET}
  ${SKEIN_PCRE2_TARGET}
  Boost::context
)

add_executable(${PROJECT_NAME} src/app/main.cpp)
target_include_directories(${PROJECT_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/app
  ${CMAKE_CURRENT_SOURCE_DIR}/src/builtins
  ${CMAKE_CURRENT_SOURCE_DIR}/src/builtins/ffi
  ${CMAKE_CURRENT_SOURCE_DIR}/src/interpreter
  ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime
  ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
  ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(${PROJECT_NAME} PRIVATE skein_builtins)

# Embedded Skein standard-library packages.  Each .skein source is turned into
# a C string at build time and imported in memory by package.cpp.
set(SKEIN_EMBEDDED_SOURCES
  fsm.skein:kStdFsmSource:std_fsm_source.inc
  set.skein:kStdSetSource:std_set_source.inc
  deque.skein:kStdDequeSource:std_deque_source.inc
  heap.skein:kStdHeapSource:std_heap_source.inc
  route.skein:kStdRouteSource:std_route_source.inc
  graph.skein:kStdGraphSource:std_graph_source.inc
  arena.skein:kStdArenaSource:std_arena_source.inc
  path.skein:kStdPathSource:std_path_source.inc
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
add_dependencies(skein_interpreter skein_embedded_sources)
# package.cpp includes the generated .inc files directly; make sure editing a
# lib/std/*.skein source rebuilds the object instead of reusing a stale string.
set_property(SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/src/interpreter/package.cpp
  APPEND PROPERTY OBJECT_DEPENDS "${SKEIN_EMBEDDED_OUTPUTS}")

# Private embedded sources are implementation details (not public packages),
# so they are intentionally excluded from check_docs_sync's package list.
set(SKEIN_PRIVATE_EMBEDDED_SOURCES
  internal/container_adapter.skein:kStdContainerAdapterSource:std_container_adapter_source.inc
)
set(SKEIN_PRIVATE_EMBEDDED_OUTPUTS)
foreach(entry IN LISTS SKEIN_PRIVATE_EMBEDDED_SOURCES)
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
  list(APPEND SKEIN_PRIVATE_EMBEDDED_OUTPUTS ${output_path})
endforeach()
add_custom_target(skein_private_embedded_sources
  DEPENDS ${SKEIN_PRIVATE_EMBEDDED_OUTPUTS})
add_dependencies(skein_interpreter skein_private_embedded_sources)
set_property(SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/src/interpreter/std_namespace.cpp
  APPEND PROPERTY OBJECT_DEPENDS "${SKEIN_PRIVATE_EMBEDDED_OUTPUTS}")

option(SKEIN_ENABLE_TLS "Enable TLS support (requires OpenSSL)" ON)
if(SKEIN_ENABLE_TLS)
  find_package(OpenSSL QUIET)
  if(OpenSSL_FOUND)
    target_sources(skein_builtins PRIVATE src/builtins/tls.cpp)
    target_compile_definitions(skein_builtins PRIVATE SKEIN_ENABLE_TLS=1)
    target_compile_definitions(skein_interpreter PRIVATE SKEIN_ENABLE_TLS=1)
    target_link_libraries(skein_builtins PUBLIC
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
    target_compile_definitions(skein_builtins PRIVATE SKEIN_ENABLE_ZLIB=1)
    target_link_libraries(skein_builtins PUBLIC ZLIB::ZLIB)
  else()
    message(STATUS "zlib not found; gzip/deflate decompression is disabled")
  endif()
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(BROTLIDEC QUIET IMPORTED_TARGET libbrotlidec)
  endif()
  if(BROTLIDEC_FOUND)
    target_compile_definitions(skein_builtins PRIVATE SKEIN_ENABLE_BROTLI=1)
    target_link_libraries(skein_builtins PUBLIC PkgConfig::BROTLIDEC)
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
    target_sources(skein_builtins PRIVATE src/builtins/http2.cpp)
    target_compile_definitions(skein_builtins PRIVATE SKEIN_ENABLE_HTTP2=1)
    target_compile_definitions(skein_interpreter PRIVATE SKEIN_ENABLE_HTTP2=1)
    target_link_libraries(skein_builtins PUBLIC PkgConfig::NGHTTP2)
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
    if(LIBFFI_VERSION VERSION_LESS 3.3)
      message(FATAL_ERROR
        "std.ffi requires libffi >= 3.3 for layout validation; "
        "use -DSKEIN_ENABLE_FFI=OFF or install a newer libffi")
    endif()
    target_sources(skein_builtins PRIVATE
                   src/builtins/ffi/ffi.cpp
                   src/builtins/ffi/ffi_types.cpp
                   src/builtins/ffi/ffi_value.cpp
                   src/builtins/ffi/ffi_marshal.cpp
                   src/builtins/ffi/ffi_callbacks.cpp
                   src/builtins/ffi/ffi_manifest.cpp
                   src/builtins/ffi/ffi_check.cpp)
    target_compile_definitions(skein_builtins PRIVATE SKEIN_ENABLE_FFI=1)
    target_compile_definitions(skein_interpreter PRIVATE SKEIN_ENABLE_FFI=1)
    target_compile_definitions(${PROJECT_NAME} PRIVATE SKEIN_ENABLE_FFI=1)
    target_link_libraries(skein_builtins PUBLIC PkgConfig::LIBFFI)
  else()
    message(STATUS "libffi not found; std.ffi is disabled")
  endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  foreach(skein_target skein_platform skein_runtime skein_bytecode
                       skein_value_runtime skein_interpreter skein_builtins
                       ${PROJECT_NAME})
    target_compile_options(${skein_target} PRIVATE -Wall -Wextra)
  endforeach()
endif()

if(UNIX)
  target_link_libraries(skein_runtime PUBLIC m)
endif()

if(WIN32)
  target_link_libraries(skein_builtins PUBLIC ws2_32 shell32)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(http1_parser_test
    tests/test_http1_parser.cpp
    src/builtins/http1_parser.cpp
  )
  target_link_libraries(http1_parser_test PRIVATE skein_llhttp)
  target_include_directories(http1_parser_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/builtins
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
  )
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(http1_parser_test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME http1_parser_test COMMAND http1_parser_test)
  set_tests_properties(http1_parser_test PROPERTIES LABELS "core")

  add_executable(bytecode_verifier_test
    tests/test_bytecode_verifier.cpp
  )
  target_link_libraries(bytecode_verifier_test PRIVATE skein_bytecode)
  target_include_directories(bytecode_verifier_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interpreter
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
  )
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(bytecode_verifier_test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME bytecode_verifier_test COMMAND bytecode_verifier_test)
  set_tests_properties(bytecode_verifier_test PROPERTIES LABELS "core")

  add_executable(runtime_link_test
    tests/test_runtime_link.cpp
  )
  target_link_libraries(runtime_link_test PRIVATE skein_runtime)
  target_include_directories(runtime_link_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
  )
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(runtime_link_test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME runtime_link_test COMMAND runtime_link_test)
  set_tests_properties(runtime_link_test PROPERTIES LABELS "core")

  add_executable(gc_roots_test
    tests/test_gc_roots.cpp
  )
  target_link_libraries(gc_roots_test PRIVATE skein_value_runtime)
  target_include_directories(gc_roots_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interpreter
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
  )
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(gc_roots_test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME gc_roots_test COMMAND gc_roots_test)
  set_tests_properties(gc_roots_test PROPERTIES LABELS "core")

  add_executable(vm_link_test
    tests/test_vm_link.cpp
  )
  target_link_libraries(vm_link_test PRIVATE skein_vm)
  target_include_directories(vm_link_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interpreter
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
  )
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(vm_link_test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME vm_link_test COMMAND vm_link_test)
  set_tests_properties(vm_link_test PROPERTIES LABELS "core")


  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    if(SKEIN_ENABLE_FFI AND LIBFFI_FOUND)
      add_library(skein_ffi_shim SHARED tests/ffi_shim.cpp)
    endif()
    add_library(skein_native_module SHARED tests/native_module.cpp)
    target_include_directories(skein_native_module PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime
      ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(skein_native_module PRIVATE -Wall -Wextra)
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
    add_test(NAME skein_vm_tests COMMAND ${SKEIN_TEST_COMMAND})
    set(SKEIN_AST_TEST_ENV "SKEIN_VM=0")
    if(SKEIN_SANITIZE)
      list(APPEND SKEIN_AST_TEST_ENV "SKEIN_TEST_TIMEOUT=120")
      list(APPEND SKEIN_AST_TEST_ENV "ASAN_OPTIONS=detect_leaks=0")
      list(APPEND SKEIN_AST_TEST_ENV "SKEIN_FIBER_STACK_KB=8192")
      # Sanitizer frames are much larger; keep deep recursion under the
      # process stack limit while still exercising the error path.
      list(APPEND SKEIN_AST_TEST_ENV "SKEIN_MAX_CALL_DEPTH=128")
    endif()
    set_tests_properties(skein_tests PROPERTIES
      ENVIRONMENT "${SKEIN_AST_TEST_ENV}"
      LABELS "core"
      RUN_SERIAL TRUE
      TIMEOUT 600
    )
    set(SKEIN_VM_TEST_ENV "SKEIN_VM=1")
    if(SKEIN_SANITIZE)
      # Sanitizer frames are much larger; keep deep recursion below the
      # process stack limit while still exercising the VM error path.
      list(APPEND SKEIN_VM_TEST_ENV "SKEIN_VM_MAX_DEPTH=128")
      list(APPEND SKEIN_VM_TEST_ENV "SKEIN_TEST_TIMEOUT=120")
      # Keep the regular sanitizer run focused on ASan/UBSan; leaks are
      # covered by skein_vm_lsan_tests below.
      list(APPEND SKEIN_VM_TEST_ENV "ASAN_OPTIONS=detect_leaks=0")
      list(APPEND SKEIN_VM_TEST_ENV "SKEIN_FIBER_STACK_KB=8192")
    endif()
    set_tests_properties(skein_vm_tests PROPERTIES
      ENVIRONMENT "${SKEIN_VM_TEST_ENV}"
      LABELS "core"
      RUN_SERIAL TRUE
      TIMEOUT 600
    )
    if(NOT SKEIN_SANITIZE)
      add_test(
        NAME vm_differential_test
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/vm_differential.py
                $<TARGET_FILE:${PROJECT_NAME}>
      )
      set_tests_properties(vm_differential_test PROPERTIES
        LABELS "core;differential"
        RUN_SERIAL TRUE
        TIMEOUT 300
      )
      if(SKEIN_ENABLE_JIT)
        add_test(
          NAME jit_smoke_test
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/jit_smoke.py
                  --bin $<TARGET_FILE:${PROJECT_NAME}>
        )
        set_tests_properties(jit_smoke_test PROPERTIES
          LABELS "core;jit"
          RUN_SERIAL TRUE
          TIMEOUT 300
        )
      endif()
      add_test(
        NAME gc_stress_test
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/gc_stress.py
                $<TARGET_FILE:${PROJECT_NAME}>
      )
      set_tests_properties(gc_stress_test PROPERTIES
        LABELS "core"
        TIMEOUT 300
      )
      add_test(
        NAME repl_flush_test
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/repl_flush.py
                $<TARGET_FILE:${PROJECT_NAME}>
      )
      set_tests_properties(repl_flush_test PROPERTIES
        LABELS "core"
        TIMEOUT 60
      )
      add_test(
        NAME vm_bench_test
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/bench_vm.py
                --bin $<TARGET_FILE:${PROJECT_NAME}>
                --check
      )
      set_tests_properties(vm_bench_test PROPERTIES
        LABELS "bench"
        RUN_SERIAL TRUE
        TIMEOUT 300
      )
    endif()
    if(SKEIN_SANITIZE)
      # LeakSanitizer run.  Known non-VM leaks are excluded:
      # - deep recursive struct print relies on freeval's depth guard;
      # - the optional FFI module has its own memory-block lifecycle.
      add_test(NAME skein_vm_lsan_tests COMMAND ${SKEIN_TEST_COMMAND})
      set(SKEIN_VM_LSAN_ENV
        "SKEIN_VM=1"
        "SKEIN_VM_MAX_DEPTH=128"
        "SKEIN_TEST_TIMEOUT=120"
        "SKEIN_TEST_SKIP=deep recursive struct print"
        "SKEIN_TEST_SKIP_SOURCE=std.ffi"
        "SKEIN_TEST_SKIP_MODULES=ffi_real"
        "ASAN_OPTIONS=detect_leaks=1"
        "SKEIN_FIBER_STACK_KB=8192"
        "SKEIN_MAX_CALL_DEPTH=128"
      )
      set_tests_properties(skein_vm_lsan_tests PROPERTIES
        ENVIRONMENT "${SKEIN_VM_LSAN_ENV}"
        LABELS "sanitize;lsan"
        RUN_SERIAL TRUE
        TIMEOUT 900
      )
    endif()
    add_test(
      NAME encoding_tables_test
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_encoding_tables.py
              --check
    )
    set_tests_properties(encoding_tables_test PROPERTIES LABELS "data")
    add_test(
      NAME docs_sync_test
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_docs_sync.py
    )
    set_tests_properties(docs_sync_test PROPERTIES LABELS "docs")
    add_test(
      NAME boundary_check_test
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_boundaries.py
              --source-dir ${CMAKE_CURRENT_SOURCE_DIR}
              --build-dir ${CMAKE_CURRENT_BINARY_DIR}
    )
    set_tests_properties(boundary_check_test PROPERTIES LABELS "docs")
    add_test(
      NAME container_bench_test
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tools/bench_containers.py
              --bin $<TARGET_FILE:${PROJECT_NAME}>
              --check
    )
    set_tests_properties(container_bench_test PROPERTIES
      LABELS "bench"
      TIMEOUT 180
      RUN_SERIAL TRUE
    )
  else()
    message(WARNING "Python3 not found; skein_tests will not be registered")
  endif()
endif()
