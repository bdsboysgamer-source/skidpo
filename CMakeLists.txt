cmake_minimum_required(VERSION 3.24)

project(FishingBot LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    message(FATAL_ERROR "FishingBot targets Windows (Win32/D3D11/DXGI) only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(fishingbot
    src/main.cpp

    src/common/Types.h

    src/config/Config.h
    src/config/Config.cpp

    src/capture/DesktopDuplication.h
    src/capture/DesktopDuplication.cpp

    src/detection/ColorModel.h
    src/detection/ColorModel.cpp
    src/detection/CandidateScoring.h
    src/detection/CandidateScoring.cpp
    src/detection/Detector.h
    src/detection/Detector.cpp

    src/tracking/Tracker.h
    src/tracking/Tracker.cpp

    src/controller/FishingController.h
    src/controller/FishingController.cpp

    src/input/InputManager.h
    src/input/InputManager.cpp

    src/ui/StatusWindow.h
    src/ui/StatusWindow.cpp
    src/ui/DebugOverlay.h
    src/ui/DebugOverlay.cpp
    src/ui/MacroConfigPanel.h
    src/ui/MacroConfigPanel.cpp

    src/app/App.h
    src/app/App.cpp
)

target_compile_definitions(fishingbot PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_options(fishingbot PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc>
)

target_link_libraries(fishingbot PRIVATE
    d3d11
    dxgi
    user32
    gdi32
    gdiplus
    comctl32
)

set_target_properties(fishingbot PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:fishingbot>"
)

# fishingbot.exe creates config/config.ini next to itself (relative to the
# process's working directory) on first run if one isn't already there -
# see src/main.cpp - so no config file needs to be installed at build time.
