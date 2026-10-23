#include "menu.h"
#include "imgui.h"

void Style_Apply(float screenHeight)
{
    ImGuiIO& io = ImGui::GetIO();
    float scale = screenHeight / 1080.0f;
    if (scale < 1.0f) scale = 1.0f;
    if (scale > 2.5f) scale = 2.5f;
    io.FontGlobalScale = scale;

    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 6.0f;
    s.FrameRounding = 4.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 4.0f;
    s.WindowPadding = ImVec2(12.0f, 10.0f);
    s.FramePadding = ImVec2(8.0f, 4.0f);
    s.ItemSpacing = ImVec2(8.0f, 6.0f);
    s.WindowBorderSize = 0.0f;

    const ImVec4 accent(0.26f, 0.62f, 0.98f, 1.0f);
    const ImVec4 accentDim(0.20f, 0.45f, 0.75f, 1.0f);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.08f, 0.10f, 0.94f);
    c[ImGuiCol_TitleBg] = ImVec4(0.09f, 0.10f, 0.13f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.11f, 0.13f, 0.17f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.16f, 0.20f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.21f, 0.27f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.26f, 0.33f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.16f, 0.19f, 0.25f, 1.0f);
    c[ImGuiCol_ButtonHovered] = accentDim;
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accentDim;
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_Tab] = ImVec4(0.12f, 0.14f, 0.18f, 1.0f);
    c[ImGuiCol_TabHovered] = accentDim;
    c[ImGuiCol_TabActive] = ImVec4(0.18f, 0.32f, 0.52f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.16f, 0.19f, 0.25f, 1.0f);
    c[ImGuiCol_HeaderHovered] = accentDim;
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_Separator] = ImVec4(0.20f, 0.23f, 0.29f, 1.0f);
}
