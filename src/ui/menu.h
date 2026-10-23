#pragma once
// The F1 menu window: tabs, their controls, and the work they keep going while the menu is shut (camera travel to
// a saved spot, a pending screenshot). Drawn on the game's present thread inside an ImGui frame.
#include <string>

class MenuState;

// Every frame, between ImGui::NewFrame and ImGui::Render. Draws only while the menu is open.
void Menu_Frame(MenuState& state);
void Menu_Shutdown();
// The same handler a widget runs, for scripts and tests: "ui do camera.freecam 1". Any thread.
std::string Menu_Do(const std::string& id, const std::string& value);
// True for the few frames a screenshot needs the overlays out of the picture.
bool Menu_HideOverlays();

// Theme and size; the font scales with the screen height (1080p = 1x).
void Style_Apply(float screenHeight);
