#pragma once
// A stand-in for gamelink's command handler: records every command and answers from a table.
#include <map>
#include <mutex>
#include <string>
#include <vector>

extern std::vector<std::string> g_fakeLog;
extern std::map<std::string, std::string> g_fakeReplies;
extern int g_fakeDelayMs;
extern std::mutex g_fakeMutex;

void FakeReset();
std::string FakeExec(const std::string& line);
