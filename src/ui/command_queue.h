#pragma once
// CommandQueue - runs gamelink text commands on one background thread, in order, so a menu action never stalls
// the game's frame (the menu draws on the present thread). Keeps the last 256 replies for the UI to show.
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class CommandQueue
{
public:
    explicit CommandQueue(std::function<std::string(const std::string&)> exec);
    ~CommandQueue();
    CommandQueue(const CommandQueue&) = delete;
    CommandQueue& operator=(const CommandQueue&) = delete;

    // Queue a command; returns its id at once.
    uint64_t Post(const std::string& line);
    // True while the command is queued or running.
    bool Busy(uint64_t id) const;
    // The reply of a finished command (false while busy, or once it has dropped out of the last 256).
    bool Reply(uint64_t id, std::string* out) const;
    // The newest finished reply to a command starting with this verb ("" if none).
    std::string LastReply(const std::string& verb) const;

private:
    struct Done { uint64_t id; std::string line; std::string reply; };
    void Run();

    std::function<std::string(const std::string&)> m_exec;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<std::pair<uint64_t, std::string>> m_pending;
    std::deque<Done> m_done;
    uint64_t m_next = 1;
    uint64_t m_running = 0;
    bool m_stop = false;
    std::thread m_thread;
};
