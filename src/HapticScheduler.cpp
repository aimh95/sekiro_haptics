#include "sekiro_haptics/HapticScheduler.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace sekiro_haptics {

namespace {

using Clock = std::chrono::steady_clock;

struct PendingEffect {
    HapticEffectId id;
    Clock::time_point when;
    HapticEffect effect;
};

} // namespace

struct HapticScheduler::Impl {
    explicit Impl(IHapticBackend& backendRef) : backend(backendRef) {
        worker = std::thread([this] { Run(); });
    }

    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        cv.notify_all();
        worker.join();

        // Best-effort: the worker may have exited (because `stopping` was
        // set) before an already-active effect's auto-stop had a chance to
        // fire. Its result isn't checked -- nothing meaningful to do with
        // a failure from a destructor.
        if (effectActive) {
            backend.Reset();
        }
    }

    HapticEffectId Schedule(HapticEffect effect, std::chrono::milliseconds delay) {
        HapticEffectId id;
        {
            std::lock_guard<std::mutex> lock(mutex);
            id = nextId++;
            pending.push_back(PendingEffect{id, Clock::now() + delay, std::move(effect)});
        }
        cv.notify_all();
        return id;
    }

    bool Cancel(HapticEffectId id) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = std::find_if(pending.begin(), pending.end(),
                                [id](const PendingEffect& p) { return p.id == id; });
        if (it == pending.end()) {
            return false;
        }
        pending.erase(it);
        return true;
    }

    void Reset() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending.clear();
            // Invalidate any pending auto-stop: this explicit Reset()
            // already stops the controller below, and bumping the
            // generation means a worker iteration that was about to fire
            // the (now redundant) auto-stop for whatever was active
            // recognizes it's stale and skips it -- see Run().
            ++activeGeneration;
            effectActive = false;
        }
        cv.notify_all();
        backend.Reset();
    }

    // Worker loop: each iteration finds the earliest thing to do --
    // dispatching the earliest still-pending effect, or auto-stopping the
    // currently-active effect at its expiry, whichever is sooner -- and
    // either does it (if due) or sleeps until it is due. Any
    // Schedule/Cancel/Reset call notifies the condition variable so the
    // sleep is re-evaluated against current state rather than a stale
    // deadline.
    //
    // Duration/auto-stop, "latest effect wins": `effectActive`/
    // `activeExpiry` describe at most ONE currently-active effect at a
    // time -- there is no queue of pending stops. When a new effect
    // dispatches, it simply overwrites `activeExpiry` (and bumps
    // `activeGeneration`), which is what makes an older effect's expiry
    // moot: the next iteration computes "what's next" from the *current*
    // `activeExpiry`, so a superseded deadline is never separately acted
    // on. `activeGeneration` additionally guards the brief window where
    // the worker has unlocked to call backend.Reset() for an expiry: only
    // clear `effectActive` afterward if nothing newer took over while
    // unlocked (see the stop branch below). With a single worker thread
    // driving both dispatch and stop, and Schedule() never touching
    // `effectActive`/`activeExpiry` directly, this can't currently race --
    // the generation guard keeps that true even if this code is refactored
    // later, rather than relying on today's single-thread shape implicitly.
    void Run() {
        std::unique_lock<std::mutex> lock(mutex);
        while (!stopping) {
            bool haveWake = false;
            Clock::time_point nextWake{};
            auto nextEffectIt = pending.end();

            if (!pending.empty()) {
                nextEffectIt = std::min_element(pending.begin(), pending.end(),
                                                 [](const PendingEffect& a, const PendingEffect& b) {
                                                     return a.when < b.when;
                                                 });
                nextWake = nextEffectIt->when;
                haveWake = true;
            }

            bool stopIsNext = false;
            if (effectActive && (!haveWake || activeExpiry < nextWake)) {
                nextWake = activeExpiry;
                haveWake = true;
                stopIsNext = true;
            }

            if (!haveWake) {
                cv.wait(lock);
                continue;
            }

            if (Clock::now() < nextWake) {
                cv.wait_until(lock, nextWake);
                continue;
            }

            if (stopIsNext) {
                std::uint64_t generationAtWake = activeGeneration;
                lock.unlock();
                backend.Reset();
                lock.lock();
                // Only clear "active" if nothing newer superseded this
                // expiry while unlocked; a newer effect already installed
                // its own activeExpiry/activeGeneration and must not be
                // turned off by this now-stale stop.
                if (activeGeneration == generationAtWake) {
                    effectActive = false;
                }
                continue;
            }

            PendingEffect due = std::move(*nextEffectIt);
            pending.erase(nextEffectIt);
            std::chrono::milliseconds duration = due.effect.duration;

            lock.unlock();
            HapticBackendResult result = backend.SendEffect(due.effect);
            lock.lock();

            if (result == HapticBackendResult::Success) {
                ++activeGeneration;
                effectActive = true;
                // Non-positive duration: expiry is already due, so the
                // very next iteration fires the stop immediately.
                activeExpiry = Clock::now() + std::max(duration, std::chrono::milliseconds{0});
            }
            // A failed dispatch never marks anything active/changes the
            // expiry -- whatever was already active (if anything) keeps
            // its own schedule; a failed send never "started" anything for
            // this scheduler's bookkeeping purposes.
        }
    }

    IHapticBackend& backend;
    std::thread worker;

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<PendingEffect> pending;
    HapticEffectId nextId = 1;
    bool stopping = false;

    // Duration/auto-stop state for the single currently-active effect, if
    // any. See the Run() comment above for the "latest effect wins" policy
    // this implements.
    bool effectActive = false;
    Clock::time_point activeExpiry{};
    std::uint64_t activeGeneration = 0;
};

HapticScheduler::HapticScheduler(IHapticBackend& backend) : impl_(std::make_unique<Impl>(backend)) {}

HapticScheduler::~HapticScheduler() = default;

HapticEffectId HapticScheduler::Schedule(HapticEffect effect, std::chrono::milliseconds delay) {
    return impl_->Schedule(std::move(effect), delay);
}

bool HapticScheduler::Cancel(HapticEffectId id) {
    return impl_->Cancel(id);
}

void HapticScheduler::Reset() {
    impl_->Reset();
}

} // namespace sekiro_haptics
