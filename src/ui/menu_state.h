#pragma once
// Open/closed state and current tab of the F1 menu. Thread-safe reads (the raw-input hooks read IsOpen on game
// threads; the present thread changes it).
#include <atomic>
#include <string>

enum class MenuTab { Camera, World, Inspector, Logic, Bots, Diagnostics, Advanced, Count };

const char* MenuTabName(MenuTab t);
bool MenuTabFromName(const std::string& name, MenuTab* out);   // case-insensitive

// Keeps the tab bar and MenuState in step. A tab requested from outside (a script's "ui tab", the remembered tab
// on first draw) is forced once; after that the tab ImGui shows is the user's choice. Tabs are ints (MenuTab).
class TabPicker
{
public:
    int Force(int wanted);                          // before the tab bar: the tab to force selected, or -1
    bool UserPicked(int shown, int wanted) const;   // for the tab ImGui is showing: did the user just pick it?
    void End(int wanted) { m_lastSeen = wanted; }   // after the tab bar, with the state's (possibly new) tab

private:
    int m_lastSeen = -1;
    bool m_external = false;
};

class MenuState
{
public:
    void Toggle();
    void Open();
    void Close();
    bool IsOpen() const;
    void OnFocusLost();             // the game window lost focus: close, so input and the cursor go back
    MenuTab Tab() const;
    void SetTab(MenuTab t);

private:
    std::atomic<bool> m_open{false};
    std::atomic<int> m_tab{static_cast<int>(MenuTab::Camera)};
};
