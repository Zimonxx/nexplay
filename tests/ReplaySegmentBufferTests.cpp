#include "storage/ReplaySegmentBuffer.h"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

int main() {
    const std::filesystem::path testRoot =
        std::filesystem::temp_directory_path() /
        (L"NexPlay-Segment-Test-" + std::to_wstring(GetCurrentProcessId()));

    try {
        std::filesystem::path sessionDirectory;
        {
            nexplay::storage::ReplaySegmentBuffer buffer(
                testRoot, 60, std::chrono::seconds(10));
            sessionDirectory = buffer.sessionDirectory();

            for (int segment = 0; segment < 12; ++segment) {
                std::ostream& output = buffer.beginSegment();
                output << "segment-" << segment;
                buffer.finishSegment(60);
            }

            if (buffer.bufferedFrames() != 600) {
                throw std::runtime_error("Bufor nie zachowal dokladnie 10 sekund.");
            }
            const auto staged = buffer.stageLatest();
            if (staged.segments.size() != 10 || staged.frameCount != 600) {
                throw std::runtime_error("Migawka bufora ma nieprawidlowy rozmiar.");
            }

            buffer.reset();
            if (buffer.bufferedFrames() != 0) {
                throw std::runtime_error("Bufor nie zostal wyzerowany po zapisie.");
            }
            for (const auto& stagedSegment : staged.segments) {
                if (!std::filesystem::exists(stagedSegment)) {
                    throw std::runtime_error("Zapisany klip utracil zabezpieczony segment.");
                }
            }
        }

        if (std::filesystem::exists(sessionDirectory)) {
            throw std::runtime_error("Katalog sesji nie zostal posprzatany.");
        }
        std::error_code cleanupError;
        std::filesystem::remove(testRoot, cleanupError);
        std::cout << "ReplaySegmentBuffer: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
