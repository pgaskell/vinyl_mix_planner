#include "core/StreamedAudio.h"

#include "core/TrackAudio.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <string>

namespace vmp
{

namespace
{
constexpr char kMagic[8] = {'V', 'M', 'P', 'A', 'U', 'D', 'I', 'O'};
constexpr uint32_t kFileVersion = 1;
constexpr off_t kHeaderBytes = 16384; // keeps the samples page aligned
// macOS rejects single reads and writes over INT_MAX bytes (a 2 GB track is ~100 minutes).
constexpr size_t kMaxIo = size_t{1} << 30;

struct Header
{
    char magic[8];
    uint32_t version;
    uint32_t channels;
    int64_t frames;
    double sampleRate;
};

bool writeAll(int fd, const void* data, size_t bytes, off_t offset)
{
    const auto* p = static_cast<const char*>(data);
    while (bytes > 0)
    {
        const ssize_t n = ::pwrite(fd, p, std::min(bytes, kMaxIo), offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        p += n;
        bytes -= static_cast<size_t>(n);
        offset += n;
    }
    return true;
}

bool readAllBytes(int fd, void* data, size_t bytes, off_t offset)
{
    auto* p = static_cast<char*>(data);
    while (bytes > 0)
    {
        const ssize_t n = ::pread(fd, p, std::min(bytes, kMaxIo), offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) // error, or the file is shorter than it should be
            return false;
        p += n;
        bytes -= static_cast<size_t>(n);
        offset += n;
    }
    return true;
}
} // namespace

std::filesystem::path temporaryPathFor(const std::filesystem::path& file)
{
    static std::atomic<unsigned> counter{0};
    auto temp = file;
    temp += "." + std::to_string(::getpid()) + "-" + std::to_string(counter.fetch_add(1)) + ".tmp";
    return temp;
}

bool StreamedAudio::write(const std::filesystem::path& file, const TrackAudio& audio)
{
    const auto temp = temporaryPathFor(file);
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return false;

    Header header{};
    std::memcpy(header.magic, kMagic, sizeof kMagic);
    header.version = kFileVersion;
    header.channels = 2;
    header.frames = audio.frames;
    header.sampleRate = audio.sampleRate;

    const size_t bytes = static_cast<size_t>(audio.frames) * 2 * sizeof(float);
    bool ok = audio.interleaved.size() * sizeof(float) >= bytes && writeAll(fd, &header, sizeof header, 0) &&
              ::ftruncate(fd, kHeaderBytes) == 0 && writeAll(fd, audio.interleaved.data(), bytes, kHeaderBytes);
#ifdef F_FULLFSYNC
    ok = ok && ::fcntl(fd, F_FULLFSYNC) != -1; // plain fsync doesn't flush the drive's cache on macOS
#else
    ok = ok && ::fsync(fd) == 0;
#endif
    ok = ::close(fd) == 0 && ok;

    std::error_code error;
    if (ok)
        std::filesystem::rename(temp, file, error);
    if (!ok || error)
    {
        std::filesystem::remove(temp, error);
        return false;
    }
    return true;
}

std::unique_ptr<StreamedAudio> StreamedAudio::open(const std::filesystem::path& file)
{
    const int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return nullptr;

    std::unique_ptr<StreamedAudio> audio(new StreamedAudio);
    audio->fd_ = fd; // closed by the destructor from here on

    Header header{};
    struct stat info{};
    if (!readAllBytes(fd, &header, sizeof header, 0) || std::memcmp(header.magic, kMagic, sizeof kMagic) != 0 ||
        header.version != kFileVersion || header.channels != 2 || header.frames <= 0 || !(header.sampleRate > 0.0) ||
        ::fstat(fd, &info) != 0 ||
        info.st_size != kHeaderBytes + static_cast<off_t>(header.frames) * 2 * static_cast<off_t>(sizeof(float)))
        return nullptr;

    audio->frames_ = header.frames;
    audio->sampleRate_ = header.sampleRate;
    audio->numChunks_ = (header.frames + kChunkFrames - 1) / kChunkFrames;
    audio->chunks_ = std::make_unique<std::atomic<const float*>[]>(static_cast<size_t>(audio->numChunks_));
    for (int64_t i = 0; i < audio->numChunks_; ++i)
        audio->chunks_[static_cast<size_t>(i)].store(nullptr);
    return audio;
}

StreamedAudio::~StreamedAudio()
{
    if (fd_ >= 0)
        ::close(fd_);
}

int64_t StreamedAudio::chunkFrames(int64_t index) const
{
    return std::min(kChunkFrames, frames_ - index * kChunkFrames);
}

bool StreamedAudio::read(int64_t index, float* destination) const
{
    if (index < 0 || index >= numChunks_)
        return false;
    const auto bytes = static_cast<size_t>(chunkFrames(index)) * 2 * sizeof(float);
    const off_t offset = kHeaderBytes + static_cast<off_t>(index * kChunkFrames) * 2 * static_cast<off_t>(sizeof(float));
    return readAllBytes(fd_, destination, bytes, offset);
}

std::shared_ptr<TrackAudio> StreamedAudio::readAll() const
{
    auto audio = std::make_shared<TrackAudio>();
    audio->frames = frames_;
    audio->sampleRate = sampleRate_;
    audio->interleaved.resize(static_cast<size_t>(frames_) * 2);
    if (!readAllBytes(fd_, audio->interleaved.data(), audio->interleaved.size() * sizeof(float), kHeaderBytes))
        return nullptr;
    return audio;
}

} // namespace vmp
