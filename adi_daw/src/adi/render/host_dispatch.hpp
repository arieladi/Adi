// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <chrono>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace adi::render {

// One render worker, one host owner. The app can post service() to its
// message loop. On shutdown the owner drains this mailbox before joining
// the worker, including calls that destroy plug-ins after cancellation.
class HostDispatch {
public:
    void invoke(std::function<void()> action, const std::function<void()>& notify = {}) {
        if (std::this_thread::get_id() == owner_) {
            action();
            return;
        }
        std::packaged_task<void()> task(std::move(action));
        auto done = task.get_future();
        {
            const std::lock_guard lock(mutex_);
            if (pending_.valid()) throw std::logic_error("concurrent calls to a render host");
            pending_ = std::move(task);
        }
        // The owner can still drain the mail when posting is unavailable
        // during shutdown. Do not leave an action referring to a dead stack.
        if (notify) {
            try { notify(); } catch (...) {}
        }
        done.get();
    }

    bool service() {
        if (std::this_thread::get_id() != owner_) return false;
        std::packaged_task<void()> task;
        {
            const std::lock_guard lock(mutex_);
            task = std::move(pending_);
        }
        if (!task.valid()) return false;
        task();
        return true;
    }

    template<class T> void wait(std::future<T>& done) {
        using namespace std::chrono_literals;
        while (done.wait_for(0ms) != std::future_status::ready) {
            if (!service()) std::this_thread::sleep_for(1ms);
        }
    }

private:
    std::thread::id owner_ = std::this_thread::get_id();
    std::mutex mutex_;
    std::packaged_task<void()> pending_;
};

} // namespace adi::render
