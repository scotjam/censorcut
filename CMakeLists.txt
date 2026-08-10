cmake_minimum_required(VERSION 3.21)
project(CensorCut
    VERSION 0.1.0
    DESCRIPTION "Mark and remove scary scenes from kids' movies"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compile commands for clangd / IDE integration
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Gui Network)
qt_standard_project_setup()

find_package(nlohmann_json 3.11 REQUIRED)

# libVLC: try pkg-config first, then a manual hint
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBVLC libvlc)
endif()

if(NOT LIBVLC_FOUND)
    # Manual hints — set LIBVLC_ROOT in your environment if needed
    find_path(LIBVLC_INCLUDE_DIR vlc/vlc.h
        HINTS $ENV{LIBVLC_ROOT}/include
              /usr/include /usr/local/include /opt/homebrew/include)
    find_library(LIBVLC_LIBRARIES NAMES vlc libvlc
        HINTS $ENV{LIBVLC_ROOT}/lib
              /usr/lib /usr/local/lib /opt/homebrew/lib)
    if(LIBVLC_INCLUDE_DIR AND LIBVLC_LIBRARIES)
        set(LIBVLC_INCLUDE_DIRS ${LIBVLC_INCLUDE_DIR})
        set(LIBVLC_FOUND TRUE)
    endif()
endif()

if(NOT LIBVLC_FOUND)
    message(FATAL_ERROR "libVLC not found. Install VLC dev package or set LIBVLC_ROOT.")
endif()

# ---------------------------------------------------------------------------
# Core library (no Qt dependency on the data model — Qt only for QObject glue)
# ---------------------------------------------------------------------------
add_library(censorcut_core STATIC
    src/core/Marker.h
    src/core/Marker.cpp
    src/core/Project.h
    src/core/Project.cpp
    src/core/MarkerModel.h
    src/core/MarkerModel.cpp
    src/core/PlaybackController.h
    src/core/PlaybackController.cpp
    src/core/AgeProfile.h
    src/core/AgeProfile.cpp
    src/core/ExportPlan.h
    src/core/ExportPlan.cpp
    src/core/EditList.h
    src/core/EditList.cpp
    src/core/FfmpegRunner.h
    src/core/FfmpegRunner.cpp
    src/core/ExportController.h
    src/core/ExportController.cpp
    src/core/ExportQueue.h
    src/core/ExportQueue.cpp
    src/core/AnalysisResult.h
    src/core/AnalysisResult.cpp
    src/core/AnalysisController.h
    src/core/AnalysisController.cpp
    src/core/SubtitleProcessor.h
    src/core/SubtitleProcessor.cpp
    src/core/FeedbackStore.h
    src/core/FeedbackStore.cpp
    src/core/FingerprintMatcher.h
    src/core/FingerprintMatcher.cpp
    src/core/EditsClient.h
    src/core/EditsClient.cpp
    src/core/SyncProcess.h
    src/core/SyncProcess.cpp
    src/core/TrustLedger.h
    src/core/TrustLedger.cpp
    src/core/SandboxedProcess.h
    src/core/SandboxedProcess.cpp
)
target_include_directories(censorcut_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${LIBVLC_INCLUDE_DIRS}
)
target_link_libraries(censorcut_core PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    nlohmann_json::nlohmann_json
    ${LIBVLC_LIBRARIES}
)

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
qt_add_executable(censorcut
    src/main.cpp
    src/ui/MainWindow.h
    src/ui/MainWindow.cpp
    src/ui/TimelineWidget.h
    src/ui/TimelineWidget.cpp
    src/ui/VideoSurface.h
    src/ui/VideoSurface.cpp
    src/ui/ExportDialog.h
    src/ui/ExportDialog.cpp
    src/ui/ExportQueuePanel.h
    src/ui/ExportQueuePanel.cpp
    src/ui/AnalyzerPanel.h
    src/ui/AnalyzerPanel.cpp
    src/ui/EditsPullDialog.h
    src/ui/EditsPullDialog.cpp
    src/ui/SharingDialog.h
    src/ui/SharingDialog.cpp
    src/ui/ProposedCategoriesDialog.h
    src/ui/ProposedCategoriesDialog.cpp
)
target_link_libraries(censorcut PRIVATE
    censorcut_core
    Qt6::Widgets
)

set_target_properties(censorcut PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)

# ---------------------------------------------------------------------------
# Tests (M1: just the marker model; expand in later milestones)
# ---------------------------------------------------------------------------
option(CENSORCUT_BUILD_TESTS "Build unit tests" ON)
if(CENSORCUT_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 COMPONENTS Test)
    if(Qt6Test_FOUND)
        qt_add_executable(test_marker_model tests/test_marker_model.cpp)
        target_link_libraries(test_marker_model PRIVATE censorcut_core Qt6::Test)
        add_test(NAME marker_model COMMAND test_marker_model)

        qt_add_executable(test_export_planner tests/test_export_planner.cpp)
        target_link_libraries(test_export_planner PRIVATE censorcut_core Qt6::Test)
        add_test(NAME export_planner COMMAND test_export_planner)

        qt_add_executable(test_edit_list tests/test_edit_list.cpp)
        target_link_libraries(test_edit_list PRIVATE censorcut_core Qt6::Test)
        add_test(NAME edit_list COMMAND test_edit_list)

        qt_add_executable(test_subtitle_processor tests/test_subtitle_processor.cpp)
        target_link_libraries(test_subtitle_processor PRIVATE censorcut_core Qt6::Test)
        add_test(NAME subtitle_processor COMMAND test_subtitle_processor)

        qt_add_executable(test_fingerprint_matcher tests/test_fingerprint_matcher.cpp)
        target_link_libraries(test_fingerprint_matcher PRIVATE censorcut_core Qt6::Test)
        add_test(NAME fingerprint_matcher COMMAND test_fingerprint_matcher)

        qt_add_executable(test_trust_ledger tests/test_trust_ledger.cpp)
        target_link_libraries(test_trust_ledger PRIVATE censorcut_core Qt6::Test)
        add_test(NAME trust_ledger COMMAND test_trust_ledger)

        # The VLC interface script's logic. VLC embeds Lua but ships no
        # standalone interpreter, so this test only registers when one is
        # available — it must never be the reason a build fails.
        find_program(LUA_EXECUTABLE NAMES lua lua5.4 lua5.3 luajit)
        if(LUA_EXECUTABLE)
            add_test(NAME vlc_lua_script
                     COMMAND ${LUA_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/players/vlc/test_censorcut.lua)
        else()
            message(STATUS "lua not found — skipping players/vlc script tests")
        endif()

        # The only widget test: TimelineWidget's pixel/time mappings. Compiles
        # the widget directly since the UI sources live in the app target.
        qt_add_executable(test_timeline_widget
            tests/test_timeline_widget.cpp
            src/ui/TimelineWidget.h
            src/ui/TimelineWidget.cpp
        )
        target_link_libraries(test_timeline_widget
            PRIVATE censorcut_core Qt6::Widgets Qt6::Test)
        add_test(NAME timeline_widget COMMAND test_timeline_widget)
    endif()
endif()
