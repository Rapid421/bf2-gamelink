#pragma once
// The narrow interface the in-game UI (src/ui) uses to reach the rest of gamelink. Implemented in main.cpp.
// Keeping the UI behind this lets its logic be unit-tested against a fake (tests/fake_gamelink.cpp).
#include <string>

namespace gl
{
// Run one gamelink text command (exactly what the TCP interface accepts) and return its reply.
// Exception-guarded; safe to call from any thread except the game's present thread (use CommandQueue there).
std::string Command(const std::string& line);

// True while the F1 menu is open: the game receives no keyboard/mouse input and the cursor is free.
bool MenuCapturesInput();
}
