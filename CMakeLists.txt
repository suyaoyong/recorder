cmake_minimum_required(VERSION 3.20)
project(loopback_recorder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(loopback_recorder
    src/main.cpp
    src/WavWriter.cpp
    src/LoopbackRecorder.cpp
    src/DeviceEnumerator.cpp
    src/Logger.cpp
    src/HResultUtils.cpp
    src/Mp3Converter.cpp
    src/SegmentNaming.cpp
    src/RecordingUtils.cpp
)

target_include_directories(loopback_recorder PRIVATE src)

target_link_libraries(loopback_recorder PRIVATE
    ole32
    avrt
)

add_executable(loopback_recorder_gui
    src/GuiApp.cpp
    src/WavWriter.cpp
    src/LoopbackRecorder.cpp
    src/DeviceEnumerator.cpp
    src/Logger.cpp
    src/HResultUtils.cpp
    src/Mp3Converter.cpp
    src/SegmentNaming.cpp
    src/RecordingUtils.cpp
)

target_include_directories(loopback_recorder_gui PRIVATE src)

target_link_libraries(loopback_recorder_gui PRIVATE
    ole32
    avrt
    user32
    comdlg32
)

set_target_properties(loopback_recorder_gui PROPERTIES WIN32_EXECUTABLE YES)
