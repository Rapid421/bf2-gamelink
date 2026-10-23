#include "menu_state.h"
#include <cctype>

namespace
{
const char* kNames[] = {"Camera", "World", "Inspector", "Logic", "Bots", "Diagnostics", "Advanced"};
}

const char* MenuTabName(MenuTab t)
{
    const int i = static_cast<int>(t);
    return i >= 0 && i < static_cast<int>(MenuTab::Count) ? kNames[i] : "?";
}

bool MenuTabFromName(const std::string& name, MenuTab* out)
{
    for (int i = 0; i < static_cast<int>(MenuTab::Count); ++i)
    {
        const std::string n = kNames[i];
        if (n.size() != name.size())
            continue;
        bool same = true;
        for (size_t k = 0; k < n.size() && same; ++k)
            same = std::tolower(static_cast<unsigned char>(n[k])) == std::tolower(static_cast<unsigned char>(name[k]));
        if (same)
        {
            *out = static_cast<MenuTab>(i);
            return true;
        }
    }
    return false;
}

void MenuState::Toggle() { m_open = !m_open.load(); }
void MenuState::Open() { m_open = true; }
void MenuState::Close() { m_open = false; }
bool MenuState::IsOpen() const { return m_open.load(); }
void MenuState::OnFocusLost() { m_open = false; }
MenuTab MenuState::Tab() const { return static_cast<MenuTab>(m_tab.load()); }
void MenuState::SetTab(MenuTab t) { m_tab = static_cast<int>(t); }
