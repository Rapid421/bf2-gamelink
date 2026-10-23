// gamelink_inject.exe - loads FrostyGameLink.dll into Star Wars Battlefront II (2017).
//
//   gamelink_inject.exe                  inject bin\FrostyGameLink.dll (next to this exe) into the running game
//   gamelink_inject.exe --wait           wait for the game to start, then inject as early as possible
//   gamelink_inject.exe --dll <path>     inject another DLL
//
// If the game runs as administrator, this asks for administrator rights once (UAC) and tries again.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <cstdio>
#include <string>

static void Pause()
{
    printf("\nPress Enter to close.");
    getchar();
}

static DWORD FindGame()
{
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe = {sizeof(pe)};
    if (snap != INVALID_HANDLE_VALUE && Process32FirstW(snap, &pe))
        do
        {
            if (_wcsicmp(pe.szExeFile, L"starwarsbattlefrontii.exe") == 0)
                pid = pe.th32ProcessID;
        } while (Process32NextW(snap, &pe));
    if (snap != INVALID_HANDLE_VALUE)
        CloseHandle(snap);
    return pid;
}

static bool AlreadyLoaded(DWORD pid)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    MODULEENTRY32W me = {sizeof(me)};
    bool found = false;
    if (Module32FirstW(snap, &me))
        do
        {
            if (_wcsicmp(me.szModule, L"FrostyGameLink.dll") == 0)
                found = true;
        } while (!found && Module32NextW(snap, &me));
    CloseHandle(snap);
    return found;
}

static bool IsElevated()
{
    HANDLE token = nullptr;
    TOKEN_ELEVATION e = {};
    DWORD n = 0;
    bool elevated = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        if (GetTokenInformation(token, TokenElevation, &e, sizeof(e), &n))
            elevated = e.TokenIsElevated != 0;
        CloseHandle(token);
    }
    return elevated;
}

int wmain(int argc, wchar_t** argv)
{
    printf("bf2-64v64 injector\n\n");
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir(self);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    const std::wstring dll = dir + L"\\FrostyGameLink.dll";
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        printf("FrostyGameLink.dll is missing - keep it in the same folder as this exe.\n");
        Pause();
        return 1;
    }

    const DWORD pid = FindGame();
    if (pid == 0)
    {
        printf("Star Wars Battlefront II is not running. Start the game, wait for the main menu, then run this again.\n");
        Pause();
        return 1;
    }
    if (AlreadyLoaded(pid))
    {
        printf("Already loaded into this game session - nothing to do. Start Instant Action -> Supremacy.\n");
        Pause();
        return 0;
    }

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
                              PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (proc == nullptr)
    {
        const DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED && !IsElevated() && !(argc > 1 && wcscmp(argv[1], L"--elevated") == 0))
        {
            printf("The game runs as administrator - asking for administrator rights...\n");
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"runas", self, L"--elevated", dir.c_str(), SW_SHOWNORMAL)) > 32)
                return 0;
            printf("Administrator rights were not granted.\n");
        }
        else
            printf("Could not open the game process (error %lu).\n", err);
        Pause();
        return 1;
    }

    const size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool ok = remote != nullptr && WriteProcessMemory(proc, remote, dll.c_str(), bytes, nullptr);
    HANDLE thread = nullptr;
    if (ok)
    {
        auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        thread = CreateRemoteThread(proc, nullptr, 0, loadLibrary, remote, 0, nullptr);
        ok = thread != nullptr;
    }
    DWORD loaded = 0;
    if (ok)
    {
        WaitForSingleObject(thread, 15000);
        GetExitCodeThread(thread, &loaded);
        CloseHandle(thread);
    }
    if (remote)
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);

    if (ok && loaded != 0)
    {
        printf("Loaded. Now start Instant Action -> Supremacy.\n");
        printf("Log: %%LOCALAPPDATA%%\\bf2-64v64\\bf2_64v64.log\n");
    }
    else
        printf("Injection failed (error %lu).\n", GetLastError());
    Pause();
    return ok && loaded ? 0 : 1;
}
