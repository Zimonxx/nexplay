#include "storage/PcmSegmentBuffer.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(const bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

int main() {
    try {
        const auto root = std::filesystem::temp_directory_path() /
            (L"NexPlay-pcm-test-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(root);
        {
            nexplay::storage::PcmSegmentBuffer buffer(
                root, L"test", std::chrono::seconds(10));
            const nexplay::audio::PcmFormat format{
                .sampleRate = 10,
                .channels = 2,
                .bitsPerSample = 16,
                .blockAlign = 4,
            };
            std::vector<std::byte> packet(7 * format.blockAlign, std::byte{0x2A});
            for (int index = 0; index < 18; ++index) {
                buffer.append(format, packet, 7);
            }
            buffer.finishSegment();
            require(buffer.bufferedFrames() == 106, "Nieprawidlowe przyciecie bufora PCM.");

            const auto saveDirectory = root / L"save";
            const auto staged = buffer.stageLatest(saveDirectory, L"Test audio", 0);
            require(staged.frameCount == 106, "Nieprawidlowa liczba zapisanych probek.");
            require(staged.segments.size() == 11, "Nieprawidlowa liczba segmentow PCM.");
            require(staged.format.sampleRate == 10, "Nie zachowano formatu PCM.");

            buffer.reset();
            require(buffer.bufferedFrames() == 0, "Reset nie wyczyscil bufora PCM.");
            for (const auto& segment : staged.segments) {
                require(std::filesystem::exists(segment), "Reset usunal zapisany hardlink PCM.");
            }
        }
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::cout << "PcmSegmentBuffer: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PcmSegmentBuffer: " << error.what() << '\n';
        return 1;
    }
}
