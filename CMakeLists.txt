cmake_minimum_required(VERSION 3.24)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(NexPlay VERSION 0.2.3 LANGUAGES CXX RC)

find_program(NEXPLAY_POWERSHELL NAMES pwsh powershell REQUIRED)
set(NEXPLAY_ICON_FILE "${CMAKE_CURRENT_BINARY_DIR}/brand/nexplay.ico")
add_custom_command(OUTPUT "${NEXPLAY_ICON_FILE}"
    COMMAND "${NEXPLAY_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/release/Generate-BrandAssets.ps1"
        -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/brand"
    DEPENDS assets/nexplay.svg release/Generate-BrandAssets.ps1
    VERBATIM)
add_custom_target(nexplay_brand DEPENDS "${NEXPLAY_ICON_FILE}")
configure_file(src/resources/nexplay.rc.in "${CMAKE_CURRENT_BINARY_DIR}/nexplay.rc" @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/nexplay.rc"
    PROPERTIES OBJECT_DEPENDS "${NEXPLAY_ICON_FILE};${CMAKE_CURRENT_SOURCE_DIR}/src/update/Updater.ps1")

add_executable(nexplay_diagnostics
    src/main.cpp
    src/platform/windows/SystemProbe.cpp
    src/storage/ReplayStorage.cpp
)

target_compile_features(nexplay_diagnostics PRIVATE cxx_std_20)
target_compile_definitions(nexplay_diagnostics PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    target_compile_options(nexplay_diagnostics PRIVATE /W4 /permissive- /utf-8)
endif()

target_include_directories(nexplay_diagnostics PRIVATE src)

add_executable(nexplay_capture_test
    src/capture_test_main.cpp
    src/capture/DesktopDuplicator.cpp
    src/encoding/NvencEncoder.cpp
    src/storage/ReplayStorage.cpp
)

target_compile_features(nexplay_capture_test PRIVATE cxx_std_20)
target_compile_definitions(nexplay_capture_test PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    target_compile_options(nexplay_capture_test PRIVATE /W4 /permissive- /utf-8)
endif()

target_include_directories(nexplay_capture_test PRIVATE
    src
    third_party/nv-codec-headers/include/ffnvcodec
)

target_link_libraries(nexplay_capture_test PRIVATE d3d11 dxgi d3dcompiler user32)

add_executable(nexplay_nvenc_test
    src/nvenc_test_main.cpp
    src/encoding/NvencEncoder.cpp
    src/storage/ReplayStorage.cpp
)

target_compile_features(nexplay_nvenc_test PRIVATE cxx_std_20)
target_compile_definitions(nexplay_nvenc_test PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    target_compile_options(nexplay_nvenc_test PRIVATE /W4 /permissive- /utf-8)
endif()

target_include_directories(nexplay_nvenc_test PRIVATE
    src
    third_party/nv-codec-headers/include/ffnvcodec
)

target_link_libraries(nexplay_nvenc_test PRIVATE d3d11 dxgi)

add_executable(nexplay_replay_test
    src/replay_test_main.cpp
    src/capture/DesktopDuplicator.cpp
    src/encoding/NvencEncoder.cpp
    src/storage/ReplaySegmentBuffer.cpp
    src/storage/ReplayStorage.cpp
)

target_compile_features(nexplay_replay_test PRIVATE cxx_std_20)
target_compile_definitions(nexplay_replay_test PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    target_compile_options(nexplay_replay_test PRIVATE /W4 /permissive- /utf-8)
endif()

target_include_directories(nexplay_replay_test PRIVATE
    src
    third_party/nv-codec-headers/include/ffnvcodec
)

target_link_libraries(nexplay_replay_test PRIVATE
    d3d11
    dxgi
    d3dcompiler
    shell32
    ole32
    user32
    windowscodecs
)

enable_testing()
add_test(NAME app_updater COMMAND powershell -NoProfile -ExecutionPolicy Bypass
    -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/UpdateTests.ps1"
    -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/update-tests")

add_executable(timeline_edit_tests tests/TimelineEditTests.cpp)
target_compile_features(timeline_edit_tests PRIVATE cxx_std_20)
target_include_directories(timeline_edit_tests PRIVATE src)
if(MSVC)
    target_compile_options(timeline_edit_tests PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME timeline_edit COMMAND timeline_edit_tests)

add_executable(save_workflow_tests tests/SaveWorkflowTests.cpp src/app/FfmpegProgress.cpp)
target_compile_features(save_workflow_tests PRIVATE cxx_std_20)
target_compile_definitions(save_workflow_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(save_workflow_tests PRIVATE src)
if(MSVC)
    target_compile_options(save_workflow_tests PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME save_workflow COMMAND save_workflow_tests)

add_executable(thumbnail_selection_tests tests/ThumbnailSelectionTests.cpp)
target_compile_features(thumbnail_selection_tests PRIVATE cxx_std_20)
target_include_directories(thumbnail_selection_tests PRIVATE src)
if(MSVC)
    target_compile_options(thumbnail_selection_tests PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME thumbnail_selection COMMAND thumbnail_selection_tests)

add_executable(preview_audio_tests tests/PreviewAudioTests.cpp src/playback/PreviewAudio.cpp src/playback/Mp4AudioSource.cpp)
target_compile_features(preview_audio_tests PRIVATE cxx_std_20)
target_compile_definitions(preview_audio_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(preview_audio_tests PRIVATE src)
target_link_libraries(preview_audio_tests PRIVATE mf mfplay mfplat mfreadwrite mfuuid ole32)
if(MSVC)
    target_compile_options(preview_audio_tests PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME preview_audio_policy COMMAND preview_audio_tests)

# Content-level regression: works headlessly, without speakers or a capture device.
find_program(NEXPLAY_TEST_FFMPEG NAMES ffmpeg)
if(NEXPLAY_TEST_FFMPEG)
    add_test(NAME save_progress_process COMMAND save_workflow_tests "${NEXPLAY_TEST_FFMPEG}")
    set_tests_properties(save_progress_process PROPERTIES TIMEOUT 30)
    add_test(NAME preview_audio_identity COMMAND ${CMAKE_COMMAND}
        "-DFFMPEG=${NEXPLAY_TEST_FFMPEG}"
        "-DTEST_EXE=$<TARGET_FILE:preview_audio_tests>"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/audio-identity-fixtures"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/PreviewAudioIntegration.cmake)
    set_tests_properties(preview_audio_identity PROPERTIES TIMEOUT 60)
endif()

add_executable(replay_segment_buffer_tests
    tests/ReplaySegmentBufferTests.cpp
    src/storage/ReplaySegmentBuffer.cpp
)
target_compile_features(replay_segment_buffer_tests PRIVATE cxx_std_20)
target_compile_definitions(replay_segment_buffer_tests PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)
target_include_directories(replay_segment_buffer_tests PRIVATE src)
if(MSVC)
    target_compile_options(replay_segment_buffer_tests PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME replay_segment_buffer COMMAND replay_segment_buffer_tests)

add_executable(pcm_segment_buffer_tests
    tests/PcmSegmentBufferTests.cpp
    src/storage/PcmSegmentBuffer.cpp
)
target_compile_features(pcm_segment_buffer_tests PRIVATE cxx_std_20)
target_compile_definitions(pcm_segment_buffer_tests PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)
target_include_directories(pcm_segment_buffer_tests PRIVATE src)
if(MSVC)
    target_compile_options(pcm_segment_buffer_tests PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME pcm_segment_buffer COMMAND pcm_segment_buffer_tests)

add_executable(nexplay_audio_test
    src/audio_test_main.cpp
    src/audio/AudioSessionScanner.cpp
    src/audio/PcmAudioCapture.cpp
    src/storage/ReplayStorage.cpp
)
target_compile_features(nexplay_audio_test PRIVATE cxx_std_20)
target_compile_definitions(nexplay_audio_test PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)
target_include_directories(nexplay_audio_test PRIVATE src)
if(MSVC)
    target_compile_options(nexplay_audio_test PRIVATE /W4 /permissive- /utf-8)
endif()
target_link_libraries(nexplay_audio_test PRIVATE
    mmdevapi
    ole32
    runtimeobject
    user32
)

add_executable(nexplay_av_replay_test
    src/av_replay_test_main.cpp
    src/audio/AudioSessionScanner.cpp
    src/audio/PcmAudioCapture.cpp
    src/capture/DesktopDuplicator.cpp
    src/encoding/NvencEncoder.cpp
    src/storage/PcmSegmentBuffer.cpp
    src/storage/ReplaySegmentBuffer.cpp
    src/storage/ReplayStorage.cpp
)
target_compile_features(nexplay_av_replay_test PRIVATE cxx_std_20)
target_compile_definitions(nexplay_av_replay_test PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)
target_include_directories(nexplay_av_replay_test PRIVATE
    src
    third_party/nv-codec-headers/include/ffnvcodec
)
if(MSVC)
    target_compile_options(nexplay_av_replay_test PRIVATE /W4 /permissive- /utf-8)
endif()
target_link_libraries(nexplay_av_replay_test PRIVATE
    d3d11
    dxgi
    d3dcompiler
    mmdevapi
    ole32
    runtimeobject
    shell32
    user32
)

add_executable(nexplay WIN32
    "${CMAKE_CURRENT_BINARY_DIR}/nexplay.rc"
    src/gui_main.cpp
    src/update/UpdateController.cpp
    src/playback/PreviewAudio.cpp
    src/playback/Mp4AudioSource.cpp
    src/app/RecorderEngine.cpp
    src/app/FfmpegProgress.cpp
    src/ui/SaveToasts.cpp
    src/audio/AudioSessionScanner.cpp
    src/audio/PcmAudioCapture.cpp
    src/capture/DesktopDuplicator.cpp
    src/encoding/NvencEncoder.cpp
    src/storage/PcmSegmentBuffer.cpp
    src/storage/ReplaySegmentBuffer.cpp
    src/storage/ReplayStorage.cpp
)
add_dependencies(nexplay nexplay_brand)
target_compile_features(nexplay PRIVATE cxx_std_20)
target_compile_definitions(nexplay PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)
target_include_directories(nexplay PRIVATE
    src
    src/resources
    third_party/nv-codec-headers/include/ffnvcodec
)
if(MSVC)
    target_compile_options(nexplay PRIVATE /W4 /permissive- /utf-8)
endif()
target_link_libraries(nexplay PRIVATE
    version
    advapi32
    comctl32
    d3d11
    d2d1
    dwrite
    dwmapi
    dxgi
    d3dcompiler
    mfplay
    mf
    mfplat
    mfuuid
    mmdevapi
    ole32
    runtimeobject
    shell32
    user32
)

# Developer-only offscreen UI validation. Not included in release packages.
get_target_property(nexplay_sources nexplay SOURCES)
list(REMOVE_ITEM nexplay_sources src/gui_main.cpp)
add_executable(nexplay_ui_preview EXCLUDE_FROM_ALL
    tests/StudioLayoutPreview.cpp ${nexplay_sources})
add_dependencies(nexplay_ui_preview nexplay_brand)
target_compile_features(nexplay_ui_preview PRIVATE cxx_std_20)
target_compile_definitions(nexplay_ui_preview PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_include_directories(nexplay_ui_preview PRIVATE src src/resources third_party/nv-codec-headers/include/ffnvcodec)
if(MSVC)
    target_compile_options(nexplay_ui_preview PRIVATE /W4 /permissive- /utf-8)
endif()
get_target_property(nexplay_libraries nexplay LINK_LIBRARIES)
target_link_libraries(nexplay_ui_preview PRIVATE ${nexplay_libraries} windowscodecs)

# Explicit opt-in test: briefly displays only its own synthetic, silent overlays.
add_executable(save_toast_native_tests EXCLUDE_FROM_ALL
    tests/SaveToastNativeTests.cpp src/ui/SaveToasts.cpp)
target_compile_features(save_toast_native_tests PRIVATE cxx_std_20)
target_compile_definitions(save_toast_native_tests PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_include_directories(save_toast_native_tests PRIVATE src)
target_link_libraries(save_toast_native_tests PRIVATE d2d1 dwrite gdi32 user32 ole32)
if(MSVC)
    target_compile_options(save_toast_native_tests PRIVATE /W4 /permissive- /utf-8)
endif()
