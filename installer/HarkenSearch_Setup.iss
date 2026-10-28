
; Aris 专业音效管理器 - Inno Setup 安装脚本

; 编译说明：
; 1. 下载并安装 Inno Setup: https://jrsoftware.org/isdl.php
; 2. 用 Inno Setup Compiler 打开此文件
; 3. 修改下面的版本号和源目录路径
; 4. 点击 "Build" -> "Compile" 生成安装包

;
; 版本说明：
; - 当前脚本默认打包 "build_dbg\Release" 目录（开发版，最新代码）
; - 如需打包其他版本，请修改下面的 #define SourceDir
; - 版本号请与代码中的 SOUNDMANAGER_VERSION_STRING 保持一致

#define MyAppName "Harken Search"
#ifndef MyAppVersion
#define MyAppVersion "1.0.41"
#endif
#define MyAppPublisher "Harken Search"
#define MyAppURL "https://harkensearch.xd.com/"
#define MyAppExeName "HarkenSearch.exe"
#define MyAppId "{{31B6E215-3D2B-43EA-88C8-72D73ABC9A0C}}"

; 打包源目录配置

; 根据你要打包的版本，修改下面的路径：
;
; 默认打包源：开发版（最新版本，带诊断功能）：
;   "..\build_dbg\Release" - 开发版（推荐，最新代码，带诊断）
;
; 用户版（正式发布用，已关闭诊断、安全加固）：
;   "..\release"             - 发布目录（包含完整的 exe + 所有 DLL）
;   "..\build_user\Release" - 用户版构建目录（如果 DLL 已复制到此目录）
;
; Debug版（调试用，不推荐打包）：
;   "..\build_dbg\Debug"   - Debug版（调试用）
;
; 注意：确保源目录中包含以下文件：
;   - HarkenSearch.exe
;   - WebView2Loader.dll
;   - rlottie.dll, rlottie-image-loader.dll（可选，如果不需要动画功能）

; Inno Setup 从脚本文件所在目录解析相对路径。
; 如果从 installer 目录编译，使用 "..\build_dbg\Release"
; 如果从项目根目录编译，使用 "build_dbg\Release"
; 当前配置：从 installer 目录编译（推荐）
; Release 构建目录（packer.py 会往这里写入 harken_features.json + .sig）
#ifndef SourceDir
#define SourceDir "..\build_final\Release"
#endif

#ifndef OutputDir
#define OutputDir "..\installer\output"
#endif

[Setup]
; 基本信息
AppId={#MyAppId}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
UsePreviousAppDir=no
AllowNoIcons=yes
LanguageDetectionMethod=uilanguage
ShowLanguageDialog=no
UsePreviousLanguage=no
OutputDir={#OutputDir}
OutputBaseFilename=HarkenSearch_Setup_v{#MyAppVersion}
SetupIconFile=..\HarkenSearch.ico
Compression=lzma2/ultra
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesInstallIn64BitMode=x64compatible
UsedUserAreasWarning=no
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}

; 安装包视觉设计
; 注意：Inno Setup 6.7.1 不再提供默认的 compiler: 图片文件
; 如果需要自定义图片，取消下面两行并指定路径：
; WizardImageFile=..\installer\wizard-large.bmp
; WizardSmallImageFile=..\installer\wizard-small.bmp
; 如果不设置，Inno Setup 会使用默认的现代样式（推荐）

[Languages]
; 按 Windows 显示语言选择；未支持的语言回退到第一项英文。
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimp"; MessagesFile: "Languages\ChineseSimplified.isl"

[CustomMessages]
english.EulaTitle=Installation and Use License Agreement
chinesesimp.EulaTitle=软件安装及使用许可协议
english.EulaDescription=Read the agreement below, then select the checkbox to agree.
chinesesimp.EulaDescription=请阅读以下协议正文，然后勾选同意。
english.EulaConsent=I agree to the Installation and Use License Agreement
chinesesimp.EulaConsent=我同意软件安装及使用许可协议
english.PrivacyTitle=Privacy Policy
chinesesimp.PrivacyTitle=隐私政策
english.PrivacyDescription=Read the privacy policy below, then select the checkbox to agree.
chinesesimp.PrivacyDescription=请阅读以下隐私政策正文，然后勾选同意。
english.PrivacyConsent=I agree to the Privacy Policy
chinesesimp.PrivacyConsent=我同意隐私政策
english.LegalReadRequired=Please read the full %1 and scroll to the bottom.
chinesesimp.LegalReadRequired=请完整阅读%1并滚动到底部。
english.LegalLoadFailed=The %1 could not be loaded. Setup cannot continue.
chinesesimp.LegalLoadFailed=无法读取%1正文，无法继续安装。
english.LegalConsentRequired=You must agree to the %1 before continuing.
chinesesimp.LegalConsentRequired=必须同意%1后才能继续安装。
english.LegacyRunning=Harken Search or an older version is running. Please close it before installing. Setup will not forcibly close programs or silently uninstall them.
chinesesimp.LegacyRunning=检测到 Harken Search 或旧版程序正在运行。请先退出后再安装。本次不会强行关闭正在运行的程序，也不会静默卸载。
english.LegacyStillRegistered=The old version could not be uninstalled: its uninstall entry still exists. Setup has stopped to avoid an incomplete installation.
chinesesimp.LegacyStillRegistered=卸载旧版本失败：卸载项仍在。已停止安装，以免留下半装状态。
english.LegacyUninstallFailed=The old version could not be uninstalled. Setup has stopped to avoid an incomplete installation. Please uninstall the old version manually and try again.
chinesesimp.LegacyUninstallFailed=卸载旧版本失败。已停止安装，以免留下半装状态。请手动卸载旧版后再试。
english.WebViewMissing=WebView2 Runtime was not detected.%nHarken Search needs WebView2 Runtime to run correctly.%n%nContinue installing? You can download WebView2 Runtime from Microsoft's website afterwards.
chinesesimp.WebViewMissing=检测到系统未安装 WebView2 运行时。%nHarken Search 需要 WebView2 运行时才能正常运行。%n%n是否继续安装？您可以在安装后从微软官网下载 WebView2 运行时。
english.ContextMenuTask=Add context menu: Scan this folder with Harken Search
chinesesimp.ContextMenuTask=添加右键菜单：使用 Harken Search 扫描此目录
english.IntegrationOptions=Integration options
chinesesimp.IntegrationOptions=集成选项
english.ScanDirectory=Scan this folder with Harken Search
chinesesimp.ScanDirectory=使用 Harken Search 扫描此目录
english.DeleteUserData=Delete Harken Search user settings and authorization records?%n%nIf deleted, you will need to request authorization again after reinstalling.
chinesesimp.DeleteUserData=是否删除 Harken Search 的用户配置文件和授权记录？%n%n删除后，重新安装时将需要重新申请授权。

[Code]
type
  TScrollInfo = record
    cbSize: UINT;
    fMask: UINT;
    nMin: Integer;
    nMax: Integer;
    nPage: UINT;
    nPos: Integer;
    nTrackPos: Integer;
  end;

const
  SB_VERT = 1;
  SIF_RANGE = $0001;
  SIF_PAGE = $0002;
  SIF_POS = $0004;
  LegacyUninstallKey = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{A8B3C9D1-E4F5-4A6B-8C9D-0E1F2A3B4C5D}_is1';
  LegacyUninstallKeyWow = 'SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\{A8B3C9D1-E4F5-4A6B-8C9D-0E1F2A3B4C5D}_is1';

var
  TrustedLegacyStagedExe: String;
  EulaPage: TWizardPage;
  PrivacyPage: TWizardPage;
  EulaMemo: TNewMemo;
  PrivacyMemo: TNewMemo;
  EulaCheck: TNewCheckBox;
  PrivacyCheck: TNewCheckBox;
  LegalText: String;
  EulaLoaded: Boolean;
  PrivacyLoaded: Boolean;

function GetScrollInfo(hWnd: HWND; nBar: Integer;
  var ScrollInfo: TScrollInfo): BOOL;
  external 'GetScrollInfo@user32.dll stdcall';

function IsLegalMemoAtBottom(MemoControl: TNewMemo): Boolean;
var
  ScrollInfo: TScrollInfo;
  LastPagePosition: Integer;
begin
  Result := False;
  ScrollInfo.cbSize := SizeOf(ScrollInfo);
  ScrollInfo.fMask := SIF_RANGE or SIF_PAGE or SIF_POS;
  if not GetScrollInfo(MemoControl.Handle, SB_VERT, ScrollInfo) then
    exit;

  LastPagePosition := ScrollInfo.nMax;
  if ScrollInfo.nPage > 0 then
    LastPagePosition := ScrollInfo.nMax - Integer(ScrollInfo.nPage - 1);
  Result := ScrollInfo.nPos >= LastPagePosition;
end;

procedure LegalCheckClick(Sender: TObject);
var
  CheckControl: TNewCheckBox;
  MemoControl: TNewMemo;
  DocumentName: String;
begin
  if Sender = EulaCheck then
  begin
    CheckControl := EulaCheck;
    MemoControl := EulaMemo;
    DocumentName := CustomMessage('EulaTitle');
  end
  else
  begin
    CheckControl := PrivacyCheck;
    MemoControl := PrivacyMemo;
    DocumentName := CustomMessage('PrivacyTitle');
  end;

  if CheckControl.Checked and (not IsLegalMemoAtBottom(MemoControl)) then
  begin
    CheckControl.Checked := False;
    MsgBox(FmtMessage(CustomMessage('LegalReadRequired'), [DocumentName]), mbError, MB_OK);
  end;
end;

function LoadUtf8LegalText(const FileName: String; var Text: String): Boolean;
var
  Lines: TArrayOfString;
  I: Integer;
begin
  Text := '';
  Result := LoadStringsFromFile(FileName, Lines);
  if not Result then
    exit;
  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    if I > 0 then
      Text := Text + #13#10;
    Text := Text + Lines[I];
  end;
end;

procedure AddLegalPageControls(Page: TWizardPage; const CheckCaption: String;
  var MemoControl: TNewMemo; var CheckControl: TNewCheckBox);
begin
  MemoControl := TNewMemo.Create(Page.Surface);
  MemoControl.Parent := Page.Surface;
  MemoControl.Left := ScaleX(0);
  MemoControl.Top := ScaleY(0);
  MemoControl.Width := Page.SurfaceWidth;
  MemoControl.Height := ScaleY(250);
  MemoControl.ScrollBars := ssVertical;
  MemoControl.ReadOnly := True;
  MemoControl.WordWrap := True;
  MemoControl.TabOrder := 0;

  CheckControl := TNewCheckBox.Create(Page.Surface);
  CheckControl.Parent := Page.Surface;
  CheckControl.Left := ScaleX(0);
  CheckControl.Top := MemoControl.Top + MemoControl.Height + ScaleY(12);
  CheckControl.Width := Page.SurfaceWidth;
  CheckControl.Height := ScaleY(24);
  CheckControl.Caption := CheckCaption;
  CheckControl.Checked := False;
  CheckControl.TabOrder := 1;
  CheckControl.OnClick := @LegalCheckClick;
end;

function LegalDocumentFileName(const BaseName: String): String;
begin
  if ActiveLanguage = 'chinesesimp' then
    Result := BaseName + '.md'
  else
    Result := BaseName + '.en.md';
end;

procedure InitializeWizard;
var
  EulaFileName, PrivacyFileName: String;
begin
  EulaFileName := LegalDocumentFileName('EULA');
  PrivacyFileName := LegalDocumentFileName('PRIVACY_POLICY');
  ExtractTemporaryFile(EulaFileName);
  ExtractTemporaryFile(PrivacyFileName);

  EulaPage := CreateCustomPage(wpWelcome,
    CustomMessage('EulaTitle'), CustomMessage('EulaDescription'));
  AddLegalPageControls(EulaPage, CustomMessage('EulaConsent'), EulaMemo, EulaCheck);
  EulaLoaded := LoadUtf8LegalText(AddBackslash(ExpandConstant('{tmp}')) + EulaFileName, LegalText);
  if EulaLoaded then
    EulaMemo.Text := LegalText
  else
    EulaMemo.Text := FmtMessage(CustomMessage('LegalLoadFailed'), [CustomMessage('EulaTitle')]);

  PrivacyPage := CreateCustomPage(EulaPage.ID,
    CustomMessage('PrivacyTitle'), CustomMessage('PrivacyDescription'));
  AddLegalPageControls(PrivacyPage, CustomMessage('PrivacyConsent'), PrivacyMemo, PrivacyCheck);
  PrivacyLoaded := LoadUtf8LegalText(AddBackslash(ExpandConstant('{tmp}')) + PrivacyFileName, LegalText);
  if PrivacyLoaded then
    PrivacyMemo.Text := LegalText
  else
    PrivacyMemo.Text := FmtMessage(CustomMessage('LegalLoadFailed'), [CustomMessage('PrivacyTitle')]);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (EulaPage <> nil) and (CurPageID = EulaPage.ID) then
  begin
    if not EulaLoaded then
    begin
      MsgBox(FmtMessage(CustomMessage('LegalLoadFailed'), [CustomMessage('EulaTitle')]), mbError, MB_OK);
      Result := False;
    end
    else if not IsLegalMemoAtBottom(EulaMemo) then
    begin
      MsgBox(FmtMessage(CustomMessage('LegalReadRequired'), [CustomMessage('EulaTitle')]), mbError, MB_OK);
      Result := False;
    end
    else if not EulaCheck.Checked then
    begin
      MsgBox(FmtMessage(CustomMessage('LegalConsentRequired'), [CustomMessage('EulaTitle')]), mbError, MB_OK);
      Result := False;
    end;
  end;
  if (PrivacyPage <> nil) and (CurPageID = PrivacyPage.ID) then
  begin
    if not PrivacyLoaded then
    begin
      MsgBox(FmtMessage(CustomMessage('LegalLoadFailed'), [CustomMessage('PrivacyTitle')]), mbError, MB_OK);
      Result := False;
    end
    else if not IsLegalMemoAtBottom(PrivacyMemo) then
    begin
      MsgBox(FmtMessage(CustomMessage('LegalReadRequired'), [CustomMessage('PrivacyTitle')]), mbError, MB_OK);
      Result := False;
    end
    else if not PrivacyCheck.Checked then
    begin
      MsgBox(FmtMessage(CustomMessage('LegalConsentRequired'), [CustomMessage('PrivacyTitle')]), mbError, MB_OK);
      Result := False;
    end;
  end;
end;

function CreateFileW(lpFileName: String; dwDesiredAccess: Cardinal;
  dwShareMode: Cardinal; lpSecurityAttributes: Cardinal; dwCreationDisposition: Cardinal;
  dwFlagsAndAttributes: Cardinal; hTemplateFile: Cardinal): LongInt;
  external 'CreateFileW@kernel32.dll stdcall';
function CloseHandle(hObject: LongInt): BOOL;
  external 'CloseHandle@kernel32.dll stdcall';

function IsFileLocked(const FileName: String): Boolean;
var
  Handle: LongInt;
begin
  Result := False;
  if not FileExists(FileName) then
    Exit;
  { GENERIC_READ, 共享模式 0（全部拒绝）, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL }
  Handle := CreateFileW(FileName, $80000000, 0, 0, 3, $80, 0);
  if Handle = -1 then
    Result := True
  else
    CloseHandle(Handle);
end;

function LegacyProductIsRunning(const InstallLocation: String): Boolean;
begin
  Result :=
    IsFileLocked(AddBackslash(InstallLocation) + 'HarkenSearch.exe') or
    IsFileLocked(AddBackslash(InstallLocation) + 'HarkenSearchBootstrap.exe') or
    IsFileLocked(AddBackslash(InstallLocation) + 'Auro.exe') or
    IsFileLocked(AddBackslash(InstallLocation) + 'Aris.exe') or
    IsFileLocked(AddBackslash(InstallLocation) + 'ArisBootstrap.exe');
end;

function IsUserProfileInstallDir(const Dir: String): Boolean;
var
  Candidate: String;
begin
  Candidate := LowerCase(Dir);
  Result :=
    (Pos(LowerCase(ExpandConstant('{localappdata}')), Candidate) = 1) or
    (Pos(LowerCase(ExpandConstant('{userappdata}')), Candidate) = 1);
end;

function QueryLegacyUninstall(RootKey: Integer; const KeyName: String;
  var UninstallString, InstallLocation: String): Boolean;
begin
  Result := RegQueryStringValue(RootKey, KeyName, 'UninstallString', UninstallString);
  if Result then
  begin
    if not RegQueryStringValue(RootKey, KeyName, 'InstallLocation', InstallLocation) then
      InstallLocation := '';
  end;
end;

function FindLegacyUninstall(var UninstallString, InstallLocation: String): Boolean;
begin
  Result :=
    QueryLegacyUninstall(HKLM, LegacyUninstallKey, UninstallString, InstallLocation) or
    QueryLegacyUninstall(HKLM, LegacyUninstallKeyWow, UninstallString, InstallLocation) or
    QueryLegacyUninstall(HKCU, LegacyUninstallKey, UninstallString, InstallLocation) or
    QueryLegacyUninstall(HKCU, LegacyUninstallKeyWow, UninstallString, InstallLocation);
end;

function LegacyUninstallKeyStillPresent: Boolean;
var
  UnusedUninstall, UnusedLocation: String;
begin
  Result := FindLegacyUninstall(UnusedUninstall, UnusedLocation);
end;

procedure SplitUninstallCommand(const UninstallString: String; var ExePath, Params: String);
var
  Text: String;
  CloseQuote: Integer;
begin
  Text := Trim(UninstallString);
  if (Length(Text) > 0) and (Text[1] = '"') then
  begin
    CloseQuote := Pos('"', Copy(Text, 2, Length(Text)));
    if CloseQuote > 0 then
    begin
      ExePath := Copy(Text, 2, CloseQuote - 1);
      Params := Trim(Copy(Text, CloseQuote + 2, Length(Text)));
      Exit;
    end;
  end;
  ExePath := Text;
  Params := '';
end;

{ Runs a console tool and captures its combined stdout/stderr into Output.
  Inno's Exec has no pipe support, so the output is redirected through a file
  in the temp dir. ResultCode receives the tool's exit code (cmd propagates it). }
function ExecAndLogOutput(const FileName, Params, WorkingDir: String;
  ShowCmd: Integer; Wait: TExecWait; var ResultCode: Integer;
  var Output: AnsiString): Boolean;
var
  OutFile: String;
begin
  Result := False;
  Output := '';
  OutFile := AddBackslash(ExpandConstant('{tmp}')) + 'signtool-output.txt';
  DeleteFile(OutFile);
  if not Exec(ExpandConstant('{cmd}'),
    '/C ""' + FileName + '" ' + Params + ' > "' + OutFile + '" 2>&1"',
    WorkingDir, ShowCmd, Wait, ResultCode) then
    Exit;
  Result := LoadStringFromFile(OutFile, Output);
  DeleteFile(OutFile);
end;

{ Extracts the leaf certificate subject from `signtool verify /pa /v` output.
  Only the first "Issued to:" line after the chain header is trusted: the rest
  of the output (including the echoed file path and timestamp blocks) must not
  influence the identity decision. }
function SigntoolLeafSubject(const ToolPath, FilePath: String;
  var Subject: String): Boolean;
var
  RawOutput: AnsiString;
  Output, After, Line: String;
  ResultCode, ChainPos, SubjectPos, LineEnd: Integer;
begin
  Result := False;
  Subject := '';
  if not ExecAndLogOutput(ToolPath, 'verify /pa /v "' + FilePath + '"', '', SW_HIDE,
    ewWaitUntilTerminated, ResultCode, RawOutput) then
    Exit;
  Output := RawOutput;
  if ResultCode <> 0 then
    Exit;
  ChainPos := Pos('Signing Certificate Chain:', Output);
  if ChainPos = 0 then
    Exit;
  After := Copy(Output, ChainPos, Length(Output) - ChainPos + 1);
  SubjectPos := Pos('Issued to:', After);
  if SubjectPos = 0 then
    Exit;
  Line := Copy(After, SubjectPos, Length(After) - SubjectPos + 1);
  LineEnd := Pos(#13#10, Line);
  if LineEnd > 0 then
    Line := Copy(Line, 1, LineEnd - 1);
  Subject := Trim(Copy(Line, Length('Issued to:') + 1, Length(Line)));
  Result := Subject <> '';
end;

function IsTrustedLegacyUninstaller(const ExePath, InstallLocation: String): Boolean;
var
  Name, Expected, ToolPath, StageDir, StageExe, StageDat, Subject: String;
begin
  Result := False;
  TrustedLegacyStagedExe := '';
  if (InstallLocation = '') or IsUserProfileInstallDir(InstallLocation) then
    Exit;
  Name := LowerCase(ExtractFileName(ExePath));
  if Name <> 'unins000.exe' then
    Exit;
  Expected := LowerCase(AddBackslash(InstallLocation) + 'unins000.exe');
  if LowerCase(ExePath) <> Expected then
    Exit;
#if defined(SignToolPath) && defined(TrustedSignerSubject)
  { Verification requires the bundled signtool.exe; if the build did not package
    it, calling ExtractTemporaryFile would raise and abort the whole upgrade. }
  ExtractTemporaryFile('signtool.exe');
  ToolPath := ExpandConstant('{tmp}\signtool.exe');
  if not FileExists(ToolPath) then
    Exit;
  { Verify and later execute a private copy staged in the temp dir: the
    HKCU-provided path can be swapped between verification and execution otherwise. }
  StageDir := AddBackslash(ExpandConstant('{tmp}')) + 'legacy-uninst\';
  if not ForceDirectories(StageDir) then
    Exit;
  StageExe := StageDir + 'unins000.exe';
  StageDat := StageDir + 'unins000.dat';
  if not FileCopy(ExePath, StageExe, False) then
    Exit;
  { The uninstaller loads its data from unins000.dat next to the exe. }
  if not FileExists(AddBackslash(InstallLocation) + 'unins000.dat') then
    Exit;
  if not FileCopy(AddBackslash(InstallLocation) + 'unins000.dat', StageDat, False) then
    Exit;
  if not SigntoolLeafSubject(ToolPath, StageExe, Subject) then
    Exit;
  if Pos('{#TrustedSignerSubject}', Subject) = 0 then
    Exit;
  TrustedLegacyStagedExe := StageExe;
  Result := True;
#else
  { No verification tool shipped in this build: refuse to run the legacy
    uninstaller at all instead of executing an unverified binary. }
  Result := False;
#endif
end;

{ Returns True only when the legacy uninstaller was staged, verified and ran to
  completion. Attempted tells the caller whether a run was tried, so "refused to
  run" no longer blocks the whole upgrade. }
function ExecLegacyUninstaller(const UninstallString, InstallLocation: String;
  var Attempted: Boolean): Boolean;
var
  ExePath, Params: String;
  ResultCode: Integer;
begin
  Result := False;
  Attempted := False;
  SplitUninstallCommand(UninstallString, ExePath, Params);
  if (ExePath = '') or (not IsTrustedLegacyUninstaller(ExePath, InstallLocation)) or (not FileExists(ExePath)) then
    Exit;
  Attempted := True;
  if Params <> '' then
    Params := Params + ' ';
  Params := Params + '/VERYSILENT /NORESTART /SUPPRESSMSGBOXES';
  { Execute the staged copy, not the HKCU-provided path. }
  Result := Exec(TrustedLegacyStagedExe, Params, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;

procedure DeleteLegacyStaleFiles(const InstallLocation: String);
var
  Names: array[0..3] of String;
  nameIndex: Integer;
  Target: String;
begin
  if (InstallLocation = '') or (not DirExists(InstallLocation)) then
    Exit;
  if IsUserProfileInstallDir(InstallLocation) then
    Exit;
  Names[0] := 'ArisBootstrapCore.lib';
  Names[1] := 'aris_thesaurus.json';
  Names[2] := 'aura_diagnostics_live.jsonl';
  Names[3] := 'user_actions.log';
  for nameIndex := 0 to 3 do
  begin
    Target := AddBackslash(InstallLocation) + Names[nameIndex];
    if FileExists(Target) then
      DeleteFile(Target);
  end;
  RemoveDir(InstallLocation);
end;

function UninstallLegacyArisIfPresent: Boolean;
var
  UninstallString, InstallLocation: String;
  Attempted: Boolean;
begin
  Result := True;
  if not FindLegacyUninstall(UninstallString, InstallLocation) then
    Exit;
  if LegacyProductIsRunning(InstallLocation) then
  begin
    MsgBox(CustomMessage('LegacyRunning'), mbError, MB_OK);
    Result := False;
    Exit;
  end;
  if ExecLegacyUninstaller(UninstallString, InstallLocation, Attempted) then
  begin
    if LegacyUninstallKeyStillPresent then
    begin
      MsgBox(CustomMessage('LegacyStillRegistered'), mbError, MB_OK);
      Result := False;
      Exit;
    end;
  end
  else if Attempted then
  begin
    MsgBox(CustomMessage('LegacyUninstallFailed'), mbError, MB_OK);
    Result := False;
    Exit;
  end;
  { 校验不过=拒绝执行旧卸载程序，但不阻断安装；残留文件照常清理，
    卸载入口由新安装写入，旧卸载项留给用户在设置里手动处理。 }
  DeleteLegacyStaleFiles(InstallLocation);
end;

function InitializeSetup: Boolean;
var
  Version: String;
begin
  Result := True;
  if not RegQueryStringValue(HKEY_LOCAL_MACHINE, 'SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', Version) then
  begin
    if MsgBox(CustomMessage('WebViewMissing'), mbConfirmation, MB_YESNO) = IDNO then
    begin
      Result := False;
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
  begin
    if not UninstallLegacyArisIfPresent then
      Abort;
  end;
end;

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "contextmenu"; Description: "{cm:ContextMenuTask}"; GroupDescription: "{cm:IntegrationOptions}"; Flags: checkedonce

[Files]
Source: "{#SourceDir}\aris_release_notes.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\HarkenSearch.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\HarkenSearch.exe.sig"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\HarkenSearchBootstrap.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\HarkenSearchBootstrap.exe.sig"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\harken-runtime-pair.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\lucide.ttf"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\splash_beta.png"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\assets\*"; DestDir: "{app}\assets"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\HarkenSearchProToolsBridge.exe"; DestDir: "{app}"; Flags: ignoreversion

; 旧卸载器身份校验依赖 signtool：构建时同时提供 /DSignToolPath=<signtool.exe 路径>
; 与 /DTrustedSignerSubject=<预期证书主体子串> 才会打包校验工具并启用校验；
; 缺少任一 define 时安装器拒绝（而非盲跑）旧卸载程序。

; Required release notices and provenance records.
Source: "{#SourceDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\THIRD_PARTY_COMPONENTS.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\third-party-sbom.cdx.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\ASSET_PROVENANCE.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\BINARY_PROVENANCE.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\PRIVACY.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\docs\legal\EULA.md"; DestDir: "{app}\docs\legal"; Flags: ignoreversion
Source: "{#SourceDir}\docs\legal\PRIVACY_POLICY.md"; DestDir: "{app}\docs\legal"; Flags: ignoreversion
Source: "{#SourceDir}\docs\legal\OPEN_SOURCE_RELEASE_NOTICE.md"; DestDir: "{app}\docs\legal"; Flags: ignoreversion
Source: "{#SourceDir}\docs\legal\EULA.en.md"; DestDir: "{app}\docs\legal"; Flags: ignoreversion
Source: "{#SourceDir}\docs\legal\PRIVACY_POLICY.en.md"; DestDir: "{app}\docs\legal"; Flags: ignoreversion
Source: "{#SourceDir}\docs\legal\OPEN_SOURCE_RELEASE_NOTICE.en.md"; DestDir: "{app}\docs\legal"; Flags: ignoreversion
Source: "{#SourceDir}\docs\HarkenSearch-Manual-zh-CN.pdf"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "..\docs\legal\EULA.md"; DestDir: "{tmp}"; Flags: dontcopy
Source: "..\docs\legal\PRIVACY_POLICY.md"; DestDir: "{tmp}"; Flags: dontcopy
Source: "..\docs\legal\EULA.en.md"; DestDir: "{tmp}"; Flags: dontcopy
Source: "..\docs\legal\PRIVACY_POLICY.en.md"; DestDir: "{tmp}"; Flags: dontcopy
#ifdef SignToolPath
Source: "{#SignToolPath}"; DestDir: "{tmp}"; Flags: dontcopy
#endif
Source: "{#SourceDir}\AI_FEATURE_DISCLOSURE.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\LICENSES\*"; DestDir: "{app}\LICENSES"; Flags: ignoreversion recursesubdirs createallsubdirs

; 功能裁剪配置（打包系统生成，HMAC 签名保护）
Source: "{#SourceDir}\harken_features.json"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\harken_features.sig"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

; 第三方 DLL（全部用 skipifsourcedoesntexist — packer.py 已按功能裁剪）
Source: "{#SourceDir}\WebView2Loader.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\rlottie.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\rlottie-image-loader.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\reaper_harken.dll"; DestDir: "{app}"; Flags: ignoreversion

; 注意：
; - dbghelp.dll 是 Windows 系统库，通过静态链接（#pragma comment(lib, "dbghelp.lib")）
;   不需要单独打包，系统已包含
; - 不要打包 settings.ini 或 .db 文件！
;   这些应该由程序在首次运行时在 AppData 目录自动生成

[InstallDelete]
Type: files; Name: "{app}\ArisBootstrapCore.lib"
Type: files; Name: "{app}\aris_thesaurus.json"
Type: files; Name: "{app}\aura_diagnostics_live.jsonl"
Type: files; Name: "{app}\user_actions.log"

[UninstallDelete]
Type: files; Name: "{app}\ArisBootstrapCore.lib"
Type: files; Name: "{app}\aris_thesaurus.json"
Type: files; Name: "{app}\aura_diagnostics_live.jsonl"
Type: files; Name: "{app}\user_actions.log"

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; 右键菜单：在文件夹上右键时显示 "使用 Aris 扫描此目录"
; 注意：此功能需要管理员权限
Root: HKCR; Subkey: "Directory\shell\ArisScan"; ValueType: string; ValueName: ""; ValueData: "{cm:ScanDirectory}"; Flags: uninsdeletekey; Tasks: contextmenu
Root: HKCR; Subkey: "Directory\shell\ArisScan"; ValueType: string; ValueName: "Icon"; ValueData: """{app}\{#MyAppExeName}"",0"; Flags: uninsdeletekey; Tasks: contextmenu
Root: HKCR; Subkey: "Directory\shell\ArisScan\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Flags: uninsdeletekey; Tasks: contextmenu

; 文件关联（可选）：如果需要关联特定音频文件格式
; Root: HKCR; Subkey: ".wav"; ValueType: string; ValueName: ""; ValueData: "ArisAudioFile"; Flags: uninsdeletevalue
; Root: HKCR; Subkey: "ArisAudioFile"; ValueType: string; ValueName: ""; ValueData: "Aris Audio File"; Flags: uninsdeletekey
; Root: HKCR; Subkey: "ArisAudioFile\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"
; Root: HKCR; Subkey: "ArisAudioFile\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""

[Run]
; 安装完成后运行程序（可选）
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Verb: runas; Flags: shellexec nowait postinstall skipifsilent

[Code]

function HasUninstallParam(const ParamName: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
  begin
    if CompareText(ParamStr(I), ParamName) = 0 then
    begin
      Result := True;
      Exit;
    end;
  end;
end;

function ShouldDeleteUserDataOnUninstall(): Boolean;
begin
  if HasUninstallParam('/DELETEUSERDATA') then
  begin
    Result := True;
    Exit;
  end;

  if UninstallSilent then
  begin
    Result := False;
    Exit;
  end;

  Result := MsgBox(CustomMessage('DeleteUserData'), mbConfirmation, MB_YESNO) = IDYES;
end;

// 卸载时清理可选的用户数据；注册表由 [Registry] uninsdeletekey 管理。
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
  begin
    if ShouldDeleteUserDataOnUninstall() then
    begin
      DelTree(ExpandConstant('{userappdata}\ArisAudio'), True, True, True);
    end;
  end;
end;
