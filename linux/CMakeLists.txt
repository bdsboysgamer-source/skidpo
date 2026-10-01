cmake_minimum_required(VERSION 3.16)

project(FishingBotLinux LANGUAGES CXX)

if(WIN32)
    message(FATAL_ERROR "This is the Linux port - build the Windows project from the repo root instead.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The shared, platform-agnostic logic (detection/tracking/control/config)
# lives in the root project's src/ and is compiled here unmodified - see
# linux/README.md.
set(SHARED_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src)

find_package(X11 REQUIRED)
if(NOT X11_Xext_FOUND)
    message(FATAL_ERROR "Xext extension (libXext, provides MIT-SHM) not found - install libXext-devel (Fedora/Bazzite) or libxext-dev (Debian/Ubuntu).")
endif()
if(NOT X11_Xtst_FOUND)
    message(FATAL_ERROR "XTest extension (libXtst) not found - install libXtst-devel (Fedora/Bazzite) or libxtst-dev (Debian/Ubuntu).")
endif()

add_executable(fishingbot-linux
    # ---- Shared, platform-agnostic logic (untouched, reused verbatim) ----
    ${SHARED_SRC_DIR}/common/Types.h

    ${SHARED_SRC_DIR}/config/Config.h
    ${SHARED_SRC_DIR}/config/Config.cpp

    ${SHARED_SRC_DIR}/detection/ColorModel.h
    ${SHARED_SRC_DIR}/detection/ColorModel.cpp
    ${SHARED_SRC_DIR}/detection/CandidateScoring.h
    ${SHARED_SRC_DIR}/detection/CandidateScoring.cpp
    ${SHARED_SRC_DIR}/detection/Detector.h
    ${SHARED_SRC_DIR}/detection/Detector.cpp

    ${SHARED_SRC_DIR}/tracking/Tracker.h
    ${SHARED_SRC_DIR}/tracking/Tracker.cpp

    ${SHARED_SRC_DIR}/controller/FishingController.h
    ${SHARED_SRC_DIR}/controller/FishingController.cpp

    # ---- Linux-specific implementations of what's Win32-specific upstream ----
    src/capture/X11Capture.h
    src/capture/X11Capture.cpp

    src/input/InputManager.h
    src/input/InputManager.cpp

    src/app/App.h
    src/app/App.cpp

    src/main.cpp
)

target_include_directories(fishingbot-linux PRIVATE
    ${SHARED_SRC_DIR}
    ${X11_INCLUDE_DIR}
)

target_link_libraries(fishingbot-linux PRIVATE
    ${X11_LIBRARIES}
    ${X11_Xtst_LIB}
    ${X11_Xext_LIB}
)
