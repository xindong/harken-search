"""Verify real filename-extension rendering through the actual settings checkbox."""
from pathlib import Path
import argparse,hashlib,json,time,traceback
from PIL import Image
from qa_required_library_setup import PrivateDesktop,capture,wait_state
from qa_fresh_result_driver import FreshResultDriver as Driver

def run(args):
    import win32con,win32event,win32process,win32gui
    exe=args.exe.resolve();root=args.isolation_root.resolve();out=args.output.resolve()
    out.mkdir(parents=True,exist_ok=False)
    (root/'profile/runtime').mkdir(parents=True,exist_ok=True)
    driver=Driver(root/'profile/runtime',out);desktop=PrivateDesktop()
    profile=root/'profile/Local/HarkenSearch/settings.ini'
    report=dict(exe=str(exe),sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),checks=[])
    process=None;identity=None;verified=False
    def cmd(**p):return driver.command(p)
    def state():return cmd(cmd='settings_ui')
    def settle():
        first=state()['frame'];deadline=time.monotonic()+8
        while time.monotonic()<deadline:
            v=state()
            if v['frame']>first+2:return v
            time.sleep(.04)
        raise AssertionError('No fresh settings frame')
    def item(id):
        v=next(i for i in settle()['items'] if i['id']==id)
        a,b,c,d=v['bounds'];l,t,r,bot=v['clip']
        assert a>=l-1 and b>=t-1 and c<=r+1 and d<=bot+1,v
        return v
    def pointer(x,y):
        ox,oy=win32gui.ClientToScreen(identity['hwnd'],(0,0))
        for phase in ('hover','down','up'):
            cmd(cmd='theme_schedule',action='pointer',phase=phase,x=x-ox,y=y-oy);settle()
    def click(id):
        a,b,c,d=item(id)['bounds'];pointer((a+c)/2,(b+d)/2)
    def key(name):
        for down in (True,False):cmd(cmd='settings_ui',action='key',key=name,down=down);settle()
    def query(value):
        click('search');cmd(cmd='settings_ui',action='key',key='Ctrl',down=True);settle();key('A')
        cmd(cmd='settings_ui',action='key',key='Ctrl',down=False);settle()
        cmd(cmd='info_edit_input',action='text',text=value);settle()
    def open_settings():
        if not state()['open']:cmd(cmd='ordinary_button_window',module='settings',action='open');settle()
        cmd(cmd='settings_ui',action='resize',width=1120,height=800);settle();click('nav-1')
    def shot(name):
        capture(driver,out/(name+'.png'))
        v=state()
        if v['open']:
            a,b,c,d=v['bounds'];ox,oy=win32gui.ClientToScreen(identity['hwnd'],(0,0))
            with Image.open(out/(name+'.png')) as im:im.crop(tuple(round(x) for x in (a-ox,b-oy,c-ox,d-oy))).save(out/(name+'-settings.png'))
    def check(label,**details):report['checks'].append(dict(case=label,**details));print(label,flush=True)
    def start():
        nonlocal process,identity,verified
        verified=False;process,pid=desktop.start(exe)
        identity=cmd(cmd='magic_eq_identity')
        assert identity['pid']==pid and Path(identity['exe_path']).resolve()==exe and Path(identity['isolation_root']).resolve()==root
        verified=True
        ready=wait_state(driver,lambda v:v['main_ui_ready'] and v['total_count']>0,'real copied library',90)
        report.setdefault('launches',[]).append(dict(pid=pid,count=ready['total_count'],database=ready['db_path']))
    def stop():
        nonlocal process
        if process is None:return
        if verified:
            cmd(cmd='info_edit_input',action='tray_exit')
            assert win32event.WaitForSingleObject(process,15000)==win32con.WAIT_OBJECT_0
            assert win32process.GetExitCodeProcess(process)==0
        elif win32event.WaitForSingleObject(process,0)==win32con.WAIT_TIMEOUT:win32process.TerminateProcess(process,1)
        process.Close();process=None
    fonts=(10,11,12,13,14,16,18,20,22,24)
    def choose_font(size):
        query('system font');click('result-systemFont');click('systemFont');click('option-'+str(fonts.index(size)));settle()
    def choose_list(size):
        query('list font');click('result-listFont');click('listFont');click('option-'+str(fonts.index(size)));settle()
    def choose_theme(index):
        query('theme');click('result-theme');click('stock-'+str(index));settle()
    def saved(key):
        return int(next(s.split('=',1)[1] for s in profile.read_text('utf-8-sig').splitlines() if s.startswith(key+'=')))
    def apply():
        if state()['pending']:click('apply');settle()
    def extension(enabled):
        query('extension');click('result-extension')
        if state()['draft_extension']!=enabled:click('extension')
        settle()
    def verify(enabled,draft=None):
        v=settle();assert v['live_extension']==enabled and v['draft_extension']==(enabled if draft is None else draft),v
        assert saved('showFileExtension')==int(enabled)
        assert v['drawn_list_frame']>=v['current_frame']-3 and len(v['drawn_list_names'])>=3,v
        assert len(v['drawn_list_raw_names'])==len(v['drawn_list_names'])==len(v['drawn_list_directories']),v
        targets={'000-audit.multi.part.wav','000-audit.UPPER.WAV','000-audit.中文.wav','000-audit.flac','000-audit.ogg','000-audit.mp3'}
        observed_files={raw for raw,directory in zip(v['drawn_list_raw_names'],v['drawn_list_directories']) if not directory}
        assert targets.issubset(observed_files),dict(missing=sorted(targets-observed_files),observed=observed_files)
        for raw,rendered,directory in zip(v['drawn_list_raw_names'],v['drawn_list_names'],v['drawn_list_directories']):
            stem=raw.rsplit('.',1)[0] if '.' in raw and raw.rfind('.')>0 else raw
            expected='[DIR] '+raw if directory else raw if enabled else stem
            assert rendered==expected,dict(raw=raw,drawn=rendered,expected=expected,directory=directory)
        return dict(raw=v['drawn_list_raw_names'],drawn=v['drawn_list_names'],directories=v['drawn_list_directories'])
    def search(term):
        assert cmd(cmd='search',query=term)['ok']
        deadline=time.monotonic()+20
        while not cmd(cmd='wait_ready')['ready']:
            assert time.monotonic()<deadline,'search did not settle';time.sleep(.05)
        settle();return cmd(cmd='get_results',limit=100)
    def manifest():
        lib=root/'fixture-library'
        return {p.relative_to(lib).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in lib.rglob('*') if p.is_file()}
    try:
        before=manifest();assert before,'Prepare the isolated real-audio fixture before running'
        baseline=root.parent/'disk-before.json'
        if baseline.exists():assert json.loads(baseline.read_text('utf-8'))==before,'Fixture differs from recorded disk baseline'
        else:baseline.write_text(json.dumps(before,ensure_ascii=False,indent=2),encoding='utf-8')
        report['diskBaseline']=str(baseline)
        start()
        if args.prepare_fixture:
            result=cmd(cmd='scan_folder',path=str(root/'fixture-library'),recursive=False,update_existing=True,remove_missing=False,cache_waveforms=False,build_similarity_index=False,worker_threads=2)
            assert result['ok'],result
            deadline=time.monotonic()+120
            while True:
                status=cmd(cmd='scan_status')
                if not status['scanning']:
                    acknowledged=cmd(cmd='scan_acknowledge_complete')
                    if acknowledged['ok']:break
                assert time.monotonic()<deadline,status
                time.sleep(.15)
            assert status['errors']==0,status
            check('isolated fixture indexed through actual scan update',status=status)
            stop();report['passed']=True;return
        open_settings()
        if state()['live_font']!=18:choose_font(18);apply()
        if state()['live_list_font']!=18:choose_list(18);apply()
        choose_theme(0);apply();extension(True);apply();click('done')
        records=search('000-audit');assert records['total']==6,records
        assert {r['name'] for r in records['items']}==set(['000-audit.multi.part.wav','000-audit.UPPER.WAV','000-audit.中文.wav','000-audit.flac','000-audit.ogg','000-audit.mp3']),records
        open_settings();verify(True);shot('01-show-extension')
        extension(False);verify(True,False);shot('02-pending-hide');click('revert');verify(True)
        check('real checkbox changes the draft; Revert retains original drawn names and saved setting')
        extension(False);click('close');assert state()['prompt'];shot('03-discard-prompt');click('discard');settle()
        assert not state()['open'];open_settings();verify(True)
        check('closing and discarding keeps extensions shown on actual list')
        extension(False);cmd(cmd='settings_ui',action='fail_save_once');click('apply');settle()
        assert state()['pending'] and state()['error'];verify(True,False);shot('04-save-failure')
        check('failed save retains original drawn names and unsaved draft')
        click('apply');value=verify(False);shot('05-hide-extension-settings');click('done');shot('06-hide-extension-main')
        check('retry Apply hides only the final extension for Chinese, multi-dot, uppercase and four audio formats',**value)
        assert cmd(cmd='get_results',limit=100)['items']==records['items']
        check('display toggle retains underlying names, file paths and current database result order')
        stop();start();open_settings();value=verify(False)
        check('normal process restart retains saved preference and actual suffix-free names',**value)
        click('done')
        for term,total in [('multi.part.wav',1),('UPPER.WAV',1),('中文',1),('000-audit.mp3',1),('000-audit.ogg',1),('000-audit.flac',1)]:
            result=search(term);assert result['total']==total,result
            check('extension hidden: actual database search still finds '+term,names=[r['name'] for r in result['items']])
        records2=search('000-audit');assert records2['total']==6
        open_settings();extension(True);apply();value=verify(True);click('done');shot('07-show-extension-main')
        assert cmd(cmd='get_results',limit=100)['items']==records2['items']
        check('re-enabling restores exact real filenames without changing result order',**value)
        bookmark=cmd(cmd='folder_shortcut',bookmark_path=str(root/'fixture-library'));assert bookmark['ok']
        assert cmd(cmd='folder_shortcut',open_index=bookmark['shortcut_count']-1)['ok']
        deadline=time.monotonic()+15
        while cmd(cmd='folder_shortcut')['entries']<182:
            assert time.monotonic()<deadline,'browser did not load real fixture';settle()
        open_settings()
        for enabled in (False,True):
            extension(enabled);apply();value=verify(enabled)
            assert any(value['directories']),value
            assert any('.' in raw for raw,d in zip(value['raw'],value['directories']) if d),value
            click('done');shot('08-browser-'+str(enabled));open_settings()
            check('browser suffix '+str(enabled)+': files change display, dotted directory names remain intact',**value)
        assert cmd(cmd='set_view_mode',mode='database')['ok'];settle();click('done');search('000-audit');open_settings()
        for size,width,height in ((18,1120,800),(24,880,600)):
            choose_font(size);apply();choose_list(size);apply()
            for theme in range(5):
                choose_theme(theme);apply()
                cmd(cmd='settings_ui',action='resize',width=width,height=height);settle()
                for enabled in (False,True):
                    extension(enabled);apply();value=verify(enabled);item('extension');item('close')
                    for nav in range(1,9):item('nav-'+str(nav))
                    shot(f'09-settings-font{size}-theme{theme}-extension{enabled}');click('done')
                    shot(f'10-main-font{size}-theme{theme}-extension{enabled}');open_settings()
                    cmd(cmd='settings_ui',action='resize',width=width,height=height);settle()
                    check(f'theme {theme} font {size}px {width}x{height} suffix {enabled}: actual names and visible control fit',**value)
        choose_font(18);apply();choose_list(18);apply();choose_theme(0);apply();extension(True);apply();click('done');stop()
        after=manifest();assert before==after
        report['diskManifest']=after
        check('all 182 real audio paths and SHA256 contents unchanged on disk')
        report['passed']=True
    except Exception:
        report['error']=traceback.format_exc();print(report['error'],flush=True)
        try:
            if verified:shot('failure')
        except Exception:pass
        raise
    finally:
        if process is not None:
            try:stop()
            except Exception:
                if win32event.WaitForSingleObject(process,0)==win32con.WAIT_TIMEOUT:win32process.TerminateProcess(process,1)
                report['forcedTermination']=True
        desktop.close();(out/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--exe',type=Path,required=True);p.add_argument('--isolation-root',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--prepare-fixture',action='store_true')
    run(p.parse_args())
