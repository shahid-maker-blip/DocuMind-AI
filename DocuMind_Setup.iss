#define MyAppName "DocuMind AI"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "DocuMind AI"

[Setup]
AppId={{B9F2D681-4A73-4C8A-9E52-1D7B6C3F8A20}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={localappdata}\DocuMind AI\Program
DefaultGroupName=DocuMind AI
UninstallDisplayIcon={app}\DocuMind_AI_Icon.ico
SetupIconFile=C:\msys64\home\HP\DocuMind\release\DocuMind\DocuMind_AI_Icon.ico
OutputDir=C:\msys64\home\HP\DocuMind\release
OutputBaseFilename=DocuMindAI_Setup
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
DisableProgramGroupPage=yes
WizardStyle=modern
CloseApplications=yes
Uninstallable=yes

[Files]
Source: "C:\msys64\home\HP\DocuMind\release\DocuMind\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{autodesktop}\DocuMind AI"; Filename: "{app}\Launch_DocuMind.vbs"; IconFilename: "{app}\DocuMind_AI_Icon.ico"; WorkingDir: "{app}"
Name: "{autoprograms}\DocuMind AI"; Filename: "{app}\Launch_DocuMind.vbs"; IconFilename: "{app}\DocuMind_AI_Icon.ico"; WorkingDir: "{app}"

[Run]
Filename: "{sys}\wscript.exe"; Parameters: """{app}\Launch_DocuMind.vbs"""; Description: "Launch DocuMind AI"; Flags: postinstall skipifsilent
