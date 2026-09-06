#include "playback/ThumbnailSelection.h"
#include <iostream>
#include <stdexcept>

int main() {
    try {
        const auto check = [](int actual, int expected) {
            if (actual != expected)
                throw std::runtime_error("Thumbnail jumped to a wrong cached frame");
        };
        std::map<int, int> frames{{0, 0}, {1, 0}, {100, 0}, {101, 0}, {300, 0}};
        using nexplay::playback::thumbnailFrame;
        check(thumbnailFrame(frames, 101, 100), 101); // Exact hit always wins.
        check(thumbnailFrame(frames, 200, 101), 101); // Forward cache miss holds.
        check(thumbnailFrame(frames, 80, 101), 101);  // Reverse cache miss also holds.
        frames.emplace(190, 0);                       // Stale job finishes; still hold.
        check(thumbnailFrame(frames, 200, 101), 101);
        frames.emplace(200, 0);
        check(thumbnailFrame(frames, 200, 101), 200); // Latest exact result replaces it.
        check(thumbnailFrame(frames, 285, -1), 300);  // Initial hover uses nearest.
        check(thumbnailFrame(frames, 0, 200), 0);     // Deliberate rewind remains possible.
        check(thumbnailFrame(std::map<int, int>{}, 9, 5), -1);
        std::cout << "Thumbnail scrub selection tests passed.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
