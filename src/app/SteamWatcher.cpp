// Ported from SteamlessController by ddeverill (MIT). See SteamWatcher.h.
#include "SteamWatcher.h"
#include "logging/Log.h"
#include <Windows.h>
#include <TlHelp32.h>
#include <iterator>

static constexpr int POLL_INTERVAL_MS         = 2000;
static constexpr int PENDING_POLL_INTERVAL_MS = 500;  // faster while confirming a downward change
static constexpr int POLL_SLICE_MS            = 100;  // wake often for fast Stop()
static constexpr int LESS_STEAM_POLL_COUNT    = 3;    // consecutive agreements before we take over

static bool IsSteamProcessRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"steam.exe") == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// Steam publishes the app id of the running game here and resets it to 0 on exit.
static bool IsGameRunningByAppId() {
    DWORD appId = 0;
    DWORD size  = sizeof(appId);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"RunningAppID",
                     RRF_RT_REG_DWORD, nullptr, &appId, &size) != ERROR_SUCCESS)
        return false;
    return appId != 0;
}

// Second opinion on RunningAppID: Steam also keeps a per-app "Running" DWORD
// under Apps\<id>. RunningAppID is a single slot and has been observed to go
// stale (it is not reset if Steam dies mid-game), whereas a stale per-app key
// disagreeing with it is a useful cross-check. Enumerating a few hundred
// subkeys of one already-open key is cheap.
static bool IsGameRunningByAppKeys() {
    HKEY apps = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\Apps", 0,
                      KEY_READ, &apps) != ERROR_SUCCESS)
        return false;

    bool running = false;
    wchar_t name[64];
    for (DWORD i = 0;; ++i) {
        DWORD nameLen = static_cast<DWORD>(std::size(name));
        if (RegEnumKeyExW(apps, i, name, &nameLen, nullptr, nullptr, nullptr, nullptr)
            != ERROR_SUCCESS)
            break;

        DWORD value = 0;
        DWORD size  = sizeof(value);
        if (RegGetValueW(apps, name, L"Running", RRF_RT_REG_DWORD, nullptr,
                         &value, &size) == ERROR_SUCCESS && value != 0) {
            running = true;
            break;
        }
    }
    RegCloseKey(apps);
    return running;
}

SteamState SteamWatcher::Detect() {
    if (!IsSteamProcessRunning()) return SteamState::NoSteam;
    return (IsGameRunningByAppId() || IsGameRunningByAppKeys())
               ? SteamState::InGame
               : SteamState::SteamIdle;
}

// Deliberately NOT extended to games launched outside Steam.
//
// It is tempting to treat any known game process as InGame, but yielding exists
// solely to hand the controller to Steam Input. A non-Steam game has no Steam
// Input to hand it to, so reporting InGame there would release the device to
// nobody and leave the game with no controller at all. Keeping control for
// non-Steam titles is the correct behaviour, not a gap to close.

void SteamWatcher::Start(SteamStateFn onChange) {
    Stop();
    m_onChange = std::move(onChange);
    m_running  = true;
    m_thread   = std::thread(&SteamWatcher::PollLoop, this);
}

void SteamWatcher::Stop() {
    if (m_running.exchange(false) && m_thread.joinable())
        m_thread.join();
}

void SteamWatcher::PollLoop() {
    SteamState reported = Detect();
    m_state = reported;
    if (m_onChange) m_onChange(reported);

    SteamState pending      = reported;
    int        pendingPolls = 0;

    // Watching HKCU\Software\Valve\Steam lets a game start or exit wake this
    // thread in milliseconds rather than on the next tick. Without it the
    // asymmetric debounce below costs up to POLL_INTERVAL_MS to notice a game,
    // and LESS_STEAM_POLL_COUNT intervals (~6s) to take the controller back.
    //
    // Best-effort: if any of this fails the loop falls back to pure polling,
    // which is exactly the old behaviour.
    HKEY   steamKey   = nullptr;
    HANDLE regEvent   = nullptr;
    bool   watchArmed = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0,
                      KEY_NOTIFY, &steamKey) == ERROR_SUCCESS) {
        regEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    auto armWatch = [&]() {
        if (!steamKey || !regEvent) return;
        ResetEvent(regEvent);
        // REG_NOTIFY_THREAD_AGNOSTIC keeps the registration alive independently
        // of this thread's message state; the subtree flag catches Apps\<id>.
        watchArmed = RegNotifyChangeKeyValue(
                         steamKey, TRUE,
                         REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME |
                             REG_NOTIFY_THREAD_AGNOSTIC,
                         regEvent, TRUE) == ERROR_SUCCESS;
    };

    armWatch();
    logging::Logf("[SteamWatcher] registry watch %s",
                  watchArmed ? "armed" : "unavailable (polling only)");

    while (m_running.load()) {
        // While a downward transition is pending we still want several agreeing
        // samples before acting, but there is no reason to space them 2s apart —
        // that is what made taking the controller back cost ~6s. Sampling faster
        // during the pending window keeps the anti-flap guarantee (still
        // LESS_STEAM_POLL_COUNT consecutive agreements) at a quarter the latency.
        const int waitMs = (pendingPolls > 0) ? PENDING_POLL_INTERVAL_MS : POLL_INTERVAL_MS;

        for (int waited = 0; waited < waitMs && m_running.load();
             waited += POLL_SLICE_MS) {
            if (watchArmed) {
                // Waits on the registry event but still wakes every slice, both
                // to observe Steam exiting (a process event, not a registry one)
                // and to keep Stop() responsive.
                if (WaitForSingleObject(regEvent, POLL_SLICE_MS) == WAIT_OBJECT_0) {
                    armWatch();  // one-shot notification; re-arm before reading
                    break;
                }
            } else {
                Sleep(POLL_SLICE_MS);
            }
        }
        if (!m_running.load()) break;

        const SteamState now = Detect();
        if (now == reported) {
            pendingPolls = 0;
            continue;
        }

        if (static_cast<int>(now) > static_cast<int>(reported)) {
            // More Steam activity (Steam appeared / game launched) — report
            // immediately so the controller is yielded before Steam needs it.
            reported     = now;
            pendingPolls = 0;
            m_state      = reported;
            logging::Logf("[SteamWatcher] state -> %d (immediate)", static_cast<int>(reported));
            if (m_onChange) m_onChange(reported);
        } else {
            // Less Steam activity (game quit / Steam gone) — must hold for
            // several consecutive polls so a Steam self-restart or game
            // relaunch doesn't cause ownership flapping.
            if (now != pending) {
                pending      = now;
                pendingPolls = 1;
            } else if (++pendingPolls >= LESS_STEAM_POLL_COUNT) {
                reported     = now;
                pendingPolls = 0;
                m_state      = reported;
                logging::Logf("[SteamWatcher] state -> %d (debounced)", static_cast<int>(reported));
                if (m_onChange) m_onChange(reported);
            }
        }
    }

    if (regEvent) CloseHandle(regEvent);
    if (steamKey) RegCloseKey(steamKey);
}
