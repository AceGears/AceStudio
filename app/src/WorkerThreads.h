#pragma once
// Detached worker threads (thumbnails, library scans, MCP scripts) hold a
// WorkerThreads::Token while they run, and main() waits for them before the
// application and its statics are torn down. A thread still running at exit
// can touch what is already destroyed (the UI audit ended in an access
// violation after its last command).
//
//   std::thread([token = WorkerThreads::token(), ...] { ... }).detach();

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace acestudio::ui {

class WorkerThreads {
public:
    using Token = std::shared_ptr<void>;

    // Counts a worker until the returned token (and every copy) is gone.
    static Token token() {
        State& s = state();
        {
            std::lock_guard lock(s.m);
            ++s.running;
        }
        return Token(static_cast<void*>(&s), [](void*) {
            State& st = state();
            {
                std::lock_guard lock(st.m);
                --st.running;
            }
            st.cv.notify_all();
        });
    }

    // Waits up to `ms` for every worker to finish; false if some still run.
    static bool waitIdle(int ms) {
        State& s = state();
        std::unique_lock lock(s.m);
        return s.cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return s.running == 0; });
    }

    static int running() {
        State& s = state();
        std::lock_guard lock(s.m);
        return s.running;
    }

private:
    struct State {
        std::mutex m;
        std::condition_variable cv;
        int running = 0;
    };
    // Never destroyed, so a worker finishing during exit still finds it.
    static State& state() {
        static State* s = new State;
        return *s;
    }
};

} // namespace acestudio::ui
