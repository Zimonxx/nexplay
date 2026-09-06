#include "PreviewAudio.h"
#include <mfapi.h>
#include <mfidl.h>
#include <algorithm>
#include <cmath>
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
        for (std::size_t ordinal = 0; ordinal < tracks.size(); ++ordinal) {
            ComPtr<IMFSourceResolver> resolver;
            ComPtr<IUnknown> object;
            ComPtr<IMFMediaSource> source;
            ComPtr<IMFPresentationDescriptor> descriptor;
            MF_OBJECT_TYPE type{};
            check(MFCreateSourceResolver(&resolver));
            check(resolver->CreateObjectFromURL(file.c_str(), MF_RESOLUTION_MEDIASOURCE, nullptr,
                                                &type, &object));
            check(object.As(&source));
            check(source->CreatePresentationDescriptor(&descriptor));

            ComPtr<IMFPMediaPlayer> player;
            ComPtr<IMFPMediaItem> item;
            check(MFPCreateMediaPlayer(nullptr, FALSE, 0, nullptr, nullptr, &player));
            check(player->SetMute(TRUE)); // No sound can leak while topology is preparing.
            check(player->CreateMediaItemFromObject(source.Get(), TRUE, 0, &item));
            DWORD count{};
            check(item->GetNumberOfStreams(&count));
            DWORD audioOrdinal = 0;
            bool matched = false;
            for (DWORD i = 0; i < count; ++i) {
                BOOL selected{};
                ComPtr<IMFStreamDescriptor> stream;
                ComPtr<IMFMediaTypeHandler> handler;
                GUID major{};
                check(descriptor->GetStreamDescriptorByIndex(i, &selected, &stream));
                check(stream->GetMediaTypeHandler(&handler));
                check(handler->GetMajorType(&major));
                const bool audio = major == MFMediaType_Audio;
                // Both the editor and this source enumerate audio in file order.
                // MF assigns its own stream IDs; they are NOT the MP4 track IDs.
                const bool wanted = audio && audioOrdinal == ordinal;
                check(item->SetStreamSelection(i, wanted));
                matched = matched || wanted;
                if (audio)
                    ++audioOrdinal;
            }
            if (!matched)
                throw std::runtime_error("Preview audio stream was not found");
            check(player->SetMediaItem(item.Get()));
            players_.push_back({std::move(player), true});
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
    if (tracks.size() != players_.size()) {
        pause();
        return;
    }
    for (std::size_t i = 0; i < players_.size(); ++i) {
        auto &track = players_[i];
        const bool mute = !audible(tracks[i], seconds);
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
} // namespace nexplay::playback
