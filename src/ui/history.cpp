#include "history.h"

void CommandHistory::Push(const std::string& line)
{
    if (!line.empty() && (m_lines.empty() || m_lines.back() != line))
    {
        m_lines.push_back(line);
        if (m_lines.size() > 100)
            m_lines.erase(m_lines.begin());
    }
    m_pos = m_lines.size();
}

std::string CommandHistory::Up()
{
    if (m_lines.empty())
        return "";
    if (m_pos > 0)
        --m_pos;
    return m_lines[m_pos];
}

std::string CommandHistory::Down()
{
    if (m_pos < m_lines.size())
        ++m_pos;
    return m_pos < m_lines.size() ? m_lines[m_pos] : "";
}
