#ifndef AppVersion
  #error AppVersion is required
#endif
#ifndef PackageDir
  #error PackageDir is required
#endif
#ifdef TestInstall
  #define ProductName "NexPlay Package Test"
  #define ProductId "{{AF77F1CA-93A9-4D29-BD39-99A2F5473D18}"
  #define RunValue "NexPlay.PackageTest"
  #define RunKey "Software\NexPlay.PackageTest\Run"
  #define SetupName "NexPlay-TestSetup"
  #define ApplicationMutex "Local\NexPlay.PackageTest"
#else
  #define ProductName "NexPlay"
  #define ProductId "{{FF9670F0-8335-4E43-A0CD-8D0EB9BF6F42}"
  #define RunValue "NexPlay"
  #define RunKey "Software\Microsoft\Windows\CurrentVersion\Run"
  #define SetupName "NexPlay-" + AppVersion + "-Setup-windows-x64"
  #define ApplicationMutex "Local\NexPlay.Application"
#endif

[Setup]
AppId={#ProductId}
AppName={#ProductName}
AppVersion={#AppVersion}
AppPublisher=Zimonxx
AppPublisherURL=https://github.com/Zimonxx/nexplay
AppSupportURL=https://github.com/Zimonxx/nexplay/issues
AppUpdatesURL=https://github.com/Zimonxx/nexplay/releases
DefaultDirName={localappdata}\Programs\{#ProductName}
DefaultGroupName={#ProductName}
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.22000
OutputDir={#OutputDir}
OutputBaseFilename={#SetupName}
SetupIconFile={#IconFile}
WizardSmallImageFile={#BrandPng}
WizardStyle=modern dark
WizardSizePercent=110
DisableWelcomePage=no
DisableProgramGroupPage=yes
LicenseFile={#PackageDir}\LICENSE
InfoBeforeFile={#PackageDir}\README.txt
UninstallDisplayIcon={app}\nexplay.exe
UninstallDisplayName={#ProductName}
AppMutex={#ApplicationMutex}
CloseApplications=no
RestartApplications=no
Compression=lzma2/max
SolidCompression=yes
VersionInfoVersion={#AppVersion}.0
VersionInfoDescription=NexPlay Setup
VersionInfoProductName=NexPlay

[Languages]
Name: "polish"; MessagesFile: "compiler:Languages\Polish.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#PackageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#ProductName}"; Filename: "{app}\nexplay.exe"; AppUserModelID: "Zimonxx.NexPlay"
Name: "{autodesktop}\{#ProductName}"; Filename: "{app}\nexplay.exe"; Tasks: desktopicon; AppUserModelID: "Zimonxx.NexPlay"

[Run]
Filename: "{app}\nexplay.exe"; Description: "{cm:LaunchProgram,{#ProductName}}"; Flags: nowait postinstall skipifsilent unchecked

[Code]
const
  RunKey = '{#RunKey}';

procedure CurStepChanged(CurStep: TSetupStep);
var
  Command: String;
begin
  { Preserve the user's opt-in when moving/upgrading an existing installation. }
  if (CurStep = ssPostInstall) and
     RegQueryStringValue(HKCU, RunKey, '{#RunValue}', Command) and
     (Pos('nexplay.exe', Lowercase(Command)) > 0) and
     (Pos('--autostart', Command) > 0) then
    RegWriteStringValue(HKCU, RunKey, '{#RunValue}',
      '"' + ExpandConstant('{app}\nexplay.exe') + '" --autostart');
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Command: String;
begin
  { Never remove another installation's startup entry, clips or settings. }
  if (CurUninstallStep = usUninstall) and
     RegQueryStringValue(HKCU, RunKey, '{#RunValue}', Command) and
     (CompareText(Command, '"' + ExpandConstant('{app}\nexplay.exe') + '" --autostart') = 0) then
    RegDeleteValue(HKCU, RunKey, '{#RunValue}');
end;
