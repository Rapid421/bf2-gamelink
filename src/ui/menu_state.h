#pragma once
// Open/closed state and current tab of the F1 menu. Thread-safe reads (the raw-input hooks read IsOpen on game
// threads; the present thread changes it).
#include <atomic>
#include <string>

enum class MenuTab { Camera, World, Inspector, Logic, Bots, Diagnostics, Advanced, Count };

const char* MenuTabName(MenuTab t);
bool MenuTabFromName(const std::string& name, MenuTab* out);   // case-insensitive

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
