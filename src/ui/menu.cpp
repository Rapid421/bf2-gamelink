#include "menu.h"

#include <windows.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "gamelink_api.h"
#include "imgui.h"
#include "imgui_internal.h"     // PushItemFlag: 1.78 has no BeginDisabled
#include "ui/actions.h"
#include "ui/command_queue.h"
#include "ui/menu_state.h"
#include "ui/menu_tabs.h"
#include "ui/settings.h"

namespace
{
// Menu_Frame (present thread) and Menu_Do (any thread) share everything below.
std::recursive_mutex g_mu;
std::unique_ptr<CommandQueue> g_queue;     // quick commands: camera, level checks, screenshots
std::unique_ptr<CommandQueue> g_slow;      // engine settings: the first write of each scans memory (~30 s)
UiSettings g_set;
bool g_coreReady = false;
bool g_imguiReady = false;
float g_styledHeight = 0.0f;
std::string g_iniPath;

// What the controls currently show (the game's real state is only what the replies say).
bool g_freeCam = false;
bool g_hideHud = false;
bool g_photo = false;
int g_viewMode = 0;
struct Sent { CommandQueue* queue; uint64_t id; };
std::map<std::string, Sent> g_last;         // control id -> its newest command

// Level detection, refreshed every 2 s while the menu is open.
enum class Level { Unknown, None, Loaded };
Level g_level = Level::Unknown;
std::string g_levelName, g_modeName;
uint64_t g_levelId = 0, g_modeId = 0;
double g_levelAt = 0.0;

// Saved spots: "Save here" and "Go" first ask where the camera is.
struct CamAsk { uint64_t id; int slot; bool go; };
std::vector<CamAsk> g_camAsks;
std::deque<std::array<float, 3>> g_path;    // camera travel, one <= 8 m step every 60 ms
double g_stepAt = 0.0;
char g_spotNames[8][32] = {};

// Screenshot: overlays hidden for two frames, then grabframe; hidden until it answers.
int g_shotFrames = 0;
uint64_t g_shotId = 0;
std::string g_shotReply;

double Now() { return GetTickCount64() / 1000.0; }

void EnsureCore()
{
    if (g_coreReady)
        return;
    g_queue.reset(new CommandQueue([](const std::string& line) { return gl::Command(line); }));
    g_slow.reset(new CommandQueue([](const std::string& line) { return gl::Command(line); }));
    LoadSettings(SettingsPath(), &g_set);
    for (int i = 0; i < 8; ++i)
    {
        char name[32] = {};
        if (sscanf_s(g_set.camSpots[i].c_str(), "%31s", name, static_cast<unsigned>(sizeof(name))) == 1)
            for (char* c = name; *c; ++c)
                if (*c == '_') *c = ' ';
        strcpy_s(g_spotNames[i], name[0] ? name : "");
        if (!g_spotNames[i][0])
            sprintf_s(g_spotNames[i], "Spot %d", i + 1);
    }
    g_coreReady = true;
}

void Save() { SaveSettings(SettingsPath(), g_set); }

// slow: an engine-settings write, which goes on its own lane so the camera never waits behind its first scan.
void Send(const char* key, const std::vector<std::string>& lines, bool slow = false)
{
    CommandQueue* q = slow ? g_slow.get() : g_queue.get();
    uint64_t id = 0;
    for (const std::string& l : lines)
        id = q->Post(l);
    if (id != 0)
        g_last[key] = {q, id};
}

// After a control: "working..." while its command runs, its reply as a tooltip afterwards.
void Status(const char* key)
{
    const auto it = g_last.find(key);
    if (it == g_last.end())
        return;
    std::string reply;
    if (it->second.queue->Busy(it->second.id))
    {
        ImGui::SameLine();
        ImGui::TextDisabled(it->second.queue == g_slow.get() ? "working... (the first change can take ~30 s)" : "working...");
    }
    else if (ImGui::IsItemHovered() && it->second.queue->Reply(it->second.id, &reply))
        ImGui::SetTooltip("%s", reply.c_str());
}

void BeginDisabledIf(bool disabled)
{
    if (!disabled)
        return;
    ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.45f);
}

void EndDisabledIf(bool disabled)
{
    if (!disabled)
        return;
    ImGui::PopStyleVar();
    ImGui::PopItemFlag();
}

std::string Clock()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[32];
    sprintf_s(b, "%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

std::wstring PicturesDir()
{
    wchar_t profile[MAX_PATH] = {};
    GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    std::wstring dir = std::wstring(profile) + L"\\Pictures";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\gamelink";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

// ---- the handlers every widget (and "ui do") runs ------------------------------------------------------------

void DoFreeCam(bool on)
{
    g_freeCam = on;
    Send("camera.freecam", ActFreeCam(on, g_set.flySpeed));
}

void DoSpeed(float v)
{
    g_set.flySpeed = v < 1.0f ? 1.0f : (v > 150.0f ? 150.0f : v);
    Save();
    if (g_freeCam)
        Send("camera.speed", {ActFreeCam(true, g_set.flySpeed)[1]});   // just "fly 1 <speed>"
}

void DoFov(float v)
{
    g_set.fov = v;
    Save();
    Send("camera.fov", ActFov(v), true);
}

void DoHud(bool hide)
{
    g_hideHud = hide;
    Send("camera.hud", ActHud(!hide), true);
}

void DoTimeScale(float v)
{
    g_set.timeScale = v < 0.0f ? 0.0f : (v > 3.0f ? 3.0f : v);
    Save();
    Send("world.timescale", ActTimeScale(g_set.timeScale), true);
}

void DoPhoto(bool on)
{
    g_photo = on;
    if (on)
    {
        DoFreeCam(true);
        DoHud(true);
        Send("camera.photo", ActTimeScale(0.0f), true);
    }
    else
    {
        DoFreeCam(false);
        DoHud(false);
        Send("camera.photo", ActTimeScale(g_set.timeScale), true);
    }
}

void DoViewMode(int mode)
{
    g_viewMode = mode;
    Send("world.viewmode", ActViewMode(mode), true);
}

void DoScreenshot()
{
    g_shotFrames = 2;
    g_shotReply.clear();
}

void DoSpot(int slot, bool go)
{
    if (slot < 0 || slot >= 8)
        return;
    if (go && g_set.camSpots[slot].empty())
        return;
    if (go && !g_freeCam)
        DoFreeCam(true);                         // camto moves the driven camera only
    g_camAsks.push_back({g_queue->Post("camera"), slot, go});
    g_last[go ? "camera.go" : "camera.save"] = {g_queue.get(), g_camAsks.back().id};
}

// ---- work that carries on whether or not the window is drawn -------------------------------------------------

void Tick(bool open)
{
    const double now = Now();
    std::string reply;

    if (open && now - g_levelAt > 2.0 && !g_queue->Busy(g_levelId) && !g_queue->Busy(g_modeId))
    {
        g_levelId = g_queue->Post("levelname");
        g_modeId = g_queue->Post("hookstatus");
        g_levelAt = now;
    }
    if (g_levelId != 0 && g_queue->Reply(g_levelId, &reply))
    {
        const char* prefix = "loaded level: ";
        if (reply.compare(0, strlen(prefix), prefix) == 0)
        {
            g_level = Level::Loaded;
            g_levelName = reply.substr(strlen(prefix));
        }
        else
            g_level = Level::None;
    }
    if (g_modeId != 0 && g_queue->Reply(g_modeId, &reply))
    {
        const size_t at = reply.find("mode='");
        const size_t end = at == std::string::npos ? at : reply.find('\'', at + 6);
        g_modeName = end == std::string::npos ? "" : reply.substr(at + 6, end - at - 6);
    }

    for (size_t i = 0; i < g_camAsks.size();)
    {
        if (!g_queue->Reply(g_camAsks[i].id, &reply))
        {
            if (g_queue->Busy(g_camAsks[i].id)) { ++i; continue; }
            g_camAsks.erase(g_camAsks.begin() + i);  // dropped out of the reply history
            continue;
        }
        float here[3];
        if (ParseCamera(reply, here))
        {
            const int slot = g_camAsks[i].slot;
            if (g_camAsks[i].go)
            {
                char name[32];
                float to[3];
                if (sscanf_s(g_set.camSpots[slot].c_str(), "%31s %f %f %f", name, static_cast<unsigned>(sizeof(name)),
                             &to[0], &to[1], &to[2]) == 4)
                {
                    g_path.clear();
                    for (const auto& p : CamPath(here, to, 8.0f))
                        g_path.push_back(p);
                }
            }
            else
            {
                std::string name = g_spotNames[slot];
                for (char& c : name)
                    if (c == ' ') c = '_';
                char line[96];
                sprintf_s(line, "%s %.2f %.2f %.2f", name.empty() ? "Spot" : name.c_str(), here[0], here[1], here[2]);
                g_set.camSpots[slot] = line;
                Save();
            }
        }
        g_camAsks.erase(g_camAsks.begin() + i);
    }

    if (!g_path.empty() && now >= g_stepAt)
    {
        g_last["camera.go"] = {g_queue.get(), g_queue->Post(ActCamStep(g_path.front()))};
        g_path.pop_front();
        g_stepAt = now + 0.06;
    }

    if (g_shotFrames > 0 && --g_shotFrames == 0)
        g_shotId = g_queue->Post(ActScreenshot(PicturesDir(), Clock)[0]);
    if (g_shotId != 0 && g_queue->Reply(g_shotId, &reply))
    {
        g_shotReply = reply;
        g_shotId = 0;
    }
}

// ---- tabs ----------------------------------------------------------------------------------------------------

bool NotInLevel()
{
    if (g_level == Level::Loaded)
        return false;
    ImGui::TextColored(ImVec4(1.0f, 0.76f, 0.28f, 1.0f), g_level == Level::Unknown ? "Checking the game..." : "Not in a level yet");
    ImGui::TextDisabled("These controls work once a match is loaded.");
    ImGui::Separator();
    return true;
}

void CameraTab()
{
    const bool off = NotInLevel();
    BeginDisabledIf(off);

    bool freeCam = g_freeCam;
    if (ImGui::Checkbox("Free camera", &freeCam))
        DoFreeCam(freeCam);
    Status("camera.freecam");
    ImGui::SameLine(260.0f);
    ImGui::TextDisabled("WASD + mouse to fly, Shift = faster");

    float speed = g_set.flySpeed > 150.0f ? 150.0f : g_set.flySpeed;
    ImGui::SetNextItemWidth(260.0f);
    ImGui::SliderFloat("Speed (m/s)", &speed, 1.0f, 150.0f, "%.0f");
    if (ImGui::IsItemDeactivatedAfterEdit())
        DoSpeed(speed);
    else
        g_set.flySpeed = speed;
    Status("camera.speed");

    float fov = g_set.fov <= 0.0f ? 70.0f : g_set.fov;
    ImGui::SetNextItemWidth(260.0f);
    ImGui::SliderFloat("Field of view", &fov, 30.0f, 120.0f, g_set.fov <= 0.0f ? "game default" : "%.0f deg");
    if (ImGui::IsItemDeactivatedAfterEdit())
        DoFov(fov);
    Status("camera.fov");
    ImGui::SameLine();
    if (ImGui::Button("Default"))
        DoFov(0.0f);

    bool hideHud = g_hideHud;
    if (ImGui::Checkbox("Hide HUD", &hideHud))
        DoHud(hideHud);
    Status("camera.hud");

    ImGui::Separator();
    bool photo = g_photo;
    if (ImGui::Checkbox("Photo mode", &photo))
        DoPhoto(photo);
    Status("camera.photo");
    ImGui::SameLine(260.0f);
    ImGui::TextDisabled("free camera, no HUD, time stopped");
    if (ImGui::Button("Screenshot"))
        DoScreenshot();
    ImGui::SameLine();
    if (g_shotFrames > 0 || g_shotId != 0)
        ImGui::TextDisabled("taking...");
    else if (!g_shotReply.empty())
        ImGui::TextWrapped("%s", g_shotReply.c_str());
    else
        ImGui::TextDisabled("saved to Pictures\\gamelink");

    ImGui::Separator();
    ImGui::TextUnformatted("Camera spots");
    ImGui::TextDisabled("Save where the camera is now; Go flies back there in short steps.");
    for (int i = 0; i < 8; ++i)
    {
        ImGui::PushID(i);
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::InputText("##name", g_spotNames[i], sizeof(g_spotNames[i])) && !g_set.camSpots[i].empty())
        {
            std::string name = g_spotNames[i];
            for (char& c : name)
                if (c == ' ') c = '_';
            float p[3];
            char old[32];
            if (sscanf_s(g_set.camSpots[i].c_str(), "%31s %f %f %f", old, static_cast<unsigned>(sizeof(old)), &p[0], &p[1], &p[2]) == 4)
            {
                char line[96];
                sprintf_s(line, "%s %.2f %.2f %.2f", name.empty() ? "Spot" : name.c_str(), p[0], p[1], p[2]);
                g_set.camSpots[i] = line;
            }
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            Save();
        ImGui::SameLine();
        if (ImGui::Button("Save here"))
            DoSpot(i, false);
        ImGui::SameLine();
        const bool empty = g_set.camSpots[i].empty();
        BeginDisabledIf(empty);
        if (ImGui::Button("Go"))
            DoSpot(i, true);
        EndDisabledIf(empty);
        ImGui::SameLine();
        if (empty)
            ImGui::TextDisabled("empty");
        else
        {
            float p[3];
            char n[32];
            if (sscanf_s(g_set.camSpots[i].c_str(), "%31s %f %f %f", n, static_cast<unsigned>(sizeof(n)), &p[0], &p[1], &p[2]) == 4)
                ImGui::TextDisabled("%.0f, %.0f, %.0f", p[0], p[1], p[2]);
        }
        ImGui::PopID();
    }
    if (!g_path.empty())
        ImGui::TextDisabled("Flying to the spot... %d steps left", static_cast<int>(g_path.size()));

    EndDisabledIf(off);
}

void WorldTab()
{
    const bool off = NotInLevel();
    if (!off)
    {
        const size_t slash = g_levelName.find_last_of('/');
        ImGui::Text("Level: %s", slash == std::string::npos ? g_levelName.c_str() : g_levelName.c_str() + slash + 1);
        ImGui::TextDisabled("%s", g_levelName.c_str());
        ImGui::Text("Mode: %s", g_modeName.empty() ? "-" : g_modeName.c_str());
        ImGui::Separator();
    }
    BeginDisabledIf(off);

    float scale = g_set.timeScale;
    ImGui::SetNextItemWidth(260.0f);
    ImGui::SliderFloat("Time scale", &scale, 0.0f, 3.0f, "%.2fx");
    if (ImGui::IsItemDeactivatedAfterEdit())
        DoTimeScale(scale);
    else
        g_set.timeScale = scale;
    Status("world.timescale");
    if (ImGui::Button("Pause"))
        DoTimeScale(0.0f);
    ImGui::SameLine();
    if (ImGui::Button("Slow motion"))
        DoTimeScale(0.25f);
    ImGui::SameLine();
    if (ImGui::Button("Normal"))
        DoTimeScale(1.0f);

    ImGui::Separator();
    // The ViewMode values of WorldRenderSettings are not documented in gamelink; listed by number.
    char preview[16];
    sprintf_s(preview, "Mode %d", g_viewMode);
    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::BeginCombo("View mode", preview))
    {
        for (int m = 0; m <= 9; ++m)
        {
            char label[16];
            sprintf_s(label, m == 0 ? "Mode %d (normal)" : "Mode %d", m);
            if (ImGui::Selectable(label, m == g_viewMode))
                DoViewMode(m);
        }
        ImGui::EndCombo();
    }
    Status("world.viewmode");

    EndDisabledIf(off);
}

void ComingSoon(const char* what)
{
    ImGui::TextDisabled("%s", what);
    ImGui::TextDisabled("Coming in a later update.");
}
}   // namespace

void Menu_Frame(MenuState& state)
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    EnsureCore();
    ImGuiIO& io = ImGui::GetIO();
    if (!g_imguiReady)
    {
        wchar_t local[MAX_PATH] = {};
        GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
        char narrow[MAX_PATH * 2] = {};
        WideCharToMultiByte(CP_ACP, 0, local, -1, narrow, sizeof(narrow), nullptr, nullptr);
        g_iniPath = std::string(narrow) + "\\gamelink\\ui.ini";
        io.IniFilename = g_iniPath.c_str();      // window size and place, kept beside settings.ini
        ImGui::LoadIniSettingsFromDisk(g_iniPath.c_str());
        g_imguiReady = true;
    }
    if (io.DisplaySize.y > 0.0f && io.DisplaySize.y != g_styledHeight)
    {
        Style_Apply(io.DisplaySize.y);
        g_styledHeight = io.DisplaySize.y;
    }

    const bool open = state.IsOpen();
    Tick(open);
    if (!open)
        return;
    if (Menu_HideOverlays())
    {
        io.MouseDrawCursor = false;
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(720.0f, 520.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (ImGui::Begin("gamelink", nullptr, ImGuiWindowFlags_NoCollapse))
    {
        static TabPicker picker;
        const int wanted = static_cast<int>(state.Tab());
        const int force = picker.Force(wanted);
        if (ImGui::BeginTabBar("tabs"))
        {
            for (int i = 0; i < static_cast<int>(MenuTab::Count); ++i)
            {
                const ImGuiTabItemFlags flags = i == force ? ImGuiTabItemFlags_SetSelected : 0;
                if (!ImGui::BeginTabItem(MenuTabName(static_cast<MenuTab>(i)), nullptr, flags))
                    continue;
                if (picker.UserPicked(i, wanted))
                    state.SetTab(static_cast<MenuTab>(i));    // the user clicked this tab
                switch (static_cast<MenuTab>(i))
                {
                case MenuTab::Camera: CameraTab(); break;
                case MenuTab::World: WorldTab(); break;
                case MenuTab::Inspector: ComingSoon("Inspector: pick anything in the world and see what it is."); break;
                case MenuTab::Logic: ComingSoon("Logic: watch an object's logic fire, step by step."); break;
                case MenuTab::Bots: ComingSoon("Bots: team sizes up to 64 v 64."); break;
                case MenuTab::Diagnostics: DiagnosticsTab(*g_queue); break;
                case MenuTab::Advanced: AdvancedTab(*g_queue); break;
                default: break;
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        picker.End(static_cast<int>(state.Tab()));
        ImGui::Separator();
        ImGui::TextDisabled("F1 closes this menu - the game gets its controls back.");
    }
    ImGui::End();
}

void Menu_Shutdown()
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    g_queue.reset();
    g_slow.reset();
    g_coreReady = false;
}

std::string Menu_Do(const std::string& id, const std::string& value)
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    EnsureCore();
    const float v = static_cast<float>(atof(value.c_str()));
    const bool on = v != 0.0f;
    if (id == "camera.freecam") DoFreeCam(on);
    else if (id == "camera.speed") DoSpeed(v);
    else if (id == "camera.fov") DoFov(v);
    else if (id == "camera.hud") DoHud(!on);              // value = show
    else if (id == "camera.photo") DoPhoto(on);
    else if (id == "camera.screenshot") DoScreenshot();
    else if (id == "camera.save") DoSpot(static_cast<int>(v) - 1, false);
    else if (id == "camera.go") DoSpot(static_cast<int>(v) - 1, true);
    else if (id == "world.timescale") DoTimeScale(v);
    else if (id == "world.viewmode") DoViewMode(static_cast<int>(v));
    else if (id == "diag")                                  // ui do diag: the Diagnostics tab's numbers
        return DiagnosticsSummary();
    else if (id == "console")                               // ui do console <command>: Enter in the Advanced tab
    {
        AdvancedSubmit(*g_queue, value);
        return "ui do console " + value + " -> queued";
    }
    else if (id == "console-last")
        return AdvancedLastReply(*g_queue);
    else if (id == "queue")                                 // ui do queue: what the menu's command thread is doing
        return g_queue->Describe() + " | slow lane " + g_slow->Describe();
    else if (id == "last")                                  // ui do last <control>: its newest reply
    {
        const auto it = g_last.find(value);
        std::string reply;
        if (it == g_last.end()) return "ui do last: nothing sent for " + value;
        if (!it->second.queue->Reply(it->second.id, &reply)) return "ui do last: " + value + " still working";
        return reply;
    }
    else
        return "ui do: unknown control (camera.freecam|speed|fov|hud|photo|screenshot|save|go, world.timescale|viewmode)";
    return "ui do " + id + " " + value + " -> queued";
}

bool Menu_HideOverlays()
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    return g_shotFrames > 0 || g_shotId != 0;
}
