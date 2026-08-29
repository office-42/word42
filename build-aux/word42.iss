; word42.iss - Inno Setup script for the Windows installer.
;
; Copyright (C) 2026 Andreas Røsdal
; SPDX-License-Identifier: GPL-3.0-or-later
;
;     iscc /DAppVersion=0.1.0 /DSourceDir=dist build-aux\word42.iss
;
; SourceDir is the tree bundle-windows.sh made (bin\word42.exe and the
; share and lib directories beside it); the installer copies it whole and
; puts word42 on the Start menu.  Nothing else touches the system: no
; file associations are taken unless the user ticks the task.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\dist"
#endif

[Setup]
AppId={{7B0E2C7A-6C1F-4B0B-9C6E-2D6B7E1F4A42}
AppName=Word42
AppVersion={#AppVersion}
AppVerName=Word42 {#AppVersion}
AppPublisher=Andreas Røsdal
AppPublisherURL=https://word42.org
AppSupportURL=https://github.com/office-42/word42
DefaultDirName={autopf}\Word42
DefaultGroupName=Word42
DisableProgramGroupPage=yes
LicenseFile={#SourceDir}\LICENSE
OutputDir=.
OutputBaseFilename=word42-{#AppVersion}-setup
SetupIconFile=word42.ico
UninstallDisplayIcon={app}\bin\word42.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequiredOverridesAllowed=dialog
ChangesAssociations=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "assocrtf"; Description: "Open Rich Text Format (.rtf) files with Word42"; GroupDescription: "File associations:"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Word42"; Filename: "{app}\bin\word42.exe"
Name: "{autodesktop}\Word42"; Filename: "{app}\bin\word42.exe"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Classes\.rtf\OpenWithProgids"; ValueType: string; ValueName: "Word42.rtf"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocrtf
Root: HKA; Subkey: "Software\Classes\Word42.rtf"; ValueType: string; ValueName: ""; ValueData: "Rich Text Document"; Flags: uninsdeletekey; Tasks: assocrtf
Root: HKA; Subkey: "Software\Classes\Word42.rtf\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\bin\word42.exe,0"; Tasks: assocrtf
Root: HKA; Subkey: "Software\Classes\Word42.rtf\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\bin\word42.exe"" ""%1"""; Tasks: assocrtf
Root: HKA; Subkey: "Software\Classes\Applications\word42.exe\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\bin\word42.exe"" ""%1"""; Flags: uninsdeletekey

[Run]
Filename: "{app}\bin\word42.exe"; Description: "{cm:LaunchProgram,Word42}"; Flags: nowait postinstall skipifsilent
