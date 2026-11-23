# SPDX-License-Identifier: MIT

# Pure plugin logic without operating system calls. The plugin library and the unit tests both link it.
add_library(genonline_caster_core STATIC
    core/api_table.cpp
    core/api_table.h
    core/frame_codec.cpp
    core/frame_codec.h
    core/frame_queue.cpp
    core/frame_queue.h
    core/host_link.cpp
    core/host_link.h
    core/reader_table.cpp
    core/reader_table.h
    core/token_bucket.cpp
    core/token_bucket.h)
target_include_directories(genonline_caster_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(genonline_caster_core PUBLIC GenOnline::PluginApi)

# The named pipe server. The plugin library and the tests both link it.
add_library(genonline_caster_platform STATIC
    platform/pipe_server.cpp
    platform/pipe_server.h)
target_compile_definitions(genonline_caster_platform PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(genonline_caster_platform PUBLIC genonline_caster_core advapi32)

set(GENONLINE_CASTER_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(plugin/build_config.h.in "${GENONLINE_CASTER_GENERATED_DIR}/build_config.h" @ONLY)

add_library(genonline_caster SHARED
    plugin/exports.cpp)
target_include_directories(genonline_caster PRIVATE "${GENONLINE_CASTER_GENERATED_DIR}")
target_link_libraries(genonline_caster PRIVATE genonline_caster_platform)

# The library name equals the folder name. The staged folder holds only files the manifest lists, so the import library
# and the PDB go elsewhere.
set_target_properties(genonline_caster PROPERTIES
    OUTPUT_NAME genonline_caster
    RUNTIME_OUTPUT_DIRECTORY "${GENONLINE_CASTER_STAGE_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${GENONLINE_CASTER_STAGE_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pdb"
    COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pdb")
foreach(config RELEASE RELWITHDEBINFO MINSIZEREL DEBUG)
    set_target_properties(genonline_caster PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${config} "${GENONLINE_CASTER_STAGE_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_${config} "${GENONLINE_CASTER_STAGE_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY_${config} "${CMAKE_BINARY_DIR}/lib"
        PDB_OUTPUT_DIRECTORY_${config} "${CMAKE_BINARY_DIR}/pdb")
endforeach()

# Release builds keep a PDB outside the staged folder for crash reports.
target_compile_options(genonline_caster PRIVATE $<$<CONFIG:Release>:/Zi>)
target_link_options(genonline_caster PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)

# Runs on every build and removes stray files from the staged folder.
add_custom_target(genonline_caster_stage
    COMMAND "${CMAKE_COMMAND}"
        "-DPLUGIN_DIR=${GENONLINE_CASTER_STAGE_DIR}"
        "-DPLUGIN_FILE=$<TARGET_FILE_NAME:genonline_caster>"
        -P "${PROJECT_SOURCE_DIR}/cmake/StagePlugin.cmake"
    VERBATIM)
add_dependencies(genonline_caster_stage genonline_caster)

genonline_plugin_manifest(genonline_caster_manifest
    FOLDER "${GENONLINE_CASTER_STAGE_DIR}"
    NAME genonline_caster
    VERSION "${GENONLINE_CASTER_VERSION}"
    DEPENDS genonline_caster genonline_caster_stage)

# Writes the manifest on every build so it always matches the staged files.
add_custom_target(genonline_caster_package ALL)
add_dependencies(genonline_caster_package genonline_caster_manifest)
