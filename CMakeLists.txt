cmake_minimum_required(VERSION 3.14)
project(Tactileviewer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find OpenCV
find_package(OpenCV REQUIRED)

# Find CURL for Remote Inference
find_package(CURL REQUIRED)

# Find SQLite3 for Persistent Dossiers
find_package(SQLite3 REQUIRED)

# Find OpenGL, GLFW, and GLEW
find_package(OpenGL REQUIRED)
if(WIN32 OR APPLE)
    # Homebrew (macOS) and vcpkg (Windows) both install CMake config files
    find_package(glfw3 CONFIG REQUIRED)
    find_package(GLEW REQUIRED)
    set(GLFW_LIBRARIES    glfw)
    set(GLFW_INCLUDE_DIRS "")
    set(GLEW_LIBRARIES    GLEW::GLEW)
    set(GLEW_INCLUDE_DIRS "")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GLFW REQUIRED glfw3)
    pkg_check_modules(GLEW REQUIRED glew)
endif()

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
    src/core/UpdateChecker.cpp
    src/core/Application.cpp
    src/core/AudioEngine.cpp
    src/core/AudioCapture.cpp
    src/core/DataLogger.cpp
    src/core/ROIManager.cpp
    src/core/DossierDatabase.cpp
    src/vision/CameraModule.cpp
    src/vision/VideoRenderer.cpp
    src/vision/MotionDetector.cpp
    src/vision/FaceRecognizer.cpp
    src/vision/VisionSystem.cpp
    src/vision/AIAgent.cpp
    src/vision/ReIDManager.cpp
    src/ui/HUD.cpp
    src/ui/UIManager.cpp
    src/ui/UIManager_Settings.cpp
    src/ui/UIManager_Panels.cpp
    src/ui/UIManager_CameraView.cpp
    src/ui/UIManager_DevConsole.cpp
    src/ui/UIManager_SettingsWindow.cpp
    src/ui/UIManager_Export.cpp
    src/ui/UIManager_Analyzer.cpp
    src/ui/DevConsolePanel.cpp
    src/ui/AnalyzerPanel.cpp
    src/ui/AudioVisualizerPanel.cpp
    src/ui/ReplayPanel.cpp
    src/ui/DossierArchivePanel.cpp
    src/tracking/MultiTracker.cpp
    src/tracking/TrackingSystem.cpp
)

# Resources for macOS Bundle
set(RESOURCE_FILES
    assets/models/yolov8s.onnx
    assets/models/yolov8n.onnx
    assets/models/coco.txt
    assets/models/face_detection_yunet_2023mar.onnx
    assets/models/face_recognition_sface_2021dec.onnx
)

if(APPLE)
    add_executable(Tactileviewer MACOSX_BUNDLE src/core/main.cpp ${CORE_SRCS} ${IMGUI_SRCS} ${RESOURCE_FILES})
    set_target_properties(Tactileviewer PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
        RESOURCE "${RESOURCE_FILES}"
    )
elseif(WIN32)
    add_executable(Tactileviewer src/core/main.cpp ${CORE_SRCS} ${IMGUI_SRCS})
else()
    add_executable(Tactileviewer src/core/main.cpp ${CORE_SRCS} ${IMGUI_SRCS})
endif()

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
    ${CURL_INCLUDE_DIRS}
    ${SQLite3_INCLUDE_DIRS}
)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" APP_VERSION_RAW)
string(STRIP "${APP_VERSION_RAW}" APP_VERSION)

target_compile_definitions(Tactileviewer PRIVATE
    PROJECT_HORUS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    APP_VERSION="${APP_VERSION}"
)

target_link_libraries(Tactileviewer
    ${OpenCV_LIBS}
    OpenGL::GL
    ${GLFW_LIBRARIES}
    ${GLEW_LIBRARIES}
    ${CURL_LIBRARIES}
    SQLite::SQLite3
)
if(APPLE)
    target_link_libraries(Tactileviewer
        "-framework AudioToolbox"
        "-framework CoreFoundation"
        "-framework CoreAudio"
    )
elseif(WIN32)
    target_link_libraries(Tactileviewer
        ole32
        oleaut32
        uuid
        winmm
    )
endif()

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
include(GoogleTest)

# We add gtest-all.cc directly to the test executable to avoid any ABI mismatches
# between the pre-built library and the test object.
add_executable(unit_tests 
    tests/unit_tests.cpp
    ${googletest_SOURCE_DIR}/googletest/src/gtest-all.cc
    ${googletest_SOURCE_DIR}/googletest/src/gtest_main.cc
    ${CORE_SRCS}
    ${IMGUI_SRCS}
)

add_executable(integration_tests 
    tests/integration_tests.cpp
    ${googletest_SOURCE_DIR}/googletest/src/gtest-all.cc
    ${googletest_SOURCE_DIR}/googletest/src/gtest_main.cc
    ${CORE_SRCS}
    ${IMGUI_SRCS}
)

add_executable(functional_tests 
    tests/functional_tests.cpp
    ${googletest_SOURCE_DIR}/googletest/src/gtest-all.cc
    ${googletest_SOURCE_DIR}/googletest/src/gtest_main.cc
    ${CORE_SRCS}
    ${IMGUI_SRCS}
)

target_include_directories(unit_tests BEFORE PRIVATE 
    ${googletest_SOURCE_DIR}/googletest/include
    ${googletest_SOURCE_DIR}/googletest
)

target_include_directories(integration_tests BEFORE PRIVATE 
    ${googletest_SOURCE_DIR}/googletest/include
    ${googletest_SOURCE_DIR}/googletest
)

target_include_directories(functional_tests BEFORE PRIVATE 
    ${googletest_SOURCE_DIR}/googletest/include
    ${googletest_SOURCE_DIR}/googletest
)

target_include_directories(unit_tests PRIVATE 
    src
    src/core
    src/vision
    src/tracking
    src/ui
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${GLFW_INCLUDE_DIRS}
    ${GLEW_INCLUDE_DIRS}
    ${OpenCV_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

target_include_directories(integration_tests PRIVATE 
    src
    src/core
    src/vision
    src/tracking
    src/ui
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${GLFW_INCLUDE_DIRS}
    ${GLEW_INCLUDE_DIRS}
    ${OpenCV_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

target_include_directories(functional_tests PRIVATE 
    src
    src/core
    src/vision
    src/tracking
    src/ui
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${GLFW_INCLUDE_DIRS}
    ${GLEW_INCLUDE_DIRS}
    ${OpenCV_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

target_compile_definitions(unit_tests PRIVATE
    PROJECT_HORUS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_definitions(integration_tests PRIVATE
    PROJECT_HORUS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_definitions(functional_tests PRIVATE
    PROJECT_HORUS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(unit_tests PRIVATE
    ${OpenCV_LIBS}
    ${CURL_LIBRARIES}
    ${GLEW_LIBRARIES}
    ${GLFW_LIBRARIES}
    OpenGL::GL
    ${CMAKE_DL_LIBS}
    SQLite::SQLite3
)
if(APPLE)
    target_link_libraries(unit_tests PRIVATE
        "-framework AudioToolbox"
        "-framework CoreFoundation"
        "-framework CoreAudio"
    )
elseif(WIN32)
    target_link_libraries(unit_tests PRIVATE ole32 oleaut32 uuid winmm)
endif()

target_link_libraries(integration_tests PRIVATE
    ${OpenCV_LIBS}
    ${CURL_LIBRARIES}
    ${GLEW_LIBRARIES}
    ${GLFW_LIBRARIES}
    OpenGL::GL
    ${CMAKE_DL_LIBS}
    SQLite::SQLite3
)
if(APPLE)
    target_link_libraries(integration_tests PRIVATE
        "-framework AudioToolbox"
        "-framework CoreFoundation"
        "-framework CoreAudio"
    )
elseif(WIN32)
    target_link_libraries(integration_tests PRIVATE ole32 oleaut32 uuid winmm)
endif()

target_link_libraries(functional_tests PRIVATE
    ${OpenCV_LIBS}
    ${CURL_LIBRARIES}
    ${GLEW_LIBRARIES}
    ${GLFW_LIBRARIES}
    OpenGL::GL
    ${CMAKE_DL_LIBS}
    SQLite::SQLite3
)
if(APPLE)
    target_link_libraries(functional_tests PRIVATE
        "-framework AudioToolbox"
        "-framework CoreFoundation"
        "-framework CoreAudio"
    )
elseif(WIN32)
    target_link_libraries(functional_tests PRIVATE ole32 oleaut32 uuid winmm)
endif()

add_test(NAME unit_tests COMMAND unit_tests)
add_test(NAME integration_tests COMMAND integration_tests)
add_test(NAME functional_tests COMMAND functional_tests)

gtest_discover_tests(unit_tests DISCOVERY_TIMEOUT 30)
gtest_discover_tests(integration_tests DISCOVERY_TIMEOUT 30)
gtest_discover_tests(functional_tests DISCOVERY_TIMEOUT 30)

# On non-Apple platforms, copy model assets next to the binary so the
# exe-relative path lookup in Application.cpp (path 4) finds them.
if(NOT APPLE)
    foreach(_model IN LISTS RESOURCE_FILES)
        get_filename_component(_fname "${_model}" NAME)
        add_custom_command(TARGET Tactileviewer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:Tactileviewer>/assets/models"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/${_model}"
                "$<TARGET_FILE_DIR:Tactileviewer>/assets/models/${_fname}"
            COMMENT "Copying model ${_fname} to output dir"
        )
    endforeach()
endif()
