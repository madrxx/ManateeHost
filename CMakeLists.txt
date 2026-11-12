cmake_minimum_required(VERSION 3.25)
project(ManateeHost VERSION 0.11.0 LANGUAGES C CXX)

# building app if not included in another project unless someone chooses otherwise
set(manateeAppDefault OFF)
if(PROJECT_IS_TOP_LEVEL AND WIN32)
    set(manateeAppDefault ON)
endif()

option(manateeBuildApp "Build the Windows app" ${manateeAppDefault})

add_library(aica STATIC vendor/highly_theoretical/arm.c vendor/highly_theoretical/dcsound.c vendor/highly_theoretical/yam.c)
target_include_directories(aica PUBLIC vendor/highly_theoretical)
target_compile_definitions(aica PRIVATE EMU_COMPILE EMU_LITTLE_ENDIAN YAM_DISABLE_DYNAREC)

add_library(ManateeHostCore STATIC src/audio/Engine.cpp src/banks/RamLayout.cpp)
add_library(ManateeHost::Core ALIAS ManateeHostCore)
target_include_directories(ManateeHostCore PUBLIC src)
target_compile_features(ManateeHostCore PUBLIC cxx_std_20)
target_link_libraries(ManateeHostCore PRIVATE aica)
if(MSVC)
    target_compile_options(ManateeHostCore PRIVATE /W4 /utf-8)
else()
    target_compile_options(ManateeHostCore PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(manateeBuildApp)
    if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "Targets Win64 with Visual Studio 2022 build tools.")
    endif()

    include(cmake/WindowsMidi.cmake)

    add_executable(ManateeHost WIN32 src/main.cpp src/app/App.cpp
        src/ui/MainWindow.cpp src/midi/MidiWorker.cpp src/app/Diagnostics.cpp src/audio/AudioPlayer.cpp src/audio/audio.c)
    target_include_directories(ManateeHost PRIVATE vendor vendor/libremidi/include)
    target_compile_options(ManateeHost PRIVATE /W4 /utf-8)
    target_compile_definitions(ManateeHost PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(ManateeHost PRIVATE ManateeHostCore comdlg32 shell32)
    
    # Configure Windows MIDI Services
    target_include_directories(ManateeHost BEFORE PRIVATE "${manateeWinrtHeaders}")
    target_include_directories(ManateeHost PRIVATE "${manateeMidiSdk}/build/native/include")
    target_link_libraries(ManateeHost PRIVATE RuntimeObject windowsapp)

    add_custom_command(TARGET ManateeHost POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${manateeMidiNative}/Windows.Devices.Midi2.dll"
            "${manateeMidiNative}/Windows.Devices.Midi2.pri"
            "$<TARGET_FILE_DIR:ManateeHost>"
        VERBATIM COMMENT "Copying Windows MIDI Services dll and pri")
endif()