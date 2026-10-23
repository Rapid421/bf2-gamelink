#include "test.h"
#include "../src/ui/menu_state.h"

TEST(toggle_opens_and_closes)
{
    MenuState m;
    CHECK(!m.IsOpen());
    m.Toggle();
    CHECK(m.IsOpen());
    m.Toggle();
    CHECK(!m.IsOpen());
}

TEST(focus_lost_closes)
{
    MenuState m;
    m.Open();
    m.OnFocusLost();
    CHECK(!m.IsOpen());
    m.OnFocusLost();                  // closed already: stays closed
    CHECK(!m.IsOpen());
}

TEST(tab_persists_across_toggle)
{
    MenuState m;
    CHECK(m.Tab() == MenuTab::Camera);
    m.Open();
    m.SetTab(MenuTab::Diagnostics);
    m.Close();
    m.Open();
    CHECK(m.Tab() == MenuTab::Diagnostics);
}

TEST(tab_names_round_trip)
{
    CHECK(std::string(MenuTabName(MenuTab::Bots)) == "Bots");
    MenuTab t = MenuTab::Camera;
    CHECK(MenuTabFromName("advanced", &t) && t == MenuTab::Advanced);
    CHECK(!MenuTabFromName("nonsense", &t));
}

// One frame of the tab bar: Force() before drawing, UserPicked() for the tab ImGui shows, End() after.
TEST(tabs_user_can_click_back_to_an_earlier_tab)
{
    TabPicker p;
    int state = 0;                                   // Camera
    CHECK(p.Force(state) == 0);                      // first frame selects the remembered tab
    p.End(state);
    CHECK(p.Force(state) == -1);                     // user clicks World: ImGui shows World
    if (p.UserPicked(1, state)) state = 1;
    p.End(state);
    CHECK(state == 1);
    CHECK(p.Force(state) == -1);                     // user clicks Camera again
    if (p.UserPicked(0, state)) state = 0;
    p.End(state);
    CHECK(state == 0);
    CHECK(p.Force(state) == -1);                     // and it stays on Camera
    CHECK(!p.UserPicked(0, state));
}

TEST(tabs_script_request_is_forced_once)
{
    TabPicker p;
    int state = 0;
    p.Force(state);
    p.End(state);
    state = 4;                                       // "ui tab bots" from a script
    CHECK(p.Force(state) == 4);
    CHECK(!p.UserPicked(0, state));                  // ImGui still shows Camera this frame: not a click
    p.End(state);
    CHECK(p.Force(state) == -1);
    CHECK(!p.UserPicked(4, state));
}
