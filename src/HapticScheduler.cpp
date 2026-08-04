#include "sekiro_haptics/HapticScheduler.hpp"

#include <algorithm>
#include <condition_variable>
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
        }
        cv.notify_all();
        backend.Reset();
    }

    // Worker loop: repeatedly finds the earliest still-pending effect and
    // either dispatches it (if due) or sleeps until it is due, whichever
    // comes first. Any Schedule/Cancel/Reset call notifies the condition
    // variable so the sleep is re-evaluated against the current state
    // rather than a stale deadline.
    void Run() {
        std::unique_lock<std::mutex> lock(mutex);
        while (!stopping) {
            if (pending.empty()) {
                cv.wait(lock);
                continue;
            }

            auto next = std::min_element(pending.begin(), pending.end(),
                                          [](const PendingEffect& a, const PendingEffect& b) {
                                              return a.when < b.when;
                                          });

            if (Clock::now() >= next->when) {
                PendingEffect due = std::move(*next);
                pending.erase(next);

                lock.unlock();
                backend.SendEffect(due.effect);
                lock.lock();
                continue;
            }

            cv.wait_until(lock, next->when);
        }
    }

    IHapticBackend& backend;
    std::thread worker;

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<PendingEffect> pending;
    HapticEffectId nextId = 1;
    bool stopping = false;
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
