#include "command_queue.h"

CommandQueue::CommandQueue(std::function<std::string(const std::string&)> exec)
    : m_exec(std::move(exec)), m_thread([this] { Run(); })
{
}

CommandQueue::~CommandQueue()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

uint64_t CommandQueue::Post(const std::string& line)
{
    uint64_t id;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        id = m_next++;
        m_pending.emplace_back(id, line);
    }
    m_wake.notify_one();
    return id;
}

bool CommandQueue::Busy(uint64_t id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_running == id)
        return true;
    for (const auto& p : m_pending)
        if (p.first == id)
            return true;
    return false;
}

bool CommandQueue::Reply(uint64_t id, std::string* out) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const Done& d : m_done)
        if (d.id == id)
        {
            *out = d.reply;
            return true;
        }
    return false;
}

std::string CommandQueue::LastReply(const std::string& verb) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_done.rbegin(); it != m_done.rend(); ++it)
    {
        const std::string& l = it->line;
        if (l.compare(0, verb.size(), verb) == 0 && (l.size() == verb.size() || l[verb.size()] == ' '))
            return it->reply;
    }
    return "";
}

void CommandQueue::Run()
{
    for (;;)
    {
        std::pair<uint64_t, std::string> job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stop || !m_pending.empty(); });
            if (m_stop)
                return;
            job = std::move(m_pending.front());
            m_pending.pop_front();
            m_running = job.first;
        }
        std::string reply;
        try
        {
            reply = m_exec(job.second);
        }
        catch (...)
        {
            reply = "error: command threw";
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_done.push_back({job.first, job.second, std::move(reply)});
            if (m_done.size() > 256)
                m_done.pop_front();
            m_running = 0;
        }
    }
}
