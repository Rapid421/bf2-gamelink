// FrostyGameLink - in-process bridge between the Frosty editor and a running
// Star Wars Battlefront II (2017).
//
// WHY THIS EXISTS
//
// Everything that can be done from outside the game has been done and measured:
//   * the reflection registry can be walked over ReadProcessMemory, and settings
//     writes (WorldRenderSettings.ViewMode) land and are visible;
//   * the game window reparents into a WPF pane and keeps rendering.
// And everything that cannot be done from outside has been established the hard way:
//   * camera state is invisible to reflection - Camera and FreeCamera are registered
//     with empty field lists;
//   * driving entities through Kyber's Lua bindings crashes this build - property
//     reads, property writes and entity creation each killed the process, and a native
//     access violation cannot be caught by any amount of script-side guarding.
//
// So anything realtime has to run inside the process. That is this module.
//
// PROVENANCE OF THE ADDRESSES
//
// Structure layouts for the reflection registry come from Frosty Toolsuite's own SDK
// generator, and were re-verified against this executable. The renderer globals were
// identified from the Kyber project's SDK headers, which is a fact about this build
// (GameRenderer instance pointer, renderView offset, transform layout) - the code here
// is written from scratch.
//
// Everything is bound to starwarsbattlefrontii.exe with module base 0x140000000 and a
// mapped size of 0xE45C000. The module refuses to run against anything else.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <psapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <share.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cctype>
#include <cfloat>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include <map>

#include "imgui.h"                 // src/imgui: Dear ImGui 1.78 (MIT), the feed overlay
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "gamelink_api.h"         // the in-game UI's interface to the rest of gamelink
#include "ui/menu_state.h"        // F1 menu open/closed + tab
#include "ui/menu.h"              // F1 menu window and tabs

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "user32.lib")   // capture: the probe window for the swap-chain vtable

namespace gamelink
{
// ---------------------------------------------------------------------------
// Build-specific constants
// ---------------------------------------------------------------------------

constexpr uintptr_t kExpectedModuleBase = 0x140000000ull;
constexpr uintptr_t kExpectedModuleSize = 0xE45C000ull;

// GameRenderer instance pointer. Absolute in the Kyber SDK; stored here as an RVA so
// it survives a rebase.
constexpr uintptr_t kGameRendererPtrRva = 0x3FFBE10;
constexpr size_t kGameRendererRenderViewOffset = 0x538;
constexpr size_t kRenderViewFovOffset = 0xB0;

// Reflection registry, BFII layout.
constexpr size_t kClassInfoSize = 0x30;
constexpr size_t kTypeInfoSize = 0x60;
constexpr size_t kFieldInfoSize = 0x18;

constexpr int kListenPort = 31500;

// Level loading. These are engine functions, identified from the Kyber project's SDK
// header as facts about this build and stored as RVAs so a rebase does not break them.
//
//   LevelSetup::Init()                    0x14C53D220
//   LevelSetup::SetInclusionOption(k, v)  0x141136690
//   ServerLoadLevelMessage_post(setup,
//                               fadeOut,
//                               forceReloadResources)  0x140BCF350
//
// LevelSetup layout: vtable +0x00, Name +0x08, InitialDSubLevel +0x20,
// InitialStartPoint +0x28, total under 0x100 bytes.
constexpr uintptr_t kLevelSetupInitRva = 0xC53D220;
constexpr uintptr_t kLevelSetupSetInclusionOptionRva = 0x1136690;
constexpr uintptr_t kLevelSetupGetInclusionOptionRva = 0x70C3010;   // OX B11.1.1
constexpr uintptr_t kServerLoadLevelMessagePostRva = 0xBCF350;

constexpr size_t kLevelSetupNameOffset = 0x08;

// World / session state. `g_gameWorld` is an array indexed by Realm (0 = Client,
// 1 = Server); a non-null entry means that realm has a world, which is the precondition
// a level load needs and the thing a cold frontend lacks.
// Identified by the RE pass (RE-ANSWERS-OX.md S1.1/S1.6/S1.7), confirmed in-process.
constexpr uintptr_t kGameWorldArrayRva = 0x3EEC298;
constexpr uintptr_t kEntityWorldArrayRva = 0x3FCB370;
constexpr uintptr_t kClientGameContextRva = 0x3EE7858;
constexpr uintptr_t kServerGameContextRva = 0x3EC7238;
constexpr size_t kClientGameContextLevelOffset = 0x38;

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------

static FILE* g_log = nullptr;
static uintptr_t g_base = 0;
static size_t g_size = 0;

// The game holds the log file open for the whole of its life, so it could not be
// read from outside while a test was running - which is exactly when the lines
// matter. Two answers here: open the file so other readers are allowed in, and
// keep the recent lines in memory so they can be pulled back over the command
// port without touching the file at all.
static const int kLogRingLines = 512;
static const int kLogLineChars = 256;
static char g_logRing[kLogRingLines][kLogLineChars];
static int g_logRingNext = 0;
static int g_logRingCount = 0;
static CRITICAL_SECTION g_logLock;
static bool g_logLockReady = false;

static void LogLine(const char* fmt, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    SYSTEMTIME now;
    GetLocalTime(&now);

    char stamped[1152];
    _snprintf_s(stamped, sizeof(stamped), _TRUNCATE, "[%02d:%02d:%02d.%03d] %s",
                now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, buffer);

    if (g_logLockReady)
        EnterCriticalSection(&g_logLock);

    if (g_log != nullptr)
    {
        fputs(stamped, g_log);
        fputc('\n', g_log);
        fflush(g_log);
    }

    strncpy_s(g_logRing[g_logRingNext], kLogLineChars, stamped, _TRUNCATE);
    g_logRingNext = (g_logRingNext + 1) % kLogRingLines;
    if (g_logRingCount < kLogRingLines)
        ++g_logRingCount;

    if (g_logLockReady)
        LeaveCriticalSection(&g_logLock);

    OutputDebugStringA(buffer);
}

// The last `count` lines, oldest first.
static std::string LogTail(int count)
{
    if (count < 1)
        count = 1;

    std::string out;
    if (g_logLockReady)
        EnterCriticalSection(&g_logLock);

    if (count > g_logRingCount)
        count = g_logRingCount;

    for (int i = count; i > 0; --i)
    {
        const int index = ((g_logRingNext - i) % kLogRingLines + kLogRingLines) % kLogRingLines;
        out += g_logRing[index];
        out += '\n';
    }

    if (g_logLockReady)
        LeaveCriticalSection(&g_logLock);

    if (out.empty())
        out = "(nothing logged yet)\n";
    return out;
}

// Reading engine memory in-process is a plain dereference, but a wrong pointer is a
// crash rather than a failed call - so every read that could touch a bad address goes
// through this.
template <typename T>
static bool SafeRead(uintptr_t address, T* out)
{
    if (address < 0x10000)
        return false;

    __try
    {
        memcpy(out, reinterpret_cast<const void*>(address), sizeof(T));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// The same guard for a run of bytes whose size is only known at runtime -
// prologue scans and byte dumps, where a templated read on a fixed type will not
// do.
static bool SafeReadBytes(uintptr_t address, void* out, size_t size)
{
    if (address < 0x10000 || size == 0)
        return false;

    __try
    {
        memcpy(out, reinterpret_cast<const void*>(address), size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// RtlLookupFunctionEntry, wrapped so the guard lives in a frame with nothing to
// unwind. HandleCommand builds std::strings, and MSVC refuses __try in a frame
// that owns objects with destructors (C2712).
static bool SafeLookupFunctionEntry(uintptr_t address, uintptr_t* imageBase,
                                    DWORD* beginRva, DWORD* endRva)
{
    __try
    {
        DWORD64 base = 0;
        PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(address, &base, nullptr);
        if (entry == nullptr || base == 0)
            return false;
        *imageBase = static_cast<uintptr_t>(base);
        *beginRva = entry->BeginAddress;
        *endRva = entry->EndAddress;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool SafeWriteBytes(uintptr_t address, const void* data, size_t size)
{
    if (address < 0x10000)
        return false;

    __try
    {
        memcpy(reinterpret_cast<void*>(address), data, size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// MSVC will not compile __try inside a function that owns objects needing unwinding,
// so every guarded read is a plain C-style helper and the C++ containers stay outside.
static bool RawReadString(char* out, size_t outSize, uintptr_t address, size_t limit)
{
    __try
    {
        const char* p = reinterpret_cast<const char*>(address);
        size_t i = 0;
        for (; i < limit && i + 1 < outSize && p[i] != '\0'; ++i)
            out[i] = p[i];
        out[i] = '\0';
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static std::string SafeCString(uintptr_t address, size_t limit = 256)
{
    if (address < 0x10000)
        return std::string();

    char buffer[512];
    if (!RawReadString(buffer, sizeof(buffer), address, limit))
        return std::string();

    return std::string(buffer);
}

// ---------------------------------------------------------------------------
// Reflection registry
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Crash reporting.
//
// spawnServer now returns cleanly and the process dies later, inside the async
// load, on a thread we do not own. Without this the log simply stops and the
// cause is a guess. Two hooks:
//
//   - a vectored handler, which sees every fault first-chance. The engine
//     raises handled exceptions of its own, so this only logs hard memory
//     faults and caps itself so it can never flood the log or slow a load.
//   - the unhandled filter, which fires once, on the fault that actually kills
//     the process. That one is the report that matters.
//
// No dbghelp symbol init - it is slow and can deadlock inside a crashing
// process. Instead the raw context is logged, plus any return addresses lying
// on the stack that point into the game module. Those RVAs are what OX needs.
// ---------------------------------------------------------------------------
static volatile LONG g_faultsLogged = 0;
static volatile DWORD g_spawnThreadId = 0;   // set just before spawnServer is called
static const LONG kMaxFaultsLogged = 40;

// Copy up to `words` qwords from a (suspended) thread's stack; stops at the first unreadable one. No C++ objects
// here, so __try is allowed.
static size_t CopyStackWords(uintptr_t from, uintptr_t* to, size_t words)
{
    size_t got = 0;
    __try
    {
        for (; got < words; ++got)
            to[got] = reinterpret_cast<const uintptr_t*>(from)[got];
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return got;
}

// 8-aligned slots in [regionBase, regionEnd) holding exactly `value`; for findvt.
static size_t ScanRegionForQword(uintptr_t regionBase, uintptr_t regionEnd, uintptr_t value, uintptr_t* out, size_t cap)
{
    size_t count = 0;
    __try
    {
        for (uintptr_t slot = regionBase; slot + 8 <= regionEnd && count < cap; slot += 8)
            if (*reinterpret_cast<const uintptr_t*>(slot) == value)
                out[count++] = slot;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return count;
}

static void DescribeAddress(char* out, size_t outSize, uintptr_t address)
{
    if (g_base != 0 && address >= g_base && address < g_base + g_size)
        _snprintf_s(out, outSize, _TRUNCATE, "game+0x%llX", address - g_base);
    else
    {
        HMODULE owner = nullptr;
        char name[MAX_PATH] = { 0 };
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(address), &owner) &&
            owner != nullptr &&
            GetModuleBaseNameA(GetCurrentProcess(), owner, name, sizeof(name)) != 0)
        {
            _snprintf_s(out, outSize, _TRUNCATE, "%s+0x%llX", name,
                        address - reinterpret_cast<uintptr_t>(owner));
        }
        else
        {
            _snprintf_s(out, outSize, _TRUNCATE, "0x%llX (unmapped)", address);
        }
    }
}

static void ReportFault(const char* tag, EXCEPTION_POINTERS* pointers)
{
    if (pointers == nullptr || pointers->ExceptionRecord == nullptr)
        return;

    const EXCEPTION_RECORD* record = pointers->ExceptionRecord;
    const CONTEXT* context = pointers->ContextRecord;

    char where[256];
    DescribeAddress(where, sizeof(where), reinterpret_cast<uintptr_t>(record->ExceptionAddress));

    const char* codeName = "";
    switch (record->ExceptionCode)
    {
        case 0xC0000005: codeName = " ACCESS_VIOLATION"; break;
        case 0xC0000006: codeName = " IN_PAGE_ERROR"; break;
        case 0xC000001D: codeName = " ILLEGAL_INSTRUCTION"; break;
        case 0xC0000025: codeName = " NONCONTINUABLE"; break;
        case 0xC000008C: codeName = " ARRAY_BOUNDS_EXCEEDED"; break;
        case 0xC0000094: codeName = " INT_DIVIDE_BY_ZERO"; break;
        case 0xC00000FD: codeName = " STACK_OVERFLOW"; break;
        case 0xC0000374: codeName = " HEAP_CORRUPTION"; break;
        case 0xC0000409: codeName = " FAIL_FAST"; break;
        case 0x80000003: codeName = " BREAKPOINT"; break;
        default: break;
    }

    const DWORD thisThread = GetCurrentThreadId();
    const char* whose = "";
    if (g_spawnThreadId != 0)
        whose = (thisThread == g_spawnThreadId) ? " [our spawn thread]" : " [an engine thread]";

    LogLine("%s: code 0x%08X%s at %s (thread %lu%s)", tag, record->ExceptionCode, codeName,
            where, thisThread, whose);

    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
    {
        const ULONG_PTR kind = record->ExceptionInformation[0];
        const ULONG_PTR address = record->ExceptionInformation[1];
        LogLine("%s: %s 0x%llX", tag,
                kind == 0 ? "read from" : (kind == 1 ? "write to" : "execute at"),
                static_cast<unsigned long long>(address));
    }

    if (context == nullptr)
        return;

    LogLine("%s: rip=0x%llX rsp=0x%llX rbp=0x%llX", tag, context->Rip, context->Rsp, context->Rbp);
    LogLine("%s: rcx=0x%llX rdx=0x%llX r8=0x%llX r9=0x%llX rax=0x%llX",
            tag, context->Rcx, context->Rdx, context->R8, context->R9, context->Rax);

    // Every general register, then a short hex dump of whatever each one points at, so a
    // null-object fault can be read back offline (which record, which table, which vtable).
    {
        struct { const char* n; DWORD64 v; } regs[] = {
            {"rax", context->Rax}, {"rbx", context->Rbx}, {"rcx", context->Rcx}, {"rdx", context->Rdx},
            {"rsi", context->Rsi}, {"rdi", context->Rdi}, {"rbp", context->Rbp}, {"r8", context->R8},
            {"r9", context->R9}, {"r10", context->R10}, {"r11", context->R11}, {"r12", context->R12},
            {"r13", context->R13}, {"r14", context->R14}, {"r15", context->R15} };
        LogLine("%s: rbx=0x%llX rsi=0x%llX rdi=0x%llX r10=0x%llX r11=0x%llX r12=0x%llX r13=0x%llX r14=0x%llX r15=0x%llX",
                tag, context->Rbx, context->Rsi, context->Rdi, context->R10, context->R11, context->R12, context->R13, context->R14, context->R15);
        for (auto& r : regs)
        {
            if (r.v < 0x10000) continue;
            unsigned char buf[0x40];
            if (!SafeReadBytes(static_cast<uintptr_t>(r.v) - 0x10, buf, sizeof(buf))) continue;
            char hex[0x40 * 3 + 1]; int o = 0;
            for (size_t i = 0; i < sizeof(buf); ++i) o += _snprintf_s(hex + o, sizeof(hex) - o, _TRUNCATE, "%02X%s", buf[i], (i % 8 == 7) ? " " : "");
            uintptr_t first = 0; char vt[64] = "";
            if (SafeRead(static_cast<uintptr_t>(r.v), &first) && g_base != 0 && first > g_base && first < g_base + g_size)
                _snprintf_s(vt, sizeof(vt), _TRUNCATE, " [*%s = game+0x%llX]", r.n, first - g_base);
            LogLine("%s:   mem %s-0x10: %s%s", tag, r.n, hex, vt);
        }
        // The section-record walker at game+0x7DFB64C: r15 -> 12-byte records {u32 handleA, u16 x,
        // u32 handleB}, rbx -> table pointer; handle = (page<<16 | slot) -> entry -> object (null).
        if (g_base != 0 && context->Rip == g_base + 0x7DFB64C)
        {
            uint32_t hA = 0, hB = 0; uintptr_t table = 0;
            SafeRead(static_cast<uintptr_t>(context->R15) - 0x10, &hA);
            SafeRead(static_cast<uintptr_t>(context->R15) - 0x08, &hB);
            SafeRead(static_cast<uintptr_t>(context->Rbx), &table);
            LogLine("%s:   walker: handleA=0x%08X handleB=0x%08X table=0x%llX", tag, hA, hB, table);
            for (int which = 0; which < 2; ++which)
            {
                uint32_t h = which == 0 ? hA : hB;
                uintptr_t page = 0, entry = 0;
                if (!SafeRead(table + (h & 0xFFFF) * 8, &page) || !SafeRead(page + (h >> 16) * 8, &entry)) continue;
                for (int d = -3; d <= 3; ++d)
                {
                    uintptr_t e = 0, obj = 0, vtable = 0;
                    if (!SafeRead(page + ((h >> 16) + d) * 8, &e)) continue;
                    SafeRead(e, &obj);
                    if (obj != 0) SafeRead(obj, &vtable);
                    LogLine("%s:   handle%c slot%+d: entry=0x%llX obj=0x%llX vtable=%s0x%llX", tag, which == 0 ? 'A' : 'B', d, e, obj,
                            (g_base != 0 && vtable > g_base && vtable < g_base + g_size) ? "game+" : "", (g_base != 0 && vtable > g_base && vtable < g_base + g_size) ? vtable - g_base : vtable);
                }
            }
            // Name whatever asset is nearby: scan the stack and the draw record's neighbourhood for
            // pointers into memory that carries an asset path (MeshSet resources embed their name).
            {
                int found = 0;
                auto scanRange = [&](uintptr_t from, uintptr_t to, const char* label) {
                    for (uintptr_t slot = from; slot + 8 <= to && found < 24; slot += 8)
                    {
                        uintptr_t ptr = 0;
                        if (!SafeRead(slot, &ptr) || ptr < 0x10000 || ptr > 0x7FFFFFFFFFFF) continue;
                        char buf[0x400];
                        if (!SafeReadBytes(ptr, buf, sizeof(buf))) continue;
                        for (size_t i = 0; i + 8 < sizeof(buf); ++i)
                        {
                            if ((memcmp(buf + i, "objects/", 8) == 0) || (memcmp(buf + i, "s2/objec", 8) == 0) || (memcmp(buf + i, "levels/", 7) == 0) || (memcmp(buf + i, "Objects/", 8) == 0))
                            {
                                char name[160]; size_t n = 0;
                                while (i + n < sizeof(buf) && n < sizeof(name) - 1 && buf[i + n] >= 0x20 && buf[i + n] < 0x7F) { name[n] = buf[i + n]; ++n; }
                                name[n] = 0;
                                LogLine("%s:   name near %s+0x%llX -> 0x%llX+0x%zX: %s", tag, label, slot - from, ptr, i, name);
                                ++found;
                                break;
                            }
                        }
                    }
                };
                scanRange(context->Rsp, context->Rsp + 0x1000, "rsp");
                scanRange(context->R13 - 0x400, context->R13 + 0x400, "r13-0x400");
                scanRange(context->Rbx - 0x200, context->Rbx + 0x200, "rbx-0x200");
                scanRange(context->Rcx - 0x100, context->Rcx + 0x100, "rcx-0x100");
            }
            // Watched GUIDs (C:\ProgramData\FrostyGameLink\watch-guids.txt: "<32 hex, bytes as stored> <name>"
            // per line, written by the test runner from the mod's chunk records): search the stack, the
            // objects it points at, and the draw record's neighbourhood for any of them, to name the
            // mesh whose geometry is being bound.
            {
                struct Watched { unsigned char g[16]; char name[200]; };
                static Watched* watched = nullptr; static int watchedCount = 0;
                if (watched == nullptr)
                {
                    watched = new Watched[4096];
                    FILE* wf = nullptr;
                    errno_t fe = fopen_s(&wf, "C:/ProgramData/FrostyGameLink/watch-guids.txt", "r");
                    LogLine("%s:   watch file open rc=%d", tag, (int)fe);
                    if (fe == 0 && wf)
                    {
                        char line[300];
                        while (watchedCount < 4096 && fgets(line, sizeof(line), wf))
                        {
                            if (strlen(line) < 34) continue;
                            Watched& w = watched[watchedCount];
                            bool ok = true;
                            for (int k = 0; k < 16; ++k) { unsigned v; if (sscanf_s(line + k * 2, "%2x", &v) != 1) { ok = false; break; } w.g[k] = (unsigned char)v; }
                            if (!ok) continue;
                            strncpy_s(w.name, sizeof(w.name), line + 33, _TRUNCATE);
                            size_t L = strlen(w.name); while (L > 0 && (w.name[L - 1] == '\n' || w.name[L - 1] == '\r')) w.name[--L] = 0;
                            ++watchedCount;
                        }
                        fclose(wf);
                    }
                    LogLine("%s:   watch list: %d guid(s)", tag, watchedCount);
                }
                int hits = 0;
                auto scanRegion = [&](uintptr_t from, size_t size, const char* label, uintptr_t origin) {
                    static unsigned char buf[0x1000];
                    if (size > sizeof(buf)) size = sizeof(buf);
                    if (!SafeReadBytes(from, buf, size)) return;
                    for (int w = 0; w < watchedCount && hits < 40; ++w)
                        for (size_t i = 0; i + 16 <= size; ++i)
                            if (memcmp(buf + i, watched[w].g, 16) == 0)
                            { LogLine("%s:   guid hit %s (from stack+0x%llX) at 0x%llX+0x%zX: %s", tag, label, origin, from, i, watched[w].name); ++hits; break; }
                };
                scanRegion(context->Rsp, 0x1000, "stack", 0);
                for (uintptr_t slot = context->Rsp; slot < context->Rsp + 0x1000 && hits < 40; slot += 8)
                {
                    uintptr_t ptr = 0;
                    if (!SafeRead(slot, &ptr) || ptr < 0x10000 || ptr > 0x7FFFFFFFFFFF) continue;
                    scanRegion(ptr - 0x100, 0x900, "stack-ptr", slot - context->Rsp);
                }
                scanRegion(context->R13 - 0x800, 0x1000, "r13", 0);
                scanRegion(context->Rbx - 0x800, 0x1000, "rbx", 0);
                scanRegion(context->Rcx - 0x400, 0x800, "rcx", 0);
                scanRegion(context->R15 - 0x800, 0x1000, "r15", 0);
                LogLine("%s:   guid hits: %d", tag, hits);
            }
            int pages = 0, nonNull = 0, total = 0;
            for (int p = 0; p < 4096; ++p)
            {
                uintptr_t page = 0;
                if (!SafeRead(table + p * 8, &page) || page < 0x10000) break;
                ++pages;
                for (int sIdx = 0; sIdx < 65536 && total < 2000000; ++sIdx)
                {
                    uintptr_t e = 0, obj = 0;
                    if (!SafeRead(page + sIdx * 8, &e) || e < 0x10000) break;
                    ++total; if (SafeRead(e, &obj) && obj != 0) ++nonNull;
                }
            }
            LogLine("%s:   walker: table pages=%d entries=%d nonNull=%d", tag, pages, total, nonNull);
        }
    }

    // Walk the stack for anything that looks like a return address into the game
    // module. Unwinding properly needs dbghelp state this process may no longer
    // be able to give us; this is crude but it is enough to name the caller.
    int printed = 0;
    for (uintptr_t slot = context->Rsp; slot < context->Rsp + 0x800 && printed < 16; slot += sizeof(uintptr_t))
    {
        uintptr_t candidate = 0;
        if (!SafeRead(slot, &candidate))
            break;
        if (g_base != 0 && candidate > g_base && candidate < g_base + g_size)
        {
            LogLine("%s:   stack+0x%03llX -> game+0x%llX", tag, slot - context->Rsp, candidate - g_base);
            ++printed;
        }
    }
}

static HMODULE g_selfModule = nullptr;

// True when the faulting instruction is inside this DLL. SafeRead and every
// Guarded* helper fault on purpose whenever a probe hits a bad address, so those
// are routine and must not be logged - in the 15:00 run they buried the one
// fault that mattered under sixty lines of our own probing.
static bool FaultIsOurs(const EXCEPTION_RECORD* record)
{
    if (g_selfModule == nullptr || record == nullptr)
        return false;

    MODULEINFO info{};
    if (!GetModuleInformation(GetCurrentProcess(), g_selfModule, &info, sizeof(info)))
        return false;

    const uintptr_t address = reinterpret_cast<uintptr_t>(record->ExceptionAddress);
    const uintptr_t base = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
    return address >= base && address < base + info.SizeOfImage;
}

static volatile uintptr_t g_watchAddress = 0;
static volatile LONG g_watchHits = 0;
static const LONG kMaxWatchHits = 12;

// watchrw: a read/write watch that logs each DISTINCT accessing instruction once (up to kMaxWatchSites) and
// stays armed - "who reads this setting" rather than "who wrote it last". Fixed arrays: the handler runs on
// engine threads and must not allocate before deciding a hit is new.
static volatile bool g_watchUnique = false;
static const LONG kMaxWatchSites = 64;
static volatile uintptr_t g_watchSites[kMaxWatchSites];
static volatile LONG g_watchSiteCount = 0;

// Set while driving: the handler stops logging and starts overwriting.
//
// A raw 64-byte buffer rather than a Transform, because the exception handler
// lives above the struct definition and only ever memcpy's it.
static volatile bool g_watchDrive = false;
alignas(16) static unsigned char g_watchPose[64];

// Where the handler writes, and how much. Deliberately not the watched address:
// writing the bytes under the breakpoint re-enters the handler forever.
static volatile uintptr_t g_watchWriteBase = 0;
constexpr size_t kWatchWriteSize = 60;      // basis + position, not translation.w

// Field of view, written in the same handler. A thread cannot hold it: the
// engine rewrites it every frame, exactly as it did the transform.
static volatile bool g_fovOverride = false;
static volatile float g_fovRadians = 1.4f;
static volatile uintptr_t g_fovAddress = 0;

// ---------------------------------------------------------------------------
// Drawing from the engine's own thread.
//
// Debug primitives are queued per-thread. The 01:00 run drew from our thread and
// nothing appeared - though that proves nothing on its own, because the pick
// never succeeded and the draw code never ran with a real entity.
//
// The watchdrive breakpoint already traps on the engine thread that writes the
// camera, once per frame. That is a per-frame callback on the right thread, and
// it is where selection geometry belongs. So: our thread computes the geometry
// and leaves it in a snapshot; this handler only submits it.
//
// The handler stays cheap. No allocation, no engine reads, no logging - just the
// debug-renderer calls, throttled, and every one of them guarded. The trap lands
// after the watched write retires, so the thread is at an instruction boundary,
// but this is still engine code re-entered from an exception context. If it is
// going to bite, it bites on the first frame with something selected.
// ---------------------------------------------------------------------------
enum DrawRoute
{
    kDrawOff = 0,
    kDrawThread = 1,     // our own thread, the 01:00 behaviour
    kDrawFrame = 2,      // the camera breakpoint handler
    kDrawBoth = 3
};

static volatile LONG g_drawRoute = kDrawFrame;

struct SelectionSnapshot
{
    // The world AABB the pick actually hit. Authoritative: the ray struck THIS
    // box, so drawing it can never disagree with what was selected. The oriented
    // fields below are a nicety; this is the truth.
    float aabbMinimum[3];
    float aabbMaximum[3];
    bool hasAabb;

    float centre[3];
    float halfExtent[3];
    float boxLeft[3];
    float boxUp[3];
    float boxForward[3];
    float origin[3];
    float left[3];
    float up[3];
    float forward[3];
    float arm;
    bool hasTransform;
    char label[320];
};

// Published by the picker, consumed by the frame handler. The sequence counter
// is odd while it is being written, so the handler can skip a torn snapshot
// instead of locking against an engine thread.
static SelectionSnapshot g_selectionSnapshot{};
static volatile LONG g_snapshotSequence = 0;
static volatile LONG g_frameDrawTicks = 0;
static volatile LONG g_frameDrawsIssued = 0;
static volatile LONG g_frameDrawLines = 0;
static const LONG kFrameDrawEvery = 6;      // ~200 traps/s down to ~33 draws/s
// coords overlay: the per-frame trap runs in OBSERVE mode (no camera / FOV write -- the player keeps the camera) and draws
// the camera position and heading on screen, so a play-tester can read off where something is.
static volatile bool g_watchObserve = false;
static volatile bool g_coordsOverlay = false;
static void DrawCoordsOnFrameThread();
static HWND FindOwnGameWindow();

// A one-shot marker the handler draws on the next frame, so the drawing pipe can
// be proved without a successful pick.
static volatile LONG g_frameTestPending = 0;
static float g_frameTestCentre[3] = { 0.0f, 0.0f, 0.0f };
static volatile float g_frameTestSize = 1.0f;

// The selection itself. Declared up here because the frame handler, which sits
// above almost everything, is one of its readers.
static uintptr_t g_selectedEntity = 0;
static char g_selectionLabel[320];   // two lines: what it is, then where
static volatile bool g_showSelection = false;   // off until asked for (showsel 1)
static volatile bool g_selectionDrawing = false;

// OX established tonight that DebugRenderer_current() picks its instance out of
// an indexed static table keyed on a per-thread context id. So the object it
// returns depends entirely on which thread asks - and ours returns a context
// nothing drains. The later vtable audit established that entry 31 was rodata,
// because the real DebugRenderer2 table ends at 27.
//
// This captures the renderer the *engine's* thread sees, from inside the
// breakpoint handler, so the two can be compared side by side.
constexpr int kCapturedSlots = 48;

static volatile LONG g_captureRenderer = 0;
static volatile LONG g_captureDone = 0;
static uintptr_t g_capturedRenderer = 0;
static uintptr_t g_capturedVtable = 0;
static uintptr_t g_capturedSlots[kCapturedSlots];
static volatile DWORD g_capturedThread = 0;

// Same story for the vertex-slot sweep: requested from a command, run on the
// engine's thread, read back afterwards.
static volatile LONG g_sweepWanted = 0;
static volatile LONG g_sweepDone = 0;

// Defined with the debug-renderer helpers, far below; the handler needs them.
static void SubmitSelectionOnFrameThread();
static void CaptureDebugRendererOnFrameThread();
static void SweepVertexSlotsOnFrameThread();

// Applies the debug registers to every thread in the process, including the ones
// that already exist - a breakpoint set only on our own thread would never fire,
// since the writer is an engine thread.
static int ArmHardwareWatch(uintptr_t address, unsigned length, bool readWrite = false)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return -1;

    THREADENTRY32 entry;
    entry.dwSize = sizeof(entry);
    const DWORD self = GetCurrentThreadId();
    const DWORD process = GetCurrentProcessId();
    int armed = 0;

    // DR7 bits: L0 enables the breakpoint; bits 16-17 = 01 for write; bits 18-19
    // encode the length (00=1, 01=2, 11=4, 10=8).
    unsigned lengthBits = 3;                 // 4 bytes
    if (length == 8) lengthBits = 2;
    else if (length == 2) lengthBits = 1;
    else if (length == 1) lengthBits = 0;

    // RW bits 16-17: 01 = write, 11 = read or write.
    const DWORD64 dr7 = 1ull | ((readWrite ? 3ull : 1ull) << 16) | (static_cast<DWORD64>(lengthBits) << 18);

    if (Thread32First(snapshot, &entry))
    {
        do
        {
            if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self)
                continue;

            HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                       THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
            if (thread == nullptr)
                continue;

            SuspendThread(thread);

            CONTEXT context{};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(thread, &context))
            {
                context.Dr0 = address;
                context.Dr6 = 0;
                context.Dr7 = address != 0 ? dr7 : 0;
                context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (SetThreadContext(thread, &context))
                    ++armed;
            }

            ResumeThread(thread);
            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return armed;
}

static LONG CALLBACK VectoredFaultHandler(EXCEPTION_POINTERS* pointers)
{
    if (pointers == nullptr || pointers->ExceptionRecord == nullptr)
        return EXCEPTION_CONTINUE_SEARCH;

    // A hardware watchpoint arrives as a single-step with a DR6 hit bit set. It
    // is not a fault: report where the write came from and carry on.
    // Any single-step, not just one with a DR6 hit bit. An unhandled single-step
    // terminates the process, and the 00:38 startup crash was exactly that -
    // code 0x80000004 at game+0x7C5F1E7, logged and passed straight on. Whatever
    // raises it, swallowing it is strictly safer than letting it through: there
    // is no debugger attached for it to be meant for.
    if (pointers->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP &&
        pointers->ContextRecord != nullptr)
    {
        const uintptr_t rip = static_cast<uintptr_t>(pointers->ContextRecord->Rip);
        const LONG hit = InterlockedIncrement(&g_watchHits);

        const bool ours = (pointers->ContextRecord->Dr6 & 0xF) != 0;
        if (!ours && hit <= 3)
        {
            char where[160];
            DescribeAddress(where, sizeof(where), rip);
            LogLine("single-step at %s with no DR6 hit - swallowed", where);
        }

        if (g_watchDrive && ours)
        {
            // The engine has just written; put ours back before the frame reads
            // it. No lock, no logging - this interrupts an engine thread.
            __try
            {
                if (g_watchWriteBase != 0 && !g_watchObserve)
                    memcpy(reinterpret_cast<void*>(g_watchWriteBase), g_watchPose,
                           kWatchWriteSize);

                if (g_fovOverride && g_fovAddress != 0 && !g_watchObserve)
                {
                    const float fov = g_fovRadians;
                    memcpy(reinterpret_cast<void*>(g_fovAddress), &fov, sizeof(fov));
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }

            if (g_sweepWanted != 0)
            {
                __try
                {
                    SweepVertexSlotsOnFrameThread();
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                }
                InterlockedExchange(&g_sweepWanted, 0);
                InterlockedExchange(&g_sweepDone, 1);
            }

            // One-shot: record which renderer this thread is handed, before any
            // drawing is attempted with it.
            if (g_captureRenderer != 0)
            {
                __try
                {
                    CaptureDebugRendererOnFrameThread();
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                }
                InterlockedExchange(&g_captureRenderer, 0);
                InterlockedExchange(&g_captureDone, 1);
            }

            if (g_coordsOverlay)
            {
                const LONG tick = InterlockedIncrement(&g_frameDrawTicks);
                if (tick % kFrameDrawEvery == 0)
                {
                    __try
                    {
                        DrawCoordsOnFrameThread();
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                    }
                }
            }

            // Then the drawing, on this thread, throttled. Separate guard: a
            // fault in the renderer must not cost us the camera write above.
            const LONG route = g_drawRoute;
            if ((route & kDrawFrame) != 0)
            {
                const LONG tick = InterlockedIncrement(&g_frameDrawTicks);
                if (tick % kFrameDrawEvery == 0 || g_frameTestPending != 0)
                {
                    __try
                    {
                        SubmitSelectionOnFrameThread();
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                    }
                }
            }

            pointers->ContextRecord->Dr6 = 0;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        if (ours && g_watchUnique)
        {
            bool seen = false;
            const LONG count = g_watchSiteCount;
            for (LONG i = 0; i < count && i < kMaxWatchSites; ++i)
                if (g_watchSites[i] == rip) { seen = true; break; }
            if (!seen && count < kMaxWatchSites)
            {
                const LONG slot = InterlockedIncrement(&g_watchSiteCount) - 1;
                if (slot < kMaxWatchSites)
                {
                    g_watchSites[slot] = rip;
                    char where[160];
                    DescribeAddress(where, sizeof(where), rip);
                    uintptr_t ret = 0;
                    SafeRead(static_cast<uintptr_t>(pointers->ContextRecord->Rsp), &ret);
                    char caller[160];
                    DescribeAddress(caller, sizeof(caller), ret);
                    LogLine("watchrw site #%ld: 0x%llX accessed by %s (thread %lu, [rsp] %s, hit %ld)",
                            slot + 1, static_cast<unsigned long long>(g_watchAddress), where,
                            GetCurrentThreadId(), caller, hit);
                }
            }
            pointers->ContextRecord->Dr6 = 0;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        if (ours && hit <= kMaxWatchHits)
        {
            char where[160];
            DescribeAddress(where, sizeof(where), rip);
            LogLine("watch #%ld: 0x%llX written from %s (thread %lu)",
                    hit, static_cast<unsigned long long>(g_watchAddress), where,
                    GetCurrentThreadId());
        }
        else if (ours && hit == kMaxWatchHits + 1 && !g_watchDrive)
        {
            LogLine("watch: enough - disarming");
            g_watchAddress = 0;
            pointers->ContextRecord->Dr7 = 0;
        }

        pointers->ContextRecord->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (FaultIsOurs(pointers->ExceptionRecord))
        return EXCEPTION_CONTINUE_SEARCH;

    const DWORD code = pointers->ExceptionRecord->ExceptionCode;

    // Routine traffic in any large C++ process - a running game raises these
    // constantly and they are not failures.
    const bool routine = code == 0xE06D7363 ||      // C++ throw
                         code == 0x406D1388 ||      // thread naming
                         code == 0x40010006 ||      // OutputDebugString
                         code == 0x000006BA ||      // RPC unavailable
                         (code & 0x10000000) != 0;  // informational range
    if (routine)
        return EXCEPTION_CONTINUE_SEARCH;

    if (InterlockedIncrement(&g_faultsLogged) <= kMaxFaultsLogged)
        ReportFault("fault", pointers);

    // Never swallow it - the engine may well handle this itself.
    return EXCEPTION_CONTINUE_SEARCH;
}

static LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;

static void FeedCrashDump();       // the logic/property feeds' last entries, see below

static LONG WINAPI TerminalFaultHandler(EXCEPTION_POINTERS* pointers)
{
    LogLine("---- process is going down ----");
    ReportFault("crash", pointers);
    FeedCrashDump();
    if (g_previousFilter != nullptr)
        return g_previousFilter(pointers);
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI FilterKeeperThread(LPVOID)
{
    for (;;)
    {
        Sleep(2000);
        LPTOP_LEVEL_EXCEPTION_FILTER current = SetUnhandledExceptionFilter(TerminalFaultHandler);
        if (current != TerminalFaultHandler)
        {
            // Somebody replaced us. Chain to them rather than dropping them.
            if (current != nullptr)
                g_previousFilter = current;
            LogLine("crash reporting re-armed (displaced 0x%p)", current);
        }
    }
}

static void InstallCrashReporting()
{
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&InstallCrashReporting), &g_selfModule);

    AddVectoredExceptionHandler(0, VectoredFaultHandler);
    g_previousFilter = SetUnhandledExceptionFilter(TerminalFaultHandler);

    HANDLE keeper = CreateThread(nullptr, 0, FilterKeeperThread, nullptr, 0, nullptr);
    if (keeper != nullptr)
        CloseHandle(keeper);

    LogLine("crash reporting installed");
}

struct FieldRec
{
    std::string name;
    uint32_t offset = 0;
    uintptr_t typePtr = 0;
};

struct TypeRec
{
    uintptr_t classInfo = 0;
    uintptr_t typeInfo = 0;
    std::string name;
    int kind = 0;
    uint32_t size = 0;
    uintptr_t parentPtr = 0;
    std::vector<FieldRec> fields;
};

static std::vector<TypeRec> g_types;
static CRITICAL_SECTION g_registryLock;
static bool g_registryLockReady = false;

// Below this the registry is still being built during startup and must not be
// cached as if it were complete.
constexpr size_t kMinimumCompleteRegistry = 20000;

static void LockRegistry()
{
    if (g_registryLockReady)
        EnterCriticalSection(&g_registryLock);
}

static void UnlockRegistry()
{
    if (g_registryLockReady)
        LeaveCriticalSection(&g_registryLock);
}
static bool g_registryWalked = false;

// The five prologue patterns Frosty's SDK generator uses to find the head of the type
// list. Pattern 1 is the one that matches this build.
static const unsigned char kPattern1[] = {
    0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x48, 0x89, 0x41, 0x08, 0x48, 0x89, 0x0D
};
static const bool kPattern1Mask[] = {
    true, true, true, false, false, false, false, true, true, true, true, true, true, true
};

static uintptr_t FindRegistryHead()
{
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t address = g_base;
    const uintptr_t end = g_base + g_size;

    while (address < end && VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == sizeof(mbi))
    {
        const bool executable =
            (mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;

        if (mbi.State == MEM_COMMIT && executable)
        {
            const unsigned char* region = reinterpret_cast<const unsigned char*>(mbi.BaseAddress);
            const size_t regionSize = mbi.RegionSize;

            for (size_t i = 0; i + sizeof(kPattern1) <= regionSize; ++i)
            {
                bool match = true;
                for (size_t j = 0; j < sizeof(kPattern1); ++j)
                {
                    if (kPattern1Mask[j] && region[i + j] != kPattern1[j])
                    {
                        match = false;
                        break;
                    }
                }
                if (!match)
                    continue;

                // mov rax,[rip+rel32] : the displacement follows the 3-byte opcode.
                const uintptr_t hit = reinterpret_cast<uintptr_t>(region + i);
                int32_t rel = 0;
                if (!SafeRead(hit + 3, &rel))
                    continue;

                const uintptr_t target = hit + 3 + 4 + rel;
                uintptr_t head = 0;
                if (!SafeRead(target, &head))
                    continue;

                if (head > 0x10000 && head < 0x00007FFFFFFFFFFFull)
                {
                    LogLine("registry head ClassInfo* = 0x%llX (pattern at 0x%llX)", head, hit);
                    return head;
                }
            }
        }

        address = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }

    return 0;
}

static void ReadTypeInfo(uintptr_t typeInfoAddr, uintptr_t classInfoAddr, TypeRec* out)
{
    unsigned char blob[kTypeInfoSize];
    if (!SafeRead(typeInfoAddr, &blob))
        return;

    uintptr_t namePtr = *reinterpret_cast<uintptr_t*>(blob + 0x00);
    uint16_t rawFlags = *reinterpret_cast<uint16_t*>(blob + 0x08);
    uint16_t flags = static_cast<uint16_t>(rawFlags >> 1);           // BFII shifts by one
    uint32_t size = *reinterpret_cast<uint32_t*>(blob + 0x0A);
    uint16_t fieldCount = *reinterpret_cast<uint16_t*>(blob + 0x22);
    uintptr_t offsets[7];
    memcpy(offsets, blob + 0x28, sizeof(offsets));

    out->classInfo = classInfoAddr;
    out->typeInfo = typeInfoAddr;
    out->name = SafeCString(namePtr);
    out->kind = (flags >> 4) & 0x1F;
    out->size = size;
    out->parentPtr = offsets[0];

    uintptr_t fieldBase = 0;
    switch (out->kind)
    {
    case 2: fieldBase = offsets[5]; break;      // Struct
    case 3: fieldBase = offsets[1]; break;      // Class
    case 8: fieldBase = offsets[0]; out->parentPtr = 0; break;   // Enum
    default: return;
    }

    if (fieldBase == 0 || fieldCount > 8192)
        return;

    for (uint16_t i = 0; i < fieldCount; ++i)
    {
        unsigned char field[kFieldInfoSize];
        if (!SafeRead(fieldBase + i * kFieldInfoSize, &field))
            break;

        FieldRec rec;
        rec.name = SafeCString(*reinterpret_cast<uintptr_t*>(field + 0x00));
        rec.offset = *reinterpret_cast<uint32_t*>(field + 0x0A);
        rec.typePtr = *reinterpret_cast<uintptr_t*>(field + 0x10);
        out->fields.push_back(rec);
    }
}

static bool WalkRegistryUnlocked()
{
    if (g_registryWalked)
        return !g_types.empty();

    const uintptr_t head = FindRegistryHead();
    if (head == 0)
    {
        // Deliberately NOT marking the walk as done: the registry may simply not
        // be constructed yet, and an early attempt must not poison every later one.
        LogLine("could not locate the registry head - will try again next time");
        return false;
    }

    g_registryWalked = true;

    std::unordered_set<uintptr_t> seen;
    uintptr_t cursor = head;
    const DWORD started = GetTickCount();

    while (cursor != 0 && g_types.size() < 200000)
    {
        if (!seen.insert(cursor).second)
            break;

        unsigned char blob[kClassInfoSize];
        if (!SafeRead(cursor, &blob))
            break;

        const uintptr_t typeInfoPtr = *reinterpret_cast<uintptr_t*>(blob + 0x00);
        const uintptr_t next = *reinterpret_cast<uintptr_t*>(blob + 0x08);

        TypeRec rec;
        ReadTypeInfo(typeInfoPtr, cursor, &rec);
        if (!rec.name.empty())
            g_types.push_back(std::move(rec));

        cursor = next;
    }

    LogLine("walked %zu types in %lu ms", g_types.size(), GetTickCount() - started);

    // A short walk means the registry was still being constructed. Caching that
    // made SpectatorFreeCamera "not exist" for a whole session.
    if (g_types.size() < kMinimumCompleteRegistry)
    {
        LogLine("registry looks incomplete (%zu types) - will walk again", g_types.size());
        g_registryWalked = false;
    }

    return !g_types.empty();
}

static bool StrContainsNoCase(const char* haystack, const char* needle)
{
    if (haystack == nullptr || needle == nullptr || *needle == '\0')
        return false;

    for (const char* start = haystack; *start != '\0'; ++start)
    {
        const char* a = start;
        const char* b = needle;
        while (*a != '\0' && *b != '\0' &&
               tolower(static_cast<unsigned char>(*a)) == tolower(static_cast<unsigned char>(*b)))
        {
            ++a;
            ++b;
        }
        if (*b == '\0')
            return true;
    }
    return false;
}

static bool WalkRegistry()
{
    LockRegistry();
    const bool ok = WalkRegistryUnlocked();
    UnlockRegistry();
    return ok;
}

static const TypeRec* FindType(const std::string& name)
{
    for (const TypeRec& type : g_types)
    {
        if (type.name == name)
            return &type;
    }
    return nullptr;
}

static const TypeRec* FindType(uintptr_t typePointer)
{
    for (const TypeRec& type : g_types)
        if (type.typeInfo == typePointer || type.classInfo == typePointer)
            return &type;
    return nullptr;
}

static const FieldRec* FindFieldInHierarchy(const TypeRec* type, const std::string& fieldName)
{
    int hops = 0;
    while (type != nullptr && hops++ < 64)
    {
        for (const FieldRec& field : type->fields)
            if (field.name == fieldName)
                return &field;

        const uintptr_t parent = type->parentPtr;
        const TypeRec* next = nullptr;
        for (const TypeRec& candidate : g_types)
        {
            if (candidate.classInfo == parent || candidate.typeInfo == parent)
            {
                next = &candidate;
                break;
            }
        }
        type = next;
    }

    return nullptr;
}

// Resolve a field on a type or any ancestor. Dotted paths walk reflected value
// structs, so FiringFunctionData::FireLogic.RateOfFire can be edited through
// the owning DataContainer even though an embedded FireLogicData has no object
// header and therefore cannot be found as a standalone live instance.
static bool ResolveField(const std::string& typeName, const std::string& fieldName,
                         uint32_t* offsetOut)
{
    const TypeRec* type = FindType(typeName);
    uint32_t offset = 0;
    size_t start = 0;

    while (type != nullptr && start < fieldName.size())
    {
        const size_t dot = fieldName.find('.', start);
        const std::string segment = fieldName.substr(start, dot - start);
        if (segment.empty()) return false;

        const FieldRec* field = FindFieldInHierarchy(type, segment);
        if (field == nullptr) return false;
        offset += field->offset;

        if (dot == std::string::npos)
        {
            *offsetOut = offset;
            return true;
        }

        type = FindType(field->typePtr);
        start = dot + 1;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Instance discovery
//
// In-process this is cheap: no ReadProcessMemory round trip per page, and the object
// layout is known - [vtable][ClassInfo*] with fields from +0x10, so a candidate is the
// address 8 bytes before a slot holding the type's ClassInfo pointer, whose vtable
// points into the module image.
// ---------------------------------------------------------------------------

// Scan one region. Raw pointers only, so the structured-exception guard is legal here.
// A region can be freed mid-scan, which is exactly what the guard is for.
static size_t ScanRegion(uintptr_t regionBase, uintptr_t regionEnd,
                         uintptr_t classInfo, uintptr_t typeInfo,
                         uintptr_t moduleBase, uintptr_t moduleEnd,
                         uintptr_t* out, size_t cap)
{
    size_t count = 0;

    __try
    {
        for (uintptr_t slot = regionBase; slot + sizeof(uintptr_t) <= regionEnd; slot += sizeof(uintptr_t))
        {
            const uintptr_t value = *reinterpret_cast<uintptr_t*>(slot);
            if (value != classInfo && value != typeInfo)
                continue;

            const uintptr_t candidate = slot - 8;      // [vtable][ClassInfo*]
            if (candidate < regionBase)
                continue;

            const uintptr_t vtable = *reinterpret_cast<uintptr_t*>(candidate);
            if (vtable < moduleBase || vtable >= moduleEnd)
                continue;

            // Require the first vtable slot to be executable. A TypeInfo record
            // also sits in the module and also carries a ClassInfo pointer, so
            // without this the registry's own records are reported as instances.
            const uintptr_t firstSlot = *reinterpret_cast<uintptr_t*>(vtable);
            if (firstSlot < moduleBase || firstSlot >= moduleEnd)
                continue;

            MEMORY_BASIC_INFORMATION slotInfo;
            if (VirtualQuery(reinterpret_cast<void*>(firstSlot), &slotInfo, sizeof(slotInfo)) !=
                sizeof(slotInfo))
                continue;

            const bool executable =
                (slotInfo.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
            if (!executable)
                continue;

            out[count++] = candidate;
            if (count >= cap)
                break;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    return count;
}

static std::vector<uintptr_t> FindInstances(const TypeRec& type, size_t cap = 64)
{
    std::vector<uintptr_t> found;
    uintptr_t batch[64];

    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t address = 0;

    while (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == sizeof(mbi))
    {
        const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t regionEnd = regionBase + mbi.RegionSize;

        const bool writable =
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        const bool usable = mbi.State == MEM_COMMIT && writable &&
                            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
                            mbi.Type == MEM_PRIVATE;

        // Do NOT cut off at 4 GiB. Live objects sit in the 3.2-4.0 GiB band and
        // above (0xEB0xxxxx, 0xFF7xxxxx, 0x10375xxxx), so the old limit dropped
        // exactly the objects we were looking for.
        if (usable && regionBase < 0x200000000ull)
        {
            const size_t room = (cap - found.size()) < 64 ? (cap - found.size()) : 64;
            const size_t got = ScanRegion(regionBase, regionEnd, type.classInfo, type.typeInfo,
                                          g_base, g_base + g_size, batch, room);
            for (size_t i = 0; i < got; ++i)
                found.push_back(batch[i]);
        }

        if (found.size() >= cap)
            break;

        address = regionEnd;
        if (address == 0)
            break;
    }

    return found;
}

// ---------------------------------------------------------------------------
// Renderer access - the realtime half
// ---------------------------------------------------------------------------

struct Transform
{
    float left[4];
    float up[4];
    float forward[4];
    float translation[4];
};

static uintptr_t GetRenderView()
{
    uintptr_t renderer = 0;
    if (!SafeRead(g_base + kGameRendererPtrRva, &renderer) || renderer == 0)
        return 0;

    uintptr_t renderView = 0;
    if (!SafeRead(renderer + kGameRendererRenderViewOffset, &renderView))
        return 0;

    return renderView;
}

// Camera override.
//
// The engine rebuilds the render view every frame from whatever camera is active, so
// holding a position means writing it back continuously. A proper implementation hooks
// the camera update; this writes at a high rate from its own thread, which is honest
// about what it is - first cut, may fight the engine on the frame boundary.
static volatile bool g_cameraOverride = false;
static volatile bool g_viewerWanted = false;   // take the camera as soon as a level loads
static Transform g_cameraTarget{};
static CRITICAL_SECTION g_cameraLock;

static bool ReadCamera(Transform* transform, float* fov)
{
    const uintptr_t view = GetRenderView();
    if (view == 0)
        return false;

    if (!SafeRead(view, transform))
        return false;

    SafeRead(view + kRenderViewFovOffset, fov);
    return true;
}

static float g_cameraYaw = 0.0f;      // degrees, around world up
static float g_cameraPitch = 0.0f;    // degrees, positive looks up

// Rotates the basis the engine already gave us. Yaw turns about world up, pitch
// about the camera's own lateral axis. Nothing is invented, so whatever
// handedness the render view uses is preserved exactly.
static void RotateAxis(float* vector, const float axis[3], float radians)
{
    const float c = cosf(radians);
    const float sn = sinf(radians);

    const float dot = vector[0] * axis[0] + vector[1] * axis[1] + vector[2] * axis[2];
    const float cross[3] = {
        axis[1] * vector[2] - axis[2] * vector[1],
        axis[2] * vector[0] - axis[0] * vector[2],
        axis[0] * vector[1] - axis[1] * vector[0]
    };

    for (int i = 0; i < 3; ++i)
        vector[i] = vector[i] * c + cross[i] * sn + axis[i] * dot * (1.0f - c);
}

static void TurnCamera(Transform* transform, float yawDegrees, float pitchDegrees)
{
    const float toRadians = 3.14159265358979f / 180.0f;

    if (yawDegrees != 0.0f)
    {
        const float worldUp[3] = { 0.0f, 1.0f, 0.0f };
        RotateAxis(transform->left, worldUp, yawDegrees * toRadians);
        RotateAxis(transform->up, worldUp, yawDegrees * toRadians);
        RotateAxis(transform->forward, worldUp, yawDegrees * toRadians);
    }

    if (pitchDegrees != 0.0f)
    {
        float axis[3] = { transform->left[0], transform->left[1], transform->left[2] };
        const float length = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
        if (length > 0.0001f)
        {
            for (int i = 0; i < 3; ++i)
                axis[i] /= length;

            // Do not pitch past vertical - the basis degenerates and the view rolls.
            const float nextY = transform->forward[1] * cosf(pitchDegrees * toRadians) +
                                (axis[2] * transform->forward[0] - axis[0] * transform->forward[2]) *
                                sinf(pitchDegrees * toRadians);
            if (nextY < 0.99f && nextY > -0.99f)
            {
                RotateAxis(transform->up, axis, pitchDegrees * toRadians);
                RotateAxis(transform->forward, axis, pitchDegrees * toRadians);
            }
        }
    }
}

static volatile bool g_flyEnabled = true;
static volatile float g_flySpeed = 40.0f;      // metres per second; shift is x5
static volatile float g_flyTurnRate = 90.0f;   // degrees per second

static bool GameHasFocus()
{
    DWORD owner = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &owner);
    return owner == GetCurrentProcessId();
}

// Declared here so the fly stepper can trigger a pick on a mouse click; the
// implementation needs the entity helpers and lives further down.
static std::string CastFromCamera(float range);
static std::string PickFromCamera(float range);
static volatile bool g_clickToSelect = false;   // off until asked for (clickselect 1, or the Inspector tab)

static volatile float g_lookSensitivity = 0.12f;   // degrees per mouse count
static volatile bool g_invertYaw = true;           // both default to the way round
static volatile bool g_invertPitch = true;         // the first flight asked for
static bool g_mouseLookActive = false;
static POINT g_mouseAnchor = { 0, 0 };

// W A S D move, Space/C up and down, right mouse to look, shift fast, alt slow.
static void StepFlyCamera(float seconds)
{
    if (!g_flyEnabled || !GameHasFocus())
    {
        g_mouseLookActive = false;
        return;
    }

    const bool fast = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool slow = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const float speed = g_flySpeed * (fast ? 5.0f : 1.0f) * (slow ? 0.2f : 1.0f);

    float turnYaw = 0.0f, turnPitch = 0.0f;

    // Left click picks whatever is under the middle of the view. Edge-triggered,
    // so holding the button does not cast every frame.
    static bool leftWasDown = false;
    const bool leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (g_clickToSelect && leftDown && !leftWasDown)
    {
        // The geometric picker, not the engine raycast. The raycast faults on
        // this route; `raypick` still runs it on demand for diagnosis.
        const std::string result = PickFromCamera(500.0f);
        LogLine("click: %s", result.c_str());
    }
    leftWasDown = leftDown;

    // Mouse look while the right button is held. The cursor is put back each
    // frame so it never runs out of screen.
    if ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0)
    {
        POINT cursor;
        if (GetCursorPos(&cursor))
        {
            if (!g_mouseLookActive)
            {
                g_mouseAnchor = cursor;
                g_mouseLookActive = true;
            }
            else
            {
                const float dx = static_cast<float>(cursor.x - g_mouseAnchor.x);
                const float dy = static_cast<float>(cursor.y - g_mouseAnchor.y);
                turnYaw += (g_invertYaw ? -dx : dx) * g_lookSensitivity;
                turnPitch += (g_invertPitch ? dy : -dy) * g_lookSensitivity;
                SetCursorPos(g_mouseAnchor.x, g_mouseAnchor.y);
            }
        }
    }
    else
    {
        g_mouseLookActive = false;
    }

    // Arrow keys still turn, for anyone without a hand on the mouse.
    if (GetAsyncKeyState(VK_LEFT) & 0x8000)  turnYaw -= g_flyTurnRate * seconds;
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) turnYaw += g_flyTurnRate * seconds;
    if (GetAsyncKeyState(VK_UP) & 0x8000)    turnPitch += g_flyTurnRate * seconds;
    if (GetAsyncKeyState(VK_DOWN) & 0x8000)  turnPitch -= g_flyTurnRate * seconds;

    float forward = 0.0f, right = 0.0f, up = 0.0f;
    if (GetAsyncKeyState('W') & 0x8000) forward += speed * seconds;
    if (GetAsyncKeyState('S') & 0x8000) forward -= speed * seconds;
    if (GetAsyncKeyState('D') & 0x8000) right += speed * seconds;
    if (GetAsyncKeyState('A') & 0x8000) right -= speed * seconds;

    if ((GetAsyncKeyState(VK_SPACE) & 0x8000) || (GetAsyncKeyState('E') & 0x8000))
        up += speed * seconds;
    if ((GetAsyncKeyState('C') & 0x8000) || (GetAsyncKeyState(VK_CONTROL) & 0x8000) ||
        (GetAsyncKeyState('Q') & 0x8000))
        up -= speed * seconds;

    if (turnYaw == 0.0f && turnPitch == 0.0f &&
        forward == 0.0f && right == 0.0f && up == 0.0f)
        return;

    EnterCriticalSection(&g_cameraLock);
    TurnCamera(&g_cameraTarget, turnYaw, turnPitch);
    for (int axis = 0; axis < 3; ++axis)
    {
        // `left` is the render view's own name for its lateral axis, so moving
        // right means subtracting it. Up is world up, not camera up - tilting
        // the view should not tilt which way "up" moves you.
        g_cameraTarget.translation[axis] +=
            g_cameraTarget.forward[axis] * forward -
            g_cameraTarget.left[axis] * right;
    }
    g_cameraTarget.translation[1] += up;
    LeaveCriticalSection(&g_cameraLock);
}

static DWORD WINAPI CameraThread(LPVOID)
{
    DWORD last = GetTickCount();

    for (;;)
    {
        if (g_cameraOverride)
        {
            const DWORD now = GetTickCount();
            float seconds = (now - last) / 1000.0f;
            last = now;
            if (seconds > 0.1f)          // a stall must not launch the camera
                seconds = 0.1f;

            StepFlyCamera(seconds);

            const uintptr_t view = GetRenderView();
            if (view != 0)
            {
                EnterCriticalSection(&g_cameraLock);
                Transform copy = g_cameraTarget;
                LeaveCriticalSection(&g_cameraLock);

                SafeWriteBytes(view, &copy, sizeof(copy));
            }
            Sleep(1);
        }
        else
        {
            last = GetTickCount();
            Sleep(16);
        }
    }
}

// ---------------------------------------------------------------------------
// Level loading
//
// The retail build ignores every command-line and cfg route into a level - that was
// established with a timed boot probe that never saw a single lookup key. The engine's
// own path is still there, though: build a LevelSetup, name it, set the GameMode
// inclusion option, and post it.
//
// Two things are deliberately blunt here. The level name is held in a static buffer
// rather than the game's memory arena, because the arena allocator's address is not
// known; if the engine takes ownership of that string this will fault, and the log
// will say which call it died in. And this assumes a server context exists - posting a
// load-level message with nothing to receive it may simply do nothing.
// ---------------------------------------------------------------------------

typedef void(__fastcall* LevelSetupInitFn)(void* self);
typedef void(__fastcall* LevelSetupSetOptionFn)(void* self, const char* key, const char* value);
typedef const char*(__fastcall* LevelSetupGetOptionFn)(void* setup, const char* key);
typedef void(__fastcall* ServerLoadLevelPostFn)(void* setup, bool fadeOut, bool forceReloadResources);

static char g_levelNameBuffer[512];
static char g_levelModeBuffer[128];

// Guards, one per engine call. They own no C++ objects, which is what lets __try
// compile, and they let the log name the exact call that faulted.
static bool GuardedInit(LevelSetupInitFn fn, void* setup)
{
    __try { fn(setup); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool GuardedGetOption(LevelSetupGetOptionFn fn, void* setup, const char* key, const char** out)
{
    __try
    {
        *out = fn(setup, key);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool GuardedSetOption(LevelSetupSetOptionFn fn, void* setup, const char* key, const char* value)
{
    __try { fn(setup, key, value); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool GuardedPost(ServerLoadLevelPostFn fn, void* setup)
{
    __try { fn(setup, true, true); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// A real loader's strings are either null or printable ASCII. Anything else is a false
// positive from the instance finder, and writing into one killed the process repeatedly.
static bool LooksLikeLoader(uintptr_t instance, uint32_t levelOff, uint32_t modeOff)
{
    const uint32_t offsets[2] = { levelOff, modeOff };

    for (uint32_t offset : offsets)
    {
        uintptr_t stringPtr = 0;
        if (!SafeRead(instance + offset, &stringPtr))
            return false;
        if (stringPtr == 0)
            continue;

        // An EMPTY string is normal and must be accepted: several real loaders ship with
        // LevelName unset. The first version of this check treated empty as unreadable
        // and rejected every genuine loader - a run wrote to 0 of 5 instances because of
        // it. Distinguish the two by reading the first byte directly.
        char first = 0;
        if (!SafeRead(stringPtr, &first))
            return false;                       // genuinely unreadable
        if (first == '\0')
            continue;                           // empty, which is fine

        const std::string text = SafeCString(stringPtr, 128);
        for (char c : text)
        {
            if (c < 0x20 || c > 0x7E)
                return false;
        }
    }

    return true;
}

// Level override, held.
//
// Writing LevelName once at the menu is not enough: the Instant Action screen writes the
// map YOU picked into the same field when you press play, so the last writer wins and it
// is not us. This thread keeps our value in place, so whatever the UI sets is replaced
// again before the load fires. Same tactic as the camera hold, same honesty about it
// being a race we win by repetition rather than by hooking.
static volatile bool g_holdLevelActive = false;
static char g_holdLevelName[512];
static char g_holdLevelMode[128];
static bool g_holdLevelHasMode = false;

static DWORD WINAPI LevelHoldThread(LPVOID)
{
    for (;;)
    {
        if (!g_holdLevelActive)
        {
            Sleep(200);
            continue;
        }

        const TypeRec* type = FindType("LoadSinglePlayerLevelEntityData");
        if (type == nullptr)
        {
            Sleep(500);
            continue;
        }

        uint32_t levelOff = 0, modeOff = 0;
        ResolveField("LoadSinglePlayerLevelEntityData", "LevelName", &levelOff);
        ResolveField("LoadSinglePlayerLevelEntityData", "GameMode", &modeOff);

        const std::vector<uintptr_t> instances = FindInstances(*type, 32);
        char* namePtr = g_holdLevelName;
        char* modePtr = g_holdLevelMode;

        int wrote = 0;
        for (uintptr_t instance : instances)
        {
            if (!LooksLikeLoader(instance, levelOff, modeOff))
                continue;

            SafeWriteBytes(instance + levelOff, &namePtr, sizeof(namePtr));
            if (g_holdLevelHasMode)
                SafeWriteBytes(instance + modeOff, &modePtr, sizeof(modePtr));
            ++wrote;
        }

        // GameSettings.Level is the other place a level name lives, and it is the one
        // Kyber writes when it wants the game pointed somewhere. The Instant Action
        // screen's own loader has an EMPTY LevelName in every dump we have taken, which
        // says the map you pick reaches the engine by some other route - so hold both.
        int wroteSettings = 0;
        const TypeRec* settings = FindType("GameSettings");
        if (settings != nullptr)
        {
            uint32_t settingsLevelOff = 0;
            if (ResolveField("GameSettings", "Level", &settingsLevelOff))
            {
                for (uintptr_t instance : FindInstances(*settings, 16))
                {
                    // Same rule: only touch one whose current value reads as a string.
                    uintptr_t current = 0;
                    if (!SafeRead(instance + settingsLevelOff, &current))
                        continue;
                    if (current != 0)
                    {
                        char first = 0;
                        if (!SafeRead(current, &first))
                            continue;
                        if (first != '\0' && (first < 0x20 || first > 0x7E))
                            continue;
                    }

                    if (SafeWriteBytes(instance + settingsLevelOff, &namePtr, sizeof(namePtr)))
                        ++wroteSettings;
                }
            }
        }

        // Throttled: one line per rescan, not per write, or the log becomes useless.
        LogLine("hold: %d loader(s), %d GameSettings holding '%s'",
                wrote, wroteSettings, g_holdLevelName);

        // Release the moment a level transition starts.
        //
        // Holding through a load is what froze the game on the Kamino loading screen:
        // the worlds are torn down for the transition (gameWorld[client] and clientLevel
        // both go to 0) and we kept rewriting the level name underneath the loader.
        // Once the transition begins our work is done - the name has already been read.
        uintptr_t clientWorldNow = 0;
        SafeRead(g_base + kGameWorldArrayRva, &clientWorldNow);
        if (clientWorldNow == 0)
        {
            LogLine("hold: level transition detected - releasing so the load can proceed");
            g_holdLevelActive = false;
            continue;
        }

        // The scan is the expensive part, so re-write the same set for a while before
        // paying for another one.
        for (int i = 0; i < 40 && g_holdLevelActive; ++i)
        {
            uintptr_t stillThere = 0;
            SafeRead(g_base + kGameWorldArrayRva, &stillThere);
            if (stillThere == 0)
            {
                LogLine("hold: level transition detected mid-cycle - releasing");
                g_holdLevelActive = false;
                break;
            }

            for (uintptr_t instance : instances)
            {
                if (LooksLikeLoader(instance, levelOff, modeOff))
                {
                    SafeWriteBytes(instance + levelOff, &namePtr, sizeof(namePtr));
                    if (g_holdLevelHasMode)
                        SafeWriteBytes(instance + modeOff, &modePtr, sizeof(modePtr));
                }
            }
            Sleep(100);
        }
    }
}

// ---------------------------------------------------------------------------
// Server spawn - the supported way into a level from a cold frontend
//
// Posting a load-level message from the frontend is not supported and kills the
// process: with m_level null the handler dereferences it ~0.6 s later. The correct
// path is to spawn a server, which initialises m_level.
//
// From the RE pass (RE-ANSWERS-OX.md B4.1/B4.2), all confirmed against two dumps:
//
//   GameSimulation vtable   RVA 0x2D53AE8
//   spawnServer             primary vtable SLOT 0 (RVA 0x18EE70)
//   instance discovery      scan the heap below 0xC0000000 for that vtable,
//                           then sanity-check tickFrequency at +0x104 and
//                           m_looping == 1 at +0x108
//   ServerSpawnInfo         LevelSetup& at +0x18 and tickFrequency at +0x20 are
//                           the only required fields; fileSystem,
//                           damageArbitrator and playerManager may be null
//
// Strings must be arena-owned: the message copies the LevelSetup by value but
// shallow-copies the char*, which then has to outlive the 0.6 s delay plus the load.
// ---------------------------------------------------------------------------

constexpr uintptr_t kGameSimVtableRva = 0x2D53AE8;
constexpr uintptr_t kMemoryArenaAllocRva = 0x541CD00;
constexpr uintptr_t kStaticArenaRva = 0x3CF74E0;      // FB_STATIC_ARENA

// Exact ServerSpawnInfo layout, total 0x58, from the engine's own
// Server\Core\ServerSpawnInfo.h (RE-ANSWERS-OX.md B5.1):
//
//   0x00 fileSystem            null OK locally
//   0x08 damageArbitrator      null OK locally
//   0x10 playerManager         null OK - the server builds one
//   0x18 levelSetup (const&)   REQUIRED, arena-owned strings
//   0x20 tickFrequency         30 matches the shipped sim
//   0x24 isSinglePlayer .. keepResources   (isDedicated at 0x26)
//   0x30 saveData              empty
//   0x38 serverCallbacks       GameSimulation implements ServerCallbacks - point
//                              it at itself rather than leaving the one field
//                              whose null behaviour is unverified
//   0x40 runtimeModules        null OK
//   0x48 loadInfo              OUT parameter, leave null
//   0x50 serverPort            0 = auto
//   0x54 validLocalPlayersMask 1 - without this the spawn has no local player
//
// The first attempt zeroed everything past 0x30 and faulted inside spawnServer.
constexpr size_t kSpawnLevelSetupPtr = 0x18;
constexpr size_t kSpawnTickFrequency = 0x20;
constexpr size_t kSpawnIsSinglePlayer = 0x24;
constexpr size_t kSpawnIsLocalHost = 0x25;
constexpr size_t kSpawnServerCallbacks = 0x38;
constexpr size_t kSpawnServerPort = 0x50;
constexpr size_t kSpawnValidLocalPlayersMask = 0x54;
constexpr size_t kSpawnSaveData = 0x30;             // OX B11.2
constexpr size_t kSpawnInfoSize = 0x58;

typedef void*(__fastcall* ArenaAllocFn)(void* arena, size_t size, size_t alignment);
typedef void(__fastcall* SpawnServerFn)(void* gameSimulation, void* spawnInfo);
typedef __int64(__fastcall* ExitServerFn)(void* gameSimulation, bool keepResources);

// GameSimulation primary vtable, decoded from the dump (RE-ANSWERS-OX.md B6.1.1):
//   [0] spawnServer   [1] restartLevel   [2] exitServer   [3] onConnectingToExternalServer
constexpr int kSlotSpawnServer = 0;
constexpr int kSlotExitServer = 2;

static char* ArenaCopy(const char* text)
{
    if (text == nullptr)
        return nullptr;

    ArenaAllocFn alloc = reinterpret_cast<ArenaAllocFn>(g_base + kMemoryArenaAllocRva);
    void* arena = reinterpret_cast<void*>(g_base + kStaticArenaRva);

    const size_t length = strlen(text) + 1;
    void* block = nullptr;

    __try
    {
        block = alloc(arena, length, 16);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }

    if (block == nullptr)
        return nullptr;

    memcpy(block, text, length);
    return static_cast<char*>(block);
}

// Walks back from a matching slot looking for the object's vptr - the first
// qword that points into the game image. That address is the likely object
// start, which is what OX needs to mark m_level.
// First occurrence of a NUL-terminated string anywhere in committed memory.
// Used to turn a level path we can see into an address we can chase.
static uintptr_t FindTextInMemory(const char* text)
{
    const size_t length = strlen(text);
    if (length == 0)
        return 0;

    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t address = 0;

    while (address < 0x200000000000ull &&
           VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == sizeof(mbi))
    {
        const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t regionEnd = regionBase + mbi.RegionSize;

        const bool readable =
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0;

        // Private committed pages only. Mapped images and shared views are most
        // of the address space and hold none of the strings we look for.
        if (mbi.State == MEM_COMMIT && readable &&
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
            mbi.Type == MEM_PRIVATE &&
            mbi.RegionSize <= 0x8000000)          // skip enormous arenas
        {
            __try
            {
                const char first = text[0];
                for (uintptr_t at = regionBase; at + length + 1 <= regionEnd; ++at)
                {
                    const char* candidate = reinterpret_cast<const char*>(at);
                    if (candidate[0] != first)
                        continue;
                    if (memcmp(candidate, text, length) != 0 || candidate[length] != '\0')
                        continue;
                    return at;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }

        if (regionEnd <= regionBase)
            break;
        address = regionEnd;
    }

    return 0;
}

static uintptr_t GuessObjectStart(uintptr_t slot, size_t maxBack)
{
    for (size_t back = 0; back <= maxBack; back += sizeof(uintptr_t))
    {
        const uintptr_t candidate = slot - back;
        uintptr_t vptr = 0;
        if (!SafeRead(candidate, &vptr))
            continue;
        if (vptr >= g_base && vptr < g_base + g_size)
            return candidate;
    }
    return 0;
}

// Scans one region for `wanted`, reporting the slot addresses that hold it.
static size_t ScanForValue(uintptr_t regionBase, uintptr_t regionEnd, uintptr_t wanted,
                           uintptr_t* out, size_t cap)
{
    size_t count = 0;

    __try
    {
        for (uintptr_t slot = regionBase; slot + sizeof(uintptr_t) <= regionEnd; slot += sizeof(uintptr_t))
        {
            if (*reinterpret_cast<uintptr_t*>(slot) != wanted)
                continue;
            out[count++] = slot;
            if (count >= cap)
                break;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    return count;
}

static std::string FindServerObject()
{
    uintptr_t ctx = 0;
    SafeRead(g_base + kServerGameContextRva, &ctx);
    if (ctx == 0)
        return "no ServerGameContext - is a server live?";

    uintptr_t playerManager = 0, peer = 0;
    SafeRead(ctx + 0x58, &playerManager);
    SafeRead(ctx + 0x60, &peer);
    if (playerManager == 0 || peer == 0)
        return "playerManager or peer is null - no live server to find";

    char header[256];
    sprintf_s(header, "ctx 0x%llX  pm 0x%llX  peer 0x%llX\n",
              static_cast<unsigned long long>(ctx),
              static_cast<unsigned long long>(playerManager),
              static_cast<unsigned long long>(peer));
    std::string out(header);

    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t address = 0;
    uintptr_t pmSlots[64];
    int reported = 0;

    while (address < 0xC0000000ull && reported < 8 &&
           VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == sizeof(mbi))
    {
        const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t regionEnd = regionBase + mbi.RegionSize;

        const bool writable =
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE)) != 0;

        if (mbi.State == MEM_COMMIT && writable &&
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 && mbi.Type == MEM_PRIVATE)
        {
            const size_t hits = ScanForValue(regionBase, regionEnd, playerManager, pmSlots, 64);
            for (size_t i = 0; i < hits && reported < 8; ++i)
            {
                const uintptr_t pmSlot = pmSlots[i];

                // The peer pointer must sit within +-0x100 of it, in the same object.
                uintptr_t peerSlot = 0;
                const uintptr_t windowLow = (pmSlot > 0x100) ? pmSlot - 0x100 : 0;
                for (uintptr_t probe = windowLow; probe <= pmSlot + 0x100; probe += sizeof(uintptr_t))
                {
                    uintptr_t value = 0;
                    if (SafeRead(probe, &value) && value == peer)
                    {
                        peerSlot = probe;
                        break;
                    }
                }
                if (peerSlot == 0)
                    continue;

                const bool isContext = (pmSlot == ctx + 0x58);

                // Reject thread stacks. Both pointers are live values, so they
                // spill into stack frames constantly and every one of those is a
                // false positive.
                if (mbi.RegionSize < 0x1000)
                    continue;

                const uintptr_t start = GuessObjectStart(pmSlot, 0x400);
                if (start == 0)
                    continue;

                uintptr_t vptr = 0;
                if (!SafeRead(start, &vptr) || vptr <= g_base || vptr >= g_base + g_size)
                    continue;

                // The image base itself turns up as a stored constant in stack
                // frames; a vtable pointer is never exactly the base.
                if (vptr == g_base)
                    continue;

                // A real object holds its members below the matching slot, not
                // above it, and the span has to be plausible for the object.
                if (start >= pmSlot || pmSlot - start > 0x400)
                    continue;

                char line[320];
                sprintf_s(line,
                          "  object 0x%llX  vptr game+0x%llX  pm at +0x%llX  peer at +0x%llX  %s\n",
                          static_cast<unsigned long long>(start),
                          static_cast<unsigned long long>(vptr - g_base),
                          static_cast<unsigned long long>(pmSlot - start),
                          static_cast<unsigned long long>(peerSlot - start),
                          isContext ? "= ServerGameContext (excluded)" : "CANDIDATE");
                out += line;

                if (!isContext)
                {
                    for (int q = 0; q < 8; ++q)
                    {
                        uintptr_t word = 0;
                        SafeRead(start + q * sizeof(uintptr_t), &word);
                        char wordLine[128];
                        sprintf_s(wordLine, "      +0x%02X = 0x%llX\n", q * 8,
                                  static_cast<unsigned long long>(word));
                        out += wordLine;
                    }
                }

                ++reported;
            }
        }

        address = regionEnd;
        if (regionEnd <= regionBase)
            break;
    }

    if (reported == 0)
        out += "  no holder found - only the context matched\n";
    return out;
}

// ---------------------------------------------------------------------------
// Riding the engine's own spawn.
//
// The early-attach init trace settled the argument. Left alone, the engine does
// this by itself during startup:
//
//     +4.5 s   clientContext
//     +9.1 s   serverContext, playerManager, peer      <- it spawns a server here
//    +14.2 s   g_gameWorld[server]                     <- and loads a level into it
//    +15.4 s   g_gameWorld[client], clientLevel
//    +35.2 s   all of it torn down, then the real frontend
//
// So there is a natural, correctly-sequenced spawnServer call ~9 seconds into
// startup, on the engine's own thread, in the engine's own phase, with a
// LevelSetup the engine built itself. Every previous attempt tried to
// manufacture that call from outside and failed on lifecycle grounds.
//
// This does not manufacture anything. It replaces slot 0 of the GameSimulation
// vtable with a thunk that rewrites the level name inside the LevelSetup the
// engine is already passing, then calls the original. The engine keeps doing
// everything else - thread, phase, teardown, streaming - exactly as it would.
// This is the position KYBER's trampoline occupies.
// ---------------------------------------------------------------------------
static void DumpLevelSetupFields(const char* tag, uintptr_t setup)
{
    const TypeRec* type = FindType("LevelSetup");
    if (type == nullptr)
    {
        LogLine("%s: LevelSetup is not in the registry", tag);
        return;
    }

    for (size_t i = 0; i < type->fields.size(); ++i)
    {
        const FieldRec& field = type->fields[i];
        uintptr_t value = 0;
        if (!SafeRead(setup + field.offset, &value))
            continue;

        char text[160] = {};
        const bool isText = value != 0 &&
                            RawReadString(text, sizeof(text), value, 159) &&
                            text[0] != '\0';

        if (isText)
            LogLine("%s   +0x%03X %-26s = 0x%llX '%s'", tag, field.offset,
                    field.name.c_str(), static_cast<unsigned long long>(value), text);
        else
            LogLine("%s   +0x%03X %-26s = 0x%llX", tag, field.offset,
                    field.name.c_str(), static_cast<unsigned long long>(value));
    }
}

static SpawnServerFn g_originalSpawnServer = nullptr;
static volatile bool g_spawnHookArmed = false;
static volatile bool g_spawnHookFired = false;
static char g_hookLevelName[512];
static char g_hookModeName[128];
static bool g_hookHasMode = false;

static void RewriteSpawnLevel(void* spawnInfo)
{
    if (spawnInfo == nullptr)
        return;

    const uintptr_t info = reinterpret_cast<uintptr_t>(spawnInfo);

    uintptr_t setup = 0;
    if (!SafeRead(info + kSpawnLevelSetupPtr, &setup) || setup == 0)
    {
        LogLine("hook: spawnInfo+0x%02zX is not a readable LevelSetup - leaving it alone",
                kSpawnLevelSetupPtr);
        return;
    }

    uintptr_t existingName = 0;
    SafeRead(setup + kLevelSetupNameOffset, &existingName);
    char existing[256] = {};
    if (existingName != 0)
        RawReadString(existing, sizeof(existing), existingName, 255);

    LogLine("hook: engine is spawning with level '%s' (setup 0x%llX)",
            existing[0] != '\0' ? existing : "(none)",
            static_cast<unsigned long long>(setup));

    char* replacement = ArenaCopy(g_hookLevelName);
    if (replacement == nullptr)
    {
        LogLine("hook: arena copy failed - the engine's own level is left in place");
        return;
    }

    DumpLevelSetupFields("hook: before", setup);

    *reinterpret_cast<char**>(setup + kLevelSetupNameOffset) = replacement;
    LogLine("hook: rewrote it to '%s'", g_hookLevelName);

    // Blank the start-point fields. They currently name the frontend's start
    // point, which does not exist in the level we just substituted, and KYBER
    // passes empty strings here for every non-campaign mode.
    static const char* const kBlankFields[] =
        { "StartPoint", "LevelManagerInitialLevel", "LevelManagerStartPoint" };
    for (int i = 0; i < 3; ++i)
    {
        uint32_t offset = 0;
        if (!ResolveField("LevelSetup", kBlankFields[i], &offset))
            continue;

        char* empty = ArenaCopy("");
        if (empty == nullptr)
            continue;

        *reinterpret_cast<char**>(setup + offset) = empty;
        LogLine("hook: blanked %s at +0x%03X", kBlankFields[i], offset);
    }

    if (g_hookHasMode)
    {
        char* mode = ArenaCopy(g_hookModeName);
        LevelSetupSetOptionFn setOption =
            reinterpret_cast<LevelSetupSetOptionFn>(g_base + kLevelSetupSetInclusionOptionRva);
        if (mode != nullptr &&
            GuardedSetOption(setOption, reinterpret_cast<void*>(setup), "GameMode", mode))
        {
            LevelSetupGetOptionFn getOption =
                reinterpret_cast<LevelSetupGetOptionFn>(g_base + kLevelSetupGetInclusionOptionRva);
            const char* stored = nullptr;
            char readBack[128] = {};
            if (GuardedGetOption(getOption, reinterpret_cast<void*>(setup), "GameMode", &stored) &&
                stored != nullptr &&
                RawReadString(readBack, sizeof(readBack), reinterpret_cast<uintptr_t>(stored), 127))
                LogLine("hook: GameMode now '%s'", readBack);
            else
                LogLine("hook: GameMode set to '%s' (no read-back)", g_hookModeName);
        }
        else
        {
            LogLine("hook: SetInclusionOption failed - the engine's own options stand");
        }
    }

    DumpLevelSetupFields("hook: after", setup);
}

// The thunk itself owns no C++ objects, so it can carry the guard directly.
static void __fastcall SpawnServerThunk(void* gameSimulation, void* spawnInfo)
{
    if (!g_spawnHookFired && g_spawnHookArmed)
    {
        g_spawnHookFired = true;
        LogLine("hook: spawnServer entered (gs=0x%p, info=0x%p) on thread %lu",
                gameSimulation, spawnInfo, GetCurrentThreadId());
        __try
        {
            RewriteSpawnLevel(spawnInfo);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LogLine("hook: faulted while rewriting - passing the call through untouched");
        }
    }

    if (g_originalSpawnServer != nullptr)
        g_originalSpawnServer(gameSimulation, spawnInfo);
}

static bool InstallSpawnHook(const char* level, const char* mode, std::string* status)
{
    if (g_spawnHookArmed)
    {
        *status = "the spawn hook is already armed";
        return false;
    }

    // Patch the shared vtable in the image rather than one instance's pointer:
    // at the time we arm this, no GameSimulation exists yet.
    const uintptr_t slot = g_base + kGameSimVtableRva;

    uintptr_t original = 0;
    if (!SafeRead(slot, &original) || original == 0)
    {
        *status = "the GameSimulation vtable slot 0 is unreadable";
        return false;
    }

    DWORD previous = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(slot), sizeof(uintptr_t),
                        PAGE_READWRITE, &previous))
    {
        *status = "could not make the vtable writable";
        return false;
    }

    g_originalSpawnServer = reinterpret_cast<SpawnServerFn>(original);
    *reinterpret_cast<uintptr_t*>(slot) = reinterpret_cast<uintptr_t>(&SpawnServerThunk);

    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(slot), sizeof(uintptr_t), previous, &ignored);

    // Walk the registry now, while there is time. The hook fires on an engine
    // thread deep in startup, and the field lookups it wants (blanking the start
    // points) need the registry already built - on the 16:05 run the dump said
    // "LevelSetup is not in the registry" and the blanking silently did nothing.
    if (!WalkRegistry())
        LogLine("hook: registry walk failed - field-name lookups will not work in the thunk");

    strncpy_s(g_hookLevelName, sizeof(g_hookLevelName), level, _TRUNCATE);
    g_hookHasMode = (mode != nullptr && *mode != '\0');
    if (g_hookHasMode)
        strncpy_s(g_hookModeName, sizeof(g_hookModeName), mode, _TRUNCATE);
    else
        g_hookModeName[0] = '\0';

    g_spawnHookFired = false;
    g_spawnHookArmed = true;

    LogLine("hook: armed - vtable slot 0 at 0x%llX now points at the thunk "
            "(original 0x%llX = game+0x%llX)",
            static_cast<unsigned long long>(slot),
            static_cast<unsigned long long>(original),
            static_cast<unsigned long long>(original - g_base));
    LogLine("hook: will rewrite the first engine spawn to '%s'%s%s", g_hookLevelName,
            g_hookHasMode ? " with GameMode " : "", g_hookHasMode ? g_hookModeName : "");

    *status = std::string("armed - the next engine spawn will load ") + level;
    return true;
}

static uintptr_t FindGameSimulation()
{
    const uintptr_t wanted = g_base + kGameSimVtableRva;

    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t address = 0;
    uintptr_t batch[8];

    while (address < 0xC0000000ull &&
           VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == sizeof(mbi))
    {
        const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t regionEnd = regionBase + mbi.RegionSize;

        const bool writable =
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE)) != 0;

        if (mbi.State == MEM_COMMIT && writable &&
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 && mbi.Type == MEM_PRIVATE)
        {
            const size_t found = ScanRegion(regionBase, regionEnd, wanted, wanted,
                                            0, ~static_cast<uintptr_t>(0), batch, 8);
            for (size_t i = 0; i < found; ++i)
            {
                // ScanRegion reports slot-8 because it expects an object header; here the
                // vtable IS at +0, so undo that.
                const uintptr_t candidate = batch[i] + 8;

                uint32_t tickFrequency = 0, looping = 0;
                if (!SafeRead(candidate + 0x104, &tickFrequency) ||
                    !SafeRead(candidate + 0x108, &looping))
                    continue;

                if (looping == 1 && tickFrequency > 0 && tickFrequency <= 1000)
                {
                    LogLine("GameSimulation at 0x%llX (tick %u, looping %u)",
                            candidate, tickFrequency, looping);
                    return candidate;
                }
            }
        }

        address = regionEnd;
        if (address == 0)
            break;
    }

    return 0;
}

static bool GuardedSpawnServer(SpawnServerFn fn, void* gs, void* info)
{
    __try { fn(gs, info); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool GuardedExitServer(ExitServerFn fn, void* gs)
{
    __try { fn(gs, false); return true; }          // keepResources = false
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The three pointers that say whether a server exists (RE-ANSWERS-OX.md B7.1).
struct ServerPulse
{
    uintptr_t playerManager;    // ctx +0x58, drains in under a second
    uintptr_t peer;             // ctx +0x60, same window
    uintptr_t world;            // g_gameWorld[1], drains over seconds

    bool operator!=(const ServerPulse& other) const
    {
        return playerManager != other.playerManager || peer != other.peer || world != other.world;
    }
};

static ServerPulse ReadServerPulse()
{
    ServerPulse pulse = { 0, 0, 0 };
    uintptr_t context = 0;
    SafeRead(g_base + kServerGameContextRva, &context);
    if (context != 0)
    {
        SafeRead(context + 0x58, &pulse.playerManager);
        SafeRead(context + 0x60, &pulse.peer);
    }
    SafeRead(g_base + kGameWorldArrayRva + sizeof(uintptr_t), &pulse.world);
    return pulse;
}

static bool ServerIsLive()
{
    const ServerPulse pulse = ReadServerPulse();
    return pulse.playerManager != 0 || pulse.peer != 0 || pulse.world != 0;
}

// The load that follows spawnServer is asynchronous and runs on engine threads.
// spawnServer returning is therefore not success - it only means the call itself
// was clean. This thread reports what happens afterwards, so a death during the
// load is timestamped against the last state the server reached rather than
// leaving the log to simply stop.
static volatile bool g_spawnWatchRunning = false;

struct SpawnWatchState
{
    ServerPulse pulse;
    uintptr_t clientWorld;

    bool operator!=(const SpawnWatchState& other) const
    {
        return pulse != other.pulse || clientWorld != other.clientWorld;
    }
};

static SpawnWatchState ReadSpawnWatchState()
{
    SpawnWatchState state{};
    state.pulse = ReadServerPulse();
    SafeRead(g_base + kGameWorldArrayRva, &state.clientWorld);
    return state;
}

static DWORD WINAPI SpawnWatchThread(LPVOID)
{
    SpawnWatchState previous = ReadSpawnWatchState();
    LogLine("watch: +0 ms pm=0x%llX peer=0x%llX server=0x%llX client=0x%llX",
            previous.pulse.playerManager, previous.pulse.peer, previous.pulse.world,
            previous.clientWorld);

    const DWORD started = GetTickCount();
    int quietSeconds = 0;

    for (int i = 0; i < 1300; ++i)
    {
        // 10 ms for the first second, 100 ms for the two minutes after it.
        Sleep(i < 100 ? 10 : 100);
        const SpawnWatchState now = ReadSpawnWatchState();
        if (now != previous)
        {
            LogLine("watch: +%lu ms pm=0x%llX peer=0x%llX server=0x%llX client=0x%llX",
                    GetTickCount() - started,
                    now.pulse.playerManager, now.pulse.peer, now.pulse.world, now.clientWorld);
            previous = now;
            quietSeconds = 0;
        }
        else if (i >= 100 && ((i - 100) % 50) == 49)
        {
            // A heartbeat every 5 s. If the log ends between heartbeats we know
            // the death happened inside that window and nothing moved before it.
            quietSeconds += 5;
            LogLine("watch: +%lu ms unchanged (%d s quiet)", GetTickCount() - started, quietSeconds);
        }
    }

    LogLine("watch: finished - pm=0x%llX peer=0x%llX server=0x%llX client=0x%llX",
            previous.pulse.playerManager, previous.pulse.peer, previous.pulse.world,
            previous.clientWorld);
    g_spawnWatchRunning = false;
    return 0;
}

static void StartSpawnWatch()
{
    if (g_spawnWatchRunning)
        return;
    g_spawnWatchRunning = true;
    HANDLE thread = CreateThread(nullptr, 0, SpawnWatchThread, nullptr, 0, nullptr);
    if (thread != nullptr)
        CloseHandle(thread);
    else
        g_spawnWatchRunning = false;
}

static void DumpBlock(const char* label, uintptr_t address, size_t bytes)
{
    for (size_t offset = 0; offset < bytes; offset += 16)
    {
        char text[128];
        int written = sprintf_s(text, "%s+0x%03zX ", label, offset);
        bool any = false;
        for (size_t i = 0; i < 16 && offset + i < bytes; ++i)
        {
            unsigned char byte = 0;
            if (SafeRead(address + offset + i, &byte))
            {
                written += sprintf_s(text + written, sizeof(text) - written, "%02X ", byte);
                any = true;
            }
            else
            {
                written += sprintf_s(text + written, sizeof(text) - written, "?? ");
            }
        }
        if (!any)
        {
            LogLine("%s+0x%03zX unreadable", label, offset);
            return;
        }
        LogLine("%s", text);
    }
}

static bool SpawnServerWithLevel(const char* level, const char* mode, std::string* status)
{
    // Warm the reflection registry before anything is torn down. It is cached, so
    // the field dump after Init costs nothing and the teardown-to-spawn window
    // stays as narrow as it was before the dump existed.
    if (!WalkRegistry())
        LogLine("spawn: registry walk failed - the LevelSetup dump will be skipped");

    const uintptr_t gs = FindGameSimulation();
    if (gs == 0)
    {
        *status = "could not find the GameSimulation instance";
        return false;
    }

    // Stop the existing server first.
    //
    // The idle frontend runs a LIVE server with a peer and a player manager but no
    // level. Spawning a second one over it is what faulted; and posting a load into it
    // is the null-m_level branch that kills the process 0.6 s later. The game's own
    // answer is neither - it stops and respawns, and m_level is only ever set inside
    // create(). So: exitServer, wait for teardown, then spawn fresh.
    uintptr_t vtableForExit = 0;
    if (SafeRead(gs, &vtableForExit) && vtableForExit != 0 && ServerIsLive())
    {
        uintptr_t exitFn = 0;
        if (SafeRead(vtableForExit + kSlotExitServer * sizeof(uintptr_t), &exitFn) && exitFn != 0)
        {
            const ServerPulse before = ReadServerPulse();

            // Capture GameSimulation before the teardown; compared against the
            // post-teardown state below, the qwords that go non-null -> null
            // include m_server.
            static uintptr_t gsBefore[64];
            const int gsWords = 64;
            for (int q = 0; q < gsWords; ++q)
            {
                gsBefore[q] = 0;
                SafeRead(gs + 0x38 + q * sizeof(uintptr_t), &gsBefore[q]);
            }

            LogLine("spawn: a server is live (pm=0x%llX peer=0x%llX world=0x%llX) - calling exitServer (0x%llX)",
                    before.playerManager, before.peer, before.world, exitFn);
            if (!GuardedExitServer(reinterpret_cast<ExitServerFn>(exitFn), reinterpret_cast<void*>(gs)))
            {
                *status = "faulted inside exitServer";
                LogLine("spawn: FAULT in exitServer");
                return false;
            }

            // Sample fast and spawn on the first drained reading. The frontend
            // brings its own server back up shortly after an exit, so the window
            // where nothing is live can be brief - a coarse poll walks straight
            // past it and then reports a timeout that never was.
            //
            // playerManager and peer are the teardown signal; the server world
            // trails them, so it is logged but does not gate.
            //
            // OX B9.2: one drained reading is not enough. Teardown releases the
            // level, then the world join, then the player manager, then the peer -
            // and a restart transient can show all three at zero for a single
            // sample while the engine is still unwinding. Require three
            // consecutive clear samples, and gate on the server world too: it
            // was observed clearing FIRST here (+20 ms), ahead of pm and peer,
            // so it costs nothing to include and it catches a half-torn state.
            ServerPulse previous = before;
            bool down = false;
            int clearRun = 0;
            for (int i = 0; i < 3000 && !down; ++i)     // up to 30 s at 10 ms
            {
                Sleep(10);
                const ServerPulse now = ReadServerPulse();
                if (now != previous)
                {
                    LogLine("spawn:   +%d ms pm=0x%llX peer=0x%llX world=0x%llX",
                            (i + 1) * 10, now.playerManager, now.peer, now.world);
                    previous = now;
                }

                if (now.playerManager == 0 && now.peer == 0 && now.world == 0)
                    ++clearRun;
                else
                    clearRun = 0;

                down = (clearRun >= 3);
            }

            if (!down)
            {
                // OX B7.1: if the exit never drains, the exit path is wedged and
                // spawning on top of it is the fault we already know about.
                *status = "exitServer never drained - not spawning";
                LogLine("spawn: teardown TIMED OUT after 30 s (pm=0x%llX peer=0x%llX world=0x%llX) - ABORTING",
                        previous.playerManager, previous.peer, previous.world);
                return false;
            }

            LogLine("spawn: teardown complete - 3 consecutive clear samples (world=0x%llX)",
                    previous.world);

            // GameSimulation owns the Server object itself; tearing THAT down is a
            // different failure from tearing down the level. Report only what the
            // teardown actually changed - the whole block is noise, the diff is
            // the answer.
            int changes = 0;
            for (int q = 0; q < gsWords; ++q)
            {
                uintptr_t now = 0;
                SafeRead(gs + 0x38 + q * sizeof(uintptr_t), &now);
                if (now == gsBefore[q])
                    continue;

                LogLine("spawn:   gs+0x%03X  0x%llX -> 0x%llX%s",
                        0x38 + q * 8, gsBefore[q], now,
                        (gsBefore[q] != 0 && now == 0) ? "   <- released" : "");
                ++changes;
            }
            if (changes == 0)
                LogLine("spawn:   gs+0x38..0x%03X unchanged across the teardown",
                        0x38 + gsWords * 8);
        }
    }
    else
    {
        LogLine("spawn: no live server to stop");
    }

    char* levelName = ArenaCopy(level);
    if (levelName == nullptr)
    {
        *status = "arena allocation failed for the level name";
        return false;
    }

    char* modeName = (mode != nullptr && *mode != '\0') ? ArenaCopy(mode) : nullptr;

    alignas(16) static unsigned char setupStorage[1024];
    memset(setupStorage, 0, sizeof(setupStorage));
    void* setup = setupStorage;

    LevelSetupInitFn init = reinterpret_cast<LevelSetupInitFn>(g_base + kLevelSetupInitRva);
    LevelSetupSetOptionFn setOption =
        reinterpret_cast<LevelSetupSetOptionFn>(g_base + kLevelSetupSetInclusionOptionRva);

    LogLine("spawn: LevelSetup::Init");
    if (!GuardedInit(init, setup))
    {
        *status = "faulted in LevelSetup::Init";
        return false;
    }

    *reinterpret_cast<char**>(reinterpret_cast<uintptr_t>(setup) + kLevelSetupNameOffset) = levelName;
    LogLine("spawn: name '%s' (arena 0x%p)", levelName, levelName);

    // Log what Init actually produced, so a null that matters is visible rather
    // than inferred. Then give the two known string fields an arena-owned empty
    // string if Init left them null (OX B10.2 item 2).
    if (WalkRegistry())
    {
        const TypeRec* setupType = FindType("LevelSetup");
        if (setupType != nullptr)
        {
            LogLine("spawn: LevelSetup has %zu fields, size 0x%X",
                    setupType->fields.size(), setupType->size);
            for (size_t i = 0; i < setupType->fields.size(); ++i)
            {
                const FieldRec& field = setupType->fields[i];
                uintptr_t value = 0;
                SafeRead(reinterpret_cast<uintptr_t>(setup) + field.offset, &value);
                LogLine("spawn:   +0x%03X %-28s = 0x%llX", field.offset, field.name.c_str(),
                        static_cast<unsigned long long>(value));
            }
        }
        else
        {
            LogLine("spawn: LevelSetup is not in the registry - cannot check its fields");
        }

        static const char* const kStringFields[] = { "InitialStartPoint", "InitialDSubLevel" };
        for (int i = 0; i < 2; ++i)
        {
            uint32_t offset = 0;
            if (!ResolveField("LevelSetup", kStringFields[i], &offset))
            {
                LogLine("spawn: no field LevelSetup::%s in the registry", kStringFields[i]);
                continue;
            }

            uintptr_t existing = 0;
            SafeRead(reinterpret_cast<uintptr_t>(setup) + offset, &existing);
            if (existing != 0)
            {
                LogLine("spawn: %s already set (0x%llX) - left alone", kStringFields[i],
                        static_cast<unsigned long long>(existing));
                continue;
            }

            char* empty = ArenaCopy("");
            if (empty == nullptr)
            {
                LogLine("spawn: arena allocation failed for %s", kStringFields[i]);
                continue;
            }

            *reinterpret_cast<char**>(reinterpret_cast<uintptr_t>(setup) + offset) = empty;
            LogLine("spawn: %s = arena \"\" at +0x%03X", kStringFields[i], offset);
        }
    }
    else
    {
        LogLine("spawn: registry walk failed - LevelSetup fields left as Init produced them");
    }

    if (modeName != nullptr)
    {
        LogLine("spawn: GameMode '%s'", modeName);
        if (!GuardedSetOption(setOption, setup, "GameMode", modeName))
        {
            *status = "faulted in SetInclusionOption";
            return false;
        }

        // Read it straight back. Whether the pair actually stored has been an
        // open question across three runs; the getter settles it (OX B11.1.1).
        LevelSetupGetOptionFn getOption =
            reinterpret_cast<LevelSetupGetOptionFn>(g_base + kLevelSetupGetInclusionOptionRva);
        const char* stored = nullptr;
        if (!GuardedGetOption(getOption, setup, "GameMode", &stored))
        {
            LogLine("spawn: GetInclusionOption faulted - read-back unavailable");
        }
        else if (stored == nullptr)
        {
            LogLine("spawn: read-back says GameMode is NOT SET");
        }
        else
        {
            char readBack[128] = {};
            if (RawReadString(readBack, sizeof(readBack), reinterpret_cast<uintptr_t>(stored), 127))
                LogLine("spawn: read-back GameMode = '%s'", readBack);
            else
                LogLine("spawn: read-back GameMode pointer 0x%p is unreadable", stored);
        }
    }

    alignas(16) static unsigned char spawnInfo[0x80];
    memset(spawnInfo, 0, sizeof(spawnInfo));
    *reinterpret_cast<void**>(spawnInfo + kSpawnLevelSetupPtr) = setup;
    *reinterpret_cast<uint32_t*>(spawnInfo + kSpawnTickFrequency) = 30;
    spawnInfo[kSpawnIsSinglePlayer] = 1;
    spawnInfo[kSpawnIsLocalHost] = 1;
    *reinterpret_cast<void**>(spawnInfo + kSpawnServerCallbacks) = reinterpret_cast<void*>(gs);
    *reinterpret_cast<uint32_t*>(spawnInfo + kSpawnServerPort) = 0;          // auto
    *reinterpret_cast<uint32_t*>(spawnInfo + kSpawnValidLocalPlayersMask) = 1;

    // saveData is an array member, and a zeroed array member is not an empty
    // array - it is a null where a header is expected. The engine's empty array
    // is a pointer just past a zeroed, aligned block so the count at data[-1]
    // reads 0 (OX B11.2).
    static __declspec(align(16)) uint32_t s_emptyArray[8] = { 0 };
    *reinterpret_cast<void**>(spawnInfo + kSpawnSaveData) = &s_emptyArray[4];
    LogLine("spawn: saveData +0x%02zX = empty-array sentinel 0x%p",
            kSpawnSaveData, &s_emptyArray[4]);

    LogLine("spawn: info size 0x%zX, callbacks=0x%llX, localPlayers=1",
            kSpawnInfoSize, static_cast<unsigned long long>(gs));

    uintptr_t vtable = 0;
    if (!SafeRead(gs, &vtable) || vtable == 0)
    {
        *status = "GameSimulation vtable unreadable";
        return false;
    }

    uintptr_t spawnFn = 0;
    if (!SafeRead(vtable, &spawnFn) || spawnFn == 0)
    {
        *status = "spawnServer slot 0 unreadable";
        return false;
    }

    g_spawnThreadId = GetCurrentThreadId();
    LogLine("spawn: inputs as handed to spawnServer -");
    DumpBlock("setup", reinterpret_cast<uintptr_t>(setup), 0x40);
    DumpBlock("info", reinterpret_cast<uintptr_t>(spawnInfo), 0x58);

    LogLine("spawn: calling GameSimulation::spawnServer (0x%llX) on 0x%llX from thread %lu",
            spawnFn, gs, GetCurrentThreadId());
    if (!GuardedSpawnServer(reinterpret_cast<SpawnServerFn>(spawnFn),
                            reinterpret_cast<void*>(gs), spawnInfo))
    {
        *status = "faulted inside spawnServer";
        LogLine("spawn: FAULT on our own thread (%lu) - state after the call:",
                GetCurrentThreadId());
        DumpBlock("info", reinterpret_cast<uintptr_t>(spawnInfo), 0x58);
        DumpBlock("gs", gs, 0x60);
        DumpBlock("b10obj", g_base + 0x3DCA7B0, 0x20);
        return false;
    }

    LogLine("spawn: returned - watching the load");
    StartSpawnWatch();
    *status = std::string("spawned a server on ") + level + " - see the watch lines in the log";
    return true;
}

static bool WorldExists()
{
    uintptr_t clientWorld = 0, serverWorld = 0;
    SafeRead(g_base + kGameWorldArrayRva, &clientWorld);
    SafeRead(g_base + kGameWorldArrayRva + sizeof(uintptr_t), &serverWorld);
    return clientWorld != 0 || serverWorld != 0;
}

static bool LoadLevel(const char* level, const char* mode, std::string* status)
{
    if (g_base == 0)
    {
        *status = "module base unknown";
        return false;
    }

    // Precondition, learned the hard way: posting a load into a process that is still
    // booting killed it. The module was injected one second after the process appeared,
    // the level-load message went into an engine with no world, and the process died -
    // the log ends at "loadlevel: returned" with nothing after it.
    //
    // The guard lives here rather than in the caller so that every caller inherits it.
    if (!WorldExists())
    {
        *status = "refused: no world exists yet (gameWorld[client] and [server] are both "
                  "null) - the game is still booting. Wait for the frontend, then retry.";
        LogLine("loadlevel: REFUSED, no world yet");
        return false;
    }

    // No mode means NO game mode - just the level.
    //
    // This is the distinction that matters for a viewer. Setting a GameMode inclusion
    // option is what turns a level load into a match: bots, HUD, round logic. The
    // reference tool's output shows none of that - no HUD, no scoreboard, just the world
    // and a log listing each sublevel as it streams in. So an empty mode skips the
    // inclusion option entirely, which is also what the engine's own path does when it
    // is handed no mode.
    const bool wantMode = (mode != nullptr && *mode != '\0');

    strncpy_s(g_levelNameBuffer, level, _TRUNCATE);
    strncpy_s(g_levelModeBuffer, wantMode ? mode : "", _TRUNCATE);

    LevelSetupInitFn init =
        reinterpret_cast<LevelSetupInitFn>(g_base + kLevelSetupInitRva);
    LevelSetupSetOptionFn setOption =
        reinterpret_cast<LevelSetupSetOptionFn>(g_base + kLevelSetupSetInclusionOptionRva);
    ServerLoadLevelPostFn post =
        reinterpret_cast<ServerLoadLevelPostFn>(g_base + kServerLoadLevelMessagePostRva);

    // Generous and zeroed: the real structure is under 0x100 bytes.
    alignas(16) static unsigned char setupStorage[1024];
    memset(setupStorage, 0, sizeof(setupStorage));
    void* setup = setupStorage;

    LogLine("loadlevel: '%s' mode '%s'", g_levelNameBuffer, g_levelModeBuffer);

    // Each engine call is guarded separately. A fault inside one of these would
    // otherwise take the whole game down and tell us nothing; this way the log names
    // the call and the process usually survives to be asked again.
    LogLine("loadlevel: calling LevelSetup::Init");
    if (!GuardedInit(init, setup))
    {
        *status = "faulted inside LevelSetup::Init";
        LogLine("loadlevel: FAULT in Init");
        return false;
    }

    LogLine("loadlevel: setting Name");
    *reinterpret_cast<char**>(reinterpret_cast<uintptr_t>(setup) + kLevelSetupNameOffset) =
        g_levelNameBuffer;

    if (wantMode)
    {
        LogLine("loadlevel: setting GameMode inclusion option '%s'", g_levelModeBuffer);
        if (!GuardedSetOption(setOption, setup, "GameMode", g_levelModeBuffer))
        {
            *status = "faulted inside SetInclusionOption";
            LogLine("loadlevel: FAULT in SetInclusionOption");
            return false;
        }
    }
    else
    {
        LogLine("loadlevel: no game mode - loading the level only");
    }

    LogLine("loadlevel: posting the load-level message");
    if (!GuardedPost(post, setup))
    {
        *status = "faulted inside ServerLoadLevelMessage_post - most likely no server "
                  "context exists yet";
        LogLine("loadlevel: FAULT in post");
        return false;
    }

    LogLine("loadlevel: returned");
    *status = std::string("posted load for ") + g_levelNameBuffer +
              (wantMode ? std::string(" mode ") + g_levelModeBuffer : std::string(" (no game mode)"));
    return true;
}

// ---------------------------------------------------------------------------
// Entity enumeration and transforms
//
// Offsets below are confirmed against this build by the RE pass (RE-ANSWERS-OX.md
// B2.3, B2.1, S3), which read them out of a full-process dump:
//
//   GameWorld  +0x28  root SubLevel
//   SubLevel   +0x70  child        +0x78  sibling       +0x80  root EntityBus
//   EntityOwner+0x10  first list node; entity = node - 8; next = node + 0x08
//   EntityOwner+0x28  first child owner; +0x30 next sibling owner
//   NativeEntity +0x28 / SpatialEntity +0x38 data (exported GUID at data-0x10)
//   data       -0x10  instance GUID, when *(u16*)(data+0x14) & 0x0100
//   Entity vtable slot 27 = getTransform(out), slot 28 = setTransform(in)
//
// The transform must go through slot 28, never a raw write: the setter queues the
// component/bounds/physics propagation, and writing the storage directly moves the
// mesh while leaving culling, the spatial-query grid and collision behind.
// ---------------------------------------------------------------------------

// Exact BFII Kyber layout and its working Lua entity enumerator agree:
// GameWorld+0x20 is m_lastRemovedEntity and GameWorld+0x28 is m_rootLevel.
// Treating a non-null +0x20 as a SubLevel explains the old 13-object result: it
// walked a removed-entity pointer as though it were the live owner tree.
constexpr size_t kGameWorldRootSubLevel = 0x28;
constexpr size_t kSubLevelChild = 0x70;
constexpr size_t kSubLevelSibling = 0x78;
constexpr size_t kSubLevelRootBus = 0x80;
constexpr size_t kOwnerFirstNode = 0x10;
constexpr size_t kNodeNext = 0x08;
constexpr size_t kOwnerFirstChild = 0x28;
constexpr size_t kOwnerNextSibling = 0x30;
constexpr size_t kNativeEntityData = 0x28;
constexpr size_t kSpatialEntityData = 0x38;
constexpr size_t kDataAssetName = 0x18;
constexpr size_t kDataFlags = 0x14;
constexpr uint16_t kHasGuidFlag = 0x0100;

constexpr int kSlotGetTransform = 27;
constexpr int kSlotSetTransform = 28;

// Exact BFII Kyber SDK.cpp/SDK.h: EntityBase::IsSpatial calls
// ClassInfo_isKindOf(getType(), typeInfo_SpatialEntity). NativeEntity and
// SpatialEntity have different bus/data offsets.
constexpr uintptr_t kClassInfoIsKindOfRva = 0x5440530;
constexpr uintptr_t kSpatialEntityTypeInfoRva = 0x456DB60;

struct EntityRow
{
    uintptr_t entity = 0;
    std::string name;
    std::string ownerAssetPath;
    std::string instanceGuid;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    bool hasTransform = false;
};

typedef Transform*(__fastcall* GetTransformFn)(void* entity, Transform* out);
typedef void(__fastcall* SetTransformFn)(void* entity, const Transform* in);
typedef bool(__fastcall* ClassInfoIsKindOfFn)(const void* type, const void* other);

static uintptr_t GuardedGetType(uintptr_t fn, uintptr_t entity);
static std::string EntityDataType(uintptr_t entity);
static std::string SubLevelName(uintptr_t subLevel);

static bool CallVirtual(uintptr_t object, int slot, uintptr_t* fnOut)
{
    uintptr_t vtable = 0;
    if (!SafeRead(object, &vtable) || vtable == 0)
        return false;
    return SafeRead(vtable + static_cast<size_t>(slot) * sizeof(uintptr_t), fnOut) && *fnOut != 0;
}

static bool TryEntityIsSpatial(uintptr_t entity, bool* spatial)
{
    if (spatial == nullptr) return false;
    *spatial = false;
    uintptr_t getType = 0;
    if (!CallVirtual(entity, 0, &getType))
        return false;
    const uintptr_t type = GuardedGetType(getType, entity);
    if (type == 0)
        return false;

    __try
    {
        ClassInfoIsKindOfFn isKindOf = reinterpret_cast<ClassInfoIsKindOfFn>(
            g_base + kClassInfoIsKindOfRva);
        *spatial = isKindOf(reinterpret_cast<const void*>(type),
                            reinterpret_cast<const void*>(g_base + kSpatialEntityTypeInfoRva));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool EntityIsSpatial(uintptr_t entity)
{
    bool spatial = false;
    return TryEntityIsSpatial(entity, &spatial) && spatial;
}

static bool ReadEntityData(uintptr_t entity, uintptr_t* data)
{
    if (data == nullptr) return false;
    bool spatial = false;
    if (!TryEntityIsSpatial(entity, &spatial)) return false;
    const size_t offset = spatial ? kSpatialEntityData : kNativeEntityData;
    return SafeRead(entity + offset, data) && *data != 0;
}

static std::string EntityInstanceGuid(uintptr_t entity)
{
    uintptr_t data = 0;
    uint16_t flags = 0;
    if (!ReadEntityData(entity, &data) || data < 0x10 ||
        !SafeRead(data + kDataFlags, &flags) || (flags & kHasGuidFlag) == 0)
        return std::string();

    GUID guid{};
    if (!SafeRead(data - 0x10, &guid))
        return std::string();
    char text[37] = {};
    sprintf_s(text, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
              guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
              guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
              guid.Data4[6], guid.Data4[7]);
    return text;
}

// getTransform without the isKindOf veto.
//
// EntityIsSpatial tests kSpatialEntityTypeInfoRva, which has never been
// verified against this build. ReadEntityBounds already records that the veto
// fired on ALL 1,903 cached objects on the 00:30 run, and works around it for
// computeBoundsInfo - but GuardedGetTransform still opens with the same check,
// so every transform read failed too. That is why the 1.5 m fallback boxes in
// the pick cache never appeared: their guard returned false before the virtual
// was ever called.
//
// The structured-exception guard below is the real safety net. A wrong slot
// faults and is caught; an unverified type check should not get a veto over a
// virtual that might work.
static bool GuardedGetTransformUnchecked(uintptr_t entity, Transform* out)
{
    uintptr_t fn = 0;
    if (!CallVirtual(entity, kSlotGetTransform, &fn))
        return false;

    __try
    {
        reinterpret_cast<GetTransformFn>(fn)(reinterpret_cast<void*>(entity), out);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    // A transform that came back as zeros or NaN is not usable even though the
    // call returned; treat it as a failure rather than boxing at the origin.
    bool anyNonZero = false;
    for (int axis = 0; axis < 3; ++axis)
    {
        const float v = out->translation[axis];
        if (!(v == v))
            return false;
        if (v != 0.0f)
            anyNonZero = true;
    }
    return anyNonZero;
}

static bool GuardedGetTransform(uintptr_t entity, Transform* out)
{
    if (!EntityIsSpatial(entity))
        return false;

    uintptr_t fn = 0;
    if (!CallVirtual(entity, kSlotGetTransform, &fn))
        return false;

    __try
    {
        reinterpret_cast<GetTransformFn>(fn)(reinterpret_cast<void*>(entity), out);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool GuardedSetTransform(uintptr_t entity, const Transform* in)
{
    if (!EntityIsSpatial(entity))
        return false;

    uintptr_t fn = 0;
    if (!CallVirtual(entity, kSlotSetTransform, &fn))
        return false;

    __try
    {
        reinterpret_cast<SetTransformFn>(fn)(reinterpret_cast<void*>(entity), in);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static std::string EntityDisplayName(uintptr_t entity)
{
    // GameObjectData is not Asset: +0x18 is DataBusPeer::Flags, not Asset::Name.
    // The owning EBX path is resolved in the plugin from the exported instance
    // GUID; until then the truthful native fallback is the reflected data type.
    return EntityDataType(entity);
}

// The reference editor's label is two lines: the asset path on top, the runtime
// class underneath - "Objects/Props/Landmarks/.../ImperialShuttleTydirium_01_Landmark"
// over "ClientStaticModelEntity". These split EntityDisplayName's two halves so
// both can be shown at once instead of one falling back to the other.
static std::string EntityAssetPath(uintptr_t entity)
{
    (void)entity;
    return std::string();
}

// The runtime class, read off the object's own vtable through the registry's
// ClassInfo rather than the data container - that is what names a
// ClientStaticModelEntity as one.
static std::string EntityDataType(uintptr_t entity)
{
    uintptr_t data = 0;
    if (ReadEntityData(entity, &data))
    {
        uintptr_t typeInfo = 0;
        if (SafeRead(data + 0x08, &typeInfo) && typeInfo != 0)
        {
            uintptr_t typeInfoData = 0;
            if (SafeRead(typeInfo, &typeInfoData) && typeInfoData != 0)
            {
                uintptr_t typeNamePtr = 0;
                if (SafeRead(typeInfoData, &typeNamePtr))
                {
                    const std::string name = SafeCString(typeNamePtr, 128);
                    if (!name.empty())
                        return name;
                }
            }
        }
    }

    return std::string();
}

static uintptr_t GuardedGetType(uintptr_t fn, uintptr_t entity)
{
    __try
    {
        return reinterpret_cast<uintptr_t(__fastcall*)(const void*)>(fn)(
            reinterpret_cast<const void*>(entity));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// TypeObject's first virtual is getType(). Kyber's exact BFII SDK declares that
// contract; calling it gives the entity class (ClientStaticModelEntity), whereas
// EntityDataType above names the EBX-backed StaticModelEntityData.
static std::string EntityRuntimeClass(uintptr_t entity)
{
    uintptr_t fn = 0;
    if (!CallVirtual(entity, 0, &fn))
        return std::string();

    const uintptr_t typeInfo = GuardedGetType(fn, entity);
    if (typeInfo == 0)
        return std::string();

    uintptr_t typeInfoData = 0;
    uintptr_t namePtr = 0;
    if (!SafeRead(typeInfo, &typeInfoData) || typeInfoData == 0 ||
        !SafeRead(typeInfoData, &namePtr) || namePtr == 0)
        return std::string();
    return SafeCString(namePtr, 128);
}

static void CollectOwnedEntities(uintptr_t owner, const std::string& ownerAssetPath,
                                 std::vector<EntityRow>* rows, size_t cap)
{
    uintptr_t node = 0;
    if (!SafeRead(owner + kOwnerFirstNode, &node))
        return;

    int guard = 0;
    while (node != 0 && rows->size() < cap && guard++ < 8192)
    {
        const uintptr_t entity = node - 8;

        EntityRow row;
        row.entity = entity;
        row.ownerAssetPath = ownerAssetPath;
        row.name = EntityDisplayName(entity);
        row.instanceGuid = EntityInstanceGuid(entity);
        const std::string runtimeClass = EntityRuntimeClass(entity);
        if (!runtimeClass.empty())
        {
            if (!row.name.empty()) row.name += " ";
            row.name += runtimeClass;
        }
        if (!row.ownerAssetPath.empty())
            row.name = row.ownerAssetPath + (row.name.empty() ? "" : " ") + row.name;

        Transform transform{};
        if (GuardedGetTransform(entity, &transform))
        {
            row.hasTransform = true;
            row.x = transform.translation[0];
            row.y = transform.translation[1];
            row.z = transform.translation[2];
        }

        if (!row.name.empty())
            rows->push_back(row);

        if (!SafeRead(node + kNodeNext, &node))
            break;
    }
}

static void CollectOwnedEntitiesRecursively(uintptr_t owner, std::vector<EntityRow>* rows,
                                            size_t cap, int depth,
                                            std::unordered_set<uintptr_t>* visitedOwners,
                                            const std::string& ownerAssetPath)
{
    if (owner == 0 || rows->size() >= cap || depth > 32 ||
        !visitedOwners->insert(owner).second)
        return;

    CollectOwnedEntities(owner, ownerAssetPath, rows, cap);

    uintptr_t child = 0;
    if (SafeRead(owner + kOwnerFirstChild, &child) && child != 0)
        CollectOwnedEntitiesRecursively(child, rows, cap, depth + 1, visitedOwners,
                                        ownerAssetPath);

    uintptr_t sibling = 0;
    if (SafeRead(owner + kOwnerNextSibling, &sibling) && sibling != 0)
        CollectOwnedEntitiesRecursively(sibling, rows, cap, depth, visitedOwners,
                                        ownerAssetPath);
}

static void WalkSubLevels(uintptr_t subLevel, std::vector<EntityRow>* rows, size_t cap, int depth)
{
    if (subLevel == 0 || depth > 16 || rows->size() >= cap)
        return;

    // SubLevel derives from EntityOwner. Its +0x10 chain contains the owned
    // entities; nested prefab/reference-object instances hang below child
    // EntityOwners. EntityBus routes connections and is not an EntityOwner.
    std::unordered_set<uintptr_t> visitedOwners;
    const std::string ownerAssetPath = SubLevelName(subLevel);
    CollectOwnedEntitiesRecursively(subLevel, rows, cap, 0, &visitedOwners, ownerAssetPath);

    uintptr_t child = 0;
    if (SafeRead(subLevel + kSubLevelChild, &child) && child != 0)
        WalkSubLevels(child, rows, cap, depth + 1);

    uintptr_t sibling = 0;
    if (SafeRead(subLevel + kSubLevelSibling, &sibling) && sibling != 0)
        WalkSubLevels(sibling, rows, cap, depth);
}

static std::vector<EntityRow> EnumerateEntities(size_t cap)
{
    std::vector<EntityRow> rows;

    uintptr_t clientWorld = 0;
    if (!SafeRead(g_base + kGameWorldArrayRva, &clientWorld) || clientWorld == 0)
        return rows;

    uintptr_t rootSubLevel = 0;
    if (!SafeRead(clientWorld + kGameWorldRootSubLevel, &rootSubLevel) || rootSubLevel == 0)
        return rows;

    WalkSubLevels(rootSubLevel, &rows, cap, 0);

    // The sub-level and its bus can name the same entity, and a prefab can be
    // reached down two branches. Keep the first of each.
    std::vector<EntityRow> unique;
    unique.reserve(rows.size());
    std::unordered_set<uintptr_t> seen;
    for (size_t i = 0; i < rows.size(); ++i)
        if (seen.insert(rows[i].entity).second)
            unique.push_back(rows[i]);

    return unique;
}

// ---------------------------------------------------------------------------
// The sub-level tree, named.
//
// OX read eleven named sub-levels out of naboo-loaded.dmp - Naboo_01 as the
// root, two character-prefab DCCBs, GameModes, TeamDeathmatch,
// TeamDeathmatch_Online, Lobby, Naboo_012, HighEnd, Sunset, and one unnamed.
// Printing the tree is how we confirm the exact +0x28 root at runtime rather
// than trusting a count. A name comes from sub+0x60 -> Asset::Name at +0x18.
// ---------------------------------------------------------------------------
constexpr size_t kSubLevelAsset = 0x60;

static std::string SubLevelName(uintptr_t subLevel)
{
    uintptr_t asset = 0;
    if (!SafeRead(subLevel + kSubLevelAsset, &asset) || asset == 0)
        return std::string();

    uintptr_t namePointer = 0;
    if (!SafeRead(asset + kDataAssetName, &namePointer) || namePointer == 0)
        return std::string();

    return SafeCString(namePointer, 200);
}

static void DescribeSubLevels(uintptr_t subLevel, int depth, int* counted, std::string* out)
{
    if (subLevel == 0 || depth > 16 || *counted > 512)
        return;

    ++(*counted);

    uintptr_t bus = 0;
    SafeRead(subLevel + kSubLevelRootBus, &bus);

    // How many entities hang off this one, so a tree with the objects in it can
    // be told from a tree without.
    int owned = 0;
    uintptr_t node = 0;
    if (SafeRead(subLevel + kOwnerFirstNode, &node))
    {
        int guard = 0;
        while (node != 0 && guard++ < 65536)
        {
            ++owned;
            if (!SafeRead(node + kNodeNext, &node))
                break;
        }
    }

    const std::string name = SubLevelName(subLevel);

    char line[352];
    sprintf_s(line, "%*s0x%llX  bus 0x%llX  %d entit%s  '%s'\n", depth * 2, "",
              static_cast<unsigned long long>(subLevel),
              static_cast<unsigned long long>(bus), owned, owned == 1 ? "y" : "ies",
              name.empty() ? "(unnamed)" : name.c_str());
    *out += line;

    uintptr_t child = 0;
    if (SafeRead(subLevel + kSubLevelChild, &child) && child != 0)
        DescribeSubLevels(child, depth + 1, counted, out);

    uintptr_t sibling = 0;
    if (SafeRead(subLevel + kSubLevelSibling, &sibling) && sibling != 0)
        DescribeSubLevels(sibling, depth, counted, out);
}

// ---------------------------------------------------------------------------
// Self-dumping
//
// The crash we need captured happens roughly 0.6 s after a level-load message is
// posted - far too fast to catch with Task Manager. Being inside the process solves
// it: post, wait, dump, all on one thread with exact timing.
//
// Full memory, because the analysis needs the heap: expect a file about the size of
// the game's working set (~12 GB).
// ---------------------------------------------------------------------------

static bool WriteFullDump(const char* path, std::string* status)
{
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        char buffer[512];
        sprintf_s(buffer, "could not create '%s' (error %lu)", path, GetLastError());
        *status = buffer;
        return false;
    }

    const DWORD started = GetTickCount();

    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithFullMemoryInfo |
        MiniDumpWithThreadInfo);

    const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                      file, type, nullptr, nullptr, nullptr);

    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    CloseHandle(file);

    char buffer[512];
    if (!ok)
    {
        sprintf_s(buffer, "MiniDumpWriteDump failed (error %lu)", GetLastError());
        *status = buffer;
        LogLine("dump: FAILED %s", buffer);
        return false;
    }

    sprintf_s(buffer, "wrote %s (%.2f GB in %.1fs)", path,
              static_cast<double>(size.QuadPart) / (1024.0 * 1024.0 * 1024.0),
              (GetTickCount() - started) / 1000.0);
    *status = buffer;
    LogLine("dump: %s", buffer);
    return true;
}

// ---------------------------------------------------------------------------
// Command handling
// ---------------------------------------------------------------------------

static const size_t kClientLevelSetupOffset = 0x30;   // an embedded LevelSetup

static bool ReadLoadedLevelName(uintptr_t clientLevel, char* out, size_t outSize)
{
    if (clientLevel == 0)
        return false;

    uintptr_t namePointer = 0;
    if (SafeRead(clientLevel + kClientLevelSetupOffset + kLevelSetupNameOffset, &namePointer) &&
        namePointer != 0 &&
        RawReadString(out, outSize, namePointer, outSize - 1) &&
        out[0] != '\0')
        return true;

    // Fallback: any pointer in the object's head that reads as a level path.
    for (size_t offset = 0; offset < 0x100; offset += sizeof(uintptr_t))
    {
        uintptr_t candidate = 0;
        if (!SafeRead(clientLevel + offset, &candidate) || candidate == 0)
            continue;

        char text[256] = {};
        if (!RawReadString(text, sizeof(text), candidate, 255))
            continue;

        if (_strnicmp(text, "Levels/", 7) == 0)
        {
            strncpy_s(out, outSize, text, _TRUNCATE);
            return true;
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// Debug drawing.
//
// Slots below are the minimal set established by BFII disassembly in OX Batch 16.
// drawLine/drawBox are non-virtual header inlines around beginVertices/endVertices,
// so raw geometry needs no addresses beyond the vtable we already have.  State
// push/pop remain unresolved and are deliberately not called.
//
// The open question is the thread. Debug primitives are queued per-thread, so a
// draw issued from our own thread may land in a context nothing ever submits.
// setTimeVisible is set generously so that if it does reach the queue it stays
// long enough to be seen. Everything is guarded: if this thread is the wrong one
// the cost is nothing drawn, not a dead process.
// ---------------------------------------------------------------------------
constexpr uintptr_t kDebugRendererCurrentRva = 0x54863E0;

constexpr int kSlotSetDepthTest = 7;
constexpr int kSlotSetDepthWrite = 8;
constexpr int kSlotSetTimeVisible = 11;
// The live table settles the argument. Dumped from both threads at 02:08, slots
// 0..27 point into the module and slot 28 onward is rodata - decoded as ASCII it
// reads 'text', 'worldViewProj', 'screenScale', 'DebugFont', 'flushDebugRenderer',
// 'debugRender'. Those are shader parameter and pass names sitting after the
// table. **The vtable has 28 entries.** Slot 31 was never a virtual, which is the
// whole reason nothing has ever drawn.
constexpr int kDebugVtableSlots = 28;

// Slot 17 is game+0x14DE6D0 - BFII's screen-space drawTextReal.  Passing a Vec3
// to it (the old code did this) is an ABI violation.  World labels are projected
// through the live camera and then sent to this exact screen-text signature.
constexpr int kSlotDrawTextScreen = 17;

// OX Batch 16 resolved both thunks directly.  endVertices divides the byte delta
// by 32, independently confirming the vertex stride used below.
static volatile LONG g_slotBeginVertices = 19;
static volatile LONG g_slotEndVertices = 20;

// DebugGeometryType, complete. Shared source declares the order and forbids
// reordering it: Engine.Core/Debug/DebugRenderVertex.h:21-32.
constexpr uint32_t kGeometryTriangle3d = 0;
constexpr uint32_t kGeometryLine3d = 1;
constexpr uint32_t kGeometryTriangle2d = 2;
constexpr uint32_t kGeometryLine2d = 3;
constexpr uint32_t kGeometryFont2d = 4;
constexpr uint32_t kGeometryTriangleStrip = 5;

// ---------------------------------------------------------------------------
// WHY LINES NEVER DREW.  This is settled from shared source, not inferred.
//
// The premise everything was built on - "text works, geometry does not, so the
// vertex path is wrong" - was false. Text IS geometry through the same two
// slots. `drawText(int,int,...)` (BFII slot 17) calls `build2DText`, which calls
// `beginVertices(DebugGeometryType_Font2d, length*6)` and `endVertices`
// (DebugRenderer2.cpp:1125-1132, :917). The yellow label on screen is therefore
// direct proof that slots 19/20 work, that the 32-byte stride is right, that the
// colour offset is right, that our renderer instance is the correct one, that
// its command list IS flushed, and that our thread's submissions DO reach the
// GPU. Every one of those was on the suspect list. All are now cleared by an
// observation we already had.
//
// What is actually different is the render pass. The backend filters every
// command by geometry type against the pass it is drawing
// (BaseDebugRenderer2.cpp:205-225):
//
//     if (pass == DebugGeometry_World)  accept Line3d, Triangle3d
//     else /* DebugGeometry_Screen */   accept Font2d, Line2d, Triangle2d
//     return false                      // everything else is dropped silently
//
// DebugRenderPass.cpp calls the pass twice per frame, World then Screen
// (DebugRenderPass.cpp:161-164, :273, :282), and only the Screen call passes
// clearCommandList=true (:153). So World-typed commands submitted while the
// World pass is not running are accumulated and then discarded by the Screen
// pass - accepted, counted, never rasterised. That is exactly the observed
// signature: twelve lines submit without fault and nothing appears.
//
// And the World pass is the one that can be absent. 3D debug geometry is a
// separate frame-graph pass rendering to an offscreen target -
// `add3dDebugGeometryPass`, node name "RenderDebug3D"
// (DebugRenderPass.cpp:277-300). The Screen overlay draws straight onto the main
// target and survives in retail because the game's own overlays use it. A retail
// frame graph that never adds "RenderDebug3D" gives precisely this: Font2d
// visible, Line3d silently dropped.
//
// So the fix is to stop asking the engine to project our lines. We project them
// ourselves - the module already does exactly this projection for the label, and
// the label lands in the right place, so the maths is observed-correct - and
// submit Line2d, which is accepted by the same pass that is already drawing our
// text.
//
//   route 0  kLineRouteScreen  project here, submit Line2d   <- proven pass
//   route 1  kLineRouteWorld   submit Line3d, engine projects <- needs RenderDebug3D
//   route 2  kLineRouteBoth    submit both, one live run tells S1 which drew
//
// Route 2 exists so the World-pass question is answered by one live run instead
// of one rebuild per guess. If both routes draw, RenderDebug3D is present and
// route 1 is preferable (true depth occlusion). If only the screen wireframe
// appears, RenderDebug3D is absent in retail and route 0 is the answer.
constexpr LONG kLineRouteScreen = 0;
constexpr LONG kLineRouteWorld = 1;
constexpr LONG kLineRouteBoth = 2;
static volatile LONG g_lineRoute = kLineRouteScreen;

// Kept only so `linefix`/`vertexslots` diagnostics can still aim the world route
// at another bucket. The default is the enum value, not a guess.
static volatile LONG g_lineGeometryType = static_cast<LONG>(kGeometryLine3d);

// Exact shared layout: Engine.Core/Debug/DebugRenderVertex.h:34-40 declares
// `Tuple3f pos; Color32 color; Tuple3f normal; u32 pad;`, so colour is at +0x0C
// and normal at +0x10. The runtime layout switch that used to guard this is
// gone: the alternative ordering is falsified by the header AND by the fact that
// Font2d text - built through this identical struct - renders in the correct
// colour. Two independent proofs agree, so there is nothing left to switch.
//
// Note also that shared source never writes `normal` for line primitives
// (DebugRenderVertex::drawLine, :246-260) and never writes it for 2D primitives
// (:216-238). Lines do not need a normal; 2D primitives set pos.z = 1.0f.
struct DebugVertex
{
    float position[3];
    uint32_t color;
    float normal[3];
    uint32_t pad;
};
static_assert(sizeof(DebugVertex) == 32, "Frostbite DebugRenderVertex ABI changed");
static_assert(offsetof(DebugVertex, color) == 0x0C, "Color32 sits at +0x0C");
static_assert(offsetof(DebugVertex, normal) == 0x10, "normal sits at +0x10");

struct Vec3Aligned
{
    float x, y, z, w;      // Frostbite passes Vec3 in a 16-byte slot
};

typedef void*(__fastcall* DebugRendererCurrentFn)();
typedef void(__fastcall* DebugVoidFn)(void* self);
typedef void(__fastcall* DebugBoolFn)(void* self, bool value);
typedef void(__fastcall* DebugFloatFn)(void* self, float value);
typedef DebugVertex*(__fastcall* BeginVerticesFn)(void* self, uint32_t type, uint32_t count);
typedef void(__fastcall* EndVerticesFn)(void* self, uint32_t type, DebugVertex* end);
typedef void(__fastcall* DrawTextScreenFn)(void* self, int x, int y, const char* text,
                                           uint32_t color, float scale, int alignment,
                                           float rotationRadians, uint32_t channelMask);

// Slot 31 of what we took for the DebugRenderer2 vtable read back as
// 0x63536E6425726373 - little-endian ASCII, "scr%dnSc". That is a string
// constant, not a function, and the old reader happily returned it to be called.
// Every slot is checked against the module's own address range now: a pointer
// that is not inside the game image is not a virtual, and calling it is how a
// guarded no-op becomes a crash.
static bool AddressIsGameCode(uintptr_t address)
{
    return g_base != 0 && address > g_base && address < g_base + g_size;
}

static bool ReadVtableSlot(void* object, int slot, uintptr_t* out)
{
    uintptr_t vtable = 0;
    if (!SafeRead(reinterpret_cast<uintptr_t>(object), &vtable) || vtable == 0)
        return false;
    if (!AddressIsGameCode(vtable))
        return false;
    if (!SafeRead(vtable + slot * sizeof(uintptr_t), out) || *out == 0)
        return false;
    return AddressIsGameCode(*out);
}

static void* GetDebugRenderer()
{
    DebugRendererCurrentFn current =
        reinterpret_cast<DebugRendererCurrentFn>(g_base + kDebugRendererCurrentRva);

    __try
    {
        return current();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Segment pipeline.
//
// Everything that draws lines now builds a segment list and hands it to
// EmitSegments, which submits Line2d (projected here), Line3d (projected by the
// engine), or both, in ONE begin/end pair per geometry type per frame.
//
// One pair per type is a preference, not a correctness requirement. The old
// comment here argued that endVertices clearing the per-type cursor meant only
// the last of twelve begin/end pairs could survive. That is falsified by
// observation: the text corner-mark box issues one begin/end pair of Font2d per
// dot - well over a hundred pairs in a frame - and every dot renders. Shared
// source agrees; internalBeginVertices allocates a NEW command per call
// (DebugRenderer2.cpp:670-692) and the per-type entry is a scratch cursor, not
// the storage. Batching is done because it is cheaper and tidier, not to dodge a
// bug that does not exist.
// ---------------------------------------------------------------------------

// A segment list sized for the worst case we ever build in one frame: an
// oriented box (12), an AABB (12), a gizmo with arrowheads and hit regions
// (~60), and headroom.
constexpr int kMaxSegments = 256;

struct SegmentList
{
    float points[kMaxSegments][2][3];
    uint32_t colors[kMaxSegments];
    int count;

    void clear() { count = 0; }

    void add(const float from[3], const float to[3], uint32_t color)
    {
        if (count >= kMaxSegments)
            return;
        for (int axis = 0; axis < 3; ++axis)
        {
            points[count][0][axis] = from[axis];
            points[count][1][axis] = to[axis];
        }
        colors[count] = color;
        ++count;
    }
};

// Screen-space segments carry pixel coordinates. Shared source sets pos.z = 1.0f
// for every 2D primitive (DebugRenderVertex::drawLine2d, :226-238) and never
// writes normal, so that is exactly what we write.
struct ScreenSegmentList
{
    float points[kMaxSegments][2][2];
    uint32_t colors[kMaxSegments];
    int count;

    void clear() { count = 0; }

    void add(float x0, float y0, float x1, float y1, uint32_t color)
    {
        if (count >= kMaxSegments)
            return;
        points[count][0][0] = x0; points[count][0][1] = y0;
        points[count][1][0] = x1; points[count][1][1] = y1;
        colors[count] = color;
        ++count;
    }
};

// The camera basis, resolved once per submission instead of once per label. The
// arithmetic is lifted verbatim from DrawText3d, which is the only projection in
// this module that has ever been confirmed against the screen - the label lands
// where it should, so these terms are observed-correct and are not re-derived.
struct CameraBasis
{
    Transform camera;
    float tanHalfFovX;
    float aspect;
    float width;
    float height;
    bool valid;
};

static bool BuildCameraBasis(CameraBasis* out)
{
    memset(out, 0, sizeof(*out));

    float fovX = 0.0f;
    if (!ReadCamera(&out->camera, &fovX) || fovX <= 0.01f || fovX >= 3.13f)
        return false;

    out->tanHalfFovX = tanf(fovX * 0.5f);
    if (out->tanHalfFovX <= 0.001f)
        return false;

    // Debug 2D geometry and debug text share one coordinate space - both are
    // pixels on the backend's screen resolution - so the resolution the label
    // already projects against is the correct one for lines too.
    const int width = GetSystemMetrics(SM_CXSCREEN);
    const int height = GetSystemMetrics(SM_CYSCREEN);
    if (width <= 0 || height <= 0)
        return false;

    out->width = static_cast<float>(width);
    out->height = static_cast<float>(height);
    out->aspect = out->width / out->height;
    out->valid = true;
    return true;
}

// World point -> camera space. `depth` is along forward; `right` is negated
// because Frostbite's basis names the lateral vector `left`.
struct ViewPoint
{
    float right;
    float up;
    float depth;
};

static ViewPoint ToViewSpace(const CameraBasis& basis, const float world[3])
{
    const float delta[3] = {
        world[0] - basis.camera.translation[0],
        world[1] - basis.camera.translation[1],
        world[2] - basis.camera.translation[2]
    };

    ViewPoint out;
    out.depth = delta[0] * basis.camera.forward[0] +
                delta[1] * basis.camera.forward[1] +
                delta[2] * basis.camera.forward[2];
    out.right = -(delta[0] * basis.camera.left[0] +
                  delta[1] * basis.camera.left[1] +
                  delta[2] * basis.camera.left[2]);
    out.up = delta[0] * basis.camera.up[0] +
             delta[1] * basis.camera.up[1] +
             delta[2] * basis.camera.up[2];
    return out;
}

static void ViewToPixels(const CameraBasis& basis, const ViewPoint& v, float* x, float* y)
{
    const float ndcX = v.right / (v.depth * basis.tanHalfFovX);
    const float ndcY = v.up * basis.aspect / (v.depth * basis.tanHalfFovX);
    *x = (ndcX * 0.5f + 0.5f) * basis.width;
    *y = (0.5f - ndcY * 0.5f) * basis.height;
}

// A single world point, for labels and gizmo anchors.
static bool ProjectPoint(const CameraBasis& basis, const float world[3],
                         float* x, float* y, float* depth)
{
    const ViewPoint v = ToViewSpace(basis, world);
    if (v.depth <= 0.05f)
        return false;
    ViewToPixels(basis, v, x, y);
    if (depth != nullptr)
        *depth = v.depth;
    return true;
}

// Near-plane clipping is the whole reason lines need more care than labels. A
// label behind the camera is simply dropped; a box edge with one end behind the
// camera must be SHORTENED to the near plane, because dropping it leaves a hole
// in the wireframe and keeping it projects to a mirrored garbage coordinate.
constexpr float kNearDepth = 0.10f;

static bool ClipToNearPlane(ViewPoint* a, ViewPoint* b)
{
    const bool aBehind = a->depth < kNearDepth;
    const bool bBehind = b->depth < kNearDepth;

    if (aBehind && bBehind)
        return false;
    if (!aBehind && !bBehind)
        return true;

    ViewPoint* behind = aBehind ? a : b;
    const ViewPoint* front = aBehind ? b : a;

    const float span = front->depth - behind->depth;
    if (fabsf(span) < 1e-6f)
        return false;

    const float t = (kNearDepth - behind->depth) / span;
    behind->right += (front->right - behind->right) * t;
    behind->up += (front->up - behind->up) * t;
    behind->depth = kNearDepth;
    return true;
}

// Cohen-Sutherland against a viewport grown by one screen in each direction. A
// clamp would bend a line that crosses the screen; a real clip keeps its
// direction. The margin means near-edge geometry stays continuous while
// coordinates remain small enough that nothing downstream sees a huge float.
static int OutCode(float x, float y, float minX, float minY, float maxX, float maxY)
{
    int code = 0;
    if (x < minX) code |= 1;
    else if (x > maxX) code |= 2;
    if (y < minY) code |= 4;
    else if (y > maxY) code |= 8;
    return code;
}

static bool ClipToViewport(const CameraBasis& basis, float* x0, float* y0, float* x1, float* y1)
{
    const float minX = -basis.width;
    const float maxX = basis.width * 2.0f;
    const float minY = -basis.height;
    const float maxY = basis.height * 2.0f;

    int code0 = OutCode(*x0, *y0, minX, minY, maxX, maxY);
    int code1 = OutCode(*x1, *y1, minX, minY, maxX, maxY);

    // Bounded: each iteration clears at least one bit of one endpoint.
    for (int guard = 0; guard < 8; ++guard)
    {
        if ((code0 | code1) == 0)
            return true;
        if ((code0 & code1) != 0)
            return false;

        const int code = code0 != 0 ? code0 : code1;
        float x = 0.0f, y = 0.0f;
        const float dx = *x1 - *x0;
        const float dy = *y1 - *y0;

        if (code & 8)        { y = maxY; x = *x0 + dx * (maxY - *y0) / dy; }
        else if (code & 4)   { y = minY; x = *x0 + dx * (minY - *y0) / dy; }
        else if (code & 2)   { x = maxX; y = *y0 + dy * (maxX - *x0) / dx; }
        else                 { x = minX; y = *y0 + dy * (minX - *x0) / dx; }

        if (code == code0)
        {
            *x0 = x; *y0 = y;
            code0 = OutCode(*x0, *y0, minX, minY, maxX, maxY);
        }
        else
        {
            *x1 = x; *y1 = y;
            code1 = OutCode(*x1, *y1, minX, minY, maxX, maxY);
        }
    }

    return false;
}

// One begin/end pair. `fill` writes vertexCount vertices into the returned
// block. Returns the number of segments the caller said it wrote, or 0.
template <typename Fill>
static int SubmitVertices(void* renderer, uint32_t type, uint32_t vertexCount,
                          int segmentCount, Fill fill)
{
    if (vertexCount == 0 || segmentCount <= 0)
        return 0;

    uintptr_t beginFn = 0, endFn = 0;
    if (!ReadVtableSlot(renderer, g_slotBeginVertices, &beginFn) ||
        !ReadVtableSlot(renderer, g_slotEndVertices, &endFn))
        return 0;

    __try
    {
        DebugVertex* vertices =
            reinterpret_cast<BeginVerticesFn>(beginFn)(renderer, type, vertexCount);
        if (vertices == nullptr)
            return 0;

        fill(vertices);

        // endVertices takes the END pointer and derives the count from it. Never
        // hand it more than was asked for: shared source fatal-asserts on
        // written > requested (DebugRenderer2.cpp:703).
        reinterpret_cast<EndVerticesFn>(endFn)(renderer, type, vertices + vertexCount);
        return segmentCount;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

// Line2d: pixels, pos.z = 1.0f, no normal.
static int SubmitScreenSegments(void* renderer, const ScreenSegmentList& list)
{
    if (list.count <= 0)
        return 0;

    const uint32_t vertexCount = static_cast<uint32_t>(list.count) * 2u;
    return SubmitVertices(renderer, kGeometryLine2d, vertexCount, list.count,
        [&list](DebugVertex* vertices)
        {
            for (int segment = 0; segment < list.count; ++segment)
                for (int end = 0; end < 2; ++end)
                {
                    DebugVertex* v = vertices + segment * 2 + end;
                    memset(v, 0, sizeof(*v));
                    v->position[0] = list.points[segment][end][0];
                    v->position[1] = list.points[segment][end][1];
                    v->position[2] = 1.0f;
                    v->color = list.colors[segment];
                }
        });
}

// Line3d: world units, the engine projects. Only reaches the screen when the
// "RenderDebug3D" frame-graph pass is present.
static int SubmitWorldSegments(void* renderer, const SegmentList& list)
{
    if (list.count <= 0)
        return 0;

    const uint32_t type = static_cast<uint32_t>(g_lineGeometryType);
    const uint32_t vertexCount = static_cast<uint32_t>(list.count) * 2u;
    return SubmitVertices(renderer, type, vertexCount, list.count,
        [&list](DebugVertex* vertices)
        {
            for (int segment = 0; segment < list.count; ++segment)
                for (int end = 0; end < 2; ++end)
                {
                    DebugVertex* v = vertices + segment * 2 + end;
                    memset(v, 0, sizeof(*v));
                    memcpy(v->position, list.points[segment][end], sizeof(float) * 3);
                    v->color = list.colors[segment];
                }
        });
}

// Project a world segment list into a screen segment list, clipping each
// segment to the near plane and then to the viewport.
static void ProjectSegments(const CameraBasis& basis, const SegmentList& world,
                            ScreenSegmentList* screen)
{
    for (int segment = 0; segment < world.count; ++segment)
    {
        ViewPoint a = ToViewSpace(basis, world.points[segment][0]);
        ViewPoint b = ToViewSpace(basis, world.points[segment][1]);

        if (!ClipToNearPlane(&a, &b))
            continue;

        float x0, y0, x1, y1;
        ViewToPixels(basis, a, &x0, &y0);
        ViewToPixels(basis, b, &x1, &y1);

        if (!ClipToViewport(basis, &x0, &y0, &x1, &y1))
            continue;

        screen->add(x0, y0, x1, y1, world.colors[segment]);
    }
}

// The one entry point every line drawing goes through. `extraScreen` carries
// segments that are already in pixels - the gizmo's screen-constant parts - so
// they ride the same single Line2d submission.
static int EmitSegments(void* renderer, const SegmentList& world,
                        const ScreenSegmentList* extraScreen)
{
    const LONG route = g_lineRoute;
    int drawn = 0;

    if (route == kLineRouteWorld || route == kLineRouteBoth)
        drawn += SubmitWorldSegments(renderer, world);

    if (route == kLineRouteScreen || route == kLineRouteBoth ||
        (extraScreen != nullptr && extraScreen->count > 0))
    {
        CameraBasis basis;
        if (BuildCameraBasis(&basis))
        {
            ScreenSegmentList screen;
            screen.clear();

            if (route == kLineRouteScreen || route == kLineRouteBoth)
                ProjectSegments(basis, world, &screen);

            if (extraScreen != nullptr)
                for (int i = 0; i < extraScreen->count; ++i)
                    screen.add(extraScreen->points[i][0][0], extraScreen->points[i][0][1],
                               extraScreen->points[i][1][0], extraScreen->points[i][1][1],
                               extraScreen->colors[i]);

            drawn += SubmitScreenSegments(renderer, screen);
        }
    }

    return drawn;
}

// Compatibility wrappers. Existing call sites keep working; each builds a list
// and emits it in one pair.
static int DrawLines3d(void* renderer, const float (*segments)[2][3], int count, uint32_t color)
{
    if (count <= 0)
        return 0;

    SegmentList list;
    list.clear();
    for (int i = 0; i < count; ++i)
        list.add(segments[i][0], segments[i][1], color);

    return EmitSegments(renderer, list, nullptr);
}

static bool DrawLine3d(void* renderer, const float from[3], const float to[3], uint32_t color)
{
    SegmentList list;
    list.clear();
    list.add(from, to, color);
    return EmitSegments(renderer, list, nullptr) > 0;
}

// The twelve edges of a box, shared by the axis-aligned and oriented builders.
static const int kBoxEdges[12][2] = {
    {0,1},{1,2},{2,3},{3,0},
    {4,5},{5,6},{6,7},{7,4},
    {0,4},{1,5},{2,6},{3,7}
};

static void AppendBoxEdges(SegmentList* list, const float points[8][3], uint32_t color)
{
    for (int edge = 0; edge < 12; ++edge)
        list->add(points[kBoxEdges[edge][0]], points[kBoxEdges[edge][1]], color);
}

// The twelve edges of an axis-aligned box.
static void AppendBox(SegmentList* list, const float centre[3], const float halfExtent[3],
                      uint32_t color)
{
    const int corners[8][3] = {
        {0,0,0}, {1,0,0}, {1,1,0}, {0,1,0},
        {0,0,1}, {1,0,1}, {1,1,1}, {0,1,1}
    };

    float points[8][3];
    for (int corner = 0; corner < 8; ++corner)
        for (int axis = 0; axis < 3; ++axis)
            points[corner][axis] = centre[axis] +
                (corners[corner][axis] != 0 ? halfExtent[axis] : -halfExtent[axis]);

    AppendBoxEdges(list, points, color);
}

// SpatialEntityBoundsInfo is an AABB in a separate transform space.  Those eight
// transformed corners give the oriented box the editor needs.
static void AppendOrientedBox(SegmentList* list, const float centre[3], const float halfExtent[3],
                              const float left[3], const float up[3], const float forward[3],
                              uint32_t color)
{
    const int signs[8][3] = {
        {-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1},
        {-1,-1, 1}, {1,-1, 1}, {1,1, 1}, {-1,1, 1}
    };

    float points[8][3];
    for (int corner = 0; corner < 8; ++corner)
        for (int axis = 0; axis < 3; ++axis)
            points[corner][axis] = centre[axis] +
                left[axis] * halfExtent[0] * signs[corner][0] +
                up[axis] * halfExtent[1] * signs[corner][1] +
                forward[axis] * halfExtent[2] * signs[corner][2];

    AppendBoxEdges(list, points, color);
}

// An AABB given by its two world corners - the box the pick ray actually struck.
static void AppendAabb(SegmentList* list, const float minimum[3], const float maximum[3],
                       uint32_t color)
{
    const int corners[8][3] = {
        {0,0,0}, {1,0,0}, {1,1,0}, {0,1,0},
        {0,0,1}, {1,0,1}, {1,1,1}, {0,1,1}
    };

    float points[8][3];
    for (int corner = 0; corner < 8; ++corner)
        for (int axis = 0; axis < 3; ++axis)
            points[corner][axis] = corners[corner][axis] ? maximum[axis] : minimum[axis];

    AppendBoxEdges(list, points, color);
}

static int DrawBox3d(void* renderer, const float centre[3], const float halfExtent[3],
                      uint32_t color)
{
    SegmentList list;
    list.clear();
    AppendBox(&list, centre, halfExtent, color);
    return EmitSegments(renderer, list, nullptr);
}

static int DrawOrientedBox3d(void* renderer, const float centre[3], const float halfExtent[3],
                             const float left[3], const float up[3], const float forward[3],
                             uint32_t color)
{
    SegmentList list;
    list.clear();
    AppendOrientedBox(&list, centre, halfExtent, left, up, forward, color);
    return EmitSegments(renderer, list, nullptr);
}

// ---------------------------------------------------------------------------
// The translate gizmo.
//
// The plugin already owns gizmo state, hit-testing, ray projection and
// commit-to-`move`, with passing tests. The only missing half was pixels on
// screen, and that is all this draws.
//
// Three parts, and the split matters:
//
//   the arms       world segments, so they follow the object's own basis and
//                  pick up true depth if the world route ever renders;
//   the arrowheads screen segments, because an arrowhead must stay the same
//                  size and shape whatever the viewing angle;
//   the hit regions screen segments, because that is the space the plugin
//                  hit-tests in - drawing them anywhere else would be a lie.
//
// Screen-constant sizing: a world offset d perpendicular to the view at depth z
// lands at ndc = d/(z*tanHalfFovX), and pixels = ndc*0.5*width. Inverting,
// d = pixels*2*z*tanHalfFovX/width. So the arm is recomputed every frame from
// the current depth and the gizmo never changes size as the camera moves.
//
// Pixel sizes are settable so the plugin can publish the same numbers it
// hit-tests with, rather than the two sides drifting apart.
// ---------------------------------------------------------------------------
static volatile bool g_gizmoEnabled = true;
static volatile float g_gizmoArmPixels = 90.0f;
static volatile float g_gizmoHitPixels = 11.0f;
static volatile float g_gizmoHeadPixels = 14.0f;

// Color32 is ABGR here: 0xAABBGGRR. X red, Y green, Z blue.
constexpr uint32_t kAxisColorX = 0xFF0000FFu;
constexpr uint32_t kAxisColorY = 0xFF00FF00u;
constexpr uint32_t kAxisColorZ = 0xFFFF0000u;
constexpr uint32_t kGizmoCentreColor = 0xFFFFFFFFu;

// Same hue, half alpha - the hit region reads as a hint rather than as geometry.
static uint32_t DimmedForHitRegion(uint32_t color)
{
    return (color & 0x00FFFFFFu) | 0x80000000u;
}

static void AppendScreenRect(ScreenSegmentList* screen, float cx, float cy, float half,
                             uint32_t color)
{
    const float minX = cx - half, maxX = cx + half;
    const float minY = cy - half, maxY = cy + half;
    screen->add(minX, minY, maxX, minY, color);
    screen->add(maxX, minY, maxX, maxY, color);
    screen->add(maxX, maxY, minX, maxY, color);
    screen->add(minX, maxY, minX, minY, color);
}

static void AppendGizmo(const CameraBasis& basis, const float origin[3],
                        const float left[3], const float up[3], const float forward[3],
                        SegmentList* world, ScreenSegmentList* screen)
{
    float originX = 0.0f, originY = 0.0f, depth = 0.0f;
    if (!ProjectPoint(basis, origin, &originX, &originY, &depth))
        return;

    const float perPixel = 2.0f * depth * basis.tanHalfFovX / basis.width;
    const float arm = g_gizmoArmPixels * perPixel;
    if (!(arm > 0.0f) || !(arm < 1.0e6f))      // rejects NaN as well as absurd
        return;

    // X is -left because Frostbite names the lateral axis `left`; Y and Z are
    // up and forward directly. This matches the axes the picker already draws.
    const float axes[3][3] = {
        { -left[0], -left[1], -left[2] },
        {  up[0],    up[1],    up[2]   },
        {  forward[0], forward[1], forward[2] }
    };
    const uint32_t colors[3] = { kAxisColorX, kAxisColorY, kAxisColorZ };

    const float hit = g_gizmoHitPixels;
    const float head = g_gizmoHeadPixels;

    for (int axis = 0; axis < 3; ++axis)
    {
        float end[3];
        for (int i = 0; i < 3; ++i)
            end[i] = origin[i] + axes[axis][i] * arm;

        world->add(origin, end, colors[axis]);

        float endX = 0.0f, endY = 0.0f;
        if (!ProjectPoint(basis, end, &endX, &endY, nullptr))
            continue;

        // Arrowhead, built from the on-screen direction of the arm. An arm
        // pointing almost straight at the camera has no usable direction, so it
        // gets the hit region and no head rather than a random one.
        const float dx = endX - originX;
        const float dy = endY - originY;
        const float length = sqrtf(dx * dx + dy * dy);
        if (length > 1.0f)
        {
            const float ux = dx / length;
            const float uy = dy / length;
            const float baseX = endX - ux * head;
            const float baseY = endY - uy * head;
            const float halfWidth = head * 0.45f;
            screen->add(endX, endY, baseX - uy * halfWidth, baseY + ux * halfWidth,
                        colors[axis]);
            screen->add(endX, endY, baseX + uy * halfWidth, baseY - ux * halfWidth,
                        colors[axis]);
        }

        AppendScreenRect(screen, endX, endY, hit, DimmedForHitRegion(colors[axis]));
    }

    // The centre handle - the plugin's free-move region.
    AppendScreenRect(screen, originX, originY, hit, DimmedForHitRegion(kGizmoCentreColor));
}

static bool DrawText3d(void* renderer, const float position[3], const char* text, uint32_t color)
{
    uintptr_t drawFn = 0;
    if (!ReadVtableSlot(renderer, kSlotDrawTextScreen, &drawFn))
        return false;

    Transform camera{};
    float fovX = 0.0f;
    if (!ReadCamera(&camera, &fovX) || fovX <= 0.01f || fovX >= 3.13f)
        return false;

    const float delta[3] = {
        position[0] - camera.translation[0],
        position[1] - camera.translation[1],
        position[2] - camera.translation[2]
    };
    const float depth = delta[0] * camera.forward[0] +
                        delta[1] * camera.forward[1] +
                        delta[2] * camera.forward[2];
    if (depth <= 0.05f)
        return false;

    // RenderView stores horizontal FOV (fovX). Frostbite's basis names the
    // lateral vector `left`, hence the minus sign for screen-right.
    const float right = -(delta[0] * camera.left[0] +
                          delta[1] * camera.left[1] +
                          delta[2] * camera.left[2]);
    const float up = delta[0] * camera.up[0] +
                     delta[1] * camera.up[1] +
                     delta[2] * camera.up[2];
    const float tanHalfFovX = tanf(fovX * 0.5f);
    if (tanHalfFovX <= 0.001f)
        return false;

    // Debug text uses desktop pixels. In the embedded Frosty view the game is a
    // child window, but the monitor aspect remains a stable projection fallback.
    const int width = GetSystemMetrics(SM_CXSCREEN);
    const int height = GetSystemMetrics(SM_CYSCREEN);
    if (width <= 0 || height <= 0)
        return false;

    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const float ndcX = right / (depth * tanHalfFovX);
    const float ndcY = up * aspect / (depth * tanHalfFovX);
    if (ndcX < -1.2f || ndcX > 1.2f || ndcY < -1.2f || ndcY > 1.2f)
        return false;

    const int x = static_cast<int>((ndcX * 0.5f + 0.5f) * width);
    const int y = static_cast<int>((0.5f - ndcY * 0.5f) * height);

    __try
    {
        reinterpret_cast<DrawTextScreenFn>(drawFn)(renderer, x, y, text, color,
                                                   1.0f, 0, 0.0f, 0xFFFFFFFFu);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static void SetDebugState(void* renderer, int slot, bool value)
{
    uintptr_t fn = 0;
    if (!ReadVtableSlot(renderer, slot, &fn))
        return;
    __try { reinterpret_cast<DebugBoolFn>(fn)(renderer, value); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

static void SetDebugTimeVisible(void* renderer, float seconds)
{
    uintptr_t fn = 0;
    if (!ReadVtableSlot(renderer, kSlotSetTimeVisible, &fn))
        return;
    __try { reinterpret_cast<DebugFloatFn>(fn)(renderer, seconds); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

static void CallDebugVoid(void* renderer, int slot)
{
    uintptr_t fn = 0;
    if (!ReadVtableSlot(renderer, slot, &fn))
        return;
    __try { reinterpret_cast<DebugVoidFn>(fn)(renderer); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// ---------------------------------------------------------------------------
// Finding beginVertices by asking, not by counting.
//
// The header this project's slot numbers came from declares more virtuals than
// this build's 28-entry table has, so everything past the removal point is
// shifted by an unknown amount and declaration order proves nothing. Slot 17
// matched OX's drawTextReal RVA exactly; nothing else can be pinned that way.
//
// So: call each slot as beginVertices(kGeometryLine3d, 2) on the engine's own
// thread, under a guard, and see which one hands back a writable buffer. A
// beginVertices returns a vertex pointer into the renderer's own cache; almost
// everything else returns a small integer, a bool, or faults. That is a
// signature, and it is cheap to read.
//
// Nothing is committed: endVertices is never called, so whatever cursor a real
// beginVertices moved is left un-submitted and the frame drops it.
// ---------------------------------------------------------------------------
struct VertexSweepResult
{
    uintptr_t returned;
    bool faulted;
    bool writable;
};

static VertexSweepResult g_sweepResults[kDebugVtableSlots];

static bool PointerIsWritableData(uintptr_t address)
{
    if (address < 0x10000 || (address & 3) != 0)
        return false;
    if (AddressIsGameCode(address))
        return false;      // a vertex buffer is heap, not the image

    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) == 0)
        return false;
    if (info.State != MEM_COMMIT)
        return false;

    const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (info.Protect & writable) != 0;
}

static uintptr_t GuardedCallTwoArgs(uintptr_t fn, void* self, unsigned first, unsigned second,
                                    bool* faulted)
{
    *faulted = false;
    __try
    {
        return reinterpret_cast<uintptr_t>(
            reinterpret_cast<BeginVerticesFn>(fn)(self, first, second));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *faulted = true;
        return 0;
    }
}

// Runs on the engine's camera thread, from the breakpoint handler.
static void SweepVertexSlotsOnFrameThread()
{
    memset(g_sweepResults, 0, sizeof(g_sweepResults));

    void* renderer = GetDebugRenderer();
    if (renderer == nullptr)
        return;

    for (int slot = 0; slot < kDebugVtableSlots; ++slot)
    {
        uintptr_t fn = 0;
        if (!ReadVtableSlot(renderer, slot, &fn))
            continue;

        bool faulted = false;
        const uintptr_t returned =
            GuardedCallTwoArgs(fn, renderer, kGeometryLine3d, 2, &faulted);

        g_sweepResults[slot].returned = returned;
        g_sweepResults[slot].faulted = faulted;
        g_sweepResults[slot].writable = !faulted && PointerIsWritableData(returned);
    }
}

// Runs on the engine's camera thread. Reads only - no virtual is called with the
// pointer here, because the whole point is to find out whether calling one would
// be safe.
static void CaptureDebugRendererOnFrameThread()
{
    g_capturedThread = GetCurrentThreadId();
    g_capturedRenderer = 0;
    g_capturedVtable = 0;
    memset(g_capturedSlots, 0, sizeof(g_capturedSlots));

    void* renderer = GetDebugRenderer();
    if (renderer == nullptr)
        return;

    g_capturedRenderer = reinterpret_cast<uintptr_t>(renderer);

    uintptr_t vtable = 0;
    if (!SafeRead(g_capturedRenderer, &vtable) || vtable == 0)
        return;

    g_capturedVtable = vtable;
    for (int slot = 0; slot < kCapturedSlots; ++slot)
        SafeRead(vtable + static_cast<size_t>(slot) * sizeof(uintptr_t), &g_capturedSlots[slot]);
}

// Asking DebugRenderer_current() from our own thread never returned - the
// command sat for the full two-minute client timeout while the engine's thread
// kept trapping normally, so it is our call that blocks, not the game. OX's
// disassembly showed the getter runs an init-once behind four guard statics
// before indexing the context table; a thread with no context appears to block
// in there. So it gets its own thread and a deadline, and a thread that never
// comes back is abandoned rather than waited on.
struct RendererProbe
{
    volatile LONG finished;
    uintptr_t renderer;
    uintptr_t vtable;
    uintptr_t slots[kCapturedSlots];
};

static DWORD WINAPI ProbeRendererThread(LPVOID parameter)
{
    RendererProbe* probe = static_cast<RendererProbe*>(parameter);

    void* renderer = GetDebugRenderer();
    probe->renderer = reinterpret_cast<uintptr_t>(renderer);

    if (renderer != nullptr && SafeRead(probe->renderer, &probe->vtable) && probe->vtable != 0)
    {
        for (int slot = 0; slot < kCapturedSlots; ++slot)
            SafeRead(probe->vtable + static_cast<size_t>(slot) * sizeof(uintptr_t),
                     &probe->slots[slot]);
    }

    InterlockedExchange(&probe->finished, 1);
    return 0;
}

// Formats one renderer's vtable. Shared by both threads' reports so they can be
// read side by side.
static std::string DescribeRendererTable(const char* who, DWORD threadId, uintptr_t renderer,
                                         uintptr_t vtable, const uintptr_t* slots)
{
    char header[288];
    if (renderer == 0)
    {
        sprintf_s(header, "%s (thread %lu): DebugRenderer_current() returned nothing\n",
                  who, threadId);
        return header;
    }

    sprintf_s(header, "%s (thread %lu): renderer 0x%llX  vtable %s\n", who, threadId,
              static_cast<unsigned long long>(renderer),
              AddressIsGameCode(vtable) ? "in module" : "NOT IN MODULE - not an object");
    std::string out(header);

    char line[160];
    sprintf_s(line, "  vtable 0x%llX", static_cast<unsigned long long>(vtable));
    out += line;
    if (AddressIsGameCode(vtable))
    {
        sprintf_s(line, " (game+0x%llX)", static_cast<unsigned long long>(vtable - g_base));
        out += line;
    }
    out += "\n";

    int lastCode = -1;
    for (int slot = 0; slot < kCapturedSlots; ++slot)
        if (AddressIsGameCode(slots[slot]))
            lastCode = slot;

    for (int slot = 0; slot < kCapturedSlots; ++slot)
    {
        const uintptr_t entry = slots[slot];
        if (AddressIsGameCode(entry))
            sprintf_s(line, "  [%2d] +0x%02X  game+0x%llX\n", slot, slot * 8,
                      static_cast<unsigned long long>(entry - g_base));
        else
            sprintf_s(line, "  [%2d] +0x%02X  0x%016llX   <- not code\n", slot, slot * 8,
                      static_cast<unsigned long long>(entry));
        out += line;
    }

    sprintf_s(line, "  last slot pointing into the module: %d\n", lastCode);
    out += line;
    return out;
}

// Runs on the engine's camera thread, inside the breakpoint handler. Allocation
// free and read only against the snapshot: everything it needs was computed on
// our thread already. It reads the sequence counter either side of the copy and
// gives up on a torn read rather than drawing half of two selections.
static void SubmitSelectionOnFrameThread()
{
    const LONG test = InterlockedExchange(&g_frameTestPending, 0);

    SelectionSnapshot local;
    LONG sequence = 0;
    bool haveSelection = false;

    if (g_showSelection && g_selectedEntity != 0)
    {
        sequence = g_snapshotSequence;
        if ((sequence & 1) == 0)
        {
            memcpy(&local, &g_selectionSnapshot, sizeof(local));
            haveSelection = (g_snapshotSequence == sequence);
        }
    }

    if (!haveSelection && test == 0)
        return;

    void* renderer = GetDebugRenderer();
    if (renderer == nullptr)
        return;

    SetDebugState(renderer, kSlotSetDepthTest, false);
    SetDebugState(renderer, kSlotSetDepthWrite, false);
    SetDebugTimeVisible(renderer, 0.1f);

    // Everything this frame draws is accumulated into one world list and one
    // screen list, then submitted in a single begin/end pair per geometry type.
    SegmentList world;
    ScreenSegmentList screen;
    world.clear();
    screen.clear();

    CameraBasis basis;
    const bool haveCamera = BuildCameraBasis(&basis);

    if (test != 0)
    {
        const float size = g_frameTestSize;
        const float halfExtent[3] = { size, size, size };
        AppendBox(&world, g_frameTestCentre, halfExtent, 0xFF00FF00u);
    }

    if (haveSelection)
    {
        // The AABB the ray actually struck is authoritative - it cannot disagree
        // with what was selected - so it is drawn solid. The oriented box is
        // drawn too, dimmed, because it carries the object's rotation and the
        // AABB does not.
        if (local.hasAabb)
            AppendAabb(&world, local.aabbMinimum, local.aabbMaximum, 0xFFFFFFFFu);

        AppendOrientedBox(&world, local.centre, local.halfExtent,
                          local.boxLeft, local.boxUp, local.boxForward,
                          local.hasAabb ? 0xC0FFFFFFu : 0xFFFFFFFFu);

        if (local.hasTransform && g_gizmoEnabled && haveCamera)
            AppendGizmo(basis, local.origin, local.left, local.up, local.forward,
                        &world, &screen);
    }

    const int lines = EmitSegments(renderer, world, &screen);

    // Text is a separate geometry bucket (Font2d), so it cannot share the line
    // pair and is submitted on its own.
    if (test != 0)
        DrawText3d(renderer, g_frameTestCentre, "framedraw\nengine thread", 0xFF00FFFFu);

    if (haveSelection)
    {
        // The text-mark fallback box.
        //
        // Kept, but now conditional. It exists because Font2d was the only
        // geometry ever proven to reach the screen, so the selection was
        // outlined with stepped full stops. If the line submission produced
        // segments this frame the dots are redundant and would only clutter the
        // wireframe, so they are drawn only when the line submission produced
        // nothing at all.
        //
        // That makes it a real fallback rather than a permanent second outline,
        // and it makes a live run readable at a glance: crisp edges mean the
        // line route works, dots mean it does not.
        if (local.hasAabb && lines == 0)
        {
            const int corners[8][3] = {
                {0,0,0}, {1,0,0}, {1,1,0}, {0,1,0},
                {0,0,1}, {1,0,1}, {1,1,1}, {0,1,1}
            };

            float points[8][3];
            for (int corner = 0; corner < 8; ++corner)
                for (int axis = 0; axis < 3; ++axis)
                    points[corner][axis] = corners[corner][axis]
                        ? local.aabbMaximum[axis] : local.aabbMinimum[axis];

            // Step count scales with the box so a 2 m prop is not smothered and
            // a 100 m hall still reads as an outline.
            float longest = 0.0f;
            for (int axis = 0; axis < 3; ++axis)
            {
                const float span = local.aabbMaximum[axis] - local.aabbMinimum[axis];
                if (span > longest)
                    longest = span;
            }
            int steps = static_cast<int>(longest / 1.5f);
            if (steps < 2) steps = 2;
            if (steps > 10) steps = 10;

            for (int edge = 0; edge < 12; ++edge)
            {
                const float* a = points[kBoxEdges[edge][0]];
                const float* b = points[kBoxEdges[edge][1]];
                for (int step = 0; step <= steps; ++step)
                {
                    const float t = static_cast<float>(step) / static_cast<float>(steps);
                    const float at[3] = {
                        a[0] + (b[0] - a[0]) * t,
                        a[1] + (b[1] - a[1]) * t,
                        a[2] + (b[2] - a[2]) * t
                    };
                    DrawText3d(renderer, at, ".", 0xFFFFFFFFu);
                }
            }
        }

        if (local.label[0] != '\0')
        {
            // Above the box in world terms, not local terms.
            float labelAt[3];
            if (local.hasAabb)
            {
                labelAt[0] = (local.aabbMinimum[0] + local.aabbMaximum[0]) * 0.5f;
                labelAt[1] = local.aabbMaximum[1] + 0.5f;
                labelAt[2] = (local.aabbMinimum[2] + local.aabbMaximum[2]) * 0.5f;
            }
            else
            {
                labelAt[0] = local.centre[0];
                labelAt[1] = local.centre[1] + fabsf(local.halfExtent[1]) + 0.5f;
                labelAt[2] = local.centre[2];
            }
            DrawText3d(renderer, labelAt, local.label, 0xFF00FFFFu);
        }
    }

    SetDebugState(renderer, kSlotSetDepthWrite, true);
    SetDebugState(renderer, kSlotSetDepthTest, true);

    InterlockedIncrement(&g_frameDrawsIssued);
    g_frameDrawLines = lines;
}

static std::string DrawTestMarker(const float centre[3], float size, const char* label,
                                  float seconds)
{
    void* renderer = GetDebugRenderer();
    if (renderer == nullptr)
        return "DebugRenderer_current() returned nothing";

    uintptr_t vtable = 0;
    SafeRead(reinterpret_cast<uintptr_t>(renderer), &vtable);
    LogLine("draw: renderer 0x%p, vtable game+0x%llX", renderer,
            static_cast<unsigned long long>(vtable - g_base));

    SetDebugState(renderer, kSlotSetDepthTest, false);
    SetDebugState(renderer, kSlotSetDepthWrite, false);
    SetDebugTimeVisible(renderer, seconds);

    const float halfExtent[3] = { size, size, size };
    const int drawn = DrawBox3d(renderer, centre, halfExtent, 0xFFFFFFFFu);

    bool text = false;
    if (label != nullptr && *label != '\0')
        text = DrawText3d(renderer, centre, label, 0xFF00FFFFu);   // yellow, ABGR

    SetDebugState(renderer, kSlotSetDepthWrite, true);
    SetDebugState(renderer, kSlotSetDepthTest, true);

    char buffer[256];
    sprintf_s(buffer, "renderer 0x%p - %d of 12 lines, text %s, visible %.1f s",
              renderer, drawn, text ? "sent" : "failed", seconds);
    return buffer;
}

// ---------------------------------------------------------------------------
// Local player, input suppression, and the free camera object.
//
// Layouts here are BFII-exact, taken from Kyber's own SDK headers in the local
// corpus (Module/Public/SDK/SDK.h) rather than inferred:
//
//   ClientGameContext   clientLevel +0x38, playerManager +0x58, gameViews[2] +0x68
//   ClientPlayer        m_name +0x18, m_localPlayerId +0x38,
//                       m_analogInputEnableMask +0x3C,
//                       m_digitalInputEnableMask +0x40,
//                       controlledControllable +0x210, cameraViewManager +0x228
//   ClientGameView      freeCamera +0xB8
//   FreeCamera          transform +0xE0, targetPos +0x180
//
// The analog/digital mask offsets agree with what the RE agent gave us
// independently, which is the only reason they are trusted without a live read.
//
// The player manager's own member layout for its player vectors is inconsistent
// in that header (two members are annotated at the same offset), so rather than
// trust it, scan the manager object for pointers that look like a ClientPlayer:
// a heap pointer whose +0x18 reads as a printable name. Everything is guarded.
// ---------------------------------------------------------------------------
constexpr size_t kContextPlayerManager = 0x58;
constexpr size_t kContextGameViews = 0x68;

constexpr size_t kPlayerName = 0x18;
constexpr size_t kPlayerLocalId = 0x38;
constexpr size_t kPlayerAnalogMask = 0x3C;
constexpr size_t kPlayerDigitalMask = 0x40;
constexpr size_t kPlayerControlled = 0x210;
constexpr size_t kPlayerCameraViewManager = 0x228;

constexpr size_t kPlayerManagerSize = 0x2258;

constexpr size_t kGameViewFreeCamera = 0xB8;
constexpr size_t kFreeCameraTransform = 0xE0;

static uintptr_t GetClientGameContext()
{
    uintptr_t context = 0;
    SafeRead(g_base + kClientGameContextRva, &context);
    return context;
}

static bool LooksLikeClientPlayer(uintptr_t candidate)
{
    if (candidate < 0x10000 || (candidate & 7) != 0)
        return false;

    uintptr_t namePointer = 0;
    if (!SafeRead(candidate + kPlayerName, &namePointer) || namePointer < 0x10000)
        return false;

    char name[64] = {};
    if (!RawReadString(name, sizeof(name), namePointer, 63) || name[0] == '\0')
        return false;

    // A player name is short printable text, not arbitrary bytes that happened
    // to survive a string read.
    for (const char* c = name; *c != '\0'; ++c)
        if (*c < 0x20 || *c > 0x7E)
            return false;

    return true;
}

static std::vector<uintptr_t> FindClientPlayers()
{
    std::vector<uintptr_t> players;

    const uintptr_t context = GetClientGameContext();
    if (context == 0)
        return players;

    uintptr_t manager = 0;
    if (!SafeRead(context + kContextPlayerManager, &manager) || manager == 0)
        return players;

    for (size_t offset = 0; offset < kPlayerManagerSize; offset += sizeof(uintptr_t))
    {
        uintptr_t candidate = 0;
        if (!SafeRead(manager + offset, &candidate))
            continue;
        if (!LooksLikeClientPlayer(candidate))
            continue;

        bool already = false;
        for (size_t i = 0; i < players.size(); ++i)
            if (players[i] == candidate)
            {
                already = true;
                break;
            }

        if (!already)
            players.push_back(candidate);

        if (players.size() >= 16)
            break;
    }

    return players;
}

static std::string DescribeClientPlayers()
{
    const uintptr_t context = GetClientGameContext();
    if (context == 0)
        return "no client game context";

    uintptr_t manager = 0;
    SafeRead(context + kContextPlayerManager, &manager);

    char header[192];
    sprintf_s(header, "clientContext 0x%llX  playerManager 0x%llX\n",
              static_cast<unsigned long long>(context),
              static_cast<unsigned long long>(manager));
    std::string out(header);

    const std::vector<uintptr_t> players = FindClientPlayers();
    if (players.empty())
        return out + "  no ClientPlayer found - is a level loaded?\n";

    for (size_t i = 0; i < players.size(); ++i)
    {
        const uintptr_t player = players[i];

        uintptr_t namePointer = 0;
        SafeRead(player + kPlayerName, &namePointer);
        char name[64] = {};
        RawReadString(name, sizeof(name), namePointer, 63);

        uint32_t localId = 0, analog = 0;
        uint64_t digital = 0;
        uintptr_t controlled = 0, cameraManager = 0;
        SafeRead(player + kPlayerLocalId, &localId);
        SafeRead(player + kPlayerAnalogMask, &analog);
        SafeRead(player + kPlayerDigitalMask, &digital);
        SafeRead(player + kPlayerControlled, &controlled);
        SafeRead(player + kPlayerCameraViewManager, &cameraManager);

        char line[384];
        sprintf_s(line,
                  "  0x%llX '%s' localId %u analog 0x%08X digital 0x%016llX\n"
                  "      controlled 0x%llX  cameraViewManager 0x%llX\n",
                  static_cast<unsigned long long>(player), name, localId, analog,
                  static_cast<unsigned long long>(digital),
                  static_cast<unsigned long long>(controlled),
                  static_cast<unsigned long long>(cameraManager));
        out += line;
    }

    return out;
}

// ---------------------------------------------------------------------------
// Driving the camera properly.
//
// The RE agent settled this and it is simpler than either of us expected:
// `Camera::getTransformWithBase()` is an INLINE, NON-VIRTUAL member read
// (`Camera.h:71`). There is no slot to hook. The per-frame feed in
// ClientCameraViewManager::postFrameUpdate compiles to a direct read of
// `Camera::m_transform`, so the supported way to drive the view is to write
// that member.
//
// This is why the previous attempt looked so bad. Writing the RenderView put us
// downstream of the engine's own writer - two writers racing, hence the
// flicker. Writing the camera makes us the source, and the view manager
// snapshots it once per frame under its own lock. Worst case a pose lands one
// frame late. Nothing is patched, so stopping the writes hands control back.
//
// Basis convention, from the agent and corroborated by
// DebugRenderer2::drawCoordinateSystem drawing left/up/forward unflipped:
// Frostbite is LEFT-handed, Y up, forward = +Z, left = +X, rows stored
// left/up/forward/translation with .w = 1. No row is negated. The mirroring we
// saw was the RenderView race, not the basis order.
// ---------------------------------------------------------------------------
constexpr uintptr_t kGetActiveCameraRva = 0x546A700;
constexpr size_t kCameraTransformDefault = 0x120;      // Kyber's offset on this build

typedef void*(__fastcall* GetActiveCameraFn)(void* cameraViewManager);
typedef void*(__fastcall* CameraGetTransformFn)(void* camera);

static uintptr_t g_activeCamera = 0;
static size_t g_cameraTransformOffset = kCameraTransformDefault;
static volatile bool g_cameraDrive = false;

// The guarded calls live in their own raw-pointer helpers: MSVC will not allow
// __try in a function that owns objects needing unwinding, and the callers below
// hold std::string and std::vector.
static void* GuardedGetActiveCamera(GetActiveCameraFn fn, void* manager)
{
    __try
    {
        return fn(manager);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

static void* GuardedCameraGetTransform(CameraGetTransformFn fn, void* camera)
{
    __try
    {
        return fn(camera);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

static uintptr_t FindActiveCamera(std::string* how)
{
    const std::vector<uintptr_t> players = FindClientPlayers();
    if (players.empty())
    {
        *how = "no ClientPlayer - is a level loaded?";
        return 0;
    }

    for (size_t i = 0; i < players.size(); ++i)
    {
        uintptr_t manager = 0;
        if (!SafeRead(players[i] + kPlayerCameraViewManager, &manager) || manager == 0)
            continue;

        GetActiveCameraFn getActive =
            reinterpret_cast<GetActiveCameraFn>(g_base + kGetActiveCameraRva);

        void* camera = GuardedGetActiveCamera(getActive, reinterpret_cast<void*>(manager));
        if (camera == nullptr)
            continue;

        char note[160];
        sprintf_s(note, "player 0x%llX -> manager 0x%llX -> camera 0x%p",
                  static_cast<unsigned long long>(players[i]),
                  static_cast<unsigned long long>(manager), camera);
        *how = note;
        return reinterpret_cast<uintptr_t>(camera);
    }

    *how = "no camera view manager returned an active camera";
    return 0;
}

// Pin m_transform rather than trusting a constant: slot 4 is Kyber's proven
// getter and returns a pointer into the camera's own storage, so whichever
// offset holds that pointer's target IS the transform.
static bool CalibrateCameraTransform(uintptr_t camera, size_t* offsetOut, std::string* how)
{
    uintptr_t getterFn = 0;
    if (ReadVtableSlot(reinterpret_cast<void*>(camera), 4, &getterFn))
    {
        void* pointer = GuardedCameraGetTransform(
            reinterpret_cast<CameraGetTransformFn>(getterFn), reinterpret_cast<void*>(camera));

        const uintptr_t target = reinterpret_cast<uintptr_t>(pointer);
        if (target > camera && target - camera < 0x480)
        {
            *offsetOut = static_cast<size_t>(target - camera);
            char note[128];
            sprintf_s(note, "slot 4 returned camera+0x%zX", *offsetOut);
            *how = note;
            return true;
        }
    }

    // Fall back on the known offset, but only if what is there looks like an
    // orthonormal basis - a wrong offset would be written blind otherwise.
    Transform candidate{};
    if (SafeRead(camera + kCameraTransformDefault, &candidate))
    {
        const float lengthSquared =
            candidate.forward[0] * candidate.forward[0] +
            candidate.forward[1] * candidate.forward[1] +
            candidate.forward[2] * candidate.forward[2];
        if (lengthSquared > 0.9f && lengthSquared < 1.1f)
        {
            *offsetOut = kCameraTransformDefault;
            *how = "slot 4 unusable; +0x120 holds a unit forward vector";
            return true;
        }
    }

    *how = "could not pin m_transform";
    return false;
}

constexpr uintptr_t kGetActiveCameraTransformRva = 0xA7A990;
constexpr size_t kCamHookPatchSize = 12;

static unsigned char g_camHookOriginal[kCamHookPatchSize];
static bool g_camHookInstalled = false;
static void* g_camHookStub = nullptr;
alignas(16) static Transform g_camHookTransform;

static DWORD WINAPI CameraDriveThread(LPVOID)
{
    DWORD last = GetTickCount();

    for (;;)
    {
        if (!g_cameraDrive || g_activeCamera == 0)
        {
            last = GetTickCount();
            Sleep(16);
            continue;
        }

        const DWORD now = GetTickCount();
        float seconds = (now - last) / 1000.0f;
        last = now;
        if (seconds > 0.1f)
            seconds = 0.1f;

        StepFlyCamera(seconds);

        EnterCriticalSection(&g_cameraLock);
        Transform copy = g_cameraTarget;
        LeaveCriticalSection(&g_cameraLock);

        // The engine reads this once per frame; writing at 8 ms is ample and
        // does not race anything, because nothing else writes it.
        if (g_camHookInstalled)
            g_camHookTransform = copy;          // the stub reads this every frame
        else
            SafeWriteBytes(g_activeCamera + g_cameraTransformOffset, &copy, sizeof(copy));
        Sleep(8);
    }
}

static std::string TakeCamera(bool enable)
{
    if (!enable)
    {
        g_cameraDrive = false;
        LogLine("camera: released - the engine resumes on its next frame");
        return "camera released";
    }

    std::string how;
    const uintptr_t camera = FindActiveCamera(&how);
    if (camera == 0)
        return std::string("camera not found: ") + how;

    size_t offset = kCameraTransformDefault;
    std::string calibration;
    if (!CalibrateCameraTransform(camera, &offset, &calibration))
        return std::string("found camera 0x") + "" + " but " + calibration;

    Transform current{};
    if (!SafeRead(camera + offset, &current))
        return "the camera transform is unreadable";

    g_activeCamera = camera;
    g_cameraTransformOffset = offset;

    EnterCriticalSection(&g_cameraLock);
    g_cameraTarget = current;          // start from the engine's own pose, verbatim
    LeaveCriticalSection(&g_cameraLock);

    g_cameraDrive = true;

    LogLine("camera: driving 0x%llX + 0x%zX (%s; %s) from %.1f %.1f %.1f",
            static_cast<unsigned long long>(camera), offset, how.c_str(),
            calibration.c_str(), current.translation[0], current.translation[1],
            current.translation[2]);

    char buffer[384];
    sprintf_s(buffer,
              "driving camera 0x%llX + 0x%zX\n  %s\n  %s\n"
              "  at %.2f %.2f %.2f  forward %.3f %.3f %.3f\n"
              "  WASD move, Q/E down/up, arrows look, shift fast (game must have focus)",
              static_cast<unsigned long long>(camera), offset, how.c_str(),
              calibration.c_str(),
              current.translation[0], current.translation[1], current.translation[2],
              current.forward[0], current.forward[1], current.forward[2]);
    return buffer;
}

// ---------------------------------------------------------------------------
// Finding Camera::m_transform by its contents.
//
// `getActiveCamera` returned an object whose +0x120 holds pointers, not a
// basis - so either that is not the camera the view is being fed from, or
// m_transform is not at Kyber's offset on this build. Rather than guess again,
// find it by what it contains: the render view's transform is a copy of the
// camera's, so every location in the process holding those same twelve floats
// is a candidate, and one of them is the camera.
//
// Live confirmation that the render view is read correctly, from a loaded Naboo:
//   left     0.9414  0.0107 -0.3371
//   up      -0.0138  0.9999 -0.0069
//   forward  0.3370  0.0111  0.9414
//   position 0.455  -2998.395 2.194
// Each row is unit length and left is perpendicular to forward, so the 16-byte
// row stride we have been reading with is right.
// ---------------------------------------------------------------------------
static bool NearlyEqual(float a, float b, float tolerance)
{
    const float difference = a - b;
    return difference > -tolerance && difference < tolerance;
}

// The scan itself owns no C++ objects, because MSVC will not allow __try in a
// function that does.
static size_t ScanRegionForTransform(uintptr_t regionBase, uintptr_t regionEnd,
                                     const Transform& wanted, uintptr_t skip,
                                     uintptr_t* out, size_t cap)
{
    size_t found = 0;

    __try
    {
        // Match on the position first - three floats, cheap, and far more
        // distinctive than a basis vector sitting near zero.
        // 16-aligned only: a LinearTransform never starts anywhere else, and
        // testing the other three positions in four was most of the cost.
        uintptr_t start = (regionBase + 0xF) & ~static_cast<uintptr_t>(0xF);

        for (uintptr_t at = start; at + 0x40 <= regionEnd && found < cap; at += 16)
        {
            const float* candidate = reinterpret_cast<const float*>(at);

            // Match the basis, not the position. The camera may store a
            // transform relative to a base translation, in which case its
            // position never equals the render view's world position - which is
            // exactly how a 64-candidate sweep found only copies.
            if (!NearlyEqual(candidate[0], wanted.left[0], 0.002f) ||
                !NearlyEqual(candidate[1], wanted.left[1], 0.002f) ||
                !NearlyEqual(candidate[2], wanted.left[2], 0.002f))
                continue;

            if (!NearlyEqual(candidate[4], wanted.up[0], 0.002f) ||
                !NearlyEqual(candidate[5], wanted.up[1], 0.002f) ||
                !NearlyEqual(candidate[6], wanted.up[2], 0.002f))
                continue;

            if (!NearlyEqual(candidate[8], wanted.forward[0], 0.002f) ||
                !NearlyEqual(candidate[9], wanted.forward[1], 0.002f) ||
                !NearlyEqual(candidate[10], wanted.forward[2], 0.002f))
                continue;

            if (at == skip)
                continue;

            out[found++] = at;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    return found;
}

// ---------------------------------------------------------------------------
// Identifying the camera by observed effect rather than by layout.
//
// Two layout guesses have now missed, and a list of candidate addresses is no
// better - most of them are copies, and writing to a copy does nothing visible.
// So test them: nudge each candidate, watch whether the render view follows,
// and put it back if it does not. The one the render view follows IS the
// camera, by definition, and no offset had to be right for us to find it.
// ---------------------------------------------------------------------------
static size_t CollectTransformCandidates(const Transform& wanted, uintptr_t skip,
                                         uintptr_t* out, size_t cap)
{
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t address = 0;
    size_t found = 0;

    while (address < 0x200000000000ull && found < cap &&
           VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == sizeof(mbi))
    {
        const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t regionEnd = regionBase + mbi.RegionSize;

        const bool writable =
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE)) != 0;

        if (mbi.State == MEM_COMMIT && writable &&
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
            mbi.Type == MEM_PRIVATE)
        {
            found += ScanRegionForTransform(regionBase, regionEnd, wanted, skip,
                                            out + found, cap - found);
        }

        if (regionEnd <= regionBase)
            break;
        address = regionEnd;
    }

    return found;
}

static std::string AutoDriveCameraBlocking()
{
    Transform original{};
    float fov = 0.0f;
    if (!ReadCamera(&original, &fov))
        return "no render view - is a level loaded?";

    const uintptr_t renderView = GetRenderView();

    static uintptr_t candidates[512];
    const size_t count = CollectTransformCandidates(original, renderView, candidates, 512);

    char header[256];
    sprintf_s(header, "%zu candidate(s) hold the current transform; testing each\n", count);
    std::string out(header);

    if (count == 0)
        return out + "  nothing to test - the render view's transform is not mirrored anywhere\n";

    for (size_t i = 0; i < count; ++i)
    {
        const uintptr_t candidate = candidates[i];

        Transform saved{};
        if (!SafeRead(candidate, &saved))
            continue;

        // Nudge it well clear of anything that could be coincidence.
        Transform probe = saved;
        probe.translation[1] += 500.0f;
        if (!SafeWriteBytes(candidate, &probe, sizeof(probe)))
            continue;

        // Give the engine a few frames to consume it. Kept short because the
        // sweep may have hundreds of candidates to get through.
        Sleep(60);

        Transform now{};
        float ignored = 0.0f;
        const bool moved = ReadCamera(&now, &ignored) &&
                           now.translation[1] > original.translation[1] + 100.0f;

        char line[288];
        sprintf_s(line, "  0x%llX holds %.1f %.1f %.1f -> render view y %.1f  %s\n",
                  static_cast<unsigned long long>(candidate),
                  saved.translation[0], saved.translation[1], saved.translation[2],
                  now.translation[1],
                  moved ? "*** THIS IS THE CAMERA ***" : "no effect");
        out += line;

        if (!moved)
        {
            SafeWriteBytes(candidate, &saved, sizeof(saved));   // leave it as found
            continue;
        }

        g_activeCamera = candidate;
        g_cameraTransformOffset = 0;

        EnterCriticalSection(&g_cameraLock);
        g_cameraTarget = saved;        // start from the pose the engine had
        LeaveCriticalSection(&g_cameraLock);

        g_cameraDrive = true;

        LogLine("camera: auto-identified 0x%llX and now driving it",
                static_cast<unsigned long long>(candidate));

        char tail[256];
        sprintf_s(tail,
                  "driving 0x%llX at %.2f %.2f %.2f\n"
                  "  click the game window: WASD move, Q/E down/up, arrows look, shift fast\n"
                  "  'camto <x> <y> <z>' to jump, 'camdrive 0' to release\n",
                  static_cast<unsigned long long>(candidate), saved.translation[0],
                  saved.translation[1], saved.translation[2]);
        return out + tail;
    }

    return out + "  none of them moved the view - the camera is fed from somewhere else\n";
}

static std::string FindCameraByTransform(unsigned cap)
{
    Transform wanted{};
    float fov = 0.0f;
    if (!ReadCamera(&wanted, &fov))
        return "no render view - is a level loaded?";

    const uintptr_t renderView = GetRenderView();

    char header[320];
    sprintf_s(header,
              "looking for position %.3f %.3f %.3f  forward %.4f %.4f %.4f\n",
              wanted.translation[0], wanted.translation[1], wanted.translation[2],
              wanted.forward[0], wanted.forward[1], wanted.forward[2]);
    std::string out(header);

    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t address = 0;
    unsigned reported = 0;
    uintptr_t batch[64];

    while (address < 0x200000000000ull && reported < cap &&
           VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == sizeof(mbi))
    {
        const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t regionEnd = regionBase + mbi.RegionSize;

        const bool writable =
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE)) != 0;

        if (mbi.State == MEM_COMMIT && writable &&
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
            mbi.Type == MEM_PRIVATE)
        {
            const size_t hits = ScanRegionForTransform(regionBase, regionEnd, wanted,
                                                       renderView, batch,
                                                       cap - reported);
            for (size_t i = 0; i < hits; ++i)
            {
                char line[224];
                sprintf_s(line, "  0x%llX%s\n",
                          static_cast<unsigned long long>(batch[i]),
                          (batch[i] & 0xF) == 0 ? "  (16-aligned)" : "");
                out += line;
                ++reported;
            }
        }

        if (regionEnd <= regionBase)
            break;
        address = regionEnd;
    }

    if (reported == 0)
        out += "  nothing else in the process holds this transform\n";
    return out;
}

static volatile bool g_camAutoRunning = false;
static char g_camAutoResult[8192];

static DWORD WINAPI AutoDriveCameraThread(LPVOID)
{
    const std::string result = AutoDriveCameraBlocking();

    strncpy_s(g_camAutoResult, sizeof(g_camAutoResult), result.c_str(), _TRUNCATE);
    LogLine("camauto finished:\n%s", g_camAutoResult);

    g_camAutoRunning = false;
    return 0;
}

// ---------------------------------------------------------------------------
// Intercepting the camera where it is USED, not where it is stored.
//
// Three sweeps have now proven there is nothing worth writing. The last one
// found the transform mirrored in dozens of places - several of them holding it
// relative, at position 0,0,0 - and nudging every one of them moved nothing.
// The conclusion is that on a deploy screen the pose is recomputed by game logic
// every frame, so any stored copy we write is overwritten before it is read.
// There is no static camera to hijack.
//
// So intercept the read. `ClientCameraViewManager_getActiveCameraTransform`
// (RVA 0xA7A990, from the RE agent) runs every frame on the client thread and
// hands the caller a transform through an out-pointer in RDX. Replace it with a
// stub that writes ours and returns.
//
// The prologue, read live from the running game:
//
//   48 8B C4              mov  rax, rsp
//   57                    push rdi
//   48 83 EC 70           sub  rsp, 0x70
//   48 C7 40 A8 FE FF FF FF
//   ...
//
// Nothing there is rip-relative, and because the stub never calls the original
// there is no trampoline to build and no instruction-length decoding to get
// wrong - the twelve bytes are simply saved and put back on release.
// ---------------------------------------------------------------------------


static void* BuildCameraStub(const void* source)
{
    void* stub = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (stub == nullptr)
        return nullptr;

    unsigned char* code = static_cast<unsigned char*>(stub);
    size_t at = 0;

    // mov rax, <source>
    code[at++] = 0x48; code[at++] = 0xB8;
    memcpy(code + at, &source, sizeof(source));
    at += sizeof(source);

    // Four 16-byte moves: xmm0 <- [rax+n], [rdx+n] <- xmm0.
    for (unsigned char offset = 0; offset < 0x40; offset += 0x10)
    {
        code[at++] = 0x0F; code[at++] = 0x10; code[at++] = 0x40; code[at++] = offset;
        code[at++] = 0x0F; code[at++] = 0x11; code[at++] = 0x42; code[at++] = offset;
    }

    // mov eax, 1 ; ret   - report success to whatever asked.
    code[at++] = 0xB8; code[at++] = 0x01; code[at++] = 0x00; code[at++] = 0x00; code[at++] = 0x00;
    code[at++] = 0xC3;

    return stub;
}

constexpr size_t kCaptureStolenBytes = 16;

typedef __int64(__fastcall* CameraFeedFn)(void* holder, void* pose);

static CameraFeedFn g_captureTrampoline = nullptr;
static unsigned char g_captureOriginal[kCaptureStolenBytes];
static bool g_captureInstalled = false;
static volatile LONG g_captureCalls = 0;
static volatile bool g_captureDrive = false;

// Runs on the engine's own thread, every frame, in the middle of the camera
// flow. Keep it cheap: the first few calls are logged and after that it either
// does nothing or overwrites the pose.
static __int64 __fastcall CameraFeedDetour(void* holder, void* pose)
{
    const __int64 result = g_captureTrampoline(holder, pose);

    const LONG call = InterlockedIncrement(&g_captureCalls);
    if (call <= 3 && pose != nullptr)
    {
        Transform produced{};
        if (SafeRead(reinterpret_cast<uintptr_t>(pose), &produced))
            LogLine("capture #%ld: holder 0x%p returned %lld, pose at 0x%p = "
                    "%.3f %.3f %.3f (fwd %.3f %.3f %.3f)",
                    call, holder, result, pose,
                    produced.translation[0], produced.translation[1], produced.translation[2],
                    produced.forward[0], produced.forward[1], produced.forward[2]);
        else
            LogLine("capture #%ld: holder 0x%p returned %lld, pose 0x%p unreadable",
                    call, holder, result, pose);
    }

    if (g_captureDrive && pose != nullptr)
    {
        EnterCriticalSection(&g_cameraLock);
        Transform wanted = g_cameraTarget;
        LeaveCriticalSection(&g_cameraLock);

        memcpy(pose, &wanted, sizeof(wanted));
    }

    return result;
}

static DWORD WINAPI WatchDriveStepperThread(LPVOID)
{
    DWORD last = GetTickCount();

    while (g_watchDrive)
    {
        const DWORD now = GetTickCount();
        float seconds = (now - last) / 1000.0f;
        last = now;
        if (seconds > 0.1f)
            seconds = 0.1f;

        StepFlyCamera(seconds);

        EnterCriticalSection(&g_cameraLock);
        Transform copy = g_cameraTarget;
        LeaveCriticalSection(&g_cameraLock);

        memcpy(g_watchPose, &copy, sizeof(copy));
        Sleep(4);
    }

    return 0;
}

// One line of debug text in the top-left corner: camera position (metres, level space -- what census/placement data
// uses) and heading. Kept visible a little longer than the draw interval so it does not flicker.
static void DrawCoordsOnFrameThread()
{
    void* renderer = GetDebugRenderer();
    if (renderer == nullptr)
        return;
    uintptr_t drawFn = 0;
    if (!ReadVtableSlot(renderer, kSlotDrawTextScreen, &drawFn))
        return;
    Transform camera{};
    float fov = 0.0f;
    if (!ReadCamera(&camera, &fov))
        return;
    const float yaw = atan2f(camera.forward[0], camera.forward[2]) * 57.29578f;
    const float pitch = asinf(camera.forward[1] < -1.0f ? -1.0f : (camera.forward[1] > 1.0f ? 1.0f : camera.forward[1])) * 57.29578f;
    char text[160];
    sprintf_s(text, "X %.1f   Y %.1f   Z %.1f      yaw %.0f   pitch %.0f",
              camera.translation[0], camera.translation[1], camera.translation[2], yaw, pitch);
    SetDebugTimeVisible(renderer, 0.25f);
    const int height = GetSystemMetrics(SM_CYSCREEN);
    reinterpret_cast<DrawTextScreenFn>(drawFn)(renderer, 40, height / 5, text, 0xFF00FFFFu, 1.6f, 0, 0.0f, 0xFFFFFFFFu);
}

// Keeps the observe trap on the CURRENT render view: it is rebuilt on respawn and camera changes, and a trap on a stale
// view never fires again.
static DWORD WINAPI CoordsRearmThread(LPVOID)
{
    while (g_coordsOverlay)
    {
        const uintptr_t view = GetRenderView();
        if (view != 0 && view != g_watchWriteBase)
        {
            g_watchWriteBase = view;
            g_fovAddress = view + kRenderViewFovOffset;
            g_watchAddress = view + 0x3C;
            ArmHardwareWatch(g_watchAddress, 4);
            LogLine("coords: re-armed on render view 0x%llX", static_cast<unsigned long long>(view));
        }
        Sleep(1000);
    }
    return 0;
}

static std::string CoordsOverlay(bool enable)
{
    if (!enable)
    {
        g_coordsOverlay = false;
        if (g_watchObserve)
        {
            g_watchDrive = false;
            g_watchAddress = 0;
            g_watchWriteBase = 0;
            ArmHardwareWatch(0, 4);
            g_watchObserve = false;
        }
        return "coords off";
    }
    if (g_watchDrive && !g_watchObserve)
        return "the camera is being driven (watchdrive) -- release it first";
    const uintptr_t view = GetRenderView();
    if (view == 0)
        return "no render view - is a level loaded?";
    g_watchObserve = true;
    g_watchWriteBase = view;
    g_fovAddress = view + kRenderViewFovOffset;
    g_watchAddress = view + 0x3C;
    g_watchHits = 0;
    g_watchDrive = true;
    if (ArmHardwareWatch(g_watchAddress, 4) <= 0)
    {
        g_watchDrive = false;
        g_watchObserve = false;
        return "could not arm the breakpoint on any thread";
    }
    g_coordsOverlay = true;
    HANDLE t = CreateThread(nullptr, 0, CoordsRearmThread, nullptr, 0, nullptr);
    if (t != nullptr)
        CloseHandle(t);
    return "coords on (camera position + heading, top left)";
}

static std::string DriveViaWatch(bool enable)
{
    if (!enable)
    {
        g_watchDrive = false;
        g_watchAddress = 0;
        g_watchWriteBase = 0;
        ArmHardwareWatch(0, 4);
        LogLine("watchdrive: released");
        return "released - the engine has its camera back";
    }

    const uintptr_t view = GetRenderView();
    if (view == 0)
        return "no render view - is a level loaded?";

    Transform current{};
    float fov = 0.0f;
    if (!ReadCamera(&current, &fov))
        return "the render view transform is unreadable";

    EnterCriticalSection(&g_cameraLock);
    g_cameraTarget = current;
    LeaveCriticalSection(&g_cameraLock);
    memcpy(g_watchPose, &current, sizeof(current));

    // Watch the final four bytes; write the first sixty. The two must not
    // overlap or the handler traps on its own write.
    const uintptr_t watched = view + 0x3C;

    g_watchWriteBase = view;
    g_fovAddress = view + kRenderViewFovOffset;
    g_watchAddress = watched;
    g_watchHits = 0;
    g_watchDrive = true;

    const int armed = ArmHardwareWatch(watched, 4);
    if (armed <= 0)
    {
        g_watchDrive = false;
        return "could not arm the breakpoint on any thread";
    }

    HANDLE stepper = CreateThread(nullptr, 0, WatchDriveStepperThread, nullptr, 0, nullptr);
    if (stepper != nullptr)
        CloseHandle(stepper);

    LogLine("watchdrive: writing 0x%llX, watching 0x%llX, across %d threads, "
            "from %.1f %.1f %.1f",
            static_cast<unsigned long long>(view),
            static_cast<unsigned long long>(watched), armed,
            current.translation[0], current.translation[1], current.translation[2]);

    char buffer[320];
    sprintf_s(buffer,
              "driving the render view at 0x%llX across %d threads\n"
              "  seeded at %.2f %.2f %.2f\n"
              "  click the game window: WASD move, Q/E down/up, arrows look, shift fast\n"
              "  'camto <x> <y> <z>' to jump, 'watchdrive 0' to hand it back",
              static_cast<unsigned long long>(view), armed,
              current.translation[0], current.translation[1], current.translation[2]);
    return buffer;
}

static std::string InstallCameraCapture(bool enable, bool drive)
{
    const uintptr_t target = g_base + kGetActiveCameraTransformRva;

    if (!enable)
    {
        if (!g_captureInstalled)
            return "the capture hook is not installed";

        DWORD previous = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(target), kCaptureStolenBytes,
                            PAGE_EXECUTE_READWRITE, &previous))
            return "could not make the function writable to restore it";

        memcpy(reinterpret_cast<void*>(target), g_captureOriginal, kCaptureStolenBytes);

        DWORD ignored = 0;
        VirtualProtect(reinterpret_cast<void*>(target), kCaptureStolenBytes, previous, &ignored);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target),
                              kCaptureStolenBytes);

        g_captureInstalled = false;
        g_captureDrive = false;
        LogLine("capture hook removed after %ld calls", g_captureCalls);

        char buffer[128];
        sprintf_s(buffer, "capture hook removed - it saw %ld calls", g_captureCalls);
        return buffer;
    }

    if (g_captureInstalled)
    {
        g_captureDrive = drive;
        char buffer[160];
        sprintf_s(buffer, "already installed - %ld calls so far, drive %s",
                  g_captureCalls, drive ? "on" : "off");
        return buffer;
    }

    if (!SafeRead(target,
                  reinterpret_cast<unsigned char(*)[kCaptureStolenBytes]>(g_captureOriginal)))
        return "could not read the function's first bytes";

    // Trampoline: the stolen instructions, then an absolute jump back past them.
    unsigned char* trampoline = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (trampoline == nullptr)
        return "could not allocate the trampoline";

    memcpy(trampoline, g_captureOriginal, kCaptureStolenBytes);

    const uintptr_t resume = target + kCaptureStolenBytes;
    size_t at = kCaptureStolenBytes;
    trampoline[at++] = 0x48; trampoline[at++] = 0xB8;          // mov rax, resume
    memcpy(trampoline + at, &resume, sizeof(resume));
    at += sizeof(resume);
    trampoline[at++] = 0xFF; trampoline[at++] = 0xE0;          // jmp rax

    g_captureTrampoline = reinterpret_cast<CameraFeedFn>(trampoline);

    // Patch: absolute jump to the detour, padded to the stolen length.
    unsigned char patch[kCaptureStolenBytes];
    memset(patch, 0x90, sizeof(patch));
    void* detour = reinterpret_cast<void*>(&CameraFeedDetour);
    patch[0] = 0x48; patch[1] = 0xB8;
    memcpy(patch + 2, &detour, sizeof(detour));
    patch[10] = 0xFF; patch[11] = 0xE0;

    DWORD previous = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(target), kCaptureStolenBytes,
                        PAGE_EXECUTE_READWRITE, &previous))
        return "could not make the function writable";

    memcpy(reinterpret_cast<void*>(target), patch, kCaptureStolenBytes);

    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(target), kCaptureStolenBytes, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target),
                          kCaptureStolenBytes);

    g_captureCalls = 0;
    g_captureInstalled = true;
    g_captureDrive = drive;

    // Seed from whatever is on screen so driving does not jump.
    Transform current{};
    float fov = 0.0f;
    if (ReadCamera(&current, &fov))
    {
        EnterCriticalSection(&g_cameraLock);
        g_cameraTarget = current;
        LeaveCriticalSection(&g_cameraLock);
    }

    LogLine("capture hook installed at game+0x%llX, trampoline 0x%p, drive %d",
            static_cast<unsigned long long>(kGetActiveCameraTransformRva), trampoline,
            drive ? 1 : 0);

    char buffer[288];
    sprintf_s(buffer,
              "capture hook installed at game+0x%llX (drive %s)\n"
              "  the original still runs; the first three calls are logged\n"
              "  'capture' again to see the call count, 'capture 0' to remove",
              static_cast<unsigned long long>(kGetActiveCameraTransformRva),
              drive ? "on" : "off");
    return buffer;
}

static std::string InstallCameraHook(bool enable)
{
    const uintptr_t target = g_base + kGetActiveCameraTransformRva;

    if (!enable)
    {
        if (!g_camHookInstalled)
            return "the camera hook is not installed";

        DWORD previous = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(target), kCamHookPatchSize,
                            PAGE_EXECUTE_READWRITE, &previous))
            return "could not make the function writable to restore it";

        memcpy(reinterpret_cast<void*>(target), g_camHookOriginal, kCamHookPatchSize);

        DWORD ignored = 0;
        VirtualProtect(reinterpret_cast<void*>(target), kCamHookPatchSize, previous, &ignored);

        g_camHookInstalled = false;
        g_cameraDrive = false;
        LogLine("camera hook removed - the engine's own function is back");
        return "camera hook removed";
    }

    if (g_camHookInstalled)
        return "the camera hook is already installed";

    // Seed from the pose currently on screen so taking control does not jump.
    Transform current{};
    float fov = 0.0f;
    if (ReadCamera(&current, &fov))
    {
        g_camHookTransform = current;
        EnterCriticalSection(&g_cameraLock);
        g_cameraTarget = current;
        LeaveCriticalSection(&g_cameraLock);
    }

    if (g_camHookStub == nullptr)
        g_camHookStub = BuildCameraStub(&g_camHookTransform);
    if (g_camHookStub == nullptr)
        return "could not allocate the stub";

    if (!SafeRead(target, reinterpret_cast<unsigned char(*)[kCamHookPatchSize]>(g_camHookOriginal)))
        return "could not read the function's first bytes";

    unsigned char patch[kCamHookPatchSize];
    patch[0] = 0x48; patch[1] = 0xB8;                       // mov rax, <stub>
    memcpy(patch + 2, &g_camHookStub, sizeof(g_camHookStub));
    patch[10] = 0xFF; patch[11] = 0xE0;                     // jmp rax

    DWORD previous = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(target), kCamHookPatchSize,
                        PAGE_EXECUTE_READWRITE, &previous))
        return "could not make the function writable";

    memcpy(reinterpret_cast<void*>(target), patch, kCamHookPatchSize);

    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(target), kCamHookPatchSize, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target),
                          kCamHookPatchSize);

    g_camHookInstalled = true;
    g_cameraDrive = true;
    g_activeCamera = reinterpret_cast<uintptr_t>(&g_camHookTransform);
    g_cameraTransformOffset = 0;

    LogLine("camera hook installed at game+0x%llX -> stub 0x%p, seeded at %.1f %.1f %.1f",
            static_cast<unsigned long long>(kGetActiveCameraTransformRva), g_camHookStub,
            current.translation[0], current.translation[1], current.translation[2]);

    char buffer[384];
    sprintf_s(buffer,
              "camera hook installed at game+0x%llX\n"
              "  seeded at %.2f %.2f %.2f\n"
              "  click the game window: WASD move, Q/E down/up, arrows look, shift fast\n"
              "  'camto <x> <y> <z>' to jump, 'camhook 0' to put the engine back",
              static_cast<unsigned long long>(kGetActiveCameraTransformRva),
              current.translation[0], current.translation[1], current.translation[2]);
    return buffer;
}

// ---------------------------------------------------------------------------
// Spectator mode - the shipped free camera.
//
// This is what Kyber actually does. Its module never implements a camera; it
// sets one boolean on the network join message and lets BFII's own spectator
// mode supply the free camera. The reflection registry confirms the whole
// feature is present in this build:
//
//   ClientSettings::IsSpectator            +0x08C
//   GameSettings::MaxSpectatorCount        +0x050
//   SpectatorFreeCameraData  RotateSpeed +0x088, MoveSpeed +0x08C
//   UIInputAction_SpectatorViewFreeCam     (the input action that selects it)
//
// Timing matters more than the write does. Kyber sets its flag BEFORE the client
// state change that joins the session, so this arms a thread that writes as soon
// as the registry is readable - which is well before the level finishes loading -
// and keeps re-writing for a while in case the join path resets it.
// ---------------------------------------------------------------------------
static volatile bool g_spectateWanted = false;

static int WriteSettingByNameUnlocked(const char* typeName, const char* fieldName,
                                      long long value, size_t width)
{
    if (!WalkRegistryUnlocked())
        return -1;

    const TypeRec* type = FindType(typeName);
    if (type == nullptr)
        return -2;

    uint32_t offset = 0;
    if (!ResolveField(typeName, fieldName, &offset))
        return -3;

    // FindInstances walks all of memory (~30 s in a level), so the instances are remembered per
    // type and re-checked on use - still [vtable in module][this type's ClassInfo] - and the scan
    // runs again only when none of them is (a level change frees them).
    static std::unordered_map<std::string, std::vector<uintptr_t>> cache;
    std::vector<uintptr_t> instances;
    auto cached = cache.find(typeName);
    if (cached != cache.end())
        for (uintptr_t candidate : cached->second)
        {
            uintptr_t vtable = 0, info = 0;
            if (SafeRead(candidate, &vtable) && vtable >= g_base && vtable < g_base + g_size &&
                SafeRead(candidate + 8, &info) && (info == type->classInfo || info == type->typeInfo))
                instances.push_back(candidate);
        }
    if (instances.empty())
    {
        instances = FindInstances(*type);
        cache[typeName] = instances;
    }
    if (instances.empty())
        return -4;

    int written = 0;
    for (size_t i = 0; i < instances.size(); ++i)
        if (SafeWriteBytes(instances[i] + offset, &value, width))
            ++written;

    return written;
}

static int WriteSettingByName(const char* typeName, const char* fieldName,
                              long long value, size_t width)
{
    LockRegistry();
    const int result = WriteSettingByNameUnlocked(typeName, fieldName, value, width);
    UnlockRegistry();
    return result;
}

static DWORD WINAPI SpectateThread(LPVOID)
{
    // Try early and keep trying: the registry is not constructed for the first
    // few seconds, and the join path may clear the flag once after we set it.
    for (int i = 0; i < 240 && g_spectateWanted; ++i)      // two minutes at 500 ms
    {
        Sleep(500);

        const int spectator = WriteSettingByName("ClientSettings", "IsSpectator", 1, 1);
        if (spectator < 0)
            continue;

        const int maxSpectators =
            WriteSettingByName("GameSettings", "MaxSpectatorCount", 8, 4);

        if ((i % 10) == 0 || spectator > 0)
            LogLine("spectate: IsSpectator written to %d instance(s), "
                    "MaxSpectatorCount to %d", spectator, maxSpectators);
    }

    LogLine("spectate: thread finished");
    return 0;
}

// ---------------------------------------------------------------------------
// Trying to make a SpectatorFreeCamera the active one.
//
// `ClientCameraViewManager_getActiveCamera` decodes as `mov rax,[rcx+0xF8]; ret`
// - read live from the running game - so "the active camera" is a single pointer
// at manager+0xF8. Whether writing it is enough is unknown; the engine may also
// keep a camera stack, a priority, or an activation flag. This tries it and puts
// it back, which is the cheapest way to find out and costs nothing if it fails.
//
// Instance addresses only became trustworthy once the scan started rejecting the
// registry's own type records, so this is the first point at which the attempt
// is even worth making.
// ---------------------------------------------------------------------------
constexpr size_t kManagerActiveCamera = 0xF8;

static uintptr_t g_savedActiveCamera = 0;
static uintptr_t g_activeCameraSlot = 0;

static uintptr_t FindLocalCameraViewManager()
{
    const std::vector<uintptr_t> players = FindClientPlayers();
    for (size_t i = 0; i < players.size(); ++i)
    {
        uintptr_t manager = 0;
        if (!SafeRead(players[i] + kPlayerCameraViewManager, &manager) || manager == 0)
            continue;

        // A real manager is a heap object, not the garbage that turns up on the
        // false-positive player records the name scan also finds.
        if (manager < 0x10000 || (manager & 7) != 0)
            continue;

        uintptr_t active = 0;
        if (!SafeRead(manager + kManagerActiveCamera, &active))
            continue;

        return manager;
    }
    return 0;
}

static std::string ActivateSpectatorFreeCamera(bool enable, const char* typeName)
{
    const uintptr_t manager = FindLocalCameraViewManager();
    if (manager == 0)
        return "no camera view manager on any local player";

    g_activeCameraSlot = manager + kManagerActiveCamera;

    if (!enable)
    {
        if (g_savedActiveCamera == 0)
            return "nothing to restore";

        SafeWriteBytes(g_activeCameraSlot, &g_savedActiveCamera, sizeof(g_savedActiveCamera));
        LogLine("activecam: restored 0x%llX",
                static_cast<unsigned long long>(g_savedActiveCamera));
        g_savedActiveCamera = 0;
        return "previous active camera restored";
    }

    if (!WalkRegistry())
        return "registry walk failed";

    const TypeRec* type = FindType(typeName);
    if (type == nullptr)
        return std::string("no such type: ") + typeName;

    const std::vector<uintptr_t> instances = FindInstances(*type, 16);
    if (instances.empty())
        return std::string("no live instances of ") + typeName;

    uintptr_t current = 0;
    SafeRead(g_activeCameraSlot, &current);
    if (g_savedActiveCamera == 0)
        g_savedActiveCamera = current;

    char header[256];
    sprintf_s(header, "manager 0x%llX, active camera slot +0x%02zX currently 0x%llX\n",
              static_cast<unsigned long long>(manager), kManagerActiveCamera,
              static_cast<unsigned long long>(current));
    std::string out(header);

    for (size_t i = 0; i < instances.size(); ++i)
    {
        if (!SafeWriteBytes(g_activeCameraSlot, &instances[i], sizeof(instances[i])))
            continue;

        Sleep(250);

        Transform now{};
        float fov = 0.0f;
        ReadCamera(&now, &fov);

        char line[256];
        sprintf_s(line, "  wrote %s 0x%llX -> view at %.1f %.1f %.1f\n", typeName,
                  static_cast<unsigned long long>(instances[i]),
                  now.translation[0], now.translation[1], now.translation[2]);
        out += line;

        LogLine("activecam: wrote 0x%llX, view now %.1f %.1f %.1f",
                static_cast<unsigned long long>(instances[i]),
                now.translation[0], now.translation[1], now.translation[2]);
    }

    out += "  left the last one in place - 'activecam 0' restores the original\n";
    return out;
}

// ---------------------------------------------------------------------------
// Editor view settings, taken from a tool that already does this.
//
// OpenGameCamera (ArmchairDevelopers, GPL, pinned in plugin-corpus) is a working
// free-camera tool for this exact game, and its SDK names the fields we had been
// hunting for:
//
//   UISettings.drawEnable        +0x44   the HUD toggle - their F7
//   GameRenderSettings.forceFov          a real FOV override, -1 to disable
//   GameRenderSettings.resolutionScale
//   GameTimeSettings.timeScale   +0x38   their F8 freeze
//
// We reach all of them by NAME through the reflection registry rather than by
// their signature-scanned static offsets, which is both more robust and less
// work - the registry already resolves every field on every registered type.
//
// The engine rewrites settings, so like the spectator flag these are re-applied
// on a timer rather than written once. One list, one thread, so adding a new
// one later is a single entry.
// ---------------------------------------------------------------------------
struct EnforcedSetting
{
    const char* type;
    const char* field;
    long long value;
    size_t width;
    bool active;
};

static EnforcedSetting g_enforced[] = {
    { "UISettings",         "DrawEnable",      1,  1, false },
    { "GameRenderSettings", "ForceFov",        0,  4, false },
    { "GameTimeSettings",   "TimeScale",       0,  4, false },
    { "GameRenderSettings", "ResolutionScale", 0,  4, false },
    // The retail gate on 3D debug geometry. Kyber's SDK names both of these on
    // GameRenderSettings (TypeInfo.h:2747-2748, +0x0105 and +0x0106). They are
    // held rather than written once because the engine rewrites settings.
    { "GameRenderSettings", "DebugRendererEnable",      1,  1, false },
    { "GameRenderSettings", "DebugRenderServiceEnable", 1,  1, false },
};

static volatile bool g_enforcerRunning = false;

static EnforcedSetting* FindEnforced(const char* type, const char* field)
{
    for (size_t i = 0; i < _countof(g_enforced); ++i)
        if (_stricmp(g_enforced[i].type, type) == 0 &&
            _stricmp(g_enforced[i].field, field) == 0)
            return &g_enforced[i];
    return nullptr;
}

static DWORD WINAPI SettingsEnforcerThread(LPVOID)
{
    while (g_enforcerRunning)
    {
        for (size_t i = 0; i < _countof(g_enforced); ++i)
        {
            if (!g_enforced[i].active)
                continue;
            WriteSettingByName(g_enforced[i].type, g_enforced[i].field,
                               g_enforced[i].value, g_enforced[i].width);
        }
        Sleep(500);
    }
    return 0;
}

static void StartEnforcer()
{
    if (g_enforcerRunning)
        return;

    g_enforcerRunning = true;
    HANDLE thread = CreateThread(nullptr, 0, SettingsEnforcerThread, nullptr, 0, nullptr);
    if (thread != nullptr)
        CloseHandle(thread);
    else
        g_enforcerRunning = false;
}

// Applies one of the enforced settings now and keeps it applied.
static std::string ApplySetting(const char* type, const char* field, long long value,
                                size_t width, bool active, const char* describe)
{
    EnforcedSetting* setting = FindEnforced(type, field);
    if (setting == nullptr)
        return "that setting is not in the enforced list";

    setting->value = value;
    setting->active = active;

    const int written = WriteSettingByName(type, field, value, width);
    if (active)
        StartEnforcer();

    LogLine("%s: %s::%s = %lld on %d instance(s)", describe, type, field, value, written);

    char buffer[224];
    if (written < 0)
        sprintf_s(buffer, "%s: could not resolve %s::%s (code %d) - is a level loaded?",
                  describe, type, field, written);
    else
        sprintf_s(buffer, "%s: %s::%s written to %d instance(s)%s",
                  describe, type, field, written,
                  active ? " and held" : " and released");
    return buffer;
}

// ---------------------------------------------------------------------------
// Click to select.
//
// `GameWorld::rayCast` is at RVA 0xCECAE0 and is a thunk:
//     mov rcx,[rcx+0x60]; mov r10,[rcx]; jmp [r10+0x18]
// so it is called with the GameWorld itself in RCX and forwards to the ray
// caster's vtable slot 3. The RE agent verified this against two shipped call
// sites, and gave the signature:
//
//   bool __fastcall rayCast(GameWorld*, const char* ident, uint test,
//                           const Vec3* from, const Vec3* to, RayCastHit* hit,
//                           uint flags, const ExcludedPhysicsEntities* excluded)
//
// `test` is 0 for detailed and 1 for collision; flags 0 is what picking wants.
//
// RayCastHit's field order is NOT settled - source order is position, normal,
// bodyHandle, material, part, bone, lambda, but the x64 padding was never
// confirmed and two earlier probes disagreed. So this dumps the whole buffer and
// reports every field that could be a position, a lambda, or an object pointer,
// rather than pretending to know. One real hit closes it.
// ---------------------------------------------------------------------------
constexpr uintptr_t kRayCastRva = 0xCECAE0;
constexpr size_t kRayCastHitSize = 0x40;

struct Vec3Ray
{
    float x, y, z, w;
};

typedef bool(__fastcall* RayCastFn)(void* world, const char* ident, unsigned test,
                                    const Vec3Ray* from, const Vec3Ray* to, void* hit,
                                    unsigned flags, void* excluded);

// The raycast faulted on every attempt of the 01:00 run - 39 of the 40 logged
// faults were one address, game+0x96AABED, reading from [rax+0x58] with rax
// zero. The old guard returned false for that, so a dead subsystem and an honest
// miss printed the same line. It records what happened now.
static volatile DWORD g_rayFaultCode = 0;
static volatile uintptr_t g_rayFaultAt = 0;

static int RayCastFaultFilter(EXCEPTION_POINTERS* pointers)
{
    if (pointers != nullptr && pointers->ExceptionRecord != nullptr)
    {
        g_rayFaultCode = pointers->ExceptionRecord->ExceptionCode;
        g_rayFaultAt = reinterpret_cast<uintptr_t>(pointers->ExceptionRecord->ExceptionAddress);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

static bool GuardedRayCast(RayCastFn fn, void* world, const Vec3Ray* from, const Vec3Ray* to,
                           void* hit)
{
    g_rayFaultCode = 0;
    g_rayFaultAt = 0;

    __try
    {
        return fn(world, "FrostyGameLink:pick", 0, from, to, hit, 0, nullptr);
    }
    __except (RayCastFaultFilter(GetExceptionInformation()))
    {
        return false;
    }
}


// Anything in the hit buffer that reads like a live object: an aligned heap
// pointer whose first qword is a vtable inside the game image.
static bool LooksLikeObject(uintptr_t candidate)
{
    if (candidate < 0x10000 || (candidate & 7) != 0)
        return false;

    uintptr_t vtable = 0;
    if (!SafeRead(candidate, &vtable))
        return false;

    return vtable > g_base && vtable < g_base + g_size;
}

static std::string CastFromCamera(float range)
{
    uintptr_t world = 0;
    SafeRead(g_base + kGameWorldArrayRva, &world);
    if (world == 0)
        return "no client game world - is a level loaded?";

    Transform camera{};
    float fov = 0.0f;
    if (!ReadCamera(&camera, &fov))
        return "no render view";

    // Cast from where the view actually is, along where it actually looks, so
    // this picks whatever is in the middle of the screen.
    Vec3Ray from{ camera.translation[0], camera.translation[1], camera.translation[2], 0.0f };
    Vec3Ray to{ camera.translation[0] + camera.forward[0] * range,
                camera.translation[1] + camera.forward[1] * range,
                camera.translation[2] + camera.forward[2] * range, 0.0f };

    alignas(16) static unsigned char hit[kRayCastHitSize];
    memset(hit, 0, sizeof(hit));

    RayCastFn cast = reinterpret_cast<RayCastFn>(g_base + kRayCastRva);
    const bool struck = GuardedRayCast(cast, reinterpret_cast<void*>(world), &from, &to, hit);

    char outcome[160];
    if (g_rayFaultCode != 0)
    {
        char where[160];
        DescribeAddress(where, sizeof(where), g_rayFaultAt);
        sprintf_s(outcome, "FAULTED code 0x%08lX at %s",
                  static_cast<unsigned long>(g_rayFaultCode), where);
    }
    else
    {
        strcpy_s(outcome, struck ? "HIT" : "no hit");
    }

    char header[448];
    sprintf_s(header, "cast from %.2f %.2f %.2f along %.3f %.3f %.3f for %.0f m -> %s\n",
              from.x, from.y, from.z, camera.forward[0], camera.forward[1], camera.forward[2],
              range, outcome);
    std::string out(header);

    LogLine("raypick: cast from %.2f %.2f %.2f -> %s", from.x, from.y, from.z, outcome);

    // Dump it whichever way it went: a miss that still wrote fields is worth
    // seeing, and the layout is not settled.
    for (size_t offset = 0; offset < kRayCastHitSize; offset += 16)
    {
        const float* floats = reinterpret_cast<const float*>(hit + offset);
        char line[224];
        sprintf_s(line, "  +0x%02zX  %12.4f %12.4f %12.4f %12.4f\n", offset,
                  floats[0], floats[1], floats[2], floats[3]);
        out += line;
    }

    // Then look for something that is plainly an object.
    for (size_t offset = 0; offset + sizeof(uintptr_t) <= kRayCastHitSize;
         offset += sizeof(uintptr_t))
    {
        uintptr_t candidate = 0;
        memcpy(&candidate, hit + offset, sizeof(candidate));
        if (!LooksLikeObject(candidate))
            continue;

        const std::string name = EntityDisplayName(candidate);

        Transform where{};
        const bool hasTransform = GuardedGetTransform(candidate, &where);

        char line[352];
        if (hasTransform)
            sprintf_s(line, "  object at +0x%02zX = 0x%llX  '%s'  at %.2f %.2f %.2f\n",
                      offset, static_cast<unsigned long long>(candidate), name.c_str(),
                      where.translation[0], where.translation[1], where.translation[2]);
        else
            sprintf_s(line, "  object at +0x%02zX = 0x%llX  '%s'  (no transform)\n",
                      offset, static_cast<unsigned long long>(candidate), name.c_str());
        out += line;

        if (g_selectedEntity == 0)
        {
            g_selectedEntity = candidate;

            // Two lines, like the reference editor: what it is, then where.
            if (hasTransform)
                sprintf_s(g_selectionLabel, "%s\n%.1f %.1f %.1f", name.c_str(),
                          where.translation[0], where.translation[1], where.translation[2]);
            else
                sprintf_s(g_selectionLabel, "%s", name.c_str());
        }

        LogLine("pick: object at hit+0x%02zX = 0x%llX '%s'", offset,
                static_cast<unsigned long long>(candidate), name.c_str());
    }

    if (g_selectedEntity != 0)
    {
        char tail[128];
        sprintf_s(tail, "  selected 0x%llX\n",
                  static_cast<unsigned long long>(g_selectedEntity));
        out += tail;
    }
    else
    {
        out += "  nothing in the buffer looks like an object\n";
    }

    return out;
}

// ---------------------------------------------------------------------------
// Showing the selection.
//
// Clicking a model has to put something on screen, so this draws a white
// wireframe box around whatever was picked and a yellow label above it, and
// keeps drawing while the selection stands.
//
// Bounds come from SpatialEntity::computeBoundsInfo, vtable slot 26 immediately
// before Kyber's exact get/set slots 27/28. Shared source defines the out-record
// as LinearTransform (0x40), AxisAlignedBox (two 16-byte Vec3 slots), then cull
// area. The AABB is local to the returned transform, not a world AABB.
// ---------------------------------------------------------------------------
constexpr int kSlotComputeBounds = 26;

typedef void(__fastcall* ComputeBoundsFn)(void* entity, void* out);

struct SpatialBoundsRecord
{
    Transform transform;
    float minimum[4];
    float maximum[4];
    float visualCullScreenArea;
    float padding[3];
};

struct WorldBounds
{
    float minimum[3];
    float maximum[3];
    float localMinimum[3];
    float localMaximum[3];
    Transform transform;
    bool valid;
    int layout;                 // which BoundsLayout produced the box, -1 if none
    bool rawValid;              // whether raw below was actually filled
    SpatialBoundsRecord raw;    // kept so 'boundsraw' can show the bytes, not a reading of them
    float origin[3];            // entity world translation, from getTransform
    bool haveOrigin;
    // True when the extents were assumed rather than measured - the transform
    // and scale are real, the size is a stand-in. Anything reporting bounds to
    // a user must say which of the two it is holding.
    bool assumed;
};
static_assert(offsetof(SpatialBoundsRecord, minimum) == 0x40, "bounds AABB offset");
static_assert(offsetof(SpatialBoundsRecord, maximum) == 0x50, "bounds AABB offset");

// Slot 26 is what two agents agree computeBoundsInfo is, and neither has ever
// verified it against this build's code. The cache calls it once per entity, so
// a wrong slot means thousands of arbitrary virtual calls rather than one. This
// counts the faults so the cache builder can stop after a handful and fall back
// to transforms instead of grinding through the whole level being wrong.
static volatile LONG g_boundsFaults = 0;

static bool GuardedComputeBounds(ComputeBoundsFn fn, void* entity, void* out)
{
    __try
    {
        fn(entity, out);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        InterlockedIncrement(&g_boundsFaults);
        return false;
    }
}

// ---------------------------------------------------------------------------
// The record layout is contested, so it is scored rather than assumed.
//
// The 23:44 and 14:38 pick censuses each returned 3 static models and 2 effect
// markers out of 2,266 cached candidates, with sampled boxes of 111.2 m and
// 29.3 m on ordinary props. That is the signature of a layout that is right for
// one entity family and wrong for the rest: the box lands where the ray never
// goes, so the only selectable objects are the few whose local box happens to
// coincide with their world position.
//
// Two sources disagree and neither was ever checked against this build:
//   - SpatialBoundsRecord above, and this file's own comment, say LinearTransform
//     at 0x00 then a LOCAL AABB at 0x40/0x50 that must be transformed;
//   - HANDOVER-2026-08-24.md section 9 says a WORLD AABB at 0x00 and 0x18 with
//     no transform at all.
//
// Guessing between them is what produced the current state. The entity already
// knows where it is: GuardedGetTransform returns its world transform through a
// different virtual, so a correct world AABB must sit on top of that translation.
// Build a candidate per layout, discard the ones whose extents are impossible,
// and keep the one that actually contains the entity. That is self-correcting
// per family, which is exactly what one fixed layout cannot be.
//
// UNVERIFIED: written without a running game. 'boundsraw' prints the bytes and
// the per-layout verdict so the next live run settles it in one command.
// ---------------------------------------------------------------------------

enum BoundsLayout
{
    kBoundsLayoutAuto        = -1,
    kBoundsLayoutXformLocal  = 0,   // LinearTransform@0x00, local AABB@0x40/0x50, transformed
    kBoundsLayoutXformWorld  = 1,   // LinearTransform@0x00, AABB@0x40/0x50 already world
    kBoundsLayoutHeadVec4    = 2,   // world AABB@0x00/0x10, Vec4 stride
    kBoundsLayoutHeadVec3    = 3,   // world AABB@0x00/0x18, the handover's literal claim
    kBoundsLayoutCount       = 4
};

static const char* BoundsLayoutName(int layout)
{
    switch (layout)
    {
    case kBoundsLayoutXformLocal: return "xform+local(0x40/0x50)";
    case kBoundsLayoutXformWorld: return "xform+world(0x40/0x50)";
    case kBoundsLayoutHeadVec4:   return "world(0x00/0x10)";
    case kBoundsLayoutHeadVec3:   return "world(0x00/0x18)";
    default:                      return "none";
    }
}

static volatile LONG g_boundsLayout = kBoundsLayoutAuto;
static volatile LONG g_boundsLayoutWins[kBoundsLayoutCount] = { 0, 0, 0, 0 };
static volatile LONG g_boundsNoCandidate = 0;
static volatile LONG g_boundsNotSpatial = 0;   // isKindOf said no; we tried anyway

// A prop is not 111 metres and it is not a millimetre. Outside this band the
// read is wrong rather than the object large, and the caller's transform-centred
// fallback box is at least in a place the ray can reach.
constexpr float kBoundsMinExtent = 0.05f;
constexpr float kBoundsMaxExtent = 400.0f;

static bool BoundsFloatOk(float value)
{
    return std::isfinite(value) != 0;
}

struct BoundsCandidate
{
    float minimum[3];
    float maximum[3];
    bool  usable;
    float score;   // squared distance from box centre to entity origin, lower is better
};

static void BuildBoundsCandidate(const SpatialBoundsRecord& record, int layout,
                                 const float origin[3], bool haveOrigin,
                                 BoundsCandidate* out)
{
    out->usable = false;
    out->score = FLT_MAX;

    const unsigned char* base = reinterpret_cast<const unsigned char*>(&record);
    const float* rawMin = nullptr;
    const float* rawMax = nullptr;

    switch (layout)
    {
    case kBoundsLayoutXformLocal:
    case kBoundsLayoutXformWorld:
        rawMin = record.minimum;
        rawMax = record.maximum;
        break;
    case kBoundsLayoutHeadVec4:
        rawMin = reinterpret_cast<const float*>(base + 0x00);
        rawMax = reinterpret_cast<const float*>(base + 0x10);
        break;
    case kBoundsLayoutHeadVec3:
        rawMin = reinterpret_cast<const float*>(base + 0x00);
        rawMax = reinterpret_cast<const float*>(base + 0x18);
        break;
    default:
        return;
    }

    for (int axis = 0; axis < 3; ++axis)
    {
        if (!BoundsFloatOk(rawMin[axis]) || !BoundsFloatOk(rawMax[axis]))
            return;
        const float size = rawMax[axis] - rawMin[axis];
        if (!(size > kBoundsMinExtent && size < kBoundsMaxExtent))
            return;
    }

    if (layout == kBoundsLayoutXformLocal)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            out->minimum[axis] = FLT_MAX;
            out->maximum[axis] = -FLT_MAX;
        }

        for (int corner = 0; corner < 8; ++corner)
        {
            const float local[3] = {
                (corner & 1) != 0 ? rawMax[0] : rawMin[0],
                (corner & 2) != 0 ? rawMax[1] : rawMin[1],
                (corner & 4) != 0 ? rawMax[2] : rawMin[2]
            };
            for (int axis = 0; axis < 3; ++axis)
            {
                const float world = record.transform.translation[axis] +
                                    record.transform.left[axis]    * local[0] +
                                    record.transform.up[axis]      * local[1] +
                                    record.transform.forward[axis] * local[2];
                if (!BoundsFloatOk(world))
                    return;
                if (world < out->minimum[axis]) out->minimum[axis] = world;
                if (world > out->maximum[axis]) out->maximum[axis] = world;
            }
        }

        // A scaled basis can push a plausible local box past plausibility once
        // transformed, which is exactly how a 2 m crate became 111 m.
        for (int axis = 0; axis < 3; ++axis)
        {
            const float size = out->maximum[axis] - out->minimum[axis];
            if (!(size > kBoundsMinExtent && size < kBoundsMaxExtent))
                return;
        }
    }
    else
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            out->minimum[axis] = rawMin[axis];
            out->maximum[axis] = rawMax[axis];
        }
    }

    out->usable = true;

    if (!haveOrigin)
    {
        out->score = 0.0f;   // nothing to score against; plausibility is all there is
        return;
    }

    // A correct world box sits on the entity. The origin is often at the base or
    // on a face rather than dead centre, so the test is containment in the box
    // grown by a quarter of its own extent; the score is the centre offset.
    float offset = 0.0f;
    for (int axis = 0; axis < 3; ++axis)
    {
        const float size  = out->maximum[axis] - out->minimum[axis];
        const float slack = size * 0.25f;
        if (origin[axis] < out->minimum[axis] - slack ||
            origin[axis] > out->maximum[axis] + slack)
        {
            out->usable = false;
            return;
        }
        const float centre = (out->minimum[axis] + out->maximum[axis]) * 0.5f;
        const float delta  = centre - origin[axis];
        offset += delta * delta;
    }

    out->score = offset;
}

// Placed-object identity, needed early because the pick cache tests it.
constexpr uintptr_t kRenderObjectVtableRva = 0x2F7CA20;
constexpr uintptr_t kPlacedEntityVtableRva = 0x2F9B060;
constexpr size_t kEntityRenderObject = 0x70;

// ---------------------------------------------------------------------------
// Bounds without computeBoundsInfo.
//
// Virtual slot 26 is NOT computeBoundsInfo on the classes this cache walks. On
// the placed entity vtable (game+0x2F9B060) it is game+0x1840C0, a three-byte
// 'return 0' stub shared by 9,007 call sites; on the placed render object
// (game+0x2F7CA20) it is game+0x16D880, a different three-byte 'return' stub.
// Neither writes the out-record. Because a stub does not fault,
// GuardedComputeBounds reports success and the whole scoring pass then runs on
// a record that is still all zeros - or, on a class where slot 26 happens to be
// some unrelated method, on whatever that method left behind. That is the
// 111.2 m box, and it is why 5 of 2,266 candidates were ever selectable.
//
// Verified against Frostbite source (engine/SpatialQuery/SpatialBoundsInfo.h):
// the record layout this file declares is exactly right - LinearTransform then
// AxisAlignedBox then the cull float - and computeBoundsInfo is declared on
// components (InternalRigidBody, DebrisCluster, MeshObject), not on entities.
// The correct slot for these classes has not been found.
//
// What IS known and measured holds regardless: the world transform, and a
// per-instance scale. A box built from those sits in the right place with the
// right orientation and a guessed size. A box in the wrong place can never be
// clicked; one of the wrong size merely needs better aim.
// ---------------------------------------------------------------------------

// Half-extents for a prop with no recoverable size, in metres, before scale.
constexpr float kAssumedHalfExtent = 1.0f;

// Tighter than kBoundsMinExtent/kBoundsMaxExtent above, deliberately. Those
// two are a coarse "is this even a box" test inside candidate building, and at
// 400 m they pass the 111 m box that made picking useless. This pair is the
// decision-point gate: is this a believable prop? Anything outside it came from
// a method that was never computeBoundsInfo, so an assumed box beats it.
constexpr float kPickPlausibleMinExtent = 0.02f;
constexpr float kPickPlausibleMaxExtent = 60.0f;

static volatile LONG g_boundsImplausible = 0;   // rejected as nonsense
static volatile LONG g_boundsAssumed = 0;       // built from transform + scale

static bool BoundsLookPlausible(const float minimum[3], const float maximum[3])
{
    for (int axis = 0; axis < 3; ++axis)
    {
        const float size = maximum[axis] - minimum[axis];
        if (!(size == size))            // NaN
            return false;
        if (size < kPickPlausibleMinExtent || size > kPickPlausibleMaxExtent)
            return false;
    }
    return true;
}

// Per-instance scale lives at +0x60 of the object's primary base subobject,
// which +0x78 of the render object points back to. Measured across 12 objects
// it varies genuinely (0.87 to 1.70, including non-uniform), unlike the
// neighbouring floats which were byte-identical everywhere and are constants.
static bool ReadPlacedScale(uintptr_t entity, float out[3])
{
    out[0] = out[1] = out[2] = 1.0f;

    uintptr_t renderObject = 0;
    if (!SafeRead(entity + kEntityRenderObject, &renderObject) || renderObject == 0)
        return false;

    uintptr_t primary = 0;
    if (!SafeRead(renderObject + 0x78, &primary) || primary == 0)
        return false;

    float scale[3] = { 1.0f, 1.0f, 1.0f };
    if (!SafeRead(primary + 0x60, &scale))
        return false;

    for (int axis = 0; axis < 3; ++axis)
    {
        // Guard the read: a wrong pointer here must not produce a giant box.
        if (!(scale[axis] == scale[axis]) || scale[axis] <= 0.0f || scale[axis] > 100.0f)
            return false;
        out[axis] = scale[axis];
    }
    return true;
}

// Fill a WorldBounds from the entity's own transform and scale. Marked so the
// caller can tell an assumed box from a measured one and say so on screen.
static bool BuildAssumedBounds(uintptr_t entity, WorldBounds* bounds)
{
    Transform where{};
    if (!GuardedGetTransformUnchecked(entity, &where))
        return false;

    float scale[3];
    ReadPlacedScale(entity, scale);          // falls back to 1,1,1 on failure

    bounds->transform = where;
    bounds->haveOrigin = true;
    for (int axis = 0; axis < 3; ++axis)
    {
        const float half = kAssumedHalfExtent * scale[axis];
        bounds->origin[axis] = where.translation[axis];
        bounds->localMinimum[axis] = -half;
        bounds->localMaximum[axis] = half;
        // World AABB for the ray test. The box is axis aligned in world space,
        // which over-covers a rotated prop slightly - the right trade for a
        // pick test, since a miss is worse than a small over-hit.
        bounds->minimum[axis] = where.translation[axis] - half;
        bounds->maximum[axis] = where.translation[axis] + half;
    }
    bounds->layout = kBoundsLayoutAuto;
    bounds->valid = true;
    bounds->assumed = true;
    InterlockedIncrement(&g_boundsAssumed);
    return true;
}

static WorldBounds ReadEntityBounds(uintptr_t entity)
{
    WorldBounds bounds{};
    bounds.valid = false;
    bounds.layout = kBoundsLayoutAuto;
    bounds.rawValid = false;

    // The spatial test used to veto this outright. On the 00:30 run that veto
    // fired on all 1,903 cached objects: every layout counter, the no-candidate
    // counter AND the fault counter all read zero, which can only happen if the
    // function returns before computeBoundsInfo is ever called. getType works -
    // the label proves it - so what fails is isKindOf against
    // kSpatialEntityTypeInfoRva, and one unverified RVA should not be able to
    // veto a virtual that might work. Count it and carry on; GuardedComputeBounds
    // catches a bad call anyway.
    if (!EntityIsSpatial(entity))
        InterlockedIncrement(&g_boundsNotSpatial);

    // Slot 26 is not computeBoundsInfo, and not consistently anything.
    //
    // On the placed entity and placed render object vtables it is a shared
    // three-byte stub. On ClientStaticModelEntity it is some other real method,
    // which writes plausible-looking values: the 01:00 pick census showed props
    // 4-7 m from the camera carrying 17.7 m, 41.5 m and 61.4 m boxes. A box
    // that large around a nearby prop contains the camera, and the pick loop
    // discards those - which is the whole reason only effect markers were ever
    // selectable.
    //
    // Frostbite source (engine/SpatialQuery/SpatialBoundsInfo.h) declares
    // computeBoundsInfo on components - InternalRigidBody, DebrisCluster,
    // MeshObject - not on entities, so a fixed entity slot was never going to
    // be right across families. Until the correct per-class route is found, a
    // measured transform with an assumed size beats a measured-looking box that
    // is wrong by an order of magnitude.
    //
    // 'boundslayout <n>' still forces the old path for experiments.
    if (g_boundsLayout == kBoundsLayoutAuto)
    {
        if (BuildAssumedBounds(entity, &bounds))
            return bounds;
        // If even getTransform fails there is nothing to build from; fall
        // through and let the old path try, since it can only do better.
    }

    uintptr_t fn = 0;
    if (!ReadVtableSlot(reinterpret_cast<void*>(entity), kSlotComputeBounds, &fn))
    {
        BuildAssumedBounds(entity, &bounds);
        return bounds;
    }

    alignas(16) SpatialBoundsRecord record{};

    if (!GuardedComputeBounds(reinterpret_cast<ComputeBoundsFn>(fn),
                              reinterpret_cast<void*>(entity), &record))
    {
        BuildAssumedBounds(entity, &bounds);
        return bounds;
    }

    bounds.raw = record;
    bounds.rawValid = true;
    bounds.transform = record.transform;

    for (int axis = 0; axis < 3; ++axis)
    {
        bounds.localMinimum[axis] = record.minimum[axis];
        bounds.localMaximum[axis] = record.maximum[axis];
    }

    // The entity's own world transform, through a different virtual. This is the
    // ground truth every candidate is scored against.
    Transform where{};
    const bool haveOrigin = GuardedGetTransform(entity, &where);
    float origin[3] = { 0.0f, 0.0f, 0.0f };
    if (haveOrigin)
        for (int axis = 0; axis < 3; ++axis)
            origin[axis] = where.translation[axis];

    bounds.haveOrigin = haveOrigin;
    for (int axis = 0; axis < 3; ++axis)
        bounds.origin[axis] = origin[axis];

    const LONG forced = g_boundsLayout;

    int best = -1;
    BoundsCandidate bestCandidate{};
    bestCandidate.score = FLT_MAX;

    for (int layout = 0; layout < kBoundsLayoutCount; ++layout)
    {
        if (forced != kBoundsLayoutAuto && layout != static_cast<int>(forced))
            continue;

        BoundsCandidate candidate{};
        BuildBoundsCandidate(record, layout, origin, haveOrigin, &candidate);
        if (!candidate.usable)
            continue;

        if (best < 0 || candidate.score < bestCandidate.score)
        {
            best = layout;
            bestCandidate = candidate;
        }
    }

    if (best < 0)
    {
        InterlockedIncrement(&g_boundsNoCandidate);
        BuildAssumedBounds(entity, &bounds);
        return bounds;
    }

    // A candidate that survived scoring can still be nonsense, because the
    // record it was scored from was never written by a bounds function. Size is
    // the tell: the scoring only ever checked that the box CONTAINS the entity
    // origin, which a 111 m box trivially does.
    if (!BoundsLookPlausible(bestCandidate.minimum, bestCandidate.maximum))
    {
        InterlockedIncrement(&g_boundsImplausible);
        BuildAssumedBounds(entity, &bounds);
        return bounds;
    }

    for (int axis = 0; axis < 3; ++axis)
    {
        bounds.minimum[axis] = bestCandidate.minimum[axis];
        bounds.maximum[axis] = bestCandidate.maximum[axis];
    }

    // Oriented drawing uses the record transform only when that transform is what
    // produced the box. For a world-AABB layout the box is axis aligned and the
    // entity's own transform is the honest basis.
    if (best != kBoundsLayoutXformLocal && haveOrigin)
        bounds.transform = where;

    bounds.layout = best;
    bounds.valid = true;
    InterlockedIncrement(&g_boundsLayoutWins[best]);

    return bounds;
}

// ---------------------------------------------------------------------------
// Picking without physics.
//
// GameWorld::rayCast is not usable on this route. It faulted on every attempt of
// the 01:00 run, reading from a null pointer deep inside the ray caster - which
// is what a physics subsystem that was never initialised looks like. There is no
// server simulation and no match here, so there is nothing to ask.
//
// Physics picking was the wrong dependency anyway. An editor wants to select
// what is drawn, not what has collision. So: take the entity list the sublevel
// walk already produces, get each one's world AABB from computeBoundsInfo, and
// intersect the ray ourselves. No engine call that can be absent, and the entity
// pointer falls out directly - which is what the label and the box both need.
//
// The cache is built once and reused. Rebuilding it walks the whole tree and
// makes one virtual call per entity, which is far too slow to do per click.
// ---------------------------------------------------------------------------

struct PickCandidate
{
    uintptr_t entity;
    std::string name;
    float minimum[3] = { 0.0f, 0.0f, 0.0f };
    float maximum[3] = { 0.0f, 0.0f, 0.0f };
    float origin[3] = { 0.0f, 0.0f, 0.0f };   // world translation - 'pushbox' matches on this
    bool realBounds = false;
    // Never leave this indeterminate. BuildPickCache did not assign it, PickCandidate had
    // no initialiser, and the pick loop skips !hasMesh when pickMeshesOnly is on - so a
    // cache built by that path dropped candidates according to whatever was on the stack.
    bool hasMesh = false;
    bool pushedBounds = false;   // box came from the plugin (EBX + MeshSet), not the engine
};

// Only offer things you can actually see.
//
// The 13:40 run kept selecting LocatorEntity and EffectEntity - invisible marker
// entities whose bounds sprawl across a room. Clicking a pillar should give the
// pillar. An entity is renderable when +0x70 holds a render object, which is the
// same link the transform is read through.
static volatile bool g_pickMeshesOnly = true;

// Test the runtime class name, not a pointer layout.
//
// The first attempt required entity+0x70 to hold a render object of a specific
// vtable. That relationship was measured on one class in the dump and does not
// generalise - it matched 0 of 2299 of the walker's entities. The walker already
// gives us the runtime class in the display name, e.g.
// "Levels/MP/Naboo_01/Naboo_01 StaticModelEntityData ClientStaticModelEntity",
// so match on that instead. Cheap, and it uses evidence we can see.
// The object's OWN asset path, not the level's.
//
// The label was showing "Levels/MP/Naboo_01/Naboo_01" for everything, because
// the entity's data container names the owning level asset. The reference editor
// shows the model - "Objects/Architecture/Naboo/Palace/O_NAB_DoorPalace_03".
//
// Recovered from naboo-loaded.dmp by breadth-first search: the path sits two
// hops from entity+0x10, with the final offset varying by type (+0xD8 .. +0x118
// observed). A bounded search over depth 3 found it for 396 of 400 entities
// sampled, so search rather than hard-code an offset that only fits one type.
static bool LooksLikeAssetPath(const std::string& text)
{
    if (text.size() < 8 || text.size() > 200)
        return false;
    if (text.find('/') == std::string::npos)
        return false;
    if (text.compare(0, 7, "Levels/") == 0 || text.compare(0, 7, "levels/") == 0)
        return false;
    for (size_t i = 0; i < text.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 32 || c > 126)
            return false;
    }
    return true;
}

static std::string FindOwnAssetPath(uintptr_t entity)
{
    std::vector<uintptr_t> frontier;
    std::unordered_set<uintptr_t> seen;
    frontier.push_back(entity);
    seen.insert(entity);

    for (int depth = 0; depth < 3; ++depth)
    {
        std::vector<uintptr_t> next;
        for (size_t n = 0; n < frontier.size() && next.size() < 64; ++n)
        {
            const uintptr_t node = frontier[n];
            for (size_t off = 0; off < 0x120; off += 8)
            {
                uintptr_t v = 0;
                if (!SafeRead(node + off, &v) || v < 0x10000)
                    continue;
                if (AddressIsGameCode(v))
                    continue;

                const std::string text = SafeCString(v, 220);
                if (LooksLikeAssetPath(text))
                    return text;

                if ((v & 7) == 0 && seen.size() < 4096 && seen.insert(v).second)
                    next.push_back(v);
            }
        }
        frontier.swap(next);
        if (frontier.empty())
            break;
    }

    return std::string();
}

static bool NameLooksRenderable(const std::string& name)
{
    static const char* const kMeshy[] = {
        "ModelEntity", "MeshEntity", "StaticModel", "DynamicModel",
        "VehicleEntity", "SoldierEntity", "DestructionEntity", "BangerEntity"
    };
    for (size_t i = 0; i < _countof(kMeshy); ++i)
        if (name.find(kMeshy[i]) != std::string::npos)
            return true;
    return false;
}

static bool EntityHasMesh(uintptr_t entity)
{
    // Kept as a secondary test for candidates built by the render-vtable scan.
    uintptr_t render = 0;
    if (!SafeRead(entity + kEntityRenderObject, &render) || render == 0)
        return false;
    uintptr_t vptr = 0;
    if (!SafeRead(render, &vptr))
        return false;
    return vptr == g_base + kRenderObjectVtableRva;
}

static std::vector<PickCandidate> g_pickCache;
static CRITICAL_SECTION g_pickLock;
static volatile bool g_pickLockReady = false;
static volatile LONG g_pickCacheReal = 0;      // how many had usable computeBoundsInfo

// Entities with no usable bounds still get a box, so they can be picked and the
// wrong size is visible rather than silent.
static const float kFallbackHalfExtent = 1.5f;

// A prop bigger than this on every axis is a volume, not something you click.
// pickdump, 13:56: legitimate ClientStaticModelEntity boxes run from 1.2 m
// props up to 195 m palace halls. Anything past ~300 m on an axis is terrain or
// a streaming volume, not something you click.
static const float kMaxPickableSpan = 300.0f;

// Above this diagonal, a box containing the camera is treated as a volume and
// skipped; below it, standing inside it is a valid selection.
static const float kContainerDiagonal = 220.0f;

static void LockPick()
{
    if (g_pickLockReady)
        EnterCriticalSection(&g_pickLock);
}

static void UnlockPick()
{
    if (g_pickLockReady)
        LeaveCriticalSection(&g_pickLock);
}

// ---------------------------------------------------------------------------
// Picking from a type scan.
//
// The reference screenshot names the selected object's runtime class in its own
// label: **ClientStaticModelEntity**. That is the placed static geometry - the
// shuttle, the walls, the props. It is also a registered reflection type, which
// means the instance scanner can find every live one without going anywhere near
// the sub-level walk that only ever finds thirteen objects.
//
// The scan is slow - it walks every committed private region below 0x200000000,
// which is why a settings write took minutes with a level loaded. So it runs on
// its own thread and fills the cache when it finishes, rather than holding a
// socket open for the duration.
// ---------------------------------------------------------------------------
static const char* const kDefaultPickTypes[] = {
    "ClientStaticModelEntity",
    "ClientDynamicModelEntity",
};

static volatile LONG g_pickScanRunning = 0;
static volatile LONG g_pickScanFound = 0;
static char g_pickScanStatus[256] = "no type scan has run";

static size_t AddInstancesOfType(const char* typeName, size_t cap,
                                 std::vector<PickCandidate>* into, LONG* realBounds)
{
    LockRegistry();
    const TypeRec* type = FindType(typeName);
    const bool known = type != nullptr;
    TypeRec copy;
    if (known)
        copy = *type;
    UnlockRegistry();

    if (!known)
        return 0;

    const std::vector<uintptr_t> instances = FindInstances(copy, cap);

    for (size_t i = 0; i < instances.size(); ++i)
    {
        const uintptr_t entity = instances[i];

        PickCandidate candidate;
        candidate.entity = entity;
        candidate.realBounds = false;
        candidate.hasMesh = NameLooksRenderable(typeName) || EntityHasMesh(entity);

        const std::string path = EntityAssetPath(entity);
        candidate.name = path.empty() ? typeName : path;

        const WorldBounds bounds = ReadEntityBounds(entity);
        if (bounds.valid)
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                candidate.minimum[axis] = bounds.minimum[axis];
                candidate.maximum[axis] = bounds.maximum[axis];
            }
            // An assumed box is not a measured one. Counting them together
            // is how 'pickcache' came to claim 2,381 entities had real
            // computeBoundsInfo bounds when none of them did.
            candidate.realBounds = !bounds.assumed;
            if (candidate.realBounds)
                ++(*realBounds);
            for (int axis = 0; axis < 3; ++axis)
                candidate.origin[axis] = bounds.haveOrigin
                    ? bounds.origin[axis]
                    : (candidate.minimum[axis] + candidate.maximum[axis]) * 0.5f;
        }
        else
        {
            Transform where{};
            if (!GuardedGetTransform(entity, &where))
                continue;

            for (int axis = 0; axis < 3; ++axis)
            {
                candidate.origin[axis] = where.translation[axis];
                candidate.minimum[axis] = where.translation[axis] - kFallbackHalfExtent;
                candidate.maximum[axis] = where.translation[axis] + kFallbackHalfExtent;
            }
        }

        candidate.pushedBounds = false;
        into->push_back(candidate);
    }

    return instances.size();
}

struct PickScanRequest
{
    char typeName[128];
    size_t cap;
};

static DWORD WINAPI PickScanThread(LPVOID parameter)
{
    PickScanRequest* request = static_cast<PickScanRequest*>(parameter);

    std::vector<PickCandidate> fresh;
    LONG realBounds = 0;
    std::string summary;

    if (request->typeName[0] != '\0')
    {
        const size_t found = AddInstancesOfType(request->typeName, request->cap,
                                                &fresh, &realBounds);
        char part[224];
        sprintf_s(part, "%s: %zu", request->typeName, found);
        summary = part;
    }
    else
    {
        for (size_t i = 0; i < _countof(kDefaultPickTypes); ++i)
        {
            const size_t found = AddInstancesOfType(kDefaultPickTypes[i], request->cap,
                                                    &fresh, &realBounds);
            char part[224];
            sprintf_s(part, "%s%s: %zu", summary.empty() ? "" : ", ",
                      kDefaultPickTypes[i], found);
            summary += part;
        }
    }

    const size_t count = fresh.size();

    LockPick();
    g_pickCache.swap(fresh);
    g_pickCacheReal = realBounds;
    UnlockPick();

    sprintf_s(g_pickScanStatus, "%zu pickable (%s), %ld with real bounds",
              count, summary.c_str(), realBounds);
    LogLine("pickscan: %s", g_pickScanStatus);

    g_pickScanFound = static_cast<LONG>(count);
    InterlockedExchange(&g_pickScanRunning, 0);
    delete request;
    return 0;
}

// ---------------------------------------------------------------------------
// Placed-object enumeration by render vtable.
//
// Recovered from naboo-loaded.dmp on 2026-08-24 and validated against the whole
// population, not a sample:
//
//   scan 8-byte-aligned qwords for  g_base + kRenderObjectVtableRva
//     -> 1987 render objects found in 7 s across 11.14 GB
//   read the 4x4 at render+0x128, require three orthonormal basis rows
//     -> 1883 valid  (94.8%)
//   1854 of those (98%) lie in the level around where the camera was
//
// Two things this replaces:
//   - the sub-level walk, which returned thirteen objects in a loaded Naboo
//   - the registry-based FindInstances, which took 73 s for one settings write
//
// **8-byte alignment is load-bearing.** The same scan restricted to 16-aligned
// positions returns ZERO of these objects.
//
// This is an enumeration, not the engine's own container - that remains unknown,
// and no pointer array to these objects exists anywhere in the dump. It is
// sufficient for picking because it yields every placed object with a position.
// ---------------------------------------------------------------------------


// Scan for the ENTITY, not the render object. Two reasons, both learned the hard
// way on the 12:43 acceptance run:
//   - the entity is what ReadEntityBounds() needs; computeBoundsInfo is a
//     virtual on the spatial entity, and the render object is not one
//   - entity+0x70 is its render object, so the transform is still reachable,
//     while there is no pointer the other way (all 1,929 references to render
//     objects in the dump come from entity+0x70)
// That run boxed every object as a 1.5 m cube around its origin and missed on
// all 1,883 candidates: a 50 m building has a point origin, and aiming at its
// wall from 150 m away cannot hit a cube at its centre.

constexpr size_t kRenderTransformOffset = 0x128;

static bool ReadRenderTransform(uintptr_t render, float position[3])
{
    float m[16];
    if (!SafeRead(render + kRenderTransformOffset, &m))
        return false;

    // Three basis rows, each of sane length and mutually orthogonal. This is the
    // predicate that separated real placements from noise in the dump.
    float length[3];
    for (int row = 0; row < 3; ++row)
    {
        const float* r = m + row * 4;
        const float sq = r[0] * r[0] + r[1] * r[1] + r[2] * r[2];
        if (!(sq > 0.0004f && sq < 250000.0f))
            return false;
        length[row] = sqrtf(sq);
    }

    for (int a = 0; a < 3; ++a)
    {
        for (int b = a + 1; b < 3; ++b)
        {
            const float* ra = m + a * 4;
            const float* rb = m + b * 4;
            const float dot = (ra[0] * rb[0] + ra[1] * rb[1] + ra[2] * rb[2]) /
                              (length[a] * length[b]);
            if (dot > 0.06f || dot < -0.06f)
                return false;
        }
    }

    for (int axis = 0; axis < 3; ++axis)
    {
        const float value = m[12 + axis];
        if (!(value > -100000.0f && value < 100000.0f))
            return false;
        position[axis] = value;
    }

    return true;
}

// Returns how many candidates were added.
static size_t CollectRenderObjects(std::vector<PickCandidate>* into, size_t cap)
{
    const uintptr_t wanted = g_base + kPlacedEntityVtableRva;
    const uintptr_t renderVtable = g_base + kRenderObjectVtableRva;
    size_t scanned = 0;
    size_t found = 0;
    size_t withRealBounds = 0;

    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t address = 0;

    while (into->size() < cap &&
           VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == sizeof(mbi))
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = base + mbi.RegionSize;

        const bool writable = (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                                              PAGE_EXECUTE_READWRITE |
                                              PAGE_EXECUTE_WRITECOPY)) != 0;
        const bool usable = mbi.State == MEM_COMMIT && writable &&
                            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
                            mbi.Type == MEM_PRIVATE;

        if (usable && base < 0x200000000ull)
        {
            // Eight-byte stride, deliberately. Sixteen finds none of them.
            for (uintptr_t at = base; at + sizeof(uintptr_t) <= end; at += sizeof(uintptr_t))
            {
                ++scanned;
                uintptr_t value = 0;
                if (!SafeRead(at, &value) || value != wanted)
                    continue;

                // entity -> render object -> world transform
                uintptr_t render = 0;
                if (!SafeRead(at + kEntityRenderObject, &render) || render == 0)
                    continue;
                uintptr_t renderVptr = 0;
                if (!SafeRead(render, &renderVptr) || renderVptr != renderVtable)
                    continue;

                float position[3];
                if (!ReadRenderTransform(render, position))
                    continue;

                PickCandidate candidate;
                candidate.entity = at;
                candidate.name = "placed object";
                candidate.realBounds = false;
                candidate.hasMesh = true;

                // Real extents from the spatial entity's own bounds virtual.
                // Without this every box is a 1.5 m cube on a point origin and
                // the ray misses everything, which is exactly what the 12:43
                // acceptance run did on all 1,883 candidates.
                const WorldBounds bounds = ReadEntityBounds(at);
                if (bounds.valid)
                {
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        candidate.minimum[axis] = bounds.minimum[axis];
                        candidate.maximum[axis] = bounds.maximum[axis];
                    }
                    candidate.realBounds = true;
                    ++withRealBounds;
                }
                else
                {
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        candidate.minimum[axis] = position[axis] - kFallbackHalfExtent;
                        candidate.maximum[axis] = position[axis] + kFallbackHalfExtent;
                    }
                }

                candidate.pushedBounds = false;
                for (int axis = 0; axis < 3; ++axis)
                    candidate.origin[axis] = position[axis];
                into->push_back(candidate);
                ++found;

                if (into->size() >= cap)
                    break;
            }
        }

        address = end;
        if (address == 0)
            break;
    }

    LogLine("renderscan: %zu qwords scanned, %zu placed entities, %zu with real bounds",
            scanned, found, withRealBounds);
    return found;
}

static size_t BuildPickCache(size_t cap)
{
    const std::vector<EntityRow> rows = EnumerateEntities(cap);

    std::vector<PickCandidate> fresh;
    fresh.reserve(rows.size());
    LONG real = 0;

    const LONG faultsAtStart = g_boundsFaults;
    const LONG kBoundsFaultBudget = 16;

    for (size_t i = 0; i < rows.size(); ++i)
    {
        const EntityRow& row = rows[i];

        PickCandidate candidate;
        candidate.entity = row.entity;
        candidate.name = row.name;
        candidate.realBounds = false;
        candidate.hasMesh = NameLooksRenderable(row.name) || EntityHasMesh(row.entity);

        // Stop asking once it is clear the call is not what we think it is.
        const bool askBounds = row.hasTransform &&
                               (g_boundsFaults - faultsAtStart) < kBoundsFaultBudget;

        const WorldBounds bounds = askBounds ? ReadEntityBounds(row.entity) : WorldBounds{};
        if (bounds.valid)
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                candidate.minimum[axis] = bounds.minimum[axis];
                candidate.maximum[axis] = bounds.maximum[axis];
            }
            candidate.realBounds = true;
            ++real;
            const float rowPosition[3] = { row.x, row.y, row.z };
            for (int axis = 0; axis < 3; ++axis)
                candidate.origin[axis] = bounds.haveOrigin
                    ? bounds.origin[axis]
                    : (row.hasTransform ? rowPosition[axis]
                                        : (candidate.minimum[axis] + candidate.maximum[axis]) * 0.5f);
        }
        else if (row.hasTransform)
        {
            const float position[3] = { row.x, row.y, row.z };
            for (int axis = 0; axis < 3; ++axis)
            {
                candidate.origin[axis] = position[axis];
                candidate.minimum[axis] = position[axis] - kFallbackHalfExtent;
                candidate.maximum[axis] = position[axis] + kFallbackHalfExtent;
            }
        }
        else
        {
            continue;      // no bounds and nowhere to put it
        }

        candidate.pushedBounds = false;
        fresh.push_back(candidate);
    }

    LockPick();
    g_pickCache.swap(fresh);
    g_pickCacheReal = real;
    const size_t count = g_pickCache.size();
    UnlockPick();

    const LONG faulted = g_boundsFaults - faultsAtStart;
    LogLine("pickcache: %zu entities walked, %zu pickable, %ld with real bounds, "
            "%ld computeBoundsInfo faults%s",
            rows.size(), count, real, faulted,
            faulted >= kBoundsFaultBudget ? " - gave up on slot 26, boxed the rest" : "");
    return count;
}

// Slab intersection. Returns the near distance along a unit direction, or a
// negative number for a miss. A ray starting inside the box hits at zero.
static float RayAabb(const float origin[3], const float direction[3],
                     const float minimum[3], const float maximum[3])
{
    float nearest = 0.0f;
    float furthest = 3.402823466e+38f;

    for (int axis = 0; axis < 3; ++axis)
    {
        if (fabsf(direction[axis]) < 1e-8f)
        {
            if (origin[axis] < minimum[axis] || origin[axis] > maximum[axis])
                return -1.0f;
            continue;
        }

        const float inverse = 1.0f / direction[axis];
        float entry = (minimum[axis] - origin[axis]) * inverse;
        float exit = (maximum[axis] - origin[axis]) * inverse;
        if (entry > exit)
        {
            const float swap = entry;
            entry = exit;
            exit = swap;
        }

        if (entry > nearest) nearest = entry;
        if (exit < furthest) furthest = exit;
        if (nearest > furthest)
            return -1.0f;
    }

    return nearest;
}

// Fills the snapshot the frame handler draws from. Sequence goes odd, then even:
// the handler skips anything it catches mid-write rather than locking against an
// engine thread.
// Replace the label without disturbing the box the pick already published.
static void PublishSelectionLabelOnly(const char* label)
{
    InterlockedIncrement(&g_snapshotSequence);
    strcpy_s(g_selectionSnapshot.label, label != nullptr ? label : "");
    InterlockedIncrement(&g_snapshotSequence);
}

static void PublishSelection(uintptr_t entity, const char* label,
                             const float* aabbMin, const float* aabbMax)
{
    if (entity == 0)
    {
        InterlockedIncrement(&g_snapshotSequence);
        g_selectionSnapshot.hasTransform = false;
        g_selectionSnapshot.label[0] = '\0';
        InterlockedIncrement(&g_snapshotSequence);
        return;
    }

    Transform where{};
    const bool hasTransform = GuardedGetTransform(entity, &where);

    float centre[3] = { 0.0f, 0.0f, 0.0f };
    float halfExtent[3] = { kFallbackHalfExtent, kFallbackHalfExtent, kFallbackHalfExtent };
    float boxLeft[3] = { 1.0f, 0.0f, 0.0f };
    float boxUp[3] = { 0.0f, 1.0f, 0.0f };
    float boxForward[3] = { 0.0f, 0.0f, 1.0f };

    const WorldBounds bounds = ReadEntityBounds(entity);
    if (bounds.valid)
    {
        const float localCentre[3] = {
            (bounds.localMinimum[0] + bounds.localMaximum[0]) * 0.5f,
            (bounds.localMinimum[1] + bounds.localMaximum[1]) * 0.5f,
            (bounds.localMinimum[2] + bounds.localMaximum[2]) * 0.5f
        };
        for (int axis = 0; axis < 3; ++axis)
        {
            centre[axis] = bounds.transform.translation[axis] +
                           bounds.transform.left[axis] * localCentre[0] +
                           bounds.transform.up[axis] * localCentre[1] +
                           bounds.transform.forward[axis] * localCentre[2];
            halfExtent[axis] = (bounds.localMaximum[axis] - bounds.localMinimum[axis]) * 0.5f;
            boxLeft[axis] = bounds.transform.left[axis];
            boxUp[axis] = bounds.transform.up[axis];
            boxForward[axis] = bounds.transform.forward[axis];
        }
    }
    else if (hasTransform)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            centre[axis] = where.translation[axis];
            boxLeft[axis] = where.left[axis];
            boxUp[axis] = where.up[axis];
            boxForward[axis] = where.forward[axis];
        }
    }
    else
    {
        return;
    }

    InterlockedIncrement(&g_snapshotSequence);

    g_selectionSnapshot.hasAabb = (aabbMin != nullptr && aabbMax != nullptr);
    if (g_selectionSnapshot.hasAabb)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            g_selectionSnapshot.aabbMinimum[axis] = aabbMin[axis];
            g_selectionSnapshot.aabbMaximum[axis] = aabbMax[axis];
        }
    }

    for (int axis = 0; axis < 3; ++axis)
    {
        g_selectionSnapshot.centre[axis] = centre[axis];
        g_selectionSnapshot.halfExtent[axis] = halfExtent[axis];
        g_selectionSnapshot.boxLeft[axis] = boxLeft[axis];
        g_selectionSnapshot.boxUp[axis] = boxUp[axis];
        g_selectionSnapshot.boxForward[axis] = boxForward[axis];
        g_selectionSnapshot.origin[axis] = hasTransform ? where.translation[axis] : centre[axis];
        g_selectionSnapshot.left[axis] = hasTransform ? where.left[axis] : 0.0f;
        g_selectionSnapshot.up[axis] = hasTransform ? where.up[axis] : 0.0f;
        g_selectionSnapshot.forward[axis] = hasTransform ? where.forward[axis] : 0.0f;
    }

    const float span = halfExtent[0] + halfExtent[1] + halfExtent[2];
    g_selectionSnapshot.arm = span > 0.0f ? span * 0.5f : 1.0f;
    g_selectionSnapshot.hasTransform = hasTransform;
    strcpy_s(g_selectionSnapshot.label, label != nullptr ? label : "");

    InterlockedIncrement(&g_snapshotSequence);
}

// The picker the left click uses. Casts the view centre against the cache and
// takes the nearest box.
static std::string PickFromCamera(float range)
{
    Transform camera{};
    float fov = 0.0f;
    if (!ReadCamera(&camera, &fov))
        return "no render view - is a level loaded?";

    LockPick();
    const bool empty = g_pickCache.empty();
    UnlockPick();

    if (empty && BuildPickCache(20000) == 0)
        return "no pickable entities - the sublevel walk found nothing (try pickcache)";

    const float origin[3] = { camera.translation[0], camera.translation[1],
                              camera.translation[2] };
    const float direction[3] = { camera.forward[0], camera.forward[1], camera.forward[2] };

    size_t skippedContaining = 0;
    size_t skippedHuge = 0;
    size_t skippedNoMesh = 0;
    float bestDiagonal = 3.402823466e+38f;
    float bestMinimum[3] = { 0.0f, 0.0f, 0.0f };
    float bestMaximum[3] = { 0.0f, 0.0f, 0.0f };
    uintptr_t bestEntity = 0;
    std::string bestName;
    float bestDistance = range;
    bool bestReal = false;
    float bestCentre[3] = { 0.0f, 0.0f, 0.0f };
    size_t tested = 0;

    LockPick();
    for (size_t i = 0; i < g_pickCache.size(); ++i)
    {
        const PickCandidate& candidate = g_pickCache[i];
        ++tested;

        // Skip anything the camera is standing inside. RayAabb returns 0 for a
        // ray starting inside a box, so a level-sized volume - terrain, a
        // streaming bounds - wins every click at zero distance and hides every
        // real object behind it. That is what limited the 13:10 run to a single
        // selectable object.
        if (g_pickMeshesOnly && !candidate.hasMesh)
        {
            ++skippedNoMesh;
            continue;
        }

        bool containsCamera = true;
        float span[3];
        for (int axis = 0; axis < 3; ++axis)
        {
            span[axis] = candidate.maximum[axis] - candidate.minimum[axis];
            if (origin[axis] < candidate.minimum[axis] || origin[axis] > candidate.maximum[axis])
                containsCamera = false;
        }
        const float diagonal = sqrtf(span[0] * span[0] + span[1] * span[1] +
                                     span[2] * span[2]);

        // Only discard a box that contains the camera when it is big enough to
        // be a volume rather than a room. pickdump showed the palace halls are
        // 100-195 m boxes and you stand inside them, so blanket-skipping
        // containers threw away exactly what the user was clicking on. A small
        // box you are standing in is a legitimate pick.
        if (containsCamera && diagonal > kContainerDiagonal)
        {
            ++skippedContaining;
            continue;
        }

        // Skip anything too large to be a prop. This used to require ALL THREE
        // axes to be oversized, which let a flat-but-enormous object - a terrain
        // slab, a water plane - through the filter and swallow every click. Any
        // single axis over the limit is enough to disqualify it.
        if (span[0] > kMaxPickableSpan || span[1] > kMaxPickableSpan ||
            span[2] > kMaxPickableSpan)
        {
            ++skippedHuge;
            continue;
        }

        const float distance = RayAabb(origin, direction, candidate.minimum, candidate.maximum);
        if (distance < 0.0f || distance > range)
            continue;

        // Prefer the SMALLEST box, not the nearest entry point. Boxes overlap
        // constantly - a lamppost sits inside a courtyard's bounds, which sits
        // inside a terrain tile's - and nearest-entry always returns the biggest
        // container. Smallest-box returns the thing you were actually aiming at.
        if (diagonal >= bestDiagonal)
            continue;
        bestDiagonal = diagonal;
        for (int axis = 0; axis < 3; ++axis)
        {
            bestMinimum[axis] = candidate.minimum[axis];
            bestMaximum[axis] = candidate.maximum[axis];
        }

        bestDistance = distance;
        bestEntity = candidate.entity;
        bestName = candidate.name;
        bestReal = candidate.realBounds;
        for (int axis = 0; axis < 3; ++axis)
            bestCentre[axis] = (candidate.minimum[axis] + candidate.maximum[axis]) * 0.5f;
    }
    UnlockPick();

    // Nothing struck a box. Rather than report nothing - which is what the 12:43
    // run did on all 1,883 candidates - take the candidate nearest the ray,
    // within a distance-scaled cone. A click should always select something:
    // selecting the wrong object is recoverable, selecting nothing is a dead
    // tool. Only reached when no real AABB was hit.
    if (bestEntity == 0)
    {
        float bestScore = 1.0f;
        LockPick();
        for (size_t i = 0; i < g_pickCache.size(); ++i)
        {
            const PickCandidate& candidate = g_pickCache[i];
            float centre[3], toward[3];
            for (int axis = 0; axis < 3; ++axis)
            {
                centre[axis] = (candidate.minimum[axis] + candidate.maximum[axis]) * 0.5f;
                toward[axis] = centre[axis] - origin[axis];
            }

            const float along = toward[0] * direction[0] + toward[1] * direction[1] +
                                toward[2] * direction[2];
            if (along <= 1.0f || along > range)
                continue;

            float perpendicular = 0.0f;
            for (int axis = 0; axis < 3; ++axis)
            {
                const float d = toward[axis] - direction[axis] * along;
                perpendicular += d * d;
            }
            perpendicular = sqrtf(perpendicular);

            // Tolerance grows with distance so far objects are not favoured.
            const float allowed = 2.0f + along * 0.06f;
            const float score = perpendicular / allowed;
            if (score < bestScore)
            {
                bestScore = score;
                bestEntity = candidate.entity;
                bestName = candidate.name;
                bestReal = false;
                bestDistance = along;
                for (int axis = 0; axis < 3; ++axis)
                {
                    bestCentre[axis] = centre[axis];
                    bestMinimum[axis] = candidate.minimum[axis];
                    bestMaximum[axis] = candidate.maximum[axis];
                }
            }
        }
        UnlockPick();

        if (bestEntity != 0)
            LogLine("pick: no box hit, took nearest-to-ray 0x%llX at %.1f m",
                    static_cast<unsigned long long>(bestEntity), bestDistance);
    }

    char buffer[512];
    if (bestEntity == 0)
    {
        g_selectedEntity = 0;
        g_selectionLabel[0] = '\0';
        PublishSelection(0, nullptr, nullptr, nullptr);
        size_t cacheSize = 0;
        size_t withMesh = 0;
        size_t withReal = 0;
        LockPick();
        cacheSize = g_pickCache.size();
        for (size_t i = 0; i < g_pickCache.size(); ++i)
        {
            if (g_pickCache[i].hasMesh) ++withMesh;
            if (g_pickCache[i].realBounds) ++withReal;
        }
        UnlockPick();

        sprintf_s(buffer,
                  "no hit - %zu of %zu tested; skipped: no-mesh %zu, container %zu, huge %zu; "
                  "cache has %zu with a mesh flag and %zu with real bounds; "
                  "ray from %.1f %.1f %.1f along %.3f %.3f %.3f",
                  tested, cacheSize, skippedNoMesh, skippedContaining, skippedHuge,
                  withMesh, withReal,
                  origin[0], origin[1], origin[2],
                  direction[0], direction[1], direction[2]);
        LogLine("pick: %s", buffer);
        return buffer;
    }

    g_selectedEntity = bestEntity;

    // Two lines, the way the reference editor does it: asset path on top, the
    // runtime class underneath. Position moves to the reply instead - it is
    // useful to us and it is not on the screenshot.
    const std::string runtimeClass = EntityRuntimeClass(bestEntity);
    const std::string ownPath = FindOwnAssetPath(bestEntity);
    const std::string topLine = ownPath.empty() ? bestName : ownPath;
    if (!runtimeClass.empty())
        sprintf_s(g_selectionLabel, "%s\n%s", topLine.c_str(), runtimeClass.c_str());
    else
        sprintf_s(g_selectionLabel, "%s", topLine.c_str());

    PublishSelection(bestEntity, g_selectionLabel, bestMinimum, bestMaximum);

    sprintf_s(buffer, "hit '%s' at 0x%llX, %.1f m away, centre %.1f %.1f %.1f (%s bounds, "
                      "%zu candidates, %zu no mesh, %zu contained the camera, %zu too large, "
                      "box %.1f m)",
              bestName.c_str(), static_cast<unsigned long long>(bestEntity), bestDistance,
              bestCentre[0], bestCentre[1], bestCentre[2],
              bestReal ? "real" : "fallback", tested, skippedNoMesh, skippedContaining,
              skippedHuge,
              bestDiagonal < 1e30f ? bestDiagonal : 0.0f);
    LogLine("pick: %s", buffer);
    return buffer;
}


static DWORD WINAPI SelectionDrawThread(LPVOID)
{
    g_selectionDrawing = true;

    while (g_selectionDrawing)
    {
        const uintptr_t entity = g_selectedEntity;
        if (!g_showSelection || entity == 0 || (g_drawRoute & kDrawThread) == 0)
        {
            Sleep(50);
            continue;
        }

        void* renderer = GetDebugRenderer();
        if (renderer == nullptr)
        {
            Sleep(100);
            continue;
        }

        Transform where{};
        const bool hasTransform = GuardedGetTransform(entity, &where);

        float centre[3];
        float halfExtent[3] = { 1.0f, 1.0f, 1.0f };

        const WorldBounds bounds = ReadEntityBounds(entity);
        if (bounds.valid)
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                centre[axis] = (bounds.minimum[axis] + bounds.maximum[axis]) * 0.5f;
                halfExtent[axis] = (bounds.maximum[axis] - bounds.minimum[axis]) * 0.5f;
            }
        }
        else if (hasTransform)
        {
            for (int axis = 0; axis < 3; ++axis)
                centre[axis] = where.translation[axis];
        }
        else
        {
            Sleep(100);
            continue;
        }

        SetDebugState(renderer, kSlotSetDepthTest, false);
        SetDebugState(renderer, kSlotSetDepthWrite, false);
        SetDebugTimeVisible(renderer, 0.25f);

        DrawBox3d(renderer, centre, halfExtent, 0xFFFFFFFFu);

        // Three coloured axes at the origin, so the thing reads as a selection
        // rather than a stray box.
        if (hasTransform)
        {
            const float* origin = where.translation;
            float end[3];
            const float length = halfExtent[0] + halfExtent[1] + halfExtent[2];
            const float arm = length > 0.0f ? length * 0.5f : 1.0f;

            for (int axis = 0; axis < 3; ++axis) end[axis] = origin[axis] - where.left[axis] * arm;
            DrawLine3d(renderer, origin, end, 0xFF0000FFu);          // X, red
            for (int axis = 0; axis < 3; ++axis) end[axis] = origin[axis] + where.up[axis] * arm;
            DrawLine3d(renderer, origin, end, 0xFF00FF00u);          // Y, green
            for (int axis = 0; axis < 3; ++axis) end[axis] = origin[axis] + where.forward[axis] * arm;
            DrawLine3d(renderer, origin, end, 0xFFFF0000u);          // Z, blue
        }

        if (g_selectionLabel[0] != '\0')
        {
            float labelAt[3] = { centre[0], centre[1] + halfExtent[1] + 0.5f, centre[2] };
            DrawText3d(renderer, labelAt, g_selectionLabel, 0xFF00FFFFu);   // yellow
        }

        SetDebugState(renderer, kSlotSetDepthWrite, true);
        SetDebugState(renderer, kSlotSetDepthTest, true);
        Sleep(30);
    }

    return 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// grabframe <file.bmp> - the frame the game presents, read from its own swap chain.
//
// Window captures stop working when another window holds the foreground (someone else using the PC while a capture
// runs): the foreground-only screen grab skips, and PrintWindow(PW_RENDERFULLCONTENT) returned
// a buffer without the display conversion (a near-black sky over clipped snow in every exterior shot, the known-good
// I5 build included). This hooks IDXGISwapChain::Present / Present1 on dxgi's shared swap-chain vtable (found through a
// throwaway 8x8 swap chain) and, on request, copies back buffer 0 to a staging texture on the presenting thread,
// converts it to 8-bit BGR by format and writes a 24-bit BMP. The hook stays installed (a pass-through when idle).
typedef HRESULT (STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* Present1Fn)(IDXGISwapChain*, UINT, UINT, const void*);
static PresentFn g_origPresent = nullptr;
static Present1Fn g_origPresent1 = nullptr;
static bool g_presentHooked = false;
static volatile LONG g_captureWanted = 0;
static char g_capturePath[MAX_PATH] = {};
static std::string g_captureResult;
static HANDLE g_captureEvent = nullptr;

static float HalfToFloat(uint16_t h)
{
    const uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
    uint32_t f;
    if (exp == 0)
    {
        if (man == 0) f = sign;
        else { float v = std::ldexp(static_cast<float>(man), -24); return (h & 0x8000u) ? -v : v; }
    }
    else if (exp == 31) f = sign | 0x7F800000u | (man << 13);
    else f = sign | ((exp + 112) << 23) | (man << 13);
    float out;
    memcpy(&out, &f, 4);
    return out;
}

static uint8_t LinearToSrgb8(float v)
{
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f) return 255;
    const float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::lround(s * 255.0f));
}

static std::string WriteBmp24(const char* path, const std::vector<uint8_t>& bgr, int w, int h)
{
    const int row = (w * 3 + 3) & ~3;
    std::vector<uint8_t> file(54 + static_cast<size_t>(row) * h, 0);
    auto put32 = [&](size_t at, uint32_t v) { memcpy(&file[at], &v, 4); };
    auto put16 = [&](size_t at, uint16_t v) { memcpy(&file[at], &v, 2); };
    file[0] = 'B'; file[1] = 'M';
    put32(2, static_cast<uint32_t>(file.size())); put32(10, 54); put32(14, 40);
    put32(18, static_cast<uint32_t>(w)); put32(22, static_cast<uint32_t>(h)); put16(26, 1); put16(28, 24);
    put32(34, static_cast<uint32_t>(row * h));
    for (int y = 0; y < h; ++y)
        memcpy(&file[54 + static_cast<size_t>(h - 1 - y) * row], &bgr[static_cast<size_t>(y) * w * 3], static_cast<size_t>(w) * 3);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || f == nullptr) return std::string("could not open ") + path;
    fwrite(file.data(), 1, file.size(), f);
    fclose(f);
    return "";
}

static std::string CaptureSwapChain(IDXGISwapChain* swapChain)
{
    ID3D11Texture2D* back = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || back == nullptr)
        return "GetBuffer(0) failed (not a D3D11 swap chain?)";
    D3D11_TEXTURE2D_DESC desc = {};
    back->GetDesc(&desc);
    ID3D11Device* device = nullptr;
    back->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);

    ID3D11Texture2D* source = back;
    ID3D11Texture2D* resolved = nullptr;
    if (desc.SampleDesc.Count > 1)
    {
        D3D11_TEXTURE2D_DESC rd = desc;
        rd.SampleDesc.Count = 1; rd.SampleDesc.Quality = 0; rd.BindFlags = 0; rd.MiscFlags = 0;
        if (SUCCEEDED(device->CreateTexture2D(&rd, nullptr, &resolved)))
        {
            context->ResolveSubresource(resolved, 0, back, 0, desc.Format);
            source = resolved;
        }
    }
    D3D11_TEXTURE2D_DESC sd = desc;
    sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0; sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0; sd.MipLevels = 1; sd.ArraySize = 1;
    ID3D11Texture2D* staging = nullptr;
    std::string result;
    if (FAILED(device->CreateTexture2D(&sd, nullptr, &staging)) || staging == nullptr)
        result = "could not create the staging texture";
    else
    {
        context->CopyResource(staging, source);
        D3D11_MAPPED_SUBRESOURCE map = {};
        if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &map)))
            result = "Map failed";
        else
        {
            const int w = static_cast<int>(desc.Width), h = static_cast<int>(desc.Height);
            std::vector<uint8_t> bgr(static_cast<size_t>(w) * h * 3);
            bool known = true;
            for (int y = 0; y < h && known; ++y)
            {
                const uint8_t* src = static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch;
                uint8_t* dst = &bgr[static_cast<size_t>(y) * w * 3];
                for (int x = 0; x < w; ++x, dst += 3)
                {
                    switch (desc.Format)
                    {
                    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                        dst[0] = src[x * 4 + 2]; dst[1] = src[x * 4 + 1]; dst[2] = src[x * 4 + 0];
                        break;
                    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
                    case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
                        dst[0] = src[x * 4 + 0]; dst[1] = src[x * 4 + 1]; dst[2] = src[x * 4 + 2];
                        break;
                    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
                    {
                        uint32_t v;
                        memcpy(&v, src + x * 4, 4);
                        dst[2] = static_cast<uint8_t>((v & 0x3FF) >> 2);
                        dst[1] = static_cast<uint8_t>(((v >> 10) & 0x3FF) >> 2);
                        dst[0] = static_cast<uint8_t>(((v >> 20) & 0x3FF) >> 2);
                        break;
                    }
                    case DXGI_FORMAT_R16G16B16A16_FLOAT:   // scRGB: linear, 1.0 = SDR white
                    {
                        uint16_t c[3];
                        memcpy(c, src + x * 8, 6);
                        dst[2] = LinearToSrgb8(HalfToFloat(c[0]));
                        dst[1] = LinearToSrgb8(HalfToFloat(c[1]));
                        dst[0] = LinearToSrgb8(HalfToFloat(c[2]));
                        break;
                    }
                    default:
                        known = false;
                        break;
                    }
                }
            }
            context->Unmap(staging, 0);
            if (!known)
                result = "unsupported back-buffer format " + std::to_string(static_cast<int>(desc.Format));
            else
            {
                std::string err = WriteBmp24(g_capturePath, bgr, w, h);
                if (!err.empty())
                    result = err;
                else
                {
                    char line[400];
                    sprintf_s(line, "captured %dx%d format %d samples %u -> %s", w, h, static_cast<int>(desc.Format),
                              desc.SampleDesc.Count, g_capturePath);
                    result = line;
                }
            }
        }
        staging->Release();
    }
    if (resolved) resolved->Release();
    context->Release();
    device->Release();
    back->Release();
    return result;
}

static void ServeCaptureRequest(IDXGISwapChain* swapChain)
{
    if (InterlockedCompareExchange(&g_captureWanted, 0, 1) != 1)
        return;
    try { g_captureResult = CaptureSwapChain(swapChain); }
    catch (...) { g_captureResult = "capture faulted"; }
    if (g_captureEvent) SetEvent(g_captureEvent);
}

static void OverlayFrameGuarded(IDXGISwapChain* swapChain);    // the feed overlay, defined with the feeds

// frametimes: per-Present frame intervals (QPC) into a fixed ring, for stutter measurement. Cheap: one QPC read and
// one store per frame.
static const int kFrameRing = 8192;
static volatile LONG64 g_frameIndex = 0;
static LONG64 g_frameLastQpc = 0;
static float g_frameMs[kFrameRing];

static void FrameTick()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    if (g_frameLastQpc != 0)
    {
        const float ms = static_cast<float>((now.QuadPart - g_frameLastQpc) * 1000.0 / freq.QuadPart);
        const LONG64 i = InterlockedIncrement64(&g_frameIndex) - 1;
        g_frameMs[i % kFrameRing] = ms;
    }
    g_frameLastQpc = now.QuadPart;
}

static HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain* swapChain, UINT sync, UINT flags)
{
    FrameTick();
    OverlayFrameGuarded(swapChain);         // drawn first, so grabframe captures it too
    ServeCaptureRequest(swapChain);
    return g_origPresent(swapChain, sync, flags);
}

static HRESULT STDMETHODCALLTYPE Present1Hook(IDXGISwapChain* swapChain, UINT sync, UINT flags, const void* params)
{
    FrameTick();
    OverlayFrameGuarded(swapChain);         // drawn first, so grabframe captures it too
    ServeCaptureRequest(swapChain);
    return g_origPresent1(swapChain, sync, flags, params);
}

static bool PatchVtableSlot(uintptr_t* slot, uintptr_t value, uintptr_t* original)
{
    DWORD previous = 0;
    if (!VirtualProtect(slot, sizeof(uintptr_t), PAGE_READWRITE, &previous)) return false;
    *original = *slot;
    *slot = value;
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(uintptr_t), previous, &ignored);
    return true;
}

static std::string InstallPresentHook()
{
    if (g_presentHooked) return "";
    HWND window = CreateWindowExA(0, "STATIC", "gamelink-capture", WS_OVERLAPPEDWINDOW, 0, 0, 8, 8,
                                  nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
    if (window == nullptr) return "could not create the probe window";
    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = 1;
    desc.BufferDesc.Width = 8;
    desc.BufferDesc.Height = 8;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* probe = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HMODULE d3d11 = LoadLibraryA("d3d11.dll");
    typedef HRESULT (WINAPI* CreateFn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                       const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*,
                                       ID3D11DeviceContext**);
    CreateFn create = d3d11 ? reinterpret_cast<CreateFn>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain")) : nullptr;
    HRESULT hr = create ? create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc,
                                 &probe, &device, nullptr, &context) : E_FAIL;
    if (FAILED(hr) || probe == nullptr)
    {
        DestroyWindow(window);
        return "D3D11CreateDeviceAndSwapChain failed";
    }
    // every DXGI swap chain in the process is the same dxgi.dll class: one shared vtable (Present 8, Present1 22)
    uintptr_t* vtable = *reinterpret_cast<uintptr_t**>(probe);
    IDXGISwapChain1* probe1 = nullptr;
    const bool hasPresent1 = SUCCEEDED(probe->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&probe1)));
    uintptr_t orig = 0;
    const bool ok = PatchVtableSlot(&vtable[8], reinterpret_cast<uintptr_t>(&PresentHook), &orig);
    if (ok) g_origPresent = reinterpret_cast<PresentFn>(orig);
    if (ok && hasPresent1 && probe1 != nullptr)
    {
        uintptr_t* vtable1 = *reinterpret_cast<uintptr_t**>(probe1);
        uintptr_t orig1 = 0;
        if (PatchVtableSlot(&vtable1[22], reinterpret_cast<uintptr_t>(&Present1Hook), &orig1))
            g_origPresent1 = reinterpret_cast<Present1Fn>(orig1);
    }
    if (probe1) probe1->Release();
    probe->Release();
    context->Release();
    device->Release();
    DestroyWindow(window);
    if (!ok) return "could not patch the swap-chain vtable";
    if (g_captureEvent == nullptr) g_captureEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    g_presentHooked = true;
    LogLine("capture: Present hooked on dxgi's swap-chain vtable");
    return "";
}

static std::string CaptureCommand(const char* path)
{
    if (path == nullptr || *path == '\0') return "usage: capture <file.bmp>";
    std::string err = InstallPresentHook();
    if (!err.empty()) return "capture: " + err;
    strncpy_s(g_capturePath, sizeof(g_capturePath), path, _TRUNCATE);
    g_captureResult.clear();
    ResetEvent(g_captureEvent);
    InterlockedExchange(&g_captureWanted, 1);
    if (WaitForSingleObject(g_captureEvent, 10000) != WAIT_OBJECT_0)
    {
        InterlockedExchange(&g_captureWanted, 0);
        return "capture: no Present within 10 s";
    }
    return g_captureResult;
}

// ---- game-only input -----------------------------------------------------------------------------
//
// BFII reads mouse and keyboard as RAW INPUT (WM_INPUT), so posted WM_KEYDOWN/WM_LBUTTONDOWN reach its window but
// not its input system (measured 2026-09-27: 12 posted deploy sequences, the player never left class select).
// The harness therefore used SendInput, which is global: it typed into whatever the user was using and moved their
// cursor. This feeds synthetic raw input to the GAME only:
//   * the packed executable resolves its imports at run time, so the redirect is a scan of the unpacked module
//     image for pointers to the user32 functions, each slot re-pointed at a hook here (process-local, reversible);
//   * rawkey/rawclick queue synthetic RAWINPUT records and post WM_INPUT with a marker handle; the GetRawInputData
//     hook answers the marker from the queue, and the GetRawInputBuffer hook appends queued records to a real read;
//   * fakefocus makes GetForegroundWindow/GetActiveWindow/GetFocus answer the game window, GetCursorPos answer the
//     synthetic cursor, and ClipCursor/SetCursorPos do nothing - the game never takes the user's real cursor.

struct FakeRaw { RAWINPUT ri; };
static CRITICAL_SECTION g_fakeLock;
static volatile LONG g_fakeLockInit = 0;
static std::vector<FakeRaw> g_fakeQueue;        // pending records for the GetRawInputBuffer path
static std::map<uintptr_t, FakeRaw> g_fakeByHandle;   // marker handle -> record, for the GetRawInputData path
static uintptr_t g_fakeNextHandle = 0x7FAB0000;
static volatile LONG g_fakeFocus = 0;
static POINT g_fakeCursor = { 0, 0 };
static HWND g_fakeWindow = nullptr;

typedef UINT (WINAPI* GetRawInputDataFn)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
typedef UINT (WINAPI* GetRawInputBufferFn)(PRAWINPUT, PUINT, UINT);
typedef HWND (WINAPI* NoArgHwndFn)();
typedef BOOL (WINAPI* GetCursorPosFn)(LPPOINT);
typedef BOOL (WINAPI* ClipCursorFn)(const RECT*);
typedef BOOL (WINAPI* SetCursorPosFn)(int, int);
static GetRawInputDataFn g_realGetRawInputData = nullptr;
static GetRawInputBufferFn g_realGetRawInputBuffer = nullptr;
static NoArgHwndFn g_realGetForegroundWindow = nullptr, g_realGetActiveWindow = nullptr, g_realGetFocus = nullptr;
static GetCursorPosFn g_realGetCursorPos = nullptr;
static ClipCursorFn g_realClipCursor = nullptr;
static SetCursorPosFn g_realSetCursorPos = nullptr;
static volatile LONG g_rawDataCalls = 0, g_rawBufferCalls = 0;

static void FakeLockInit()
{
    if (InterlockedCompareExchange(&g_fakeLockInit, 1, 0) == 0)
        InitializeCriticalSection(&g_fakeLock);
}

// ---- F1 menu: input capture ---------------------------------------------------------------------------------
// While the menu is open the game must see no keyboard/mouse input, and the menu must get it. BFII reads input as
// raw input, possibly without legacy window messages, so both directions go through the raw-input hooks below:
// every real record is copied into the menu's queue and then neutralised for the game - key-downs become key-ups
// (nothing sticks), mouse deltas, wheel and button-downs are cleared (button-ups are kept, so a button held while
// opening is released). Synthetic records (rawkey/rawclick) are not touched.
static MenuState g_menu;
struct UiKey { USHORT vk; USHORT scan; bool down; };
static CRITICAL_SECTION g_uiInputLock;
static volatile LONG g_uiInputLockInit = 0;
static std::vector<UiKey> g_uiKeys;
static volatile LONG g_uiWheel = 0;
static volatile LONG g_menuScrubbed = 0;
static volatile LONG g_inputHooksPatched = 0;      // import slots redirected by InstallInputHooks

static void UiInputLockInit()
{
    if (InterlockedCompareExchange(&g_uiInputLockInit, 1, 0) == 0)
        InitializeCriticalSection(&g_uiInputLock);
}

static void MenuConsumeRaw(RAWINPUT* ri)
{
    if (ri->header.dwType == RIM_TYPEKEYBOARD)
    {
        RAWKEYBOARD& k = ri->data.keyboard;
        const bool down = (k.Flags & RI_KEY_BREAK) == 0;
        UiInputLockInit();
        EnterCriticalSection(&g_uiInputLock);
        if (g_uiKeys.size() < 256)
            g_uiKeys.push_back({k.VKey, k.MakeCode, down});
        LeaveCriticalSection(&g_uiInputLock);
        k.Flags |= RI_KEY_BREAK;
        if (k.Message == WM_KEYDOWN) k.Message = WM_KEYUP;
        else if (k.Message == WM_SYSKEYDOWN) k.Message = WM_SYSKEYUP;
    }
    else if (ri->header.dwType == RIM_TYPEMOUSE)
    {
        RAWMOUSE& m = ri->data.mouse;
        if ((m.usButtonFlags & RI_MOUSE_WHEEL) != 0)
            InterlockedAdd(&g_uiWheel, static_cast<SHORT>(m.usButtonData));
        const USHORT ups = RI_MOUSE_LEFT_BUTTON_UP | RI_MOUSE_RIGHT_BUTTON_UP | RI_MOUSE_MIDDLE_BUTTON_UP |
                           RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP;
        m.lLastX = 0;
        m.lLastY = 0;
        m.usButtonFlags &= ups;
        m.usButtonData = 0;
    }
    InterlockedIncrement(&g_menuScrubbed);
}

static bool MenuIsOpen()
{
    return g_menu.IsOpen();
}

static UINT WINAPI HookGetRawInputData(HRAWINPUT h, UINT command, LPVOID data, PUINT size, UINT headerSize)
{
    InterlockedIncrement(&g_rawDataCalls);
    const uintptr_t key = reinterpret_cast<uintptr_t>(h);
    if (key >= 0x7FAB0000 && key < 0x7FAC0000 && size != nullptr)
    {
        FakeRaw rec = {};
        bool found = false;
        EnterCriticalSection(&g_fakeLock);
        auto it = g_fakeByHandle.find(key);
        if (it != g_fakeByHandle.end()) { rec = it->second; found = true; }
        LeaveCriticalSection(&g_fakeLock);
        if (!found) return static_cast<UINT>(-1);
        const UINT want = command == RID_HEADER ? sizeof(RAWINPUTHEADER) : rec.ri.header.dwSize;
        if (data == nullptr) { *size = want; return 0; }
        if (*size < want) { *size = want; return static_cast<UINT>(-1); }
        memcpy(data, &rec.ri, want);
        if (command == RID_INPUT)
        {
            EnterCriticalSection(&g_fakeLock);
            g_fakeByHandle.erase(key);
            LeaveCriticalSection(&g_fakeLock);
        }
        return want;
    }
    const UINT got = g_realGetRawInputData(h, command, data, size, headerSize);
    if (MenuIsOpen() && command == RID_INPUT && data != nullptr && got != static_cast<UINT>(-1) &&
        got >= sizeof(RAWINPUTHEADER))
        MenuConsumeRaw(static_cast<RAWINPUT*>(data));
    return got;
}

static UINT WINAPI HookGetRawInputBuffer(PRAWINPUT data, PUINT size, UINT headerSize)
{
    InterlockedIncrement(&g_rawBufferCalls);
    const UINT real = g_realGetRawInputBuffer(data, size, headerSize);
    if (data == nullptr || size == nullptr || real == static_cast<UINT>(-1))
        return real;
    // Append queued records after the real ones (each block 8-aligned, as NEXTRAWINPUTBLOCK walks them).
    unsigned char* cursor = reinterpret_cast<unsigned char*>(data);
    const bool menu = MenuIsOpen();
    for (UINT i = 0; i < real; ++i)
    {
        PRAWINPUT block = reinterpret_cast<PRAWINPUT>(cursor);
        if (menu)
            MenuConsumeRaw(block);              // real records only; the fakes are appended below
        cursor += (block->header.dwSize + 7) & ~7u;
    }
    const unsigned char* end = reinterpret_cast<unsigned char*>(data) + *size;
    UINT added = 0;
    EnterCriticalSection(&g_fakeLock);
    while (!g_fakeQueue.empty())
    {
        const RAWINPUT& ri = g_fakeQueue.front().ri;
        const UINT step = (ri.header.dwSize + 7) & ~7u;
        if (cursor + step > end) break;
        memcpy(cursor, &ri, ri.header.dwSize);
        cursor += step;
        g_fakeQueue.erase(g_fakeQueue.begin());
        ++added;
    }
    LeaveCriticalSection(&g_fakeLock);
    return real + added;
}

static HWND WINAPI HookGetForegroundWindow() { return (g_fakeFocus && g_fakeWindow) ? g_fakeWindow : g_realGetForegroundWindow(); }
static HWND WINAPI HookGetActiveWindow() { return (g_fakeFocus && g_fakeWindow) ? g_fakeWindow : g_realGetActiveWindow(); }
static HWND WINAPI HookGetFocus() { return (g_fakeFocus && g_fakeWindow) ? g_fakeWindow : g_realGetFocus(); }
static BOOL WINAPI HookGetCursorPos(LPPOINT p)
{
    if (g_fakeFocus && p != nullptr) { *p = g_fakeCursor; return TRUE; }
    return g_realGetCursorPos(p);
}
// The game confines and recentres the cursor every frame; while the menu is open it may not.
static BOOL WINAPI HookClipCursor(const RECT* r) { return (g_fakeFocus || MenuIsOpen()) ? TRUE : g_realClipCursor(r); }
static BOOL WINAPI HookSetCursorPos(int x, int y) { return (g_fakeFocus || MenuIsOpen()) ? TRUE : g_realSetCursorPos(x, y); }

// Re-points every pointer-sized slot in the module image that holds `target` (an import slot of the packed
// executable) at `hook`. Returns the number of slots patched.
static int PatchPointerSlots(HMODULE module, void* target, void* hook)
{
    MODULEINFO info = {};
    if (!GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)))
        return 0;
    unsigned char* base = static_cast<unsigned char*>(info.lpBaseOfDll);
    unsigned char* stop = base + info.SizeOfImage;
    int patched = 0;
    MEMORY_BASIC_INFORMATION mbi = {};
    for (unsigned char* p = base; p < stop; p += mbi.RegionSize)
    {
        if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0 || mbi.RegionSize == 0) break;
        const DWORD prot = mbi.Protect & 0xFF;
        const bool readable = mbi.State == MEM_COMMIT &&
            (prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY);   // data, not code
        if (!readable) continue;
        uintptr_t* q = reinterpret_cast<uintptr_t*>(mbi.BaseAddress);
        uintptr_t* qend = reinterpret_cast<uintptr_t*>(static_cast<unsigned char*>(mbi.BaseAddress) + mbi.RegionSize);
        for (; q < qend; ++q)
        {
            if (*q != reinterpret_cast<uintptr_t>(target)) continue;
            DWORD old = 0;
            if (VirtualProtect(q, sizeof(uintptr_t), PAGE_READWRITE, &old))
            {
                *q = reinterpret_cast<uintptr_t>(hook);
                DWORD ignored = 0;
                VirtualProtect(q, sizeof(uintptr_t), old, &ignored);
                ++patched;
            }
        }
    }
    return patched;
}

static std::string InstallInputHooks()
{
    FakeLockInit();
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    HMODULE game = GetModuleHandleW(nullptr);
    if (user32 == nullptr || game == nullptr) return "inputhook: user32 or the game module not found";
    struct Spec { const char* name; void** real; void* hook; };
    Spec specs[] = {
        { "GetRawInputData", reinterpret_cast<void**>(&g_realGetRawInputData), reinterpret_cast<void*>(&HookGetRawInputData) },
        { "GetRawInputBuffer", reinterpret_cast<void**>(&g_realGetRawInputBuffer), reinterpret_cast<void*>(&HookGetRawInputBuffer) },
        { "GetForegroundWindow", reinterpret_cast<void**>(&g_realGetForegroundWindow), reinterpret_cast<void*>(&HookGetForegroundWindow) },
        { "GetActiveWindow", reinterpret_cast<void**>(&g_realGetActiveWindow), reinterpret_cast<void*>(&HookGetActiveWindow) },
        { "GetFocus", reinterpret_cast<void**>(&g_realGetFocus), reinterpret_cast<void*>(&HookGetFocus) },
        { "GetCursorPos", reinterpret_cast<void**>(&g_realGetCursorPos), reinterpret_cast<void*>(&HookGetCursorPos) },
        { "ClipCursor", reinterpret_cast<void**>(&g_realClipCursor), reinterpret_cast<void*>(&HookClipCursor) },
        { "SetCursorPos", reinterpret_cast<void**>(&g_realSetCursorPos), reinterpret_cast<void*>(&HookSetCursorPos) },
    };
    std::string out = "inputhook:";
    char line[160];
    for (const Spec& s : specs)
    {
        void* fn = reinterpret_cast<void*>(GetProcAddress(user32, s.name));
        if (fn == nullptr) continue;
        if (*s.real == nullptr) *s.real = fn;
        const int n = PatchPointerSlots(game, fn, s.hook);
        InterlockedAdd(&g_inputHooksPatched, n);
        sprintf_s(line, " %s=%d", s.name, n);
        out += line;
    }
    g_fakeWindow = FindOwnGameWindow();
    LogLine("%s", out.c_str());
    return out;
}

static HANDLE FirstRawDevice(DWORD type)
{
    UINT count = 0;
    GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST));
    if (count == 0) return nullptr;
    std::vector<RAWINPUTDEVICELIST> list(count);
    if (GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1)) return nullptr;
    for (const RAWINPUTDEVICELIST& d : list) if (d.dwType == type) return d.hDevice;
    return nullptr;
}

// Queues one synthetic record for both read paths and posts WM_INPUT with its marker handle.
static void PushFakeRaw(const RAWINPUT& ri)
{
    FakeRaw rec = {};
    rec.ri = ri;
    uintptr_t key;
    EnterCriticalSection(&g_fakeLock);
    key = g_fakeNextHandle++;
    if (g_fakeNextHandle >= 0x7FAC0000) g_fakeNextHandle = 0x7FAB0000;
    g_fakeByHandle[key] = rec;
    g_fakeQueue.push_back(rec);
    LeaveCriticalSection(&g_fakeLock);
    if (g_fakeWindow != nullptr)
        PostMessageW(g_fakeWindow, WM_INPUT, RIM_INPUT, static_cast<LPARAM>(key));
}

static std::string FakeKey(UINT scan, bool extended)
{
    const UINT vk = MapVirtualKeyW(scan, MAPVK_VSC_TO_VK);
    for (int phase = 0; phase < 2; ++phase)
    {
        RAWINPUT ri = {};
        ri.header.dwType = RIM_TYPEKEYBOARD;
        ri.header.dwSize = sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD);
        ri.header.hDevice = FirstRawDevice(RIM_TYPEKEYBOARD);
        ri.header.wParam = RIM_INPUT;
        ri.data.keyboard.MakeCode = static_cast<USHORT>(scan);
        ri.data.keyboard.Flags = static_cast<USHORT>((phase ? RI_KEY_BREAK : RI_KEY_MAKE) | (extended ? RI_KEY_E0 : 0));
        ri.data.keyboard.VKey = static_cast<USHORT>(vk);
        ri.data.keyboard.Message = phase ? WM_KEYUP : WM_KEYDOWN;
        PushFakeRaw(ri);
        if (phase == 0) Sleep(60);
    }
    char buffer[120];
    sprintf_s(buffer, "rawkey scan 0x%02X (vk 0x%02X) queued", scan, vk);
    return buffer;
}

static std::string FakeClick(float fx, float fy)
{
    if (g_fakeWindow == nullptr) g_fakeWindow = FindOwnGameWindow();
    if (g_fakeWindow == nullptr) return "rawclick: no game window";
    RECT client = {};
    GetClientRect(g_fakeWindow, &client);
    POINT p = { static_cast<LONG>(fx * (client.right - client.left)), static_cast<LONG>(fy * (client.bottom - client.top)) };
    ClientToScreen(g_fakeWindow, &p);
    g_fakeCursor = p;
    // client-space mouse messages too: some UI layers read WM_MOUSEMOVE / button messages rather than raw buttons
    const LPARAM at = MAKELPARAM(static_cast<int>(fx * (client.right - client.left)), static_cast<int>(fy * (client.bottom - client.top)));
    PostMessageW(g_fakeWindow, WM_MOUSEMOVE, 0, at);
    const USHORT flags[2] = { RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP };
    for (int phase = 0; phase < 2; ++phase)
    {
        RAWINPUT ri = {};
        ri.header.dwType = RIM_TYPEMOUSE;
        ri.header.dwSize = sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE);
        ri.header.hDevice = FirstRawDevice(RIM_TYPEMOUSE);
        ri.header.wParam = RIM_INPUT;
        ri.data.mouse.usFlags = MOUSE_MOVE_RELATIVE;
        ri.data.mouse.usButtonFlags = flags[phase];
        PushFakeRaw(ri);
        PostMessageW(g_fakeWindow, phase ? WM_LBUTTONUP : WM_LBUTTONDOWN, phase ? 0 : MK_LBUTTON, at);
        if (phase == 0) Sleep(80);
    }
    char buffer[120];
    sprintf_s(buffer, "rawclick screen %ld,%ld queued", p.x, p.y);
    return buffer;
}

// The game's own top-level window: the largest visible, unowned window of this process.
static HWND FindOwnGameWindow()
{
    struct Search { DWORD pid; HWND best; LONG area; } search = { GetCurrentProcessId(), nullptr, -1 };
    EnumWindows([](HWND h, LPARAM p) -> BOOL {
        Search* s = reinterpret_cast<Search*>(p);
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid != s->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER) != nullptr)
            return TRUE;
        RECT r = {};
        GetClientRect(h, &r);
        const LONG area = (r.right - r.left) * (r.bottom - r.top);
        if (area > s->area) { s->area = area; s->best = h; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.best;
}

// ---------------------------------------------------------------------------
// trace - live logic-event feed: "Delay.0b87df4b -> EventGate.00597302".
//
// FullEntityBus::internalFireEventOnConnection(bus, const EventConnection*, EntityEvent*)
// runs once for every wired event connection that fires (findings ledger #53).
// Every caller reaches it through the 5-byte jmp at game+0x114C860, so the hook
// retargets that one rel32 at a stub inside its range rather than stealing prologue
// bytes, and the original is called directly - no trampoline to build.
//
// The detour runs on the engine's threads, client and server, thousands of times a
// second. It copies five fields into a ring buffer and nothing else; type names are
// resolved when the buffer is read.
//
// EventConnection (32 bytes): Source +0x0, Target +0x8, SourceEvent.Id +0x10,
// TargetEvent.Id +0x14, TargetType +0x18 (4 NetworkedClient, 5 NetworkedClientAndServer).
// Bus +0x3C bit 28 is the server realm.
// ---------------------------------------------------------------------------

constexpr uintptr_t kFireOnConnectionThunkRva = 0x114C860;
constexpr uintptr_t kFireOnConnectionRva = 0x73285E0;
constexpr size_t kTraceCapacity = size_t(1) << 18;     // ~37 s of unfiltered events at ~7k/s

struct TraceEntry
{
    volatile LONG64 seq;        // index + 1, written last; anything else = not ready / overwritten
    uintptr_t source;
    uintptr_t target;
    int32_t sourceEvent;
    int32_t targetEvent;
    int32_t targetType;
    uint32_t server;
    uintptr_t owner;            // the bus's EntityBusData: the blueprint that holds this wire
};

typedef void(__fastcall* FireOnConnectionFn)(void* bus, const void* connection, void* entityEvent);
static void LoadHashNames();

// ---- deferred arming --------------------------------------------------------------
// Right after injection (the frontend) the hook sites read as zero: the protected image
// unpacks those pages later. Turning a feed on then (the harness's "feeds" option) is
// remembered instead of refused, and a thread arms it once the expected bytes appear.
// A site holding some OTHER jump is still refused - that is somebody else's hook.
static SRWLOCK g_feedLock = SRWLOCK_INIT;
static volatile LONG g_feedWanted = 0;             // bit 1 = trace, bit 2 = prop
static volatile LONG g_feedThreadStarted = 0;
static std::string InstallEventTraceUnlocked(bool enable);
static std::string InstallPropTraceUnlocked(bool enable);

static DWORD WINAPI FeedArmThread(LPVOID)
{
    for (int attempt = 0; attempt < 3600 && g_feedWanted != 0; ++attempt)     // 30 min
    {
        Sleep(500);
        AcquireSRWLockExclusive(&g_feedLock);
        if ((g_feedWanted & 1) != 0)
        {
            const std::string result = InstallEventTraceUnlocked(true);
            if (result.compare(0, 8, "deferred") != 0)
            {
                InterlockedAnd(&g_feedWanted, ~1L);
                LogLine("feeds: deferred trace -> %s", result.c_str());
            }
        }
        if ((g_feedWanted & 2) != 0)
        {
            const std::string result = InstallPropTraceUnlocked(true);
            if (result.compare(0, 8, "deferred") != 0)
            {
                InterlockedAnd(&g_feedWanted, ~2L);
                LogLine("feeds: deferred prop -> %s", result.c_str());
            }
        }
        ReleaseSRWLockExclusive(&g_feedLock);
    }
    InterlockedExchange(&g_feedThreadStarted, 0);
    return 0;
}

// Patching the hook sites before a level is up broke the game every time (2026-10-08,
// four launches): at injection or the frontend the process stalled behind the
// protection's "activation" window or exited with no fault logged; after LOADED the same
// patches ran clean in every run. So both feeds only ever arm with a level loaded.
static bool FeedLevelLoaded()
{
    uintptr_t gameContext = 0, clientLevel = 0;
    SafeRead(g_base + kClientGameContextRva, &gameContext);
    if (gameContext != 0)
        SafeRead(gameContext + kClientGameContextLevelOffset, &clientLevel);
    return clientLevel != 0;
}

static std::string FeedDefer(LONG bit, const char* name)
{
    InterlockedOr(&g_feedWanted, bit);
    if (InterlockedCompareExchange(&g_feedThreadStarted, 1, 0) == 0)
    {
        HANDLE thread = CreateThread(nullptr, 0, FeedArmThread, nullptr, 0, nullptr);
        if (thread != nullptr)
            CloseHandle(thread);
    }
    return std::string("deferred: ") + name + " arms itself once a level is loaded";
}

static TraceEntry* g_trace = nullptr;
static volatile LONG64 g_traceNext = 0;
static LONG64 g_traceRead = 0;
static LONG64 g_traceDropped = 0;
static bool g_traceInstalled = false;
static bool g_traceAddresses = false;
static int g_traceGuids = 1;            // 0 none, 1 short "#1a2b3c4d", 2 full GUID
static bool g_traceWhere = true;        // append "@<blueprint path>"
static std::map<uintptr_t, std::pair<uintptr_t, std::string>> g_traceOwnerNames;
static unsigned char* g_traceStub = nullptr;
static FireOnConnectionFn g_traceOriginal = nullptr;
static std::string g_traceFilter;
static std::map<uintptr_t, std::string> g_traceTypeNames;

// 'trace pause' freezes both feeds' recording (hooks stay in) so the last seconds before
// something odd stay in the buffers to be read at leisure; 'trace resume' carries on.
static volatile bool g_feedsPaused = false;

static void TraceRecord(uintptr_t bus, uintptr_t connection)
{
    if (g_feedsPaused)
        return;
    __try
    {
        const LONG64 index = InterlockedIncrement64(&g_traceNext) - 1;
        TraceEntry& entry = g_trace[index & (kTraceCapacity - 1)];
        entry.seq = 0;
        _WriteBarrier();
        entry.source = *reinterpret_cast<const uintptr_t*>(connection + 0x00);
        entry.target = *reinterpret_cast<const uintptr_t*>(connection + 0x08);
        entry.sourceEvent = *reinterpret_cast<const int32_t*>(connection + 0x10);
        entry.targetEvent = *reinterpret_cast<const int32_t*>(connection + 0x14);
        entry.targetType = *reinterpret_cast<const int32_t*>(connection + 0x18);
        entry.server = (*reinterpret_cast<const uint32_t*>(bus + 0x3C) >> 28) & 1;
        const uintptr_t runtime = *reinterpret_cast<const uintptr_t*>(bus + 0x68);
        entry.owner = runtime != 0 ? *reinterpret_cast<const uintptr_t*>(runtime + 0xB8) : 0;
        MemoryBarrier();
        entry.seq = index + 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// ---- fire / deliver, carried out on the engine's own logic thread -------------------
// A 'fire' or 'deliver' request is queued by the command thread and carried out here, in the
// event hook, right after a real event on a bus of the requested realm has been delivered -
// so it runs on the thread that runs that realm's logic, never alongside it.
constexpr uintptr_t kFireEventByIdRva = 0x114AB50;   // EntityBus::fireEvent(bus, source, EventId, ...)
constexpr uintptr_t kRegisterEventRva = 0x1180770;   // EventAndPropertyModificationQueue::registerEvent(realm, peer, event)
constexpr uintptr_t kEntityEventVtableRva = 0x2D7BF78;   // the plain EntityEvent, as fireEvent builds it
typedef void(__fastcall* FireByIdFn)(void* bus, const void* source, uint32_t eventId, uintptr_t unused);
typedef void(__fastcall* RegisterEventFn)(uintptr_t realm, void* peer, void* entityEvent);

struct FireRequest
{
    volatile LONG state;        // 0 idle, 1 queued, 2 running, 3 done
    bool deliver;               // false: emit from source on its bus; true: deliver into entity
    uintptr_t bus, source, entity;
    uint32_t eventId, realm;
    bool faulted;
    void (*job)(void*);         // set: run this instead of a fire/deliver (inspect / write)
    void* jobContext;
};
static FireRequest g_fireRequest = {};
static HANDLE g_fireDone = nullptr;

static void ExecuteFireRequest()
{
    __try
    {
        if (g_fireRequest.job != nullptr)
            g_fireRequest.job(g_fireRequest.jobContext);
        else if (g_fireRequest.deliver)
        {
            unsigned char ev[16] = {};
            const uintptr_t vtable = g_base + kEntityEventVtableRva;
            memcpy(ev, &vtable, 8);
            memcpy(ev + 8, &g_fireRequest.eventId, 4);
            const uint32_t senderParent = 1;            // as if a wire delivered it
            memcpy(ev + 12, &senderParent, 4);
            reinterpret_cast<RegisterEventFn>(g_base + kRegisterEventRva)(
                g_fireRequest.realm, reinterpret_cast<void*>(g_fireRequest.entity), ev);
        }
        else
        {
            reinterpret_cast<FireByIdFn>(g_base + kFireEventByIdRva)(
                reinterpret_cast<void*>(g_fireRequest.bus), reinterpret_cast<const void*>(g_fireRequest.source),
                g_fireRequest.eventId, 0);
        }
        g_fireRequest.faulted = false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_fireRequest.faulted = true;
    }
}

static void ServeFireRequestRealm(uint32_t realm)
{
    if (g_fireRequest.state != 1 || realm != g_fireRequest.realm)
        return;
    if (InterlockedCompareExchange(&g_fireRequest.state, 2, 1) != 1)
        return;
    ExecuteFireRequest();
    InterlockedExchange(&g_fireRequest.state, 3);
    if (g_fireDone != nullptr)
        SetEvent(g_fireDone);
}

// From the event hook: the bus's realm (+0x3C bit 28). The property hook serves too (see
// PropDetour) - on a quiet spawn screen the server fires no events but writes properties.
static void ServeFireRequest(uintptr_t bus)
{
    if (g_fireRequest.state != 1)
        return;
    uint32_t flags = 0;
    if (SafeRead(bus + 0x3C, &flags))
        ServeFireRequestRealm((flags >> 28) & 1);
}

static void __fastcall TraceDetour(void* bus, const void* connection, void* entityEvent)
{
    TraceRecord(reinterpret_cast<uintptr_t>(bus), reinterpret_cast<uintptr_t>(connection));
    g_traceOriginal(bus, connection, entityEvent);
    ServeFireRequest(reinterpret_cast<uintptr_t>(bus));
}

// The stub has to sit within rel32 reach of the thunk: walk down from below the
// image, then up from above it, one allocation granule at a time.
static unsigned char* AllocateNear(uintptr_t target, size_t size)
{
    const uintptr_t granule = 0x10000;
    const uintptr_t imageEnd = g_base + kExpectedModuleSize;
    for (uintptr_t step = 1; step < 0x7000; ++step)
    {
        const uintptr_t below = (g_base & ~(granule - 1)) - step * granule;
        if (target - below < 0x7FFF0000)
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(below), size,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))
                return static_cast<unsigned char*>(p);

        const uintptr_t above = ((imageEnd + granule - 1) & ~(granule - 1)) + step * granule;
        if (above - target < 0x7FFF0000)
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(above), size,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))
                return static_cast<unsigned char*>(p);
    }
    return nullptr;
}

// Hook stubs must sit within rel32 reach of the code they patch. Late in a big level that space can
// be full (Scarif 2015, 2026-10-08: "could not allocate a stub within reach"), so one block is
// reserved when gamelink attaches, while the address space is still empty, and stubs are carved
// from it. Never freed; AllocateNear stays the fallback.
static unsigned char* g_nearArena = nullptr;
static size_t g_nearArenaUsed = 0;
constexpr size_t kNearArenaSize = 0x10000;

static unsigned char* NearCode(uintptr_t target, size_t size)
{
    size = (size + 15) & ~size_t(15);
    if (g_nearArena != nullptr && g_nearArenaUsed + size <= kNearArenaSize)
    {
        unsigned char* p = g_nearArena + g_nearArenaUsed;
        const intptr_t reach = reinterpret_cast<intptr_t>(p) - static_cast<intptr_t>(target);
        if (reach > -0x7FFF0000LL && reach < 0x7FFF0000LL)
        {
            g_nearArenaUsed += size;
            return p;
        }
    }
    return AllocateNear(target, size);
}

// Rewrites the jmp's rel32 as one aligned 8-byte exchange, so a thread executing the
// thunk sees either the old target or the new one and never half of each.
static bool RetargetThunk(uintptr_t thunk, uintptr_t destination)
{
    const intptr_t delta = static_cast<intptr_t>(destination) - static_cast<intptr_t>(thunk + 5);
    if (delta < INT32_MIN || delta > INT32_MAX)
        return false;

    const uintptr_t aligned = thunk & ~uintptr_t(7);
    const size_t at = thunk - aligned;
    if (at + 5 > 8)
        return false;

    DWORD previous = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(aligned), 8, PAGE_EXECUTE_READWRITE, &previous))
        return false;

    unsigned char bytes[8];
    memcpy(bytes, reinterpret_cast<const void*>(aligned), 8);
    const int32_t rel = static_cast<int32_t>(delta);
    bytes[at] = 0xE9;
    memcpy(bytes + at + 1, &rel, sizeof(rel));
    LONG64 value = 0;
    memcpy(&value, bytes, 8);
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(aligned), value);

    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(aligned), 8, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(aligned), 8);
    return true;
}

// ---------------------------------------------------------------------------
// 64v64: the 64-slot pool (vtable game+0x2D7C988, allocator game+0x5589E90 behind thunk game+0x290780).
// Its used-mask at +0xA0D0 is one qword; with all 64 bits set the allocator's free-slot scan (inc / bt / jb)
// never ends - the client freezes once a pool needs a 65th entry. poolfix: the thunk goes to PoolAllocDetour,
// which returns 0 for a full pool, and the only known caller (game+0x55732A8, owner +0x78 entry, +0x70 state)
// gets a stub that marks the owner pending only when it actually got an entry.
// ---------------------------------------------------------------------------
typedef uintptr_t (*PoolAllocFn)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static PoolAllocFn g_poolAllocOriginal = nullptr;
static volatile LONG g_poolRefused = 0;
static volatile LONG g_poolCalls = 0;
static bool g_poolFixOn = false;
// The caller tail stub records each owner (rdi) it sees here, newest at [g_poolOwnerNext-1].
static volatile uintptr_t g_poolOwners[64];
static volatile LONG64 g_poolOwnerNext = 0;
static volatile uintptr_t g_poolOwnerSlot = 0;

static uintptr_t PoolAllocDetour(uintptr_t self, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    InterlockedIncrement(&g_poolCalls);
    const uintptr_t owner = g_poolOwnerSlot;
    if (owner != 0)
    {
        bool known = false;
        for (int i = 0; i < 64; ++i)
            if (g_poolOwners[i] == owner) { known = true; break; }
        if (!known)
            g_poolOwners[InterlockedIncrement64(&g_poolOwnerNext) % 64] = owner;
    }
    uint64_t mask = 0;
    if (SafeRead(self + 0xA0D0, &mask) && mask == ~0ull)
    {
        InterlockedIncrement(&g_poolRefused);
        return 0;
    }
    return g_poolAllocOriginal(self, a, b, c, d);
}

static std::string PoolFixInstall()
{
    if (g_poolFixOn)
        return "poolfix already on";
    const uintptr_t thunk = g_base + 0x290780;
    const uintptr_t alloc = g_base + 0x5589E90;
    const uintptr_t site = g_base + 0x55732AA;          // mov [rdi+78h],rax / mov dword [rdi+70h],1 / jmp +2F
    const uintptr_t resume = g_base + 0x55732E6;
    static const unsigned char expectSite[] = {0x48, 0x89, 0x47, 0x78, 0xC7, 0x47, 0x70, 0x01, 0x00, 0x00, 0x00, 0xEB, 0x2F};
    unsigned char thunkBytes[5] = {}, siteBytes[sizeof(expectSite)] = {};
    SafeReadBytes(thunk, thunkBytes, 5);
    SafeReadBytes(site, siteBytes, sizeof(siteBytes));
    int32_t rel = 0;
    memcpy(&rel, thunkBytes + 1, 4);
    if (thunkBytes[0] != 0xE9 || thunk + 5 + rel != alloc)
        return "poolfix: thunk game+0x290780 is not 'jmp game+0x5589E90' - different build?";
    if (memcmp(siteBytes, expectSite, sizeof(expectSite)) != 0)
        return "poolfix: caller bytes at game+0x55732AA differ - different build?";
    g_poolAllocOriginal = reinterpret_cast<PoolAllocFn>(alloc);

    // stub 1: absolute jump to the detour, reachable from the thunk
    unsigned char* jumpStub = NearCode(thunk, 16);
    // stub 2: the caller's tail
    unsigned char* tail = NearCode(site, 48);
    if (jumpStub == nullptr || tail == nullptr)
        return "poolfix: no near memory for the stubs";
    const uintptr_t detour = reinterpret_cast<uintptr_t>(&PoolAllocDetour);
    unsigned char js[12] = {0x48, 0xB8};                 // mov rax, imm64 ; jmp rax
    memcpy(js + 2, &detour, 8);
    js[10] = 0xFF; js[11] = 0xE0;
    memcpy(jumpStub, js, sizeof(js));

    // tail: mov [rdi+78h],rax ; test rax,rax ; jz L ; mov dword [rdi+70h],1 ;
    //   L: push rax ; mov rax, &g_poolOwnerSlot ; mov [rax], rdi ; pop rax ; jmp resume
    // (one shared slot, copied into the ring by PoolAllocDetour on the next call - good enough for sampling)
    unsigned char t[48];
    size_t n = 0;
    const unsigned char a1[] = {0x48, 0x89, 0x47, 0x78, 0x48, 0x85, 0xC0, 0x74, 0x07, 0xC7, 0x47, 0x70, 0x01, 0x00, 0x00, 0x00};
    memcpy(t, a1, sizeof(a1)); n = sizeof(a1);
    {
        const uintptr_t slot = reinterpret_cast<uintptr_t>(&g_poolOwnerSlot);
        t[n++] = 0x50;                                   // push rax
        t[n++] = 0x48; t[n++] = 0xB8;                    // mov rax, imm64
        memcpy(t + n, &slot, 8); n += 8;
        t[n++] = 0x48; t[n++] = 0x89; t[n++] = 0x38;     // mov [rax], rdi
        t[n++] = 0x58;                                   // pop rax
    }
    t[n++] = 0xE9;
    const int32_t back = static_cast<int32_t>(static_cast<intptr_t>(resume) - static_cast<intptr_t>(reinterpret_cast<uintptr_t>(tail) + n + 4));
    memcpy(t + n, &back, 4); n += 4;
    memcpy(tail, t, n);
    FlushInstructionCache(GetCurrentProcess(), tail, n);
    FlushInstructionCache(GetCurrentProcess(), jumpStub, sizeof(js));

    // caller: jmp tail + nops (13 bytes)
    unsigned char patch[sizeof(expectSite)];
    memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xE9;
    const int32_t to = static_cast<int32_t>(static_cast<intptr_t>(reinterpret_cast<uintptr_t>(tail)) - static_cast<intptr_t>(site + 5));
    memcpy(patch + 1, &to, 4);
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(site), sizeof(patch), PAGE_EXECUTE_READWRITE, &old))
        return "poolfix: VirtualProtect on the caller failed";
    memcpy(reinterpret_cast<void*>(site), patch, sizeof(patch));
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(site), sizeof(patch), old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), sizeof(patch));

    if (!RetargetThunk(thunk, reinterpret_cast<uintptr_t>(jumpStub)))
        return "poolfix: caller patched but the thunk retarget failed";
    g_poolFixOn = true;
    LogLine("poolfix: on - thunk game+0x290780 -> stub 0x%p -> detour, caller tail stub 0x%p", jumpStub, tail);
    char buf[200];
    sprintf_s(buf, "poolfix on (jump stub 0x%p, tail stub 0x%p)", jumpStub, tail);
    return buf;
}

// ---------------------------------------------------------------------------
// exitwatch: who ends the process? ntdll!NtTerminateProcess is patched with an absolute jump to
// TerminateDetour, which logs the call stack (frames inside the game module) and then runs a copy of the
// original syscall stub. A process that vanishes with no such line died by __fastfail (int 29h).
// ---------------------------------------------------------------------------
typedef LONG(NTAPI* NtTerminateFn)(HANDLE, LONG);
static NtTerminateFn g_ntTerminateTrampoline = nullptr;
static bool g_exitWatchOn = false;

static LONG NTAPI TerminateDetour(HANDLE process, LONG status)
{
    void* frames[48] = {};
    const USHORT n = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
    char line[1400];
    size_t used = 0;
    used += _snprintf_s(line + used, sizeof(line) - used, _TRUNCATE, "exitwatch: NtTerminateProcess(handle %p, status 0x%08lX) thread %lu stack:",
                        process, static_cast<unsigned long>(status), GetCurrentThreadId());
    for (USHORT i = 0; i < n && used + 40 < sizeof(line); ++i)
    {
        const uintptr_t f = reinterpret_cast<uintptr_t>(frames[i]);
        char where[96];
        DescribeAddress(where, sizeof(where), f);
        used += _snprintf_s(line + used, sizeof(line) - used, _TRUNCATE, " %s", where);
    }
    LogLine("%s", line);
    Sleep(50);
    return g_ntTerminateTrampoline(process, status);
}

static std::string ExitWatchInstall()
{
    if (g_exitWatchOn)
        return "exitwatch already on";
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    unsigned char* target = ntdll ? reinterpret_cast<unsigned char*>(GetProcAddress(ntdll, "NtTerminateProcess")) : nullptr;
    if (target == nullptr)
        return "exitwatch: NtTerminateProcess not found";
    // expected Win10/11 stub: 4C 8B D1 B8 xx xx 00 00 F6 04 25 08 03 FE 7F 01 75 03 0F 05 C3 CD 2E C3
    if (!(target[0] == 0x4C && target[1] == 0x8B && target[2] == 0xD1 && target[3] == 0xB8 && target[8] == 0xF6 &&
          target[18] == 0x0F && target[19] == 0x05 && target[20] == 0xC3 && target[21] == 0xCD && target[22] == 0x2E))
        return "exitwatch: unexpected NtTerminateProcess bytes - not patched";
    unsigned char* tramp = static_cast<unsigned char*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (tramp == nullptr)
        return "exitwatch: no trampoline memory";
    memcpy(tramp, target, 24);                         // position-independent syscall stub, both paths
    g_ntTerminateTrampoline = reinterpret_cast<NtTerminateFn>(tramp);
    unsigned char jmp[14] = {0xFF, 0x25, 0, 0, 0, 0};
    const uintptr_t detour = reinterpret_cast<uintptr_t>(&TerminateDetour);
    memcpy(jmp + 6, &detour, 8);
    DWORD old = 0;
    if (!VirtualProtect(target, sizeof(jmp), PAGE_EXECUTE_READWRITE, &old))
        return "exitwatch: VirtualProtect failed";
    memcpy(target, jmp, sizeof(jmp));
    DWORD ignored = 0;
    VirtualProtect(target, sizeof(jmp), old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(jmp));
    g_exitWatchOn = true;
    LogLine("exitwatch: on (NtTerminateProcess at %p, trampoline %p)", target, tramp);
    return "exitwatch on";
}

// ---------------------------------------------------------------------------
// poolgrow: real capacity past 64. The pool's owner calls it at game+0x55732A0 (mov rax,[rcx] / mov [rsp+20h],r15 /
// call [rax] / mov [rdi+78h],rax / mov dword [rdi+70h],1 / jmp +2F; rdi = owner, rcx = owner+0x338 pool). That
// site jumps to a stub that calls PoolAllocViaOwner: allocate from the owner's pool; when it is full, from an
// overflow pool of the same manager (owner+0x328) - created the way the manager creates its own (game+0x296220:
// alloc(0xA3D0, mgr+0x1D8) via game+0x1C6780, ctor game+0x287A30(pool, key), append to the fixed_vector<Pool*,32>
// at mgr+0x38230/+0x38238, capacity end mgr+0x38240), so the manager's per-frame loops update it and its
// destructor frees it. An owner served by an overflow pool gets owner+0x338 = that pool, so every later call
// (update, free) goes to the pool that holds its entry.
// ---------------------------------------------------------------------------
typedef uintptr_t (*GameAllocFn)(size_t size, uintptr_t arena);
typedef uintptr_t (*PoolCtorFn)(uintptr_t pool, int key);
struct OverflowSet
{
    uintptr_t manager;
    int count;
    uintptr_t pools[24];
};
static OverflowSet g_overflow[4];
static CRITICAL_SECTION g_overflowLock;
static volatile LONG g_overflowLockReady = 0;
static volatile LONG g_poolOverflowed = 0;
static volatile LONG g_poolCreated = 0;
static bool g_poolGrowOn = false;

static bool PoolFull(uintptr_t pool)
{
    uint64_t mask = 0;
    return !SafeRead(pool + 0xA0D0, &mask) || mask == ~0ull;
}

static uintptr_t OverflowPoolFor(uintptr_t manager, int key)
{
    if (manager == 0)
        return 0;
    EnterCriticalSection(&g_overflowLock);
    OverflowSet* set = nullptr;
    for (OverflowSet& s : g_overflow)
        if (s.manager == manager) { set = &s; break; }
    if (set == nullptr)
        for (OverflowSet& s : g_overflow)
            if (s.manager == 0) { s.manager = manager; s.count = 0; set = &s; break; }
    uintptr_t result = 0;
    if (set != nullptr)
    {
        for (int i = 0; i < set->count && result == 0; ++i)
            if (!PoolFull(set->pools[i]))
                result = set->pools[i];
        if (result == 0 && set->count < 24)
        {
            uintptr_t arena = 0, end = 0, cap = 0;
            SafeRead(manager + 0x1D8, &arena);
            SafeRead(manager + 0x38238, &end);
            SafeRead(manager + 0x38240, &cap);
            if (arena != 0 && end != 0 && end + 8 <= cap)
            {
                const uintptr_t pool = reinterpret_cast<GameAllocFn>(g_base + 0x1C6780)(0xA3D0, arena);
                if (pool != 0)
                {
                    // Same key as the pool it extends: the pool's queries pass the key (+0xA0E0) and the block the
                    // ctor builds from it (+0xA0E4) - with an invented key (run 19: 0x7000) the overflow bots spawned
                    // but never moved. Key lookups (manager vfuncs +0x60/+0x68) return the first match, i.e. the
                    // original pool, which sits earlier in the vector.
                    const int newKey = key;
                    reinterpret_cast<PoolCtorFn>(g_base + 0x287A30)(pool, newKey);
                    *reinterpret_cast<uintptr_t*>(end) = pool;
                    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(manager + 0x38238), static_cast<LONG64>(end + 8));
                    set->pools[set->count++] = pool;
                    InterlockedIncrement(&g_poolCreated);
                    LogLine("poolgrow: manager 0x%llX key %d -> overflow pool #%d at 0x%llX (key 0x%X), manager now has %llu pools",
                            static_cast<unsigned long long>(manager), key, set->count, static_cast<unsigned long long>(pool),
                            newKey, static_cast<unsigned long long>((end + 8 - (manager + 0x38250)) / 8));
                    result = pool;
                }
            }
        }
    }
    LeaveCriticalSection(&g_overflowLock);
    return result;
}

static void PoolAllocViaOwner(uintptr_t pool, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t owner)
{
    InterlockedIncrement(&g_poolCalls);
    const PoolAllocFn alloc = reinterpret_cast<PoolAllocFn>(g_base + 0x5589E90);
    uintptr_t used = pool, entry = 0;
    if (!PoolFull(pool))
        entry = alloc(pool, a, b, c, d);
    if (entry == 0)
    {
        uintptr_t manager = 0;
        int key = 0;
        SafeRead(owner + 0x328, &manager);
        SafeRead(pool + 0xA0E0, &key);
        used = OverflowPoolFor(manager, key);
        if (used != 0 && !PoolFull(used))
        {
            entry = alloc(used, a, b, c, d);
            if (entry != 0)
                InterlockedIncrement(&g_poolOverflowed);
        }
    }
    if (entry == 0)
    {
        InterlockedIncrement(&g_poolRefused);
        return;                                          // owner stays idle and asks again later
    }
    *reinterpret_cast<uintptr_t*>(owner + 0x78) = entry;
    *reinterpret_cast<int*>(owner + 0x70) = 1;
    if (used != pool)
        *reinterpret_cast<uintptr_t*>(owner + 0x338) = used;
}

static std::string PoolGrowInstall()
{
    if (g_poolGrowOn)
        return "poolgrow already on";
    if (g_poolFixOn)
        return "poolgrow: poolfix is on in this process - use one or the other";
    if (InterlockedCompareExchange(&g_overflowLockReady, 1, 0) == 0)
        InitializeCriticalSection(&g_overflowLock);
    const uintptr_t site = g_base + 0x55732A0;
    const uintptr_t resume = g_base + 0x55732E6;
    static const unsigned char expect[] = {0x48, 0x8B, 0x01, 0x4C, 0x89, 0x7C, 0x24, 0x20, 0xFF, 0x10, 0x48, 0x89, 0x47, 0x78,
                                           0xC7, 0x47, 0x70, 0x01, 0x00, 0x00, 0x00, 0xEB, 0x2F};
    unsigned char have[sizeof(expect)] = {};
    SafeReadBytes(site, have, sizeof(have));
    if (memcmp(have, expect, sizeof(expect)) != 0)
        return "poolgrow: call-site bytes at game+0x55732A0 differ - different build?";
    unsigned char* stub = NearCode(site, 64);
    if (stub == nullptr)
        return "poolgrow: no near memory for the stub";
    unsigned char s[48];
    size_t n = 0;
    const unsigned char head[] = {0x48, 0x83, 0xEC, 0x30,                   // sub rsp,30h
                                  0x48, 0x89, 0x7C, 0x24, 0x28,             // mov [rsp+28h],rdi   (owner, arg 6)
                                  0x4C, 0x89, 0x7C, 0x24, 0x20,             // mov [rsp+20h],r15   (arg 5)
                                  0x48, 0xB8};                              // mov rax, imm64
    memcpy(s, head, sizeof(head)); n = sizeof(head);
    const uintptr_t fn = reinterpret_cast<uintptr_t>(&PoolAllocViaOwner);
    memcpy(s + n, &fn, 8); n += 8;
    const unsigned char tail[] = {0xFF, 0xD0,                               // call rax
                                  0x48, 0x83, 0xC4, 0x30,                   // add rsp,30h
                                  0xE9};                                    // jmp resume
    memcpy(s + n, tail, sizeof(tail)); n += sizeof(tail);
    const int32_t back = static_cast<int32_t>(static_cast<intptr_t>(resume) - static_cast<intptr_t>(reinterpret_cast<uintptr_t>(stub) + n + 4));
    memcpy(s + n, &back, 4); n += 4;
    memcpy(stub, s, n);
    FlushInstructionCache(GetCurrentProcess(), stub, n);

    unsigned char patch[sizeof(expect)];
    memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xE9;
    const int32_t to = static_cast<int32_t>(static_cast<intptr_t>(reinterpret_cast<uintptr_t>(stub)) - static_cast<intptr_t>(site + 5));
    memcpy(patch + 1, &to, 4);
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(site), sizeof(patch), PAGE_EXECUTE_READWRITE, &old))
        return "poolgrow: VirtualProtect failed";
    memcpy(reinterpret_cast<void*>(site), patch, sizeof(patch));
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(site), sizeof(patch), old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), sizeof(patch));
    g_poolGrowOn = true;
    LogLine("poolgrow: on - game+0x55732A0 -> stub 0x%p -> PoolAllocViaOwner", stub);
    char buf[120];
    sprintf_s(buf, "poolgrow on (stub 0x%p)", stub);
    return buf;
}

static uintptr_t ThunkDestination(uintptr_t thunk)
{
    unsigned char bytes[5] = {};
    if (!SafeReadBytes(thunk, bytes, sizeof(bytes)) || bytes[0] != 0xE9)
        return 0;
    int32_t rel = 0;
    memcpy(&rel, bytes + 1, sizeof(rel));
    return thunk + 5 + static_cast<intptr_t>(rel);
}

static std::string InstallEventTraceUnlocked(bool enable)
{
    const uintptr_t thunk = g_base + kFireOnConnectionThunkRva;
    const uintptr_t original = g_base + kFireOnConnectionRva;

    if (!enable)
    {
        if (!g_traceInstalled)
            return "trace is not installed";
        if (!RetargetThunk(thunk, original))
            return "could not restore the thunk";
        g_traceInstalled = false;
        LogLine("trace: removed after %lld events", static_cast<long long>(g_traceNext));
        char buffer[128];
        sprintf_s(buffer, "trace off - %lld events seen", static_cast<long long>(g_traceNext));
        return buffer;
    }

    if (g_traceInstalled)
        return "trace already on";

    if (!FeedLevelLoaded())
        return FeedDefer(1, "trace");          // never patch before a level is up (see FeedLevelLoaded)
    const uintptr_t current = ThunkDestination(thunk);
    if (current == 0)
        return FeedDefer(1, "trace");          // code not unpacked yet: arm when it is
    if (current != original)
    {
        char buffer[192];
        sprintf_s(buffer, "refusing: the thunk at game+0x%llX jumps to 0x%llX, expected game+0x%llX",
                  static_cast<unsigned long long>(kFireOnConnectionThunkRva),
                  static_cast<unsigned long long>(current),
                  static_cast<unsigned long long>(kFireOnConnectionRva));
        return buffer;
    }

    if (g_trace == nullptr)
    {
        g_trace = static_cast<TraceEntry*>(VirtualAlloc(nullptr, sizeof(TraceEntry) * kTraceCapacity,
                                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (g_trace == nullptr)
            return "could not allocate the trace buffer";
    }

    // Allocated once and never freed: a thread can still be inside the detour after
    // 'trace off' has put the thunk back.
    if (g_traceStub == nullptr)
    {
        g_traceStub = NearCode(thunk, 0x100);
        if (g_traceStub == nullptr)
            return "could not allocate a stub within rel32 reach of the thunk";
        void* detour = reinterpret_cast<void*>(&TraceDetour);
        const unsigned char jump[6] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };   // jmp [rip+0]
        memcpy(g_traceStub, jump, sizeof(jump));
        memcpy(g_traceStub + sizeof(jump), &detour, sizeof(detour));
        FlushInstructionCache(GetCurrentProcess(), g_traceStub, 0x20);
    }

    LoadHashNames();
    g_traceOriginal = reinterpret_cast<FireOnConnectionFn>(original);
    g_traceRead = g_traceNext;
    g_traceDropped = 0;

    if (!RetargetThunk(thunk, reinterpret_cast<uintptr_t>(g_traceStub)))
        return "could not patch the thunk";

    g_traceInstalled = true;
    LogLine("trace: on, thunk game+0x%llX -> stub 0x%p -> detour",
            static_cast<unsigned long long>(kFireOnConnectionThunkRva), g_traceStub);
    return "trace on - 'trace' reads new events, 'trace off' removes the hook";
}

// "EventGateEntityData" -> "EventGate", the way the feed is meant to read.
static const std::string& TraceTypeName(uintptr_t data)
{
    static const std::string unknown = "?";
    uintptr_t typeInfo = 0;
    if (data == 0 || !SafeRead(data + 0x08, &typeInfo) || typeInfo == 0)
        return unknown;

    auto found = g_traceTypeNames.find(typeInfo);
    if (found != g_traceTypeNames.end())
        return found->second;

    std::string name;
    uintptr_t typeInfoData = 0, namePtr = 0;
    if (SafeRead(typeInfo, &typeInfoData) && typeInfoData != 0 && SafeRead(typeInfoData, &namePtr))
        name = SafeCString(namePtr, 128);
    if (name.empty())
        name = "?";
    for (const char* suffix : { "EntityData", "Data" })
    {
        const size_t n = strlen(suffix);
        if (name.size() > n && name.compare(name.size() - n, n, suffix) == 0)
        {
            name.erase(name.size() - n);
            break;
        }
    }
    return g_traceTypeNames.emplace(typeInfo, name).first->second;
}

// ---- instance identity -----------------------------------------------------------
// Only exported instances keep their GUID at runtime. The rest are identified by their
// ordinal in the partition, which is exactly the placeholder GUID Frosty shows for them
// (00000000-0000-0000-0000-00000000002e = instance 0x2e).
//
// A loaded partition is one block in file order, starting with the primary instance
// (the blueprint = the bus's EntityBusData): non-exported instances packed at 8-byte
// alignment, exported ones 16-aligned behind their 16-byte GUID. Each instance's size
// is its TypeInfo size. Verified against PF_Weapon_HeatLogic: 12 of 12 live offsets
// matched the XML dump's order (2026-10-07).

static std::unordered_set<uintptr_t> g_traceClassInfos;
static std::map<uintptr_t, std::pair<uintptr_t, std::unordered_map<uintptr_t, uint32_t>>> g_traceOrdinals;

static uint32_t TraceInstanceSize(uintptr_t data)
{
    uintptr_t vtable = 0, classInfo = 0, typeInfoData = 0;
    uint32_t size = 0;
    if (!SafeRead(data, &vtable) || vtable < g_base || vtable >= g_base + kExpectedModuleSize)
        return 0;
    if (!SafeRead(data + 0x08, &classInfo) || classInfo == 0)
        return 0;
    if (!g_traceClassInfos.empty() && g_traceClassInfos.count(classInfo) == 0)
        return 0;
    if (!SafeRead(classInfo, &typeInfoData) || typeInfoData == 0 ||
        !SafeRead(typeInfoData + 0x0A, &size) || size < 0x18 || size > 0x100000)
        return 0;
    return size;
}

static bool TraceHasGuid(uintptr_t data)
{
    uint16_t flags = 0;
    return SafeRead(data + kDataFlags, &flags) && (flags & kHasGuidFlag) != 0;
}

static const std::unordered_map<uintptr_t, uint32_t>& TraceOrdinals(uintptr_t owner)
{
    static const std::unordered_map<uintptr_t, uint32_t> none;
    uintptr_t namePtr = 0;
    if (owner == 0 || !SafeRead(owner + 0x18, &namePtr))
        return none;
    auto found = g_traceOrdinals.find(owner);
    if (found != g_traceOrdinals.end() && found->second.first == namePtr)
        return found->second.second;

    if (g_traceClassInfos.empty() && WalkRegistry())
    {
        LockRegistry();
        for (const TypeRec& type : g_types)
            g_traceClassInfos.insert(type.classInfo);
        UnlockRegistry();
    }

    auto& slot = g_traceOrdinals[owner];
    slot.first = namePtr;
    slot.second.clear();

    uint32_t size = TraceInstanceSize(owner);
    if (size == 0)
        return slot.second;
    slot.second[owner] = 0;
    uintptr_t end = owner + size;
    // Each instance is aligned to its own type (8, or 16 for anything holding a
    // LinearTransform), and an exported one has its GUID immediately before it - so
    // StormTrooperAI_Skirmish's first SyncedBool sits at +0x78 behind a GUID at +0x68.
    for (uint32_t ordinal = 1; ordinal < 200000; ++ordinal)
    {
        const struct { uintptr_t at; bool exported; } candidates[] = {
            { (end + 7) & ~uintptr_t(7), false },
            { (end + 16 + 7) & ~uintptr_t(7), true },
            { (end + 16 + 15) & ~uintptr_t(15), true },
            { (end + 15) & ~uintptr_t(15), false },
        };
        uintptr_t at = 0;
        for (const auto& candidate : candidates)
        {
            if ((size = TraceInstanceSize(candidate.at)) != 0 &&
                TraceHasGuid(candidate.at) == candidate.exported)
            {
                at = candidate.at;
                break;
            }
        }
        if (at == 0)
            break;
        slot.second[at] = ordinal;
        end = at + size;
    }
    LogLine("trace: '%s' -> %zu instances", SafeCString(namePtr, 160).c_str(), slot.second.size());
    return slot.second;
}

// Exported instances: their real GUID (short form is Data1). Everything else: the
// partition ordinal, "[2e]", or Frosty's full placeholder GUID with 'trace guid 2'.
static std::string TraceInstanceTag(uintptr_t data, uintptr_t owner)
{
    if (g_traceGuids == 0 || data < 0x10)
        return std::string();
    char text[48] = {};
    GUID guid{};
    if (TraceHasGuid(data) && SafeRead(data - 0x10, &guid))
    {
        if (g_traceGuids == 1)
            sprintf_s(text, "#%08x", guid.Data1);
        else
            sprintf_s(text, "#%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                      guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
                      guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
                      guid.Data4[6], guid.Data4[7]);
        return text;
    }
    const auto& ordinals = TraceOrdinals(owner);
    auto it = ordinals.find(data);
    if (it == ordinals.end())
        return std::string();
    if (g_traceGuids == 1)
        sprintf_s(text, "[%x]", it->second);
    else
        sprintf_s(text, "#00000000-0000-0000-0000-%012x", it->second);
    return text;
}

// The bus's EntityBusData is the blueprint (prefab, SubWorldData, LevelData) that owns
// the connection; as an Asset its Name is the EBX path. Cached against the name
// pointer, so a blueprint freed and replaced at the same address is re-read.
static const std::string& TraceOwnerName(uintptr_t owner)
{
    static const std::string unknown;
    uintptr_t namePtr = 0;
    if (owner == 0 || !SafeRead(owner + 0x18, &namePtr) || namePtr == 0)
        return unknown;
    auto found = g_traceOwnerNames.find(owner);
    if (found != g_traceOwnerNames.end() && found->second.first == namePtr)
        return found->second.second;
    auto& slot = g_traceOwnerNames[owner];
    slot.first = namePtr;
    slot.second = SafeCString(namePtr, 256);
    return slot.second;
}

static bool ContainsNoCase(const std::string& haystack, const std::string& needle)
{
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return tolower(static_cast<unsigned char>(a)) ==
                                                      tolower(static_cast<unsigned char>(b)); });
    return it != haystack.end();
}

// Event and field ids are DJB2-xor hashes of their names (h = h*33 ^ c, seed 5381). Names
// come from the frostbite-research catalogue plus gamelink/event_hashes_extra.csv (grown
// by harvest_hashes.py / harvest_combo.py), loaded when a feed is first turned on so the
// feeds print names and filters can match them. An unknown id stays as 8 hex digits.
static std::unordered_map<uint32_t, std::string> g_hashNames;
static bool g_hashNamesLoaded = false;

static void LoadHashNames()
{
    if (g_hashNamesLoaded)
        return;
    g_hashNamesLoaded = true;

    // Both tables live in the gamelink folder, one up from bin\ where the DLL is built.
    char modulePath[MAX_PATH] = {};
    std::string dir;
    if (GetModuleFileNameA(g_selfModule, modulePath, MAX_PATH) != 0)
    {
        dir = modulePath;
        dir = dir.substr(0, dir.find_last_of("\\/"));          // ...\gamelink\bin
        dir = dir.substr(0, dir.find_last_of("\\/"));          // ...\gamelink
    }
    const std::string paths[] = {
        dir.empty() ? std::string() : dir + "\\field_hashes.csv",        // Frosty's names (EBX dumps)
        dir.empty() ? std::string() : dir + "\\event_hashes_extra.csv",  // harvest_*.py additions
    };
    for (const std::string& path : paths)
    {
        if (path.empty())
            continue;
        FILE* file = nullptr;
        if (fopen_s(&file, path.c_str(), "r") != 0 || file == nullptr)
            continue;
        char row[512];
        size_t added = 0;
        while (fgets(row, sizeof(row), file) != nullptr)
        {
            // hash_hex,hash_i32,name
            if (row[0] != '0' || row[1] != 'x')
                continue;
            const uint32_t hash = static_cast<uint32_t>(strtoul(row + 2, nullptr, 16));
            const char* name = strchr(row, ',');
            name = name != nullptr ? strchr(name + 1, ',') : nullptr;
            if (name == nullptr)
                continue;
            std::string text(name + 1);
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
                text.pop_back();
            if (!text.empty() && g_hashNames.emplace(hash, text).second)
                ++added;
        }
        fclose(file);
        LogLine("names: %zu from %s", added, path.c_str());
    }
}

static std::string HashText(uint32_t hash)
{
    auto it = g_hashNames.find(hash);
    if (it != g_hashNames.end())
        return it->second;
    char text[12];
    sprintf_s(text, "%08x", hash);
    return text;
}

// Filter for both feeds: comma-separated terms, case-insensitive. A line passes when it
// contains any plain term (or there are none) and none of the "!" terms:
//   "MosEspa"  "EventGate,CompareBool"  "GameModes,!Transform"  "!LinearTransform,!Phantom"
static bool FilterPasses(const std::string& text, const std::string& filter)
{
    if (filter.empty())
        return true;
    bool anyInclude = false, included = false;
    size_t start = 0;
    while (start <= filter.size())
    {
        size_t end = filter.find(',', start);
        if (end == std::string::npos)
            end = filter.size();
        std::string term = filter.substr(start, end - start);
        while (!term.empty() && term.front() == ' ') term.erase(term.begin());
        while (!term.empty() && term.back() == ' ') term.pop_back();
        if (!term.empty())
        {
            if (term[0] == '!')
            {
                if (term.size() > 1 && ContainsNoCase(text, term.substr(1)))
                    return false;
            }
            else
            {
                anyInclude = true;
                included = included || ContainsNoCase(text, term);
            }
        }
        start = end + 1;
    }
    return !anyInclude || included;
}

// One line per fired connection:  S Delay.0b87df4b -> EventGate.00597302 [net]
// S/C is the realm of the bus that fired it; 'net' marks a networked target type.
// One event line - shared by the 'trace' reader and the overlay.
static std::string FormatTraceEntry(const TraceEntry& copy)
{
    std::string text;
    text.reserve(160);
    text += copy.server ? "S " : "C ";
    text += TraceTypeName(copy.source) + TraceInstanceTag(copy.source, copy.owner);
    text += "." + HashText(static_cast<uint32_t>(copy.sourceEvent)) + " -> ";
    text += TraceTypeName(copy.target) + TraceInstanceTag(copy.target, copy.owner);
    text += "." + HashText(static_cast<uint32_t>(copy.targetEvent));
    if (copy.targetType == 4 || copy.targetType == 5)
        text += " net";
    if (g_traceWhere)
    {
        const std::string& owner = TraceOwnerName(copy.owner);
        if (!owner.empty())
            text += "  @" + owner;
    }
    if (g_traceAddresses)
    {
        char line[128];
        sprintf_s(line, "  [0x%llX -> 0x%llX, bus data 0x%llX]",
                  static_cast<unsigned long long>(copy.source),
                  static_cast<unsigned long long>(copy.target),
                  static_cast<unsigned long long>(copy.owner));
        text += line;
    }
    return text;
}

static std::string ReadEventTrace(int maxLines)
{
    if (g_trace == nullptr)
        return "trace has never been on";

    const LONG64 next = g_traceNext;
    if (next - g_traceRead > static_cast<LONG64>(kTraceCapacity))
    {
        g_traceDropped += next - g_traceRead - kTraceCapacity;
        g_traceRead = next - kTraceCapacity;
    }

    std::string out;
    char line[512];
    int shown = 0;
    LONG64 scanned = 0;
    while (g_traceRead < next && shown < maxLines)
    {
        const TraceEntry& slot = g_trace[g_traceRead & (kTraceCapacity - 1)];
        const LONG64 expected = g_traceRead + 1;
        const LONG64 before = slot.seq;
        if (before < expected)
        {
            if (next - g_traceRead < 4096)
                break;                          // still being written; pick it up next read
            ++g_traceRead;                      // a write that faulted never completes
            ++g_traceDropped;
            continue;
        }
        TraceEntry copy;
        memcpy(&copy, const_cast<const TraceEntry*>(&slot), sizeof(copy));
        _ReadBarrier();
        ++g_traceRead;
        ++scanned;
        if (before != expected || slot.seq != expected)
        {
            ++g_traceDropped;                   // lapped by the writers while reading
            continue;
        }

        const std::string text = FormatTraceEntry(copy);
        if (!FilterPasses(text, g_traceFilter))
            continue;
        out += text;
        out += '\n';
        ++shown;
    }

    sprintf_s(line, "%d shown, %lld scanned, %lld pending, %lld dropped, %s%s%s\n",
              shown, static_cast<long long>(scanned), static_cast<long long>(next - g_traceRead),
              static_cast<long long>(g_traceDropped), g_traceInstalled ? "on" : "off",
              g_traceFilter.empty() ? "" : ", filter: ", g_traceFilter.c_str());
    return line + out;
}

// ---------------------------------------------------------------------------
// prop - live property feed: "S CompareBool[2a].Bool = true  @<blueprint>".
//
// PropertyModificationQueue::internalCallListeners(queue, CacheData*, isFirstTimeWritten,
// flag) at game+0x1D3780 hands every property value write - immediate ones, and the
// batched write entries of the queue flush at game+0x1179000 - to the CacheData's
// listeners through listener->propertyChanged (vtable +0x28). Only the flush's
// re-notify and parent-relay entries call listeners without it. It has no thunk, so its
// six call sites are retargeted at per-site entries in a stub within rel32 reach; one
// site's rel32 straddles a 16-byte line, so its entry sits where the first byte of the
// displacement does not change and the write stays inside one aligned block.
//
// CacheData (BFII): +0x30 listener lists, +0x38 value*, +0x40 value TypeInfo, +0x48
// flags (bit 0 = server). A list is { rec-or-array, count u32, isArray u32 }, a second
// one follows at +0x18 (used when 'flag' is set); a ListenerRec is { listener*, field
// hash }. Which entity and field OWN the CacheData - the writing side - is not in it, so
// lines show the value arriving at each listener, not where it came from.
// ---------------------------------------------------------------------------

constexpr uintptr_t kCallListenersRva = 0x1D3780;
constexpr uintptr_t kCallListenersSites[] = { 0x54421FF, 0x543977A, 0x54417FA, 0x11791F5, 0x1179347, 0x11793DE };
constexpr size_t kCallListenersSiteCount = sizeof(kCallListenersSites) / sizeof(kCallListenersSites[0]);
constexpr uintptr_t kFullEntityBusVtableRva = 0x305F1E0;
// internalRegisterCallListeners(queue, CacheData*, isFirst, teleport) = game+0x543E610, only
// reached through its jmp stub game+0x1D4C30 (from registerWrite's batched branch and
// registerCallListeners). It queues listener calls for a value already written; the
// flush (game+0x1179000 case 0) later calls them without going through 0x1D3780.
constexpr uintptr_t kRegisterCallListenersThunkRva = 0x1D4C30;
constexpr uintptr_t kRegisterCallListenersRva = 0x543E610;
constexpr size_t kPropCapacity = size_t(1) << 17;      // ~20 s of unfiltered writes at ~6k/s
constexpr uint32_t kPropListeners = 4;

struct PropEntry
{
    volatile LONG64 seq;
    uintptr_t cache;
    uintptr_t valueType;
    unsigned char value[16];
    uintptr_t listener[kPropListeners];
    uint32_t field[kPropListeners];
    uint32_t listenerCount;
    uint32_t server;
    uint32_t translated;        // value[] holds a LinearTransform's translation row
    uint32_t pad;
};

typedef void(__fastcall* CallListenersFn)(void* queue, void* cache, uintptr_t firstTime, uintptr_t flag);

static PropEntry* g_prop = nullptr;
static volatile LONG64 g_propNext = 0;
static LONG64 g_propRead = 0;
static LONG64 g_propDropped = 0;
static bool g_propInstalled = false;
static unsigned char* g_propStub = nullptr;
static uintptr_t g_propEntries[kCallListenersSiteCount + 1] = {};   // + the queued-listeners thunk
static CallListenersFn g_propOriginal = nullptr;
static CallListenersFn g_propQueuedOriginal = nullptr;
static std::string g_propFilter;
static uintptr_t g_propLinearTransform = 0;     // its TypeInfo, so the detour can compare one pointer

static void PropCollectList(PropEntry& entry, uintptr_t list)
{
    const uintptr_t first = *reinterpret_cast<const uintptr_t*>(list);
    const uint32_t isArray = *reinterpret_cast<const uint32_t*>(list + 0x10);
    const uint32_t count = isArray == 0 ? (first != 0 ? 1u : 0u) : *reinterpret_cast<const uint32_t*>(list + 0x08);
    const uintptr_t recs = isArray == 0 ? list : first;
    for (uint32_t i = 0; i < count; ++i)
    {
        if (entry.listenerCount < kPropListeners)
        {
            entry.listener[entry.listenerCount] = *reinterpret_cast<const uintptr_t*>(recs + i * 16);
            entry.field[entry.listenerCount] = *reinterpret_cast<const uint32_t*>(recs + i * 16 + 8);
        }
        ++entry.listenerCount;
    }
}

static void PropReadValue(PropEntry& entry, uintptr_t value)
{
    __try
    {
        memcpy(entry.value, reinterpret_cast<const void*>(value), 16);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        __try { memcpy(entry.value, reinterpret_cast<const void*>(value), 8); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

static void PropRecord(uintptr_t cache, uintptr_t flag)
{
    if (g_feedsPaused)
        return;
    __try
    {
        PropEntry local;
        memset(&local, 0, sizeof(local));
        const uintptr_t lists = *reinterpret_cast<const uintptr_t*>(cache + 0x30);
        if (lists == 0)
            return;
        PropCollectList(local, lists);
        if ((flag & 0xFF) != 0)
            PropCollectList(local, lists + 0x18);
        if (local.listenerCount == 0)
            return;                                     // nobody listening: nothing propagates
        local.cache = cache;
        local.valueType = *reinterpret_cast<const uintptr_t*>(cache + 0x40);
        local.server = *reinterpret_cast<const uint32_t*>(cache + 0x48) & 1;
        const uintptr_t value = *reinterpret_cast<const uintptr_t*>(cache + 0x38);
        local.translated = local.valueType != 0 && local.valueType == g_propLinearTransform;
        if (value != 0)                                 // a LinearTransform keeps its translation row
            PropReadValue(local, local.translated ? value + 0x30 : value);

        const LONG64 index = InterlockedIncrement64(&g_propNext) - 1;
        PropEntry& slot = g_prop[index & (kPropCapacity - 1)];
        slot.seq = 0;
        _WriteBarrier();
        memcpy(reinterpret_cast<char*>(&slot) + sizeof(LONG64), reinterpret_cast<const char*>(&local) + sizeof(LONG64),
               sizeof(PropEntry) - sizeof(LONG64));
        MemoryBarrier();
        slot.seq = index + 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

static void __fastcall PropDetour(void* queue, void* cache, uintptr_t firstTime, uintptr_t flag)
{
    PropRecord(reinterpret_cast<uintptr_t>(cache), flag);
    g_propOriginal(queue, cache, firstTime, flag);
    if (g_fireRequest.state == 1)                       // CacheData +0x48 bit 0 = server
    {
        uint32_t flags = 0;
        if (SafeRead(reinterpret_cast<uintptr_t>(cache) + 0x48, &flags))
            ServeFireRequestRealm(flags & 1);
    }
}

// The batched path: the value is already in the CacheData, its listener calls are being
// queued for the flush. Recorded now, as the same line the flush's delivery would make.
static void __fastcall PropQueuedDetour(void* queue, void* cache, uintptr_t firstTime, uintptr_t teleport)
{
    PropRecord(reinterpret_cast<uintptr_t>(cache), 0);
    g_propQueuedOriginal(queue, cache, firstTime, teleport);
}

// Retarget the rel32 of the call/jmp at 'site' at 'destination', writing only one aligned
// 8- or 16-byte block so a thread executing the instruction never sees a torn
// displacement. Fails, writing nothing, when the bytes that change don't fit one block.
static bool PatchRel32Atomic(uintptr_t site, uintptr_t destination, bool dryRun)
{
    unsigned char old[5] = {};
    if (!SafeReadBytes(site, old, sizeof(old)) || (old[0] != 0xE8 && old[0] != 0xE9))
        return false;
    const intptr_t delta = static_cast<intptr_t>(destination) - static_cast<intptr_t>(site + 5);
    if (delta < INT32_MIN || delta > INT32_MAX)
        return false;
    unsigned char now[5];
    now[0] = old[0];
    const int32_t rel = static_cast<int32_t>(delta);
    memcpy(now + 1, &rel, sizeof(rel));

    uintptr_t lo = 0, hi = 0;
    bool changed = false;
    for (int i = 1; i < 5; ++i)
    {
        if (old[i] == now[i])
            continue;
        if (!changed)
            lo = site + i;
        hi = site + i;
        changed = true;
    }
    if (!changed)
        return true;

    size_t width = 0;
    uintptr_t block = 0;
    if ((lo & ~uintptr_t(7)) == (hi & ~uintptr_t(7)))
    {
        width = 8;
        block = lo & ~uintptr_t(7);
    }
    else if ((lo & ~uintptr_t(15)) == (hi & ~uintptr_t(15)))
    {
        width = 16;
        block = lo & ~uintptr_t(15);
    }
    else
        return false;
    if (dryRun)
        return true;

    DWORD previous = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(block), width, PAGE_EXECUTE_READWRITE, &previous))
        return false;

    unsigned char bytes[16];
    auto apply = [&](unsigned char* image)
    {
        for (int i = 1; i < 5; ++i)
        {
            const uintptr_t at = site + i;
            if (at >= block && at < block + width)
                image[at - block] = now[i];
        }
    };
    if (width == 8)
    {
        memcpy(bytes, reinterpret_cast<const void*>(block), 8);
        apply(bytes);
        LONG64 value = 0;
        memcpy(&value, bytes, 8);
        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(block), value);
    }
    else
    {
        __declspec(align(16)) LONG64 expected[2];
        memcpy(expected, reinterpret_cast<const void*>(block), 16);
        for (;;)
        {
            memcpy(bytes, expected, 16);
            apply(bytes);
            LONG64 wanted[2];
            memcpy(wanted, bytes, 16);
            if (InterlockedCompareExchange128(reinterpret_cast<volatile LONG64*>(block), wanted[1], wanted[0], expected))
                break;
        }
    }

    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(block), width, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(block), width);
    return true;
}

static uintptr_t Rel32Destination(uintptr_t site)
{
    unsigned char bytes[5] = {};
    if (!SafeReadBytes(site, bytes, sizeof(bytes)) || (bytes[0] != 0xE8 && bytes[0] != 0xE9))
        return 0;
    int32_t rel = 0;
    memcpy(&rel, bytes + 1, sizeof(rel));
    return site + 5 + static_cast<intptr_t>(rel);
}

static std::string InstallPropTraceUnlocked(bool enable)
{
    const uintptr_t original = g_base + kCallListenersRva;
    char buffer[256];

    const uintptr_t queuedThunk = g_base + kRegisterCallListenersThunkRva;
    const uintptr_t queuedOriginal = g_base + kRegisterCallListenersRva;

    if (!enable)
    {
        if (!g_propInstalled)
            return "prop is not installed";
        int restored = 0;
        for (size_t i = 0; i < kCallListenersSiteCount; ++i)
            restored += PatchRel32Atomic(g_base + kCallListenersSites[i], original, false) ? 1 : 0;
        restored += PatchRel32Atomic(queuedThunk, queuedOriginal, false) ? 1 : 0;
        g_propInstalled = false;
        LogLine("prop: removed after %lld writes (%d of %zu sites restored)",
                static_cast<long long>(g_propNext), restored, kCallListenersSiteCount + 1);
        sprintf_s(buffer, "prop off - %lld writes seen, %d of %zu sites restored",
                  static_cast<long long>(g_propNext), restored, kCallListenersSiteCount + 1);
        return buffer;
    }
    if (g_propInstalled)
        return "prop already on";
    if (!FeedLevelLoaded())
        return FeedDefer(2, "prop");           // never patch before a level is up (see FeedLevelLoaded)

    for (size_t i = 0; i < kCallListenersSiteCount; ++i)
    {
        const uintptr_t site = g_base + kCallListenersSites[i];
        const uintptr_t reaches = Rel32Destination(site);
        if (reaches == 0)
            return FeedDefer(2, "prop");           // code not unpacked yet (frontend): arm when it is
        if (reaches != original)
        {
            sprintf_s(buffer, "refusing: call site game+0x%llX does not reach game+0x%llX",
                      static_cast<unsigned long long>(kCallListenersSites[i]),
                      static_cast<unsigned long long>(kCallListenersRva));
            return buffer;
        }
    }
    {
        const uintptr_t reaches = Rel32Destination(queuedThunk);
        if (reaches == 0)
            return FeedDefer(2, "prop");
        if (reaches != queuedOriginal)
        {
            sprintf_s(buffer, "refusing: the thunk at game+0x%llX does not reach game+0x%llX",
                      static_cast<unsigned long long>(kRegisterCallListenersThunkRva),
                      static_cast<unsigned long long>(kRegisterCallListenersRva));
            return buffer;
        }
    }

    if (g_prop == nullptr)
    {
        g_prop = static_cast<PropEntry*>(VirtualAlloc(nullptr, sizeof(PropEntry) * kPropCapacity,
                                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (g_prop == nullptr)
            return "could not allocate the prop buffer";
    }

    // One entry per site, never freed (a thread may still be inside the detour after
    // 'prop off'). Entries are placed where each site's patch stays atomic.
    if (g_propStub == nullptr)
    {
        g_propStub = NearCode(g_base + kCallListenersSites[0], 0x1000);
        if (g_propStub == nullptr)
            return "could not allocate a stub within rel32 reach";
        void* detour = reinterpret_cast<void*>(&PropDetour);
        size_t cursor = 0x40;
        for (size_t i = 0; i < kCallListenersSiteCount; ++i)
        {
            const uintptr_t site = g_base + kCallListenersSites[i];
            size_t at = cursor;
            while (at < 0xF00 && !PatchRel32Atomic(site, reinterpret_cast<uintptr_t>(g_propStub) + at, true))
                ++at;
            if (at >= 0xF00)
            {
                sprintf_s(buffer, "no atomic entry placement for call site game+0x%llX",
                          static_cast<unsigned long long>(kCallListenersSites[i]));
                return buffer;
            }
            const unsigned char jump[6] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };   // jmp [rip+0]
            memcpy(g_propStub + at, jump, sizeof(jump));
            memcpy(g_propStub + at + sizeof(jump), &detour, sizeof(detour));
            g_propEntries[i] = reinterpret_cast<uintptr_t>(g_propStub) + at;
            cursor = at + 16;
        }
        // The queued-listeners thunk gets its own entry, into its own detour.
        void* queuedDetour = reinterpret_cast<void*>(&PropQueuedDetour);
        size_t at = cursor;
        while (at < 0xF00 && !PatchRel32Atomic(queuedThunk, reinterpret_cast<uintptr_t>(g_propStub) + at, true))
            ++at;
        if (at >= 0xF00)
            return "no atomic entry placement for the queued-listeners thunk";
        const unsigned char jump[6] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
        memcpy(g_propStub + at, jump, sizeof(jump));
        memcpy(g_propStub + at + sizeof(jump), &queuedDetour, sizeof(queuedDetour));
        g_propEntries[kCallListenersSiteCount] = reinterpret_cast<uintptr_t>(g_propStub) + at;
        FlushInstructionCache(GetCurrentProcess(), g_propStub, 0x1000);
    }

    if (g_propLinearTransform == 0 && WalkRegistry())
    {
        LockRegistry();
        if (const TypeRec* type = FindType(std::string("LinearTransform")))
            g_propLinearTransform = type->classInfo;
        UnlockRegistry();
    }

    LoadHashNames();
    g_propOriginal = reinterpret_cast<CallListenersFn>(original);
    g_propQueuedOriginal = reinterpret_cast<CallListenersFn>(queuedOriginal);
    g_propRead = g_propNext;
    g_propDropped = 0;

    int patched = 0;
    for (size_t i = 0; i < kCallListenersSiteCount; ++i)
        patched += PatchRel32Atomic(g_base + kCallListenersSites[i], g_propEntries[i], false) ? 1 : 0;
    patched += PatchRel32Atomic(queuedThunk, g_propEntries[kCallListenersSiteCount], false) ? 1 : 0;
    g_propInstalled = patched > 0;
    LogLine("prop: on, %d of %zu sites -> stub 0x%p", patched, kCallListenersSiteCount + 1, g_propStub);
    sprintf_s(buffer, "prop on - %d of %zu sites hooked; 'prop' reads, 'prop off' removes",
              patched, kCallListenersSiteCount + 1);
    return buffer;
}

// A listener is an entity when its bus slot holds a FullEntityBus and its data slot a
// known EBX instance: +0x20/+0x28 for native entities, +0x30/+0x38 for spatial ones.
// Decided by memory alone - no virtual is called on something that may be a CacheData
// relay rather than an entity.
static bool PropListenerEntity(uintptr_t listener, uintptr_t* data, uintptr_t* owner)
{
    const uintptr_t busVtable = g_base + kFullEntityBusVtableRva;
    for (const size_t busAt : { size_t(0x20), size_t(0x30) })
    {
        uintptr_t bus = 0, vtable = 0, candidate = 0;
        if (!SafeRead(listener + busAt, &bus) || bus == 0 || !SafeRead(bus, &vtable) || vtable != busVtable)
            continue;
        if (!SafeRead(listener + busAt + 8, &candidate) || TraceInstanceSize(candidate) == 0)
            continue;
        uintptr_t runtime = 0, busData = 0;
        if (SafeRead(bus + 0x68, &runtime) && runtime != 0)
            SafeRead(runtime + 0xB8, &busData);
        *data = candidate;
        *owner = busData;
        return true;
    }
    return false;
}

// The writing side, recovered from the data: a value reaching listener field F of
// instance T arrived over a PropertyConnection in T's own blueprint whose Target is T and
// TargetFieldId is F. DataBusData.PropertyConnections is at +0x28 (32-byte entries:
// Source +0, Target +8, SourceFieldId +0x10, TargetFieldId +0x14; count at array-4).
struct PropSource { uintptr_t source; uint32_t field; };
static std::map<uintptr_t, std::pair<uintptr_t, std::map<std::pair<uintptr_t, uint32_t>, std::vector<PropSource>>>> g_propSources;

static const std::vector<PropSource>* PropSourcesFor(uintptr_t owner, uintptr_t target, uint32_t field)
{
    uintptr_t namePtr = 0;
    if (owner == 0 || !SafeRead(owner + 0x18, &namePtr))
        return nullptr;
    auto& slot = g_propSources[owner];
    if (slot.first != namePtr)
    {
        slot.first = namePtr;
        slot.second.clear();
        uintptr_t array = 0;
        uint32_t count = 0;
        if (SafeRead(owner + 0x28, &array) && array != 0 && SafeRead(array - 4, &count) && count < 1000000)
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                unsigned char rec[0x18];
                if (!SafeReadBytes(array + i * 0x20, rec, sizeof(rec)))
                    break;
                PropSource src;
                uintptr_t to = 0;
                uint32_t toField = 0;
                memcpy(&src.source, rec + 0x00, 8);
                memcpy(&to, rec + 0x08, 8);
                memcpy(&src.field, rec + 0x10, 4);
                memcpy(&toField, rec + 0x14, 4);
                slot.second[{ to, toField }].push_back(src);
            }
        }
    }
    auto it = slot.second.find({ target, field });
    return it == slot.second.end() ? nullptr : &it->second;
}

static std::string TypeInfoName(uintptr_t typeInfo)
{
    uintptr_t typeInfoData = 0, namePtr = 0;
    if (typeInfo == 0 || !SafeRead(typeInfo, &typeInfoData) || typeInfoData == 0 || !SafeRead(typeInfoData, &namePtr))
        return std::string();
    return SafeCString(namePtr, 96);
}

static std::string TypedValueText(const std::string& type, const unsigned char* v, bool translated);

static std::string PropValueText(const PropEntry& entry)
{
    return TypedValueText(TypeInfoName(entry.valueType), entry.value, entry.translated != 0);
}

// A value of a named engine type as text (first 16 bytes; a LinearTransform's row as given).
static std::string TypedValueText(const std::string& type, const unsigned char* v, bool translated)
{
    char text[160] = {};
    float f[4];
    memcpy(f, v, sizeof(f));
    if (type == "Boolean")
        return v[0] ? "true" : "false";
    if (type == "Float32")
        sprintf_s(text, "%g", f[0]);
    else if (type == "Float64")
    {
        double d = 0;
        memcpy(&d, v, sizeof(d));
        sprintf_s(text, "%g", d);
    }
    else if (type == "Int8" || type == "Int16" || type == "Int32" || type == "Int64")
    {
        int64_t i = 0;
        memcpy(&i, v, type == "Int8" ? 1 : type == "Int16" ? 2 : type == "Int32" ? 4 : 8);
        if (type == "Int8") i = static_cast<int8_t>(i);
        else if (type == "Int16") i = static_cast<int16_t>(i);
        else if (type == "Int32") i = static_cast<int32_t>(i);
        sprintf_s(text, "%lld", static_cast<long long>(i));
    }
    else if (type == "Uint8" || type == "Uint16" || type == "Uint32" || type == "Uint64")
    {
        uint64_t u = 0;
        memcpy(&u, v, type == "Uint8" ? 1 : type == "Uint16" ? 2 : type == "Uint32" ? 4 : 8);
        sprintf_s(text, "%llu", static_cast<unsigned long long>(u));
    }
    else if (type == "Vec2")
        sprintf_s(text, "(%g, %g)", f[0], f[1]);
    else if (type == "Vec3")
        sprintf_s(text, "(%g, %g, %g)", f[0], f[1], f[2]);
    else if (type == "Vec4" || type == "Quat")
        sprintf_s(text, "(%g, %g, %g, %g)", f[0], f[1], f[2], f[3]);
    else if (type == "LinearTransform")
        sprintf_s(text, translated ? "at (%g, %g, %g)" : "right (%g, %g, %g)", f[0], f[1], f[2]);
    else if (type == "CString")
    {
        uintptr_t p = 0;
        memcpy(&p, v, sizeof(p));
        return "\"" + SafeCString(p, 96) + "\"";
    }
    else
    {
        uint64_t raw = 0;
        memcpy(&raw, v, sizeof(raw));
        sprintf_s(text, "0x%llX (%s)", static_cast<unsigned long long>(raw), type.empty() ? "?" : type.c_str());
    }
    return text;
}

// One line per listener a write reached:  S CompareBool[2a].Bool = true  @<blueprint>
// Listeners that are not entities (CacheData relays between buses) show as "relay".
// One property line for listener i of a recorded write - shared by 'prop' and the overlay.
static std::string FormatPropLine(const PropEntry& copy, uint32_t i, const std::string& value)
{
    uintptr_t data = 0, owner = 0;
    std::string text;
    if (PropListenerEntity(copy.listener[i], &data, &owner))
    {
        std::string from = "?";
        if (const std::vector<PropSource>* sources = PropSourcesFor(owner, data, copy.field[i]))
        {
            const PropSource& src = sources->front();
            from = TraceTypeName(src.source) + TraceInstanceTag(src.source, owner) + "." + HashText(src.field);
            if (sources->size() > 1)
                from += " (+" + std::to_string(sources->size() - 1) + ")";
        }
        text = std::string(copy.server ? "S " : "C ") + from + " -> " +
               TraceTypeName(data) + TraceInstanceTag(data, owner) + "." + HashText(copy.field[i]) + " = " + value;
        if (g_traceWhere && !TraceOwnerName(owner).empty())
            text += "  @" + TraceOwnerName(owner);
    }
    else
    {
        text = std::string(copy.server ? "S " : "C ") + "relay" +
               (copy.field[i] != 0 ? "." + HashText(copy.field[i]) : std::string()) + " = " + value;
    }
    if (g_traceAddresses)
    {
        char line[96];
        sprintf_s(line, "  [cache 0x%llX, listener 0x%llX]",
                  static_cast<unsigned long long>(copy.cache), static_cast<unsigned long long>(copy.listener[i]));
        text += line;
    }
    return text;
}

static std::string ReadPropTrace(int maxLines)
{
    if (g_prop == nullptr)
        return "prop has never been on";

    // Turned on before the registry was complete (the harness's "feeds" arms it at the
    // frontend): look LinearTransform up again so later transforms keep their position.
    if (g_propLinearTransform == 0 && WalkRegistry())
    {
        LockRegistry();
        if (const TypeRec* type = FindType(std::string("LinearTransform")))
            g_propLinearTransform = type->classInfo;
        UnlockRegistry();
    }

    const LONG64 next = g_propNext;
    if (next - g_propRead > static_cast<LONG64>(kPropCapacity))
    {
        g_propDropped += next - g_propRead - kPropCapacity;
        g_propRead = next - kPropCapacity;
    }

    std::string out;
    char line[512];
    int shown = 0;
    LONG64 scanned = 0;
    while (g_propRead < next && shown < maxLines)
    {
        const PropEntry& slot = g_prop[g_propRead & (kPropCapacity - 1)];
        const LONG64 expected = g_propRead + 1;
        const LONG64 before = slot.seq;
        if (before < expected)
        {
            if (next - g_propRead < 4096)
                break;
            ++g_propRead;
            ++g_propDropped;
            continue;
        }
        PropEntry copy;
        memcpy(&copy, const_cast<const PropEntry*>(&slot), sizeof(copy));
        _ReadBarrier();
        ++g_propRead;
        ++scanned;
        if (before != expected || slot.seq != expected)
        {
            ++g_propDropped;
            continue;
        }

        const std::string value = PropValueText(copy);
        const uint32_t listeners = copy.listenerCount < kPropListeners ? copy.listenerCount : kPropListeners;
        for (uint32_t i = 0; i < listeners; ++i)
        {
            const std::string text = FormatPropLine(copy, i, value);
            if (!FilterPasses(text, g_propFilter))
                continue;
            out += text;
            out += '\n';
            ++shown;
        }
        if (copy.listenerCount > kPropListeners && shown < maxLines)
        {
            sprintf_s(line, "  (+%u more listeners)\n", copy.listenerCount - kPropListeners);
            if (g_propFilter.empty())
                out += line;
        }
    }

    sprintf_s(line, "%d shown, %lld scanned, %lld pending, %lld dropped, %s%s%s\n",
              shown, static_cast<long long>(scanned), static_cast<long long>(next - g_propRead),
              static_cast<long long>(g_propDropped), g_propInstalled ? "on" : "off",
              g_propFilter.empty() ? "" : ", filter: ", g_propFilter.c_str());
    return line + out;
}


// ---- in-game overlay ------------------------------------------------------------
// 'overlay on [filter]' draws the newest feed lines over the game with Dear ImGui, from
// the Present hook (a vtable patch on dxgi's swap chain, not game code, so it is safe at
// any time). Display only: no input is taken, so it cannot get in the way of play.
// Lines are re-collected 4 times a second, newest last, without moving the 'trace' /
// 'prop' read cursors - the overlay and gl.py follow can run together. The name caches
// are shared with the command thread, so both sides format under g_formatLock.

static SRWLOCK g_formatLock = SRWLOCK_INIT;
static volatile bool g_overlayOn = false;
static bool g_overlayFailed = false;
static bool g_overlayReady = false;
static int g_overlayFeeds = 3;                  // bit 1 = events, bit 2 = properties
static std::string g_overlayFilter;
static std::vector<std::string> g_overlayEvents, g_overlayProps;
static DWORD g_overlayRefreshed = 0;
static ID3D11Device* g_overlayDevice = nullptr;
static ID3D11DeviceContext* g_overlayContext = nullptr;

static void OverlayCollect()
{
    g_overlayEvents.clear();
    g_overlayProps.clear();
    if ((g_overlayFeeds & 1) != 0 && g_trace != nullptr)
    {
        const LONG64 next = g_traceNext;
        for (LONG64 index = next - 1; index >= 0 && index >= next - 6000 && g_overlayEvents.size() < 22; --index)
        {
            const TraceEntry& slot = g_trace[index & (kTraceCapacity - 1)];
            if (slot.seq != index + 1)
                continue;
            TraceEntry copy;
            memcpy(&copy, const_cast<const TraceEntry*>(&slot), sizeof(copy));
            const std::string text = FormatTraceEntry(copy);
            if (FilterPasses(text, g_overlayFilter))
                g_overlayEvents.push_back(text);
        }
        std::reverse(g_overlayEvents.begin(), g_overlayEvents.end());
    }
    if ((g_overlayFeeds & 2) != 0 && g_prop != nullptr)
    {
        const LONG64 next = g_propNext;
        for (LONG64 index = next - 1; index >= 0 && index >= next - 6000 && g_overlayProps.size() < 16; --index)
        {
            const PropEntry& slot = g_prop[index & (kPropCapacity - 1)];
            if (slot.seq != index + 1)
                continue;
            PropEntry copy;
            memcpy(&copy, const_cast<const PropEntry*>(&slot), sizeof(copy));
            const std::string value = PropValueText(copy);
            const uint32_t listeners = copy.listenerCount < kPropListeners ? copy.listenerCount : kPropListeners;
            for (uint32_t i = 0; i < listeners && g_overlayProps.size() < 16; ++i)
            {
                const std::string text = FormatPropLine(copy, i, value);
                if (FilterPasses(text, g_overlayFilter))
                    g_overlayProps.push_back(text);
            }
        }
        std::reverse(g_overlayProps.begin(), g_overlayProps.end());
    }
}

static bool OverlayInit(IDXGISwapChain* swapChain)
{
    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_overlayDevice))) ||
        g_overlayDevice == nullptr)
    {
        LogLine("overlay: the swap chain has no D3D11 device (DX12 mode?) - overlay off");
        return false;
    }
    g_overlayDevice->GetImmediateContext(&g_overlayContext);
    DXGI_SWAP_CHAIN_DESC desc = {};
    swapChain->GetDesc(&desc);
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;                    // nothing written next to the game
    io.LogFilename = nullptr;
    ImGui::StyleColorsDark();
    if (!ImGui_ImplWin32_Init(desc.OutputWindow) || !ImGui_ImplDX11_Init(g_overlayDevice, g_overlayContext))
    {
        LogLine("overlay: ImGui backend init failed - overlay off");
        return false;
    }
    LogLine("overlay: ready on window 0x%p", desc.OutputWindow);
    return true;
}

// A typeless back buffer needs a concrete format for its render-target view.
static DXGI_FORMAT OverlayViewFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return format;
    }
}

static void MenuFrameInputs();         // F1 menu: raw input -> ImGui (defined below)
static void BadgeDraw();               // corner "gamelink - F1" badge
static void MenuEnsureWindowHook(IDXGISwapChain* swapChain);
static volatile bool g_badgeOn = true;

static void OverlayFrame(IDXGISwapChain* swapChain)
{
    if (g_overlayFailed)
        return;
    if (!g_overlayReady)
    {
        if (!OverlayInit(swapChain))
        {
            g_overlayFailed = true;
            return;
        }
        g_overlayReady = true;
    }

    MenuEnsureWindowHook(swapChain);
    const DWORD now = GetTickCount();
    if (g_overlayOn && now - g_overlayRefreshed >= 250)
    {
        g_overlayRefreshed = now;
        AcquireSRWLockExclusive(&g_formatLock);
        OverlayCollect();
        ReleaseSRWLockExclusive(&g_formatLock);
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    MenuFrameInputs();
    ImGui::NewFrame();
    const ImGuiIO& io = ImGui::GetIO();
    if (g_overlayOn)
    {
    ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.62f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.62f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_AlwaysAutoResize;
    ImGui::Begin("gamelink feeds", nullptr, flags);
    ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1.0f), "%s%s%s%s",
                       g_traceInstalled ? "trace on" : "trace off", g_propInstalled ? "  |  prop on" : "  |  prop off",
                       g_feedsPaused ? "  |  PAUSED" : "",
                       g_overlayFilter.empty() ? "" : ("  |  filter: " + g_overlayFilter).c_str());
    auto draw = [](const std::vector<std::string>& lines)
    {
        for (const std::string& line : lines)
        {
            const ImVec4 colour = line[0] == 'S' ? ImVec4(1.0f, 0.82f, 0.55f, 1.0f) : ImVec4(0.62f, 0.85f, 1.0f, 1.0f);
            ImGui::TextColored(colour, "%s", line.c_str());
        }
    };
    if ((g_overlayFeeds & 1) != 0)
    {
        ImGui::Separator();
        draw(g_overlayEvents);
    }
    if ((g_overlayFeeds & 2) != 0)
    {
        ImGui::Separator();
        draw(g_overlayProps);
    }
    ImGui::End();
    }
    Menu_Frame(g_menu);                          // draws while open; camera travel and screenshots tick always
    if (g_badgeOn && !Menu_HideOverlays())
        BadgeDraw();
    ImGui::Render();

    ID3D11Texture2D* back = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || back == nullptr)
        return;
    D3D11_TEXTURE2D_DESC backDesc = {};
    back->GetDesc(&backDesc);
    D3D11_RENDER_TARGET_VIEW_DESC viewDesc = {};
    viewDesc.Format = OverlayViewFormat(backDesc.Format);
    viewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ID3D11RenderTargetView* view = nullptr;
    if (SUCCEEDED(g_overlayDevice->CreateRenderTargetView(back, &viewDesc, &view)) && view != nullptr)
    {
        // The DX11 backend restores most state but not the bound targets: put the game's back.
        ID3D11RenderTargetView* savedViews[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ID3D11DepthStencilView* savedDepth = nullptr;
        g_overlayContext->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedViews, &savedDepth);
        g_overlayContext->OMSetRenderTargets(1, &view, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_overlayContext->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedViews, savedDepth);
        for (ID3D11RenderTargetView* saved : savedViews)
            if (saved != nullptr)
                saved->Release();
        if (savedDepth != nullptr)
            savedDepth->Release();
        view->Release();
    }
    back->Release();
}

static void OverlayFrameUnguarded(IDXGISwapChain* swapChain)
{
    try { OverlayFrame(swapChain); }
    catch (...) { g_overlayFailed = true; }
}

// F9 shows/hides the overlay, F10 pauses/resumes both feeds - read on the presenting
// thread, only while the game window is in front, edge-triggered. Armed by the first
// 'overlay on', so F-keys do nothing in a session that never asked for the overlay.
static volatile bool g_overlayKeysArmed = false;
static HWND g_overlayWindow = nullptr;

static void OverlayKeys(IDXGISwapChain* swapChain)
{
    static bool f1Down = false, f9Down = false, f10Down = false;
    if (g_overlayWindow == nullptr)
    {
        DXGI_SWAP_CHAIN_DESC desc = {};
        if (SUCCEEDED(swapChain->GetDesc(&desc)))
            g_overlayWindow = desc.OutputWindow;
    }
    if (g_overlayWindow == nullptr || GetForegroundWindow() != g_overlayWindow)
        return;
    // F1 - the menu - is always live once the present hook is in; F9/F10 only after 'overlay on'.
    const bool f1 = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
    if (f1 && !f1Down)
    {
        g_menu.Toggle();
        LogLine("menu: F1 -> %s", g_menu.IsOpen() ? "open" : "closed");
    }
    f1Down = f1;
    if (!g_overlayKeysArmed)
        return;
    const bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    const bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (f9 && !f9Down)
        g_overlayOn = !g_overlayOn;
    if (f10 && !f10Down)
        g_feedsPaused = !g_feedsPaused;
    f9Down = f9;
    f10Down = f10;
}

static void OverlayFrameGuarded(IDXGISwapChain* swapChain)
{
    OverlayKeys(swapChain);
    if (!g_overlayOn && !MenuIsOpen() && !g_badgeOn)
        return;
    __try
    {
        OverlayFrameUnguarded(swapChain);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_overlayFailed = true;                  // never take the game down for a debug overlay
    }
}

// ---- F1 menu: frame glue ------------------------------------------------------------------------------------
// Keyboard and wheel arrive through the raw-input hooks (MenuConsumeRaw); mouse buttons are read directly; the
// mouse position comes from the ImGui Win32 backend. Window messages are not fed to ImGui, so nothing is counted
// twice. The WndProc subclass only closes the menu on focus loss and keeps legacy input messages from the game.
static WNDPROC g_gameWndProc = nullptr;
static HWND g_menuWindow = nullptr;
static bool g_menuWasOpen = false;

static LRESULT CALLBACK MenuWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_ACTIVATEAPP && wParam == FALSE)
        g_menu.OnFocusLost();
    if (g_menu.IsOpen())
    {
        const bool input = (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
                           (message >= WM_KEYFIRST && message <= WM_KEYLAST);
        if (input)
            return 0;
    }
    return CallWindowProcW(g_gameWndProc, window, message, wParam, lParam);
}

static void MenuEnsureWindowHook(IDXGISwapChain* swapChain)
{
    if (g_gameWndProc != nullptr)
        return;
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(swapChain->GetDesc(&desc)) || desc.OutputWindow == nullptr)
        return;
    g_menuWindow = desc.OutputWindow;
    g_gameWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_menuWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MenuWndProc)));
    LogLine("menu: window procedure hooked on 0x%p", g_menuWindow);
}

static void MenuFrameInputs()
{
    ImGuiIO& io = ImGui::GetIO();
    const bool open = g_menu.IsOpen();
    io.MouseDrawCursor = open;
    UiInputLockInit();
    std::vector<UiKey> keys;
    EnterCriticalSection(&g_uiInputLock);
    keys.swap(g_uiKeys);
    LeaveCriticalSection(&g_uiInputLock);
    const LONG wheel = InterlockedExchange(&g_uiWheel, 0);
    if (!open)
    {
        if (g_menuWasOpen)                       // release everything ImGui thought was held
        {
            memset(io.KeysDown, 0, sizeof(io.KeysDown));
            memset(io.MouseDown, 0, sizeof(io.MouseDown));
        }
        g_menuWasOpen = false;
        return;
    }
    g_menuWasOpen = true;
    io.MouseDown[0] = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    io.MouseDown[1] = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    io.MouseDown[2] = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
    io.MouseWheel += static_cast<float>(wheel) / WHEEL_DELTA;
    for (const UiKey& k : keys)
    {
        if (k.vk < 512)
            io.KeysDown[k.vk] = k.down;
        if (!k.down || k.vk == VK_F1)
            continue;
        BYTE state[256] = {};
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) state[VK_SHIFT] = 0x80;
        if (GetKeyState(VK_CAPITAL) & 1) state[VK_CAPITAL] = 0x01;
        wchar_t text[4] = {};
        const int n = ToUnicode(k.vk, k.scan, state, text, 4, 0);
        for (int i = 0; i < n; ++i)
            if (text[i] >= 32)
                io.AddInputCharacter(text[i]);
    }
}

static void BadgeDraw()
{
    const ImGuiIO& io = ImGui::GetIO();
    const char* text = g_size == kExpectedModuleSize ? "gamelink - F1" : "gamelink - unsupported game version";
    const ImVec4 colour = g_size == kExpectedModuleSize ? ImVec4(0.36f, 0.84f, 0.48f, 0.9f) : ImVec4(1.0f, 0.76f, 0.28f, 0.95f);
    const ImVec2 size = ImGui::CalcTextSize(text);
    ImGui::GetForegroundDrawList()->AddText(ImVec2(io.DisplaySize.x - size.x - 14.0f, io.DisplaySize.y - size.y - 10.0f),
                                            ImGui::ColorConvertFloat4ToU32(colour), text);
}

// Once the game window and its D3D device exist: the present hook (the menu's drawing) and the input hooks (the
// menu's input). Retries the input hooks until the game's import slots are unpacked.
static DWORD WINAPI MenuBootThread(LPVOID)
{
    for (int i = 0; i < 600; ++i)                 // up to ~20 minutes
    {
        Sleep(2000);
        if (FindOwnGameWindow() == nullptr || GetModuleHandleW(L"d3d11.dll") == nullptr ||
            GetModuleHandleW(L"dxgi.dll") == nullptr)
            continue;
        Sleep(3000);
        const std::string err = InstallPresentHook();
        LogLine("menu: present hook %s", err.empty() ? "installed - F1 opens the menu" : err.c_str());
        for (int k = 0; k < 60 && g_inputHooksPatched == 0; ++k)
        {
            LogLine("menu: %s", InstallInputHooks().c_str());
            if (g_inputHooksPatched == 0)
                Sleep(5000);
        }
        return 0;
    }
    return 0;
}

// ---- wires: every connection of the loaded blueprints ----------------------------
// 'wires <blueprint filter>' lists the event connections (EntityBusData+0x40) and property
// connections (DataBusData+0x28) of every loaded blueprint whose path matches, formatted
// exactly like the feed lines (minus realm and value), so `frostycli game wires --against`
// can mark which ones fired in a recording and which never did.
//   E Delay[d].Out -> EventGate[13].In  @AI/.../PF_Skirmish_AI_Ability_Logic
//   P InterfaceDescriptor[4].DistanceToTarget -> CompareFloat[3].A  @AI/.../PF_..._Hunter_Logic
// Blueprints come from both worlds' sub-levels (their asset at +0x60 is the SubWorldData /
// LevelData), every live entity's bus, and whatever the feeds have already seen fire.

static void WiresCollectSubLevels(uintptr_t subLevel, std::unordered_set<uintptr_t>& owners,
                                  std::unordered_set<uintptr_t>& visited, int depth)
{
    if (subLevel == 0 || depth > 32 || !visited.insert(subLevel).second)
        return;
    uintptr_t asset = 0;
    if (SafeRead(subLevel + kSubLevelAsset, &asset) && asset != 0)
        owners.insert(asset);
    uintptr_t next = 0;
    if (SafeRead(subLevel + kSubLevelChild, &next) && next != 0)
        WiresCollectSubLevels(next, owners, visited, depth + 1);
    if (SafeRead(subLevel + kSubLevelSibling, &next) && next != 0)
        WiresCollectSubLevels(next, owners, visited, depth);
}

static std::string WiresList(const std::string& filter, bool events, bool props)
{
    std::unordered_set<uintptr_t> owners;
    for (int realm = 0; realm < 2; ++realm)
    {
        uintptr_t world = 0, root = 0;
        if (!SafeRead(g_base + kGameWorldArrayRva + realm * sizeof(uintptr_t), &world) || world == 0 ||
            !SafeRead(world + kGameWorldRootSubLevel, &root) || root == 0)
            continue;
        std::unordered_set<uintptr_t> visited;
        WiresCollectSubLevels(root, owners, visited, 0);
        std::vector<EntityRow> rows;
        WalkSubLevels(root, &rows, 400000, 0);
        for (const EntityRow& row : rows)
        {
            uintptr_t data = 0, owner = 0;
            if (PropListenerEntity(row.entity, &data, &owner) && owner != 0)
                owners.insert(owner);
        }
    }
    for (const auto& seen : g_traceOwnerNames)
        owners.insert(seen.first);

    std::vector<std::pair<std::string, uintptr_t>> matched;
    for (uintptr_t owner : owners)
    {
        const std::string& name = TraceOwnerName(owner);
        if (!name.empty() && FilterPasses(name, filter))
            matched.emplace_back(name, owner);
    }
    std::sort(matched.begin(), matched.end());

    std::string out;
    size_t eventCount = 0, propCount = 0;
    for (const auto& entry : matched)
    {
        const uintptr_t owner = entry.second;
        const std::string where = "  @" + entry.first;
        const auto readArray = [&](size_t offset, size_t stride, std::vector<unsigned char>& bytes) -> uint32_t
        {
            uintptr_t array = 0;
            uint32_t count = 0;
            if (!SafeRead(owner + offset, &array) || array == 0 || !SafeRead(array - 4, &count) || count > 1000000)
                return 0;
            bytes.resize(static_cast<size_t>(count) * stride);
            return count != 0 && SafeReadBytes(array, bytes.data(), bytes.size()) ? count : 0;
        };
        std::vector<unsigned char> bytes;
        if (events)
        {
            const uint32_t count = readArray(0x40, 0x20, bytes);
            for (uint32_t i = 0; i < count; ++i)
            {
                const unsigned char* c = bytes.data() + i * 0x20;
                uintptr_t source = 0, target = 0;
                uint32_t sourceEvent = 0, targetEvent = 0;
                int32_t targetType = 0;
                memcpy(&source, c, 8); memcpy(&target, c + 8, 8);
                memcpy(&sourceEvent, c + 0x10, 4); memcpy(&targetEvent, c + 0x14, 4); memcpy(&targetType, c + 0x18, 4);
                out += "E " + TraceTypeName(source) + TraceInstanceTag(source, owner) + "." + HashText(sourceEvent) +
                       " -> " + TraceTypeName(target) + TraceInstanceTag(target, owner) + "." + HashText(targetEvent) +
                       ((targetType == 4 || targetType == 5) ? " net" : "") + where + "\n";
                ++eventCount;
            }
        }
        if (props)
        {
            const uint32_t count = readArray(0x28, 0x20, bytes);
            for (uint32_t i = 0; i < count; ++i)
            {
                const unsigned char* c = bytes.data() + i * 0x20;
                uintptr_t source = 0, target = 0;
                uint32_t sourceField = 0, targetField = 0;
                memcpy(&source, c, 8); memcpy(&target, c + 8, 8);
                memcpy(&sourceField, c + 0x10, 4); memcpy(&targetField, c + 0x14, 4);
                out += "P " + TraceTypeName(source) + TraceInstanceTag(source, owner) + "." + HashText(sourceField) +
                       " -> " + TraceTypeName(target) + TraceInstanceTag(target, owner) + "." + HashText(targetField) +
                       where + "\n";
                ++propCount;
            }
        }
    }

    char header[192];
    sprintf_s(header, "%zu blueprints matched (of %zu loaded), %zu event wires, %zu property wires\n",
              matched.size(), owners.size(), eventCount, propCount);
    return header + out;
}

// ---- budget: the fixed engine limits ports run into -------------------------------
// MeshStreamingManager (global game+0x4057528, built by game+0x1591790) keeps one 0x390-byte
// MeshInfo per unique loaded MeshAsset in a fixed vector (+0x70 begin, +0x78 end, +0x80 capacity
// end, 2,048 entries) with no bounds check: going over corrupts what follows and crashes at
// game+0x15AC36B or, later, game+0x7C652xx. Placements of the same MeshAsset share one entry;
// soldiers, weapons and vehicles streamed at runtime take entries too, so leave headroom.
constexpr uintptr_t kMeshStreamingManagerRva = 0x4057528;
constexpr uintptr_t kMeshInfoSize = 0x390;

static std::string BudgetReport()
{
    std::string out;
    char line[256];
    uintptr_t mgr = 0, begin = 0, end = 0, capEnd = 0;
    if (SafeRead(g_base + kMeshStreamingManagerRva, &mgr) && mgr != 0 &&
        SafeRead(mgr + 0x70, &begin) && SafeRead(mgr + 0x78, &end) && SafeRead(mgr + 0x80, &capEnd) &&
        begin != 0 && end >= begin && capEnd >= end)
    {
        const size_t used = (end - begin) / kMeshInfoSize, cap = (capEnd - begin) / kMeshInfoSize;
        const int pct = cap ? static_cast<int>(used * 100 / cap) : 0;
        sprintf_s(line, "unique meshes   %5zu / %zu  (%d%%)%s\n", used, cap, pct,
                  used >= cap ? "  OVER - crash at game+0x15AC36B / 0x7C652xx expected"
                  : pct >= 90 ? "  DANGER - soldiers/vehicles spawning at runtime can push it over"
                  : pct >= 75 ? "  getting close" : "");
        out += line;
    }
    else
        out += "unique meshes   (mesh streaming manager not readable yet)\n";

    for (int world = 0; world < 2; ++world)
    {
        uintptr_t gameWorld = 0, root = 0;
        if (!SafeRead(g_base + kGameWorldArrayRva + world * sizeof(uintptr_t), &gameWorld) || gameWorld == 0 ||
            !SafeRead(gameWorld + kGameWorldRootSubLevel, &root) || root == 0)
            continue;
        std::unordered_set<uintptr_t> subLevels, visited;
        WiresCollectSubLevels(root, subLevels, visited, 0);
        std::vector<EntityRow> rows;
        WalkSubLevels(root, &rows, 400000, 0);
        std::unordered_set<uintptr_t> unique;          // a prefab can be reached down two branches
        for (const EntityRow& row : rows)
            unique.insert(row.entity);
        sprintf_s(line, "%s  %4zu sub-levels, %6zu entities%s\n", world == 0 ? "client" : "server",
                  visited.size(), unique.size(), rows.size() >= 400000 ? "+ (walk limit reached)" : "");
        out += line;
    }
    return out;
}

// ---- fire / deliver: resolve the target, queue, wait -----------------------------
//   fire    <Type[tag]>.<Event> @<blueprint> [S|C] [all]  - the instance emits the event: every
//           wire from it runs, as if it had fired (EntityBus::fireEvent on its bus)
//   deliver <Type[tag]>.<Event> @<blueprint> [S|C] [all]  - the event arrives at the instance, as
//           if a wire delivered it (registerEvent to its entity)
// [tag] is the feed's instance tag ([2e] or #1a2b3c4d), the event a name or 8 hex digits, the
// blueprint any unique part of its path. S/C picks the realm (default: server if it exists there).
// A blueprint placed several times has one entity per placement: the first is used, or every
// one with 'all'. Needs the trace on: requests run inside its hook, on the realm's logic thread.

struct LiveInstance { uintptr_t entity, bus, data, owner; uint32_t realm; };

static void CollectLiveInstances(std::vector<LiveInstance>& out)
{
    const uintptr_t busVtable = g_base + kFullEntityBusVtableRva;
    std::unordered_set<uintptr_t> seen;
    for (int world = 0; world < 2; ++world)
    {
        uintptr_t gameWorld = 0, root = 0;
        if (!SafeRead(g_base + kGameWorldArrayRva + world * sizeof(uintptr_t), &gameWorld) || gameWorld == 0 ||
            !SafeRead(gameWorld + kGameWorldRootSubLevel, &root) || root == 0)
            continue;
        std::vector<EntityRow> rows;
        WalkSubLevels(root, &rows, 400000, 0);
        for (const EntityRow& row : rows)
        {
            for (const size_t busAt : { size_t(0x20), size_t(0x30) })
            {
                uintptr_t bus = 0, vtable = 0, data = 0, runtime = 0, owner = 0;
                uint32_t flags = 0;
                if (!SafeRead(row.entity + busAt, &bus) || bus == 0 || !SafeRead(bus, &vtable) || vtable != busVtable)
                    continue;
                if (!SafeRead(row.entity + busAt + 8, &data) || TraceInstanceSize(data) == 0)
                    continue;
                if (SafeRead(bus + 0x68, &runtime) && runtime != 0)
                    SafeRead(runtime + 0xB8, &owner);
                SafeRead(bus + 0x3C, &flags);
                if (seen.insert(row.entity).second)
                    out.push_back({ row.entity, bus, data, owner, (flags >> 28) & 1 });
                break;
            }
        }
    }
}

static uint32_t EventIdFromText(const std::string& text)
{
    if (text.size() == 8 && text.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos)
        return static_cast<uint32_t>(strtoul(text.c_str(), nullptr, 16));
    uint32_t h = 5381;                                  // DJB2-xor, the engine's name hash
    for (unsigned char c : text)
        h = (h * 33) ^ c;
    return h;
}

static std::string FireCommand(const std::string& rest, bool deliver)
{
    const char* usage = deliver ? "usage: deliver <Type[tag]>.<Event> @<blueprint> [S|C] [all]"
                                : "usage: fire <Type[tag]>.<Event> @<blueprint> [S|C] [all]";
    std::vector<std::string> tokens;
    {
        size_t at = 0;
        while (at < rest.size())
        {
            while (at < rest.size() && rest[at] == ' ') ++at;
            size_t end = rest.find(' ', at);
            if (end == std::string::npos) end = rest.size();
            if (end > at) tokens.push_back(rest.substr(at, end - at));
            at = end;
        }
    }
    std::string spec, blueprint;
    int realm = -1;
    bool all = false;
    for (const std::string& t : tokens)
    {
        if (t[0] == '@') blueprint = t.substr(1);
        else if (t == "S" || t == "s" || t == "server") realm = 1;
        else if (t == "C" || t == "c" || t == "client") realm = 0;
        else if (t == "all") all = true;
        else spec = t;
    }
    const size_t dot = spec.rfind('.');
    if (spec.empty() || blueprint.empty() || dot == std::string::npos)
        return usage;
    const std::string instance = spec.substr(0, dot);
    const uint32_t eventId = EventIdFromText(spec.substr(dot + 1));
    const size_t tagAt = instance.find_first_of("[#");
    if (tagAt == std::string::npos)
        return "fire: the instance needs its tag from the feed, e.g. EventGate[13] or LogicPrefabReferenceObject#32914a11";
    const std::string typeName = instance.substr(0, tagAt);
    const std::string tag = instance.substr(tagAt);

    if (!g_traceInstalled)
        return "fire: turn the trace on first ('trace on') - requests run inside its hook, on the game's logic thread";
    if (!FeedLevelLoaded())
        return "fire: no level is loaded";

    std::vector<LiveInstance> live;
    CollectLiveInstances(live);
    std::vector<LiveInstance> matches;
    std::unordered_set<uintptr_t> owners;
    AcquireSRWLockExclusive(&g_formatLock);
    for (const LiveInstance& li : live)
    {
        const std::string& owner = TraceOwnerName(li.owner);
        if (owner.empty() || !ContainsNoCase(owner, blueprint))
            continue;
        if (!typeName.empty() && TraceTypeName(li.data) != typeName)
            continue;
        std::string liveTag = TraceInstanceTag(li.data, li.owner);
        if (liveTag.empty()) continue;
        if (tag[0] == '#' ? liveTag.compare(0, tag.size(), tag) != 0 : liveTag != tag)
            continue;
        if (realm >= 0 && li.realm != static_cast<uint32_t>(realm))
            continue;
        matches.push_back(li);
        owners.insert(li.owner);
    }
    std::string ownerNames;
    for (uintptr_t o : owners)
        ownerNames += "\n  @" + TraceOwnerName(o);
    ReleaseSRWLockExclusive(&g_formatLock);

    if (matches.empty())
        return "fire: no live " + instance + " in a blueprint matching '" + blueprint + "'" +
               (realm >= 0 ? (realm == 1 ? " on the server" : " on the client") : "") +
               " - take the type, tag and blueprint from a feed or 'wires' line";
    if (owners.size() > 1)
        return "fire: '" + blueprint + "' matches several blueprints - give more of the path:" + ownerNames;
    if (realm < 0)                                       // default: the server side if there is one
    {
        const bool onServer = std::any_of(matches.begin(), matches.end(), [](const LiveInstance& m) { return m.realm == 1; });
        const uint32_t pick = onServer ? 1u : 0u;
        matches.erase(std::remove_if(matches.begin(), matches.end(), [pick](const LiveInstance& m) { return m.realm != pick; }),
                      matches.end());
    }
    const size_t total = matches.size();
    if (!all)
        matches.resize(1);

    if (g_fireDone == nullptr)
        g_fireDone = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    size_t done = 0, faulted = 0;
    for (const LiveInstance& target : matches)
    {
        g_fireRequest.job = nullptr;
        g_fireRequest.deliver = deliver;
        g_fireRequest.bus = target.bus;
        g_fireRequest.source = target.data;
        g_fireRequest.entity = target.entity;
        g_fireRequest.eventId = eventId;
        g_fireRequest.realm = target.realm;
        g_fireRequest.faulted = false;
        ResetEvent(g_fireDone);
        InterlockedExchange(&g_fireRequest.state, 1);
        if (WaitForSingleObject(g_fireDone, 5000) != WAIT_OBJECT_0)
        {
            // Not picked up: withdraw it unless the hook took it in the meantime.
            if (InterlockedCompareExchange(&g_fireRequest.state, 0, 1) == 1)
                return "fire: no logic ran on the " + std::string(target.realm ? "server" : "client") +
                       " within 5 s to carry it out (is the game paused or in a menu?)";
            WaitForSingleObject(g_fireDone, 5000);
        }
        InterlockedExchange(&g_fireRequest.state, 0);
        ++done;
        if (g_fireRequest.faulted)
            ++faulted;
    }

    char line[256];
    sprintf_s(line, "%s %s.%s on %zu of %zu %s instance(s)%s", deliver ? "delivered" : "fired",
              instance.c_str(), HashText(eventId).c_str(), done, total, matches[0].realm ? "server" : "client",
              faulted ? " - FAULTED (caught), nothing more done" : "");
    LogLine("%s %s", deliver ? "deliver:" : "fire:", line);
    return line + ownerNames;
}



// ---- viewhook: the camera override at its source -----------------------------------------
// The render view is written by exactly one routine, game+0x6FD3E90: a RenderView copy (dst rcx,
// src rdx; the transform is its first 0x40 bytes, then fov and the rest) that the camera code runs
// every frame - found with 'watchwrite renderview' in a live match, 2026-10-08, same single writer
// on the spawn screen and while alive. Every other override raced it and the soldier camera won
// some frames (flicker). Hooked here, the copy runs, then - only when its destination is the live
// render view and camlock is on - our pose is written over it, on the engine's own thread, inside
// the same call. Nothing is left to race.
constexpr uintptr_t kRenderViewCopyRva = 0x6FD3E90;
static const unsigned char kRenderViewCopyHead[6] = { 0x0F, 0x28, 0x02, 0x0F, 0x29, 0x01 };  // movaps xmm0,[rdx]; movaps [rcx],xmm0
static unsigned char* g_viewHookStub = nullptr;
static uint64_t g_viewHookOriginal8 = 0;
static bool g_viewHookInstalled = false;
static volatile LONG64 g_viewHookHits = 0;

static void __fastcall ViewCopyOverride(uintptr_t destination)
{
    if (!g_cameraOverride || destination == 0 || destination != GetRenderView())
        return;
    Transform copy;
    EnterCriticalSection(&g_cameraLock);
    copy = g_cameraTarget;
    LeaveCriticalSection(&g_cameraLock);
    __try
    {
        memcpy(reinterpret_cast<void*>(destination), &copy, sizeof(copy));
        InterlockedIncrement64(&g_viewHookHits);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static std::string InstallViewHook(bool enable)
{
    const uintptr_t site = g_base + kRenderViewCopyRva;      // 16-aligned function start: 8 bytes patched as one
    if (!enable)
    {
        if (!g_viewHookInstalled)
            return "viewhook is off";
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(site), 8, PAGE_EXECUTE_READWRITE, &old))
            return "viewhook: could not unprotect the copy routine";
        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(site), static_cast<LONG64>(g_viewHookOriginal8));
        VirtualProtect(reinterpret_cast<void*>(site), 8, old, &old);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), 8);
        g_viewHookInstalled = false;
        LogLine("viewhook: off after %lld overrides", static_cast<long long>(g_viewHookHits));
        return "viewhook off";
    }
    if (g_viewHookInstalled)
        return "viewhook is on";
    if (!FeedLevelLoaded())
        return "viewhook: no level is loaded (never patch code before one is)";
    unsigned char head[8] = {};
    if (!SafeReadBytes(site, head, sizeof(head)) || memcmp(head, kRenderViewCopyHead, sizeof(kRenderViewCopyHead)) != 0)
        return "viewhook: refusing - the RenderView copy at game+0x6FD3E90 doesn't start with the expected bytes";

    if (g_viewHookStub == nullptr)                          // built once, never freed
    {
        unsigned char* stub = NearCode(site, 0x100);
        if (stub == nullptr)
            return "viewhook: could not allocate a stub within reach";
        unsigned char* c = stub;
        auto put = [&](std::initializer_list<unsigned char> bytes) { for (unsigned char b : bytes) *c++ = b; };
        // entry (rsp = 8 mod 16, as after a call)
        put({ 0x51 });                                      // push rcx                      (dst)
        put({ 0x48, 0x83, 0xEC, 0x30 });                    // sub rsp, 0x30                 (shadow + rax slot)
        unsigned char* callTrampoline = c;
        put({ 0xE8, 0, 0, 0, 0 });                          // call trampoline               (the original copy)
        put({ 0x48, 0x89, 0x44, 0x24, 0x20 });              // mov [rsp+0x20], rax
        put({ 0x48, 0x8B, 0x4C, 0x24, 0x30 });              // mov rcx, [rsp+0x30]           (dst)
        unsigned char* callOverride = c;
        put({ 0xFF, 0x15, 0, 0, 0, 0 });                    // call [rip+disp]               (ViewCopyOverride)
        put({ 0x48, 0x8B, 0x44, 0x24, 0x20 });              // mov rax, [rsp+0x20]           (the copy's return value)
        put({ 0x48, 0x83, 0xC4, 0x30 });                    // add rsp, 0x30
        put({ 0x59 });                                      // pop rcx
        put({ 0xC3 });                                      // ret
        unsigned char* trampoline = c;
        for (unsigned char b : kRenderViewCopyHead) *c++ = b;   // the 6 bytes the patch replaces
        put({ 0xFF, 0x25, 0, 0, 0, 0 });                    // jmp [rip+0]
        const uintptr_t resume = site + sizeof(kRenderViewCopyHead);
        memcpy(c, &resume, 8); c += 8;
        unsigned char* overridePtr = c;
        void* fn = reinterpret_cast<void*>(&ViewCopyOverride);
        memcpy(c, &fn, 8); c += 8;
        const int32_t toTrampoline = static_cast<int32_t>(trampoline - (callTrampoline + 5));
        memcpy(callTrampoline + 1, &toTrampoline, 4);
        const int32_t toPtr = static_cast<int32_t>(overridePtr - (callOverride + 6));
        memcpy(callOverride + 2, &toPtr, 4);
        FlushInstructionCache(GetCurrentProcess(), stub, 0x100);
        g_viewHookStub = stub;
    }

    const intptr_t delta = reinterpret_cast<intptr_t>(g_viewHookStub) - static_cast<intptr_t>(site + 5);
    if (delta < INT32_MIN || delta > INT32_MAX)
        return "viewhook: stub out of rel32 reach";
    memcpy(&g_viewHookOriginal8, head, 8);
    unsigned char patched[8];
    memcpy(patched, head, 8);
    patched[0] = 0xE9;                                      // jmp stub
    const int32_t rel = static_cast<int32_t>(delta);
    memcpy(patched + 1, &rel, 4);
    patched[5] = 0x90;                                      // nop (bytes 6-7 are the routine's own)
    uint64_t value = 0;
    memcpy(&value, patched, 8);
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(site), 8, PAGE_EXECUTE_READWRITE, &old))
        return "viewhook: could not unprotect the copy routine";
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(site), static_cast<LONG64>(value));
    VirtualProtect(reinterpret_cast<void*>(site), 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), 8);
    g_viewHookInstalled = true;
    g_viewHookHits = 0;
    LogLine("viewhook: on - RenderView copy game+0x%llX -> stub 0x%p", static_cast<unsigned long long>(kRenderViewCopyRva), g_viewHookStub);
    return "viewhook on - with camlock on, the render view takes our pose inside the engine's own copy";
}

// ---- inspect / write: an instance's fields through the engine's own property reader and writer --
// A live entity reads each input field through a PropertyReader: a CacheData when something
// is wired to (or overrides) that field on its bus, else the EBX value. Asking the engine's
// PropertyReaderBase::set for (bus, data, field) gives the same reader the entity has; its
// get() is the value the entity sees right now. Writing goes through PropertyWriterBase::set
// on that same CacheData (registerWrite), so every listener is notified exactly as if the
// wire had carried the value. Addresses: ArmchairDevelopers/Kyber NativeEntityManager.h
// (same build), checked by disassembly 2026-10-08 - set ends in registerWrite (#56).
constexpr uintptr_t kDataContextCtorRva = 0x71DF800;  // DataContext(bus) -> {bus, data, exposed}
constexpr uintptr_t kReaderSetRva = 0x5442490;        // PropertyReaderBase::set(reader, dc, data, fieldHash, typeInfo, default)
constexpr uintptr_t kReaderGetRva = 0x543AD40;        // PropertyReaderBase::get: cache value if written (+0x48 bit 6), else default
constexpr uintptr_t kWriterSetRva = 0x543E700;        // PropertyWriterBase::set(writer, value, callListeners) -> registerWrite
struct GlDataContext { uintptr_t bus, data, exposed; };
struct GlReader { uintptr_t cache; const void* defaultValue; };
typedef GlDataContext*(__fastcall* DataContextCtorFn)(uintptr_t bus, GlDataContext* out);
typedef void(__fastcall* ReaderSetFn)(GlReader* reader, const GlDataContext* dc, uintptr_t data, int32_t fieldHash,
                                      uintptr_t typeInfo, const void* defaultValue);
typedef const void*(__fastcall* ReaderGetFn)(const GlReader* reader);
typedef void(__fastcall* WriterSetFn)(const uintptr_t* writer, const void* value, bool callListeners);

constexpr size_t kFieldValueBytes = 64;                // a LinearTransform
struct FieldProbe
{
    uint32_t hash, offset, size;
    uintptr_t typeInfo;
    bool viaBus, live, readFault;                      // in: read through the bus; out: wired/overridden, value from the cache
    bool written;                                      // out: the cache holds a value (+0x48 bit 6)
    uintptr_t liveType;                                // out: the cache's value TypeInfo (+0x40)
    uintptr_t via;                                     // in: read this instance's reader instead (an output's target)
    unsigned char value[kFieldValueBytes];
};
struct FieldJob
{
    uintptr_t bus, data;
    FieldProbe* probes;
    size_t count;
    // write: probes[0] is the field; the value to write and what was there
    bool write, notWired;
    unsigned char newValue[kFieldValueBytes];
    unsigned char before[kFieldValueBytes];
};

static void CopyOut(unsigned char* to, const void* from, uint32_t size)
{
    memset(to, 0, kFieldValueBytes);
    if (from != nullptr)
        memcpy(to, from, size < kFieldValueBytes ? size : kFieldValueBytes);
}

// On the realm's logic thread (see ExecuteFireRequest). No C++ objects here: SEH only.
static void RunFieldJob(void* context)
{
    FieldJob* job = static_cast<FieldJob*>(context);
    GlDataContext dc = {};
    reinterpret_cast<DataContextCtorFn>(g_base + kDataContextCtorRva)(job->bus, &dc);
    for (size_t i = 0; i < job->count; ++i)
    {
        FieldProbe& probe = job->probes[i];
        const uintptr_t data = probe.via != 0 ? probe.via : job->data;
        const void* ebxValue = reinterpret_cast<const void*>(data + probe.offset);
        if (!probe.viaBus)
        {
            __try { CopyOut(probe.value, ebxValue, probe.size); }
            __except (EXCEPTION_EXECUTE_HANDLER) { probe.readFault = true; }
            continue;
        }
        __try
        {
            GlReader reader = {};
            reinterpret_cast<ReaderSetFn>(g_base + kReaderSetRva)(&reader, &dc, data, static_cast<int32_t>(probe.hash),
                                                                 probe.typeInfo, ebxValue);
            probe.live = reader.cache != 0;
            if (reader.cache != 0)
            {
                probe.liveType = *reinterpret_cast<const uintptr_t*>(reader.cache + 0x40);
                probe.written = (*reinterpret_cast<const uint32_t*>(reader.cache + 0x48) & 0x40) != 0;
            }
            const void* now = reinterpret_cast<ReaderGetFn>(g_base + kReaderGetRva)(&reader);
            CopyOut(probe.value, now, probe.size);
            if (job->write)
            {
                if (reader.cache == 0)
                {
                    job->notWired = true;                // the entity reads its EBX value directly
                    return;
                }
                memcpy(job->before, probe.value, kFieldValueBytes);
                const uintptr_t writer = reader.cache;   // a PropertyWriterBase is just its CacheData*
                reinterpret_cast<WriterSetFn>(g_base + kWriterSetRva)(&writer, job->newValue, true);
                CopyOut(probe.value, reinterpret_cast<ReaderGetFn>(g_base + kReaderGetRva)(&reader), probe.size);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            probe.readFault = true;
            if (job->write)
                return;
        }
    }
}

// Runs a job on the given realm's logic thread; "" when done, else why not.
static std::string RunLogicJob(uint32_t realm, void (*job)(void*), void* context, bool* faulted)
{
    if (g_fireDone == nullptr)
        g_fireDone = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (g_fireRequest.state != 0)
        return "another fire / write is still running";
    g_fireRequest.job = job;
    g_fireRequest.jobContext = context;
    g_fireRequest.realm = realm;
    g_fireRequest.faulted = false;
    ResetEvent(g_fireDone);
    InterlockedExchange(&g_fireRequest.state, 1);
    if (WaitForSingleObject(g_fireDone, 5000) != WAIT_OBJECT_0)
    {
        if (InterlockedCompareExchange(&g_fireRequest.state, 0, 1) == 1)
        {
            g_fireRequest.job = nullptr;
            return std::string("no logic ran on the ") + (realm ? "server" : "client") +
                   " within 5 s to carry it out (is the game paused or in a menu?)";
        }
        WaitForSingleObject(g_fireDone, 5000);
    }
    InterlockedExchange(&g_fireRequest.state, 0);
    g_fireRequest.job = nullptr;
    *faulted = g_fireRequest.faulted;
    return std::string();
}

// "Type[tag]" (or "Type#guid") + blueprint filter + realm -> the live instances, one blueprint.
static std::string ResolveLiveInstances(const std::string& instance, const std::string& blueprint, int realm,
                                        std::vector<LiveInstance>& matches, std::string& ownerNames)
{
    const size_t tagAt = instance.find_first_of("[#");
    if (tagAt == std::string::npos)
        return "the instance needs its tag from the feed, e.g. EventGate[13] or LogicPrefabReferenceObject#32914a11";
    const std::string typeName = instance.substr(0, tagAt);
    const std::string tag = instance.substr(tagAt);
    if (!FeedLevelLoaded())
        return "no level is loaded";

    std::vector<LiveInstance> live;
    CollectLiveInstances(live);
    std::unordered_set<uintptr_t> owners;
    AcquireSRWLockExclusive(&g_formatLock);
    for (const LiveInstance& li : live)
    {
        const std::string& owner = TraceOwnerName(li.owner);
        if (owner.empty() || !ContainsNoCase(owner, blueprint))
            continue;
        if (!typeName.empty() && TraceTypeName(li.data) != typeName)
            continue;
        std::string liveTag = TraceInstanceTag(li.data, li.owner);
        if (liveTag.empty()) continue;
        if (tag[0] == '#' ? liveTag.compare(0, tag.size(), tag) != 0 : liveTag != tag)
            continue;
        if (realm >= 0 && li.realm != static_cast<uint32_t>(realm))
            continue;
        matches.push_back(li);
        owners.insert(li.owner);
    }
    for (uintptr_t o : owners)
        ownerNames += "\n  @" + TraceOwnerName(o);
    ReleaseSRWLockExclusive(&g_formatLock);

    if (matches.empty())
        return "no live " + instance + " in a blueprint matching '" + blueprint + "'" +
               (realm >= 0 ? (realm == 1 ? " on the server" : " on the client") : "") +
               " - take the type, tag and blueprint from a feed or 'wires' line";
    if (owners.size() > 1)
        return "'" + blueprint + "' matches several blueprints - give more of the path:" + ownerNames;
    return std::string();
}

struct InspectField { std::string name, typeName; int kind; uint32_t offset, size; uintptr_t typeInfo; uintptr_t via = 0; uint32_t viaField = 0; };

// The instance's reflected fields, most derived class first, without the DataContainer plumbing.
static std::vector<InspectField> InspectFields(uintptr_t data)
{
    std::vector<InspectField> out;
    uintptr_t classInfo = 0;
    if (!SafeRead(data + 0x08, &classInfo) || classInfo == 0 || !WalkRegistry())
        return out;
    LockRegistry();
    const TypeRec* type = FindType(classInfo);
    std::unordered_set<std::string> seen;
    for (int hops = 0; type != nullptr && hops < 32; ++hops)
    {
        if (type->name == "DataContainer" || type->name == "DataBusPeer" || type->name == "GameDataContainer")
            break;
        for (const FieldRec& field : type->fields)
        {
            if (field.name.empty() || field.name == "Flags" || !seen.insert(field.name).second)
                continue;
            InspectField f;
            f.name = field.name;
            f.offset = field.offset;
            f.typeInfo = field.typePtr;
            const TypeRec* ft = FindType(field.typePtr);
            f.typeName = ft != nullptr ? ft->name : TypeInfoName(field.typePtr);
            f.kind = ft != nullptr ? ft->kind : 0;
            f.size = ft != nullptr && ft->size > 0 ? ft->size : 8;
            if (f.kind == 3 || f.kind == 4 || f.kind == 7)
                f.size = 8;                                 // a reference, an array pointer, a char*
            out.push_back(f);
        }
        type = type->parentPtr != 0 ? FindType(type->parentPtr) : nullptr;
    }
    UnlockRegistry();
    return out;
}

static bool FieldViaBus(const InspectField& f)
{
    // What a PropertyReader can carry: primitives, enums and value types (Vec3, LinearTransform...).
    return f.kind == 2 || f.kind == 8 || (f.kind >= 10 && f.kind <= 20) || f.kind == 7;
}

static std::string EnumText(const InspectField& f, const unsigned char* v)
{
    int32_t n = 0;
    memcpy(&n, v, sizeof(n));
    char text[32];
    sprintf_s(text, "%d", n);
    std::string name;
    LockRegistry();
    if (const TypeRec* e = FindType(f.typeInfo))
        for (const FieldRec& member : e->fields)
            if (static_cast<int32_t>(member.offset) == n || static_cast<int32_t>(member.offset >> 16) == n)
            {
                name = member.name;
                break;
            }
    UnlockRegistry();
    return name.empty() ? std::string(text) : name + " (" + text + ")";
}

static std::string FieldValueText(const InspectField& f, const FieldProbe& probe, uintptr_t owner)
{
    if (probe.readFault)
        return "(unreadable)";
    const unsigned char* v = probe.value;
    uintptr_t pointer = 0;
    memcpy(&pointer, v, sizeof(pointer));
    if (f.kind == 3)                                        // a reference to another instance
    {
        if (pointer == 0)
            return "null";
        AcquireSRWLockExclusive(&g_formatLock);
        std::string text = "-> " + TraceTypeName(pointer) + TraceInstanceTag(pointer, owner);
        ReleaseSRWLockExclusive(&g_formatLock);
        return text;
    }
    if (f.kind == 4)
    {
        uint32_t count = 0;
        if (pointer != 0)
            SafeRead(pointer - 4, &count);
        return "[" + std::to_string(count) + " items]";
    }
    if (f.kind == 8)
        return EnumText(f, v);
    if (f.typeName == "LinearTransform")
        return TypedValueText(f.typeName, v + 0x30, true);
    if (f.kind == 2 && f.typeName != "Vec2" && f.typeName != "Vec3" && f.typeName != "Vec4" && f.typeName != "Quat")
        return "{" + f.typeName + "}";
    return TypedValueText(f.typeName, v, false);
}

static std::vector<std::string> SplitWords(const std::string& text)
{
    std::vector<std::string> out;
    size_t at = 0;
    while (at < text.size())
    {
        while (at < text.size() && text[at] == ' ') ++at;
        size_t end = text.find(' ', at);
        if (end == std::string::npos) end = text.size();
        if (end > at) out.push_back(text.substr(at, end - at));
        at = end;
    }
    return out;
}



// The first PropertyConnection out of this instance's field: (target instance, target field).
static bool FindOutputPeer(const LiveInstance& li, uint32_t field, uintptr_t* peer, uint32_t* peerField)
{
    uintptr_t array = 0;
    uint32_t count = 0;
    if (li.owner == 0 || !SafeRead(li.owner + 0x28, &array) || array == 0 || !SafeRead(array - 4, &count) || count > 1000000)
        return false;
    for (uint32_t i = 0; i < count; ++i)
    {
        unsigned char rec[0x18];
        if (!SafeReadBytes(array + i * 0x20, rec, sizeof(rec)))
            return false;
        uintptr_t from = 0;
        uint32_t fromField = 0;
        memcpy(&from, rec, 8);
        memcpy(&fromField, rec + 0x10, 4);
        if (from == li.data && fromField == field)
        {
            memcpy(peer, rec + 8, 8);
            memcpy(peerField, rec + 0x14, 4);
            return true;
        }
    }
    return false;
}
// Fields that exist only as wires (an Or's In1/Out, a Math's named inputs...): every
// PropertyConnection of the blueprint into or out of this instance, read through the bus.
static std::string WiredFields(const LiveInstance& li, const std::vector<InspectField>& reflected)
{
    struct Wire { bool in; uint32_t field; std::vector<std::string> peers; uintptr_t peer; uint32_t peerField; };
    std::vector<Wire> wires;
    uintptr_t array = 0;
    uint32_t count = 0;
    if (li.owner == 0 || !SafeRead(li.owner + 0x28, &array) || array == 0 || !SafeRead(array - 4, &count) || count > 1000000)
        return std::string();
    std::unordered_set<uint32_t> reflectedHashes;
    for (const InspectField& f : reflected)
        reflectedHashes.insert(EventIdFromText(f.name));
    AcquireSRWLockExclusive(&g_formatLock);
    for (uint32_t i = 0; i < count; ++i)
    {
        unsigned char rec[0x18];
        if (!SafeReadBytes(array + i * 0x20, rec, sizeof(rec)))
            break;
        uintptr_t from = 0, to = 0;
        uint32_t fromField = 0, toField = 0;
        memcpy(&from, rec, 8);
        memcpy(&to, rec + 8, 8);
        memcpy(&fromField, rec + 0x10, 4);
        memcpy(&toField, rec + 0x14, 4);
        for (int side = 0; side < 2; ++side)
        {
            const bool in = side == 0;
            if ((in ? to : from) != li.data)
                continue;
            const uint32_t field = in ? toField : fromField;
            const uintptr_t peer = in ? from : to;
            const std::string peerText = TraceTypeName(peer) + TraceInstanceTag(peer, li.owner) + "." + HashText(in ? fromField : toField);
            auto it = std::find_if(wires.begin(), wires.end(), [&](const Wire& w) { return w.in == in && w.field == field; });
            if (it == wires.end())
                wires.push_back({ in, field, { peerText }, peer, in ? fromField : toField });
            else if (it->peers.size() < 4)
                it->peers.push_back(peerText);
            else if (it->peers.size() == 4)
                it->peers.push_back("...");
        }
    }
    ReleaseSRWLockExclusive(&g_formatLock);
    if (wires.empty())
        return "  wired: none\n";

    std::vector<FieldProbe> probes(wires.size());
    for (size_t i = 0; i < wires.size(); ++i)
    {
        memset(&probes[i], 0, sizeof(FieldProbe));
        probes[i].hash = wires[i].in ? wires[i].field : wires[i].peerField;
        probes[i].via = wires[i].in ? 0 : wires[i].peer;   // an output's cache is the one its target reads
        probes[i].size = kFieldValueBytes;
        probes[i].viaBus = true;
        probes[i].offset = 0;                               // no EBX default: data+0 only seeds the reader
    }
    FieldJob job = {};
    job.bus = li.bus;
    job.data = li.data;
    job.probes = probes.data();
    job.count = probes.size();
    bool faulted = false;
    const std::string why = RunLogicJob(li.realm, RunFieldJob, &job, &faulted);
    std::string out = "  wired (property connections, value as the bus holds it now):\n";
    if (!why.empty())
        return out + "    " + why + "\n";
    for (size_t i = 0; i < wires.size(); ++i)
    {
        const Wire& w = wires[i];
        const FieldProbe& p = probes[i];
        std::string value;
        const std::string type = TypeInfoName(p.liveType);
        if (p.readFault) value = "(unreadable)";
        else if (!p.live) value = "(no cache on this bus)";
        else if (!p.written) value = "(nothing written yet)";
        else if (type == "LinearTransform") value = TypedValueText(type, p.value + 0x30, true);
        else value = TypedValueText(type, p.value, false);
        std::string peers;
        for (const std::string& peer : w.peers)
            peers += (peers.empty() ? "" : ", ") + peer;
        char row[96];
        sprintf_s(row, "    %-3s %-24s %-16s ", w.in ? "in" : "out", HashText(w.field).c_str(), type.c_str());
        out += row + value + (w.in ? "   <- " : "   -> ") + peers + "\n";
    }
    return out;
}
// The last wires in the event ring that touched this instance, newest first.
static std::string RecentWires(uintptr_t data, uintptr_t owner, size_t want)
{
    if (g_trace == nullptr)
        return std::string();
    const LONG64 next = g_traceNext;
    const LONG64 span = next < static_cast<LONG64>(kTraceCapacity) ? next : static_cast<LONG64>(kTraceCapacity);
    std::string out;
    size_t found = 0, total = 0;
    std::unordered_set<std::string> shown;
    AcquireSRWLockExclusive(&g_formatLock);
    for (LONG64 i = next - 1; i >= next - span && i >= 0; --i)
    {
        const TraceEntry& e = g_trace[i & (kTraceCapacity - 1)];
        if (e.seq != i + 1 || e.owner != owner || (e.source != data && e.target != data))
            continue;
        ++total;
        if (found >= want)
            continue;
        TraceEntry copy = e;
        std::string line = FormatTraceEntry(copy);
        const size_t at = line.find("  @");
        if (at != std::string::npos) line.erase(at);
        if (!shown.insert(line).second)
            continue;
        char ago[48];
        sprintf_s(ago, "%lld events ago", static_cast<long long>(next - 1 - i));
        out += "    " + line + "   (" + ago + ")\n";
        ++found;
    }
    ReleaseSRWLockExclusive(&g_formatLock);
    if (total == 0)
        return "  recent events: none in the trace buffer\n";
    return "  recent events (" + std::to_string(total) + " in the trace buffer, newest first):\n" + out;
}

// inspect <Type[tag]> @<blueprint> [S|C]
static std::string InspectCommand(const std::string& rest)
{
    const char* usage = "usage: inspect <Type[tag]> @<blueprint> [S|C]";
    std::string instance, blueprint;
    int realm = -1;
    for (const std::string& t : SplitWords(rest))
    {
        if (t[0] == '@') blueprint = t.substr(1);
        else if (t == "S" || t == "s" || t == "server") realm = 1;
        else if (t == "C" || t == "c" || t == "client") realm = 0;
        else instance = t;
    }
    if (instance.empty() || blueprint.empty())
        return usage;
    if (!g_traceInstalled && !g_propInstalled)
        return "inspect: turn a feed on first ('trace on' or 'prop on') - reads run inside its hook, on the game's logic thread";
    std::vector<LiveInstance> matches;
    std::string ownerNames;
    const std::string error = ResolveLiveInstances(instance, blueprint, realm, matches, ownerNames);
    if (!error.empty())
        return "inspect: " + error;

    std::string out;
    for (const LiveInstance& li : matches)
    {
        std::vector<InspectField> fields = InspectFields(li.data);
        std::vector<FieldProbe> probes(fields.size());
        for (size_t i = 0; i < fields.size(); ++i)
        {
            FieldProbe& p = probes[i];
            memset(&p, 0, sizeof(p));
            p.hash = EventIdFromText(fields[i].name);
            p.offset = fields[i].offset;
            p.size = fields[i].size;
            p.typeInfo = fields[i].typeInfo;
            p.viaBus = FieldViaBus(fields[i]);
        }
        FieldJob job = {};
        job.bus = li.bus;
        job.data = li.data;
        job.probes = probes.data();
        job.count = probes.size();
        bool faulted = false;
        const std::string why = RunLogicJob(li.realm, RunFieldJob, &job, &faulted);
        AcquireSRWLockExclusive(&g_formatLock);
        const std::string head = TraceTypeName(li.data) + TraceInstanceTag(li.data, li.owner);
        ReleaseSRWLockExclusive(&g_formatLock);
        char line[200];
        sprintf_s(line, "%s %s  (entity 0x%llX, data 0x%llX)\n", li.realm ? "S" : "C", head.c_str(),
                  static_cast<unsigned long long>(li.entity), static_cast<unsigned long long>(li.data));
        out += line;
        if (!why.empty())
        {
            out += "  " + why + "\n";
            continue;
        }
        if (faulted)
            out += "  (a read faulted and was caught - values may be partial)\n";
        for (size_t i = 0; i < fields.size(); ++i)
        {
            const InspectField& f = fields[i];
            char row[96];
            sprintf_s(row, "  %-28s %-22s ", f.name.c_str(), f.typeName.c_str());
            out += row + FieldValueText(f, probes[i], li.owner);
            if (probes[i].live)
                out += "   <- live (wired/overridden on the bus)";
            out += "\n";
        }
        out += WiredFields(li, fields);
        out += RecentWires(li.data, li.owner, 8);
    }
    return out + "blueprint:" + ownerNames;
}

// Text -> the bytes of a field's type. False with a reason when it can't be written that way.
static bool ParseFieldValue(const InspectField& f, const std::string& text, const unsigned char* current,
                            unsigned char* out, std::string& why)
{
    memset(out, 0, kFieldValueBytes);
    memcpy(out, current, kFieldValueBytes);                 // structs keep what isn't given
    const std::string& t = f.typeName;
    if (t == "Boolean")
    {
        if (text == "toggle") out[0] = current[0] ? 0 : 1;
        else if (text == "true" || text == "1") out[0] = 1;
        else if (text == "false" || text == "0") out[0] = 0;
        else { why = "a Boolean takes true, false or toggle"; return false; }
        return true;
    }
    if (t == "Float32" || t == "Float64")
    {
        char* end = nullptr;
        const double d = strtod(text.c_str(), &end);
        if (end == text.c_str()) { why = "not a number: " + text; return false; }
        if (t == "Float32") { const float fv = static_cast<float>(d); memcpy(out, &fv, 4); }
        else memcpy(out, &d, 8);
        return true;
    }
    if (f.kind == 8 || (f.kind >= 11 && f.kind <= 18))
    {
        char* end = nullptr;
        long long n = strtoll(text.c_str(), &end, 0);
        if (end == text.c_str() || *end != '\0')
        {
            bool named = false;
            if (f.kind == 8)
            {
                LockRegistry();
                if (const TypeRec* e = FindType(f.typeInfo))
                    for (const FieldRec& member : e->fields)
                        if (member.name == text) { n = static_cast<int32_t>(member.offset); named = true; break; }
                UnlockRegistry();
            }
            if (!named) { why = "not a whole number" + std::string(f.kind == 8 ? " or a member of " + t : "") + ": " + text; return false; }
        }
        memcpy(out, &n, f.size < 8 ? f.size : 8);
        return true;
    }
    if (t == "Vec2" || t == "Vec3" || t == "Vec4" || t == "Quat" || t == "LinearTransform")
    {
        const int want = t == "Vec2" ? 2 : t == "Vec3" || t == "LinearTransform" ? 3 : 4;
        float v[4] = {};
        std::string clean = text;
        for (char& c : clean) if (c == '(' || c == ')' || c == ',') c = ' ';
        const std::vector<std::string> parts = SplitWords(clean);
        if (static_cast<int>(parts.size()) != want) { why = t + " takes " + std::to_string(want) + " numbers" + (t == "LinearTransform" ? " (the position; rotation is kept)" : ""); return false; }
        for (int i = 0; i < want; ++i) v[i] = static_cast<float>(atof(parts[i].c_str()));
        memcpy(out + (t == "LinearTransform" ? 0x30 : 0), v, sizeof(float) * want);
        return true;
    }
    why = "writing a " + (t.empty() ? std::string("?") : t) + " isn't supported (numbers, Booleans, enums, vectors and positions are)";
    return false;
}

// write <Type[tag]>.<Field> = <value> @<blueprint> [S|C] [all]
static std::string WriteCommand(const std::string& rest)
{
    const char* usage = "usage: write <Type[tag]>.<Field> = <value> @<blueprint> [S|C] [all]";
    const size_t at = rest.find(" @");
    const size_t eq = rest.find('=');
    if (at == std::string::npos || eq == std::string::npos || eq > at)
        return usage;
    std::string spec = rest.substr(0, eq), text = rest.substr(eq + 1, at - eq - 1);
    auto trim = [](std::string& v) { while (!v.empty() && v.back() == ' ') v.pop_back(); while (!v.empty() && v[0] == ' ') v.erase(0, 1); };
    trim(spec);
    trim(text);
    std::string blueprint;
    int realm = -1;
    bool all = false;
    for (const std::string& t : SplitWords(rest.substr(at + 1)))
    {
        if (t[0] == '@') blueprint = t.substr(1);
        else if (t == "S" || t == "s" || t == "server") realm = 1;
        else if (t == "C" || t == "c" || t == "client") realm = 0;
        else if (t == "all") all = true;
    }
    const size_t dot = spec.rfind('.');
    if (dot == std::string::npos || blueprint.empty() || text.empty())
        return usage;
    const std::string instance = spec.substr(0, dot), fieldName = spec.substr(dot + 1);
    if (!g_traceInstalled && !g_propInstalled)
        return "write: turn a feed on first ('trace on' or 'prop on') - writes run inside its hook, on the game's logic thread";
    std::vector<LiveInstance> matches;
    std::string ownerNames;
    const std::string error = ResolveLiveInstances(instance, blueprint, realm, matches, ownerNames);
    if (!error.empty())
        return "write: " + error;
    if (realm < 0 && !all)                                   // default: the server side if there is one
    {
        const bool onServer = std::any_of(matches.begin(), matches.end(), [](const LiveInstance& m) { return m.realm == 1; });
        matches.erase(std::remove_if(matches.begin(), matches.end(),
                                     [onServer](const LiveInstance& m) { return m.realm != (onServer ? 1u : 0u); }),
                      matches.end());
    }
    if (!all)
        matches.resize(1);

    std::string out;
    for (const LiveInstance& li : matches)
    {
        std::vector<InspectField> fields = InspectFields(li.data);
        const InspectField* field = nullptr;
        for (const InspectField& f : fields)
            if (_stricmp(f.name.c_str(), fieldName.c_str()) == 0) { field = &f; break; }
        if (field == nullptr)
        {
            // Not reflected: a wire-only field (an Or's In1, a Math input...). Its type is the cache's.
            FieldProbe look = {};
            look.hash = EventIdFromText(fieldName);
            look.size = kFieldValueBytes;
            look.viaBus = true;
            uintptr_t outPeer = 0;
            uint32_t outPeerField = 0;
            FindOutputPeer(li, look.hash, &outPeer, &outPeerField);
            FieldJob find = {};
            find.bus = li.bus;
            find.data = li.data;
            find.probes = &look;
            find.count = 1;
            bool lookFaulted = false;
            std::string lookWhy = RunLogicJob(li.realm, RunFieldJob, &find, &lookFaulted);
            if (lookWhy.empty() && !lookFaulted && !look.live && outPeer != 0)
            {
                look.via = outPeer;                         // an output: its value is the cache its target reads
                look.hash = outPeerField;
                lookWhy = RunLogicJob(li.realm, RunFieldJob, &find, &lookFaulted);
            }
            if (!lookWhy.empty() || lookFaulted || !look.live || look.liveType == 0)
                return "write: " + TraceTypeName(li.data) + " has no field '" + fieldName + "' - not reflected and not wired on this bus ('inspect' lists both)";
            InspectField wired;
            wired.name = fieldName;
            wired.via = look.via;
            wired.viaField = look.via != 0 ? look.hash : 0;
            wired.offset = 0;
            wired.typeInfo = look.liveType;
            LockRegistry();
            const TypeRec* t = FindType(look.liveType);
            wired.typeName = t != nullptr ? t->name : TypeInfoName(look.liveType);
            wired.kind = t != nullptr ? t->kind : 0;
            wired.size = t != nullptr && t->size > 0 && t->size <= kFieldValueBytes ? t->size : 8;
            UnlockRegistry();
            fields.push_back(wired);
            field = &fields.back();
        }
        if (!FieldViaBus(*field) || field->kind == 7)
            return "write: " + field->name + " is a " + field->typeName + " - only numbers, Booleans, enums, vectors and positions can be written";

        // Read first (for toggle and to keep the rest of a struct), then write.
        FieldProbe probe = {};
        probe.hash = field->via != 0 ? field->viaField : EventIdFromText(field->name);
        probe.via = field->via;
        probe.offset = field->offset;
        probe.size = field->size;
        probe.typeInfo = field->typeInfo;
        probe.viaBus = true;
        FieldJob job = {};
        job.bus = li.bus;
        job.data = li.data;
        job.probes = &probe;
        job.count = 1;
        bool faulted = false;
        std::string why = RunLogicJob(li.realm, RunFieldJob, &job, &faulted);
        if (!why.empty() || faulted)
            return "write: " + (why.empty() ? std::string("reading the field faulted (caught), nothing written") : why);
        if (!probe.live)
            return "write: " + field->name + " isn't wired or overridden on this bus - the entity reads its EBX value (" +
                   FieldValueText(*field, probe, li.owner) + ") directly, so a live write can't reach it. "
                   "Change it in the EBX, or wire something to it.";
        unsigned char current[kFieldValueBytes];
        memcpy(current, probe.value, kFieldValueBytes);
        if (!ParseFieldValue(*field, text, current, job.newValue, why))
            return "write: " + why;
        job.write = true;
        memset(probe.value, 0, sizeof(probe.value));
        why = RunLogicJob(li.realm, RunFieldJob, &job, &faulted);
        if (!why.empty())
            return "write: " + why;
        if (faulted || probe.readFault || job.notWired)
            return "write: the write faulted (caught) - nothing more done";
        FieldProbe before = probe;
        memcpy(before.value, job.before, kFieldValueBytes);
        AcquireSRWLockExclusive(&g_formatLock);
        const std::string head = TraceTypeName(li.data) + TraceInstanceTag(li.data, li.owner);
        ReleaseSRWLockExclusive(&g_formatLock);
        const std::string line = std::string(li.realm ? "S " : "C ") + head + "." + field->name + ": " +
                                 FieldValueText(*field, before, li.owner) + " -> " + FieldValueText(*field, probe, li.owner);
        LogLine("write: %s", line.c_str());
        out += line + "\n";
    }
    return out + "(listeners notified; the wire's source overwrites it the next time it writes)" + ownerNames;
}

// The command entry points: serialised with the deferred-arming thread, and 'off'
// also cancels an arming that is still waiting.
static std::string InstallEventTrace(bool enable)
{
    if (!enable)
        InterlockedAnd(&g_feedWanted, ~1L);
    AcquireSRWLockExclusive(&g_feedLock);
    std::string result = InstallEventTraceUnlocked(enable);
    ReleaseSRWLockExclusive(&g_feedLock);
    return result;
}

static std::string InstallPropTrace(bool enable)
{
    if (!enable)
        InterlockedAnd(&g_feedWanted, ~2L);
    AcquireSRWLockExclusive(&g_feedLock);
    std::string result = InstallPropTraceUnlocked(enable);
    ReleaseSRWLockExclusive(&g_feedLock);
    return result;
}

// ---- crash dump of the feeds ----------------------------------------------------
// On the way down (TerminalFaultHandler) the feeds' last entries go to the module log, so
// a crash leaves "which logic ran last" next to the fault address. Runs on the faulting
// thread with the heap possibly broken: no allocation here - names are copied from engine
// memory into stack buffers, and the caches are only looked up, never built.

static void CrashCopyString(uintptr_t address, char* out, size_t size)
{
    size_t i = 0;
    for (; address >= 0x10000 && i + 1 < size; ++i)
    {
        char c = 0;
        if (!SafeRead(address + i, &c) || c == 0)
            break;
        out[i] = (c >= 32 && c < 127) ? c : '?';
    }
    out[i] = 0;
}

static void CrashTypeName(uintptr_t data, char* out, size_t size)
{
    uintptr_t classInfo = 0, typeInfoData = 0, namePtr = 0;
    out[0] = '?';
    out[1] = 0;
    if (data != 0 && SafeRead(data + 0x08, &classInfo) && classInfo != 0 &&
        SafeRead(classInfo, &typeInfoData) && typeInfoData != 0 && SafeRead(typeInfoData, &namePtr))
        CrashCopyString(namePtr, out, size);
    const size_t n = strlen(out);
    if (n > 10 && strcmp(out + n - 10, "EntityData") == 0)
        out[n - 10] = 0;
    else if (n > 4 && strcmp(out + n - 4, "Data") == 0)
        out[n - 4] = 0;
}

static void CrashInstanceTag(uintptr_t data, uintptr_t owner, char* out, size_t size)
{
    out[0] = 0;
    GUID guid{};
    if (data >= 0x10 && TraceHasGuid(data) && SafeRead(data - 0x10, &guid))
    {
        sprintf_s(out, size, "#%08x", guid.Data1);
        return;
    }
    auto block = g_traceOrdinals.find(owner);
    if (block == g_traceOrdinals.end())
        return;
    auto it = block->second.second.find(data);
    if (it != block->second.second.end())
        sprintf_s(out, size, "[%x]", it->second);
}

static const char* CrashHashName(uint32_t hash, char* buffer, size_t size)
{
    auto it = g_hashNames.find(hash);
    if (it != g_hashNames.end())
        return it->second.c_str();
    sprintf_s(buffer, size, "%08x", hash);
    return buffer;
}

static void CrashOwnerName(uintptr_t owner, char* out, size_t size)
{
    uintptr_t namePtr = 0;
    out[0] = 0;
    if (owner != 0 && SafeRead(owner + 0x18, &namePtr))
        CrashCopyString(namePtr, out, size);
}

static void FeedCrashDumpUnguarded()
{
    char source[96], target[96], sourceTag[48], targetTag[48], owner[192], e1[12], e2[12];
    if (g_trace != nullptr && g_traceNext > 0)
    {
        const LONG64 next = g_traceNext;
        const LONG64 count = next < 40 ? next : 40;
        LogLine("feed: last %lld logic events (trace %s)", count, g_traceInstalled ? "on" : "off");
        for (LONG64 index = next - count; index < next; ++index)
        {
            const TraceEntry& e = g_trace[index & (kTraceCapacity - 1)];
            if (e.seq != index + 1)
                continue;
            CrashTypeName(e.source, source, sizeof(source));
            CrashTypeName(e.target, target, sizeof(target));
            CrashInstanceTag(e.source, e.owner, sourceTag, sizeof(sourceTag));
            CrashInstanceTag(e.target, e.owner, targetTag, sizeof(targetTag));
            CrashOwnerName(e.owner, owner, sizeof(owner));
            LogLine("feed: %c %s%s.%s -> %s%s.%s%s  @%s", e.server ? 'S' : 'C',
                    source, sourceTag, CrashHashName(static_cast<uint32_t>(e.sourceEvent), e1, sizeof(e1)),
                    target, targetTag, CrashHashName(static_cast<uint32_t>(e.targetEvent), e2, sizeof(e2)),
                    (e.targetType == 4 || e.targetType == 5) ? " net" : "", owner);
        }
    }
    if (g_prop != nullptr && g_propNext > 0)
    {
        const LONG64 next = g_propNext;
        const LONG64 count = next < 24 ? next : 24;
        LogLine("feed: last %lld property writes (prop %s)", count, g_propInstalled ? "on" : "off");
        for (LONG64 index = next - count; index < next; ++index)
        {
            const PropEntry& e = g_prop[index & (kPropCapacity - 1)];
            if (e.seq != index + 1 || e.listenerCount == 0)
                continue;
            char type[64];
            uintptr_t typeInfoData = 0, namePtr = 0;
            type[0] = 0;
            if (e.valueType != 0 && SafeRead(e.valueType, &typeInfoData) && typeInfoData != 0 &&
                SafeRead(typeInfoData, &namePtr))
                CrashCopyString(namePtr, type, sizeof(type));
            float f[4];
            memcpy(f, e.value, sizeof(f));
            char value[96];
            if (strcmp(type, "Boolean") == 0)
                strcpy_s(value, e.value[0] ? "true" : "false");
            else if (strcmp(type, "Float32") == 0)
                sprintf_s(value, "%g", f[0]);
            else if (strcmp(type, "Vec3") == 0 || strcmp(type, "LinearTransform") == 0)
                sprintf_s(value, "(%g, %g, %g)", f[0], f[1], f[2]);
            else
            {
                uint64_t raw = 0;
                memcpy(&raw, e.value, sizeof(raw));
                sprintf_s(value, "0x%llX (%s)", static_cast<unsigned long long>(raw), type);
            }
            uintptr_t data = 0, ownerData = 0;
            if (PropListenerEntity(e.listener[0], &data, &ownerData))
            {
                CrashTypeName(data, target, sizeof(target));
                CrashInstanceTag(data, ownerData, targetTag, sizeof(targetTag));
                CrashOwnerName(ownerData, owner, sizeof(owner));
                LogLine("feed: %c -> %s%s.%s = %s%s  @%s", e.server ? 'S' : 'C', target, targetTag,
                        CrashHashName(e.field[0], e1, sizeof(e1)), value,
                        e.listenerCount > 1 ? " (+more listeners)" : "", owner);
            }
            else
                LogLine("feed: %c -> relay.%s = %s", e.server ? 'S' : 'C', CrashHashName(e.field[0], e1, sizeof(e1)), value);
        }
    }
}

static void FeedCrashDump()
{
    __try
    {
        FeedCrashDumpUnguarded();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LogLine("feed: the crash dump itself faulted");
    }
}


static std::string HandleCommand(const std::string& line)
{
    char verb[64] = {};
    char rest[4096] = {};
    sscanf_s(line.c_str(), "%63s %4095[^\n]", verb, static_cast<unsigned>(sizeof(verb)),
             rest, static_cast<unsigned>(sizeof(rest)));

    const std::string command(verb);

    if (command == "ping")
        return "pong gamelink";

    // help [verb] - every command with its one-line description (src/commands_table.inc), or just one.
    if (command == "help")
    {
        static const struct { const char* verb; const char* text; } kCommands[] = {
#include "commands_table.inc"
        };
        std::string out;
        for (const auto& c : kCommands)
            if (*rest == '\0' || strcmp(rest, c.verb) == 0)
                out += std::string(c.text) + "\n";
        return out.empty() ? std::string("help: no command called ") + rest : out;
    }

    // ticks - the server's frame counter (ServerGameContext +0x30 -> +0x0); sample twice to get the tick rate.
    if (command == "ticks")
    {
        uintptr_t context = 0, clock = 0;
        uint32_t ticks = 0;
        if (!SafeRead(g_base + kServerGameContextRva, &context) || context == 0 ||
            !SafeRead(context + 0x30, &clock) || clock == 0 || !SafeRead(clock, &ticks))
            return "ticks unavailable (no server running)";
        return "ticks " + std::to_string(ticks);
    }

    // ui open|close|toggle|status|tab <name>|test-focuslost - the F1 menu, for scripts and tests.
    if (command == "ui")
    {
        char sub[32] = {};
        char arg[64] = {};
        sscanf_s(rest, "%31s %63s", sub, static_cast<unsigned>(sizeof(sub)), arg, static_cast<unsigned>(sizeof(arg)));
        const std::string s(sub);
        if (s == "do")                           // ui do <control> <value>: what that widget does
        {
            char value[64] = {};
            sscanf_s(rest, "%*s %*s %63s", value, static_cast<unsigned>(sizeof(value)));
            return Menu_Do(arg, value);
        }
        if (s == "open") g_menu.Open();
        else if (s == "close") g_menu.Close();
        else if (s == "toggle") g_menu.Toggle();
        else if (s == "test-focuslost") g_menu.OnFocusLost();
        else if (s == "tab")
        {
            MenuTab tab;
            if (!MenuTabFromName(arg, &tab))
                return "ui: unknown tab (Camera World Inspector Logic Bots Diagnostics Advanced)";
            g_menu.SetTab(tab);
        }
        else if (s != "status" && !s.empty())
            return "usage: ui open|close|toggle|status|tab <name>|do <control> <value>|test-focuslost";
        if (!g_menu.IsOpen())
            return "ui closed";
        char out[200];
        sprintf_s(out, "ui open tab %s input %s (hooks %ld, scrubbed %ld)", MenuTabName(g_menu.Tab()),
                  g_inputHooksPatched > 0 ? "blocked" : "not blocked", static_cast<long>(g_inputHooksPatched),
                  static_cast<long>(g_menuScrubbed));
        return out;
    }

    // grabframe <file.bmp> - the presented frame from the game's own swap chain (see CaptureSwapChain). Not "capture":
    // that verb already installs the camera-transform detour further down.
    if (command == "grabframe")
        return CaptureCommand(rest);

    // typeat <hex-address> - parse a TypeInfo at a known address, bypassing the
    // registry entirely. The addresses come from Kyber's generated BFII headers.
    if (command == "typeat")
    {
        if (rest == nullptr || *rest == '\0')
            return "usage: typeat <hex-address>   (absolute, or +RVA)";

        uintptr_t address = 0;
        const char* digits = rest;
        bool relative = false;
        if (*digits == '+') { relative = true; ++digits; }
        else if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) digits += 2;
        if (sscanf_s(digits, "%llx", &address) != 1)
            return "could not parse the address";
        if (relative)
            address += g_base;

        TypeRec rec;
        ReadTypeInfo(address, 0, &rec);

        if (rec.name.empty())
            return "no readable TypeInfo there";

        char header[224];
        sprintf_s(header, "%s  size 0x%X  kind %d  %zu fields\n",
                  rec.name.c_str(), rec.size, rec.kind, rec.fields.size());

        std::string out(header);
        for (size_t i = 0; i < rec.fields.size(); ++i)
        {
            char line[288];
            sprintf_s(line, "  +0x%03X  %-34s  type 0x%llX\n", rec.fields[i].offset,
                      rec.fields[i].name.c_str(),
                      static_cast<unsigned long long>(rec.fields[i].typePtr));
            out += line;
        }
        return out;
    }

    // rewalk - force the type registry to be read again.
    if (command == "rewalk")
    {
        // Hold the lock across the whole rebuild: another thread may be part way
        // through FindType/FindInstances with a pointer into this vector, and
        // clearing it under them is what killed the process at 23:56.
        LockRegistry();
        g_registryWalked = false;
        std::vector<TypeRec> fresh;
        g_types.swap(fresh);
        const bool ok = WalkRegistryUnlocked();
        UnlockRegistry();

        if (!ok)
            return "registry walk failed again";

        char buffer[96];
        sprintf_s(buffer, "walked %zu types", g_types.size());
        return buffer;
    }

    // fields <TypeName> - the registered field table for one type: offset, name,
    // and the type pointer. Guessing at a struct layout is how the earlier spawn
    // faults happened; the registry knows, so ask it.
    if (command == "fields")
    {
        if (rest == nullptr || *rest == '\0')
            return "usage: fields <TypeName>";

        if (!WalkRegistry())
            return "registry walk failed";

        std::string wanted(rest);
        while (!wanted.empty() && (wanted.back() == ' ' || wanted.back() == '\r'))
            wanted.pop_back();

        const TypeRec* type = FindType(wanted);
        if (type == nullptr)
            return "no such type: " + wanted;

        char header[256];
        sprintf_s(header, "%s  size 0x%X  kind %d  %zu fields\n",
                  type->name.c_str(), type->size, type->kind, type->fields.size());

        std::string out(header);
        for (size_t i = 0; i < type->fields.size(); ++i)
        {
            const FieldRec& field = type->fields[i];
            char line[320];
            const TypeRec* fieldType = FindType(field.typePtr);
            sprintf_s(line, "  +0x%03X  %-32s  type 0x%llX  %s\n",
                      field.offset, field.name.c_str(),
                      static_cast<unsigned long long>(field.typePtr),
                      fieldType != nullptr ? fieldType->name.c_str() : "Unknown");
            out += line;
        }
        return out;
    }

    // entityfields <entity> - reflection fields for the selected entity's own
    // GameObjectData. Unlike `fields <T>`, identity comes from the live object.
    if (command == "entityfields")
    {
        if (rest == nullptr || *rest == '\0')
            return "usage: entityfields <entityAddress>";
        const uintptr_t entity = static_cast<uintptr_t>(strtoull(rest, nullptr, 0));
        uintptr_t data = 0;
        if (entity == 0 || !ReadEntityData(entity, &data))
            return "entityfields failed: no GameObjectData";
        if (!WalkRegistry())
            return "registry walk failed";
        const std::string typeName = EntityDataType(entity);
        const TypeRec* type = FindType(typeName);
        if (type == nullptr)
            return "entityfields failed: data type is not registered";

        char header[256];
        sprintf_s(header, "%s  size 0x%X  kind %d  %zu fields\n",
                  type->name.c_str(), type->size, type->kind, type->fields.size());
        std::string out(header);
        for (const FieldRec& field : type->fields)
        {
            char line[320];
            const TypeRec* fieldType = FindType(field.typePtr);
            sprintf_s(line, "  +0x%03X  %-32s  type 0x%llX  %s\n", field.offset,
                      field.name.c_str(), static_cast<unsigned long long>(field.typePtr),
                      fieldType != nullptr ? fieldType->name.c_str() : "Unknown");
            out += line;
        }
        return out;
    }

    // entityget/entityset target one live entity's GameObjectData, avoiding the
    // old `set <T>` behavior that intentionally updates every instance of a type.
    if (command == "entityget" || command == "entityset")
    {
        char addressText[64] = {};
        char fieldName[128] = {};
        char expectedGuid[64] = {};
        long long value = 0;
        unsigned width = 4;
        const int parsed = command == "entityget"
            ? sscanf_s(rest, "%63s %127s %u", addressText, static_cast<unsigned>(sizeof(addressText)),
                       fieldName, static_cast<unsigned>(sizeof(fieldName)), &width)
            : sscanf_s(rest, "%63s %127s %lld %u %63s", addressText, static_cast<unsigned>(sizeof(addressText)),
                       fieldName, static_cast<unsigned>(sizeof(fieldName)), &value, &width,
                       expectedGuid, static_cast<unsigned>(sizeof(expectedGuid)));
        if ((command == "entityget" && parsed < 2) || (command == "entityset" && parsed < 3))
            return command == "entityget" ? "usage: entityget <entity> <field> [bytes]"
                                          : "usage: entityset <entity> <field> <value> [bytes]";
        if (width != 1 && width != 4)
            return "byte width must be 1 or 4";

        const uintptr_t entity = static_cast<uintptr_t>(strtoull(addressText, nullptr, 0));
        uintptr_t data = 0;
        if (entity == 0 || !ReadEntityData(entity, &data))
            return "entity field access failed: no GameObjectData";
        if (!WalkRegistry())
            return "registry walk failed";
        const std::string typeName = EntityDataType(entity);
        if (command == "entityset" && parsed >= 5 && expectedGuid[0] != '\0')
        {
            const std::string actualGuid = EntityInstanceGuid(entity);
            if (actualGuid.empty() || _stricmp(actualGuid.c_str(), expectedGuid) != 0)
                return "entity field access failed: instance GUID changed";
        }
        uint32_t offset = 0;
        if (typeName.empty() || !ResolveField(typeName, fieldName, &offset))
            return "entity field access failed: field is not registered on the data type";

        uint32_t before = 0;
        if (!SafeRead(data + offset, &before))
            return "entity field access failed: field is unreadable";
        const uint32_t oldValue = width == 1 ? (before & 0xFF) : before;
        if (command == "entityget")
        {
            char line[256];
            sprintf_s(line, "%s::%s +0x%03X\n  0x%llX = %u\n", typeName.c_str(),
                      fieldName, offset, static_cast<unsigned long long>(entity), oldValue);
            return line;
        }

        const bool ok = width == 1 ? SafeWriteBytes(data + offset, &value, 1)
                                   : SafeWriteBytes(data + offset, &value, 4);
        if (!ok)
            return "entity field access failed: write rejected";
        char line[320];
        sprintf_s(line, "%s::%s +0x%03X on 0x%llX  %u -> %lld", typeName.c_str(),
                  fieldName, offset, static_cast<unsigned long long>(entity), oldValue, value);
        return line;
    }

    if (command == "types")
    {
        if (!WalkRegistry())
            return "registry walk failed";

        char buffer[128];
        sprintf_s(buffer, "types %zu", g_types.size());
        return buffer;
    }

    // worldinfo - does a session exist yet?
    //
    // This is the precondition question for the whole level-load path. Posting a
    // load-level message into a process with no world is the most likely reason a cold
    // frontend swallows it silently.
    if (command == "worldinfo")
    {
        uintptr_t clientWorld = 0, serverWorld = 0, clientEntityWorld = 0, gameContext = 0;
        SafeRead(g_base + kGameWorldArrayRva, &clientWorld);
        SafeRead(g_base + kGameWorldArrayRva + sizeof(uintptr_t), &serverWorld);
        SafeRead(g_base + kEntityWorldArrayRva, &clientEntityWorld);
        SafeRead(g_base + kClientGameContextRva, &gameContext);

        uintptr_t clientLevel = 0;
        if (gameContext != 0)
            SafeRead(gameContext + kClientGameContextLevelOffset, &clientLevel);

        // Server-side spawn indicators (RE-ANSWERS-OX.md B5.2): playerManager and peer
        // are set during the synchronous part of create(); if they are still null a few
        // seconds after spawnServer returns, the spawn failed early.
        uintptr_t serverContext = 0, serverPlayerManager = 0, serverPeer = 0;
        SafeRead(g_base + kServerGameContextRva, &serverContext);
        if (serverContext != 0)
        {
            SafeRead(serverContext + 0x58, &serverPlayerManager);
            SafeRead(serverContext + 0x60, &serverPeer);
        }

        char buffer[512];
        sprintf_s(buffer,
                  "gameWorld[client]=0x%llX gameWorld[server]=0x%llX entityWorld[client]=0x%llX "
                  "gameContext=0x%llX clientLevel=0x%llX renderView=0x%llX "
                  "serverPlayerManager=0x%llX serverPeer=0x%llX",
                  clientWorld, serverWorld, clientEntityWorld, gameContext, clientLevel,
                  GetRenderView(), serverPlayerManager, serverPeer);
        LogLine("%s", buffer);
        return buffer;
    }

    if (command == "camera")
    {
        Transform transform{};
        float fov = 0.0f;
        if (!ReadCamera(&transform, &fov))
            return "camera unavailable";

        char buffer[512];
        sprintf_s(buffer,
                  "camera pos %.6f %.6f %.6f left %.6f %.6f %.6f "
                  "up %.6f %.6f %.6f forward %.6f %.6f %.6f fovx %.7f",
                  transform.translation[0], transform.translation[1], transform.translation[2],
                  transform.left[0], transform.left[1], transform.left[2],
                  transform.up[0], transform.up[1], transform.up[2],
                  transform.forward[0], transform.forward[1], transform.forward[2], fov);
        return buffer;
    }

    // spl - inspect the frontend's own single-player level loader.
    //
    // LoadSinglePlayerLevelEntityData is what the game itself uses to start an offline
    // level: shipped instances live in UI/Frontend/Prefabs/ScreenLogic/
    // PF_UI_PlayScreen_InstantActionLogic and ..._SkirmishLogic, carrying LevelName,
    // GameMode, StartPoint and DifficultyIndex. It needs no matchmaking and no server
    // browser - it is the local path.
    //
    //   LevelName       +0x20  CString
    //   StartPoint      +0x28  CString
    //   GameMode        +0x30  CString
    //   DifficultyIndex +0x38  Int32
    //   SplitScreen     +0x3C  Boolean
    // input off | on - stop the game eating keyboard and mouse.
    //
    // Writing InputSettings did nothing in an earlier session, and the RE pass explains
    // why: UseMouseAndKeyboardSystem is consumed ONCE, when the input devices are built,
    // so flipping it later cannot un-create them. What those devices do is register for
    // RAW INPUT against the game's window (usages 0x02 mouse, 0x06 keyboard).
    //
    // Raw-input registration is per-process and can be revoked from inside, which is what
    // this does. No engine addresses involved.
    if (command == "input")
    {
        const bool enable = _strnicmp(rest, "on", 2) == 0;

        RAWINPUTDEVICE devices[2] = {};
        devices[0].usUsagePage = 0x01;
        devices[0].usUsage = 0x02;              // mouse
        devices[1].usUsagePage = 0x01;
        devices[1].usUsage = 0x06;              // keyboard

        if (enable)
        {
            // Re-registering needs the window the game uses; it owns the foreground one.
            HWND window = GetForegroundWindow();
            devices[0].dwFlags = 0;
            devices[1].dwFlags = 0;
            devices[0].hwndTarget = window;
            devices[1].hwndTarget = window;
        }
        else
        {
            devices[0].dwFlags = RIDEV_REMOVE;
            devices[1].dwFlags = RIDEV_REMOVE;
        }

        const BOOL ok = RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE));
        char buffer[160];
        sprintf_s(buffer, "input %s -> %s%s", enable ? "on" : "off",
                  ok ? "ok" : "FAILED", ok ? "" : " (raw input unchanged)");
        LogLine("%s", buffer);
        return buffer;
    }

    // entities [filter] - the outliner's data source.
    if (command == "entities")
    {
        const std::vector<EntityRow> rows = EnumerateEntities(4000);
        if (rows.empty())
            return "no entities (is a level loaded?)";

        const std::string filter(rest);
        std::string out;
        char line[512];
        int shown = 0;

        for (const EntityRow& row : rows)
        {
            if (!filter.empty() && row.name.find(filter) == std::string::npos)
                continue;

            const char* guid = row.instanceGuid.empty() ? "-" : row.instanceGuid.c_str();
            if (row.hasTransform)
                sprintf_s(line, "0x%llX %.1f %.1f %.1f %s %s\n",
                          row.entity, row.x, row.y, row.z, guid, row.name.c_str());
            else
                sprintf_s(line, "0x%llX - - - %s %s\n", row.entity, guid, row.name.c_str());

            out += line;
            ++shown;
        }

        sprintf_s(line, "%zu entities, %d shown\n", rows.size(), shown);
        LogLine("entities: %zu found, %d shown", rows.size(), shown);
        return line + out;
    }

    // move <entityAddress> <x> <y> <z> - place an entity, through the engine's setter.
    if (command == "move")
    {
        unsigned long long address = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        char expectedGuid[64] = {};
        const int parsed = sscanf_s(rest, "%llx %f %f %f %63s", &address, &x, &y, &z,
                                    expectedGuid, static_cast<unsigned>(sizeof(expectedGuid)));
        if (parsed < 4)
            return "usage: move <0xEntityAddress> <x> <y> <z>";

        if (parsed >= 5)
        {
            const std::string actualGuid = EntityInstanceGuid(static_cast<uintptr_t>(address));
            if (actualGuid.empty() || _stricmp(actualGuid.c_str(), expectedGuid) != 0)
                return "move rejected: instance GUID changed";
        }

        Transform transform{};
        if (!GuardedGetTransform(address, &transform))
            return "could not read that entity's transform - wrong address?";

        transform.translation[0] = x;
        transform.translation[1] = y;
        transform.translation[2] = z;

        if (!GuardedSetTransform(address, &transform))
            return "setTransform faulted";

        char buffer[160];
        sprintf_s(buffer, "moved 0x%llX to %.1f %.1f %.1f", address, x, y, z);
        LogLine("%s", buffer);
        return buffer;
    }

    // serverdump - raw qwords out of the ServerGameContext.
    //
    // Reason this exists: the RE pass expected an idle frontend to have no server
    // (peer and playerManager null, from a crash-window dump). A live frontend here
    // reports BOTH non-null, plus a server GameWorld. So a server already exists and
    // spawning a second one is very likely why spawnServer faults. This prints the
    // context so the state can be identified rather than guessed at.
    if (command == "serverdump")
    {
        uintptr_t context = 0;
        if (!SafeRead(g_base + kServerGameContextRva, &context) || context == 0)
            return "ServerGameContext is null";

        std::string out;
        char line[128];
        sprintf_s(line, "ServerGameContext = 0x%llX\n", context);
        out += line;

        for (int i = 0; i < 32; ++i)
        {
            uintptr_t value = 0;
            if (!SafeRead(context + i * sizeof(uintptr_t), &value))
                break;

            sprintf_s(line, "  +0x%02X = 0x%llX%s\n", i * 8, value,
                      (value > g_base && value < g_base + g_size) ? "  (module)" : "");
            out += line;
        }

        LogLine("serverdump: context 0x%llX", context);
        return out;
    }

    // spawn <level> [mode] - the supported route into a level from the frontend.
    // spawn - superseded and known to kill the process. See hookspawn.
    if (command == "spawn" && (rest == nullptr || strstr(rest, "--i-know") == nullptr))
    {
        return "refused: calling spawnServer directly from a live frontend crashes the "
               "process every time - the write lands in the torn-down server's allocation. "
               "Use 'hookspawn <level> [mode]' before the engine's own startup spawn "
               "instead; that is the route that works. Append --i-know to force this.";
    }

    if (command == "spawn")
    {
        char level[384] = {};
        char mode[128] = {};
        const int parsed = sscanf_s(rest, "%383s %127s",
                                    level, static_cast<unsigned>(sizeof(level)),
                                    mode, static_cast<unsigned>(sizeof(mode)));
        if (parsed < 1)
            return "usage: spawn <levelPath> [mode]";

        std::string status;
        SpawnServerWithLevel(level, parsed >= 2 ? mode : nullptr, &status);
        return status;
    }

    // gamesim - locate the GameSimulation instance without spawning anything.
    if (command == "gamesim")
    {
        const uintptr_t gs = FindGameSimulation();
        if (gs == 0)
            return "GameSimulation not found";

        char buffer[128];
        sprintf_s(buffer, "GameSimulation at 0x%llX", gs);
        return buffer;
    }

    // log [n] - the last n lines straight out of memory. The file is held open by
    // the game, so before this the only way to read a run was to close the game.
    if (command == "log")
    {
        int count = 60;
        if (rest != nullptr && *rest != '\0')
        {
            const int parsed = atoi(rest);
            if (parsed > 0)
                count = parsed;
        }
        return LogTail(count);
    }

    // hookspawn <level> [mode] - arm the vtable thunk so the engine's OWN startup
    // spawn loads our level instead of its own. Must be armed before the engine
    // reaches its spawn, which the init trace puts at about 9 seconds in.
    if (command == "hookspawn")
    {
        if (rest == nullptr || *rest == '\0')
            return "usage: hookspawn <level-path> [GameMode]";

        char level[512] = {};
        char mode[128] = {};
        const int parsed = sscanf_s(rest, "%511s %127s", level,
                                    static_cast<unsigned>(sizeof(level)),
                                    mode, static_cast<unsigned>(sizeof(mode)));
        if (parsed < 1)
            return "usage: hookspawn <level-path> [GameMode]";

        std::string status;
        InstallSpawnHook(level, parsed >= 2 ? mode : nullptr, &status);
        return status;
    }

    // drawtest <x> <y> <z> [size] [seconds] - a white wireframe box and a yellow
    // label at a world position. The first real use of DebugRenderer2, and the
    // thing selection rendering is built out of.
    if (command == "drawtest")
    {
        float x = 0.0f, y = 0.0f, z = 0.0f, size = 1.0f, seconds = 15.0f;
        const int parsed = sscanf_s(rest, "%f %f %f %f %f", &x, &y, &z, &size, &seconds);
        if (parsed < 3)
            return "usage: drawtest <x> <y> <z> [size] [seconds]";

        const float centre[3] = { x, y, z };
        return DrawTestMarker(centre, size, "drawtest\nDebugRenderer2", seconds);
    }

    // drawhere [size] [seconds] - the same marker, in front of the camera, so it
    // does not need coordinates guessed from nothing.
    if (command == "drawhere")
    {
        float size = 1.0f, seconds = 15.0f;
        sscanf_s(rest, "%f %f", &size, &seconds);

        Transform current{};
        float fov = 0.0f;
        if (!ReadCamera(&current, &fov))
            return "no render view - is a level loaded?";

        const float centre[3] = {
            current.translation[0] + current.forward[0] * 5.0f,
            current.translation[1] + current.forward[1] * 5.0f,
            current.translation[2] + current.forward[2] * 5.0f
        };

        LogLine("draw: marker at %.2f %.2f %.2f (camera %.2f %.2f %.2f)",
                centre[0], centre[1], centre[2],
                current.translation[0], current.translation[1], current.translation[2]);

        return DrawTestMarker(centre, size, "drawhere\nDebugRenderer2", seconds);
    }

    // search <text> [max] - every registered type or field whose name contains
    // the text. The registry has known all 23,221 types since this module was
    // written and there has never been a way to ask it a question in English -
    // which is what you need when hunting for whatever gates a round starting.
    if (command == "search")
    {
        if (rest == nullptr || *rest == '\0')
            return "usage: search <text> [max]";

        char needle[128] = {};
        unsigned cap = 60;
        if (sscanf_s(rest, "%127s %u", needle, static_cast<unsigned>(sizeof(needle)), &cap) < 1)
            return "usage: search <text> [max]";
        if (cap < 1) cap = 1;
        if (cap > 400) cap = 400;

        if (!WalkRegistry())
            return "registry walk failed";

        std::string out;
        unsigned found = 0;

        for (size_t t = 0; t < g_types.size() && found < cap; ++t)
        {
            const TypeRec& type = g_types[t];
            const bool typeMatches = StrContainsNoCase(type.name.c_str(), needle);

            if (typeMatches)
            {
                char line[256];
                sprintf_s(line, "%s  (type, size 0x%X, %zu fields)\n",
                          type.name.c_str(), type.size, type.fields.size());
                out += line;
                ++found;
            }

            for (size_t f = 0; f < type.fields.size() && found < cap; ++f)
            {
                if (!StrContainsNoCase(type.fields[f].name.c_str(), needle))
                    continue;

                char line[320];
                sprintf_s(line, "%s::%s  +0x%03X\n", type.name.c_str(),
                          type.fields[f].name.c_str(), type.fields[f].offset);
                out += line;
                ++found;
            }
        }

        if (found == 0)
            return std::string("nothing matches '") + needle + "'";
        return out;
    }

    // get <Type> <Field> [bytes] - read a field back from every live instance.
    if (command == "get")
    {
        char typeName[128] = {};
        char fieldName[128] = {};
        unsigned width = 4;

        const int parsed = sscanf_s(rest, "%127s %127s %u",
                                    typeName, static_cast<unsigned>(sizeof(typeName)),
                                    fieldName, static_cast<unsigned>(sizeof(fieldName)),
                                    &width);
        if (parsed < 2)
            return "usage: get <Type> <Field> [bytes: 1|4]";

        if (!WalkRegistry())
            return "registry walk failed";

        const TypeRec* type = FindType(typeName);
        if (type == nullptr)
            return std::string("no such type: ") + typeName;

        uint32_t offset = 0;
        if (!ResolveField(typeName, fieldName, &offset))
            return std::string("no field ") + typeName + "::" + fieldName;

        const std::vector<uintptr_t> instances = FindInstances(*type);
        if (instances.empty())
            return std::string("no live instances of ") + typeName;

        char header[224];
        sprintf_s(header, "%s::%s +0x%03X across %zu instance(s)\n",
                  typeName, fieldName, offset, instances.size());
        std::string out(header);

        for (size_t i = 0; i < instances.size(); ++i)
        {
            uint32_t value = 0;
            if (!SafeRead(instances[i] + offset, &value))
                continue;

            char line[160];
            sprintf_s(line, "  0x%llX = %u%s\n",
                      static_cast<unsigned long long>(instances[i]),
                      width == 1 ? (value & 0xFF) : value,
                      (width == 1 && (value & 0xFF) != 0) ? "  (set)" : "");
            out += line;
        }
        return out;
    }

    // set <Type> <Field> <value> [bytes] - write a number into every live
    // instance of a type. This is how viewmode already works; generalising it
    // means settings can be changed without a rebuild each time.
    if (command == "set")
    {
        char typeName[128] = {};
        char fieldName[128] = {};
        long long value = 0;
        unsigned width = 4;

        const int parsed = sscanf_s(rest, "%127s %127s %lld %u",
                                    typeName, static_cast<unsigned>(sizeof(typeName)),
                                    fieldName, static_cast<unsigned>(sizeof(fieldName)),
                                    &value, &width);
        if (parsed < 3)
            return "usage: set <Type> <Field> <value> [bytes: 1|4]";
        if (width != 1 && width != 4)
            return "byte width must be 1 or 4";

        if (!WalkRegistry())
            return "registry walk failed";

        const TypeRec* type = FindType(typeName);
        if (type == nullptr)
            return std::string("no such type: ") + typeName;

        uint32_t offset = 0;
        if (!ResolveField(typeName, fieldName, &offset))
            return std::string("no field ") + typeName + "::" + fieldName;

        const std::vector<uintptr_t> instances = FindInstances(*type);
        if (instances.empty())
            return std::string("no live instances of ") + typeName;

        int written = 0;
        std::string detail;
        for (size_t i = 0; i < instances.size(); ++i)
        {
            uint32_t before = 0;
            SafeRead(instances[i] + offset, &before);

            const bool ok = (width == 1)
                ? SafeWriteBytes(instances[i] + offset, &value, 1)
                : SafeWriteBytes(instances[i] + offset, &value, 4);
            if (!ok)
                continue;

            ++written;
            char line[192];
            sprintf_s(line, "  0x%llX  %u -> %lld\n",
                      static_cast<unsigned long long>(instances[i]),
                      width == 1 ? (before & 0xFF) : before, value);
            detail += line;
        }

        LogLine("set %s::%s +0x%03X = %lld in %d of %zu instances",
                typeName, fieldName, offset, value, written, instances.size());

        char header[224];
        sprintf_s(header, "%s::%s +0x%03X = %lld in %d of %zu instances\n",
                  typeName, fieldName, offset, value, written, instances.size());
        return std::string(header) + detail;
    }

    // activecam <0|1> [TypeName] - point the camera view manager's active-camera
    // slot at a live camera of the named type. Defaults to SpectatorFreeCamera.
    if (command == "activecam")
    {
        int enable = 1;
        char typeName[128] = {};
        const int parsed = sscanf_s(rest, "%d %127s", &enable, typeName,
                                    static_cast<unsigned>(sizeof(typeName)));
        if (parsed < 1)
            return "usage: activecam <0|1> [TypeName]";

        return ActivateSpectatorFreeCamera(enable != 0,
                                           parsed >= 2 ? typeName : "SpectatorFreeCamera");
    }

    // poke <hex-address> <hex-qword> - write one qword. General-purpose, so an
    // idea can be tested without a rebuild.
    if (command == "poke")
    {
        char addressText[64] = {}, valueText[64] = {};
        if (sscanf_s(rest, "%63s %63s", addressText, static_cast<unsigned>(sizeof(addressText)),
                     valueText, static_cast<unsigned>(sizeof(valueText))) < 2)
            return "usage: poke <hex-address> <hex-qword>";

        uintptr_t address = 0, value = 0;
        const char* a = addressText;
        if (a[0] == '0' && (a[1] == 'x' || a[1] == 'X')) a += 2;
        const char* v = valueText;
        if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) v += 2;
        if (sscanf_s(a, "%llx", &address) != 1 || sscanf_s(v, "%llx", &value) != 1)
            return "could not parse the address or value";

        uintptr_t before = 0;
        SafeRead(address, &before);
        if (!SafeWriteBytes(address, &value, sizeof(value)))
            return "write failed";

        LogLine("poke 0x%llX: 0x%llX -> 0x%llX", static_cast<unsigned long long>(address),
                static_cast<unsigned long long>(before),
                static_cast<unsigned long long>(value));

        char buffer[160];
        sprintf_s(buffer, "0x%llX was 0x%llX, now 0x%llX",
                  static_cast<unsigned long long>(address),
                  static_cast<unsigned long long>(before),
                  static_cast<unsigned long long>(value));
        return buffer;
    }

    // spectate <0|1> - arm BFII's own spectator mode, which is where the shipped
    // free camera lives. Arm it before the level loads.
    if (command == "spectate")
    {
        int enable = 1;
        if (rest != nullptr && *rest != '\0')
            sscanf_s(rest, "%d", &enable);

        if (enable == 0)
        {
            g_spectateWanted = false;
            WriteSettingByName("ClientSettings", "IsSpectator", 0, 1);
            return "spectator mode disarmed";
        }

        if (g_spectateWanted)
            return "already arming spectator mode";

        g_spectateWanted = true;
        HANDLE thread = CreateThread(nullptr, 0, SpectateThread, nullptr, 0, nullptr);
        if (thread == nullptr)
        {
            g_spectateWanted = false;
            return "could not start the spectate thread";
        }
        CloseHandle(thread);

        // Report the first attempt straight away so a failure is visible now
        // rather than only in the log.
        const int immediate = WriteSettingByName("ClientSettings", "IsSpectator", 1, 1);
        char buffer[224];
        if (immediate == -1)
            sprintf_s(buffer, "armed - the registry is not readable yet, retrying every 500 ms");
        else if (immediate < 0)
            sprintf_s(buffer, "armed - ClientSettings::IsSpectator not resolvable yet (code %d)",
                      immediate);
        else
            sprintf_s(buffer, "armed - IsSpectator written to %d instance(s) already", immediate);
        return buffer;
    }

    // viewer <0|1> - arm the camera takeover. Set this before the level loads and
    // the view is ours from the first frame, rather than after the match has had
    // a chance to point it somewhere.
    if (command == "viewer")
    {
        int enable = 1;
        if (rest != nullptr && *rest != '\0')
            sscanf_s(rest, "%d", &enable);

        g_viewerWanted = (enable != 0);
        LogLine("viewer: takeover %s", g_viewerWanted ? "armed" : "disarmed");
        return g_viewerWanted ? "viewer armed - the camera will be taken when a level loads"
                              : "viewer disarmed";
    }

    // levelname - which level is actually loaded right now.
    if (command == "levelname")
    {
        uintptr_t gameContext = 0, clientLevel = 0;
        SafeRead(g_base + kClientGameContextRva, &gameContext);
        if (gameContext != 0)
            SafeRead(gameContext + kClientGameContextLevelOffset, &clientLevel);

        if (clientLevel == 0)
            return "no level is loaded";

        char levelName[256] = {};
        if (!ReadLoadedLevelName(clientLevel, levelName, sizeof(levelName)))
            return "a level is loaded but its name could not be read";

        return std::string("loaded level: ") + levelName;
    }

    // hookstatus - has the engine reached its spawn yet?
    if (command == "hookstatus")
    {
        char buffer[256];
        sprintf_s(buffer, "armed=%d fired=%d level='%s' mode='%s'",
                  g_spawnHookArmed ? 1 : 0, g_spawnHookFired ? 1 : 0,
                  g_hookLevelName, g_hookModeName);
        return buffer;
    }

    // refs <hex-address|"text"> [max] - every qword in the process that holds
    // this pointer, or a pointer to this string.
    //
    // The registry-based instance search does not find GameSettings - the
    // addresses it returns are 0x20 apart for a 0xB0 type, so it is matching a
    // table of type pointers rather than objects. Working backwards from a value
    // we know is live is more reliable than guessing at object shapes: find the
    // current level path string, then find everything that points at it. One of
    // those holders is the settings object.
    if (command == "refs")
    {
        if (rest == nullptr || *rest == '\0')
            return "usage: refs <hex-address> [max]   or   refs \"some text\" [max]";

        uintptr_t wanted = 0;
        unsigned cap = 32;
        std::string describe;

        if (*rest == '"')
        {
            // Locate the text first, then report what points at it.
            char text[192] = {};
            size_t length = 0;
            const char* p = rest + 1;
            while (*p != '\0' && *p != '"' && length + 1 < sizeof(text))
                text[length++] = *p++;
            if (*p == '"')
                ++p;
            sscanf_s(p, "%u", &cap);

            if (length == 0)
                return "empty search text";

            const uintptr_t found = FindTextInMemory(text);
            if (found == 0)
                return std::string("text not found in memory: ") + text;

            wanted = found;
            char note[288];
            sprintf_s(note, "'%s' lives at 0x%llX\n", text,
                      static_cast<unsigned long long>(found));
            describe = note;
        }
        else
        {
            char addressText[64] = {};
            if (sscanf_s(rest, "%63s %u", addressText,
                         static_cast<unsigned>(sizeof(addressText)), &cap) < 1)
                return "usage: refs <hex-address> [max]";

            const char* digits = addressText;
            bool relative = false;
            if (*digits == '+') { relative = true; ++digits; }
            else if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) digits += 2;
            if (sscanf_s(digits, "%llx", &wanted) != 1)
                return "could not parse the address";
            if (relative)
                wanted += g_base;
        }

        if (cap < 1) cap = 1;
        if (cap > 256) cap = 256;

        std::string out = describe;
        char header[128];
        sprintf_s(header, "holders of 0x%llX:\n", static_cast<unsigned long long>(wanted));
        out += header;

        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t address = 0;
        uintptr_t slots[256];
        unsigned reported = 0;

        while (address < 0x200000000000ull && reported < cap &&
               VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == sizeof(mbi))
        {
            const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            const uintptr_t regionEnd = regionBase + mbi.RegionSize;

            const bool writable =
                (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE)) != 0;

            if (mbi.State == MEM_COMMIT && writable &&
                (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0)
            {
                const size_t hits = ScanForValue(regionBase, regionEnd, wanted,
                                                 slots, cap - reported);
                for (size_t i = 0; i < hits && reported < cap; ++i)
                {
                    const bool inImage = slots[i] >= g_base && slots[i] < g_base + g_size;
                    char line[192];
                    if (inImage)
                        sprintf_s(line, "  game+0x%llX\n",
                                  static_cast<unsigned long long>(slots[i] - g_base));
                    else
                        sprintf_s(line, "  0x%llX\n",
                                  static_cast<unsigned long long>(slots[i]));
                    out += line;
                    ++reported;
                }
            }

            if (regionEnd <= regionBase)
                break;
            address = regionEnd;
        }

        if (reported == 0)
            out += "  (nothing points at it)\n";
        return out;
    }

    // gamesettings - every live GameSettings instance with its string fields
    // resolved. Four instances exist and only one of them is the live one; the
    // level path it holds is what identifies it. This is one command rather than
    // a find followed by four obj calls with hand-copied addresses.
    if (command == "gamesettings")
    {
        if (!WalkRegistry())
            return "registry walk failed";

        const TypeRec* type = FindType("GameSettings");
        if (type == nullptr)
            return "GameSettings is not in the registry";

        const std::vector<uintptr_t> found = FindInstances(*type, 16);
        if (found.empty())
            return "no GameSettings instances found";

        std::string out;
        for (size_t i = 0; i < found.size(); ++i)
        {
            char header[96];
            sprintf_s(header, "GameSettings 0x%llX\n",
                      static_cast<unsigned long long>(found[i]));
            out += header;

            for (size_t f = 0; f < type->fields.size(); ++f)
            {
                const FieldRec& field = type->fields[f];

                uintptr_t value = 0;
                if (!SafeRead(found[i] + field.offset, &value))
                    continue;

                char text[192] = {};
                if (value == 0 || !RawReadString(text, sizeof(text), value, 191) ||
                    text[0] == '\0')
                    continue;

                char line[320];
                sprintf_s(line, "    %-34s '%s'\n", field.name.c_str(), text);
                out += line;
            }
        }
        return out;
    }

    // find <TypeName> [max] - addresses of live instances of a registered type.
    //
    // The registry has known every type in the process since this module was
    // written, but there has been no way to ask where the instances are. The
    // engine-driven route into a level runs through GameSettings and the client
    // state machine, and both have to be located before they can be read.
    if (command == "find")
    {
        if (rest == nullptr || *rest == '\0')
            return "usage: find <TypeName> [max]";

        char typeName[128] = {};
        unsigned cap = 16;
        const int parsed = sscanf_s(rest, "%127s %u", typeName,
                                    static_cast<unsigned>(sizeof(typeName)), &cap);
        if (parsed < 1)
            return "usage: find <TypeName> [max]";
        if (cap < 1) cap = 1;
        if (cap > 64) cap = 64;

        if (!WalkRegistry())
            return "registry walk failed";

        const TypeRec* type = FindType(typeName);
        if (type == nullptr)
            return std::string("no such type: ") + typeName;

        const std::vector<uintptr_t> found = FindInstances(*type, cap);
        char header[192];
        sprintf_s(header, "%s  size 0x%X  %zu instance(s)\n",
                  type->name.c_str(), type->size, found.size());

        std::string out(header);
        for (size_t i = 0; i < found.size(); ++i)
        {
            char line[64];
            sprintf_s(line, "  0x%llX\n", static_cast<unsigned long long>(found[i]));
            out += line;
        }
        if (found.empty())
            out += "  (none found - the type may not be instantiated yet)\n";
        return out;
    }

    // obj <TypeName> <hex-address> - the field table of a type applied to a live
    // object: name, offset, and the value actually there. `fields` gives the
    // layout; this gives the contents.
    if (command == "obj")
    {
        if (rest == nullptr || *rest == '\0')
            return "usage: obj <TypeName> <hex-address>";

        char typeName[128] = {};
        char addressText[64] = {};
        if (sscanf_s(rest, "%127s %63s", typeName, static_cast<unsigned>(sizeof(typeName)),
                     addressText, static_cast<unsigned>(sizeof(addressText))) < 2)
            return "usage: obj <TypeName> <hex-address>";

        uintptr_t address = 0;
        const char* digits = addressText;
        bool relative = false;
        if (*digits == '+') { relative = true; ++digits; }
        else if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) digits += 2;
        if (sscanf_s(digits, "%llx", &address) != 1)
            return "could not parse the address";
        if (relative)
            address += g_base;

        if (!WalkRegistry())
            return "registry walk failed";

        const TypeRec* type = FindType(typeName);
        if (type == nullptr)
            return std::string("no such type: ") + typeName;

        char header[192];
        sprintf_s(header, "%s at 0x%llX  size 0x%X\n", type->name.c_str(),
                  static_cast<unsigned long long>(address), type->size);

        std::string out(header);
        for (size_t i = 0; i < type->fields.size(); ++i)
        {
            const FieldRec& field = type->fields[i];
            uintptr_t value = 0;
            const bool readable = SafeRead(address + field.offset, &value);

            char line[384];
            if (!readable)
            {
                sprintf_s(line, "  +0x%03X  %-32s  <unreadable>\n", field.offset,
                          field.name.c_str());
                out += line;
                continue;
            }

            // If it points at printable text, show the text - most of what we
            // care about on these objects is level paths and mode names.
            char text[96] = {};
            const bool isText = value != 0 &&
                                RawReadString(text, sizeof(text), value, 95) &&
                                text[0] != '\0';

            if (isText)
                sprintf_s(line, "  +0x%03X  %-32s  0x%llX  '%s'\n", field.offset,
                          field.name.c_str(), static_cast<unsigned long long>(value), text);
            else
                sprintf_s(line, "  +0x%03X  %-32s  0x%llX\n", field.offset,
                          field.name.c_str(), static_cast<unsigned long long>(value));
            out += line;
        }
        return out;
    }

    // findserver - locate the Server object by the pointers it shares with the
    // ServerGameContext (OX B10.5). Read-only; a wrong candidate costs nothing.
    if (command == "findserver")
        return FindServerObject();

    // watch - start the post-spawn state trace by hand, for loads we did not start
    // ourselves (a match begun from the menu, for instance).
    if (command == "watch")
    {
        if (g_spawnWatchRunning)
            return "already watching";
        StartSpawnWatch();
        return "watching for 120 s - see the watch lines in the log";
    }

    // peek <hex-address> [bytes] - read live memory and return it as hex plus qwords.
    //
    // A dump is 8 GB and an hour of someone's day; most reverse-engineering questions
    // need thirty-two bytes. Every read is guarded, so a bad address costs nothing.
    if (command == "peek")
    {
        char addressText[64] = {};
        unsigned length = 64;
        const int parsed = sscanf_s(rest, "%63s %u", addressText, static_cast<unsigned>(sizeof(addressText)), &length);
        if (parsed < 1)
            return "usage: peek <hex-address> [bytes]  (address may be absolute or +RVA)";

        uintptr_t address = 0;
        if (addressText[0] == '+')
            address = g_base + strtoull(addressText + 1, nullptr, 16);
        else
            address = strtoull(addressText, nullptr, 16);

        if (length == 0 || length > 1024)
            length = 64;

        std::vector<unsigned char> bytes(length);
        unsigned readable = 0;
        for (; readable < length; ++readable)
        {
            if (!SafeRead(address + readable, &bytes[readable]))
                break;
        }

        if (readable == 0)
            return "unreadable";

        std::string out;
        char line[256];
        sprintf_s(line, "0x%llX, %u of %u bytes readable\n", address, readable, length);
        out += line;

        for (unsigned offset = 0; offset < readable; offset += 16)
        {
            sprintf_s(line, "+%03X ", offset);
            out += line;

            const unsigned span = (readable - offset < 16) ? readable - offset : 16;
            for (unsigned i = 0; i < span; ++i)
            {
                sprintf_s(line, "%02X ", bytes[offset + i]);
                out += line;
            }
            out += " |";
            for (unsigned i = 0; i < span; ++i)
            {
                const unsigned char c = bytes[offset + i];
                out += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.';
            }
            out += "|\n";
        }

        // Qwords too - pointers and vtable slots are what these reads are usually for.
        for (unsigned offset = 0; offset + 8 <= readable; offset += 8)
        {
            unsigned long long value = 0;
            memcpy(&value, &bytes[offset], 8);
            sprintf_s(line, "q+%03X 0x%016llX%s\n", offset, value,
                      (value > g_base && value < g_base + 0x10000000ULL) ? "  (in module)" : "");
            out += line;
        }

        return out;
    }

    // dump <path> - snapshot the process as it stands.
    if (command == "dump")
    {
        char path[400] = {};
        if (sscanf_s(rest, "%399s", path, static_cast<unsigned>(sizeof(path))) != 1)
            return "usage: dump <path>";

        std::string status;
        WriteFullDump(path, &status);
        return status;
    }

    // repro <level> [mode] - post the level load that kills the process, then dump
    // inside the crash window.
    //
    // The handler faults about 0.6 s later on a null m_level, so this waits 300 ms and
    // captures the queued message and the torn state in flight. The process is expected
    // to die shortly afterwards; that is the point.
    if (command == "repro")
    {
        char level[384] = {};
        char mode[128] = {};
        const int parsed = sscanf_s(rest, "%383s %127s",
                                    level, static_cast<unsigned>(sizeof(level)),
                                    mode, static_cast<unsigned>(sizeof(mode)));
        if (parsed < 1)
            return "usage: repro <levelPath> [mode]";

        LogLine("repro: posting the load that is expected to fault");
        std::string loadStatus;
        LoadLevel(level, parsed >= 2 ? mode : nullptr, &loadStatus);
        LogLine("repro: post said '%s'", loadStatus.c_str());

        Sleep(300);

        std::string dumpStatus;
        WriteFullDump("C:\\ProgramData\\FrostyGameLink\\crashwindow.dmp", &dumpStatus);
        return "repro: " + loadStatus + " | " + dumpStatus;
    }

    // splhold <level> [mode] | splhold off
    if (command == "splhold")
    {
        if (_strnicmp(rest, "off", 3) == 0)
        {
            g_holdLevelActive = false;
            LogLine("level hold OFF");
            return "level hold off";
        }

        char level[384] = {};
        char mode[128] = {};
        const int parsed = sscanf_s(rest, "%383s %127s",
                                    level, static_cast<unsigned>(sizeof(level)),
                                    mode, static_cast<unsigned>(sizeof(mode)));
        if (parsed < 1)
            return "usage: splhold <levelPath> [mode] | splhold off";

        if (!WalkRegistry())
            return "registry walk failed";

        strncpy_s(g_holdLevelName, level, _TRUNCATE);
        strncpy_s(g_holdLevelMode, parsed >= 2 ? mode : "", _TRUNCATE);
        g_holdLevelHasMode = parsed >= 2;
        g_holdLevelActive = true;

        LogLine("level hold ON: '%s'%s", g_holdLevelName,
                g_holdLevelHasMode ? (std::string(" mode '") + g_holdLevelMode + "'").c_str() : "");

        return std::string("holding level override: ") + g_holdLevelName +
               " - start a match from the game's menu and it should load this instead";
    }

    if (command == "spl" || command == "splset")
    {
        if (!WalkRegistry())
            return "registry walk failed";

        const TypeRec* type = FindType("LoadSinglePlayerLevelEntityData");
        if (type == nullptr)
            return "LoadSinglePlayerLevelEntityData is not in the registry";

        const std::vector<uintptr_t> instances = FindInstances(*type, 32);
        if (instances.empty())
            return "no LoadSinglePlayerLevelEntityData instances live right now";

        uint32_t levelOff = 0, modeOff = 0;
        ResolveField("LoadSinglePlayerLevelEntityData", "LevelName", &levelOff);
        ResolveField("LoadSinglePlayerLevelEntityData", "GameMode", &modeOff);

        std::string out;
        char line[512];

        // Validate before writing.
        //
        // The instance finder matches on [vtable in module][ClassInfo*], which is strong
        // but not perfect - a read-only pass over six matches produced:
        //
        //   0xED8F1640 level='Levels/InitialExperience/InitialExperience_01' mode='SkirmishOnslaught'
        //   0xED942A50 level='' mode='Mode8'
        //   0x344CD488 level=' %sD' mode='\xFF\xFFPD'          <- not a loader at all
        //
        // The first two match the shipped frontend prefabs exactly. The third is a false
        // positive, and writing a pointer into it is what killed the process: the write
        // lands in whatever that object really is.
        //
        // A real loader's strings are either null or printable ASCII. Anything else is
        // rejected rather than written.
        // One implementation, shared with the hold thread - two copies of a rule this
        // fiddly is how the empty-string bug survived as long as it did.
        auto looksLikeLoader = [&](uintptr_t instance) -> bool
        {
            return LooksLikeLoader(instance, levelOff, modeOff);
        };

        if (command == "splset")
        {
            char level[384] = {};
            char mode[128] = {};
            const int parsed = sscanf_s(rest, "%383s %127s",
                                        level, static_cast<unsigned>(sizeof(level)),
                                        mode, static_cast<unsigned>(sizeof(mode)));
            if (parsed < 1)
                return "usage: splset <levelPath> [mode]";

            strncpy_s(g_levelNameBuffer, level, _TRUNCATE);
            strncpy_s(g_levelModeBuffer, parsed >= 2 ? mode : "", _TRUNCATE);

            char* namePtr = g_levelNameBuffer;
            char* modePtr = g_levelModeBuffer;
            int written = 0;
            int skipped = 0;

            for (uintptr_t instance : instances)
            {
                if (!looksLikeLoader(instance))
                {
                    LogLine("splset: SKIPPED 0x%llX - does not look like a level loader", instance);
                    ++skipped;
                    continue;
                }

                if (SafeWriteBytes(instance + levelOff, &namePtr, sizeof(namePtr)))
                    ++written;
                if (parsed >= 2)
                    SafeWriteBytes(instance + modeOff, &modePtr, sizeof(modePtr));

                LogLine("splset: wrote 0x%llX", instance);
            }

            sprintf_s(line, "splset: '%s' written to %d instance(s), %d skipped as unsafe",
                      g_levelNameBuffer, written, skipped);
            LogLine("%s", line);
            return line;
        }

        for (uintptr_t instance : instances)
        {
            uintptr_t levelPtr = 0, modePtr = 0;
            SafeRead(instance + levelOff, &levelPtr);
            SafeRead(instance + modeOff, &modePtr);

            const std::string levelText = SafeCString(levelPtr);
            const std::string modeText = SafeCString(modePtr);

            sprintf_s(line, "  0x%llX level='%s' mode='%s'\n",
                      instance, levelText.c_str(), modeText.c_str());
            out += line;

            // Also to the module log: the reply goes to the editor pane, and when the
            // process dies that pane is the only copy - which is exactly the evidence
            // that goes missing at the worst moment.
            LogLine("spl:   0x%llX level='%s' mode='%s'",
                    instance, levelText.c_str(), modeText.c_str());
        }

        sprintf_s(line, "%zu instance(s):\n", instances.size());
        LogLine("spl: %zu instance(s)", instances.size());
        return std::string(line) + out;
    }

    if (command == "loadlevel")
    {
        char level[384] = {};
        char mode[128] = {};
        const int parsed = sscanf_s(rest, "%383s %127s",
                                    level, static_cast<unsigned>(sizeof(level)),
                                    mode, static_cast<unsigned>(sizeof(mode)));
        if (parsed < 1)
            return "usage: loadlevel <levelPath> [mode]";

        std::string status;
        if (!LoadLevel(level, parsed >= 2 ? mode : nullptr, &status))
            return "loadlevel failed: " + status;

        return status;
    }

    // camlock 1|0 - take or release the render view.
    //
    // Taking it snapshots the live transform first, so the view does not jump: you get
    // exactly what was on screen, then it stops moving.
    if (command == "camlock")
    {
        int enable = 0;
        if (sscanf_s(rest, "%d", &enable) != 1)
            return "usage: camlock <0|1>";

        if (enable != 0)
        {
            Transform current{};
            float fov = 0.0f;
            if (!ReadCamera(&current, &fov))
                return "camlock failed: no render view";

            EnterCriticalSection(&g_cameraLock);
            g_cameraTarget = current;          // keep the engine's own basis verbatim
            LeaveCriticalSection(&g_cameraLock);

            g_cameraOverride = true;
            LogLine("camera override ON at %.2f %.2f %.2f",
                    current.translation[0], current.translation[1], current.translation[2]);
            return "camlock on";
        }

        g_cameraOverride = false;
        LogLine("camera override OFF");
        return "camlock off";
    }

    // watchdrive <0|1> - drive the camera from the breakpoint handler itself.
    // The trap fires AFTER the engine's write, so ours lands last by ordering
    // rather than by luck - which is exactly what every thread-based attempt
    // could not achieve.
    // coords <0|1> - on-screen camera position and heading for play-testing; the camera stays the player's.
    // trace on [filter] | trace off | trace [n] | trace filter [text] | trace clear
    // trace addr <0|1> | trace guid <0|1|2> | trace where <0|1>
    // The live logic-event feed; see InstallEventTrace. Reading returns events since the
    // last read, filtered case-insensitively on the printed line: type name, instance
    // guid, event hex, or the owning blueprint's path.
    if (command == "trace")
    {
        char sub[32] = {};
        char arg[256] = {};
        sscanf_s(rest, "%31s %255[^\n]", sub, static_cast<unsigned>(sizeof(sub)),
                 arg, static_cast<unsigned>(sizeof(arg)));
        const std::string action(sub);

        if (action == "on")
        {
            const std::string result = InstallEventTrace(true);
            g_traceFilter = arg;
            return result;
        }
        if (action == "off")
            return InstallEventTrace(false);
        if (action == "filter")
        {
            g_traceFilter = arg;
            return g_traceFilter.empty() ? std::string("filter cleared") : "filter: " + g_traceFilter;
        }
        if (action == "clear")
        {
            g_traceRead = g_traceNext;
            g_traceDropped = 0;
            return "trace buffer skipped to now";
        }
        if (action == "status")
        {
            AcquireSRWLockExclusive(&g_formatLock);     // the reader's summary line, nothing consumed
            const std::string header = ReadEventTrace(0);
            ReleaseSRWLockExclusive(&g_formatLock);
            return header;
        }
        if (action == "addr")
        {
            g_traceAddresses = atoi(arg) != 0;
            return g_traceAddresses ? "trace lines include data addresses" : "trace addresses off";
        }
        if (action == "pause" || action == "resume")
        {
            g_feedsPaused = action == "pause";
            return g_feedsPaused ? "both feeds paused - buffers frozen, hooks still in; 'trace resume' to carry on"
                                 : "both feeds recording again";
        }
        if (action == "dump")
        {
            FeedCrashDump();                // what a crash would write; read it with 'log 80'
            return LogTail(70);
        }
        if (action == "guid")
        {
            g_traceGuids = arg[0] != '\0' ? atoi(arg) : 1;
            return g_traceGuids == 0 ? "instance ids off"
                 : g_traceGuids == 1 ? "short instance ids: #Data1 (exported) or [ordinal]"
                                     : "full instance guids (Frosty placeholder form for non-exported)";
        }
        if (action == "where")
        {
            g_traceWhere = arg[0] == '\0' || atoi(arg) != 0;
            return g_traceWhere ? "lines end with @<owning blueprint>" : "owning blueprint off";
        }

        int count = 300;
        if (action == "read" && arg[0] != '\0')
            count = atoi(arg);
        else if (!action.empty() && action != "read")
            count = atoi(sub);
        if (count <= 0)
            count = 300;
        if (count > 5000)
            count = 5000;
        AcquireSRWLockExclusive(&g_formatLock);
        const std::string events = ReadEventTrace(count);
        ReleaseSRWLockExclusive(&g_formatLock);
        return events;
    }

    // budget - unique meshes against MeshStreamingManager's fixed 2,048, sub-levels, entities.
    if (command == "budget")
        return BudgetReport();

    // fire / deliver <Type[tag]>.<Event> @<blueprint> [S|C] [all] - see FireCommand.
    if (command == "fire" || command == "deliver")
        return FireCommand(rest, command == "deliver");

    // inspect <Type[tag]> @<blueprint> [S|C] - every field as the live entity sees it now
    // (bus value where wired/overridden, else EBX), plus its recent wires. See InspectCommand.
    if (command == "inspect")
        return InspectCommand(rest);

    // write <Type[tag]>.<Field> = <value> @<blueprint> [S|C] [all] - a wired field's value,
    // written through the engine's PropertyWriter so its listeners react. See WriteCommand.
    if (command == "write")
        return WriteCommand(rest);

    // wires [--events|--props] <blueprint filter> - every connection of the matching loaded
    // blueprints, in feed-line form (see WiresList). Read-only; fine at any time.
    if (command == "wires")
    {
        std::string filter(rest);
        bool events = true, props = true;
        if (filter.compare(0, 8, "--events") == 0) { props = false; filter.erase(0, 8); }
        else if (filter.compare(0, 7, "--props") == 0) { events = false; filter.erase(0, 7); }
        while (!filter.empty() && filter.front() == ' ')
            filter.erase(filter.begin());
        if (filter.empty())
            return "usage: wires [--events|--props] <blueprint filter>   (e.g. wires GameModes_Flow)";
        LoadHashNames();
        AcquireSRWLockExclusive(&g_formatLock);
        const std::string listing = WiresList(filter, events, props);
        ReleaseSRWLockExclusive(&g_formatLock);
        return listing;
    }

    // overlay on [filter] | overlay off | overlay filter [text] | overlay feed events|props|both
    // The newest feed lines drawn over the game (see the overlay section). Needs the feeds on.
    if (command == "overlay")
    {
        char sub[32] = {};
        char arg[256] = {};
        sscanf_s(rest, "%31s %255[^\n]", sub, static_cast<unsigned>(sizeof(sub)),
                 arg, static_cast<unsigned>(sizeof(arg)));
        const std::string action(sub);
        if (action == "on")
        {
            const std::string err = InstallPresentHook();
            if (!err.empty())
                return "overlay: " + err;
            AcquireSRWLockExclusive(&g_formatLock);
            g_overlayFilter = arg;
            ReleaseSRWLockExclusive(&g_formatLock);
            g_overlayFailed = false;
            g_overlayOn = true;
            g_overlayKeysArmed = true;             // F9 overlay, F10 pause from now on
            return std::string("overlay on") + (g_traceInstalled || g_propInstalled ? "" : " (no feed is on yet - 'trace on' / 'prop on')");
        }
        if (action == "off")
        {
            g_overlayOn = false;
            return "overlay off";
        }
        if (action == "filter")
        {
            AcquireSRWLockExclusive(&g_formatLock);
            g_overlayFilter = arg;
            ReleaseSRWLockExclusive(&g_formatLock);
            return g_overlayFilter.empty() ? std::string("overlay filter cleared") : "overlay filter: " + g_overlayFilter;
        }
        if (action == "feed")
        {
            const std::string which(arg);
            g_overlayFeeds = which == "events" ? 1 : which == "props" ? 2 : 3;
            return "overlay shows " + std::string(g_overlayFeeds == 1 ? "events" : g_overlayFeeds == 2 ? "properties" : "events and properties");
        }
        return std::string("overlay is ") + (g_overlayOn ? (g_overlayFailed ? "on but failed (see log)" : "on") : "off") +
               " - usage: overlay on [filter] | off | filter <f> | feed events|props|both";
    }

    // prop on [filter] | prop off | prop [n] | prop filter [text] | prop clear
    // The live property feed (values arriving at listeners); see InstallPropTrace.
    // Shares 'trace guid', 'trace where' and 'trace addr' with the event feed.
    if (command == "prop")
    {
        char sub[32] = {};
        char arg[256] = {};
        sscanf_s(rest, "%31s %255[^\n]", sub, static_cast<unsigned>(sizeof(sub)),
                 arg, static_cast<unsigned>(sizeof(arg)));
        const std::string action(sub);

        if (action == "on")
        {
            const std::string result = InstallPropTrace(true);
            g_propFilter = arg;
            return result;
        }
        if (action == "off")
            return InstallPropTrace(false);
        if (action == "filter")
        {
            g_propFilter = arg;
            return g_propFilter.empty() ? std::string("prop filter cleared") : "prop filter: " + g_propFilter;
        }
        if (action == "clear")
        {
            g_propRead = g_propNext;
            g_propDropped = 0;
            return "prop buffer skipped to now";
        }
        if (action == "status")
        {
            AcquireSRWLockExclusive(&g_formatLock);     // the reader's summary line, nothing consumed
            const std::string header = ReadPropTrace(0);
            ReleaseSRWLockExclusive(&g_formatLock);
            return header;
        }

        int count = 300;
        if (action == "read" && arg[0] != '\0')
            count = atoi(arg);
        else if (!action.empty() && action != "read")
            count = atoi(sub);
        if (count <= 0)
            count = 300;
        if (count > 5000)
            count = 5000;
        AcquireSRWLockExclusive(&g_formatLock);
        const std::string props = ReadPropTrace(count);
        ReleaseSRWLockExclusive(&g_formatLock);
        return props;
    }

    if (command == "coords")
    {
        int enable = 1;
        if (rest != nullptr && *rest != '\0')
            sscanf_s(rest, "%d", &enable);
        return CoordsOverlay(enable != 0);
    }

    if (command == "watchdrive")
    {
        int enable = 1;
        if (rest != nullptr && *rest != '\0')
            sscanf_s(rest, "%d", &enable);
        return DriveViaWatch(enable != 0);
    }

    // watchwrite <hex-address|renderview> [bytes] - set a hardware write
    // breakpoint and log the instruction that writes it. This names the camera's
    // writer outright instead of guessing at it, which is what the last six
    // attempts were doing.
    if (command == "watchwrite")
    {
        char target[64] = {};
        unsigned length = 4;
        if (sscanf_s(rest, "%63s %u", target, static_cast<unsigned>(sizeof(target)),
                     &length) < 1)
            return "usage: watchwrite <hex-address|renderview> [1|2|4|8]";

        uintptr_t address = 0;
        if (_stricmp(target, "renderview") == 0)
        {
            address = GetRenderView();
            if (address == 0)
                return "no render view - is a level loaded?";
        }
        else
        {
            const char* digits = target;
            if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) digits += 2;
            if (sscanf_s(digits, "%llx", &address) != 1)
                return "could not parse the address";
        }

        g_watchAddress = address;
        g_watchHits = 0;

        const int armed = ArmHardwareWatch(address, length);
        if (armed < 0)
            return "could not enumerate threads";

        LogLine("watch: armed on 0x%llX (%u bytes) across %d threads",
                static_cast<unsigned long long>(address), length, armed);

        char buffer[256];
        sprintf_s(buffer, "watching writes to 0x%llX across %d threads - "
                          "the writer's address goes to the log within a frame or two",
                  static_cast<unsigned long long>(address), armed);
        return buffer;
    }

    // watchrw <hex-address> [1|2|4|8] - read/write watch; every distinct accessing instruction is logged once
    // ("watchrw site #n ... accessed by game+0x..."), the watch stays armed until watchoff. Threads created
    // after arming are not covered (debug registers are per thread).
    if (command == "watchrw")
    {
        char target[64] = {};
        unsigned length = 4;
        if (sscanf_s(rest, "%63s %u", target, static_cast<unsigned>(sizeof(target)), &length) < 1)
            return "usage: watchrw <hex-address> [1|2|4|8]";
        const char* digits = target;
        if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) digits += 2;
        uintptr_t address = 0;
        if (sscanf_s(digits, "%llx", &address) != 1)
            return "could not parse the address";

        g_watchSiteCount = 0;
        for (LONG i = 0; i < kMaxWatchSites; ++i)
            g_watchSites[i] = 0;
        g_watchHits = 0;
        g_watchUnique = true;
        g_watchAddress = address;
        const int armed = ArmHardwareWatch(address, length, true);
        if (armed < 0)
            return "could not enumerate threads";
        LogLine("watchrw: armed on 0x%llX (%u bytes) across %d threads",
                static_cast<unsigned long long>(address), length, armed);
        char buffer[200];
        sprintf_s(buffer, "watchrw armed on 0x%llX across %d threads", static_cast<unsigned long long>(address), armed);
        return buffer;
    }

    // frametimes [seconds=10] - Present-to-Present intervals over the last N seconds of frames (installs the Present
    // hook on first use): frames, fps, avg / p50 / p95 / p99 / max ms, frames over 33 / 50 / 100 ms.
    if (command == "frametimes")
    {
        if (!g_presentHooked)
        {
            const std::string err = InstallPresentHook();
            if (!g_presentHooked)
                return "frametimes: Present hook not installed: " + err;
            return "frametimes: Present hook installed - ask again in a few seconds";
        }
        const LONG64 n = g_frameIndex;
        int rawCount = 0;
        if (sscanf_s(rest, "raw %d", &rawCount) == 1)      // frametimes raw <n>: the last n frames in ms, oldest first
        {
            if (rawCount > kFrameRing) rawCount = kFrameRing;
            std::string out = "frames";
            char one[16];
            for (LONG64 i = (n - rawCount < 0 ? 0 : n - rawCount); i < n; ++i)
            {
                sprintf_s(one, " %.2f", g_frameMs[i % kFrameRing]);
                out += one;
            }
            return out;
        }
        double seconds = 10.0;
        if (rest != nullptr && *rest != '\0')
            sscanf_s(rest, "%lf", &seconds);
        std::vector<float> v;
        double total = 0.0;
        for (LONG64 i = n - 1; i >= 0 && i >= n - kFrameRing; --i)
        {
            const float ms = g_frameMs[i % kFrameRing];
            if (total + ms > seconds * 1000.0)
                break;
            total += ms;
            v.push_back(ms);
        }
        if (v.empty())
            return "frametimes: no frames yet";
        std::vector<float> s = v;
        std::sort(s.begin(), s.end());
        auto pct = [&](double p) -> float {
            size_t k = static_cast<size_t>(p * (s.size() - 1));
            if (k > s.size() - 1) k = s.size() - 1;
            return s[k];
        };
        int over33 = 0, over50 = 0, over100 = 0;
        for (float ms : v) { over33 += ms > 33.4f; over50 += ms > 50.0f; over100 += ms > 100.0f; }
        char buf[320];
        sprintf_s(buf, "frametimes over %.1f s: %zu frames, %.1f fps, avg %.2f ms, p50 %.2f, p95 %.2f, p99 %.2f, max %.2f ms; "
                       ">33ms %d, >50ms %d, >100ms %d",
                  total / 1000.0, v.size(), v.size() * 1000.0 / total, total / v.size(), pct(0.5), pct(0.95), pct(0.99),
                  s.back(), over33, over50, over100);
        return buf;
    }

    // poolgrow [status] - see PoolGrowInstall.
    if (command == "poolgrow")
    {
        if (rest != nullptr && _strnicmp(rest, "status", 6) == 0)
        {
            char buf[220];
            sprintf_s(buf, "poolgrow %s: %ld allocations, %ld from overflow pools, %ld refused, %ld overflow pools created",
                      g_poolGrowOn ? "on" : "off", g_poolCalls, g_poolOverflowed, g_poolRefused, g_poolCreated);
            return buf;
        }
        return PoolGrowInstall();
    }

    // exitwatch - log the stack of whoever calls NtTerminateProcess (see ExitWatchInstall).
    if (command == "exitwatch")
        return ExitWatchInstall();

    // poolfix [status] - see PoolFixInstall. "poolfix" installs; "poolfix status" reports calls / refusals.
    if (command == "poolfix")
    {
        if (rest != nullptr && _strnicmp(rest, "owners", 6) == 0)
        {
            std::string out = "owners seen by the caller stub:\n";
            char line[64];
            for (int i = 0; i < 64; ++i)
                if (g_poolOwners[i] != 0)
                {
                    sprintf_s(line, "  0x%llX\n", static_cast<unsigned long long>(g_poolOwners[i]));
                    out += line;
                }
            return out;
        }
        if (rest != nullptr && _strnicmp(rest, "status", 6) == 0)
        {
            char buf[160];
            sprintf_s(buf, "poolfix %s: %ld allocations, %ld refused (pool full)", g_poolFixOn ? "on" : "off",
                      g_poolCalls, g_poolRefused);
            return buf;
        }
        return PoolFixInstall();
    }

    // findvt <vtable: absolute hex or +RVA> [max] - objects whose first qword is that vtable (private writable
    // memory below 8 GiB, like `find`). For runtime classes that carry no ClassInfo, e.g. engine singletons.
    if (command == "findvt")
    {
        char target[64] = {};
        unsigned cap = 16;
        if (sscanf_s(rest, "%63s %u", target, static_cast<unsigned>(sizeof(target)), &cap) < 1)
            return "usage: findvt <hex-vtable|+RVA> [max]";
        if (cap < 1) cap = 1;
        if (cap > 256) cap = 256;
        const char* digits = target;
        const bool rva = digits[0] == '+';
        if (rva) ++digits;
        if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) digits += 2;
        uintptr_t vtable = 0;
        if (sscanf_s(digits, "%llx", &vtable) != 1)
            return "could not parse the vtable";
        if (rva) vtable += g_base;

        std::vector<uintptr_t> found;
        static uintptr_t batch[256];
        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t address = 0;
        while (found.size() < cap && VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == sizeof(mbi))
        {
            const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            const uintptr_t regionEnd = regionBase + mbi.RegionSize;
            const bool usable = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
                                (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) != 0 &&
                                (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;
            if (usable && regionBase < 0x200000000ull)
            {
                const size_t got = ScanRegionForQword(regionBase, regionEnd, vtable, batch, cap - found.size());
                for (size_t i = 0; i < got; ++i)
                    found.push_back(batch[i]);
            }
            address = regionEnd;
            if (address == 0)
                break;
        }
        char line[96];
        sprintf_s(line, "vtable 0x%llX: %zu object(s)\n", static_cast<unsigned long long>(vtable), found.size());
        std::string out(line);
        for (uintptr_t f : found)
        {
            sprintf_s(line, "  0x%llX\n", static_cast<unsigned long long>(f));
            out += line;
        }
        return out;
    }

    // patchcode <hex-address|+RVA> <hex bytes, no spaces> - write bytes into code (VirtualProtect, write,
    // restore, FlushInstructionCache). Logs the bytes it replaced.
    if (command == "patchcode")
    {
        char target[64] = {}, hex[513] = {};
        if (sscanf_s(rest, "%63s %512s", target, static_cast<unsigned>(sizeof(target)), hex, static_cast<unsigned>(sizeof(hex))) < 2)
            return "usage: patchcode <hex-address|+RVA> <hexbytes>";
        const char* digits = target;
        const bool rva = digits[0] == '+';
        if (rva) ++digits;
        if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) digits += 2;
        uintptr_t address = 0;
        if (sscanf_s(digits, "%llx", &address) != 1)
            return "could not parse the address";
        if (rva) address += g_base;
        unsigned char bytes[256];
        const size_t n = strlen(hex) / 2;
        if (n == 0 || n > sizeof(bytes) || strlen(hex) % 2 != 0)
            return "hex bytes: an even number of hex digits, at most 512";
        for (size_t i = 0; i < n; ++i)
        {
            unsigned v = 0;
            if (sscanf_s(hex + 2 * i, "%2x", &v) != 1)
                return "bad hex";
            bytes[i] = static_cast<unsigned char>(v);
        }
        unsigned char before[256] = {};
        SafeReadBytes(address, before, n);
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(address), n, PAGE_EXECUTE_READWRITE, &old))
            return "VirtualProtect failed";
        const bool ok = SafeWriteBytes(address, bytes, n);
        DWORD ignored = 0;
        VirtualProtect(reinterpret_cast<void*>(address), n, old, &ignored);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), n);
        std::string was;
        char b2[4];
        for (size_t i = 0; i < n; ++i) { sprintf_s(b2, "%02X", before[i]); was += b2; }
        LogLine("patchcode 0x%llX (%zu bytes) was %s -> %s %s", static_cast<unsigned long long>(address), n,
                was.c_str(), hex, ok ? "ok" : "FAILED");
        return std::string(ok ? "patched, was " : "write failed, was ") + was;
    }

    // threads [stackBytes] - where is every thread right now? Each thread is suspended only long enough to copy
    // its registers and the top of its stack into fixed buffers (no allocation while anything is suspended - a
    // suspended thread may hold the heap lock); formatting happens after all are resumed. One line per thread,
    // busiest (CPU time) first: rip, then up to 16 return-address candidates into the game module.
    if (command == "threads")
    {
        unsigned stackBytes = 0x1800;
        if (rest != nullptr && *rest != '\0')
            sscanf_s(rest, "%x", &stackBytes);
        if (stackBytes < 0x100) stackBytes = 0x100;
        if (stackBytes > 0x8000) stackBytes = 0x8000;

        struct ThreadSample
        {
            DWORD id;
            unsigned long long cpu100ns;
            uintptr_t rip, rsp;
            int retCount;
            uintptr_t rets[16];
            bool ok;
        };
        std::vector<DWORD> ids;
        {
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot == INVALID_HANDLE_VALUE)
                return "could not enumerate threads";
            THREADENTRY32 entry;
            entry.dwSize = sizeof(entry);
            const DWORD process = GetCurrentProcessId();
            if (Thread32First(snapshot, &entry))
                do
                {
                    if (entry.th32OwnerProcessID == process && entry.th32ThreadID != GetCurrentThreadId())
                        ids.push_back(entry.th32ThreadID);
                } while (Thread32Next(snapshot, &entry));
            CloseHandle(snapshot);
        }
        std::vector<ThreadSample> samples(ids.size());
        static uintptr_t stackCopy[0x8000 / 8];
        for (size_t i = 0; i < ids.size(); ++i)
        {
            ThreadSample& s = samples[i];
            s = ThreadSample{};
            s.id = ids[i];
            HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, FALSE, ids[i]);
            if (thread == nullptr)
                continue;
            FILETIME c, e, k, u;
            if (GetThreadTimes(thread, &c, &e, &k, &u))
                s.cpu100ns = (static_cast<unsigned long long>(k.dwHighDateTime) << 32 | k.dwLowDateTime) +
                             (static_cast<unsigned long long>(u.dwHighDateTime) << 32 | u.dwLowDateTime);
            if (SuspendThread(thread) != static_cast<DWORD>(-1))
            {
                CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                if (GetThreadContext(thread, &context))
                {
                    s.ok = true;
                    s.rip = static_cast<uintptr_t>(context.Rip);
                    s.rsp = static_cast<uintptr_t>(context.Rsp);
                    const size_t got = CopyStackWords(s.rsp, stackCopy, stackBytes / 8);
                    for (size_t w = 0; w < got && s.retCount < 16; ++w)
                        if (stackCopy[w] >= g_base && stackCopy[w] < g_base + g_size)
                            s.rets[s.retCount++] = stackCopy[w];
                }
                ResumeThread(thread);
            }
            CloseHandle(thread);
        }
        std::sort(samples.begin(), samples.end(),
                  [](const ThreadSample& a, const ThreadSample& b) { return a.cpu100ns > b.cpu100ns; });
        std::string out;
        char line[256];
        sprintf_s(line, "%zu threads\n", samples.size());
        out += line;
        for (const ThreadSample& s : samples)
        {
            if (!s.ok)
                continue;
            char where[160];
            DescribeAddress(where, sizeof(where), s.rip);
            sprintf_s(line, "tid %lu cpu %.1fs rip %s :", s.id, s.cpu100ns / 1e7, where);
            out += line;
            for (int r = 0; r < s.retCount; ++r)
            {
                sprintf_s(line, " +%llX", static_cast<unsigned long long>(s.rets[r] - g_base));
                out += line;
            }
            out += "\n";
        }
        return out;
    }

    // watchoff - disarm.
    if (command == "watchoff")
    {
        g_watchUnique = false;
        g_watchAddress = 0;
        const int armed = ArmHardwareWatch(0, 4);
        char buffer[128];
        sprintf_s(buffer, "watch cleared on %d threads after %ld hits", armed, g_watchHits);
        return buffer;
    }

    // capture <0|1> [drive] - detour getActiveCameraTransform so the ORIGINAL still
    // runs, then inspect the pose it produced. This is what distinguishes "the
    // function is never called" from "its output is ignored" - the outright
    // replacement could not tell those apart. With drive on, the pose is
    // overwritten with ours after the original has filled it in.
    if (command == "capture")
    {
        int enable = 1, drive = 0;
        const int parsed = sscanf_s(rest, "%d %d", &enable, &drive);
        if (parsed < 1)
            enable = 1;
        return InstallCameraCapture(enable != 0, drive != 0);
    }

    // camhook <0|1> - replace getActiveCameraTransform with a stub that hands the
    // engine our pose. Nothing to find, nothing to race: we answer the question
    // the engine asks every frame.
    if (command == "camhook")
    {
        int enable = 1;
        if (rest != nullptr && *rest != '\0')
            sscanf_s(rest, "%d", &enable);
        return InstallCameraHook(enable != 0);
    }

    // camauto - find the camera and start driving it, without anyone having to
    // copy an address. Each candidate is nudged and put back; the one the render
    // view follows is kept.
    if (command == "camauto")
    {
        if (g_camAutoRunning)
            return "already sweeping - watch the log, or 'camautoresult' when it finishes";

        g_camAutoRunning = true;
        g_camAutoResult[0] = '\0';

        HANDLE thread = CreateThread(nullptr, 0, AutoDriveCameraThread, nullptr, 0, nullptr);
        if (thread == nullptr)
        {
            g_camAutoRunning = false;
            return "could not start the sweep thread";
        }
        CloseHandle(thread);

        return "sweeping on a background thread - the port stays live. "
               "Ask 'camautoresult' for the outcome, or read the log.";
    }

    // camautoresult - the outcome of the last sweep.
    if (command == "camautoresult")
    {
        if (g_camAutoRunning)
            return "still sweeping";
        if (g_camAutoResult[0] == '\0')
            return "no sweep has run yet";
        return g_camAutoResult;
    }

    // camto <x> <y> <z> - put the driven camera somewhere directly. The deploy
    // screen parks the view about 3 km below the map; flying that on WASD is not
    // a viewer, so jump instead.
    if (command == "camto")
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        if (sscanf_s(rest, "%f %f %f", &x, &y, &z) != 3)
            return "usage: camto <x> <y> <z>";

        if (!g_cameraDrive && !g_watchDrive)
            return "not driving a camera - run watchdrive 1 first";

        EnterCriticalSection(&g_cameraLock);
        g_cameraTarget.translation[0] = x;
        g_cameraTarget.translation[1] = y;
        g_cameraTarget.translation[2] = z;
        g_cameraTarget.translation[3] = 1.0f;
        LeaveCriticalSection(&g_cameraLock);

        memcpy(g_watchPose, &g_cameraTarget, sizeof(g_cameraTarget));
        LogLine("camera: moved to %.2f %.2f %.2f", x, y, z);

        char buffer[128];
        sprintf_s(buffer, "camera at %.2f %.2f %.2f", x, y, z);
        return buffer;
    }

    // camwhere - where the driven camera currently is, so you can find your way
    // back to somewhere worth looking at.
    if (command == "camwhere")
    {
        if (!g_cameraDrive)
            return "not driving a camera";

        EnterCriticalSection(&g_cameraLock);
        Transform copy = g_cameraTarget;
        LeaveCriticalSection(&g_cameraLock);

        char buffer[224];
        sprintf_s(buffer, "at %.2f %.2f %.2f  forward %.3f %.3f %.3f  (driving 0x%llX)",
                  copy.translation[0], copy.translation[1], copy.translation[2],
                  copy.forward[0], copy.forward[1], copy.forward[2],
                  static_cast<unsigned long long>(g_activeCamera));
        return buffer;
    }

    // camfind [max] - every place in the process holding the render view's current
    // transform. One of them is Camera::m_transform; the addresses are stable
    // enough to write to once identified.
    if (command == "camfind")
    {
        unsigned cap = 24;
        if (rest != nullptr && *rest != '\0')
            sscanf_s(rest, "%u", &cap);
        if (cap < 1) cap = 1;
        if (cap > 64) cap = 64;
        return FindCameraByTransform(cap);
    }

    // camdrive <hex-address> - drive the transform at an address found by camfind,
    // rather than one guessed from a layout.
    if (command == "camdrive")
    {
        if (rest == nullptr || *rest == '\0')
            return "usage: camdrive <hex-address>   (0 to release)";

        uintptr_t address = 0;
        const char* digits = rest;
        if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) digits += 2;
        if (sscanf_s(digits, "%llx", &address) != 1)
            return "could not parse the address";

        if (address == 0)
        {
            g_cameraDrive = false;
            return "released";
        }

        Transform current{};
        if (!SafeRead(address, &current))
            return "that address is unreadable";

        g_activeCamera = address;
        g_cameraTransformOffset = 0;

        EnterCriticalSection(&g_cameraLock);
        g_cameraTarget = current;
        LeaveCriticalSection(&g_cameraLock);

        g_cameraDrive = true;
        LogLine("camera: driving raw address 0x%llX from %.2f %.2f %.2f",
                static_cast<unsigned long long>(address), current.translation[0],
                current.translation[1], current.translation[2]);

        char buffer[256];
        sprintf_s(buffer, "driving 0x%llX at %.2f %.2f %.2f - WASD with the game focused",
                  static_cast<unsigned long long>(address), current.translation[0],
                  current.translation[1], current.translation[2]);
        return buffer;
    }

    // camtake <0|1> - drive the view by writing Camera::m_transform, which is the
    // member the engine's own per-frame feed reads. Replaces the RenderView
    // writer, which fought the engine and mirrored the view.
    if (command == "camtake")
    {
        int enable = 1;
        if (rest != nullptr && *rest != '\0')
            sscanf_s(rest, "%d", &enable);
        return TakeCamera(enable != 0);
    }

    // camobj - locate the active camera and report where its transform sits,
    // without writing anything.
    if (command == "camobj")
    {
        std::string how;
        const uintptr_t camera = FindActiveCamera(&how);
        if (camera == 0)
            return std::string("camera not found: ") + how;

        size_t offset = kCameraTransformDefault;
        std::string calibration;
        const bool pinned = CalibrateCameraTransform(camera, &offset, &calibration);

        Transform current{};
        SafeRead(camera + offset, &current);

        char buffer[512];
        sprintf_s(buffer,
                  "camera 0x%llX\n  %s\n  transform +0x%zX (%s)\n"
                  "  left     %.4f %.4f %.4f\n"
                  "  up       %.4f %.4f %.4f\n"
                  "  forward  %.4f %.4f %.4f\n"
                  "  position %.3f %.3f %.3f\n",
                  static_cast<unsigned long long>(camera), how.c_str(), offset,
                  pinned ? calibration.c_str() : "NOT PINNED",
                  current.left[0], current.left[1], current.left[2],
                  current.up[0], current.up[1], current.up[2],
                  current.forward[0], current.forward[1], current.forward[2],
                  current.translation[0], current.translation[1], current.translation[2]);
        return buffer;
    }

    // players - the local ClientPlayer objects, their input masks, and what they
    // control. The first step of turning a match into a viewer.
    if (command == "players")
        return DescribeClientPlayers();

    // winkey <vk> | winclick <fx> <fy> | winfocus - input for the game ONLY.
    //
    // The harness used to press deploy with SendInput, which drives the real mouse and keyboard: whatever the
    // user was typing into received Enter/Space, and the cursor jumped to the START button. These post window messages to the game's own top-level window from inside the
    // game process - no global input, no cursor movement, no foreground change, and no UIPI in the way.
    //   winkey <vk>          WM_KEYDOWN + WM_KEYUP (+ WM_CHAR for printable keys), scan code from MapVirtualKey
    //   winclick <fx> <fy>   WM_MOUSEMOVE + WM_LBUTTONDOWN + WM_LBUTTONUP at a fraction of the client area
    //   winfocus             WM_ACTIVATEAPP/WM_ACTIVATE/WM_SETFOCUS, so the game treats the posts as focused input
    if (command == "winkey" || command == "winclick" || command == "winfocus")
    {
        HWND window = FindOwnGameWindow();
        if (window == nullptr)
            return "no visible top-level window in this process";

        char buffer[200];
        if (command == "winfocus")
        {
            PostMessageW(window, WM_ACTIVATEAPP, TRUE, 0);
            PostMessageW(window, WM_ACTIVATE, WA_ACTIVE, 0);
            PostMessageW(window, WM_SETFOCUS, 0, 0);
            sprintf_s(buffer, "winfocus -> posted to hwnd 0x%llX", reinterpret_cast<unsigned long long>(window));
            return buffer;
        }
        if (command == "winkey")
        {
            unsigned int vk = 0;
            if (rest == nullptr || sscanf_s(rest, "%i", &vk) != 1 || vk == 0 || vk > 0xFE)
                return "usage: winkey <virtual-key code, e.g. 0x0D Enter, 0x20 Space>";
            const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
            const LPARAM down = 1 | (static_cast<LPARAM>(scan) << 16);
            const LPARAM up = down | (1LL << 30) | (1LL << 31);
            PostMessageW(window, WM_KEYDOWN, vk, down);
            if (vk == VK_RETURN || vk == VK_SPACE || (vk >= '0' && vk <= 'Z'))
                PostMessageW(window, WM_CHAR, vk == VK_RETURN ? '\r' : vk, down);
            PostMessageW(window, WM_KEYUP, vk, up);
            sprintf_s(buffer, "winkey 0x%02X (scan 0x%02X) -> hwnd 0x%llX", vk, scan, reinterpret_cast<unsigned long long>(window));
            return buffer;
        }
        float fx = -1.0f, fy = -1.0f;
        if (rest == nullptr || sscanf_s(rest, "%f %f", &fx, &fy) != 2 || fx < 0.0f || fx > 1.0f || fy < 0.0f || fy > 1.0f)
            return "usage: winclick <x fraction 0..1> <y fraction 0..1>";
        RECT client = {};
        GetClientRect(window, &client);
        const int x = static_cast<int>(fx * (client.right - client.left));
        const int y = static_cast<int>(fy * (client.bottom - client.top));
        const LPARAM at = MAKELPARAM(x, y);
        PostMessageW(window, WM_MOUSEMOVE, 0, at);
        PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, at);
        PostMessageW(window, WM_LBUTTONUP, 0, at);
        sprintf_s(buffer, "winclick %d,%d (client %ldx%ld) -> hwnd 0x%llX", x, y, client.right - client.left,
                  client.bottom - client.top, reinterpret_cast<unsigned long long>(window));
        return buffer;
    }

    // inputoff <0|1> - zero the analog and digital input masks so the soldier
    // stops responding, or put them back. This is what stops "it is still a
    // game" being true while we fly the camera around.
    if (command == "inputoff")
    {
        int disable = 1;
        if (rest != nullptr && *rest != '\0')
            sscanf_s(rest, "%d", &disable);

        const std::vector<uintptr_t> players = FindClientPlayers();
        if (players.empty())
            return "no ClientPlayer found - is a level loaded?";

        const uint32_t analog = disable ? 0u : 0xFFFFFFFFu;
        const uint64_t digital = disable ? 0ull : 0xFFFFFFFFFFFFFFFFull;

        int written = 0;
        for (size_t i = 0; i < players.size(); ++i)
        {
            if (SafeWriteBytes(players[i] + kPlayerAnalogMask, &analog, sizeof(analog)) &&
                SafeWriteBytes(players[i] + kPlayerDigitalMask, &digital, sizeof(digital)))
                ++written;
        }

        LogLine("input %s on %d of %zu players", disable ? "disabled" : "restored",
                written, players.size());

        char buffer[160];
        sprintf_s(buffer, "input %s on %d of %zu players",
                  disable ? "disabled" : "restored", written, players.size());
        return buffer;
    }

    // freecam - the engine's own FreeCamera object. It always exists; only its
    // activation is unknown. Reading it first so the transform layout is
    // confirmed before anything is written to it.
    if (command == "freecam")
    {
        const uintptr_t context = GetClientGameContext();
        if (context == 0)
            return "no client game context";

        uintptr_t view = 0;
        if (!SafeRead(context + kContextGameViews, &view) || view == 0)
            return "gameViews[0] is null";

        uintptr_t freeCamera = 0;
        if (!SafeRead(view + kGameViewFreeCamera, &freeCamera) || freeCamera == 0)
            return "no FreeCamera on gameViews[0]";

        Transform transform{};
        if (!SafeRead(freeCamera + kFreeCameraTransform, &transform))
            return "FreeCamera transform is unreadable";

        char buffer[512];
        sprintf_s(buffer,
                  "gameView 0x%llX  freeCamera 0x%llX  transform +0x%02zX\n"
                  "  left     %.4f %.4f %.4f\n"
                  "  up       %.4f %.4f %.4f\n"
                  "  forward  %.4f %.4f %.4f\n"
                  "  position %.3f %.3f %.3f\n",
                  static_cast<unsigned long long>(view),
                  static_cast<unsigned long long>(freeCamera), kFreeCameraTransform,
                  transform.left[0], transform.left[1], transform.left[2],
                  transform.up[0], transform.up[1], transform.up[2],
                  transform.forward[0], transform.forward[1], transform.forward[2],
                  transform.translation[0], transform.translation[1], transform.translation[2]);
        return buffer;
    }

    // camprobe - the render view's transform exactly as the engine holds it.
    // The basis convention has been guessed at twice; this measures it.
    if (command == "camprobe")
    {
        Transform current{};
        float fov = 0.0f;
        if (!ReadCamera(&current, &fov))
            return "no render view - is a level loaded?";

        const uintptr_t view = GetRenderView();
        char buffer[512];
        sprintf_s(buffer,
                  "renderView 0x%llX  fov %.2f\n"
                  "  left     %.4f %.4f %.4f  (w %.4f)\n"
                  "  up       %.4f %.4f %.4f  (w %.4f)\n"
                  "  forward  %.4f %.4f %.4f  (w %.4f)\n"
                  "  position %.3f %.3f %.3f  (w %.4f)\n",
                  static_cast<unsigned long long>(view), fov,
                  current.left[0], current.left[1], current.left[2], current.left[3],
                  current.up[0], current.up[1], current.up[2], current.up[3],
                  current.forward[0], current.forward[1], current.forward[2], current.forward[3],
                  current.translation[0], current.translation[1], current.translation[2],
                  current.translation[3]);
        return buffer;
    }

    // select <entityAddress> - set selection from the Frosty scene tree.  This is
    // the reverse of the in-game picker and uses the same published snapshot.
    if (command == "select")
    {
        if (rest == nullptr || *rest == '\0')
            return "usage: select <entityAddress>";

        char addressText[64] = {};
        char editorPath[1024] = {};
        const int parsed = sscanf_s(rest, "%63s %1023s", addressText,
                                    static_cast<unsigned>(sizeof(addressText)), editorPath,
                                    static_cast<unsigned>(sizeof(editorPath)));
        const uintptr_t entity = parsed >= 1
            ? static_cast<uintptr_t>(strtoull(addressText, nullptr, 0)) : 0;
        uintptr_t vtable = 0;
        if (entity == 0 || !SafeRead(entity, &vtable) || !AddressIsGameCode(vtable))
            return "select failed: address is not a live game object";

        const std::string path = parsed >= 2 ? std::string(editorPath) : EntityAssetPath(entity);
        const std::string runtimeClass = EntityRuntimeClass(entity);
        if (path.empty() && runtimeClass.empty())
            return "select failed: object has no entity data";

        g_selectedEntity = entity;
        sprintf_s(g_selectionLabel, "%s%s%s",
                  path.empty() ? "(unnamed entity)" : path.c_str(),
                  runtimeClass.empty() ? "" : "\n",
                  runtimeClass.c_str());
        // No pick box here - this is the plugin's direct `select` path. Fall
        // back to the bounds virtual by passing no AABB.
        PublishSelection(entity, g_selectionLabel, nullptr, nullptr);
        g_showSelection = true;

        char buffer[512];
        sprintf_s(buffer, "selected 0x%llX '%s'", static_cast<unsigned long long>(entity),
                  g_selectionLabel);
        return buffer;
    }

    // showsel <0|1> - draw the selection box and label. Off by default; the
    // drawing thread starts on the first use.
    if (command == "showsel")
    {
        int enable = 1;
        if (rest != nullptr && *rest != '\0')
            sscanf_s(rest, "%d", &enable);

        g_showSelection = (enable != 0);
        if (g_showSelection && !g_selectionDrawing)
        {
            HANDLE thread = CreateThread(nullptr, 0, SelectionDrawThread, nullptr, 0, nullptr);
            if (thread != nullptr)
                CloseHandle(thread);
        }

        return g_showSelection ? "selection drawing on" : "selection drawing off";
    }

    // bounds - what computeBoundsInfo returns for the selection, so a wrong box
    // can be told apart from a wrong call.
    if (command == "bounds")
    {
        if (g_selectedEntity == 0)
            return "nothing selected - use pick";

        const WorldBounds bounds = ReadEntityBounds(g_selectedEntity);
        char buffer[256];
        if (!bounds.valid)
            sprintf_s(buffer, "computeBoundsInfo gave nothing usable for 0x%llX",
                      static_cast<unsigned long long>(g_selectedEntity));
        else
            sprintf_s(buffer, "min %.2f %.2f %.2f  max %.2f %.2f %.2f  size %.2f %.2f %.2f  via %s",
                      bounds.minimum[0], bounds.minimum[1], bounds.minimum[2],
                      bounds.maximum[0], bounds.maximum[1], bounds.maximum[2],
                      bounds.maximum[0] - bounds.minimum[0],
                      bounds.maximum[1] - bounds.minimum[1],
                      bounds.maximum[2] - bounds.minimum[2],
                      BoundsLayoutName(bounds.layout));
        return buffer;
    }

    // boundsraw - the record bytes and every layout's verdict for the selection,
    // side by side with the entity's own world transform. This is the command
    // that settles which layout BFII actually returns; it reads and judges, it
    // never picks for you.
    if (command == "boundsraw")
    {
        if (g_selectedEntity == 0)
            return "nothing selected - use pick";

        const WorldBounds bounds = ReadEntityBounds(g_selectedEntity);
        if (!bounds.rawValid)
            return "computeBoundsInfo faulted or the entity is not spatial";

        Transform where{};
        const bool haveOrigin = GuardedGetTransform(g_selectedEntity, &where);

        std::string out = "entity 0x";
        {
            char head[160];
            sprintf_s(head, "%llX  %s\n",
                      static_cast<unsigned long long>(g_selectedEntity),
                      EntityDataType(g_selectedEntity).c_str());
            out += head;
        }

        if (haveOrigin)
        {
            char line[160];
            sprintf_s(line, "world origin (getTransform)  %.3f %.3f %.3f\n",
                      where.translation[0], where.translation[1], where.translation[2]);
            out += line;
        }
        else
        {
            out += "world origin (getTransform)  UNAVAILABLE - candidates cannot be scored\n";
        }

        // The first 0x80 bytes as floats, four per line, with their offsets. The
        // reader decides what the layout is; this only shows the bytes.
        const float* asFloat = reinterpret_cast<const float*>(&bounds.raw);
        for (int offset = 0; offset < 0x80; offset += 0x10)
        {
            char line[160];
            const int index = offset / 4;
            sprintf_s(line, "  +0x%02X  %12.4f %12.4f %12.4f %12.4f\n", offset,
                      asFloat[index], asFloat[index + 1],
                      asFloat[index + 2], asFloat[index + 3]);
            out += line;
        }

        float origin[3] = { 0.0f, 0.0f, 0.0f };
        if (haveOrigin)
            for (int axis = 0; axis < 3; ++axis)
                origin[axis] = where.translation[axis];

        for (int layout = 0; layout < kBoundsLayoutCount; ++layout)
        {
            BoundsCandidate candidate{};
            BuildBoundsCandidate(bounds.raw, layout, origin, haveOrigin, &candidate);

            char line[256];
            if (!candidate.usable)
            {
                sprintf_s(line, "  %-24s REJECTED\n", BoundsLayoutName(layout));
            }
            else
            {
                sprintf_s(line,
                          "  %-24s size %8.2f %8.2f %8.2f  centre off %7.2f m%s\n",
                          BoundsLayoutName(layout),
                          candidate.maximum[0] - candidate.minimum[0],
                          candidate.maximum[1] - candidate.minimum[1],
                          candidate.maximum[2] - candidate.minimum[2],
                          sqrtf(candidate.score),
                          layout == bounds.layout ? "   <- chosen" : "");
            }
            out += line;
        }

        return out;
    }

    // pushbox - the plugin supplies the boxes the engine would not.
    //
    // computeBoundsInfo returns nothing usable here: on the 00:30 run all 1,903
    // cached objects fell back to a 1.5 m cube, so the ray had nothing to hit and
    // clicking reported "1 candidates tested". The plugin does not need that call.
    // It reads ObjectReferenceObjectData (37,827 shipped) for the world transform,
    // follows Blueprint -> StaticModelEntityData -> MeshAsset -> MeshSetResource,
    // and takes MeshSet.BoundingBox, which Frosty already parses and exposes.
    // That is the same box the renderer uses, from the same file.
    //
    // Matching is by world position rather than by address, because the plugin has
    // no addresses - it has EBX rows. Every cached candidate carries its own world
    // translation, so nearest-within-tolerance is an exact join for placed static
    // geometry.
    //
    //   pushbox <px> <py> <pz> <minX> <minY> <minZ> <maxX> <maxY> <maxZ>
    //   pushbox <record>;<record>;...            batched, one line
    //   pushbox clear                            drop every pushed box
    //
    // UNVERIFIED: written without a running game.
    if (command == "pushbox")
    {
        if (rest == nullptr || *rest == '\0')
            return "usage: pushbox <px py pz minx miny minz maxx maxy maxz>[;...]  |  pushbox clear";

        if (_strnicmp(rest, "clear", 5) == 0)
        {
            size_t cleared = 0;
            LockPick();
            for (size_t i = 0; i < g_pickCache.size(); ++i)
            {
                if (!g_pickCache[i].pushedBounds)
                    continue;
                PickCandidate& c = g_pickCache[i];
                for (int axis = 0; axis < 3; ++axis)
                {
                    c.minimum[axis] = c.origin[axis] - kFallbackHalfExtent;
                    c.maximum[axis] = c.origin[axis] + kFallbackHalfExtent;
                }
                c.pushedBounds = false;
                c.realBounds = false;
                ++cleared;
            }
            UnlockPick();
            char buffer[96];
            sprintf_s(buffer, "%zu pushed box(es) cleared", cleared);
            return buffer;
        }

        // Tolerance is generous enough for float round-tripping through text and
        // for a prefab transform composed in a different order, tight enough that
        // two distinct props never collide: shipped placements are metres apart.
        const float tolerance = 0.35f;

        size_t applied = 0, unmatched = 0, malformed = 0;
        float worstMiss = 0.0f;

        const char* cursor = rest;
        while (cursor != nullptr && *cursor != '\0')
        {
            float v[9];
            const int got = sscanf_s(cursor, "%f %f %f %f %f %f %f %f %f",
                                     &v[0], &v[1], &v[2], &v[3], &v[4],
                                     &v[5], &v[6], &v[7], &v[8]);
            if (got == 9)
            {
                bool sane = true;
                for (int i = 0; i < 9; ++i)
                    if (!BoundsFloatOk(v[i])) sane = false;
                for (int axis = 0; axis < 3 && sane; ++axis)
                {
                    const float size = v[6 + axis] - v[3 + axis];
                    if (!(size > kBoundsMinExtent && size < kBoundsMaxExtent))
                        sane = false;
                }

                if (!sane)
                {
                    ++malformed;
                }
                else
                {
                    size_t bestIndex = static_cast<size_t>(-1);
                    float bestDistance = tolerance;

                    LockPick();
                    for (size_t i = 0; i < g_pickCache.size(); ++i)
                    {
                        const PickCandidate& c = g_pickCache[i];
                        float sum = 0.0f;
                        for (int axis = 0; axis < 3; ++axis)
                        {
                            const float d = c.origin[axis] - v[axis];
                            sum += d * d;
                        }
                        const float distance = sqrtf(sum);
                        if (distance < bestDistance)
                        {
                            bestDistance = distance;
                            bestIndex = i;
                        }
                    }

                    if (bestIndex != static_cast<size_t>(-1))
                    {
                        PickCandidate& c = g_pickCache[bestIndex];
                        for (int axis = 0; axis < 3; ++axis)
                        {
                            c.minimum[axis] = v[3 + axis];
                            c.maximum[axis] = v[6 + axis];
                        }
                        c.realBounds = true;
                        c.pushedBounds = true;
                        ++applied;
                        if (bestDistance > worstMiss) worstMiss = bestDistance;
                    }
                    else
                    {
                        ++unmatched;
                    }
                    UnlockPick();
                }
            }
            else
            {
                ++malformed;
            }

            const char* next = strchr(cursor, ';');
            cursor = (next != nullptr) ? next + 1 : nullptr;
        }

        size_t pushedTotal = 0, cacheSize = 0;
        LockPick();
        cacheSize = g_pickCache.size();
        for (size_t i = 0; i < g_pickCache.size(); ++i)
            if (g_pickCache[i].pushedBounds) ++pushedTotal;
        UnlockPick();

        char buffer[256];
        sprintf_s(buffer,
                  "applied %zu, unmatched %zu, malformed %zu; %zu of %zu cached now have a "
                  "pushed box; worst accepted miss %.3f m (tolerance %.2f m)",
                  applied, unmatched, malformed, pushedTotal, cacheSize, worstMiss, tolerance);
        return buffer;
    }

    // boundslayout [-1|0|1|2|3] - pin the record layout, or -1 to score every
    // candidate per entity. Reports the running tally either way, which is how a
    // family-dependent layout shows itself.
    if (command == "boundslayout")
    {
        if (rest != nullptr && *rest != '\0')
        {
            int requested = kBoundsLayoutAuto;
            if (sscanf_s(rest, "%d", &requested) == 1 &&
                requested >= kBoundsLayoutAuto && requested < kBoundsLayoutCount)
            {
                g_boundsLayout = requested;
            }
            else
            {
                return "usage: boundslayout [-1 auto | 0 | 1 | 2 | 3]";
            }
        }

        char buffer[512];
        sprintf_s(buffer,
                  "layout %s\n"
                  "  0 %-24s %ld\n"
                  "  1 %-24s %ld\n"
                  "  2 %-24s %ld\n"
                  "  3 %-24s %ld\n"
                  "  no usable candidate      %ld\n"
                  "  computeBoundsInfo faults %ld\n"
                  "  isKindOf said not spatial %ld  (tried anyway)",
                  g_boundsLayout == kBoundsLayoutAuto
                      ? "auto (scored per entity)"
                      : BoundsLayoutName(static_cast<int>(g_boundsLayout)),
                  BoundsLayoutName(0), g_boundsLayoutWins[0],
                  BoundsLayoutName(1), g_boundsLayoutWins[1],
                  BoundsLayoutName(2), g_boundsLayoutWins[2],
                  BoundsLayoutName(3), g_boundsLayoutWins[3],
                  g_boundsNoCandidate, g_boundsFaults, g_boundsNotSpatial);
        return buffer;
    }

    // clickselect <0|1> - whether a left click picks. Off by default: every shot would pick.
    if (command == "clickselect")
    {
        int enable = 1;
        if (rest != nullptr && *rest != '\0')
            sscanf_s(rest, "%d", &enable);
        g_clickToSelect = (enable != 0);
        return g_clickToSelect ? "left click picks" : "left click does nothing";
    }

    // pick [range] - the geometric picker: ray against the cached entity boxes.
    if (command == "pick")
    {
        float range = 500.0f;
        if (rest != nullptr && *rest != '\0')
            sscanf_s(rest, "%f", &range);
        if (range <= 0.0f)
            range = 500.0f;

        return PickFromCamera(range);
    }

    // raypick [range] - the engine's own GameWorld::rayCast, kept for diagnosis.
    // It faulted on every attempt of the 01:00 run; this reports the fault rather
    // than reporting it as a miss.
    if (command == "raypick")
    {
        float range = 500.0f;
        if (rest != nullptr && *rest != '\0')
            sscanf_s(rest, "%f", &range);
        if (range <= 0.0f)
            range = 500.0f;

        g_selectedEntity = 0;
        return CastFromCamera(range);
    }

    // pickcache [cap] - rebuild the entity/bounds cache the picker casts against.
    // Built automatically on the first pick; rebuild after the level changes.
    if (command == "pickcache")
    {
        size_t cap = 20000;
        if (rest != nullptr && *rest != '\0')
        {
            unsigned requested = 0;
            if (sscanf_s(rest, "%u", &requested) == 1 && requested > 0)
                cap = requested;
        }

        const size_t count = BuildPickCache(cap);
        char buffer[224];
        sprintf_s(buffer, "%zu pickable entities cached, %ld with real computeBoundsInfo "
                          "bounds, the rest boxed at %.1f m around their transform",
                  count, g_pickCacheReal, kFallbackHalfExtent);
        return buffer;
    }

    // pickrender [cap] - fill the pick cache by scanning for placed render
    // objects by vtable. Recovered from naboo-loaded.dmp and validated against
    // the full population: 1987 found, 1883 (94.8%) with a valid orthonormal
    // transform, 98% of those inside the level. Replaces the sub-level walk,
    // which returned thirteen objects in a loaded map.
    if (command == "pickrender")
    {
        size_t cap = 20000;
        if (rest != nullptr && *rest != '\0')
        {
            unsigned requested = 0;
            if (sscanf_s(rest, "%u", &requested) == 1 && requested > 0)
                cap = requested;
        }

        std::vector<PickCandidate> fresh;
        const size_t found = CollectRenderObjects(&fresh, cap);

        float centre[3] = { 0.0f, 0.0f, 0.0f };
        if (!fresh.empty())
        {
            for (size_t i = 0; i < fresh.size(); ++i)
                for (int axis = 0; axis < 3; ++axis)
                    centre[axis] += (fresh[i].minimum[axis] + fresh[i].maximum[axis]) * 0.5f;
            for (int axis = 0; axis < 3; ++axis)
                centre[axis] /= static_cast<float>(fresh.size());
        }

        const size_t count = fresh.size();
        LockPick();
        g_pickCache.swap(fresh);
        g_pickCacheReal = 0;
        UnlockPick();

        char buffer[352];
        sprintf_s(buffer, "%zu placed objects cached (vtable game+0x%llX, transform +0x%X)\n"
                          "average position %.1f %.1f %.1f\n"
                          "expected ~1883 on Naboo; camera sat near 690 460 -50",
                  count, static_cast<unsigned long long>(kRenderObjectVtableRva),
                  static_cast<unsigned>(kRenderTransformOffset),
                  centre[0], centre[1], centre[2]);
        return buffer;
    }

    // pickscan [Type] [cap] - fill the pick cache from live instances of a
    // reflection type instead of the sub-level walk. Defaults to
    // ClientStaticModelEntity and ClientDynamicModelEntity, which is what the
    // reference editor's own label says the placed props are. Runs on its own
    // thread because the instance scan walks every committed region and takes
    // minutes with a level loaded; ask again for progress.
    if (command == "pickscan")
    {
        if (g_pickScanRunning != 0)
            return std::string("still scanning. last finished: ") + g_pickScanStatus;

        // Asking again with no arguments is asking for progress, not for another
        // multi-minute scan. Pass a type name, or "rescan", to run it again.
        const bool rescan = rest != nullptr && *rest != '\0';
        if (!rescan && g_pickScanFound != 0)
            return std::string("scan finished: ") + g_pickScanStatus +
                   "\npass a type name or 'rescan' to run it again";

        PickScanRequest* request = new PickScanRequest();
        memset(request, 0, sizeof(*request));
        request->cap = 20000;

        if (rest != nullptr && *rest != '\0')
        {
            unsigned cap = 0;
            char typeName[128] = {};
            const int parsed = sscanf_s(rest, "%127s %u", typeName,
                                        static_cast<unsigned>(sizeof(typeName)), &cap);
            if (parsed >= 1 && _stricmp(typeName, "rescan") != 0)
                strcpy_s(request->typeName, typeName);
            if (parsed >= 2 && cap > 0)
                request->cap = cap;
        }

        InterlockedExchange(&g_pickScanRunning, 1);
        HANDLE thread = CreateThread(nullptr, 0, PickScanThread, request, 0, nullptr);
        if (thread == nullptr)
        {
            InterlockedExchange(&g_pickScanRunning, 0);
            delete request;
            return "could not start the scan thread";
        }
        CloseHandle(thread);

        char buffer[288];
        sprintf_s(buffer, "scanning for %s (cap %zu) on its own thread - this takes minutes "
                          "with a level loaded. Ask 'pickscan' again for progress.",
                  request->typeName[0] != '\0' ? request->typeName
                                               : "ClientStaticModelEntity + ClientDynamicModelEntity",
                  request->cap);
        return buffer;
    }

    // sweepvertices - permanently disabled. The historical implementation
    // called unknown virtuals on the live frame renderer and corrupted the
    // process even when SEH caught the faulting calls. Slot identification is a
    // read-only vtable/RVA and disassembly problem.
    if (command == "sweepvertices")
    {
        return "refusing permanently: calling unknown DebugRenderer2 virtuals corrupted the "
               "frame renderer and killed the process at 02:22:47. Use read-only `debugrenderer` "
               "RVA capture; dump disassembly proves begin/end are slots 19/20.";
    }

    // pickdump [n] - what is actually in the pick cache, relative to where the
    // camera is standing. Added because three successive picking fixes were
    // reasoned from the log rather than from the data, and the data is cheap.
    if (command == "pickdump")
    {
        int want = 12;
        if (rest != nullptr && *rest != 0)
            sscanf_s(rest, "%d", &want);
        if (want < 1 || want > 60)
            want = 12;

        Transform camera{};
        float fov = 0.0f;
        if (!ReadCamera(&camera, &fov))
            return "no render view - is a level loaded?";

        const float* eye = camera.translation;

        struct Row { float distance; float diagonal; size_t index; };
        std::vector<Row> rows;
        size_t withMesh = 0, realBounds = 0, degenerate = 0;

        LockPick();
        for (size_t i = 0; i < g_pickCache.size(); ++i)
        {
            const PickCandidate& c = g_pickCache[i];
            if (c.hasMesh) ++withMesh;
            if (c.realBounds) ++realBounds;

            float centre[3], span[3];
            for (int a = 0; a < 3; ++a)
            {
                centre[a] = (c.minimum[a] + c.maximum[a]) * 0.5f;
                span[a] = c.maximum[a] - c.minimum[a];
            }
            if (span[0] <= 0.0f || span[1] <= 0.0f || span[2] <= 0.0f)
                ++degenerate;

            float d = 0.0f;
            for (int a = 0; a < 3; ++a)
                d += (centre[a] - eye[a]) * (centre[a] - eye[a]);

            Row r;
            r.distance = sqrtf(d);
            r.diagonal = sqrtf(span[0] * span[0] + span[1] * span[1] + span[2] * span[2]);
            r.index = i;
            rows.push_back(r);
        }

        std::sort(rows.begin(), rows.end(),
                  [](const Row& a, const Row& b) { return a.distance < b.distance; });

        char header[352];
        sprintf_s(header, "camera at %.1f %.1f %.1f\n"
                          "%zu candidates: %zu with a mesh flag, %zu with real bounds, "
                          "%zu degenerate\nnearest %d by box centre:\n",
                  eye[0], eye[1], eye[2], g_pickCache.size(), withMesh, realBounds,
                  degenerate, want);
        std::string out(header);

        for (int i = 0; i < want && i < static_cast<int>(rows.size()); ++i)
        {
            const PickCandidate& c = g_pickCache[rows[i].index];
            char line[416];
            sprintf_s(line, "  %7.1f m  box %7.1f m  %s%s  %s\n",
                      rows[i].distance, rows[i].diagonal,
                      c.realBounds ? "real" : "fall",
                      c.hasMesh ? "/mesh" : "/    ",
                      c.name.c_str());
            out += line;
        }
        UnlockPick();
        return out;
    }

    // setlabel <text> - let the plugin supply the selection label.
    //
    // The asset path is not recoverable from the runtime object. Four searches
    // established that: render object + one hop (3 hits in 600), entity fields
    // as Blueprint (1%), asset-heap BFS to depth 4 (20 of 500), and the owner
    // chain (25 owners, 12 named, none an Objects/ path). The engine simply does
    // not keep a per-instance path where an outside caller can reach it.
    //
    // The reference editor does not read it from memory either. Its tree shows
    // [ObjectBlueprint] and [SpatialPrefabBlueprint] - Frosty EBX asset types -
    // so the path comes from the level EBX the plugin already has open, and the
    // composed label is pushed back to the game to draw. This is that command.
    //
    // Use "|" for the line break, since the protocol is line-based.
    if (command == "setlabel")
    {
        if (rest == nullptr || *rest == 0)
            return "usage: setlabel <line one>|<line two>";

        char text[320];
        strncpy_s(text, rest, _TRUNCATE);
        for (char* c = text; *c != 0; ++c)
            if (*c == '|')
                *c = 10;

        strcpy_s(g_selectionLabel, text);

        if (g_selectedEntity != 0)
            PublishSelectionLabelOnly(g_selectionLabel);

        char buffer[416];
        sprintf_s(buffer, "label set on 0x%llX:\n%s",
                  static_cast<unsigned long long>(g_selectedEntity), text);
        return buffer;
    }

    // meshonly <0|1> - restrict picking to entities that actually have a mesh.
    // On by default: without it the ray keeps returning LocatorEntity and
    // EffectEntity, invisible markers whose bounds sprawl across a room, so
    // clicking a pillar never gives you the pillar.
    if (command == "meshonly")
    {
        int enable = 1;
        if (rest != nullptr && *rest != 0)
            sscanf_s(rest, "%d", &enable);
        g_pickMeshesOnly = (enable != 0);

        size_t withMesh = 0;
        LockPick();
        for (size_t i = 0; i < g_pickCache.size(); ++i)
            if (g_pickCache[i].hasMesh)
                ++withMesh;
        const size_t total = g_pickCache.size();
        UnlockPick();

        char buffer[224];
        sprintf_s(buffer, "meshes only: %s   (%zu of %zu cached candidates have a mesh)",
                  g_pickMeshesOnly ? "on" : "off", withMesh, total);
        return buffer;
    }

    // debug3d [0|1] - the retail gate on 3D debug geometry.
    //
    // BaseDebugRenderer2 filters every command by geometry type against the pass
    // it is drawing, and Line3d/Triangle3d survive only the World pass
    // (BaseDebugRenderer2.cpp:205-225). Whether that pass runs is a settings
    // question, and this is the switch: Kyber's SDK names
    // GameRenderSettings.DebugRendererEnable at +0x0105 and
    // DebugRenderServiceEnable at +0x0106, and the retail image does carry the
    // "RenderDebug3D" frame-graph node name, so the pass is compiled in rather
    // than stripped.
    //
    // Both go through the enforcer, so they are re-applied every 500 ms instead
    // of written once and quietly reverted.
    //
    // This changes only a render debug flag. It touches no gameplay state and is
    // reversed by debug3d 0.
    if (command == "debug3d")
    {
        int enable = 1;
        if (rest != nullptr && *rest != 0)
            sscanf_s(rest, "%d", &enable);
        const bool on = (enable != 0);

        std::string out;
        out += ApplySetting("GameRenderSettings", "DebugRendererEnable",
                            on ? 1 : 0, 1, on, "debug3d");
        out += "\n";
        out += ApplySetting("GameRenderSettings", "DebugRenderServiceEnable",
                            on ? 1 : 0, 1, on, "debug3d");
        out += "\nthen: lineroute 2   and   framedraw";
        return out;
    }

    // lineroute [0|1|2] [worldType] - which pass the wireframe is aimed at.
    //
    // The vertex-layout switch this command used to carry is gone. Shared source
    // fixes the layout (DebugRenderVertex.h:34-40) and the rendering text proves
    // it, so there was nothing left to toggle. What is genuinely open is whether
    // this retail build runs the 3D debug pass at all, and that is what the
    // route selects.
    //
    //   0  Line2d, projected by this module   - the pass our text already uses
    //   1  Line3d, projected by the engine    - needs the "RenderDebug3D" pass
    //   2  both, so one live run answers it
    //
    // Route 2 is the diagnostic: submit the same wireframe down both paths and
    // look. Two overlapping boxes means the world pass exists and route 1 is the
    // better home for it, because it gets real depth occlusion. One box means it
    // does not exist and route 0 is the answer.
    if (command == "lineroute" || command == "linefix")
    {
        int route = -1, type = -1;
        if (rest != nullptr && *rest != 0)
            sscanf_s(rest, "%d %d", &route, &type);
        if (route >= kLineRouteScreen && route <= kLineRouteBoth)
            g_lineRoute = route;
        if (type >= 0 && type <= 5)
            g_lineGeometryType = type;

        const char* name = "screen (Line2d, projected here)";
        if (g_lineRoute == kLineRouteWorld)
            name = "world (Line3d, engine projects)";
        else if (g_lineRoute == kLineRouteBoth)
            name = "both";

        char buffer[384];
        sprintf_s(buffer, "line route %ld = %s\n"
                          "world geometry type %ld; vertex layout is fixed at "
                          "pos +0x00, color +0x0C, normal +0x10 (source-proven)\n"
                          "try: lineroute 2   then framedraw   - two boxes means "
                          "the RenderDebug3D pass exists",
                  g_lineRoute, name, g_lineGeometryType);
        return buffer;
    }

    // gizmo [on|off] [armPixels] [hitPixels] [headPixels] - the translate
    // gizmo's on-screen size. The plugin hit-tests in pixels, so it should
    // publish the same numbers here rather than the two sides drifting apart.
    if (command == "gizmo")
    {
        char word[32] = { 0 };
        float arm = 0.0f, hit = 0.0f, head = 0.0f;
        if (rest != nullptr && *rest != 0)
        {
            const int fields = sscanf_s(rest, "%31s %f %f %f", word,
                                        static_cast<unsigned>(sizeof(word)),
                                        &arm, &hit, &head);
            if (fields >= 1)
            {
                if (_stricmp(word, "on") == 0 || strcmp(word, "1") == 0)
                    g_gizmoEnabled = true;
                else if (_stricmp(word, "off") == 0 || strcmp(word, "0") == 0)
                    g_gizmoEnabled = false;
                else
                    return "gizmo [on|off] [armPixels] [hitPixels] [headPixels]";
            }
            if (fields >= 2 && arm >= 8.0f && arm <= 600.0f)
                g_gizmoArmPixels = arm;
            if (fields >= 3 && hit >= 2.0f && hit <= 120.0f)
                g_gizmoHitPixels = hit;
            if (fields >= 4 && head >= 2.0f && head <= 120.0f)
                g_gizmoHeadPixels = head;
        }

        char buffer[256];
        sprintf_s(buffer, "gizmo %s - arm %.0f px, hit region %.0f px, head %.0f px\n"
                          "axes: X red (-left), Y green (up), Z blue (forward)",
                  g_gizmoEnabled ? "on" : "off",
                  g_gizmoArmPixels, g_gizmoHitPixels, g_gizmoHeadPixels);
        return buffer;
    }

    // fnstart <rva> - the entry point of the function containing an RVA.
    //
    // This is what replaces "walk back from the RVA with peek". The retail EXE
    // is packed, so its file bytes are worthless - but the image is unpacked in
    // memory by the time this module runs, and that includes the exception
    // directory. RtlLookupFunctionEntry reads the unwind tables and returns
    // exact function bounds. That is an answer rather than a heuristic, and it
    // requires disassembling nothing.
    //
    // If the packer stripped .pdata, or the target is a leaf function with no
    // unwind data, the lookup returns null and this falls back to scanning
    // backwards for the int3/nop alignment padding that precedes a function,
    // then prints the prologue bytes so the result can be judged rather than
    // trusted. Both paths are read-only: nothing is called, nothing is written.
    if (command == "fnstart")
    {
        unsigned long long requested = 0;
        if (rest == nullptr || *rest == 0 || sscanf_s(rest, "%llx", &requested) != 1)
            return "fnstart <rva>   e.g. fnstart 6FD3E96 (the camera writer)";
        if (g_base == 0)
            return "module base unknown";

        const uintptr_t rva = static_cast<uintptr_t>(requested);
        if (rva >= g_size)
            return "rva is outside the module";

        const uintptr_t address = g_base + rva;
        std::string out;
        char line[288];

        sprintf_s(line, "target game+0x%llX (0x%llX)\n",
                  static_cast<unsigned long long>(rva),
                  static_cast<unsigned long long>(address));
        out = line;

        uintptr_t imageBase = 0;
        DWORD beginRva = 0, endRva = 0;
        const bool haveUnwind =
            SafeLookupFunctionEntry(address, &imageBase, &beginRva, &endRva);

        uintptr_t start = 0;

        if (haveUnwind)
        {
            start = imageBase + beginRva;
            sprintf_s(line, "unwind table: function game+0x%lX .. game+0x%lX "
                            "(entry 0x%llX, %lu bytes, target is +0x%llX into it)\n",
                      beginRva, endRva,
                      static_cast<unsigned long long>(start),
                      endRva - beginRva,
                      static_cast<unsigned long long>(address - start));
            out += line;
            out += "grade: OBSERVED - read from the live image exception directory\n";
        }
        else
        {
            out += "unwind table: no entry - either the packer stripped .pdata or "
                   "this is a leaf function; falling back to a padding scan\n";

            // Walk back for the alignment padding that separates functions. Two
            // or more 0xCC, or two or more 0x90, is a compiler gap, and the byte
            // after the run is an entry point.
            const uintptr_t floorAddress = (rva > 0x4000) ? address - 0x4000 : g_base;
            int run = 0;
            unsigned char runByte = 0;

            for (uintptr_t probe = address; probe > floorAddress; --probe)
            {
                unsigned char byte = 0;
                if (!SafeReadBytes(probe - 1, &byte, 1))
                    break;

                if (byte == 0xCC || byte == 0x90)
                {
                    if (run != 0 && byte == runByte)
                        ++run;
                    else
                    {
                        run = 1;
                        runByte = byte;
                    }

                    if (run >= 2)
                    {
                        start = probe + 1;
                        break;
                    }
                }
                else
                {
                    run = 0;
                }
            }

            if (start == 0)
            {
                out += "padding scan: no gap within 0x4000 bytes, no entry point "
                       "recovered\ngrade: UNKNOWN\n";
                return out;
            }

            sprintf_s(line, "padding scan: entry game+0x%llX (0x%llX), target is "
                            "+0x%llX into it\n",
                      static_cast<unsigned long long>(start - g_base),
                      static_cast<unsigned long long>(start),
                      static_cast<unsigned long long>(address - start));
            out += line;
            out += "grade: INFERRED - alignment padding is a strong separator, "
                   "not an exact one\n";
        }

        // The first sixteen bytes, so the answer can be judged by eye.
        unsigned char prologue[16] = { 0 };
        if (SafeReadBytes(start, prologue, sizeof(prologue)))
        {
            out += "prologue:";
            for (size_t i = 0; i < sizeof(prologue); ++i)
            {
                sprintf_s(line, " %02X", prologue[i]);
                out += line;
            }
            out += "\n";

            // Naming the shapes rather than disassembling. These are the MSVC
            // x64 prologue openings; anything else deserves a second look before
            // a hook goes anywhere near it.
            const char* shape = nullptr;
            if (prologue[0] == 0x48 && prologue[1] == 0x89 && prologue[2] == 0x5C)
                shape = "mov [rsp+x], rbx - standard register save";
            else if (prologue[0] == 0x48 && prologue[1] == 0x8B && prologue[2] == 0xC4)
                shape = "mov rax, rsp - frame pointer setup";
            else if (prologue[0] == 0x48 && prologue[1] == 0x83 && prologue[2] == 0xEC)
                shape = "sub rsp, imm8 - small frame";
            else if (prologue[0] == 0x48 && prologue[1] == 0x81 && prologue[2] == 0xEC)
                shape = "sub rsp, imm32 - large frame";
            else if (prologue[0] == 0x40 && prologue[1] == 0x55)
                shape = "push rbp";
            else if (prologue[0] == 0x55)
                shape = "push rbp";
            else if (prologue[0] == 0x53 || prologue[0] == 0x56 || prologue[0] == 0x57)
                shape = "push rbx/rsi/rdi";

            sprintf_s(line, "shape: %s\n",
                      shape != nullptr ? shape : "not a recognised MSVC prologue");
            out += line;
        }

        return out;
    }

    // vertexslots [begin] [end] - which slots the drawing uses, without a rebuild.
    if (command == "vertexslots")
    {
        int begin = 0, end = 0;
        if (rest != nullptr && *rest != '\0' && sscanf_s(rest, "%d %d", &begin, &end) == 2)
        {
            if (begin < 0 || begin >= kDebugVtableSlots || end < 0 || end >= kDebugVtableSlots)
                return "both slots must be within the 28-entry table";
            g_slotBeginVertices = begin;
            g_slotEndVertices = end;
        }

        char buffer[224];
        sprintf_s(buffer, "beginVertices slot %ld, endVertices slot %ld, "
                          "screenText slot %d (game+0x14DE6D0 = OX's drawTextReal)",
                  g_slotBeginVertices, g_slotEndVertices, kSlotDrawTextScreen);
        return buffer;
    }

    // worldprobe [address] - dump an object's first 0x180 bytes as qwords and
    // say what each one points at. Exact BFII Kyber names GameWorld+0x28 as the
    // root SubLevel and +0x20 as the removed-entity tail.
    //
    // Every qword is classified: a pointer into the module, a heap object with a
    // module vtable (and its type name), a plain integer, or a float pair. A
    // sub-level root is a heap object whose own +0x70/+0x78/+0x80 are also heap
    // objects - that shape is flagged, so the right field can be recognised
    // rather than guessed.
    if (command == "worldprobe")
    {
        uintptr_t target = 0;
        if (rest != nullptr && *rest != '\0')
        {
            const char* digits = rest;
            if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) digits += 2;
            unsigned long long parsed = 0;
            if (sscanf_s(digits, "%llx", &parsed) == 1)
                target = static_cast<uintptr_t>(parsed);
        }

        if (target == 0 && !SafeRead(g_base + kGameWorldArrayRva, &target))
            return "no client game world - is a level loaded?";
        if (target == 0)
            return "no client game world - is a level loaded?";

        uintptr_t ownVtable = 0;
        SafeRead(target, &ownVtable);

        char header[288];
        sprintf_s(header, "0x%llX  vtable %s\n",
                  static_cast<unsigned long long>(target),
                  AddressIsGameCode(ownVtable) ? "in module" : "not in module");
        std::string out(header);

        for (size_t offset = 0; offset < 0x180; offset += 8)
        {
            uintptr_t value = 0;
            if (!SafeRead(target + offset, &value))
                break;

            char line[416];
            sprintf_s(line, "  +0x%03zX  0x%016llX", offset,
                      static_cast<unsigned long long>(value));
            out += line;

            if (value == 0)
            {
                out += "\n";
                continue;
            }

            if (AddressIsGameCode(value))
            {
                sprintf_s(line, "   game+0x%llX\n",
                          static_cast<unsigned long long>(value - g_base));
                out += line;
                continue;
            }

            uintptr_t targetVtable = 0;
            if ((value & 7) == 0 && value > 0x10000 && SafeRead(value, &targetVtable) &&
                AddressIsGameCode(targetVtable))
            {
                const std::string name = EntityDisplayName(value);

                // Does it look like a sub-level? Child, sibling and bus are the
                // three fields the walk needs, so test all three.
                uintptr_t child = 0, sibling = 0, bus = 0, probe = 0;
                SafeRead(value + kSubLevelChild, &child);
                SafeRead(value + kSubLevelSibling, &sibling);
                SafeRead(value + kSubLevelRootBus, &bus);

                int objectFields = 0;
                if (child != 0 && SafeRead(child, &probe) && AddressIsGameCode(probe))
                    ++objectFields;
                if (sibling != 0 && SafeRead(sibling, &probe) && AddressIsGameCode(probe))
                    ++objectFields;
                if (bus != 0 && SafeRead(bus, &probe) && AddressIsGameCode(probe))
                    ++objectFields;

                sprintf_s(line, "   object, vt game+0x%llX  '%s'%s\n",
                          static_cast<unsigned long long>(targetVtable - g_base),
                          name.c_str(),
                          objectFields >= 2 ? "   <- SUB-LEVEL SHAPE (child/sibling/bus)" : "");
                out += line;
                continue;
            }

            const float* asFloats = reinterpret_cast<const float*>(&value);
            if (value < 0x100000000ull)
                sprintf_s(line, "   %llu\n", static_cast<unsigned long long>(value));
            else
                sprintf_s(line, "   %.3f %.3f\n", asFloats[0], asFloats[1]);
            out += line;
        }

        return out;
    }

    // sublevels - the sub-level tree with names and per-node entity counts.
    // Confirms exact GameWorld+0x28 at runtime. The per-node count walks the
    // SubLevel's EntityOwner list, not its EntityBus object.
    if (command == "sublevels")
    {
        uintptr_t clientWorld = 0;
        if (!SafeRead(g_base + kGameWorldArrayRva, &clientWorld) || clientWorld == 0)
            return "no client game world - is a level loaded?";

        uintptr_t root = 0;
        uintptr_t removedTail = 0;
        SafeRead(clientWorld + kGameWorldRootSubLevel, &root);
        SafeRead(clientWorld + 0x20, &removedTail);

        char header[288];
        sprintf_s(header, "gameWorld 0x%llX  rootSubLevel(+0x28) 0x%llX  "
                          "lastRemovedEntity(+0x20) 0x%llX\n",
                  static_cast<unsigned long long>(clientWorld),
                  static_cast<unsigned long long>(root),
                  static_cast<unsigned long long>(removedTail));
        std::string out(header);

        if (root == 0)
            return out + "nothing at +0x28\n";

        int counted = 0;
        DescribeSubLevels(root, 0, &counted, &out);

        char tail[160];
        sprintf_s(tail, "\n%d sub-level(s)\n", counted);
        out += tail;
        return out;
    }

    // levelobjects [address] - read the paged container at ClientLevel+0x120.
    // OX found {ptr, 0x200, 0x181} there in the dump - 385 used of 512, which is
    // the right magnitude for a placed-object table and nothing else examined
    // came close. This does its "next session, first action" live.
    if (command == "levelobjects")
    {
        uintptr_t level = 0;
        if (rest != nullptr && *rest != '\0')
        {
            const char* digits = rest;
            if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) digits += 2;
            unsigned long long parsed = 0;
            if (sscanf_s(digits, "%llx", &parsed) == 1)
                level = static_cast<uintptr_t>(parsed);
        }

        if (level == 0)
        {
            uintptr_t context = 0;
            if (!SafeRead(g_base + kClientGameContextRva, &context) || context == 0)
                return "no client game context - is a level loaded?";
            SafeRead(context + 0x38, &level);   // ClientGameContext::clientLevel
        }

        if (level == 0)
            return "no client level - is a level loaded?";

        uintptr_t table = 0;
        uint64_t capacity = 0;
        uint64_t used = 0;
        SafeRead(level + 0x120, &table);
        SafeRead(level + 0x128, &capacity);
        SafeRead(level + 0x130, &used);

        char header[352];
        sprintf_s(header, "clientLevel 0x%llX  +0x120 table 0x%llX  +0x128 0x%llX  +0x130 0x%llX\n",
                  static_cast<unsigned long long>(level),
                  static_cast<unsigned long long>(table),
                  static_cast<unsigned long long>(capacity),
                  static_cast<unsigned long long>(used));
        std::string out(header);

        if (table == 0)
            return out + "nothing there\n";

        const uint64_t limit = capacity > 0 && capacity <= 65536 ? capacity : 512;

        int objects = 0;
        int shown = 0;
        for (uint64_t index = 0; index < limit; ++index)
        {
            uintptr_t entry = 0;
            if (!SafeRead(table + static_cast<size_t>(index) * sizeof(uintptr_t), &entry))
                break;
            if (entry == 0)
                continue;

            uintptr_t vtable = 0;
            if (!SafeRead(entry, &vtable) || !AddressIsGameCode(vtable))
                continue;

            ++objects;
            if (shown >= 12)
                continue;
            ++shown;

            const std::string name = EntityDisplayName(entry);
            Transform where{};
            const bool hasTransform = GuardedGetTransform(entry, &where);

            char line[416];
            if (hasTransform)
                sprintf_s(line, "  [%3llu] 0x%llX  vt game+0x%llX  '%s'  at %.1f %.1f %.1f\n",
                          static_cast<unsigned long long>(index),
                          static_cast<unsigned long long>(entry),
                          static_cast<unsigned long long>(vtable - g_base), name.c_str(),
                          where.translation[0], where.translation[1], where.translation[2]);
            else
                sprintf_s(line, "  [%3llu] 0x%llX  vt game+0x%llX  '%s'  (no transform)\n",
                          static_cast<unsigned long long>(index),
                          static_cast<unsigned long long>(entry),
                          static_cast<unsigned long long>(vtable - g_base), name.c_str());
            out += line;
        }

        char tail[224];
        sprintf_s(tail, "\n%d of %llu slots hold something with a module vtable\n",
                  objects, static_cast<unsigned long long>(limit));
        out += tail;
        LogLine("levelobjects: %d objects in the +0x120 table", objects);
        return out;
    }

    // debugrenderer - what DebugRenderer_current() hands each thread, side by
    // side, and every vtable slot it can reach. OX established the instance
    // comes out of a table indexed by a per-thread context id, so our thread and
    // the engine's do not get the same object. Nothing here calls a virtual:
    // the question is whether calling one would be safe at all.
    if (command == "debugrenderer")
    {
        // Our-thread probe, on a deadline. It hung outright last run.
        static RendererProbe probe;
        memset(&probe, 0, sizeof(probe));

        std::string out;
        HANDLE prober = CreateThread(nullptr, 0, ProbeRendererThread, &probe, 0, nullptr);
        if (prober == nullptr)
        {
            out = "our thread: could not start the probe thread\n";
        }
        else
        {
            const DWORD waited = WaitForSingleObject(prober, 3000);
            CloseHandle(prober);

            if (waited != WAIT_OBJECT_0 || probe.finished == 0)
                out = "our thread: DebugRenderer_current() did NOT return within 3 s - it blocks "
                      "on a thread with no debug context. That is the answer to whether we can "
                      "draw from our own thread: we cannot even ask.\n";
            else
                out = DescribeRendererTable("our thread", GetCurrentThreadId(), probe.renderer,
                                            probe.vtable, probe.slots);
        }

        const uintptr_t mineRenderer = probe.finished != 0 ? probe.renderer : 0;

        if (!g_watchDrive)
        {
            out += "\nframe thread: watchdrive is off, so the handler never runs - "
                   "run watchdrive 1 and ask again\n";
            return out;
        }

        InterlockedExchange(&g_captureDone, 0);
        InterlockedExchange(&g_captureRenderer, 1);

        for (int waited = 0; waited < 40 && g_captureDone == 0; ++waited)
            Sleep(25);

        out += "\n";
        if (g_captureDone == 0)
            out += "frame thread: the handler did not run within a second\n";
        else
            out += DescribeRendererTable("frame thread", g_capturedThread, g_capturedRenderer,
                                         g_capturedVtable, g_capturedSlots);

        if (g_capturedRenderer != 0 && mineRenderer != 0 && g_capturedRenderer != mineRenderer)
            out += "\nthe two threads get DIFFERENT renderers - the per-thread table is real, "
                   "and drawing belongs on the frame thread\n";
        else if (g_capturedRenderer != 0 && g_capturedRenderer == mineRenderer)
            out += "\nboth threads get the SAME renderer - the queue is shared, and the thread "
                   "was never the problem\n";

        return out;
    }

    // drawmode <off|thread|frame|both> - which thread submits the selection
    // geometry. Debug primitives queue per-thread, so this is the difference
    // between drawing and drawing into nothing. `frame` rides the camera
    // breakpoint, which is already on the engine's own thread.
    if (command == "drawmode")
    {
        if (rest != nullptr && *rest != '\0')
        {
            char mode[32] = {};
            sscanf_s(rest, "%31s", mode, static_cast<unsigned>(sizeof(mode)));

            if (_stricmp(mode, "off") == 0) g_drawRoute = kDrawOff;
            else if (_stricmp(mode, "thread") == 0) g_drawRoute = kDrawThread;
            else if (_stricmp(mode, "frame") == 0) g_drawRoute = kDrawFrame;
            else if (_stricmp(mode, "both") == 0) g_drawRoute = kDrawBoth;
            else return "usage: drawmode <off|thread|frame|both>";
        }

        const LONG route = g_drawRoute;
        const char* name = route == kDrawOff ? "off"
                         : route == kDrawThread ? "thread"
                         : route == kDrawFrame ? "frame"
                         : "both";

        char buffer[288];
        sprintf_s(buffer, "drawmode %s  (frame handler: %ld traps, %ld submits, "
                          "%ld lines last submit)%s",
                  name, g_frameDrawTicks, g_frameDrawsIssued, g_frameDrawLines,
                  ((route & kDrawFrame) != 0 && !g_watchDrive)
                      ? "\n  note: frame drawing needs watchdrive 1 - the handler is the hook"
                      : "");
        return buffer;
    }

    // framedraw [size] - queue one green box on the engine's frame thread, at the
    // camera. Proves the drawing pipe without needing a successful pick.
    if (command == "framedraw")
    {
        float size = 1.0f;
        if (rest != nullptr && *rest != '\0')
            sscanf_s(rest, "%f", &size);
        if (size <= 0.0f)
            size = 1.0f;

        Transform current{};
        float fov = 0.0f;
        if (!ReadCamera(&current, &fov))
            return "no render view - is a level loaded?";

        // Ten metres ahead, so it is not inside the near plane.
        for (int axis = 0; axis < 3; ++axis)
            g_frameTestCentre[axis] = current.translation[axis] + current.forward[axis] * 10.0f;
        g_frameTestSize = size;

        const LONG before = g_frameDrawsIssued;
        InterlockedExchange(&g_frameTestPending, 1);

        if (!g_watchDrive)
            return "queued, but watchdrive is off - the frame handler never runs. "
                   "run watchdrive 1 first";

        Sleep(120);
        char buffer[224];
        sprintf_s(buffer, "queued at %.1f %.1f %.1f, size %.1f - handler submitted %ld time(s) "
                          "since, %ld lines on the last one",
                  g_frameTestCentre[0], g_frameTestCentre[1], g_frameTestCentre[2], size,
                  g_frameDrawsIssued - before, g_frameDrawLines);
        return buffer;
    }

    // selected - what is currently selected, with its live transform.
    if (command == "selected")
    {
        if (g_selectedEntity == 0)
            return "nothing selected - use pick";

        const std::string name = EntityDisplayName(g_selectedEntity);
        const std::string instanceGuid = EntityInstanceGuid(g_selectedEntity);
        Transform where{};
        const bool hasTransform = GuardedGetTransform(g_selectedEntity, &where);

        char buffer[352];
        if (hasTransform)
            sprintf_s(buffer, "0x%llX guid %s '%s' at %.2f %.2f %.2f",
                      static_cast<unsigned long long>(g_selectedEntity),
                      instanceGuid.empty() ? "-" : instanceGuid.c_str(), name.c_str(),
                      where.translation[0], where.translation[1], where.translation[2]);
        else
            sprintf_s(buffer, "0x%llX guid %s '%s' (no transform)",
                      static_cast<unsigned long long>(g_selectedEntity),
                      instanceGuid.empty() ? "-" : instanceGuid.c_str(), name.c_str());
        return buffer;
    }

    // hud <0|1> - the game's own HUD/UI draw flag. OpenGameCamera binds this to
    // F7; UISettings.drawEnable is the same field.
    if (command == "hud")
    {
        int show = 0;
        if (rest != nullptr && *rest != '\0')
            sscanf_s(rest, "%d", &show);

        return ApplySetting("UISettings", "DrawEnable", show ? 1 : 0, 1,
                            show == 0, show ? "hud on" : "hud off");
    }

    // forcefov <degrees> - the engine's own FOV override, -1 to release. Better
    // than writing the render view: this is the field the renderer reads.
    if (command == "forcefov")
    {
        float degrees = -1.0f;
        if (rest == nullptr || *rest == '\0' || sscanf_s(rest, "%f", &degrees) != 1)
            degrees = -1.0f;

        long long bits = 0;
        const float stored = degrees;
        memcpy(&bits, &stored, sizeof(stored));

        const bool holding = degrees > 0.0f;
        return ApplySetting("GameRenderSettings", "ForceFov", bits, 4, holding,
                            holding ? "fov forced" : "fov released");
    }

    // timescale <n> - 0 freezes the world, 1 is normal. OpenGameCamera's F8.
    if (command == "timescale")
    {
        float scale = 1.0f;
        if (rest == nullptr || *rest == '\0' || sscanf_s(rest, "%f", &scale) != 1)
            scale = 1.0f;

        long long bits = 0;
        memcpy(&bits, &scale, sizeof(scale));

        return ApplySetting("GameTimeSettings", "TimeScale", bits, 4, scale != 1.0f,
                            scale == 0.0f ? "time frozen" : "time scaled");
    }

    // invert [yaw] [pitch] - 1 or 0 each. No argument reports the current setting.
    if (command == "invert")
    {
        int yaw = -1, pitch = -1;
        const int parsed = sscanf_s(rest, "%d %d", &yaw, &pitch);
        if (parsed >= 1) g_invertYaw = (yaw != 0);
        if (parsed >= 2) g_invertPitch = (pitch != 0);

        char buffer[96];
        sprintf_s(buffer, "invert yaw %d, pitch %d", g_invertYaw ? 1 : 0,
                  g_invertPitch ? 1 : 0);
        return buffer;
    }

    // fov <degrees> - 0 hands it back to the engine. Written every frame in the
    // breakpoint handler, because a thread cannot hold it.
    if (command == "fov")
    {
        float degrees = 0.0f;
        if (sscanf_s(rest, "%f", &degrees) != 1)
        {
            char buffer[128];
            sprintf_s(buffer, "fov override %s, %.1f degrees",
                      g_fovOverride ? "on" : "off",
                      g_fovRadians * 180.0f / 3.14159265358979f);
            return buffer;
        }

        if (degrees <= 0.0f)
        {
            g_fovOverride = false;
            return "fov handed back to the engine";
        }

        if (degrees > 170.0f) degrees = 170.0f;
        g_fovRadians = degrees * 3.14159265358979f / 180.0f;
        g_fovOverride = true;

        char buffer[128];
        sprintf_s(buffer, "fov %.1f degrees (%.4f rad)", degrees, g_fovRadians);
        return buffer;
    }

    // camtoassets - put the camera where the level's entities actually are.
    // The deploy pose is about three kilometres below the map, and flying that
    // by hand is not a viewer.
    if (command == "camtoassets")
    {
        const std::vector<EntityRow> rows = EnumerateEntities(256);

        double sumX = 0.0, sumY = 0.0, sumZ = 0.0;
        int counted = 0;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const EntityRow& row = rows[i];
            if (!row.hasTransform)
                continue;
            // Ignore the origin and the lobby volume far below the map.
            if (row.y < -1000.0f)
                continue;
            if (row.x == 0.0f && row.y == 0.0f && row.z == 0.0f)
                continue;

            sumX += row.x;
            sumY += row.y;
            sumZ += row.z;
            ++counted;
        }

        if (counted == 0)
            return "no entity positions to aim at - is the level streamed in?";

        const float x = static_cast<float>(sumX / counted);
        const float y = static_cast<float>(sumY / counted) + 25.0f;   // above it
        const float z = static_cast<float>(sumZ / counted);

        EnterCriticalSection(&g_cameraLock);
        g_cameraTarget.translation[0] = x;
        g_cameraTarget.translation[1] = y;
        g_cameraTarget.translation[2] = z;
        g_cameraTarget.translation[3] = 1.0f;
        Transform copy = g_cameraTarget;
        LeaveCriticalSection(&g_cameraLock);
        memcpy(g_watchPose, &copy, sizeof(copy));

        LogLine("camera: moved to the middle of %d entities at %.1f %.1f %.1f",
                counted, x, y, z);

        char buffer[192];
        sprintf_s(buffer, "moved to %.1f %.1f %.1f - the middle of %d placed entities",
                  x, y, z, counted);
        return buffer;
    }

    // look <sensitivity> - degrees of turn per mouse count.
    if (command == "look")
    {
        float sensitivity = 0.0f;
        if (sscanf_s(rest, "%f", &sensitivity) != 1 || sensitivity <= 0.0f)
        {
            char buffer[96];
            sprintf_s(buffer, "look sensitivity is %.3f", g_lookSensitivity);
            return buffer;
        }

        g_lookSensitivity = sensitivity;
        char buffer[96];
        sprintf_s(buffer, "look sensitivity %.3f", g_lookSensitivity);
        return buffer;
    }

    // speed <metres per second> - base fly speed; shift is x5, alt is x0.2.
    if (command == "speed")
    {
        float speed = 0.0f;
        if (sscanf_s(rest, "%f", &speed) != 1 || speed <= 0.0f)
        {
            char buffer[96];
            sprintf_s(buffer, "fly speed is %.1f m/s", g_flySpeed);
            return buffer;
        }

        g_flySpeed = speed;
        char buffer[96];
        sprintf_s(buffer, "fly speed %.1f m/s (shift x5, alt x0.2)", g_flySpeed);
        return buffer;
    }

    // fly <0|1> [speed] - keyboard control of the locked camera.
    if (command == "fly")
    {
        int enable = 1;
        float speed = 0.0f;
        const int parsed = sscanf_s(rest, "%d %f", &enable, &speed);
        if (parsed >= 1)
            g_flyEnabled = (enable != 0);
        if (parsed >= 2 && speed > 0.0f)
            g_flySpeed = speed;

        char buffer[160];
        sprintf_s(buffer, "fly %s at %.1f m/s - WASD move, Q/E down/up, arrows look, "
                          "shift fast, ctrl slow (game window must have focus)",
                  g_flyEnabled ? "on" : "off", g_flySpeed);
        return buffer;
    }

    // viewhook <0|1> [status] - the flicker-free camera override (see InstallViewHook).
    if (command == "viewhook")
    {
        if (rest != nullptr && strncmp(rest, "status", 6) == 0)
        {
            char buffer[96];
            sprintf_s(buffer, "viewhook %s, %lld overrides", g_viewHookInstalled ? "on" : "off", static_cast<long long>(g_viewHookHits));
            return buffer;
        }
        int enable = 1;
        if (rest != nullptr && *rest != '\0')
            sscanf_s(rest, "%d", &enable);
        return InstallViewHook(enable != 0);
    }

    // camaim <x> <y> <z> - turn the locked camera to face a world point. Built on TurnCamera (the
    // engine's own basis, rotated) instead of a basis made from angles, which once mirrored the view:
    // the yaw and pitch needed are worked out, and of each turn's two directions the one that
    // actually ends up facing the point is kept. Reports how far off it ended up.
    if (command == "camaim")
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        if (rest == nullptr || sscanf_s(rest, "%f %f %f", &x, &y, &z) != 3)
            return "usage: camaim <x> <y> <z>   (camlock 1 first)";
        if (!g_cameraOverride)
            return "ignored: camlock is off";

        EnterCriticalSection(&g_cameraLock);
        Transform pose = g_cameraTarget;
        LeaveCriticalSection(&g_cameraLock);

        float want[3] = { x - pose.translation[0], y - pose.translation[1], z - pose.translation[2] };
        const float length = sqrtf(want[0] * want[0] + want[1] * want[1] + want[2] * want[2]);
        if (length < 0.01f)
            return "camaim: the point is where the camera is";
        // The view looks along -forward (verified 2026-10-08: aiming forward at a point below showed sky,
        // at the mirrored point above showed the rooftops below), so forward is turned AWAY from the point.
        for (float& c : want)
            c = -c / length;

        const float toDegrees = 180.0f / 3.14159265358979f;
        auto misses = [&](const Transform& t) {
            const float f[3] = { t.forward[0], t.forward[1], t.forward[2] };
            const float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
            float dot = (f[0] * want[0] + f[1] * want[1] + f[2] * want[2]) / (fl > 0.0001f ? fl : 1.0f);
            dot = dot > 1.0f ? 1.0f : dot < -1.0f ? -1.0f : dot;
            return acosf(dot) * toDegrees;
        };

        // yaw: the horizontal angle between forward and the wanted direction
        float yaw = (atan2f(want[0], want[2]) - atan2f(pose.forward[0], pose.forward[2])) * toDegrees;
        while (yaw > 180.0f) yaw -= 360.0f;
        while (yaw < -180.0f) yaw += 360.0f;
        Transform a = pose, b = pose;
        TurnCamera(&a, yaw, 0.0f);
        TurnCamera(&b, -yaw, 0.0f);
        Transform turned = misses(a) <= misses(b) ? a : b;

        // pitch: from forward's elevation to the wanted elevation
        const float fy = turned.forward[1] > 1.0f ? 1.0f : turned.forward[1] < -1.0f ? -1.0f : turned.forward[1];
        const float pitch = (asinf(want[1]) - asinf(fy)) * toDegrees;
        a = turned;
        b = turned;
        TurnCamera(&a, 0.0f, pitch);
        TurnCamera(&b, 0.0f, -pitch);
        turned = misses(a) <= misses(b) ? a : b;

        EnterCriticalSection(&g_cameraLock);
        g_cameraTarget = turned;
        LeaveCriticalSection(&g_cameraLock);

        char buffer[160];
        sprintf_s(buffer, "aimed at %.2f %.2f %.2f from %.2f %.2f %.2f (%.1f deg off)", x, y, z,
                  turned.translation[0], turned.translation[1], turned.translation[2], misses(turned));
        return buffer;
    }

    // camturn <dyaw> <dpitch> - turn the camera by rotating the basis the engine
    // gave us. There is no absolute-angle form any more: synthesising a basis
    // from angles got the handedness wrong and mirrored the view.
    if (command == "camturn" || command == "camlook")
    {
        float yaw = 0.0f, pitch = 0.0f;
        if (sscanf_s(rest, "%f %f", &yaw, &pitch) != 2)
            return "usage: camturn <dyaw> <dpitch>   (degrees, relative)";

        if (!g_cameraOverride)
            return "ignored: camlock is off";

        EnterCriticalSection(&g_cameraLock);
        TurnCamera(&g_cameraTarget, yaw, pitch);
        Transform copy = g_cameraTarget;
        LeaveCriticalSection(&g_cameraLock);

        char buffer[192];
        sprintf_s(buffer, "forward %.3f %.3f %.3f", copy.forward[0], copy.forward[1],
                  copy.forward[2]);
        return buffer;
    }

    // cammove <dx> <dy> <dz> - move along the view's own axes, which is what makes it
    // fly like a camera rather than slide along world axes.
    if (command == "cammove")
    {
        float dx = 0.0f, dy = 0.0f, dz = 0.0f;
        if (sscanf_s(rest, "%f %f %f", &dx, &dy, &dz) != 3)
            return "usage: cammove <right> <up> <forward>";

        if (!g_cameraOverride)
            return "cammove ignored: camlock is off";

        EnterCriticalSection(&g_cameraLock);
        for (int axis = 0; axis < 3; ++axis)
        {
            g_cameraTarget.translation[axis] +=
                g_cameraTarget.left[axis] * dx +
                g_cameraTarget.up[axis] * dy +
                g_cameraTarget.forward[axis] * dz;
        }
        Transform copy = g_cameraTarget;
        LeaveCriticalSection(&g_cameraLock);

        char buffer[128];
        sprintf_s(buffer, "camera %.2f %.2f %.2f",
                  copy.translation[0], copy.translation[1], copy.translation[2]);
        return buffer;
    }

    // camwarp <x> <y> <z> - absolute placement, orientation untouched.
    if (command == "camwarp")
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        if (sscanf_s(rest, "%f %f %f", &x, &y, &z) != 3)
            return "usage: camwarp <x> <y> <z>";

        if (!g_cameraOverride)
            return "camwarp ignored: camlock is off";

        EnterCriticalSection(&g_cameraLock);
        g_cameraTarget.translation[0] = x;
        g_cameraTarget.translation[1] = y;
        g_cameraTarget.translation[2] = z;
        LeaveCriticalSection(&g_cameraLock);

        return "camera warped";
    }

    if (command == "viewmode")
    {
        int value = 0;
        if (sscanf_s(rest, "%d", &value) != 1)
            return "usage: viewmode <n>";

        // Through the settings writer: it remembers the instances, so only the first change scans memory (~30 s).
        const int written = WriteSettingByName("WorldRenderSettings", "ViewMode", value, sizeof(value));
        char buffer[128];
        if (written < 0)
            sprintf_s(buffer, "viewmode: could not resolve WorldRenderSettings::ViewMode (code %d) - is a level loaded?", written);
        else
            sprintf_s(buffer, "viewmode %d written to %d instance(s)", value, written);
        LogLine("%s", buffer);
        return buffer;
    }

    return "unknown command: " + command;
}

// The std::string work happens in a callee so the guarded frame owns no C++
// object that would require unwinding. The result itself is dynamic: the old
// 64 KiB copy buffer silently truncated large entity and reflection tables.
static void CopyHandlerResult(const char* line, std::string* out)
{
    const std::string reply = HandleCommand(std::string(line));
    *out = reply;
}

// Each connection is served on its own thread, so the reply buffer can no longer
// be one shared static - two slow commands overlapping would write over each
// other. 64 KB on the heap per request, freed when the thread ends.
static DWORD WINAPI ServeClientThread(LPVOID parameter);

static bool GuardedHandle(const char* line, std::string* out)
{
    __try
    {
        CopyHandlerResult(line, out);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// The in-game UI's way in (gamelink_api.h): the same guarded path a TCP command takes.
static std::string CommandForUi(const std::string& line)
{
    std::string out;                             // not logged: the Diagnostics tab polls 6 commands a second
    if (!GuardedHandle(line.c_str(), &out))
        return "error: the command faulted (see the gamelink log)";
    return out;
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

static DWORD WINAPI ServerThread(LPVOID)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        LogLine("WSAStartup failed");
        return 1;
    }

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET)
    {
        LogLine("socket() failed: %d", WSAGetLastError());
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kListenPort);
    InetPtonA(AF_INET, "127.0.0.1", &address.sin_addr);

    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
    {
        LogLine("bind(%d) failed: %d", kListenPort, WSAGetLastError());
        closesocket(listener);
        return 1;
    }

    listen(listener, SOMAXCONN);
    LogLine("listening on 127.0.0.1:%d", kListenPort);

    for (;;)
    {
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET)
        {
            Sleep(50);
            continue;
        }

        // One thread per connection. The loop used to serve clients one at a
        // time, and a settings write that scans committed memory for live
        // instances takes over a minute with a level loaded - `forcefov 80` sat
        // there for six minutes on the 01:40 run and every request behind it,
        // including the diagnostics we were trying to read, was never seen. A
        // slow command is now slow on its own thread and nothing else waits.
        HANDLE worker = CreateThread(nullptr, 0, ServeClientThread,
                                     reinterpret_cast<LPVOID>(client), 0, nullptr);
        if (worker != nullptr)
            CloseHandle(worker);
        else
            closesocket(client);      // no thread, no service - do not leak it
    }
}

static DWORD WINAPI ServeClientThread(LPVOID parameter)
{
    const SOCKET client = reinterpret_cast<SOCKET>(parameter);

    // TCP is a byte stream: even a short request is not guaranteed to arrive in
    // one recv. Accumulate one bounded line so split packets cannot execute a
    // truncated command. Extra bytes after LF are intentionally ignored because
    // this protocol defines exactly one request per fresh connection.
    constexpr size_t kMaximumCommandBytes = 4095;
    std::string line;
    char input[1024] = {};
    bool complete = false;
    bool tooLong = false;
    while (!complete)
    {
        const int received = recv(client, input, sizeof(input), 0);
        if (received <= 0)
            break;
        for (int i = 0; i < received; ++i)
        {
            if (input[i] == '\n') { complete = true; break; }
            if (line.size() >= kMaximumCommandBytes) { tooLong = true; complete = true; break; }
            if (input[i] != '\r') line.push_back(input[i]);
        }
    }

    if (!line.empty() || tooLong)
    {
        LogLine("command: %s", line.c_str());

        std::string reply;
        if (tooLong)
            reply = "command rejected: line exceeds 4095 bytes";
        else if (!GuardedHandle(line.c_str(), &reply))
        {
            LogLine("EXCEPTION handling '%s'", line.c_str());
            reply = "command faulted - see the gamelink log";
        }

        reply.push_back('\n');
        size_t sent = 0;
        while (sent < reply.size())
        {
            const size_t remaining = reply.size() - sent;
            const int chunk = remaining > INT_MAX ? INT_MAX : static_cast<int>(remaining);
            const int wrote = send(client, reply.data() + sent, chunk, 0);
            if (wrote == SOCKET_ERROR || wrote == 0)
                break;
            sent += static_cast<size_t>(wrote);
        }
    }

    closesocket(client);
    return 0;
}

struct InitPulse
{
    uintptr_t clientWorld;
    uintptr_t serverWorld;
    uintptr_t clientContext;
    uintptr_t clientLevel;
    uintptr_t serverContext;
    uintptr_t playerManager;
    uintptr_t peer;

    bool operator!=(const InitPulse& other) const
    {
        return clientWorld != other.clientWorld || serverWorld != other.serverWorld ||
               clientContext != other.clientContext || clientLevel != other.clientLevel ||
               serverContext != other.serverContext ||
               playerManager != other.playerManager || peer != other.peer;
    }
};

static InitPulse ReadInitPulse()
{
    InitPulse pulse{};
    SafeRead(g_base + kGameWorldArrayRva, &pulse.clientWorld);
    SafeRead(g_base + kGameWorldArrayRva + sizeof(uintptr_t), &pulse.serverWorld);
    SafeRead(g_base + kClientGameContextRva, &pulse.clientContext);
    if (pulse.clientContext != 0)
        SafeRead(pulse.clientContext + kClientGameContextLevelOffset, &pulse.clientLevel);
    SafeRead(g_base + kServerGameContextRva, &pulse.serverContext);
    if (pulse.serverContext != 0)
    {
        SafeRead(pulse.serverContext + 0x58, &pulse.playerManager);
        SafeRead(pulse.serverContext + 0x60, &pulse.peer);
    }
    return pulse;
}

static DWORD WINAPI InitWatchThread(LPVOID)
{
    const DWORD started = GetTickCount();
    InitPulse previous{};
    LogLine("init: watching engine construction from t=0");

    for (int i = 0; i < 12000; ++i)          // 10 minutes at 50 ms
    {
        Sleep(50);
        const InitPulse now = ReadInitPulse();
        if (now != previous)
        {
            LogLine("init: +%lu ms clientWorld=0x%llX serverWorld=0x%llX clientCtx=0x%llX "
                    "clientLevel=0x%llX serverCtx=0x%llX pm=0x%llX peer=0x%llX",
                    GetTickCount() - started,
                    now.clientWorld, now.serverWorld, now.clientContext, now.clientLevel,
                    now.serverContext, now.playerManager, now.peer);

            if (now.clientLevel != 0 && now.clientLevel != previous.clientLevel)
            {
                char levelName[256] = {};
                if (ReadLoadedLevelName(now.clientLevel, levelName, sizeof(levelName)))
                    LogLine("init: >>> LOADED LEVEL IS '%s' <<<", levelName);
                else
                    LogLine("init: level object 0x%llX - could not read a name from it",
                            now.clientLevel);

                // Deliberately not taking the camera automatically any more. The
                // render-view write fights the engine and produces a flickering,
                // mirrored view; until the camera hook replaces it, this only
                // happens when it is explicitly asked for.
                if (false && g_viewerWanted && !g_cameraOverride)
                {
                    // The render view is not necessarily valid the instant the
                    // level object appears, so keep trying briefly rather than
                    // taking one shot at it.
                    for (int attempt = 0; attempt < 100 && !g_cameraOverride; ++attempt)
                    {
                        Transform current{};
                        float fov = 0.0f;
                        if (ReadCamera(&current, &fov))
                        {
                            EnterCriticalSection(&g_cameraLock);
                            g_cameraTarget = current;
                            LeaveCriticalSection(&g_cameraLock);
                            g_cameraOverride = true;
                            LogLine("viewer: camera taken at %.1f %.1f %.1f",
                                    current.translation[0], current.translation[1],
                                    current.translation[2]);
                            break;
                        }
                        Sleep(100);
                    }
                    if (!g_cameraOverride)
                        LogLine("viewer: no render view after 10 s - camera not taken");
                }
            }

            previous = now;
        }
    }

    LogLine("init: watch finished");
    return 0;
}

static DWORD WINAPI Startup(LPVOID)
{
    InitializeCriticalSection(&g_logLock);
    g_logLockReady = true;

    CreateDirectoryA("C:\\ProgramData\\FrostyGameLink", nullptr);
    // _SH_DENYNO rather than fopen: fopen's default share mode locks the file
    // for the life of the process, and a run is precisely when it wants reading.
    g_log = _fsopen("C:\\ProgramData\\FrostyGameLink\\gamelink.log", "a", _SH_DENYNO);

    LogLine("---- FrostyGameLink attached ----");

    HMODULE mainModule = GetModuleHandleA(nullptr);
    MODULEINFO info{};
    if (GetModuleInformation(GetCurrentProcess(), mainModule, &info, sizeof(info)))
    {
        g_base = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
        g_size = info.SizeOfImage;
    }

    LogLine("module base 0x%llX size 0x%llX", g_base, g_size);
    if (g_base != 0)
    {
        g_nearArena = AllocateNear(g_base + g_size / 2, kNearArenaSize);
        LogLine("near-code arena %s at 0x%p", g_nearArena != nullptr ? "reserved" : "NOT reserved", g_nearArena);
    }

    if (g_base != kExpectedModuleBase)
        LogLine("WARNING: base is not 0x%llX - the hardcoded renderer offset may be wrong",
                kExpectedModuleBase);

    InstallCrashReporting();

    const InitPulse atAttach = ReadInitPulse();
    const bool early = atAttach.clientWorld == 0 && atAttach.clientContext == 0 &&
                       atAttach.serverContext == 0;
    if (early)
    {
        LogLine("attached EARLY - nothing is constructed yet, this is a pre-entry-point load");
        CreateThread(nullptr, 0, InitWatchThread, nullptr, 0, nullptr);
    }
    else
    {
        LogLine("attached LATE - the engine is already up (clientWorld=0x%llX clientCtx=0x%llX)",
                atAttach.clientWorld, atAttach.clientContext);
    }

    InitializeCriticalSection(&g_registryLock);
    g_registryLockReady = true;

    InitializeCriticalSection(&g_cameraLock);
    InitializeCriticalSection(&g_pickLock);
    g_pickLockReady = true;
    CreateThread(nullptr, 0, CameraThread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, CameraDriveThread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, SelectionDrawThread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, LevelHoldThread, nullptr, 0, nullptr);
    if (g_size == kExpectedModuleSize)           // the menu's hooks are build-specific
        CreateThread(nullptr, 0, MenuBootThread, nullptr, 0, nullptr);

    ServerThread(nullptr);
    return 0;
}

} // namespace gamelink

std::string gl::Command(const std::string& line)
{
    return gamelink::CommandForUi(line);
}

bool gl::MenuCapturesInput()
{
    return gamelink::MenuIsOpen();
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        CreateThread(nullptr, 0, gamelink::Startup, nullptr, 0, nullptr);
    }
    return TRUE;
}
