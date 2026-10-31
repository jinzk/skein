cmake_minimum_required(VERSION 3.10)
project(calc-demo LANGUAGES CXX)

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
  set(CALC_LIBUV_TARGET PkgConfig::LIBUV)
else()
  find_path(LIBUV_INCLUDE_DIR uv.h)
  find_library(LIBUV_LIBRARY NAMES uv libuv)
  if(LIBUV_INCLUDE_DIR AND LIBUV_LIBRARY)
    add_library(calc_libuv UNKNOWN IMPORTED)
    set_target_properties(calc_libuv PROPERTIES
      IMPORTED_LOCATION "${LIBUV_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${LIBUV_INCLUDE_DIR}"
    )
    set(CALC_LIBUV_TARGET calc_libuv)
  endif()
endif()
if(NOT CALC_LIBUV_TARGET)
  message(FATAL_ERROR "libuv not found; install libuv development files")
endif()

add_executable(${PROJECT_NAME}
  ${BISON_MyParser_OUTPUTS}
  ${FLEX_MyScanner_OUTPUTS}
  ast.cpp
  builtins.cpp
  diagnostics.cpp
  eval.cpp
  eval_context.cpp
  event_loop.cpp
  fiber.cpp
  filetype.cpp
  format.cpp
  main.cpp
  net.cpp
  package.cpp
  repl.cpp
  scopestack.cpp
  structeval.cpp
  structtype.cpp
  symbol.cpp
  task.cpp
  valuetype.cpp
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
target_link_libraries(${PROJECT_NAME} PRIVATE ${CALC_LIBUV_TARGET})

option(CALC_ENABLE_FFI "Enable the optional std.ffi module (requires libffi)" ON)
if(CALC_ENABLE_FFI)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(LIBFFI QUIET IMPORTED_TARGET libffi)
  endif()
  if(LIBFFI_FOUND)
    target_sources(${PROJECT_NAME} PRIVATE ffi.cpp)
    target_compile_definitions(${PROJECT_NAME} PRIVATE CALC_ENABLE_FFI=1)
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
  target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32)
endif()

include(CTest)
if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    if(CALC_ENABLE_FFI AND LIBFFI_FOUND)
      add_library(calc_ffi_shim SHARED tests/ffi_shim.cpp)
    endif()
    set(CALC_TEST_COMMAND
      ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_calc.py
      $<TARGET_FILE:${PROJECT_NAME}>
    )
    if(TARGET calc_ffi_shim)
      list(APPEND CALC_TEST_COMMAND $<TARGET_FILE:calc_ffi_shim>)
    endif()
    add_test(NAME calc_tests COMMAND ${CALC_TEST_COMMAND})
  else()
    message(WARNING "Python3 not found; calc_tests will not be registered")
  endif()
endif()
