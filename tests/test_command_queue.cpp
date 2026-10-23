#include "test.h"
#include "fake_gamelink.h"
#include "../src/ui/command_queue.h"
#include <chrono>
#include <thread>

static void WaitIdle(const CommandQueue& q, uint64_t id)
{
    for (int i = 0; i < 400 && q.Busy(id); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

TEST(queue_runs_in_order)
{
    FakeReset();
    CommandQueue q(FakeExec);
    q.Post("a");
    q.Post("b");
    const uint64_t last = q.Post("c");
    WaitIdle(q, last);
    std::lock_guard<std::mutex> lock(g_fakeMutex);
    CHECK((g_fakeLog == std::vector<std::string>{"a", "b", "c"}));
}

TEST(queue_does_not_block_caller)
{
    FakeReset();
    g_fakeDelayMs = 500;
    CommandQueue q(FakeExec);
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t id = q.Post("slow");
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 20);
    CHECK(q.Busy(id));
    WaitIdle(q, id);
    std::string reply;
    CHECK(q.Reply(id, &reply));
    CHECK(reply == "ok");
}

TEST(last_reply_by_verb)
{
    FakeReset();
    g_fakeReplies["timescale 0.5"] = "time scaled";
    CommandQueue q(FakeExec);
    const uint64_t id = q.Post("timescale 0.5");
    WaitIdle(q, id);
    CHECK(q.LastReply("timescale") == "time scaled");
}
