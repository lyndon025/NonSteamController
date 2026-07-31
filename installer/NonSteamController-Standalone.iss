; Standalone installer — app plus USB/IP driver, no Game Bar widget.
;
; Exists because the full installer (SteamControllerRemapper.iss) cannot be built
; without a code-signing certificate: the widget is an MSIX, and a sideloadable
; MSIX must be signed. Nothing else in the package needs signing, so dropping the
; widget makes a genuinely self-contained installer that CI can produce
; unattended — which is what makes releases a real download rather than an
; update-only zip.
;
; What is lost versus the full installer: the Xbox Game Bar widget, and the
; AppModelUnlock sideloading registry values it needs. Everything the tray app
; itself does is unaffected.
;
; Expects, relative to this file:
;   staging\Desktop\NonSteamController.exe   (see MyAppExeName below)
;   staging\Desktop\libVIIPER.dll
;   staging\usbip\USBip-win2-x64.exe

#define MyAppName "NonSteamController"
#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif
#define MyAppPublisher "lyndon025"
; Still the inherited executable name: renaming it would touch the widget
; sideload scripts and both workflows, which is a separate change.
#define MyAppExeName "Steam Controller Remapper.exe"

[Setup]
; Deliberately a different AppId from the full installer. Sharing one would let
; this build silently replace a widget-enabled install and orphan the widget.
AppId={{3C7E5A91-2B44-4D6E-9F81-7A5C3E1B9D04}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
OutputBaseFilename=NonSteamController-{#MyAppVersion}-Setup
OutputDir=output
SetupIconFile=..\resources\SteamControllerOFF.ico
Compression=lzma2
SolidCompression=yes
; Admin required for the USB/IP driver and for installing under Program Files.
PrivilegesRequired=admin
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={app}\{#MyAppExeName}
WizardStyle=modern
DisableProgramGroupPage=yes
CloseApplications=yes
CloseApplicationsFilter={#MyAppExeName}
MinVersion=10.0

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "staging\Desktop\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "staging\Desktop\libVIIPER.dll"; DestDir: "{app}"; Flags: ignoreversion
; Temp only — run on fresh install, then removed.
Source: "staging\usbip\USBip-win2-x64.exe"; DestDir: "{tmp}\usbip"; Flags: deleteafterinstall

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"

[Registry]
; Start with Windows, for the installing user. Value name matches the app's
; APP_NAME so its own startup toggle manages the same entry.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
  ValueType: string; ValueName: "{#MyAppName}"; \
  ValueData: """{app}\{#MyAppExeName}"""; Flags: uninsdeletevalue

[Run]
; Skipped when the driver is already present, otherwise every run would prompt
; for a reboot.
Filename: "{tmp}\usbip\USBip-win2-x64.exe"; \
  Parameters: "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-"; \
  StatusMsg: "Installing USB/IP driver..."; \
  Check: NeedsUsbIp; \
  Flags: waituntilterminated

Filename: "{app}\{#MyAppExeName}"; \
  Description: "Launch {#MyAppName}"; \
  Flags: nowait postinstall skipifsilent

[Code]
// usbip-win2 0.9.x registers usbip2_ude (UDE bus driver) and usbip2_filter;
// mausbip covers older builds.
function NeedsUsbIp(): Boolean;
begin
  Result := not (
    RegKeyExists(HKEY_LOCAL_MACHINE, 'SYSTEM\CurrentControlSet\Services\usbip2_ude') or
    RegKeyExists(HKEY_LOCAL_MACHINE, 'SYSTEM\CurrentControlSet\Services\usbip2_filter') or
    RegKeyExists(HKEY_LOCAL_MACHINE, 'SYSTEM\CurrentControlSet\Services\mausbip')
  );
end;
