#pragma once
// Tabs that live in their own files. Called from Menu_Frame inside the tab item, on the present thread, with the
// menu's command queue (never block on it: post, then read replies on later frames).
class CommandQueue;

void DiagnosticsTab(CommandQueue& queue);
void AdvancedTab(CommandQueue& queue);
