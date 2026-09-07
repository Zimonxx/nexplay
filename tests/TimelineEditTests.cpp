#include "editing/TimelineEdit.h"
#include <iostream>
#include <stdexcept>
using namespace nexplay::editing;
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
int main() {
    try {
        TimelineEdit trim{2, 8, 10, false};
        auto audio = trim.audioRanges(0, 10);
        require(audio.size() == 1 && audio[0].start == 2 && audio[0].end == 8,
                "Audio did not follow video edges");
        audio = trim.audioRanges(3, 7);
        require(audio[0].start == 3 && audio[0].end == 7, "Individual audio edit was lost");
        require(trim.audioRanges(0, 1).empty(), "Audio outside video trim is audible");
        require(!trim.contains(1.9) && trim.contains(2) && !trim.contains(8),
                "Bad trim boundaries");
        TimelineEdit cut{2, 8, 10, true};
        audio = cut.audioRanges(0, 10);
        require(audio.size() == 2 && audio[0].start == 0 && audio[0].end == 2 &&
                    audio[1].start == 8 && audio[1].end == 10,
                "Middle cut did not affect audio");
        require(cut.outputDuration() == 4 && trim.outputDuration() == 6, "Bad export duration");
        require(!cut.contains(2) && !cut.contains(7.9) && cut.contains(8),
                "Cut audio still audible");
        require(TimelineEdit{0, 10, 10, true}.keptRanges().empty(), "Whole deletion is not empty");
        require(TimelineEdit{-1, 2, 10, false}.keptRanges().empty(), "Invalid edit accepted");
        require(TimelineEdit{0, 0.5, 10, true}.keptRanges()[0].start == 0.5,
                "Leading cut mismatch");
        require(TimelineEdit{9.5, 10, 10, true}.keptRanges()[0].end == 9.5,
                "Trailing cut mismatch");
        require(TimelineEdit{0.001, 9.999, 10, true}.keptRanges().size() == 2,
                "Short fragments must use the same policy for audio and video");
        std::cout << "Shared video/audio trim, cut and duration policy passed.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
