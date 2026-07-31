#pragma once
// Steam-styled remap editor, hosted in WebView2.
//
// Why WebView2 rather than restyling the existing window: the inherited editor
// is built from raw Win32 controls, and Win32 combo boxes and list boxes cannot
// be themed to look like anything else without full owner-draw. SteamlessController
// solved the same problem by hosting HTML in WebView2, which is where its
// noticeably nicer editor comes from; this follows that approach and reuses its
// palette so the two look like the same family of tool.
//
// The old Win32 window is kept as a fallback for machines without the WebView2
// runtime, and still owns the surfaces this one does not cover yet (macro
// recording, game-source management).
//
// Bridge design: actions cross as the same human-readable strings the config
// file uses ("GAMEPAD:A+B", "KEY:CTRL+C", "MENU", "NONE", with "|RAPID"), so no
// action model has to be duplicated in JavaScript. Display text comes from
// PaddleConfig::Describe, and input goes back through
// PaddleConfig::ParseActionString.

#include <Windows.h>
#include <wrl.h>
#include <WebView2.h>
#include <functional>
#include <string>
#include "VirtualController.h"

class RemapWebWindow {
public:
    // Called when the user applies a change to one button. The action string is
    // whatever the editor composed; the caller parses and stores it.
    using ApplyFn = std::function<void(int buttonIndex, const std::wstring& actionString)>;
    // Asked for the current bindings whenever the editor needs to refresh.
    using QueryFn = std::function<PaddleActionBindings()>;
    // Invoked when the user asks for the surfaces this editor does not host yet.
    using OpenAdvancedFn = std::function<void()>;
    // Profile list for the selector, and switching to one.
    using ProfilesFn      = std::function<std::vector<std::wstring>()>;
    using SwitchProfileFn = std::function<void(const std::wstring& profileId)>;
    // Auto-switch-profiles toggle state and setter.
    using AutoSwitchGetFn = std::function<bool()>;
    using AutoSwitchSetFn = std::function<void(bool)>;

    struct Callbacks {
        QueryFn         query;
        ApplyFn         apply;
        OpenAdvancedFn  openAdvanced;
        ProfilesFn      profiles;
        SwitchProfileFn switchProfile;
        AutoSwitchGetFn autoSwitchGet;
        AutoSwitchSetFn autoSwitchSet;
    };

    RemapWebWindow() = default;
    ~RemapWebWindow();
    RemapWebWindow(const RemapWebWindow&) = delete;
    RemapWebWindow& operator=(const RemapWebWindow&) = delete;

    // True when the WebView2 runtime is present. Check before Open so the caller
    // can fall back to the Win32 editor instead of showing an error.
    static bool IsRuntimeAvailable();

    // Opens, or re-focuses if already open. Returns false if the window could
    // not be created, in which case the caller should use the fallback editor.
    bool Open(HINSTANCE hInst, const std::wstring& profileId, Callbacks cb);

    void Close();
    void BringToFront() const;
    bool IsOpen() const { return m_hwnd != nullptr; }

    // Pushes current bindings into the page (e.g. after a profile switch).
    void Refresh(const std::wstring& profileId);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void CreateWebViewAsync(HWND hwnd);
    void OnWebMessage(const std::wstring& json);
    void PostState();
    void ResizeWebView();

    HWND       m_hwnd  = nullptr;
    HINSTANCE  m_hInst = nullptr;
    std::wstring m_profileId;
    Callbacks    m_cb;

    Microsoft::WRL::ComPtr<ICoreWebView2Environment> m_env;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller>  m_controller;
    Microsoft::WRL::ComPtr<ICoreWebView2>            m_webview;

    static RemapWebWindow* s_instance;

    static constexpr wchar_t CLASS_NAME[] = L"NonSteamControllerRemapWeb";
    static constexpr int WINDOW_W = 900;
    static constexpr int WINDOW_H = 720;
};
