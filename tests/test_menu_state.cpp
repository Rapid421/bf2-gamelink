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
