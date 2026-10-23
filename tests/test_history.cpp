#include "test.h"
#include "ui/history.h"

TEST(history_up_walks_back_from_newest)
{
    CommandHistory h;
    h.Push("ping");
    h.Push("levelname");
    CHECK(h.Up() == "levelname");
    CHECK(h.Up() == "ping");
    CHECK(h.Up() == "ping");                 // stays on the oldest
}

TEST(history_down_returns_to_empty_line)
{
    CommandHistory h;
    h.Push("ping");
    h.Push("levelname");
    h.Up();
    h.Up();
    CHECK(h.Down() == "levelname");
    CHECK(h.Down() == "");                   // past the newest: a fresh line
}

TEST(history_skips_repeats_and_blanks)
{
    CommandHistory h;
    h.Push("ping");
    h.Push("ping");
    h.Push("");
    CHECK(h.Size() == 1);
}

TEST(history_push_resets_position)
{
    CommandHistory h;
    h.Push("a");
    h.Push("b");
    h.Up();
    h.Push("c");
    CHECK(h.Up() == "c");
}

TEST(history_keeps_last_100)
{
    CommandHistory h;
    for (int i = 0; i < 150; ++i)
        h.Push("cmd" + std::to_string(i));
    CHECK(h.Size() == 100);
}
