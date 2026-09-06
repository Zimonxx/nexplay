#include "playback/PreviewAudio.h"
#include <mfapi.h>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <chrono>

using namespace nexplay::playback;
void require(bool result, const char *message) {
    if (!result)
        throw std::runtime_error(message);
}
int wmain(int argc, wchar_t **argv) {
    try {
        AudioSelection range{true, 1, 3};
        require(!audible(range, 0.9), "Sound before range");
        require(audible(range, 1) && audible(range, 2.99), "Missing in-range sound");
        require(!audible(range, 3), "Sound after range");
        range.included = false;
        require(!audible(range, 2), "Muted track is audible");
        if (argc == 2) {
            require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "COM init");
            require(SUCCEEDED(MFStartup(MF_VERSION)), "MF init");
            {
                PreviewAudio preview;
                std::vector<AudioSelection> tracks{{true, 0, 3}, {true, 1, 2}};
                preview.open(argv[1], tracks);
                for (int retry = 0; retry < 100 && !preview.ready(); ++retry) {
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                require(preview.ready(), "Audio renderers did not become ready");
                require(preview.size() == 2, "Did not map both MP4 streams");
                require(preview.muted(0) && preview.muted(1), "Unsafe initial sound");
                tracks[0].included = false;
                preview.update(tracks, 1.5, false);
                require(preview.muted(0) && !preview.muted(1),
                        "Per-track mute did not reach the player");
                preview.seek(2.5);
                preview.update(tracks, 2.5, false);
                require(preview.muted(0) && preview.muted(1), "Trim range not applied after seek");
                tracks[0].included = true;
                preview.update(tracks, 0.5, false);
                require(!preview.muted(0) && preview.muted(1), "Mute state lost on rewind");
                preview.close();
                require(preview.size() == 0, "Audio players were not closed");
            }
            MFShutdown();
            CoUninitialize();
            std::cout
                << "Media Foundation stream mapping and live mute checks passed (silent test).\n";
        }
        std::cout << "Preview audio policy tests passed.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
