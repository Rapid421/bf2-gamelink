#include "menu_tabs.h"

#include <windows.h>

#include <cstdio>
#include <string>

#include "bots_logic.h"
#include "imgui.h"
#include "ui/command_queue.h"
#include "ui/settings.h"

namespace
{
uint64_t g_statusId = 0, g_actionId = 0;
std::string g_status, g_actionReply;
double g_statusAt = 0.0;
int g_edit1 = -1, g_edit2 = -1;      // slider values not yet applied (-1: show the saved ones)

double Now() { return GetTickCount64() / 1000.0; }

const ImVec4 kAmber(1.0f, 0.76f, 0.28f, 1.0f);
const ImVec4 kGreen(0.36f, 0.84f, 0.48f, 1.0f);
}   // namespace

std::string BotsDo(CommandQueue& q, UiSettings& s, void (*save)(), const std::string& id, const std::string& value)
{
    if (id == "bots.on")
    {
        s.bigBattles = atoi(value.c_str()) != 0;
        save();
        char line[48];
        sprintf_s(line, "bots set %d %d", s.teamSize1, s.teamSize2);
        g_actionId = q.Post(s.bigBattles ? line : "bots off");
        return std::string("bots.on ") + (s.bigBattles ? "1" : "0") + " -> queued";
    }
    if (id == "bots.set")
    {
        int a = 0, b = 0;
        if (!ParseTeamSizes(value, &a, &b))
            return "ui do bots.set <team1> <team2>";
        s.teamSize1 = a;
        s.teamSize2 = b;
        s.bigBattles = true;
        save();
        char line[48];
        sprintf_s(line, "bots set %d %d", a, b);
        g_actionId = q.Post(line);
        return "bots.set " + std::to_string(a) + " " + std::to_string(b) + " -> queued";
    }
    if (id == "bots.last")
    {
        std::string reply;
        return q.Reply(g_actionId, &reply) ? reply : "bots.last: still working";
    }
    return "";
}

void BotsTab(CommandQueue& q, UiSettings& s, void (*save)())
{
    const double now = Now();
    std::string reply;
    if (g_statusId != 0 && q.Reply(g_statusId, &reply))
    {
        g_status = reply;
        g_statusId = 0;
    }
    if (g_statusId == 0 && now - g_statusAt >= 1.0)    // live counts while this tab is drawn
    {
        g_statusId = q.Post("bots status");
        g_statusAt = now;
    }
    if (g_actionId != 0 && q.Reply(g_actionId, &reply))
        g_actionReply = reply;

    ImGui::TextUnformatted("Big bot battles");
    ImGui::TextDisabled("Up to 64 players per team (you count as one). Offline matches only.");
    ImGui::Separator();

    bool on = s.bigBattles;
    if (ImGui::Checkbox("Big battles on", &on))
        BotsDo(q, s, save, "bots.on", on ? "1" : "0");
    ImGui::SameLine();
    ImGui::TextDisabled("remembered: gamelink sets it up again at the next start");

    int t1 = g_edit1 < 0 ? s.teamSize1 : g_edit1, t2 = g_edit2 < 0 ? s.teamSize2 : g_edit2;
    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::SliderInt("Team 1 players", &t1, 1, 64))
        g_edit1 = t1;
    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::SliderInt("Team 2 players", &t2, 1, 64))
        g_edit2 = t2;
    const bool changed = t1 != s.teamSize1 || t2 != s.teamSize2;
    if (ImGui::Button(changed ? "Apply team sizes" : "Apply"))
    {
        char v[16];
        sprintf_s(v, "%d %d", t1, t2);
        BotsDo(q, s, save, "bots.set", v);
        g_edit1 = g_edit2 = -1;
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop"))
        BotsDo(q, s, save, "bots.on", "0");
    if (g_actionId != 0 && q.Busy(g_actionId))
    {
        ImGui::SameLine();
        ImGui::TextDisabled("working... (the first time in a match can take ~30 s)");
    }

    ImGui::Separator();
    BotsReport r;
    if (!ParseBotsStatus(g_status, &r))
    {
        ImGui::TextDisabled("%s", g_status.empty() ? "Reading the match..." : g_status.c_str());
        return;
    }
    ImGui::Text("Team 1: %2d players, %2d alive        Team 2: %2d players, %2d alive", r.players1, r.alive1, r.players2,
                r.alive2);
    ImGui::TextDisabled("This match has room for %u players. Overflow pool %s, %ld bots placed in it, %ld refused.", r.cap,
                        r.poolOn ? "on" : "off", r.overflow, r.refused);
    if (r.on && !r.note.empty())
        ImGui::TextColored(kAmber, "%s", r.note.c_str());
    const int needed = CapFor(s.teamSize1, s.teamSize2);
    if (s.bigBattles && r.cap != 0 && r.cap < static_cast<unsigned>(needed))
    {
        ImGui::TextColored(kAmber, "This match was started with room for only %u players, so the teams stop there.", r.cap);
        ImGui::TextColored(kAmber, "Start a new match (any Instant Action) to get the full %d v %d.", s.teamSize1, s.teamSize2);
    }
    else if (r.on && r.players1 >= s.teamSize1 && r.players2 >= s.teamSize2)
        ImGui::TextColored(kGreen, "Both teams are full.");
    if (!g_actionReply.empty() && ImGui::IsWindowHovered())
        ImGui::SetTooltip("%s", g_actionReply.c_str());
}
