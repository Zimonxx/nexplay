#include "PreviewAudio.h"
#include "Mp4AudioSource.h"
#include <mfapi.h>
#include <mfidl.h>
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace nexplay::playback {
using Microsoft::WRL::ComPtr;
namespace {
void check(HRESULT hr) {
    if (FAILED(hr))
        throw std::runtime_error("Cannot initialize per-track preview audio");
}
void setPosition(IMFPMediaPlayer *player, double seconds) noexcept {
    PROPVARIANT position{};
    position.vt = VT_I8;
    position.hVal.QuadPart = static_cast<LONGLONG>(std::max(0.0, seconds) * 10'000'000);
    player->SetPosition(MFP_POSITIONTYPE_100NS, &position);
}
} // namespace

PreviewAudio::~PreviewAudio() { close(); }

void PreviewAudio::open(const std::filesystem::path &file,
                        const std::vector<AudioSelection> &tracks) {
    close();
    try {
        std::set<std::uint32_t> ids;
        for (const auto &track : tracks) {
            if (track.trackId == 0 || !ids.insert(track.trackId).second)
                throw std::runtime_error("Missing or duplicate preview audio identity");
        }
        for (const auto &track : tracks) {
            auto source = audioTrackSource(file, track.trackId);
            ComPtr<IMFPresentationDescriptor> descriptor;
            check(source->CreatePresentationDescriptor(&descriptor));

            ComPtr<IMFPMediaPlayer> player;
            ComPtr<IMFPMediaItem> item;
            check(MFPCreateMediaPlayer(nullptr, FALSE, 0, nullptr, nullptr, &player));
            check(player->SetMute(TRUE)); // No sound can leak while topology is preparing.
            check(player->CreateMediaItemFromObject(source.Get(), TRUE, 0, &item));
            DWORD count{};
            check(item->GetNumberOfStreams(&count));
            unsigned matched = 0;
            for (DWORD i = 0; i < count; ++i) {
                BOOL selected{};
                ComPtr<IMFStreamDescriptor> stream;
                ComPtr<IMFMediaTypeHandler> handler;
                GUID major{};
                check(descriptor->GetStreamDescriptorByIndex(i, &selected, &stream));
                check(stream->GetMediaTypeHandler(&handler));
                check(handler->GetMajorType(&major));
                const bool audio = major == MFMediaType_Audio;
                const bool wanted = audio;
                check(item->SetStreamSelection(i, wanted));
                if (wanted) ++matched;
            }
            if (matched != 1)
                throw std::runtime_error("Preview audio stream was not found");
            check(player->SetMediaItem(item.Get()));
            players_.push_back({std::move(player), true, track.trackId});
        }
    } catch (...) {
        close();
        throw;
    }
}

void PreviewAudio::close() noexcept {
    for (auto &track : players_) {
        track.player->SetMute(TRUE);
        track.player->Shutdown();
    }
    players_.clear();
    playing_ = false;
    lastSync_ = 0;
}

void PreviewAudio::pause() noexcept {
    if (!playing_) return;
    for (auto &track : players_)
        track.player->Pause();
    playing_ = false;
}

void PreviewAudio::seek(double seconds) noexcept {
    for (auto &track : players_)
        setPosition(track.player.Get(), seconds);
    lastSync_ = GetTickCount64();
}

void PreviewAudio::update(const std::vector<AudioSelection> &tracks, double seconds,
                          bool playing) noexcept {
    // Edits are keyed by the same identity as decoding, never by list position.
    const bool valid =
        std::all_of(tracks.begin(), tracks.end(), [&](const AudioSelection &edit) {
            return std::count_if(tracks.begin(), tracks.end(), [&](const AudioSelection &other) {
                       return edit.trackId == other.trackId;
                   }) == 1 &&
                   std::any_of(players_.begin(), players_.end(), [&](const TrackPlayer &player) {
                       return edit.trackId == player.trackId;
                   });
        });
    if (!valid) {
        for (auto &track : players_) {
            if (SUCCEEDED(track.player->SetMute(TRUE)))
                track.muted = true;
        }
        pause();
        return;
    }
    for (auto &track : players_) {
        const auto edit =
            std::find_if(tracks.begin(), tracks.end(),
                         [&](const AudioSelection &item) { return item.trackId == track.trackId; });
        const bool mute = edit == tracks.end() || !audible(*edit, seconds);
        if (track.muted != mute && SUCCEEDED(track.player->SetMute(mute)))
            track.muted = mute;
    }
    if (playing_ != playing) {
        seek(seconds);
        bool accepted = true;
        for (auto &track : players_) {
            const HRESULT hr = playing ? track.player->Play() : track.player->Pause();
            accepted = accepted && SUCCEEDED(hr);
        }
        if (accepted)
            playing_ = playing;
    }
    const auto now = GetTickCount64();
    if (playing && now - lastSync_ > 500) {
        for (auto &track : players_) {
            PROPVARIANT position{};
            if (SUCCEEDED(track.player->GetPosition(MFP_POSITIONTYPE_100NS, &position)) &&
                position.vt == VT_I8 &&
                std::abs(position.hVal.QuadPart / 10'000'000.0 - seconds) > 0.12) {
                setPosition(track.player.Get(), seconds);
            }
            PropVariantClear(&position);
        }
        lastSync_ = now;
    }
}

bool PreviewAudio::muted(std::size_t index) const noexcept {
    if (index >= players_.size())
        return true;
    BOOL mute = TRUE;
    return FAILED(players_[index].player->GetMute(&mute)) || mute;
}
bool PreviewAudio::ready() const noexcept {
    for (const auto &track : players_) {
        MFP_MEDIAPLAYER_STATE state{};
        if (FAILED(track.player->GetState(&state)) || state == MFP_MEDIAPLAYER_STATE_EMPTY ||
            state == MFP_MEDIAPLAYER_STATE_SHUTDOWN)
            return false;
    }
    return true;
}
bool PreviewAudio::playing(std::size_t index) const noexcept {
    MFP_MEDIAPLAYER_STATE state{};
    return index < players_.size() && SUCCEEDED(players_[index].player->GetState(&state)) &&
        state == MFP_MEDIAPLAYER_STATE_PLAYING;
}
double PreviewAudio::position(std::size_t index) const noexcept {
    if (index >= players_.size()) return -1;
    PROPVARIANT value{};
    const auto hr = players_[index].player->GetPosition(MFP_POSITIONTYPE_100NS, &value);
    const double result = SUCCEEDED(hr) && value.vt == VT_I8
        ? value.hVal.QuadPart / 10'000'000.0 : -1;
    PropVariantClear(&value);
    return result;
}
} // namespace nexplay::playback
