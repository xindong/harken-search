"""Real signed-fixture update on a private Windows desktop, with UI evidence.

The old process, file writes, new child and health acknowledgement are real.
Only the signing key and payload application are test fixtures. No installed
Harken directory, registry entry or user profile is read or changed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import time
import subprocess
from PIL import Image

import pywintypes
import ntsecuritycon
import win32api
import win32con
import win32event
import win32file
import win32gui
import win32process
import win32security
import win32service
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey


def sandbox_permissions(path):
    token = win32security.OpenProcessToken(win32api.GetCurrentProcess(), win32con.TOKEN_QUERY)
    sid = win32security.GetTokenInformation(token, win32security.TokenUser)[0]
    acl = win32security.ACL()
    acl.AddAccessAllowedAceEx(win32security.ACL_REVISION,
        win32con.OBJECT_INHERIT_ACE | win32con.CONTAINER_INHERIT_ACE, ntsecuritycon.FILE_ALL_ACCESS, sid)
    win32security.SetNamedSecurityInfo(str(path), win32security.SE_FILE_OBJECT,
        win32security.DACL_SECURITY_INFORMATION | win32security.PROTECTED_DACL_SECURITY_INFORMATION,
        None, None, acl, None)
    token.Close()


def run_case(root, binaries, case):
    root.mkdir(parents=True, exist_ok=False)
    sandbox_permissions(root)
    install, updates, data, capture = [root / n for n in ('install', 'updates', 'data', 'capture')]
    suffix = f'{os.getpid()}-{hashlib.sha256(str(root).encode()).hexdigest()[:32]}'
    staging, transaction = updates / f'staging-invocation-{suffix}', updates / f'transaction-invocation-{suffix}'
    runtime_pair = transaction/'runtime-pair'
    for path in (install, staging, transaction, data, capture, runtime_pair):
        path.mkdir(parents=True, exist_ok=True)
    helper = updates/'ArisUpdater.exe'
    installed = install/'Aris.exe'
    bootstrap = runtime_pair/'HarkenSearchBootstrap.exe'
    shutil.copy2(binaries/'aris_bootstrap_handoff_fixture.exe', helper)
    shutil.copy2(helper, installed)
    shutil.copy2(binaries/'bootstrap_progress_transaction_fixture.exe', bootstrap)
    payload = (binaries/'aris_bootstrap_health_fixture.exe').read_bytes()
    (staging/'Aris.exe').write_bytes(payload)
    sentinel = b'private-user-data-must-not-change'
    (data/'library.db').write_bytes(sentinel)
    old_bytes = installed.read_bytes()
    inventory = [{'byte_size':len(payload), 'relative_path':'Aris.exe', 'sha256':hashlib.sha256(payload).hexdigest()}]
    # Real production layout: the installed successor cleans up the running
    # temporary updater only after the progress window and its process exit.
    bootstrap_bytes = bootstrap.read_bytes()
    (staging/'HarkenSearchBootstrap.exe').write_bytes(bootstrap_bytes)
    inventory.append({'byte_size':len(bootstrap_bytes), 'relative_path':'HarkenSearchBootstrap.exe',
        'sha256':hashlib.sha256(bootstrap_bytes).hexdigest()})
    for i in range(62):
        relative = f'assets/update-progress/probe-{i:03d}.bin'
        content = hashlib.sha256(str(i).encode()).digest()*4096
        path = staging/relative
        path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(content)
        (install/relative).parent.mkdir(parents=True,exist_ok=True)
        inventory.append({'byte_size':len(content), 'relative_path':relative, 'sha256':hashlib.sha256(content).hexdigest()})
    package = {'asset_sha256':hashlib.sha256(payload).hexdigest(), 'asset_size':len(payload),
        'build_id':'build-health-bad' if case=='health-failed' else 'progress-real-update',
        'channel':'testing', 'entrypoint':'Aris.exe', 'inventory':inventory,
        'key_id':'fixture-2026', 'product':'aris', 'removals':[], 'schema_version':1, 'version':'1.2.3'}
    raw = json.dumps(package,separators=(',',':')).encode()
    (root/'package.json').write_bytes(raw)
    # RFC 8032 published test key; never used by production composition.
    key = Ed25519PrivateKey.from_private_bytes(bytes.fromhex('9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60'))
    (root/'package.sig').write_text(key.sign(raw).hex(),encoding='ascii')
    mode = 'progress-timeout' if case=='exit-timeout' else 'progress-normal'
    arguments = [str(helper),'--handoff',str(helper),str(bootstrap),str(root/'package.json'),str(root/'package.sig'),
        str(install),str(updates),str(staging),str(transaction),str(data),str(root/'bootstrap.exit'),mode,str(installed)]
    desktop_name = f'HarkenActualUpdate-{os.getpid()}-{case}'
    desktop = win32service.CreateDesktop(desktop_name,0,win32con.GENERIC_ALL,None)
    attributes=pywintypes.SECURITY_ATTRIBUTES();attributes.bInheritHandle=True
    log=win32file.CreateFile(str(root/'transaction.log'),win32con.GENERIC_WRITE,win32con.FILE_SHARE_READ,
        attributes,win32con.CREATE_NEW,win32con.FILE_ATTRIBUTE_NORMAL,None)
    null=win32file.CreateFile('NUL',win32con.GENERIC_READ,win32con.FILE_SHARE_READ|win32con.FILE_SHARE_WRITE,
        attributes,win32con.OPEN_EXISTING,win32con.FILE_ATTRIBUTE_NORMAL,None)
    startup=win32process.STARTUPINFO();startup.lpDesktop=desktop_name
    startup.dwFlags=win32con.STARTF_USESTDHANDLES
    startup.hStdInput=null;startup.hStdOutput=log;startup.hStdError=log
    environment=os.environ.copy();environment['HARKEN_UPDATE_TEST_CAPTURE']=str(capture)
    process,thread,pid,tid=win32process.CreateProcess(str(helper),subprocess.list2cmdline(arguments),
        None,None,True,win32con.CREATE_NO_WINDOW,environment,str(root),startup)
    began=time.monotonic();failed_at=None
    try:
        while win32event.WaitForSingleObject(process,50)==win32con.WAIT_TIMEOUT:
            if time.monotonic()-began>40:
                raise TimeoutError('private update exceeded 40 seconds; exact helper PID '+str(pid))
            try: windows=desktop.EnumDesktopWindows()
            except pywintypes.error: windows=[]
            for handle in windows:
                window=int(handle)
                if win32gui.GetClassName(window)!='HarkenSearchUpdateProgress':continue
                try: title=win32gui.GetWindowText(win32gui.GetDlgItem(window,101))
                except pywintypes.error: continue # Native controls are still being created/destroyed.
                if title=='更新未完成':
                    failed_at=failed_at or time.monotonic()
                    if time.monotonic()-failed_at>1.0: win32gui.PostMessage(window,win32con.WM_CLOSE,0,0)
        helper_exit=win32process.GetExitCodeProcess(process)
    finally:
        thread.Close();process.Close();log.Close();null.Close();desktop.CloseDesktop()
    expected = {'success':0,'exit-timeout':21,'health-failed':50}[case]
    actual = int((root/'bootstrap.exit').read_text()) if (root/'bootstrap.exit').exists() else None
    if case=='success' and actual==0:
        deadline=time.monotonic()+15
        while (runtime_pair.exists() or any(p.is_file() for p in transaction.rglob('*'))) and time.monotonic()<deadline:time.sleep(.05)
    valid_images=[]
    for path in capture.glob('*.png'):
        try:
            with Image.open(path) as picture:picture.verify()
            valid_images.append(path.name)
        except (OSError,SyntaxError):pass
    frames=sorted(capture.glob('[0-9]*.json'),key=lambda p:int(p.name.split('.')[0]))
    snapshots=[json.loads(p.read_text()) for p in frames if '.dom.' not in p.name]
    phases=list(dict.fromkeys(s['phase'] for s in snapshots))
    result={'case':case,'helperExit':helper_exit,'bootstrapExit':actual,'expectedExit':expected,
        'phases':phases,'screenshotCount':len(valid_images),
        'validScreenshots':valid_images,
        'runtimePairLayout':True,'temporaryRuntimeRemoved':not runtime_pair.exists(),
        'transactionRemoved':not transaction.exists(),
        'transactionJournalRetired':not any(p.is_file() for p in transaction.rglob('*')),
        'seconds':round(time.monotonic()-began,2),'plannedFiles':len(inventory),
        'realReplacementFiles':0 if case=='exit-timeout' else len(inventory),
        'userDataPreserved':(data/'library.db').read_bytes()==sentinel,
        'installedPayloadCorrect':installed.read_bytes()==(old_bytes if case=='exit-timeout' else payload),
        'source':'real signed fixture transaction and WebView2 capture, not a public release upgrade'}
    if case!='exit-timeout':
        result['allFilesMatch']=all(hashlib.sha256((install/i['relative_path']).read_bytes()).hexdigest()==i['sha256'] for i in inventory)
    else:result['allFilesMatch']=not list((install/'assets').rglob('*.bin'))
    result['passed']=helper_exit==0 and actual==expected and result['userDataPreserved'] and result['installedPayloadCorrect'] and result['allFilesMatch'] and result['screenshotCount']>0
    if case=='success': result['passed'] = result['passed'] and {'ReplacingFiles','CheckingStartup','Completed'}.issubset(phases) and result['temporaryRuntimeRemoved'] and result['transactionJournalRetired']
    else: result['passed'] = result['passed'] and 'Failed' in phases
    (root/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(result,ensure_ascii=False),flush=True)
    return result['passed']


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--binaries',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--case',choices=['success','exit-timeout','health-failed'],default='success')
    args=parser.parse_args()
    raise SystemExit(0 if run_case(args.output.resolve(),args.binaries.resolve(),args.case) else 1)
