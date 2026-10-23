#pragma once
#include <cstdio>
#include <vector>

namespace testing
{
struct Case { const char* name; void (*fn)(); };
std::vector<Case>& Registry();
extern int g_failures;
struct Registrar { Registrar(const char* n, void (*f)()) { Registry().push_back({n, f}); } };
}

#define TEST(name)                                                    \
    static void name();                                               \
    static testing::Registrar name##_registrar(#name, &name);         \
    static void name()

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) { ++testing::g_failures; printf("    %s:%d CHECK(%s)\n", __FILE__, __LINE__, #cond); } \
    } while (0)
