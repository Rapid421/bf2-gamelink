#include "menu_tabs.h"

#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "imgui.h"
#include "ui/command_queue.h"
#include "ui/history.h"

namespace
{
struct Entry { std::string line; uint64_t id; };
std::deque<Entry> g_entries;            // newest last, at most 50
CommandHistory g_history;
char g_input[512] = {};
bool g_focusInput = true;

uint64_t g_helpId = 0;
std::vector<std::pair<std::string, std::string>> g_commands;   // verb, description
char g_filter[64] = {};

int OnHistory(ImGuiInputTextCallbackData* data)
{
    if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory)
        return 0;
    const std::string line = data->EventKey == ImGuiKey_UpArrow ? g_history.Up() : g_history.Down();
    data->DeleteChars(0, data->BufTextLen);
    data->InsertChars(0, line.c_str());
    return 0;
}

void LoadCommands(CommandQueue& q)
{
    std::string reply;
    if (g_helpId == 0)
        g_helpId = q.Post("help");
    if (!g_commands.empty() || !q.Reply(g_helpId, &reply))
        return;
    size_t at = 0;
    while (at < reply.size())
    {
        size_t end = reply.find('\n', at);
        if (end == std::string::npos) end = reply.size();
        const std::string line = reply.substr(at, end - at);
        const size_t space = line.find(' ');
        if (!line.empty())
            g_commands.push_back({space == std::string::npos ? line : line.substr(0, space), line});
        at = end + 1;
    }
}
}   // namespace

void AdvancedSubmit(CommandQueue& q, const std::string& line)
{
    if (line.empty())
        return;
    g_entries.push_back({line, q.Post(line)});
    if (g_entries.size() > 50)
        g_entries.pop_front();
    g_history.Push(line);
}

std::string AdvancedLastReply(CommandQueue& q)
{
    std::string reply;
    if (g_entries.empty() || !q.Reply(g_entries.back().id, &reply))
        return "";
    return reply;
}

void AdvancedTab(CommandQueue& q)
{
    LoadCommands(q);
    const float listWidth = 260.0f;
    ImGui::BeginChild("console", ImVec2(ImGui::GetContentRegionAvail().x - listWidth - 8.0f, 0), false);

    ImGui::TextDisabled("Any gamelink command, exactly as the TCP port takes it. Up/Down: history.");
    ImGui::SetNextItemWidth(-1.0f);
    if (g_focusInput)
    {
        ImGui::SetKeyboardFocusHere();
        g_focusInput = false;
    }
    if (ImGui::InputText("##cmd", g_input, sizeof(g_input),
                         ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory, OnHistory))
    {
        AdvancedSubmit(q, g_input);
        g_input[0] = '\0';
        g_focusInput = true;
    }

    ImGui::BeginChild("replies", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    for (const Entry& e : g_entries)
    {
        ImGui::TextColored(ImVec4(0.45f, 0.72f, 1.0f, 1.0f), "> %s", e.line.c_str());
        std::string reply;
        if (q.Reply(e.id, &reply))
            ImGui::TextUnformatted(reply.c_str());
        else if (q.Busy(e.id))
            ImGui::TextDisabled("working...");
        else
            ImGui::TextDisabled("(reply no longer kept)");
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
        ImGui::SetScrollHereY(1.0f);              // follow new replies unless scrolled up
    ImGui::EndChild();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("commands", ImVec2(listWidth, 0), true);
    ImGui::TextUnformatted("Commands");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##filter", "filter", g_filter, sizeof(g_filter));
    if (g_commands.empty())
        ImGui::TextDisabled("loading...");
    for (const auto& c : g_commands)
    {
        if (g_filter[0] && c.second.find(g_filter) == std::string::npos)
            continue;
        if (ImGui::Selectable(c.first.c_str()))
        {
            strcpy_s(g_input, (c.first + " ").c_str());
            g_focusInput = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", c.second.c_str());
    }
    ImGui::EndChild();
}
