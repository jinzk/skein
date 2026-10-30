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

add_executable(${PROJECT_NAME}
  ${BISON_MyParser_OUTPUTS}
  ${FLEX_MyScanner_OUTPUTS}
  ast.cpp
  eval.cpp
  format.cpp
  main.cpp
  scopestack.cpp
  symbol.cpp
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

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
endif()

if(UNIX)
  target_link_libraries(${PROJECT_NAME} PRIVATE m)
endif()

include(CTest)
if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME calc_tests
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_calc.py
              $<TARGET_FILE:${PROJECT_NAME}>
    )
  else()
    message(WARNING "Python3 not found; calc_tests will not be registered")
  endif()
endif()
