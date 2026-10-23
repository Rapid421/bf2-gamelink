#pragma once
// UI options remembered between sessions (%LOCALAPPDATA%\gamelink\settings.ini). Missing or corrupt files give
// the defaults; out-of-range values are clamped on load.
#include <string>

struct UiSettings
{
    float flySpeed = 40.f;       // free-camera speed, m/s (1..500)
    float fov = 0.f;             // 0 = game default, else 30..120 degrees
    float timeScale = 1.f;       // 0..3
    int teamSize1 = 64;          // 1..64, humans included
    int teamSize2 = 64;
    bool openOnStart = false;
    bool bigBattles = false;     // Bots tab on: gamelink fixes the player cap at startup and holds the team sizes
    std::string camSpots[8];     // "name x y z yaw pitch", or empty
};

bool LoadSettings(const std::wstring& path, UiSettings* out);
bool SaveSettings(const std::wstring& path, const UiSettings& s);
std::wstring SettingsPath();     // %LOCALAPPDATA%\gamelink\settings.ini (creates the folder)
