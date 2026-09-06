#include "Mp4AudioSource.h"
#include <mfapi.h>
#include <wrl/implements.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>

namespace nexplay::playback {
using namespace Microsoft::WRL;
namespace {
void check(HRESULT hr) {
    if (FAILED(hr))
        throw std::runtime_error("Cannot open the identified MP4 audio track");
}
constexpr std::uint32_t tag(char a, char b, char c, char d) {
    return (std::uint32_t(a) << 24) | (std::uint32_t(b) << 16) | (std::uint32_t(c) << 8) |
           std::uint32_t(d);
}
std::uint64_t number(std::istream &input, std::uint64_t at, unsigned size) {
    std::array<unsigned char, 8> bytes{};
    input.seekg(static_cast<std::streamoff>(at));
    input.read(reinterpret_cast<char *>(bytes.data()), size);
    if (!input)
        throw std::runtime_error("Truncated MP4 metadata");
    std::uint64_t result{};
    for (unsigned i = 0; i < size; ++i)
        result = (result << 8) | bytes[i];
    return result;
}
struct Box {
    std::uint64_t start, payload, end;
    std::uint32_t type;
};
template <class Visitor>
void boxes(std::istream &input, std::uint64_t start, std::uint64_t end, Visitor visit) {
    while (start < end) {
        if (end - start < 8)
            throw std::runtime_error("Invalid MP4 box header");
        auto size = number(input, start, 4);
        const auto type = static_cast<std::uint32_t>(number(input, start + 4, 4));
        std::uint64_t header = 8;
        if (size == 1) {
            if (end - start < 16)
                throw std::runtime_error("Truncated MP4 extended box");
            size = number(input, start + 8, 8);
            header = 16;
        } else if (size == 0)
            size = end - start;
        if (size < header || size > end - start)
            throw std::runtime_error("MP4 box exceeds its parent");
        visit(Box{start, start + header, start + size, type});
        start += size;
    }
}
using Patches = std::vector<std::uint64_t>;
void patchBytes(const Patches &patches, QWORD position, BYTE *bytes, ULONG count) noexcept {
    constexpr BYTE replacement[]{'f', 'r', 'e', 'e'};
    for (auto patch : patches) {
        for (unsigned i = 0; i < 4; ++i) {
            if (patch + i >= position && patch + i - position < count)
                bytes[patch + i - position] = replacement[i];
        }
    }
}
// Private metadata on our async results: number of bytes actually returned.
constexpr GUID readCountKey{
    0xaf038c8a, 0x6f23, 0x4f1b, {0xa0, 0xe1, 0x4b, 0x59, 0x8d, 0x9d, 0x01, 0x12}};
class PatchedRead final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMFAsyncCallback, FtmBase> {
  public:
    ComPtr<IMFByteStream> input;
    ComPtr<IMFAsyncResult> result;
    ComPtr<IMFAttributes> metadata;
    std::shared_ptr<const Patches> patches;
    BYTE *bytes{};
    QWORD position{};
    STDMETHODIMP GetParameters(DWORD *, DWORD *) override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(IMFAsyncResult *completed) override {
        ULONG count{};
        HRESULT hr = input->EndRead(completed, &count);
        if (SUCCEEDED(hr)) {
            patchBytes(*patches, position, bytes, count);
            hr = metadata->SetUINT32(readCountKey, count);
        }
        result->SetStatus(hr);
        return MFInvokeCallback(result.Get());
    }
};
// Present all unwanted 'trak' boxes as equally-sized 'free' boxes IN MEMORY.
// Keeping byte offsets and lengths identical preserves every sample table and
// timestamp. The decoder can only see the chosen track, regardless of MF order.
class TrackByteStream final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMFByteStream, FtmBase> {
  public:
    ComPtr<IMFByteStream> input;
    std::shared_ptr<const Patches> patches;
    std::mutex cursor;
    STDMETHODIMP GetCapabilities(DWORD *output) override { return input->GetCapabilities(output); }
    STDMETHODIMP GetLength(QWORD *output) override { return input->GetLength(output); }
    STDMETHODIMP SetLength(QWORD) override { return E_ACCESSDENIED; }
    STDMETHODIMP GetCurrentPosition(QWORD *output) override {
        std::lock_guard lock(cursor);
        return input->GetCurrentPosition(output);
    }
    STDMETHODIMP SetCurrentPosition(QWORD position) override {
        std::lock_guard lock(cursor);
        return input->SetCurrentPosition(position);
    }
    STDMETHODIMP IsEndOfStream(BOOL *output) override { return input->IsEndOfStream(output); }
    STDMETHODIMP Read(BYTE *bytes, ULONG size, ULONG *read) override {
        if (!bytes || !read)
            return E_POINTER;
        std::lock_guard lock(cursor);
        QWORD position{};
        HRESULT hr = input->GetCurrentPosition(&position);
        if (SUCCEEDED(hr))
            hr = input->Read(bytes, size, read);
        if (SUCCEEDED(hr))
            patchBytes(*patches, position, bytes, *read);
        return hr;
    }
    STDMETHODIMP BeginRead(BYTE *bytes, ULONG size, IMFAsyncCallback *callback,
                           IUnknown *state) override {
        if (!bytes || !callback)
            return E_POINTER;
        auto pending = Make<PatchedRead>();
        if (!pending)
            return E_OUTOFMEMORY;
        pending->input = input;
        pending->patches = patches;
        pending->bytes = bytes;
        HRESULT hr = MFCreateAttributes(&pending->metadata, 1);
        if (SUCCEEDED(hr))
            hr = MFCreateAsyncResult(pending->metadata.Get(), callback, state, &pending->result);
        if (FAILED(hr))
            return hr;
        std::lock_guard lock(cursor);
        hr = input->GetCurrentPosition(&pending->position);
        if (SUCCEEDED(hr))
            hr = input->BeginRead(bytes, size, pending.Get(), nullptr);
        return hr;
    }
    STDMETHODIMP EndRead(IMFAsyncResult *result, ULONG *read) override {
        if (!result || !read)
            return E_POINTER;
        *read = 0;
        HRESULT hr = result->GetStatus();
        ComPtr<IUnknown> object;
        ComPtr<IMFAttributes> metadata;
        if (SUCCEEDED(hr))
            hr = result->GetObject(&object);
        if (SUCCEEDED(hr))
            hr = object.As(&metadata);
        UINT32 count{};
        if (SUCCEEDED(hr))
            hr = metadata->GetUINT32(readCountKey, &count);
        if (SUCCEEDED(hr))
            *read = count;
        return hr;
    }
    STDMETHODIMP Write(const BYTE *, ULONG, ULONG *) override { return E_ACCESSDENIED; }
    STDMETHODIMP BeginWrite(const BYTE *, ULONG, IMFAsyncCallback *, IUnknown *) override {
        return E_ACCESSDENIED;
    }
    STDMETHODIMP EndWrite(IMFAsyncResult *, ULONG *) override { return E_ACCESSDENIED; }
    STDMETHODIMP Seek(MFBYTESTREAM_SEEK_ORIGIN origin, LONGLONG offset, DWORD flags,
                      QWORD *current) override {
        std::lock_guard lock(cursor);
        return input->Seek(origin, offset, flags, current);
    }
    STDMETHODIMP Flush() override { return S_OK; }
    STDMETHODIMP Close() override { return input->Close(); }
};
} // namespace

std::vector<Mp4Track> readMp4Tracks(std::istream &input, std::uint64_t length) {
    std::vector<Mp4Track> tracks;
    std::set<std::uint32_t> ids;
    bool foundMoov = false;
    boxes(input, 0, length, [&](Box root) {
        if (root.type == tag('m', 'o', 'o', 'f'))
            throw std::runtime_error("Fragmented MP4 audio preview is not supported");
        if (root.type != tag('m', 'o', 'o', 'v'))
            return;
        if (foundMoov)
            throw std::runtime_error("Duplicate MP4 movie header");
        foundMoov = true;
        boxes(input, root.payload, root.end, [&](Box box) {
            if (box.type == tag('m', 'v', 'e', 'x'))
                throw std::runtime_error("Fragmented MP4 audio preview is not supported");
            if (box.type != tag('t', 'r', 'a', 'k'))
                return;
            Mp4Track track{0, box.start + 4, false};
            bool headerSeen = false, mediaSeen = false, handlerSeen = false;
            boxes(input, box.payload, box.end, [&](Box child) {
                if (child.type == tag('t', 'k', 'h', 'd')) {
                    if (headerSeen || child.end - child.payload < 4)
                        throw std::runtime_error("Invalid MP4 track header");
                    headerSeen = true;
                    const auto version = number(input, child.payload, 1);
                    const unsigned idOffset = version == 0 ? 12 : 20;
                    if (version > 1 || child.end - child.payload < idOffset + 4)
                        throw std::runtime_error("Truncated MP4 track ID");
                    track.id =
                        static_cast<std::uint32_t>(number(input, child.payload + idOffset, 4));
                } else if (child.type == tag('m', 'd', 'i', 'a')) {
                    if (mediaSeen)
                        throw std::runtime_error("Duplicate MP4 media header");
                    mediaSeen = true;
                    boxes(input, child.payload, child.end, [&](Box media) {
                        if (media.type == tag('h', 'd', 'l', 'r')) {
                            if (handlerSeen || media.end - media.payload < 12)
                                throw std::runtime_error("Invalid MP4 handler");
                            handlerSeen = true;
                            track.audio =
                                number(input, media.payload + 8, 4) == tag('s', 'o', 'u', 'n');
                        }
                    });
                }
            });
            if (!handlerSeen || track.id == 0 || !ids.insert(track.id).second ||
                tracks.size() >= 4096)
                throw std::runtime_error("Missing or ambiguous MP4 track identity");
            tracks.push_back(track);
        });
    });
    if (!foundMoov || tracks.empty())
        throw std::runtime_error("No MP4 tracks found");
    return tracks;
}

ComPtr<IMFMediaSource> audioTrackSource(const std::filesystem::path &file, std::uint32_t id) {
    std::ifstream input(file, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot read MP4 metadata");
    const auto tracks = readMp4Tracks(input, std::filesystem::file_size(file));
    auto patches = std::make_shared<Patches>();
    bool found = false;
    for (const auto &track : tracks) {
        if (track.id == id)
            found = track.audio;
        else
            patches->push_back(track.typeOffset);
    }
    if (!found)
        throw std::runtime_error("Requested MP4 audio track ID was not found");
    auto stream = Make<TrackByteStream>();
    if (!stream)
        throw std::bad_alloc();
    stream->patches = std::move(patches);
    check(MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST, MF_FILEFLAGS_NONE,
                       file.c_str(), &stream->input));
    ComPtr<IMFSourceResolver> resolver;
    ComPtr<IUnknown> object;
    ComPtr<IMFMediaSource> source;
    MF_OBJECT_TYPE type{};
    check(MFCreateSourceResolver(&resolver));
    check(resolver->CreateObjectFromByteStream(stream.Get(), L"NexPlay-preview.mp4",
                                               MF_RESOLUTION_MEDIASOURCE, nullptr, &type, &object));
    check(object.As(&source));
    return source;
}
} // namespace nexplay::playback
