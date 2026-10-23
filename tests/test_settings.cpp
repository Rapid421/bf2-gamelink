#include "test.h"
#include "../src/ui/settings.h"
#include <cstdio>
#include <windows.h>

static std::wstring TempFile(const wchar_t* name)
{
    wchar_t dir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + name;
}

TEST(roundtrip_all_fields)
{
    const std::wstring path = TempFile(L"gl_settings_roundtrip.ini");
    DeleteFileW(path.c_str());
    UiSettings a;
    a.flySpeed = 77.5f;
    a.fov = 90.f;
    a.timeScale = 0.25f;
    a.teamSize1 = 63;
    a.teamSize2 = 64;
    a.openOnStart = true;
    a.camSpots[0] = "base 1 2 3 0 0";
    a.camSpots[7] = "tower -5.5 40 12 90 -10";
    CHECK(SaveSettings(path, a));
    UiSettings b;
    CHECK(LoadSettings(path, &b));
    CHECK(b.flySpeed == 77.5f);
    CHECK(b.fov == 90.f);
    CHECK(b.timeScale == 0.25f);
    CHECK(b.teamSize1 == 63);
    CHECK(b.teamSize2 == 64);
    CHECK(b.openOnStart);
    CHECK(b.camSpots[0] == "base 1 2 3 0 0");
    CHECK(b.camSpots[7] == "tower -5.5 40 12 90 -10");
    CHECK(b.camSpots[3].empty());
    DeleteFileW(path.c_str());
}

TEST(missing_file_gives_defaults)
{
    const std::wstring path = TempFile(L"gl_settings_missing.ini");
    DeleteFileW(path.c_str());
    UiSettings s;
    s.flySpeed = 5.f;                   // must be reset to the default
    CHECK(!LoadSettings(path, &s));
    CHECK(s.flySpeed == 40.f);
    CHECK(s.teamSize1 == 64);
    CHECK(!s.openOnStart);
}

TEST(garbage_file_gives_defaults)
{
    const std::wstring path = TempFile(L"gl_settings_garbage.ini");
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    const char junk[] = "\x00\xFFzzz";
    fwrite(junk, 1, sizeof(junk), f);
    fclose(f);
    UiSettings s;
    CHECK(!LoadSettings(path, &s));
    CHECK(s.flySpeed == 40.f);
    CHECK(s.timeScale == 1.f);
    DeleteFileW(path.c_str());
}

TEST(out_of_range_is_clamped)
{
    const std::wstring path = TempFile(L"gl_settings_range.ini");
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"w");
    fputs("[gamelink]\nflySpeed=99999\nteamSize1=0\nteamSize2=500\nfov=10\ntimeScale=-2\n", f);
    fclose(f);
    UiSettings s;
    CHECK(LoadSettings(path, &s));
    CHECK(s.flySpeed == 500.f);
    CHECK(s.teamSize1 == 1);
    CHECK(s.teamSize2 == 64);
    CHECK(s.fov == 30.f);
    CHECK(s.timeScale == 0.f);
    DeleteFileW(path.c_str());
}

TEST(big_battles_flag_roundtrips_and_defaults_off)
{
    const std::wstring path = TempFile(L"gl_settings_bots.ini");
    DeleteFileW(path.c_str());
    UiSettings fresh;
    CHECK(!fresh.bigBattles);
    UiSettings a;
    a.bigBattles = true;
    CHECK(SaveSettings(path, a));
    UiSettings b;
    CHECK(LoadSettings(path, &b));
    CHECK(b.bigBattles);
}
