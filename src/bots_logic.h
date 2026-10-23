#pragma once
// The arithmetic of "bots set": how many bots to ask each team for, and the player cap that fits them. No game
// access here - unit-tested in tests/test_bots_logic.cpp; main.cpp applies the results.
#include <string>

// AutoPlayerSettings.ForceFillGameplayBotsTeamN for the next step. Moves one bot at a time toward a team of
// `target` players (humans included), and only once the team's player count has settled (same as last check).
int NextFill(int fill, int players, int prevPlayers, int target);

// The ServerPlayerManager capacity that fits two teams: 2 x the bigger team + 2 spare, at most 250 (8-bit ids).
int CapFor(int team1, int team2);

// "64 64" -> both sizes, each clamped to 1..64 (past 64 an engine table of 512 entries runs out).
bool ParseTeamSizes(const std::string& text, int* team1, int* team2);

// A "bots status" reply, parsed for the Bots tab.
struct BotsReport
{
    bool on = false, poolOn = false;
    int target1 = 0, target2 = 0, players1 = 0, players2 = 0, alive1 = 0, alive2 = 0;
    unsigned cap = 0;
    long capfix = 0, overflow = 0, refused = 0;
    std::string note;           // why bots are not running yet ("" when fine)
};
bool ParseBotsStatus(const std::string& reply, BotsReport* out);
