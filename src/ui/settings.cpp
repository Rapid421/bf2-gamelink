#include "settings.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>

namespace
{
const wchar_t* kSection = L"gamelink";

float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

bool ReadString(const std::wstring& path, const wchar_t* key, std::wstring* out)
{
    wchar_t buf[512] = {};
    const DWORD n = GetPrivateProfileStringW(kSection, key, L"\x1", buf, 512, path.c_str());
    if (n == 1 && buf[0] == L'\x1')
        return false;
    out->assign(buf, n);
    return true;
}

bool ReadFloat(const std::wstring& path, const wchar_t* key, float* out)
{
    std::wstring s;
    if (!ReadString(path, key, &s) || s.empty())
        return false;
    wchar_t* end = nullptr;
    const double v = wcstod(s.c_str(), &end);
    if (end == s.c_str())
        return false;
    *out = static_cast<float>(v);
    return true;
}

bool ReadInt(const std::wstring& path, const wchar_t* key, int* out)
{
    float f = 0.f;
    if (!ReadFloat(path, key, &f))
        return false;
    *out = static_cast<int>(f);
    return true;
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

std::wstring Widen(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

bool Write(const std::wstring& path, const wchar_t* key, const std::wstring& value)
{
    return WritePrivateProfileStringW(kSection, key, value.c_str(), path.c_str()) != 0;
}

std::wstring FloatText(float v)
{
    wchar_t buf[64];
    swprintf_s(buf, L"%.6g", v);
    return buf;
}
} // namespace

bool LoadSettings(const std::wstring& path, UiSettings* out)
{
    *out = UiSettings{};
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;
    bool any = false;
    float f = 0.f;
    int i = 0;
    if (ReadFloat(path, L"flySpeed", &f)) { out->flySpeed = Clamp(f, 1.f, 500.f); any = true; }
    if (ReadFloat(path, L"fov", &f)) { out->fov = f <= 0.f ? 0.f : Clamp(f, 30.f, 120.f); any = true; }
    if (ReadFloat(path, L"timeScale", &f)) { out->timeScale = Clamp(f, 0.f, 3.f); any = true; }
    if (ReadInt(path, L"teamSize1", &i)) { out->teamSize1 = Clamp(i, 1, 64); any = true; }
    if (ReadInt(path, L"teamSize2", &i)) { out->teamSize2 = Clamp(i, 1, 64); any = true; }
    if (ReadInt(path, L"openOnStart", &i)) { out->openOnStart = i != 0; any = true; }
    if (ReadInt(path, L"bigBattles", &i)) { out->bigBattles = i != 0; any = true; }
    for (int k = 0; k < 8; ++k)
    {
        wchar_t key[16];
        swprintf_s(key, L"camSpot%d", k);
        std::wstring s;
        if (ReadString(path, key, &s)) { out->camSpots[k] = Narrow(s); any = true; }
    }
    return any;
}

bool SaveSettings(const std::wstring& path, const UiSettings& s)
{
    bool ok = Write(path, L"flySpeed", FloatText(s.flySpeed));
    ok &= Write(path, L"fov", FloatText(s.fov));
    ok &= Write(path, L"timeScale", FloatText(s.timeScale));
    ok &= Write(path, L"teamSize1", std::to_wstring(s.teamSize1));
    ok &= Write(path, L"teamSize2", std::to_wstring(s.teamSize2));
    ok &= Write(path, L"openOnStart", s.openOnStart ? L"1" : L"0");
    ok &= Write(path, L"bigBattles", s.bigBattles ? L"1" : L"0");
    for (int k = 0; k < 8; ++k)
    {
        wchar_t key[16];
        swprintf_s(key, L"camSpot%d", k);
        ok &= Write(path, key, Widen(s.camSpots[k]));
    }
    return ok;
}

std::wstring SettingsPath()
{
    wchar_t local[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) == 0)
        return L"gamelink_settings.ini";
    std::wstring dir = std::wstring(local) + L"\\gamelink";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.ini";
}
