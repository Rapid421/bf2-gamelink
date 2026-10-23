#pragma once
// What each menu control sends to gamelink (exact command lines, in order), and parsers for the replies the menu
// reads back. Pure functions - unit-tested in tests/test_actions.cpp.
#include <array>
#include <functional>
#include <string>
#include <vector>

std::vector<std::string> ActFreeCam(bool on, float speed);      // speed clamped 1..150 m/s
std::vector<std::string> ActHud(bool show);
std::vector<std::string> ActFov(float degrees);                 // 0 = hand back to the game
std::vector<std::string> ActTimeScale(float scale);             // clamped 0..3
std::vector<std::string> ActViewMode(int mode);
std::vector<std::string> ActScreenshot(const std::wstring& dir, const std::function<std::string()>& clock);

bool ParseCamera(const std::string& reply, float pos[3]);
// Waypoints from 'from' to 'to', each at most maxStep metres from the previous one (camera jumps crash the
// renderer); the last one is exactly 'to'. Empty when already there.
std::vector<std::array<float, 3>> CamPath(const float from[3], const float to[3], float maxStep);
std::string ActCamStep(const std::array<float, 3>& p);

struct FrameStats
{
    int frames = 0;
    float fps = 0, avg = 0, p50 = 0, p95 = 0, p99 = 0, max = 0;
    int over33 = 0, over50 = 0, over100 = 0;
};
bool ParseFrameTimes(const std::string& reply, FrameStats* out);
// "frames 8.10 16.67 ..." from "frametimes raw <n>": per-frame milliseconds, oldest first.
bool ParseFrameList(const std::string& reply, std::vector<float>* out);
