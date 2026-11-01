; AceStudio installer (Inno Setup 6). Built by CI from the packaged build:
;   iscc /DAppVersion=0.10.0 /DDistDir=..\..\dist /DOutputDir=..\..\installer packaging\windows\AceStudio.iss
;
; - Installs for the current user by default (no admin); "Install for all
;   users" is offered too (needs admin).
; - Includes the embeddable Python (dist\python) so the AI features need no
;   separate Python, and the third-party licence texts.
; - Associates .acestudio files with AceStudio.
; - Uninstalling removes the program only: projects, settings, AI models,
;   licence key and trial record (user data folder) are kept.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef DistDir
  #define DistDir "..\..\dist"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\installer"
#endif

[Setup]
AppId={{6C2F1E2B-7A35-4C51-9E8B-ACE5717D0001}
AppName=AceStudio
AppVersion={#AppVersion}
AppVerName=AceStudio {#AppVersion}
AppPublisher=AceGears
AppCopyright=Copyright (C) AceGears
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\AceStudio
DefaultGroupName=AceStudio
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
LicenseFile=EULA.txt
OutputDir={#OutputDir}
OutputBaseFilename=AceStudio-{#AppVersion}-setup-x64
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ChangesAssociations=yes
CloseApplications=yes
RestartApplications=no
; AceStudio holds this mutex while it runs, so Setup and the uninstaller ask
; for it to be closed (on the test PC the uninstaller ran with AceStudio open).
AppMutex=AceGears.AceStudio.Running
UninstallDisplayIcon={app}\AceStudio.exe
UninstallDisplayName=AceStudio {#AppVersion}
; Branding: the AceStudio logo (app/resources/branding), unmodified, at each DPI.
SetupIconFile=..\..\app\resources\branding\acestudio.ico
WizardImageFile=branding\wizard-164.bmp,branding\wizard-192.bmp,branding\wizard-246.bmp,branding\wizard-314.bmp,branding\wizard-410.bmp
WizardSmallImageFile=branding\small-55.bmp,branding\small-64.bmp,branding\small-83.bmp,branding\small-92.bmp,branding\small-110.bmp,branding\small-119.bmp,branding\small-138.bmp
WizardImageStretch=no
WizardImageBackColor=$1A1412
; The user can choose the folder; upgrades reuse the previous one.
DisableDirPage=no
UsePreviousAppDir=yes
VersionInfoCompany=AceGears
VersionInfoDescription=AceStudio Setup
VersionInfoProductName=AceStudio
VersionInfoProductVersion={#AppVersion}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "association"; Description: "Open .acestudio projects with AceStudio"; GroupDescription: "File types:"

[Files]
; The program, Qt, the VC++ runtime, the AI worker and the bundled Python.
; "portable" is a development marker and must never reach customers.
Source: "{#DistDir}\*"; DestDir: "{app}"; Excludes: "portable,*.pdb,__pycache__"; Flags: recursesubdirs ignoreversion createallsubdirs
Source: "..\licences\*"; DestDir: "{app}\licences"; Flags: ignoreversion
Source: "EULA.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\AceStudio"; Filename: "{app}\AceStudio.exe"
Name: "{autodesktop}\AceStudio"; Filename: "{app}\AceStudio.exe"; Tasks: desktopicon

[Registry]
; HKA = HKCU for a per-user install, HKLM for all users.
Root: HKA; Subkey: "Software\Classes\.acestudio\OpenWithProgids"; ValueType: string; ValueName: "AceStudio.Project"; ValueData: ""; Flags: uninsdeletevalue; Tasks: association
Root: HKA; Subkey: "Software\Classes\AceStudio.Project"; ValueType: string; ValueName: ""; ValueData: "AceStudio Project"; Flags: uninsdeletekey; Tasks: association
Root: HKA; Subkey: "Software\Classes\AceStudio.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\AceStudio.exe,0"; Tasks: association
Root: HKA; Subkey: "Software\Classes\AceStudio.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\AceStudio.exe"" ""%1"""; Tasks: association
Root: HKA; Subkey: "Software\Classes\Applications\AceStudio.exe\SupportedTypes"; ValueType: string; ValueName: ".acestudio"; ValueData: ""; Flags: uninsdeletekey

[Run]
Filename: "{app}\AceStudio.exe"; Description: "{cm:LaunchProgram,AceStudio}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Byte-code caches Python wrote inside the install folder.
Type: filesandordirs; Name: "{app}\ai_worker\acestudio_worker\__pycache__"
Type: filesandordirs; Name: "{app}\ai_worker\acestudio_worker\tasks\__pycache__"
Type: filesandordirs; Name: "{app}\python\__pycache__"
