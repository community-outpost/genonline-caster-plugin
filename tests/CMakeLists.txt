# SPDX-License-Identifier: MIT

# Adds one CTest test per TEST_CASE in source, named <prefix>.<case>. Extra arguments follow the case name.
function(genonline_caster_add_cases target prefix source)
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/${source}" lines REGEX "^TEST_CASE\\([A-Za-z0-9_]+\\)")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
    foreach(line IN LISTS lines)
        string(REGEX REPLACE "^TEST_CASE\\(([A-Za-z0-9_]+)\\).*$" "\\1" case "${line}")
        add_test(NAME ${prefix}.${case} COMMAND ${target} ${case} ${ARGN})
        set_tests_properties(${prefix}.${case} PROPERTIES TIMEOUT 60)
    endforeach()
endfunction()

add_executable(genonline_caster_core_tests core_tests.cpp test_harness.h)
target_link_libraries(genonline_caster_core_tests PRIVATE genonline_caster_core)
genonline_caster_add_cases(genonline_caster_core_tests core core_tests.cpp)

add_executable(genonline_caster_pipe_tests pipe_tests.cpp test_harness.h)
target_link_libraries(genonline_caster_pipe_tests PRIVATE genonline_caster_platform)
genonline_caster_add_cases(genonline_caster_pipe_tests pipe pipe_tests.cpp)

# The DLL tests load the staged plugin library by path, the way the game loads it.
add_executable(genonline_caster_dll_tests dll_tests.cpp test_harness.h)
target_link_libraries(genonline_caster_dll_tests PRIVATE GenOnline::PluginApi)
target_compile_definitions(genonline_caster_dll_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
    GENONLINE_CASTER_EXPECTED_VERSION="${GENONLINE_CASTER_VERSION}")
add_dependencies(genonline_caster_dll_tests genonline_caster_package)
genonline_caster_add_cases(genonline_caster_dll_tests dll dll_tests.cpp "$<TARGET_FILE:genonline_caster>")

# Checks the staged folder and its manifest after the build. The DLL tests and this check need the package target.
add_test(NAME package.staged_folder_matches_the_manifest
    COMMAND "${CMAKE_COMMAND}"
        "-DPLUGIN_DIR=${GENONLINE_CASTER_STAGE_DIR}"
        "-DLIBRARY_DIR=$<TARGET_FILE_DIR:genonline_caster>"
        "-DPLUGIN_FILE=$<TARGET_FILE_NAME:genonline_caster>"
        "-DPLUGIN_VERSION=${GENONLINE_CASTER_VERSION}"
        -P "${PROJECT_SOURCE_DIR}/cmake/CheckStagedPlugin.cmake")
