// Dragg.cpp : Defines the entry point for the application.
//

#include <SDKDDKVer.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winuser.h>
#include <debugapi.h>
#include <Psapi.h>

#include <string>
#include <format>
#include <fstream>
#include <unordered_set>
#include <filesystem>
#include <wil/resource.h>

HHOOK g_hHook = nullptr;

bool g_middleButtonDown = false;
POINT g_lastPoint{};
bool g_moved = false;

UINT_PTR g_timer = 0;

std::unordered_set<std::wstring> g_excludedProcesses;
std::wstring g_currentProcess;

constexpr DWORD WM_MCLICK = WM_USER + 1;

class ConfigWatcher
{
public:
    explicit ConfigWatcher(std::filesystem::path path) : m_configPath(std::move(path))
    {
        load();
        startWatching();
    }

    HANDLE waitHandle() const { return m_hChangeDir ? m_changeEvent.get() : nullptr; }

    void onSignaled()
    {
        DWORD bytesReturned = 0;
        if (GetOverlappedResult(m_hChangeDir.get(), &m_overlapped, &bytesReturned, FALSE) && bytesReturned > 0)
        {
            const FILE_NOTIFY_INFORMATION* fni = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(m_buf);
            do
            {
                std::wstring changed(fni->FileName, fni->FileNameLength / sizeof(WCHAR));
                if (_wcsicmp(changed.c_str(), m_configPath.filename().c_str()) == 0)
                {
                    load();
                    break;
                }
                if (fni->NextEntryOffset == 0)
                    break;
                fni = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(
                    reinterpret_cast<const char*>(fni) + fni->NextEntryOffset);
            } while (true);
        }
        subscribe();
    }

private:
    void load()
    {
        std::unordered_set<std::wstring> processes;
        std::wifstream file(m_configPath);
        if (file.is_open())
        {
            std::wstring line;
            while (std::getline(file, line))
            {
                // Strip leading/trailing whitespace and skip empty lines / comments
                auto start = line.find_first_not_of(L" \t\r\n");
                if (start == std::wstring::npos) continue;
                auto end = line.find_last_not_of(L" \t\r\n");
                line = line.substr(start, end - start + 1);
                if (line.empty() || line[0] == L'#') continue;
                processes.insert(line);
            }
        }
        else
        {
            // Create a default config file
            std::wofstream out(m_configPath);
            return;
        }
        g_excludedProcesses = std::move(processes);
        OutputDebugStringA(std::format("Config loaded: {} excluded processes\n", g_excludedProcesses.size()).c_str());
    }

    void startWatching()
    {
        std::wstring dir = m_configPath.parent_path().wstring();
        m_hChangeDir.reset(CreateFileW(
            dir.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr));

        if (!m_hChangeDir)
        {
            OutputDebugStringA(std::format("CreateFile (dir watch) failed: {}\n", GetLastError()).c_str());
            return;
        }

        m_overlapped.hEvent = m_changeEvent.get();
        subscribe();
    }

    void subscribe()
    {
        m_changeEvent.ResetEvent();
        m_overlapped = {};
        m_overlapped.hEvent = m_changeEvent.get();
        ReadDirectoryChangesW(
            m_hChangeDir.get(),
            m_buf, sizeof(m_buf),
            FALSE,
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME,
            nullptr,
            &m_overlapped,
            nullptr);
    }

    std::filesystem::path m_configPath;
    wil::unique_hfile m_hChangeDir;
    OVERLAPPED m_overlapped{};
    char m_buf[4096]{};
    wil::unique_event m_changeEvent{ wil::EventOptions::ManualReset };
};


void LogErr(const char* title, const char* text)
{
    MessageBoxA(nullptr, text, title, MB_OK | MB_ICONINFORMATION);
}

void InjectTouch(bool isDown, bool isUp) {
    OutputDebugStringA(std::format("Mouse {} at ({}, {})\n",
        isDown ? "Down" : (isUp ? "Up" : "Move"), g_lastPoint.x, g_lastPoint.y).c_str());

    POINTER_TOUCH_INFO touchInfo{ .pointerInfo = { .pointerType = PT_TOUCH, .ptPixelLocation = g_lastPoint } };
    if (isDown)
        touchInfo.pointerInfo.pointerFlags = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_DOWN;
    else if (isUp)
        touchInfo.pointerInfo.pointerFlags = POINTER_FLAG_UP;
    else
        touchInfo.pointerInfo.pointerFlags = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_UPDATE;

    if (!InjectTouchInput(1, &touchInfo)) {
        //LogErr("InjectTouchInput Failed",  std::to_string(GetLastError()).data());
        OutputDebugStringA(std::format("InjectTouchInput Failed: {}\n", GetLastError()).c_str());
    }
}

void HandleMButtonUp()
{
    g_middleButtonDown = false;
    KillTimer(nullptr, g_timer);

    if (g_moved)
    {
        InjectTouch(false, true);
    }
    else
    {
        // Calling SendInput inside a hook proc is very slow
        PostMessage(nullptr, WM_MCLICK, 0, 0);
    }
}

LRESULT CALLBACK MouseLowLevelProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        const MSLLHOOKSTRUCT &mouseHook = *reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);  // NOLINT(performance-no-int-to-ptr)
        if (!(mouseHook.flags & LLMHF_INJECTED)) {
            if (g_excludedProcesses.contains(g_currentProcess)) {
                // End touch injection first
                if (g_middleButtonDown)
                    HandleMButtonUp();

                return CallNextHookEx(g_hHook, nCode, wParam, lParam);
            }

            switch (wParam) {
            case WM_MBUTTONDOWN:
                g_middleButtonDown = true;
                g_moved = false;
                g_lastPoint = mouseHook.pt;
                OutputDebugStringA(std::format("Middle button down at ({}, {})\n", g_lastPoint.x, g_lastPoint.y).c_str());
                return 1;
            case WM_MOUSEMOVE:
                if (g_middleButtonDown) {
                    if (!g_moved)
                    {
                        // Start touching only when the movement is larger than the tolerance
                        constexpr LONG tolerance = 10;
                        LONG dx = std::abs(g_lastPoint.x - mouseHook.pt.x);
                        LONG dy = std::abs(g_lastPoint.y - mouseHook.pt.y);
                        if (dx <= tolerance && dy <= tolerance)
                        {
                            break;
                        }

                        g_moved = true;
                        InjectTouch(true, false);
                        g_lastPoint = mouseHook.pt;
                        g_timer = SetTimer(nullptr, g_timer, 100, nullptr);
                    }
                    else
                    {
                        g_lastPoint = mouseHook.pt;
                        InjectTouch(false, false);
                    }
                }
                break;
            case WM_MBUTTONUP:
                HandleMButtonUp();
                return 1;
            default:
                break;
            }
        }
    }

    return CallNextHookEx(g_hHook, nCode, wParam, lParam);
}

void UpdateForegroundProcess(HWND hwnd)
{
    if (!hwnd) {
        g_currentProcess.clear();
        OutputDebugString(L"No foreground process");
        return;
    }

    HWND foregroundWin = GetForegroundWindow();
    if (foregroundWin != hwnd) {
        // Maybe the window has used MA_NOACTIVATE
        OutputDebugString(L"Foreground window not the same.\n");
        return;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    wil::unique_handle hProcess{ OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid) };
    if (hProcess) {
        wchar_t processName[MAX_PATH];
        if (GetModuleFileNameEx(hProcess.get(), nullptr, processName, MAX_PATH)) {
            g_currentProcess = std::filesystem::path(processName).filename().wstring();
            OutputDebugString(std::format(L"Foreground process: {}\n", g_currentProcess).c_str());
        }
    }
}

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG, LONG, DWORD, DWORD)
{
    if (event == EVENT_SYSTEM_FOREGROUND) {
        UpdateForegroundProcess(hwnd);
    }
}

int APIENTRY wWinMain([[maybe_unused]] _In_ HINSTANCE hInstance,
    [[maybe_unused]] _In_opt_ HINSTANCE hPrevInstance,
    [[maybe_unused]] _In_ LPWSTR    lpCmdLine,
    [[maybe_unused]] _In_ int       nCmdShow)
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    ConfigWatcher watcher{ std::filesystem::path(exePath).replace_filename(L"excluded_processes.txt") };

    if (!InitializeTouchInjection(1, TOUCH_FEEDBACK_DEFAULT)) {
        LogErr("InitializeTouchInjection Failed", std::to_string(GetLastError()).data());
        return -1;
    }

    wil::unique_hhook hHook{ SetWindowsHookEx(WH_MOUSE_LL, MouseLowLevelProc, nullptr, 0) };
    if (!hHook) {
        LogErr("SetWindowsHookEx Failed", std::to_string(GetLastError()).data());
        return -1;
    }
    g_hHook = hHook.get();

    wil::unique_hwineventhook hEventHook{ SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT) };

    HWND foregroundWin = GetForegroundWindow();
    UpdateForegroundProcess(foregroundWin);

    MSG msg{};
    for (;;)
    {
        HANDLE waitHandle = watcher.waitHandle();
        DWORD nHandles = waitHandle ? 1 : 0;
        DWORD waitResult = MsgWaitForMultipleObjects(nHandles, &waitHandle, FALSE, INFINITE, QS_ALLINPUT);

        if (waitResult == WAIT_OBJECT_0 && nHandles > 0)
        {
            watcher.onSignaled();
            continue;
        }

        // Process all pending window messages
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                return (int)msg.wParam;

            TranslateMessage(&msg);
            DispatchMessage(&msg);

            if (msg.message == WM_MCLICK) {
                INPUT input{ .type = INPUT_MOUSE, .mi = { .dx = g_lastPoint.x, .dy = g_lastPoint.y, .dwFlags = MOUSEEVENTF_MIDDLEDOWN } };
                SendInput(1, &input, sizeof(INPUT));
                input.mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
                SendInput(1, &input, sizeof(INPUT));
            }
            else if (msg.message == WM_TIMER && g_middleButtonDown)
            {
                InjectTouch(false, false);
            }
        }
    }
}
