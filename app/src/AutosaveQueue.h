#pragma once
// One background thread that runs recovery-file work in the order it was
// posted. Autosave snapshots a document on the UI thread (a memory copy) and
// compresses and writes it here, so a large document does not freeze the
// editor. Removals go through the same queue, so a removal posted after a
// write always wins. The destructor finishes everything still queued.
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace acestudio::ui {

class AutosaveQueue {
public:
    AutosaveQueue() : m_thread([this] { run(); }) {}
    ~AutosaveQueue() {
        {
            std::lock_guard lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_one();
        m_thread.join();
    }
    AutosaveQueue(const AutosaveQueue&) = delete;
    AutosaveQueue& operator=(const AutosaveQueue&) = delete;

    void post(std::function<void()> task) {
        {
            std::lock_guard lock(m_mutex);
            m_tasks.push_back(std::move(task));
        }
        m_cv.notify_one();
    }

    // Blocks until everything posted so far has run.
    void drain() {
        std::unique_lock lock(m_mutex);
        m_idle.wait(lock, [this] { return m_tasks.empty() && !m_busy; });
    }

private:
    void run() {
        std::unique_lock lock(m_mutex);
        for (;;) {
            m_cv.wait(lock, [this] { return m_stop || !m_tasks.empty(); });
            if (m_tasks.empty()) return;   // stopping, and nothing left
            auto task = std::move(m_tasks.front());
            m_tasks.pop_front();
            m_busy = true;
            lock.unlock();
            try {
                task();
            } catch (...) {
                // Tasks report their own errors; never let one end the thread.
            }
            lock.lock();
            m_busy = false;
            if (m_tasks.empty()) m_idle.notify_all();
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_cv, m_idle;
    std::deque<std::function<void()>> m_tasks;
    bool m_stop = false, m_busy = false;
    std::thread m_thread;   // last: starts after the members it uses exist
};

} // namespace acestudio::ui
