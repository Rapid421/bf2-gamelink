#pragma once
// Tabs that live in their own files. Called from Menu_Frame inside the tab item, on the present thread, with the
// menu's command queue (never block on it: post, then read replies on later frames).
class CommandQueue;

#include <string>

void DiagnosticsTab(CommandQueue& queue);
// What the Diagnostics tab shows, as one line: "diag fps 118 p99 12.8 over33 0 tickrate 30.0 graph 1180".
std::string DiagnosticsSummary();

void AdvancedTab(CommandQueue& queue);
// The Advanced command line's Enter: run it, add it to the history and the reply panel.
void AdvancedSubmit(CommandQueue& queue, const std::string& line);
// The newest console entry's reply ("" while working).
std::string AdvancedLastReply(CommandQueue& queue);
