// The AI build (build.bat ai): no F1 menu. Compiled instead of menu.cpp, style.cpp and the tab files, so main.cpp
// links unchanged; every menu entry point does nothing and the "ui" command says why.
#include "menu.h"

void Menu_Frame(MenuState&) {}
void Menu_Shutdown() {}
std::string Menu_Do(const std::string&, const std::string&) { return "ui: this is the AI build of gamelink - it has no menu"; }
bool Menu_HideOverlays() { return false; }
void Style_Apply(float) {}
