#include "fake_gamelink.h"
#include <chrono>
#include <thread>

std::vector<std::string> g_fakeLog;
std::map<std::string, std::string> g_fakeReplies;
int g_fakeDelayMs = 0;
std::mutex g_fakeMutex;

void FakeReset()
{
    std::lock_guard<std::mutex> lock(g_fakeMutex);
    g_fakeLog.clear();
    g_fakeReplies.clear();
    g_fakeDelayMs = 0;
}

std::string FakeExec(const std::string& line)
{
    int delay = 0;
    {
        std::lock_guard<std::mutex> lock(g_fakeMutex);
        g_fakeLog.push_back(line);
        delay = g_fakeDelayMs;
    }
    if (delay > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    std::lock_guard<std::mutex> lock(g_fakeMutex);
    auto it = g_fakeReplies.find(line);
    return it != g_fakeReplies.end() ? it->second : "ok";
}
