#pragma once
// Ported from SteamlessController by ddeverill (MIT), which is the upstream this
// fork takes its Steam-aware handoff design from. Relicensed GPL-3.0-or-later
// here along with the rest of this project. See LICENSES/ for the original
// notice.
#include <atomic>
#include <functional>
#include <thread>

// What Steam is doing right now, as far as controller ownership is concerned.
// Ordered by "how much Steam is going on" — transitions to a HIGHER value are
// reported immediately (yield the controller fast), transitions to a LOWER
// value are debounced (don't grab it during a Steam restart or game relaunch).
enum class SteamState {
    NoSteam   = 0,  // steam.exe not running
    SteamIdle = 1,  // Steam running, no game active (RunningAppID == 0)
    InGame    = 2,  // Steam running a game
};

// Watches the Steam process and its RunningAppID registry value, reporting
// debounced state transitions from a 2-second polling thread.
class SteamWatcher {
public:
    using SteamStateFn = std::function<void(SteamState)>;

    ~SteamWatcher() { Stop(); }

    // Starts the polling thread. Fires the callback once immediately with the
    // current state (so startup can assert the right mode), then on every
    // debounced transition. The callback runs on the watcher thread — marshal
    // to the UI thread (e.g. PostMessage) before touching UI or controller state.
    void Start(SteamStateFn onChange);
    void Stop();

    SteamState GetState() const { return m_state.load(); }

    // Immediate, unsynchronised probe. Used by the tray app when it needs the
    // current state outside a transition (e.g. when a mode is selected).
    static SteamState Detect();

private:
    void PollLoop();

    SteamStateFn            m_onChange;
    std::thread             m_thread;
    std::atomic<bool>       m_running{false};
    std::atomic<SteamState> m_state{SteamState::NoSteam};
};
