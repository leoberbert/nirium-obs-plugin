; Inno Setup script. Build with: iscc /DAppVersion=0.1.3 installer\niriun-obs.iss
; Expects `cmake --install build --prefix dist` to have been run (dist\obs-plugins, dist\data).
#ifndef AppVersion
  #define AppVersion "0.1.3"
#endif

[Setup]
AppId={{6F1B7D52-3C0E-4B8A-9A57-2D4E8C1F7A90}
AppName=Niriun - plugin para OBS Studio
AppVersion={#AppVersion}
AppPublisher=Leonardo Berbert
AppPublisherURL=https://github.com/leoberbert/nirium-obs-plugin
DefaultDirName={code:ObsDir}
DirExistsWarning=no
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile=..\LICENSE
OutputDir=..\dist-installer
OutputBaseFilename=niriun-obs-{#AppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=Niriun - plugin para OBS Studio

[Languages]
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; OBS 32 and older: obs-plugins\64bit + data\obs-plugins\<name>. OBS 33+ (has a "core" folder): plugins\<name>\<name>.dll
; + plugins\<name>\data. OBS 33 still loads the old layout, but only as "legacy"; the new one is used when available.
Source: "..\dist\obs-plugins\64bit\niriun-obs.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion; Check: not NewLayout
Source: "..\dist\data\obs-plugins\niriun-obs\locale\*"; DestDir: "{app}\data\obs-plugins\niriun-obs\locale"; Flags: ignoreversion recursesubdirs; Check: not NewLayout
Source: "..\dist\obs-plugins\64bit\niriun-obs.dll"; DestDir: "{app}\plugins\niriun-obs"; Flags: ignoreversion; Check: NewLayout
Source: "..\dist\data\obs-plugins\niriun-obs\locale\*"; DestDir: "{app}\plugins\niriun-obs\data\locale"; Flags: ignoreversion recursesubdirs; Check: NewLayout

[InstallDelete]
; Upgrading OBS 32 -> 33 would leave the old copy behind and the plugin would load twice.
Type: files; Name: "{app}\obs-plugins\64bit\niriun-obs.dll"; Check: NewLayout
Type: filesandordirs; Name: "{app}\data\obs-plugins\niriun-obs"; Check: NewLayout
Type: filesandordirs; Name: "{app}\plugins\niriun-obs"; Check: not NewLayout

[Code]
function NewLayout: Boolean;
begin
  Result := DirExists(AddBackslash(WizardDirValue) + 'core');
end;

// The folder OBS Studio was installed to, from its own installer's registry key; the usual place otherwise.
function ObsDir(Param: string): string;
begin
  if not RegQueryStringValue(HKLM64, 'SOFTWARE\OBS Studio', '', Result) then
    Result := ExpandConstant('{commonpf64}\obs-studio');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = wpSelectDir then
    if not FileExists(AddBackslash(WizardDirValue) + 'bin\64bit\obs64.exe') then
      Result := MsgBox('O OBS Studio não foi encontrado nesta pasta. Instalar mesmo assim?',
        mbConfirmation, MB_YESNO) = IDYES;
end;
