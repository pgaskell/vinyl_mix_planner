#pragma once

#include "engine/Engine.h"
#include "engine/Schedule.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace vmp
{

// Keeps the streamed audio the engine is about to play loaded in memory. It reads chunks of the
// decode cache into a fixed pool of memory that is allocated and wired (mlock) once, so neither the
// pool nor the audio thread's reads of it can be paged out. Around the playhead it keeps a window
// of every clip (1 s behind, 8 s ahead), and a little audio where Stop returns to.
//
// Schedules go through the prefetcher: it loads what a new schedule needs around the playhead
// before handing it to the engine, so edits while playing don't cause underruns.
//
// A chunk that is no longer wanted stays loaded for a couple of seconds (seeks and schedule changes
// race with the audio thread), and its memory is reused only once no render that could have seen it
// is still running (Engine::renderCounter()).
class Prefetcher
{
public:
    static constexpr double kBehindSeconds = 1.0;
    static constexpr double kAheadSeconds = 8.0;
    static constexpr double kHotSpotSeconds = 2.0;
    static constexpr double kGraceSeconds = 2.0;

    explicit Prefetcher(Engine& engine, int poolChunks = 256);
    ~Prefetcher();
    Prefetcher(const Prefetcher&) = delete;
    Prefetcher& operator=(const Prefetcher&) = delete;

    // Runs update() on a background thread every few milliseconds until stop() or destruction.
    void start();
    void stop();

    // Message thread. The engine gets the schedule once the audio it needs around the playhead
    // is loaded (or the pool is full).
    void setSchedule(std::unique_ptr<Schedule> schedule);
    // Message thread: keep the audio at this timeline position loaded too (where Stop returns to).
    void setHotSpot(double seconds) { hotSpot_.store(seconds); }

    // One pass: load what is wanted, nearest the playhead first, and release what hasn't been
    // wanted for a while. Called by the background thread; tests call it directly instead.
    void update();

    struct Stats
    {
        int chunksLoaded = 0;
        int poolChunks = 0;
        uint64_t readErrors = 0;
        bool memoryLocked = false; // the pool is wired; if not, it could be paged out
    };
    Stats stats() const;

private:
    struct Want
    {
        std::shared_ptr<StreamedAudio> stream;
        int64_t chunk;
        double priority; // seconds until it's needed, roughly; lower first
    };
    struct Loaded
    {
        std::shared_ptr<StreamedAudio> stream; // kept alive while its chunk is published
        int slot;
        double lastWanted;
    };
    struct Quarantined
    {
        int slot;
        uint64_t renderCounter; // the engine's counter just after the chunk was hidden
    };
    using Key = std::pair<const StreamedAudio*, int64_t>;

    std::vector<Want> wanted(const Schedule& schedule, double playhead) const;
    int takeSlot(double now);
    void release(std::map<Key, Loaded>::iterator it);
    void reclaimQuarantine();
    float* slotMemory(int slot) const { return pool_ + static_cast<size_t>(slot) * StreamedAudio::kChunkFloats; }

    Engine& engine_;
    const int poolChunks_;
    float* pool_ = nullptr;
    size_t poolBytes_ = 0;
    bool memoryLocked_ = false;

    std::atomic<double> hotSpot_;
    std::atomic<uint64_t> readErrors_{0};
    std::atomic<int> chunksLoaded_{0};

    std::mutex pendingLock_;
    std::unique_ptr<Schedule> pending_; // guarded by pendingLock_
    std::condition_variable wake_;
    bool running_ = false; // guarded by pendingLock_
    std::thread thread_;

    // Update thread only.
    std::shared_ptr<const Schedule> current_;   // the newest schedule
    std::shared_ptr<const Schedule> published_; // the last one handed to the engine
    std::shared_ptr<const Schedule> engineHas_; // one the engine has certainly picked up
    std::map<Key, Loaded> loaded_;
    std::vector<int> free_;
    std::vector<Quarantined> quarantine_;
};

} // namespace vmp
