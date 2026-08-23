; Inno Setup script for SignalBox Category Monitor by TheNerdyBox.
;
; Installs an OBS Studio plugin, which means writing into the OBS install
; directory rather than one of our own. The wizard locates OBS from the
; registry and offers that path instead of asking the user to guess.
;
; Branding assets are generated from the shared TheNerdyBox brand set - the
; "escape" mark on the void ground. Palette is taken from the live site
; (globals.css), not invented here. Coral (#ff6b4a) is deliberately unused:
; the brand rules reserve it for the escaping block and nothing else.
;
; Compile with ISCC (Inno Setup 6).

#define AppName        "SignalBox Category Monitor"
#define AppPublisher   "TheNerdyBox"
; Version comes from buildspec.json via CMake - see installer-version.iss.
#include "installer-version.iss"
#define HomeURL        "https://thenerdybox.com"

; NOTE: the release-notes page does not exist yet - it is still in design.
; Change this one line when it is live.
#define ReleaseNotesURL "https://thenerdybox.com/signalbox/release-notes"

[Setup]
; Identity is pinned to this GUID, not to the display name. Inno falls back
; to AppName when no AppId is set, so renaming the product would otherwise
; create a second Add/Remove entry and orphan the first instead of upgrading
; in place. This value must never change.
AppId={{A7F3C29E-5B41-4D8A-9E62-1C0F8B3D7A54}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#HomeURL}
AppSupportURL={#HomeURL}
AppUpdatesURL={#ReleaseNotesURL}
VersionInfoDescription={#AppName} Setup
VersionInfoProductName={#AppName}
VersionInfoVersion={#AppVersion}
VersionInfoCompany={#AppPublisher}

DefaultDirName={autopf}\obs-studio
DirExistsWarning=no
DisableProgramGroupPage=yes
; Inno 6 skips the Welcome page by default, which meant the first screen was
; Select Destination - small corner icon only. Turning it back on gives the
; opening screen the same full banner the finish screen has.
DisableWelcomePage=no
PrivilegesRequired=admin

OutputDir=setup
OutputBaseFilename=SignalBox-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes

; --- Branding -------------------------------------------------------------
WizardStyle=modern
SetupIconFile=installer-assets\signalbox.ico
WizardImageFile=installer-assets\wizard-large.bmp,installer-assets\wizard-large@2x.bmp
WizardSmallImageFile=installer-assets\wizard-small.bmp,installer-assets\wizard-small@2x.bmp
WizardImageStretch=no

UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\obs-plugins\64bit\signalbox.dll
UninstallFilesDir={app}\signalbox-uninstall

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
english.ObsNotFound=OBS Studio could not be found automatically.%n%nChoose the folder OBS is installed in on the next page - the one containing obs-plugins and data.
english.ObsRunning=OBS Studio appears to be running.%n%nClose it before continuing, or the plugin file will be locked and cannot be replaced.
english.NotAnObsFolder=That folder does not look like an OBS Studio installation - no obs-plugins folder was found inside it.%n%nContinue anyway?
english.KeepSettings=Keep your SignalBox settings and Twitch connection?%n%nChoosing No also removes your saved category overrides and signs you out.
english.LegacyRemovalFailed=An older SignalBox installation was found but could not be removed automatically.%n%nUninstall "SignalBox Category Monitor" from Apps & Features first, then run this installer again.
english.RunObs=Start OBS Studio
english.RunObsAdmin=Start OBS Studio as administrator
english.ViewNotes=View the release notes
english.VisitHome=Visit thenerdybox.com

[InstallDelete]
; Runs before any file is copied. The plugin binaries are deleted rather than
; merely overwritten so a file that a NEWER version stopped shipping cannot
; linger and still get loaded: OBS enumerates whatever is in these locations,
; it does not consult a manifest, so an orphaned file from an older build is
; indistinguishable from a current one.
;
; Only paths this product owns are listed. {app} is OBS's own install
; directory, shared with OBS itself and every other plugin - nothing here may
; ever be a broad wildcard or a directory OBS owns.
Type: files;          Name: "{app}\obs-plugins\64bit\signalbox.dll"
Type: files;          Name: "{app}\obs-plugins\64bit\signalbox.pdb"
Type: filesandordirs; Name: "{app}\data\obs-plugins\signalbox"

[Files]
Source: "build_x64\rundir\RelWithDebInfo\signalbox.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
Source: "build_x64\rundir\RelWithDebInfo\signalbox\*"; DestDir: "{app}\data\obs-plugins\signalbox"; Flags: ignoreversion recursesubdirs createallsubdirs

[Run]
; Order matters: the [Code] section enforces that the first two are mutually
; exclusive by their index in the finish-page list.
;
; Credential flags matter here and are easy to get backwards: with the
; postinstall flag, Inno defaults to runasoriginaluser - the non-elevated
; user - so an entry with no flag does NOT inherit the installer's elevation.
; CreateProcess cannot elevate, so that combination fails with error 740
; (ERROR_ELEVATION_REQUIRED). runascurrentuser runs in the installer's own
; elevated context, which is what the administrator option needs.
Filename: "{app}\bin\64bit\obs64.exe"; Description: "{cm:RunObs}"; \
    Flags: nowait postinstall skipifsilent runasoriginaluser
Filename: "{app}\bin\64bit\obs64.exe"; Description: "{cm:RunObsAdmin}"; \
    Flags: nowait postinstall skipifsilent unchecked runascurrentuser
; Browsers open as the signed-in user, never elevated.
Filename: "{#ReleaseNotesURL}"; Description: "{cm:ViewNotes}"; \
    Flags: shellexec nowait postinstall skipifsilent unchecked runasoriginaluser
Filename: "{#HomeURL}"; Description: "{cm:VisitHome}"; \
    Flags: shellexec nowait postinstall skipifsilent unchecked runasoriginaluser

[UninstallDelete]
Type: files;          Name: "{app}\obs-plugins\64bit\signalbox.dll"
Type: filesandordirs; Name: "{app}\data\obs-plugins\signalbox"

[Code]
const
  RunIdxObs      = 0;
  RunIdxObsAdmin = 1;

var
  DetectedObsPath: String;

{ Installers built before the AppId GUID was pinned had no AppId at all, and
  Inno falls back to AppName in that case. Those builds therefore registered
  under a DIFFERENT identity than every build since, which is why two
  SignalBox entries could appear in Apps & Features at once: Windows
  considered them unrelated products. Two registrations also means two
  independent sets of files, either of which can drop a signalbox.dll into
  OBS - and OBS loads whatever it finds there, so the older one can win.

  Pinning the GUID stops new duplicates being created. It cannot clean up a
  machine that already has the legacy entry, because that entry's own
  uninstaller is the only thing that knows how to remove it. So: find it, run
  it silently, and only then install.

  This is a one-way fix for a historical mistake, not general "uninstall the
  previous version" logic - the pinned AppId already makes an ordinary
  upgrade replace itself in place. }
{ ONE root path, and the registry VIEW chosen by the root-key constant.

  Spelling the view into the path ('SOFTWARE\WOW6432Node\...') looks like it
  covers both, and does not. Inno's setup.exe is 32-bit, and inside a 32-bit
  process Windows redirects 'SOFTWARE\Microsoft\...\Uninstall' to the
  WOW6432Node copy - so both spellings resolved to the SAME physical key and
  the 64-bit view was never read at all. That is not academic: the entry this
  installer writes on this machine lives in WOW6432Node, which is the proof
  the redirection is active.

  HKLM32/HKLM64 (and the HKCU pair) name the view explicitly and are the only
  way a 32-bit installer can see across it. The 64-bit views are guarded by
  IsWin64 because those constants are meaningless on a 32-bit Windows. }
const
  LegacyAppId = 'SignalBox Category Monitor';
  UninstallRoot = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall';

function ReadLegacyUninstaller(RootKey: Integer; var Command: String): Boolean;
begin
  Result := RegQueryStringValue(RootKey, UninstallRoot + '\' + LegacyAppId + '_is1', 'UninstallString', Command)
            and (Command <> '');
end;

{ EVERY legacy registration, not the first one found.

  This used to chain the four locations with `or` and hand back a single
  uninstall command, which is wrong for the exact machine this code exists
  to repair: a duplicate is by definition MORE THAN ONE registration, and
  the likeliest shape of it is one per-machine entry and one per-user entry
  sitting side by side. Removing whichever happened to be checked first left
  the other in Apps & Features, still owning its own copy of signalbox.dll -
  and OBS loads whatever DLL it finds, so the survivor could still win.

  Each location is therefore visited and uninstalled independently. }
function CountLegacyInstalls(): Integer;
var
  Command: String;
begin
  Result := 0;
  if ReadLegacyUninstaller(HKLM32, Command) then Result := Result + 1;
  if ReadLegacyUninstaller(HKCU32, Command) then Result := Result + 1;
  if IsWin64 then
  begin
    if ReadLegacyUninstaller(HKLM64, Command) then Result := Result + 1;
    if ReadLegacyUninstaller(HKCU64, Command) then Result := Result + 1;
  end;
end;

procedure RunLegacyUninstallerAt(RootKey: Integer);
var
  Command: String;
  ResultCode: Integer;
begin
  if not ReadLegacyUninstaller(RootKey, Command) then
    Exit;

  Command := RemoveQuotes(Command);

  { Silent because the user asked to install, not to be walked through an
    uninstall they did not know they needed. The WAIT is the part that
    matters: returning before the old uninstaller finishes would let it
    delete the very files this setup is about to write.

    /SUPPRESSMSGBOXES also settles the old uninstaller's "Keep your SignalBox
    settings and Twitch connection?" question by taking its default, which is
    Yes - an automatic cleanup must never be what signs someone out. }
  Exec(Command, '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

function RemoveLegacyInstall(): String;
var
  Pass: Integer;
begin
  Result := '';

  { Bounded retry: an uninstaller can hand back control a moment before its
    own registry key disappears, and one legacy entry's uninstaller can take
    another's files with it. Three passes is plenty for four possible
    registrations and cannot spin. }
  for Pass := 1 to 3 do
  begin
    if CountLegacyInstalls() = 0 then
      Exit;

    RunLegacyUninstallerAt(HKLM32);
    RunLegacyUninstallerAt(HKCU32);
    if IsWin64 then
    begin
      RunLegacyUninstallerAt(HKLM64);
      RunLegacyUninstallerAt(HKCU64);
    end;
  end;

  { Judged on the OUTCOME, not on whether Exec managed to start something.
    The old code reported success whenever the process launched, which is not
    the same claim at all - a uninstaller that runs and fails would have been
    reported as a clean removal, and the duplicate would survive into the
    install that follows. }
  if CountLegacyInstalls() > 0 then
    Result := CustomMessage('LegacyRemovalFailed');
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  { Runs after the wizard and before the first file is copied - the only
    point at which removing the old install is safe. }
  NeedsRestart := False;
  Result := RemoveLegacyInstall();
end;

function FindObs(): String;
var
  Path: String;
begin
  Result := '';
  if RegQueryStringValue(HKEY_LOCAL_MACHINE, 'SOFTWARE\OBS Studio', '', Path) then
    Result := Path
  else if RegQueryStringValue(HKEY_LOCAL_MACHINE, 'SOFTWARE\WOW6432Node\OBS Studio', '', Path) then
    Result := Path;
end;

function InitializeSetup(): Boolean;
begin
  { A locked DLL fails midway through copying, which is far more confusing
    than being told up front. }
  if CheckForMutexes('OBSStudioCore') then
  begin
    MsgBox(CustomMessage('ObsRunning'), mbError, MB_OK);
    Result := False;
    Exit;
  end;

  DetectedObsPath := FindObs();
  if DetectedObsPath = '' then
    MsgBox(CustomMessage('ObsNotFound'), mbInformation, MB_OK);

  Result := True;
end;

procedure InitializeWizard();
begin
  if DetectedObsPath <> '' then
    WizardForm.DirEdit.Text := DetectedObsPath;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  { Installing to the wrong folder produces a plugin that silently never
    loads, so confirm the destination actually looks like OBS. }
  if CurPageID = wpSelectDir then
    if not DirExists(AddBackslash(WizardForm.DirEdit.Text) + 'obs-plugins') then
      Result := (MsgBox(CustomMessage('NotAnObsFolder'), mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES);
end;

procedure RunListClickCheck(Sender: TObject);
var
  Clicked: Integer;
begin
  { Starting OBS twice - once elevated, once not - is never what anyone
    wants, so these two behave like radio buttons: ticking one clears the
    other. Both may be left unticked. The two link options below them are
    independent and can be ticked together. }
  Clicked := WizardForm.RunList.ItemIndex;
  if (Clicked = RunIdxObs) and WizardForm.RunList.Checked[RunIdxObs] then
    WizardForm.RunList.Checked[RunIdxObsAdmin] := False
  else if (Clicked = RunIdxObsAdmin) and WizardForm.RunList.Checked[RunIdxObsAdmin] then
    WizardForm.RunList.Checked[RunIdxObs] := False;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpFinished then
    WizardForm.RunList.OnClickCheck := @RunListClickCheck;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ConfigDir: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    { Settings live outside the install directory and survive by default -
      reinstalling should not cost anyone their Twitch connection. }
    ConfigDir := ExpandConstant('{userappdata}\obs-studio\plugin_config\signalbox');
    if DirExists(ConfigDir) then
      if MsgBox(CustomMessage('KeepSettings'), mbConfirmation, MB_YESNO or MB_DEFBUTTON1) = IDNO then
        DelTree(ConfigDir, True, True, True);
  end;
end;
