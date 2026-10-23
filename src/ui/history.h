#pragma once
// Up/down command history for the Advanced tab's command line. Keeps the last 100 distinct entries.
#include <string>
#include <vector>

class CommandHistory
{
public:
    void Push(const std::string& line);     // blanks and a repeat of the newest are ignored
    std::string Up();                        // one older (stays on the oldest)
    std::string Down();                      // one newer; past the newest gives ""
    size_t Size() const { return m_lines.size(); }

private:
    std::vector<std::string> m_lines;
    size_t m_pos = 0;                        // == size: not browsing
};
