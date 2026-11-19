# Copy the DLLs an executable needs next to it on Windows.
#
# Invoked from a POST_BUILD step:
#   cmake -DBINARY=<exe> -DDESTINATION=<dir> [-DEXTRA_DIRS=<dir>;...]
#         -P tools/copy_windows_dlls.cmake
#
# DLLs that belong to Windows itself (System32, SysWOW64, WinSxS) are never
# copied.  Missing third-party DLLs are reported but do not fail the build.

cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED BINARY OR NOT DEFINED DESTINATION)
    message(FATAL_ERROR
        "copy_windows_dlls.cmake requires BINARY and DESTINATION")
endif()
if(NOT EXISTS "${BINARY}")
    message(WARNING "copy_windows_dlls: ${BINARY} does not exist")
    return()
endif()
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()

set(directories)
if(DEFINED EXTRA_DIRS)
    set(directories DIRECTORIES ${EXTRA_DIRS})
endif()

file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${BINARY}"
    ${directories}
    RESOLVED_DEPENDENCIES_VAR resolved
    UNRESOLVED_DEPENDENCIES_VAR unresolved
    PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
    POST_EXCLUDE_REGEXES "[Ww][Ii][Nn][Dd][Oo][Ww][Ss]/[Ss][Yy][Ss][Tt][Ee][Mm]32/")

set(copied 0)
foreach(dependency IN LISTS resolved)
    get_filename_component(directory "${dependency}" DIRECTORY)
    string(TOLOWER "${directory}" directory_lower)
    # Never ship DLLs that are part of Windows.
    if(directory_lower MATCHES "/windows/(system32|syswow64|winsxs)")
        continue()
    endif()
    get_filename_component(name "${dependency}" NAME)
    file(COPY "${dependency}" DESTINATION "${DESTINATION}")
    math(EXPR copied "${copied} + 1")
endforeach()

set(missing 0)
foreach(dependency IN LISTS unresolved)
    # API sets and other Windows-only names are expected to be unresolved.
    string(TOLOWER "${dependency}" lower)
    if(lower MATCHES "^(api-ms-|ext-ms-)")
        continue()
    endif()
    message(WARNING "copy_windows_dlls: cannot resolve ${dependency}")
    math(EXPR missing "${missing} + 1")
endforeach()

message(STATUS "copy_windows_dlls: copied ${copied} DLL(s), ${missing} unresolved")
