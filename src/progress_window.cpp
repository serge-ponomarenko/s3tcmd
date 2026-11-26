#include "progress_window.h"

#include <commctrl.h>

#include "log.h"

namespace s3tc {

namespace {

const wchar_t kClassName[] = L"S3TcmdProgressWindow";
constexpr UINT kMsgClose = WM_APP + 1;
constexpr UINT_PTR kTimerId = 1;
constexpr int kIdText = 101, kIdBar = 102, kIdCancel = 103;

HINSTANCE ModuleHandle() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModuleHandle), &m);
    return m;
}

}  // namespace

ProgressWindow::ProgressWindow(std::wstring title, DWORD showDelayMs)
    : title_(std::move(title)), showDelayMs_(showDelayMs) {
    if (HWND fg = GetForegroundWindow()) GetWindowRect(fg, &anchor_);
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = CreateThread(nullptr, 0, ThreadMain, this, 0, nullptr);
}

ProgressWindow::~ProgressWindow() {
    SetEvent(stop_);
    if (HWND h = hwnd_.load()) PostMessageW(h, kMsgClose, 0, 0);
    if (thread_) {
        WaitForSingleObject(thread_, 5000);
        CloseHandle(thread_);
    }
    CloseHandle(stop_);
}

void ProgressWindow::Update(const std::wstring& text, int percent) {
    std::lock_guard<std::mutex> lock(mutex_);
    text_ = text;
    percent_ = percent;
    changed_ = true;
}

DWORD WINAPI ProgressWindow::ThreadMain(LPVOID self) {
    static_cast<ProgressWindow*>(self)->Run();
    return 0;
}

void ProgressWindow::Run() {
    // Short work: never show anything.
    if (WaitForSingleObject(stop_, showDelayMs_) == WAIT_OBJECT_0) return;

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);
    HINSTANCE inst = ModuleHandle();
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);  // fails harmlessly if already registered

    const int w = 520, h = 150;
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (anchor_.right > anchor_.left) {
        x = anchor_.left + (anchor_.right - anchor_.left - w) / 2;
        y = anchor_.top + (anchor_.bottom - anchor_.top - h) / 2;
    }
    // Deliberately not owned by TC's window: an owner in a busy thread would freeze this one too.
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, kClassName, title_.c_str(),
                                WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h, nullptr, nullptr, inst, this);
    Log(L"progress window: %s (error %lu)", hwnd ? L"created" : L"CreateWindow failed", hwnd ? 0UL : GetLastError());
    if (!hwnd) return;
    hwnd_ = hwnd;
    if (WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0) {  // closed while we were creating it
        DestroyWindow(hwnd);
        return;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    hwnd_ = nullptr;
}

LRESULT CALLBACK ProgressWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<ProgressWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            HINSTANCE inst = cs->hInstance;
            HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            HWND text = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_PATHELLIPSIS,
                                        12, 10, 486, 36, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdText)), inst, nullptr);
            HWND bar = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, 12, 52, 486, 18, hwnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBar)), inst, nullptr);
            HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                          408, 80, 90, 26, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCancel)), inst, nullptr);
            for (HWND c : {text, cancel}) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
            SendMessageW(bar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
            SetTimer(hwnd, kTimerId, 200, nullptr);
            return 0;
        }
        case WM_TIMER:
            if (self) {
                std::wstring text;
                int pct = 0;
                bool changed = false;
                {
                    std::lock_guard<std::mutex> lock(self->mutex_);
                    changed = self->changed_;
                    self->changed_ = false;
                    text = self->text_;
                    pct = self->percent_;
                }
                if (changed) {
                    SetDlgItemTextW(hwnd, kIdText, text.c_str());
                    SendDlgItemMessageW(hwnd, kIdBar, PBM_SETPOS, pct, 0);
                }
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == kIdCancel && self) {
                self->cancelled_ = true;
                SetDlgItemTextW(hwnd, kIdCancel, L"Cancelling...");
                EnableWindow(GetDlgItem(hwnd, kIdCancel), FALSE);
            }
            return 0;
        case WM_CLOSE:  // the [x] button cancels; the window itself closes when the work ends
            if (self) SendMessageW(hwnd, WM_COMMAND, kIdCancel, 0);
            return 0;
        case kMsgClose:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kTimerId);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

}  // namespace s3tc
