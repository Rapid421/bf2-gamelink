#include "menu_tabs.h"

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"
#include "ui/actions.h"
#include "ui/command_queue.h"

namespace
{
struct Poll { const char* line; uint64_t id = 0; std::string reply; };
Poll g_stats{"frametimes 10"}, g_frames{"frametimes raw 1500"}, g_ticks{"ticks"}, g_ui{"ui status"}, g_log{"log 30"},
    g_ping{"ping"};
double g_pollAt = 0.0;

// Tick rate from two "ticks" samples about a second apart.
double g_tickAt = 0.0;
long long g_tickCount = -1;
float g_tickRate = 0.0f;

std::vector<float> g_graph;     // last 10 s of frame times, ms

double Now() { return GetTickCount64() / 1000.0; }

void Collect(CommandQueue& q, Poll& p)
{
    if (p.id != 0 && q.Reply(p.id, &p.reply))
        p.id = 0;
}

void Ask(CommandQueue& q, Poll& p)
{
    if (p.id == 0 || !q.Busy(p.id))
        p.id = q.Post(p.line);
}
}   // namespace

void DiagnosticsTab(CommandQueue& q)
{
    const double now = Now();
    Poll* polls[] = {&g_stats, &g_frames, &g_ticks, &g_ui, &g_log, &g_ping};
    for (Poll* p : polls)
        Collect(q, *p);
    if (now - g_pollAt >= 1.0)                   // only while this tab is drawn
    {
        for (Poll* p : polls)
            Ask(q, *p);
        g_pollAt = now;
    }

    std::vector<float> frames;
    if (ParseFrameList(g_frames.reply, &frames))
    {
        float total = 0.0f;
        size_t first = frames.size();
        while (first > 0 && total + frames[first - 1] <= 10000.0f)
            total += frames[--first];
        g_graph.assign(frames.begin() + first, frames.end());
    }
    long long ticks = -1;
    if (sscanf_s(g_ticks.reply.c_str(), "ticks %lld", &ticks) == 1 && ticks != g_tickCount)
    {
        if (g_tickCount >= 0 && now > g_tickAt)
            g_tickRate = static_cast<float>((ticks - g_tickCount) / (now - g_tickAt));
        g_tickCount = ticks;
        g_tickAt = now;
    }

    FrameStats s;
    const bool haveStats = ParseFrameTimes(g_stats.reply, &s);
    ImGui::TextUnformatted("Frame time, last 10 seconds (ms)");
    char overlay[96] = "";
    if (haveStats)
        sprintf_s(overlay, "%.0f fps   p99 %.1f ms   max %.1f ms", s.fps, s.p99, s.max);
    ImGui::PlotLines("##frames", g_graph.empty() ? nullptr : g_graph.data(), static_cast<int>(g_graph.size()), 0, overlay,
                     0.0f, 50.0f, ImVec2(ImGui::GetContentRegionAvail().x, 110.0f));
    if (!haveStats)
        ImGui::TextDisabled("%s", g_stats.reply.empty() ? "Measuring..." : g_stats.reply.c_str());
    else
    {
        ImGui::Text("FPS %.0f    average %.1f ms    p99 %.1f ms", s.fps, s.avg, s.p99);
        ImGui::SameLine();
        if (s.over33 > 0)
            ImGui::TextColored(ImVec4(1.0f, 0.76f, 0.28f, 1.0f), "  %d frames over 33 ms", s.over33);
        else
            ImGui::TextDisabled("  no frames over 33 ms");
    }
    if (g_tickRate > 0.0f)
        ImGui::Text("Server tick rate %.0f per second", g_tickRate);
    else
        ImGui::TextDisabled("Server tick rate: %s", g_ticks.reply.compare(0, 5, "ticks") == 0 && g_tickCount < 0 ? g_ticks.reply.c_str() : "measuring...");

    ImGui::Separator();
    ImGui::TextUnformatted("gamelink");
    ImGui::TextDisabled("%s", g_ping.reply.empty() ? "..." : g_ping.reply.c_str());
    ImGui::TextDisabled("%s", g_ui.reply.empty() ? "..." : g_ui.reply.c_str());

    ImGui::Separator();
    ImGui::TextUnformatted("Log (last 30 lines)");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 150.0f);
    if (ImGui::Button("Copy diagnostics"))
    {
        std::string text = "gamelink diagnostics\n" + g_ping.reply + "\n" + g_ui.reply + "\n" + g_stats.reply + "\n";
        char line[64];
        sprintf_s(line, "server tick rate %.1f/s\n", g_tickRate);
        text += line;
        text += "---- log ----\n" + g_log.reply + "\n";
        ImGui::SetClipboardText(text.c_str());
    }
    ImGui::BeginChild("log", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted(g_log.reply.c_str());
    ImGui::EndChild();
}
