#include "test.h"
#include "bots_logic.h"

TEST(fill_steps_up_once_counts_settle)
{
    CHECK(NextFill(63, 40, 40, 64) == 64);       // settled and short: one more
    CHECK(NextFill(63, 40, 38, 64) == 63);       // still changing: wait
    CHECK(NextFill(64, 40, 40, 64) == 64);       // never past the target
}

TEST(fill_steps_down_when_over)
{
    CHECK(NextFill(64, 66, 66, 64) == 63);
    CHECK(NextFill(0, 3, 3, 1) == 0);            // never below zero
}

TEST(fill_holds_at_target)
{
    CHECK(NextFill(63, 64, 64, 64) == 63);       // a human makes 64 with 63 bots: hold
}

TEST(cap_for_team_sizes)
{
    CHECK(CapFor(64, 64) == 130);                // 2 x 64 + 2
    CHECK(CapFor(10, 40) == 82);                 // the bigger team decides
    CHECK(CapFor(200, 200) == 250);              // player ids are 8-bit
}

TEST(parse_bots_set_clamps)
{
    int a = 0, b = 0;
    CHECK(ParseTeamSizes("64 64", &a, &b) && a == 64 && b == 64);
    CHECK(ParseTeamSizes("100 0", &a, &b) && a == 64 && b == 1);
    CHECK(!ParseTeamSizes("64", &a, &b));
    CHECK(!ParseTeamSizes("x y", &a, &b));
}

TEST(parse_bots_status_reply)
{
    BotsReport s;
    CHECK(ParseBotsStatus("bots on 64v64 players 64/63 alive 60/58 cap 130 capfix 130 pool on overflow 63 refused 0", &s));
    CHECK(s.on && s.target1 == 64 && s.target2 == 64);
    CHECK(s.players1 == 64 && s.players2 == 63 && s.alive1 == 60 && s.alive2 == 58);
    CHECK(s.cap == 130 && s.capfix == 130 && s.poolOn && s.overflow == 63 && s.refused == 0);
    CHECK(s.note.empty());
    CHECK(ParseBotsStatus("bots off 64v64 players 0/0 alive 0/0 cap 0 capfix 0 pool off overflow 0 refused 0 - waiting for a match", &s));
    CHECK(!s.on && s.note == "waiting for a match");
    CHECK(!ParseBotsStatus("unknown command: bots", &s));
}
