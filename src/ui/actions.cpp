#include "actions.h"
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

std::string Fmt(const char* f, double v)
{
    char buf[64];
    sprintf_s(buf, f, v);
    return buf;
}

std::string Narrow(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}
} // namespace

std::vector<std::string> ActFreeCam(bool on, float speed)
{
    if (!on)
        return {"fly 0", "watchdrive 0", "inputoff 0"};
    return {"watchdrive 1", "fly 1 " + Fmt("%.1f", Clamp(speed, 1.f, 150.f)), "inputoff 1"};
}

std::vector<std::string> ActHud(bool show) { return {show ? "hud 1" : "hud 0"}; }

std::vector<std::string> ActFov(float degrees)
{
    if (degrees <= 0.f)
        return {"forcefov -1"};
    return {"forcefov " + Fmt("%.1f", degrees)};
}

std::vector<std::string> ActTimeScale(float scale) { return {"timescale " + Fmt("%.2f", Clamp(scale, 0.f, 3.f))}; }

std::vector<std::string> ActViewMode(int mode) { return {"viewmode " + std::to_string(mode)}; }

std::vector<std::string> ActScreenshot(const std::wstring& dir, const std::function<std::string()>& clock)
{
    return {"grabframe " + Narrow(dir) + "\\gamelink_" + clock() + ".bmp"};
}

bool ParseCamera(const std::string& reply, float pos[3])
{
    return sscanf_s(reply.c_str(), "camera pos %f %f %f", &pos[0], &pos[1], &pos[2]) == 3;
}

std::vector<std::array<float, 3>> CamPath(const float from[3], const float to[3], float maxStep)
{
    std::vector<std::array<float, 3>> path;
    const float d[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len <= 0.f || maxStep <= 0.f)
        return path;
    const int steps = static_cast<int>(std::ceil(len / maxStep));
    for (int i = 1; i < steps; ++i)
    {
        const float t = static_cast<float>(i) / steps;
        path.push_back({from[0] + d[0] * t, from[1] + d[1] * t, from[2] + d[2] * t});
    }
    path.push_back({to[0], to[1], to[2]});
    return path;
}

std::string ActCamStep(const std::array<float, 3>& p)
{
    char buf[96];
    sprintf_s(buf, "camto %.2f %.2f %.2f", p[0], p[1], p[2]);
    return buf;
}

bool ParseFrameTimes(const std::string& reply, FrameStats* out)
{
    FrameStats s;
    float seconds = 0.f;
    const int n = sscanf_s(reply.c_str(),
                           "frametimes over %f s: %d frames, %f fps, avg %f ms, p50 %f, p95 %f, p99 %f, max %f ms; "
                           ">33ms %d, >50ms %d, >100ms %d",
                           &seconds, &s.frames, &s.fps, &s.avg, &s.p50, &s.p95, &s.p99, &s.max, &s.over33, &s.over50,
                           &s.over100);
    if (n != 11)
        return false;
    *out = s;
    return true;
}
