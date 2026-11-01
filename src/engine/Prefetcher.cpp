#include "engine/Prefetcher.h"

#include <sys/mman.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace vmp
{

namespace
{
constexpr auto kInterval = std::chrono::milliseconds(5);

double nowSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

Prefetcher::Prefetcher(Engine& engine, int poolChunks)
    : engine_(engine), poolChunks_(std::max(poolChunks, 1)), hotSpot_(std::numeric_limits<double>::quiet_NaN())
{
    poolBytes_ = static_cast<size_t>(poolChunks_) * StreamedAudio::kChunkFloats * sizeof(float);
    void* memory = ::mmap(nullptr, poolBytes_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED)
        throw std::bad_alloc();
    pool_ = static_cast<float*>(memory);
    memoryLocked_ = ::mlock(pool_, poolBytes_) == 0; // also faults every page in
    free_.reserve(static_cast<size_t>(poolChunks_));
    for (int slot = poolChunks_ - 1; slot >= 0; --slot)
        free_.push_back(slot);
}

Prefetcher::~Prefetcher()
{
    stop();
    // Hide every chunk before the memory goes. The host has stopped the audio callback by now.
    for (auto& [key, entry] : loaded_)
        entry.stream->publish(key.second, nullptr);
    loaded_.clear();
    if (memoryLocked_)
        ::munlock(pool_, poolBytes_);
    ::munmap(pool_, poolBytes_);
}

void Prefetcher::start()
{
    std::lock_guard lock(pendingLock_);
    if (running_)
        return;
    running_ = true;
    thread_ = std::thread([this] {
        std::unique_lock guard(pendingLock_);
        while (running_)
        {
            guard.unlock();
            update();
            guard.lock();
            wake_.wait_for(guard, kInterval, [this] { return !running_ || pending_ != nullptr; });
        }
    });
}

void Prefetcher::stop()
{
    {
        std::lock_guard lock(pendingLock_);
        running_ = false;
    }
    wake_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void Prefetcher::setSchedule(std::unique_ptr<Schedule> schedule)
{
    {
        std::lock_guard lock(pendingLock_);
        pending_ = std::move(schedule);
    }
    wake_.notify_all();
}

Prefetcher::Stats Prefetcher::stats() const
{
    return {chunksLoaded_.load(), poolChunks_, readErrors_.load(), memoryLocked_};
}

std::vector<Prefetcher::Want> Prefetcher::wanted(const Schedule& schedule, double playhead) const
{
    std::vector<Want> wants;

    // The chunks of a clip's audio heard between timeline times from and to (clipped to the clip),
    // with the deck's read margin. Priority is roughly how far each chunk is from `near` on the
    // timeline, so a clip starting later waits its turn behind the one playing.
    auto add = [&](const ScheduledClip& clip, double from, double to, double near, double basePriority) {
        if (clip.audio == nullptr || clip.audio->stream == nullptr)
            return;
        from = std::max(from, clip.start);
        to = std::min(to, clip.end);
        if (to <= from)
            return;
        const auto& stream = clip.audio->stream;
        const double margin = Deck::kReadMarginSeconds * clip.audio->sampleRate;
        const double first = clip.timing.sourceFrameAt(from) - margin, last = clip.timing.sourceFrameAt(to) + margin;
        const double closest = std::clamp(near, from, to);
        const double nearFrame = clip.timing.sourceFrameAt(closest);
        // Source frames per timeline second over the window (pitch), to turn frames into time.
        const double framesPerSecond =
            std::max(1.0, (clip.timing.sourceFrameAt(to) - clip.timing.sourceFrameAt(from)) / (to - from));
        const auto firstChunk = std::max<int64_t>(0, static_cast<int64_t>(std::floor(first / StreamedAudio::kChunkFrames)));
        const auto lastChunk =
            std::min(stream->numChunks() - 1, static_cast<int64_t>(std::floor(last / StreamedAudio::kChunkFrames)));
        for (int64_t chunk = firstChunk; chunk <= lastChunk; ++chunk)
        {
            const double middle = (static_cast<double>(chunk) + 0.5) * StreamedAudio::kChunkFrames;
            const double distance = std::abs(closest - near) + std::abs(middle - nearFrame) / framesPerSecond;
            wants.push_back({stream, chunk, basePriority + distance});
        }
    };

    const double hotSpot = hotSpot_.load();
    for (int d = 0; d < schedule.numDecks; ++d)
        for (const auto& clip : schedule.lanes[static_cast<size_t>(d)])
        {
            add(clip, playhead - kBehindSeconds, playhead + kAheadSeconds, playhead, 0.0);
            if (!std::isnan(hotSpot))
                add(clip, hotSpot, hotSpot + kHotSpotSeconds, hotSpot, 1.0);
        }

    std::sort(wants.begin(), wants.end(), [](const Want& a, const Want& b) { return a.priority < b.priority; });
    return wants;
}

void Prefetcher::update()
{
    std::unique_ptr<Schedule> incoming;
    {
        std::lock_guard lock(pendingLock_);
        incoming = std::move(pending_);
    }
    const bool changed = incoming != nullptr;
    if (changed)
        current_ = std::shared_ptr<const Schedule>(std::move(incoming));
    // Once the engine has taken the last schedule we gave it, only that one matters.
    if (!engine_.schedulePending())
        engineHas_ = published_;

    reclaimQuarantine();
    const double now = nowSeconds();

    if (current_ != nullptr)
    {
        auto wants = wanted(*current_, engine_.prefetchTarget());
        // Until the engine picks up the newest schedule it is still playing an older one; keep its
        // audio too (edits while the message thread is busy).
        if (engineHas_ != nullptr && engineHas_ != current_)
        {
            auto older = wanted(*engineHas_, engine_.prefetchTarget());
            wants.insert(wants.end(), older.begin(), older.end());
            std::sort(wants.begin(), wants.end(), [](const Want& a, const Want& b) { return a.priority < b.priority; });
        }
        for (const auto& want : wants)
        {
            const Key key{want.stream.get(), want.chunk};
            if (auto it = loaded_.find(key); it != loaded_.end())
            {
                it->second.lastWanted = now;
                continue;
            }
            const int slot = takeSlot(now);
            if (slot < 0)
                break; // pool full: the rest is further away
            if (!want.stream->read(want.chunk, slotMemory(slot)))
            {
                readErrors_.fetch_add(1);
                free_.push_back(slot);
                continue;
            }
            want.stream->publish(want.chunk, slotMemory(slot));
            loaded_.emplace(key, Loaded{want.stream, slot, now});
        }
    }

    // Hand over a new schedule now that its audio around the playhead is in.
    if (changed)
    {
        engine_.setSchedule(std::make_unique<Schedule>(*current_));
        published_ = current_;
    }

    for (auto it = loaded_.begin(); it != loaded_.end();)
    {
        auto next = std::next(it);
        if (now - it->second.lastWanted > kGraceSeconds)
            release(it);
        it = next;
    }
    chunksLoaded_.store(static_cast<int>(loaded_.size()));
}

int Prefetcher::takeSlot(double now)
{
    if (free_.empty())
        reclaimQuarantine();
    if (free_.empty())
    {
        // Give up the least recently wanted chunk that isn't wanted right now. Its memory becomes
        // usable on a later pass, once the audio thread can't be reading it.
        auto oldest = loaded_.end();
        for (auto it = loaded_.begin(); it != loaded_.end(); ++it)
            if (oldest == loaded_.end() || it->second.lastWanted < oldest->second.lastWanted)
                oldest = it;
        if (oldest != loaded_.end() && oldest->second.lastWanted < now) // not wanted in this pass
            release(oldest);
        reclaimQuarantine();
    }
    if (free_.empty())
        return -1;
    const int slot = free_.back();
    free_.pop_back();
    return slot;
}

void Prefetcher::release(std::map<Key, Loaded>::iterator it)
{
    it->second.stream->publish(it->first.second, nullptr);
    // Pairs with the fence after the engine bumps its counter: either the render sees the chunk
    // hidden, or we see that render's counter and wait for it to finish.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    quarantine_.push_back({it->second.slot, engine_.renderCounter()});
    loaded_.erase(it);
}

void Prefetcher::reclaimQuarantine()
{
    if (quarantine_.empty())
        return;
    const uint64_t counter = engine_.renderCounter();
    std::erase_if(quarantine_, [&](const Quarantined& q) {
        // Even: no render was running when the chunk was hidden. Changed: that render has finished.
        if (q.renderCounter % 2 == 0 || counter != q.renderCounter)
        {
            free_.push_back(q.slot);
            return true;
        }
        return false;
    });
}

} // namespace vmp
