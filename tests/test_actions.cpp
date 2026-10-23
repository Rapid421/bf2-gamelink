#include "test.h"
#include "../src/ui/actions.h"
#include <cmath>

using V = std::vector<std::string>;

TEST(freecam_on_and_off)
{
    CHECK((ActFreeCam(true, 40.f) == V{"watchdrive 1", "fly 1 40.0", "inputoff 1"}));
    CHECK((ActFreeCam(false, 40.f) == V{"fly 0", "watchdrive 0", "inputoff 0"}));
}

TEST(freecam_speed_is_clamped)
{
    CHECK((ActFreeCam(true, 9999.f) == V{"watchdrive 1", "fly 1 150.0", "inputoff 1"}));
    CHECK((ActFreeCam(true, 0.f) == V{"watchdrive 1", "fly 1 1.0", "inputoff 1"}));
}

TEST(hud_fov_timescale_viewmode)
{
    CHECK((ActHud(true) == V{"hud 1"}));
    CHECK((ActHud(false) == V{"hud 0"}));
    CHECK((ActFov(90.f) == V{"forcefov 90.0"}));
    CHECK((ActFov(0.f) == V{"forcefov -1"}));
    CHECK((ActTimeScale(0.5f) == V{"timescale 0.50"}));
    CHECK((ActTimeScale(9.f) == V{"timescale 3.00"}));
    CHECK((ActTimeScale(-1.f) == V{"timescale 0.00"}));
    CHECK((ActViewMode(3) == V{"viewmode 3"}));
}

TEST(screenshot_name_uses_clock)
{
    const V v = ActScreenshot(L"C:\\Pics\\gamelink", [] { return std::string("20261023_174500"); });
    CHECK((v == V{"grabframe C:\\Pics\\gamelink\\gamelink_20261023_174500.bmp"}));
}

TEST(parse_camera_reply)
{
    float pos[3] = {};
    CHECK(ParseCamera("camera pos 12.500000 -3.000000 400.250000 left 1 0 0 up 0 1 0 forward 0 0 1 fovx 1.2000000", pos));
    CHECK(pos[0] == 12.5f && pos[1] == -3.f && pos[2] == 400.25f);
    CHECK(!ParseCamera("camera unavailable", pos));
}

TEST(parse_camera_real_reply)
{
    // Captured in game (Felucia_01, 2026-10-23 run task5_r1).
    float pos[3] = {};
    CHECK(ParseCamera("camera pos -40.651432 274.277954 73.604256 left -0.933455 0.000000 0.358693 up -0.053626 "
                      "0.988761 -0.139555 forward -0.354662 -0.149503 -0.922965 fovx 1.4014995", pos));
    CHECK(pos[0] == -40.651432f && pos[1] == 274.277954f && pos[2] == 73.604256f);
}

TEST(campath_steps_are_short_and_end_on_target)
{
    const float from[3] = {0.f, 0.f, 0.f};
    const float to[3] = {300.f, 40.f, -100.f};
    const auto path = CamPath(from, to, 8.f);
    CHECK(!path.empty());
    float prev[3] = {0.f, 0.f, 0.f};
    bool short_steps = true;
    for (const auto& p : path)
    {
        const float d = std::sqrt((p[0] - prev[0]) * (p[0] - prev[0]) + (p[1] - prev[1]) * (p[1] - prev[1]) +
                                  (p[2] - prev[2]) * (p[2] - prev[2]));
        if (d > 8.001f) short_steps = false;
        prev[0] = p[0]; prev[1] = p[1]; prev[2] = p[2];
    }
    CHECK(short_steps);
    CHECK(path.back()[0] == 300.f && path.back()[1] == 40.f && path.back()[2] == -100.f);
}

TEST(campath_zero_distance_is_empty)
{
    const float a[3] = {5.f, 5.f, 5.f};
    CHECK(CamPath(a, a, 8.f).empty());
}

TEST(camgoto_step_command)
{
    const std::array<float, 3> p = {1.5f, -2.f, 300.f};
    CHECK(ActCamStep(p) == "camto 1.50 -2.00 300.00");
}

TEST(parse_frametimes_reply)
{
    FrameStats s;
    CHECK(ParseFrameTimes("frametimes over 12.0 s: 1421 frames, 118.4 fps, avg 8.44 ms, p50 8.19, p95 10.72, p99 12.82, "
                          "max 31.86 ms; >33ms 0, >50ms 0, >100ms 0", &s));
    CHECK(s.frames == 1421);
    CHECK(s.fps == 118.4f);
    CHECK(s.p99 == 12.82f);
    CHECK(s.max == 31.86f);
    CHECK(s.over33 == 0 && s.over50 == 0 && s.over100 == 0);
    CHECK(!ParseFrameTimes("frametimes: no frames yet", &s));
}

TEST(parse_frame_list_reply)
{
    std::vector<float> ms;
    CHECK(ParseFrameList("frames 8.10 16.67 33.40", &ms));
    CHECK(ms.size() == 3);
    CHECK(ms.size() == 3 && ms[0] == 8.10f && ms[2] == 33.40f);
    CHECK(ParseFrameList("frames", &ms) && ms.empty());
    CHECK(!ParseFrameList("frametimes: Present hook not installed", &ms));
}
