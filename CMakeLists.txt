cmake_minimum_required(VERSION 3.24)

project(NexPlay VERSION 0.1.0 LANGUAGES CXX)

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
    src/gui_main.cpp
    src/app/RecorderEngine.cpp
    src/audio/AudioSessionScanner.cpp
    src/audio/PcmAudioCapture.cpp
    src/capture/DesktopDuplicator.cpp
    src/encoding/NvencEncoder.cpp
    src/storage/PcmSegmentBuffer.cpp
    src/storage/ReplaySegmentBuffer.cpp
    src/storage/ReplayStorage.cpp
)
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
    third_party/nv-codec-headers/include/ffnvcodec
)
if(MSVC)
    target_compile_options(nexplay PRIVATE /W4 /permissive- /utf-8)
endif()
target_link_libraries(nexplay PRIVATE
    advapi32
    comctl32
    d3d11
    d2d1
    dwrite
    dwmapi
    dxgi
    d3dcompiler
    mfplay
    mfplat
    mfuuid
    mmdevapi
    ole32
    runtimeobject
    shell32
    user32
)
