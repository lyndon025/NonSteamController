#pragma once
#include "PaddleConfig.h"
#include "RemapBackend.h"
#include "RemapIpcServer.h"
#include "RemapWebWindow.h"
#include "SteamWatcher.h"
#include <Windows.h>
#include <memory>
#include <string>
#include <vector>

class ControllerManager;
class PaddleConfigWindow;

// Who decides when this app owns the physical controller.
//
// Ported from SteamlessController, which is the behaviour this fork exists to
// pick up. Before this, the choice was a single "auto enable" checkbox that
// yielded whenever steam.exe was running at all — so steamless mode could never
// be active with Steam open, and emulation only worked with Steam shut down.
enum class AutoMode {
    Manual        = 0,  // tray toggle only
    OffWhileSteam = 1,  // yield whenever steam.exe is running (the old behaviour)
    OffOnlyInGame = 2,  // keep the controller while Steam idles, yield only for games
};

class TrayApp {
public:
    TrayApp();
    ~TrayApp();

    bool Init(HINSTANCE hInstance);
    int  Run();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void AddTrayIcon();
    void RemoveTrayIcon();
    void UpdateTrayIcon(bool connected, bool gameModeActive, bool outputBackendMissing = false);
    void ShowOutputBackendBalloon();
    void ShowContextMenu();
    void LoadSettings();
    void SaveSettings();
    void LoadPaddleConfig();
    void ShowRemapEditor();
    void ShowPaddleConfigWindow();
    bool GetAutoSwitchProfiles() const;
    void SetAutoSwitchProfiles(bool enabled);
    std::wstring GetDetectedGameProfileId() const;
    std::string HandleIpcRequest(const std::string& request);
    void PublishWidgetState();
    void ProcessWidgetBridge();
    void ApplyProfileById(const std::wstring& profileId, bool force = false);
    void ReconcileAutoMode();

    // Auto-mode plumbing.
    void SetAutoMode(AutoMode mode);
    bool WantControl(SteamState state) const;
    void ApplySteamState(SteamState state);
    bool IsStartupEnabled() const;
    void SetStartupEnabled(bool enabled);
    void CheckForUpdates(bool userInitiated);
    void CheckControllerReportSignature();
    void ShowFirmwareChangedBalloon(const std::wstring& signature);

    HWND                               m_hwnd      = nullptr;
    HINSTANCE                          m_hInstance = nullptr;
    UINT                               m_wmTaskbar = 0;
    HICON                              m_iconOff   = nullptr;
    HICON                              m_iconOn    = nullptr;
    std::unique_ptr<ControllerManager> m_controller;
    std::unique_ptr<PaddleConfigWindow> m_paddleConfigWindow;
    std::unique_ptr<RemapWebWindow>     m_remapWebWindow;
    std::unique_ptr<RemapIpcServer>    m_ipcServer;
    SteamWatcher                       m_steamWatcher;
    AutoMode                           m_autoMode = AutoMode::OffWhileSteam;
    bool                               m_autoSwitchProfiles      = false;
    bool                               m_manualProfileOverride   = false;
    // m_steamRunning is derived from m_steamState and kept only because the
    // Game Bar widget payload and a few UI gates already report it.
    SteamState                         m_steamState              = SteamState::NoSteam;
    bool                               m_steamRunning            = false;
    bool                               m_reportSignatureChecked  = false;
    ULONGLONG                          m_lastReconnectAttemptTick = 0;
    bool                               m_smapiWasRunning          = false;
    ULONGLONG                          m_smapiDetachedMs          = 0;
    std::string                        m_lastWidgetStateJson;
    std::string                        m_lastWidgetRequestId;
    std::wstring                       m_savedControllerReportSignature;
    RemapBackend                       m_remapBackend;

    static constexpr UINT IDM_TOGGLE        = 1001;
    static constexpr UINT IDM_EXIT          = 1002;
    static constexpr UINT IDM_TRACKPAD      = 1003;
    static constexpr UINT IDM_BACKBUTTONS   = 1004;
    static constexpr UINT IDM_LEFT_TRACKPAD = 1005;
    static constexpr UINT IDM_STARTUP       = 1006;
    static constexpr UINT IDM_OUTPUT_X360   = 1008;
    static constexpr UINT IDM_OUTPUT_DS4    = 1009;
    static constexpr UINT IDM_CHECK_UPDATES  = 1010;
    static constexpr UINT IDM_MODE_MANUAL   = 1011;
    static constexpr UINT IDM_MODE_STEAM    = 1012;
    static constexpr UINT IDM_MODE_GAME     = 1013;
    static constexpr UINT IDM_CONFIGURE_PADDLES   = 1400;
    static constexpr UINT WM_TRAY          = WM_APP + 1;
    static constexpr UINT WM_STEAMSTATE    = WM_APP + 2;
    static constexpr UINT TRAY_UID         = 1;
    static constexpr UINT TIMER_STEAM_POLL  = 1;
    static constexpr UINT STEAM_POLL_MS     = 1000;
    static constexpr UINT TIMER_SMAPI_WATCH = 3;
    static constexpr UINT SMAPI_WATCH_MS    = 250;
    static constexpr ULONGLONG SMAPI_REINIT_GRACE_MS = 10000;
    static constexpr UINT RECONNECT_BACKOFF_MS  = 3000;
    static constexpr ULONGLONG GAME_ACTIVE_GRACE_MS  = 30000;
};
