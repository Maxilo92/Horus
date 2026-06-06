cmake_minimum_required(VERSION 3.14)
project(Tactileviewer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find OpenCV
find_package(OpenCV REQUIRED)

# Find OpenGL, GLFW, and GLEW
find_package(OpenGL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLFW REQUIRED glfw3)
pkg_check_modules(GLEW REQUIRED glew)

# Fetch Dear ImGui
include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
)
FetchContent_MakeAvailable(imgui)

# Define ImGui Source Files
set(IMGUI_SRCS
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

# Project Source Files (excluding main.cpp for tests)
set(CORE_SRCS
    src/core/Application.cpp
    src/core/AudioEngine.cpp
    src/core/DataLogger.cpp
    src/core/ROIManager.cpp
    src/vision/CameraModule.cpp
    src/vision/VideoRenderer.cpp
    src/vision/MotionDetector.cpp
    src/ui/HUD.cpp
    src/tracking/MultiTracker.cpp
)

# Resources for macOS Bundle
set(RESOURCE_FILES
    assets/models/yolov8n.onnx
    assets/models/coco.txt
)

add_executable(Tactileviewer MACOSX_BUNDLE src/core/main.cpp ${CORE_SRCS} ${IMGUI_SRCS} ${RESOURCE_FILES})

# macOS Bundle Properties
set_target_properties(Tactileviewer PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
    RESOURCE "${RESOURCE_FILES}"
)

target_include_directories(Tactileviewer PRIVATE 
    src/core
    src/vision
    src/tracking
    src/ui
    ${OpenCV_INCLUDE_DIRS}
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${GLFW_INCLUDE_DIRS}
    ${GLEW_INCLUDE_DIRS}
)

target_compile_definitions(Tactileviewer PRIVATE
    PROJECT_HORUS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(Tactileviewer 
    ${OpenCV_LIBS}
    OpenGL::GL
    ${GLFW_LIBRARIES}
    ${GLEW_LIBRARIES}
    "-framework AudioToolbox"
    "-framework CoreFoundation"
)

# --- TESTING ---
enable_testing()

set(BUILD_GMOCK OFF CACHE BOOL "Build GMock" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "Install GTest" FORCE)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0
)
FetchContent_MakeAvailable(googletest)

# We add gtest-all.cc directly to the test executable to avoid any ABI mismatches
# between the pre-built library and the test object.
add_executable(unit_tests 
    tests/unit_tests.cpp
    ${googletest_SOURCE_DIR}/googletest/src/gtest-all.cc
    ${googletest_SOURCE_DIR}/googletest/src/gtest_main.cc
    ${CORE_SRCS}
    ${IMGUI_SRCS}
)

target_include_directories(unit_tests BEFORE PRIVATE 
    ${googletest_SOURCE_DIR}/googletest/include
    ${googletest_SOURCE_DIR}/googletest
)

target_include_directories(unit_tests PRIVATE 
    src/core
    src/vision
    src/tracking
    src/ui
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${GLFW_INCLUDE_DIRS}
    ${GLEW_INCLUDE_DIRS}
    ${OpenCV_INCLUDE_DIRS}
)

target_compile_definitions(unit_tests PRIVATE
    PROJECT_HORUS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(unit_tests 
    ${OpenCV_LIBS}
    OpenGL::GL
    ${GLFW_LIBRARIES}
    ${GLEW_LIBRARIES}
    "-framework AudioToolbox"
    "-framework CoreFoundation"
)

include(GoogleTest)
gtest_discover_tests(unit_tests DISCOVERY_TIMEOUT 30)
