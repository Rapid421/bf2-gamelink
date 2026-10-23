#include "bots_logic.h"
#include <cstdio>
#include <cstring>

int NextFill(int fill, int players, int prevPlayers, int target)
{
    if (players != prevPlayers)
        return fill;
    if (players < target && fill < target)
        return fill + 1;
    if (players > target && fill > 0)
        return fill - 1;
    return fill;
}

int CapFor(int team1, int team2)
{
    const int cap = 2 * (team1 > team2 ? team1 : team2) + 2;
    return cap > 250 ? 250 : cap;
}

bool ParseTeamSizes(const std::string& text, int* team1, int* team2)
{
    int a = 0, b = 0;
    if (sscanf_s(text.c_str(), "%d %d", &a, &b) != 2)
        return false;
    auto clamp = [](int v) { return v < 1 ? 1 : (v > 64 ? 64 : v); };
    *team1 = clamp(a);
    *team2 = clamp(b);
    return true;
}

bool ParseBotsStatus(const std::string& reply, BotsReport* out)
{
    char on[8] = {}, pool[8] = {};
    BotsReport r;
    if (sscanf_s(reply.c_str(), "bots %7s %dv%d players %d/%d alive %d/%d cap %u capfix %ld pool %7s overflow %ld refused %ld",
                 on, static_cast<unsigned>(sizeof(on)), &r.target1, &r.target2, &r.players1, &r.players2, &r.alive1,
                 &r.alive2, &r.cap, &r.capfix, pool, static_cast<unsigned>(sizeof(pool)), &r.overflow, &r.refused) != 12)
        return false;
    r.on = strcmp(on, "on") == 0;
    r.poolOn = strcmp(pool, "on") == 0;
    const size_t dash = reply.find(" - ");
    if (dash != std::string::npos)
        r.note = reply.substr(dash + 3);
    *out = r;
    return true;
}
