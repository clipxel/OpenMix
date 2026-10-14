; Instalador de OpenMix para Windows (Inno Setup 6). Lo compila .github/workflows/release.yml:
;   ISCC /DAppVersion=<versión> /DSourceDir=<carpeta con OpenMix.exe> installer\openmix.iss
; Se instala por usuario (sin permisos de administrador), así la actualización automática
; puede reemplazarlo sola.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\build\OpenMix_artefacts\Release"
#endif

[Setup]
AppId={{6F1C2E7A-0B8D-4E57-9C3A-0A11D3C5E201}
AppName=OpenMix
AppVersion={#AppVersion}
AppVerName=OpenMix {#AppVersion}
AppPublisher=OpenMix
AppPublisherURL=https://clipxel.github.io/OpenMix/
AppSupportURL=https://github.com/clipxel/OpenMix/discussions
AppUpdatesURL=https://github.com/clipxel/OpenMix/releases
DefaultDirName={localappdata}\Programs\OpenMix
DefaultGroupName=OpenMix
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=OpenMix-{#AppVersion}-Windows-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\OpenMix.exe
UninstallDisplayName=OpenMix
CloseApplications=yes
RestartApplications=no
LicenseFile={#SourceDir}\LICENSE.txt

[Languages]
Name: "es"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#SourceDir}\OpenMix.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\OpenMix"; Filename: "{app}\OpenMix.exe"
Name: "{autodesktop}\OpenMix"; Filename: "{app}\OpenMix.exe"; Tasks: desktopicon

[Run]
; Al terminar la instalación normal: casilla "Abrir OpenMix".
Filename: "{app}\OpenMix.exe"; Description: "{cm:LaunchProgram,OpenMix}"; Flags: nowait postinstall skipifsilent
