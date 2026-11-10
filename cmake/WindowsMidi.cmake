set(manateeMidiSdk "${CMAKE_BINARY_DIR}/dependencies/windows-midi-0.99.92-preview.11")
set(manateeCppWinrt "${CMAKE_BINARY_DIR}/dependencies/cppwinrt-3.0.260520.1")

file(DOWNLOAD
    "https://github.com/microsoft/MIDI/releases/download/inbox-preview-11/Windows.Devices.Midi2.0.99.92-preview.11.nupkg"
    "${manateeMidiSdk}.zip"
    EXPECTED_HASH "SHA256=13afc6e6da2adac309ab8c174352f46f1c0ebe2d79e65475bd6274defb73df74"
    TLS_VERIFY ON
    TIMEOUT 180
    STATUS midiDownloadStatus)

file(DOWNLOAD
    "https://www.nuget.org/api/v2/package/Microsoft.Windows.CppWinRT/3.0.260520.1"
    "${manateeCppWinrt}.zip"
    EXPECTED_HASH "SHA256=d22e2e26133d63217ae26e91b1685fb024b03a508a78af645f8347a3126c8435"
    TLS_VERIFY ON
    TIMEOUT 180
    STATUS cppWinrtDownloadStatus)

if(NOT midiDownloadStatus MATCHES "^0;" OR NOT cppWinrtDownloadStatus MATCHES "^0;")
    message(FATAL_ERROR
        "Download failed:\nMIDI: ${midiDownloadStatus}\nC++/WinRT: ${cppWinrtDownloadStatus}")
endif()

file(REMOVE_RECURSE "${manateeMidiSdk}" "${manateeCppWinrt}")
file(ARCHIVE_EXTRACT INPUT "${manateeMidiSdk}.zip" DESTINATION "${manateeMidiSdk}")
file(ARCHIVE_EXTRACT INPUT "${manateeCppWinrt}.zip" DESTINATION "${manateeCppWinrt}")

cmake_host_system_information(RESULT kitsRoot QUERY WINDOWS_REGISTRY
    "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots" VALUE KitsRoot10)

if(NOT IS_DIRECTORY "${kitsRoot}/References")
    message(FATAL_ERROR "Windows SDK not found.")
endif()

set(sdkVersion "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
if(NOT sdkVersion)
    message(FATAL_ERROR "VS Generator with Windows SDK required.")
endif()

file(GLOB_RECURSE platformWinmds "${kitsRoot}/References/${sdkVersion}/*.winmd")
# The package is authoritative for this preview namespace, even with a newer SDK.
list(FILTER platformWinmds EXCLUDE REGEX "[/\\\\]Windows\\.Devices\\.Midi2[^/\\\\]*\\.winmd$")
if(NOT platformWinmds)
    message(FATAL_ERROR "No Windows SDK metadata found for ${sdkVersion}.")
endif()
set(manateeWinrtHeaders "${CMAKE_BINARY_DIR}/generated-winrt")
file(REMOVE_RECURSE "${manateeWinrtHeaders}")
file(MAKE_DIRECTORY "${manateeWinrtHeaders}")
set(projectionArgs "-base\n-input \"${manateeMidiSdk}/ref/native/Windows.Devices.Midi2.winmd\"\n")
foreach(winmd IN LISTS platformWinmds)
    string(APPEND projectionArgs "-input \"${winmd}\"\n")
endforeach()
file(WRITE "${CMAKE_BINARY_DIR}/manateeHostWinrt.rsp" "${projectionArgs}")
execute_process(COMMAND "${manateeCppWinrt}/bin/cppwinrt.exe" "@${CMAKE_BINARY_DIR}/manateeHostWinrt.rsp"
    -output "${manateeWinrtHeaders}"
    RESULT_VARIABLE projectionResult OUTPUT_VARIABLE projectionStdout ERROR_VARIABLE projectionStderr)
if(NOT projectionResult EQUAL 0 OR NOT EXISTS "${manateeWinrtHeaders}/winrt/Windows.Devices.Midi2.h")
    message(FATAL_ERROR "C++/WinRT projection generation failed (${projectionResult}):\n${projectionStdout}\n${projectionStderr}")
endif()

set(manateeMidiNative "${manateeMidiSdk}/runtimes/win-x64/native")
foreach(runtimeFile Windows.Devices.Midi2.dll Windows.Devices.Midi2.pri)
    if(NOT EXISTS "${manateeMidiNative}/${runtimeFile}")
        message(FATAL_ERROR "MIDI SDK package is missing ${runtimeFile}.")
    endif()
endforeach()
