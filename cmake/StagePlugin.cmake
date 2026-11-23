# SPDX-License-Identifier: MIT
# Cleans the staged plugin folder before the manifest is written.
#
# cmake -DPLUGIN_DIR=<folder> -DPLUGIN_FILE=<file name of the plugin library> -P StagePlugin.cmake
#
# The folder belongs to the build. Every file the manifest would wrongly list, such as a leftover from an earlier build
# or a stale signature, is removed.

cmake_minimum_required(VERSION 3.14)

foreach(required PLUGIN_DIR PLUGIN_FILE)
    if (NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

set(kept "${PLUGIN_FILE}" plugin.manifest)

file(GLOB entries LIST_DIRECTORIES true RELATIVE "${PLUGIN_DIR}" "${PLUGIN_DIR}/*")
foreach(entry IN LISTS entries)
    list(FIND kept "${entry}" index)
    if (index EQUAL -1)
        message(STATUS "Removing ${entry} from the staged plugin folder")
        file(REMOVE_RECURSE "${PLUGIN_DIR}/${entry}")
    endif()
endforeach()
