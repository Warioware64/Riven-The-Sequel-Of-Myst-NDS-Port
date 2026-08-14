cmake_minimum_required(VERSION 3.20)
project(riven-convert LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# ---------------------------------------------------------------------------
# libvaht (LGPL-3.0) -- Mohawk archive reader and tBMP decoder
#
# Built here rather than through its autotools setup: only src/*.c is needed
# (src/vahttool/ is a CLI that wants libpng), and those files need nothing but
# libc. This keeps the converter cross-compilable for Windows, which autotools
# would not be.
#
# LGPL note: this is a static link. The obligation to let a user relink against
# a modified libvaht is met because the whole converter is published as source
# with libvaht as a submodule -- see docs/licensing.md.
# ---------------------------------------------------------------------------
set(VAHT_DIR "${REPO_ROOT}/thirdparty/libvaht")
if(NOT EXISTS "${VAHT_DIR}/src/vaht_archive.c")
    message(FATAL_ERROR
        "libvaht is missing. Run: git submodule update --init")
endif()

# libvaht 0.3 cannot open ANY Riven archive as shipped: its file-table struct
# is 10 bytes on disc but pads to 12 in memory, which fails the entry-count
# check in vaht_archive_open and would misread every resource offset even past
# it. patches/ carries the one-hunk fix; see the patch header for the details.
#
# Applied here rather than committed to a fork so the submodule keeps pointing
# at upstream. Idempotent: --reverse --check succeeding means it is already in.
file(GLOB VAHT_PATCHES "${CMAKE_CURRENT_SOURCE_DIR}/patches/*.patch")
list(SORT VAHT_PATCHES)
find_package(Git QUIET)
foreach(patch ${VAHT_PATCHES})
    if(NOT GIT_FOUND)
        message(FATAL_ERROR "git is needed to apply ${patch} to libvaht")
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${patch}"
        WORKING_DIRECTORY "${VAHT_DIR}"
        RESULT_VARIABLE already_applied
        OUTPUT_QUIET ERROR_QUIET)
    if(already_applied EQUAL 0)
        message(STATUS "libvaht: ${patch} already applied")
    else()
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply "${patch}"
            WORKING_DIRECTORY "${VAHT_DIR}"
            RESULT_VARIABLE patch_result
            ERROR_VARIABLE patch_error)
        if(NOT patch_result EQUAL 0)
            message(FATAL_ERROR
                "Failed to apply ${patch} to libvaht:\n${patch_error}")
        endif()
        message(STATUS "libvaht: applied ${patch}")
    endif()
endforeach()

file(GLOB VAHT_SOURCES "${VAHT_DIR}/src/*.c")
add_library(vaht STATIC ${VAHT_SOURCES})
target_include_directories(vaht
    PUBLIC "${VAHT_DIR}/include"
    PRIVATE "${VAHT_DIR}/src")
set_target_properties(vaht PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    target_compile_options(vaht PRIVATE -w)  # third-party: not our warnings to fix
endif()

# ---------------------------------------------------------------------------
# Core -- pure C++20, NO Qt.
#
# The GUI and the CLI are both thin drivers over this. Keeping Qt out is what
# lets the tests run fast and headless, and what lets a CI job without Qt still
# verify the conversion logic.
# ---------------------------------------------------------------------------
add_library(riven_convert_core STATIC
    core/src/AtomicWrite.cpp
    core/src/Converter.cpp
    core/src/Layout.cpp
    core/src/Options.cpp
    core/src/Preflight.cpp
    core/src/Progress.cpp
    core/src/installer/Dcl.cpp
    core/src/installer/InstallerArchive.cpp
    core/src/pe/PeCursors.cpp
    core/src/cursor/CursorPipeline.cpp
    core/src/mhk/Archive.cpp
    core/src/mhk/CardParse.cpp
    core/src/mhk/MovieList.cpp
    core/src/mhk/WaterEffect.cpp
    core/src/ffmpeg/FFmpeg.cpp
    core/src/rvid/VideoPipeline.cpp
    core/src/image/Bmp.cpp
    core/src/image/ImagePipeline.cpp
    core/src/image/Lz77.cpp
    core/src/image/TopBg.cpp
    core/src/audio/Ima.cpp
    core/src/audio/Mp2.cpp
    core/src/audio/SoundPipeline.cpp
    core/src/audio/Twav.cpp
    core/src/audio/minimp3_impl.c
)

# minimp3 (CC0-1.0) is instantiated in exactly one C translation unit, and like
# libvaht it is not ours to fix warnings in. Mp2.cpp includes the same header
# without MINIMP3_IMPLEMENTATION and so only sees declarations.
if(NOT MSVC)
    set_source_files_properties(core/src/audio/minimp3_impl.c PROPERTIES COMPILE_OPTIONS "-w")
endif()

target_include_directories(riven_convert_core PUBLIC
    core/include
    "${REPO_ROOT}/shared"      # RivenData.hpp -- the schema the DS also compiles
    "${REPO_ROOT}/thirdparty"  # <yas/...>, same spelling as the ARM9 build
)
find_package(Threads REQUIRED)
target_link_libraries(riven_convert_core PUBLIC vaht Threads::Threads)

if(NOT MSVC)
    target_compile_options(riven_convert_core PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
add_executable(riven-convert cli/main.cpp)
target_link_libraries(riven-convert PRIVATE riven_convert_core)

# ---------------------------------------------------------------------------
# Tests
#
# include(CTest) comes BEFORE the GUI subdirectory on purpose: it defines
# BUILD_TESTING, and without it gui/CMakeLists.txt silently registers no tests.
# ---------------------------------------------------------------------------
include(CTest)

# ---------------------------------------------------------------------------
# GUI (Qt6 Widgets)
#
# Optional so that a machine or CI job without Qt still builds and tests the
# core. Qt 6.5 rather than the 6.10 installed here, so distro and runner Qt
# versions are not needlessly excluded. Qt6::Svg is deliberately NOT used --
# it is not part of a default qtbase install.
# ---------------------------------------------------------------------------
find_package(Qt6 6.5 COMPONENTS Core Gui Widgets QUIET)

option(RIVEN_BUILD_GUI "Build the Qt GUI" ${Qt6_FOUND})

if(RIVEN_BUILD_GUI)
    if(NOT Qt6_FOUND)
        message(FATAL_ERROR
            "RIVEN_BUILD_GUI is ON but Qt6 (Core Gui Widgets >= 6.5) was not found.")
    endif()
    message(STATUS "Qt ${Qt6_VERSION}: building riven-convert-gui")
    add_subdirectory(gui)
elseif(Qt6_FOUND)
    message(STATUS "Qt ${Qt6_VERSION} found but RIVEN_BUILD_GUI is OFF: building CLI only")
else()
    message(STATUS "Qt6 (Core Gui Widgets >= 6.5) not found: building CLI only. "
                   "Install qt6-base-dev, or set RIVEN_BUILD_GUI=ON to make this an error.")
endif()

if(BUILD_TESTING)
    foreach(t schema_roundtrip archive atomic_write cards image sound video installer
              transition topbg resume)
        add_executable(test_${t} tests/test_${t}.cpp)
        target_link_libraries(test_${t} PRIVATE riven_convert_core)
        add_test(NAME ${t} COMMAND test_${t})
    endforeach()

    # The container reader the DS uses, compiled for the HOST. It is the only
    # device-side file this build touches, and it is deliberately free of <nds.h>
    # so that it can be: on hardware nothing can check that a movie was read the
    # way it was written.
    # The .rcur reader the DS uses, compiled for the HOST -- same argument as
    # test_rvid_arm9 below, and the only place the tile ordering of a sprite cel
    # can be checked at all.
    add_executable(test_rcur_arm9
        tests/test_rcur_arm9.cpp
        "${REPO_ROOT}/source/render/RcurFile.cpp")
    target_include_directories(test_rcur_arm9 PRIVATE "${REPO_ROOT}/source")
    target_link_libraries(test_rcur_arm9 PRIVATE riven_convert_core)
    if(NOT MSVC)
        target_compile_options(test_rcur_arm9 PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME rcur_arm9 COMMAND test_rcur_arm9)

    add_executable(test_rvid_arm9
        tests/test_rvid_arm9.cpp
        "${REPO_ROOT}/source/rvid/RvidFile.cpp")
    target_include_directories(test_rvid_arm9 PRIVATE "${REPO_ROOT}/source")
    target_link_libraries(test_rvid_arm9 PRIVATE riven_convert_core)
    if(NOT MSVC)
        target_compile_options(test_rvid_arm9 PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME rvid_arm9 COMMAND test_rvid_arm9)

    # The save file the DS writes, compiled for the HOST -- same argument again,
    # and the only place the four ways a slot can be damaged can be checked at
    # all. A save is written by the player and read back by a later build, so it
    # is the one format here whose compatibility nothing else guards.
    add_executable(test_save_arm9
        tests/test_save_arm9.cpp
        "${REPO_ROOT}/source/SaveGame.cpp")
    target_include_directories(test_save_arm9 PRIVATE "${REPO_ROOT}/source")
    target_link_libraries(test_save_arm9 PRIVATE riven_convert_core)
    if(NOT MSVC)
        target_compile_options(test_save_arm9 PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME save_arm9 COMMAND test_save_arm9)

    # The notebook files the DS writes, compiled for the HOST. Same argument as
    # save_arm9: the player made these, and every allocation in the reader is
    # sized from a field in a file that a bad sector can rewrite.
    add_executable(test_notes_arm9
        tests/test_notes_arm9.cpp
        "${REPO_ROOT}/source/BookNotes.cpp")
    target_include_directories(test_notes_arm9 PRIVATE "${REPO_ROOT}/source")
    target_link_libraries(test_notes_arm9 PRIVATE riven_convert_core)
    if(NOT MSVC)
        target_compile_options(test_notes_arm9 PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME notes_arm9 COMMAND test_notes_arm9)

    # RIVEN_TEST_DATA is deliberately NOT set as a test property: a
    # set_tests_properties(ENVIRONMENT) captures the value at CONFIGURE time and
    # then overrides whatever the runtime environment holds, so
    # `RIVEN_TEST_DATA=... ctest` would silently skip against a stale empty
    # value. Letting the ambient environment through is what makes it work.
endif()
