#pragma once
#include <mfplay.h>
#include <wrl/client.h>
#include <atomic>
#include <cmath>
#include <optional>

namespace nexplay::playback {
// One outstanding MFPlay seek per player. Mouse moves only replace the queued
// destination: GetPosition is not authoritative until POSITION_SET arrives.
// Each player owns its own callback, so late events from a closed/fullscreen
// player cannot acknowledge a seek belonging to its replacement.
class VideoSeek final {
    class Events final : public IMFPMediaPlayerCallback {
    public:
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
            if (!object) return E_POINTER;
            *object = nullptr;
            if (iid != __uuidof(IUnknown) && iid != __uuidof(IMFPMediaPlayerCallback)) return E_NOINTERFACE;
            *object = static_cast<IMFPMediaPlayerCallback*>(this); AddRef(); return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
        ULONG STDMETHODCALLTYPE Release() override {
            const auto refs = --references_; if (!refs) delete this; return refs;
        }
        void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* event) override {
            if (event && FAILED(event->hrEvent)) error.store(event->hrEvent, std::memory_order_release);
            if (event && event->eEventType == MFP_EVENT_TYPE_MEDIAITEM_SET && SUCCEEDED(event->hrEvent))
                ready.store(true, std::memory_order_release);
            if (event && event->eEventType == MFP_EVENT_TYPE_POSITION_SET) {
                result.store(event->hrEvent, std::memory_order_relaxed);
                completed.fetch_add(1, std::memory_order_release);
            }
        }
        std::atomic<unsigned> completed{};
        std::atomic<bool> ready{};
        std::atomic<HRESULT> error{S_OK};
        std::atomic<HRESULT> result{S_OK};
    private:
        std::atomic<ULONG> references_{1};
    };
public:
    VideoSeek() { reset(); }
    void reset() {
        events_.Attach(new Events);
        pending_ = false; queued_.reset(); held_.reset(); observed_ = 0; failure_ = S_OK;
        failedDispatch_ = false;
        priming_ = 0;
    }
    IMFPMediaPlayerCallback* callback() const { return events_.Get(); }
    bool busy() const { return pending_ || queued_.has_value(); }
    std::optional<double> heldPosition() const { return held_; }
    HRESULT failure() const { return failure_; }
    void request(IMFPMediaPlayer* player, double seconds) {
        held_ = seconds;
        if (pending_ && std::abs(seconds - submitted_) < 0.0000001) queued_.reset();
        else queued_ = seconds;
        dispatch(player);
    }
    // True when the latest requested position (not an obsolete intermediate
    // destination) has completed, so audio can be synchronized exactly once.
    bool poll(IMFPMediaPlayer* player) {
        const HRESULT error = events_->error.exchange(S_OK, std::memory_order_acq_rel);
        if (FAILED(error)) {
            failure_ = error; pending_ = false; queued_.reset(); held_.reset();
            failedDispatch_ = false; priming_ = 0;
            return true;
        }
        if (failedDispatch_) { failedDispatch_ = false; return true; }
        if (!pending_ && queued_) dispatch(player);
        const auto completed = events_->completed.load(std::memory_order_acquire);
        if (!pending_ || completed == observed_) return false;
        observed_ = completed; pending_ = false;
        failure_ = events_->result.load(std::memory_order_relaxed);
        if (FAILED(failure_)) { queued_.reset(); held_.reset(); return true; }
        if (queued_) { dispatch(player); return false; }
        return true;
    }
private:
    void dispatch(IMFPMediaPlayer* player) {
        if (!player || pending_ || !queued_) return;
        MFP_MEDIAPLAYER_STATE state{};
        if (FAILED(player->GetState(&state))) return;
        // A freshly created, never-played MFPlay surface has no presentation
        // clock yet. Prime this permanently muted video player, pause it, then
        // apply the queued destination. This also recovers a stopped EOF state.
        if (!priming_ && (state == MFP_MEDIAPLAYER_STATE_EMPTY || state == MFP_MEDIAPLAYER_STATE_STOPPED)) {
            failure_ = player->Play();
            if (FAILED(failure_)) { queued_.reset(); held_.reset(); failedDispatch_ = true; }
            else priming_ = 1;
            return;
        }
        if (!events_->ready.load(std::memory_order_acquire)) return;
        if (priming_ == 1) {
            if (state != MFP_MEDIAPLAYER_STATE_PLAYING) return;
            failure_ = player->Pause();
            if (FAILED(failure_)) { queued_.reset(); held_.reset(); failedDispatch_ = true; }
            else priming_ = 2;
            return;
        }
        if (priming_ == 2) {
            if (state != MFP_MEDIAPLAYER_STATE_PAUSED) return;
            priming_ = 0;
        }
        submitted_ = *queued_; queued_.reset();
        observed_ = events_->completed.load(std::memory_order_acquire);
        pending_ = true;
        PROPVARIANT position{};
        position.vt = VT_I8;
        position.hVal.QuadPart = static_cast<LONGLONG>(submitted_ * 10'000'000.0);
        failure_ = player->SetPosition(MFP_POSITIONTYPE_100NS, &position);
        if (FAILED(failure_)) { pending_ = false; held_.reset(); failedDispatch_ = true; }
    }
    Microsoft::WRL::ComPtr<Events> events_;
    std::optional<double> queued_, held_;
    bool pending_{};
    bool failedDispatch_{};
    int priming_{};
    unsigned observed_{};
    double submitted_{};
    HRESULT failure_{S_OK};
};
}
