# Screenshots

Empty on purpose. The inherited screenshots were removed because they no longer
matched the app: they showed the old Win32 remap editor and a tray menu without
the **Automatic control** submenu.

Drop replacements here using the exact filenames below and uncomment the matching
lines in the top-level `README.md`.

## Shot list

### `remapper.png` — desktop editor

Tray icon → right-click → **Remap Buttons…**

Capture the whole window with one button selected so the editor panel is visible.
On a machine with the WebView2 runtime this is the Steam-styled editor; if it
looks like a grey Win32 dialog instead, WebView2 is missing and the fallback
opened — check the log for `[RemapWeb] WebView2 runtime missing`.

### `tray-icon.png` — tray menu

Right-click the tray icon with the **Automatic control** submenu expanded, so all
three modes and the checked one are visible. This is the main thing the old
screenshot lacked.

### `gamebar-widget.png` — Game Bar widget

`Win + G`, with the NonSteamController widget pinned and showing an active
profile.

## Capture tips

- Use a dark Windows theme so the editor's Steam palette does not clash with a
  light title bar.
- `Win + Shift + S` → window mode, or `Alt + PrtScn` for the focused window only.
- Keep them under roughly 1600px wide; GitHub scales them down anyway.
- PNG, not JPEG — these are UI screenshots with text and flat colour.
