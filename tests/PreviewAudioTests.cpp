#include "playback/PreviewAudio.h"
#include "playback/Mp4AudioSource.h"
#include <mfapi.h>
#include <mfreadwrite.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

using namespace nexplay::playback;
using Microsoft::WRL::ComPtr;
void require(bool result, const char *message) {
    if (!result)
        throw std::runtime_error(message);
}
void check(HRESULT hr) {
    if (FAILED(hr))
        throw std::runtime_error("Media Foundation test call failed: " +
                                 std::to_string(static_cast<unsigned long>(hr)));
}
std::string be32(std::uint32_t n) {
    std::string bytes;
    for (int i = 3; i >= 0; --i)
        bytes.push_back(static_cast<char>(n >> (8 * i)));
    return bytes;
}
std::string box(const char *type, const std::string &payload) {
    return be32(static_cast<std::uint32_t>(payload.size() + 8)) + type + payload;
}
std::string track(std::uint32_t id, bool audio, bool v1 = false) {
    std::string header(v1 ? 20 : 12, '\0');
    header[0] = v1 ? 1 : 0;
    return box("trak",
               box("tkhd", header + be32(id)) +
                   box("mdia", box("hdlr", std::string(8, '\0') + (audio ? "soun" : "vide"))));
}
void parserTests() {
    const std::string fixture =
        box("moov", track(42, true, true) + track(7, false) + track(13, true));
    std::istringstream input(fixture);
    auto tracks = readMp4Tracks(input, fixture.size());
    require(tracks.size() == 3 && tracks[0].id == 42 && tracks[0].audio && tracks[1].id == 7 &&
                !tracks[1].audio && tracks[2].id == 13,
            "Stable track IDs not retained across arbitrary order");
    for (const auto &t : tracks)
        require(fixture.substr(t.typeOffset, 4) == "trak", "Invalid view patch offset");
    const auto payload = track(42, true, true) + track(7, false);
    for (const auto &unusual : {be32(1) + "moov" + be32(0) +
                                    be32(static_cast<std::uint32_t>(payload.size() + 16)) + payload,
                                be32(0) + "moov" + payload}) {
        std::istringstream extendedInput(unusual);
        require(readMp4Tracks(extendedInput, unusual.size()).front().id == 42,
                "Extended/zero-size box lost identity");
    }
    for (auto malformed :
         {box("moov", track(2, true) + track(2, true)), box("moov", track(0, true)),
          box("moov", box("trak", box("tkhd", ""))), be32(100) + "moov",
          box("moov", box("mvex", "") + track(2, true)),
          box("moov", track(2, true)) + box("moov", track(3, true)),
          box("moov", box("trak", box("tkhd", std::string(12, '\0') + be32(2))))}) {
        bool rejected = false;
        try {
            std::istringstream bad(malformed);
            (void)readMp4Tracks(bad, malformed.size());
        } catch (const std::exception &) {
            rejected = true;
        }
        require(rejected, "Ambiguous or malformed identity must not fall back to an ordinal");
    }
}
// Decode the exact single-track source used by playback, without playing sound.
// Checking actual PCM frequency catches swapped sources that mute-state tests miss.
void verifyTone(const std::filesystem::path &file, std::uint32_t id, double expected,
                double seconds) {
    auto source = audioTrackSource(file, id);
    ComPtr<IMFSourceReader> reader;
    check(MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader));
    ComPtr<IMFMediaType> format;
    check(MFCreateMediaType(&format));
    check(format->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    check(format->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float));
    check(format->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1));
    check(format->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000));
    constexpr DWORD audioStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    check(reader->SetCurrentMediaType(audioStream, nullptr, format.Get()));
    PROPVARIANT position{};
    position.vt = VT_I8;
    position.hVal.QuadPart = static_cast<LONGLONG>(seconds * 10'000'000);
    check(reader->SetCurrentPosition(GUID_NULL, position));
    std::vector<float> pcm;
    for (int retry = 0; retry < 200 && pcm.size() < 12000; ++retry) {
        DWORD flags{};
        ComPtr<IMFSample> sample;
        check(reader->ReadSample(audioStream, 0, nullptr, &flags, nullptr, &sample));
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            break;
        if (!sample)
            continue;
        ComPtr<IMFMediaBuffer> buffer;
        check(sample->ConvertToContiguousBuffer(&buffer));
        BYTE *bytes{};
        DWORD length{};
        check(buffer->Lock(&bytes, nullptr, &length));
        if (length % sizeof(float) != 0) {
            buffer->Unlock();
            throw std::runtime_error("Invalid decoded PCM size");
        }
        const auto size = pcm.size();
        pcm.resize(size + length / sizeof(float));
        std::memcpy(pcm.data() + size, bytes, length);
        check(buffer->Unlock());
    }
    require(pcm.size() > 8000, "Not enough decoded audio for identity test");
    int crossings = 0;
    for (std::size_t i = 2001; i < pcm.size(); ++i)
        if (pcm[i - 1] <= 0 && pcm[i] > 0)
            ++crossings;
    const double frequency = crossings * 48000.0 / (pcm.size() - 2000);
    std::cout << "MP4 track " << id << " at " << seconds << " s: " << frequency << " Hz (expected "
              << expected << ")\n";
    require(std::abs(frequency - expected) < 20, "Wrong audio content assigned to MP4 track ID");
    reader.Reset();
    source->Shutdown();
}
void playbackTests(const std::filesystem::path &file, std::uint32_t firstId, double firstTone,
                   std::uint32_t secondId, double secondTone, bool decodeOnly) {
    for (double seconds : {0.0, 1.5, 0.25}) {
        verifyTone(file, secondId, secondTone, seconds);
        verifyTone(file, firstId, firstTone, seconds);
    }
    bool rejected = false;
    try {
        (void)audioTrackSource(file, 99999);
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "Unknown identity must not select another stream");
    if (decodeOnly)
        return; // CI can validate content without an audio output device.
    PreviewAudio preview;
    // Deliberately reverse editor order. Names and ordinal positions are irrelevant.
    std::vector<AudioSelection> tracks{{true, 0, 3, secondId}, {true, 1, 2, firstId}};
    preview.open(file, tracks);
    for (int retry = 0; retry < 100 && !preview.ready(); ++retry) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    require(preview.ready() && preview.size() == 2, "Identified audio players not ready");
    require(preview.muted(0) && preview.muted(1), "Unsafe initial sound");
    tracks[0].included = false;
    preview.update(tracks, 1.5, false);
    require(preview.muted(0) && !preview.muted(1), "Mute did not reach the assigned player");
    std::reverse(tracks.begin(), tracks.end());
    preview.update(tracks, 1.5, false);
    require(preview.muted(0) && !preview.muted(1), "Reordering edits changed audio identity");
    preview.update({tracks[0]}, 1.5, false);
    require(preview.muted(0) && !preview.muted(1), "Delete muted or swapped the surviving track");
    preview.update({}, 1.5, false);
    require(preview.muted(0) && preview.muted(1), "Deleting every track left sound enabled");
    preview.update(tracks, 1.5, false);
    require(preview.muted(0) && !preview.muted(1), "Undo did not restore the right player");
    preview.update({tracks[0], tracks[0]}, 1.5, false);
    require(preview.muted(0) && preview.muted(1), "Ambiguous update must mute all players");
    preview.seek(2.5);
    preview.update(tracks, 2.5, false);
    require(preview.muted(0) && preview.muted(1), "Trim range not applied after seek");
    preview.close();
    rejected = false;
    try {
        preview.open(file, {{true, 0, 3, 99999}});
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected && preview.size() == 0, "Unknown identity must not play a different stream");
    rejected = false;
    try {
        preview.open(file, {tracks[0], tracks[0]});
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected && preview.size() == 0,
            "Duplicate identity must not create duplicate playback");
}
int wmain(int argc, wchar_t **argv) {
    try {
        parserTests();
        AudioSelection range{true, 1, 3, 42};
        require(!audible(range, 0.9) && audible(range, 1) && audible(range, 2.99) &&
                    !audible(range, 3),
                "Bad audible range");
        range.included = false;
        require(!audible(range, 2), "Muted track is audible");
        const bool decodeOnly = argc > 1 && std::wstring(argv[argc - 1]) == L"--decode-only";
        const int arguments = argc - (decodeOnly ? 1 : 0);
        require(arguments == 1 || arguments == 2 || arguments == 6,
                "Expected [fixture [idA hzA idB hzB] [--decode-only]]");
        if (arguments > 1) {
            check(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
            check(MFStartup(MF_VERSION));
            playbackTests(argv[1], arguments == 6 ? std::stoul(argv[2]) : 2,
                          arguments == 6 ? std::stod(argv[3]) : 440,
                          arguments == 6 ? std::stoul(argv[4]) : 3,
                          arguments == 6 ? std::stod(argv[5]) : 880, decodeOnly);
            MFShutdown();
            CoUninitialize();
        }
        std::cout << "Audio identity, parser and mute tests passed.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
