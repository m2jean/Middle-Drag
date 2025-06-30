// Dragg.cpp : Defines the entry point for the application.
//

#include <SDKDDKVer.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winuser.h>
#include <debugapi.h>
#include <format>

#include <string>

HHOOK g_hHook = nullptr;

bool g_middleButtonDown = false;
POINT g_lastPoint{};
bool g_moved = false;

UINT_PTR g_timer = 0;

constexpr DWORD WM_MCLICK = WM_USER + 1;

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

LRESULT CALLBACK MouseLowLevelProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        const MSLLHOOKSTRUCT &mouseHook = *reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);  // NOLINT(performance-no-int-to-ptr)
        if (!(mouseHook.flags & LLMHF_INJECTED)) {
            switch (wParam) {
            case WM_MBUTTONDOWN:
                g_middleButtonDown = true;
                g_moved = false;
                g_lastPoint = mouseHook.pt;
                return 1;
            case WM_MOUSEMOVE:
                if (g_middleButtonDown) {
                    if (!g_moved)
                    {
                        // Start touching only when the movement is larger than the tolerance
                        constexpr LONG tolerance = 2;
                        LONG dx = std::abs(g_lastPoint.x - mouseHook.pt.x);
                        LONG dy = std::abs(g_lastPoint.y - mouseHook.pt.y);
                        if (dx <= tolerance && dy <= tolerance)
                        {
                            break;
                        }

                        g_lastPoint = mouseHook.pt;
                        g_moved = true;
                        InjectTouch(true, false);
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
                return 1;
            default:
                break;
            }
        }
    }

    return CallNextHookEx(g_hHook, nCode, wParam, lParam);
}

int APIENTRY wWinMain([[maybe_unused]] _In_ HINSTANCE hInstance,
    [[maybe_unused]] _In_opt_ HINSTANCE hPrevInstance,
    [[maybe_unused]] _In_ LPWSTR    lpCmdLine,
    [[maybe_unused]] _In_ int       nCmdShow)
{
    if (!InitializeTouchInjection(1, TOUCH_FEEDBACK_DEFAULT)) {
        LogErr("InitializeTouchInjection Failed", std::to_string(GetLastError()).data());
        return -1;
    }

    g_hHook = SetWindowsHookEx(WH_MOUSE_LL, MouseLowLevelProc, nullptr, 0);
    if (!g_hHook) {
        LogErr("SetWindowsHookEx Failed", std::to_string(GetLastError()).data());
        return -1;
    }

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
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

    UnhookWindowsHookEx(g_hHook);
    return (int)msg.wParam;
}
