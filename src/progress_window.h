// A small progress window with Cancel, for long plugin work during which Total Commander shows no
// progress dialog of its own (Space = CALCSIZE, command-line commands). TC's main thread is busy
// waiting for the plugin then, so the window lives in its own thread with its own message loop and
// stays responsive.
#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>

namespace s3tc {

class ProgressWindow {
public:
    // The window appears only if the work takes longer than showDelayMs.
    explicit ProgressWindow(std::wstring title, DWORD showDelayMs = 1000);
    ~ProgressWindow();
    ProgressWindow(const ProgressWindow&) = delete;
    ProgressWindow& operator=(const ProgressWindow&) = delete;

    void Update(const std::wstring& text, int percent);
    bool Cancelled() const { return cancelled_; }

private:
    static DWORD WINAPI ThreadMain(LPVOID self);
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void Run();

    std::wstring title_;
    DWORD showDelayMs_;
    RECT anchor_{};  // window rect of the caller's foreground window (TC), for centering
    HANDLE stop_ = nullptr;
    HANDLE thread_ = nullptr;
    std::atomic<HWND> hwnd_{nullptr};
    std::atomic<bool> cancelled_{false};
    std::mutex mutex_;
    std::wstring text_;
    int percent_ = 0;
    bool changed_ = false;
};

}  // namespace s3tc
