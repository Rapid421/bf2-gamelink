// Minimal test runner for the gamelink UI logic (no framework). Tests register with TEST(name) and assert with
// CHECK(cond). Exit code 0 = all passed.
#include "test.h"
#include <cstdio>
#include <vector>

namespace testing
{
std::vector<Case>& Registry()
{
    static std::vector<Case> cases;
    return cases;
}
int g_failures = 0;
}

int main()
{
    int passed = 0, failed = 0;
    for (const testing::Case& c : testing::Registry())
    {
        const int before = testing::g_failures;
        c.fn();
        if (testing::g_failures == before) { ++passed; printf("  ok   %s\n", c.name); }
        else { ++failed; printf("  FAIL %s\n", c.name); }
    }
    printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
