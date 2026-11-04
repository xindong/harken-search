"""Verify the default display mode through actual waveform and spectrogram drawing."""
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
    def choose_mode(mode):
        query('waveform');click('result-waveMode');click('waveMode');click('option-'+str(mode));settle()
    def verify(mode,draft=None):
        v=settle();assert v['live_wave_mode']==mode and v['draft_wave_mode']==(mode if draft is None else draft),v
        assert saved('waveformDisplayMode')==mode
        assert v['live_spectrum']==(mode==2),v
        if mode<2:
            assert v['drawn_wave_frame']>=v['current_frame']-3,v
            assert v['drawn_wave_style']==(3 if mode==0 else 2),dict(mode=mode,actual_style=v['drawn_wave_style'])
            assert v['drawn_spectrum_frame']<v['current_frame']-3,v
        else:
            deadline=time.monotonic()+45
            while v['drawn_spectrum_frame']<v['current_frame']-3:
                assert time.monotonic()<deadline,'actual spectrogram texture was not drawn'
                time.sleep(.1);v=settle()
        return dict(mode=mode,style=v['drawn_wave_style'],wave_frame=v['drawn_wave_frame'],spectrum_frame=v['drawn_spectrum_frame'],current_frame=v['current_frame'])
    def load():
        assert cmd(cmd='load_path',path=str(next((root/'fixture-library').glob('*.wav'))))['ok']
        deadline=time.monotonic()+45
        while True:
            v=settle()
            if max(v['drawn_wave_frame'],v['drawn_spectrum_frame'])>=v['current_frame']-3:break
            assert time.monotonic()<deadline,'real waveform or spectrogram did not render';time.sleep(.1)
        cmd(cmd='pause');settle()
    original_profile=None
    try:
        if args.compat_only:
            original_profile=profile.read_bytes()
            for legacy_mode,legacy_style in ((None,2),(99,3)):
                text=original_profile.decode('utf-8-sig')
                values=dict(waveformStyle=str(legacy_style),showSpectrogram='0')
                lines=[]
                for line in text.splitlines():
                    name=line.split('=',1)[0]
                    if name=='waveformDisplayMode':
                        if legacy_mode is not None:lines.append(name+'='+str(legacy_mode))
                    else:lines.append(name+'='+values[name] if name in values else line)
                profile.write_text('\n'.join(lines)+'\n',encoding='utf-8')
                start();load();open_settings();v=settle()
                expected_mode=0 if legacy_mode is None else 2
                assert v['live_wave_mode']==expected_mode and v['live_wave_style']==legacy_style,v
                assert v['drawn_wave_style']==legacy_style and not v['live_spectrum'],v
                shot('compat-'+str(legacy_mode))
                check('legacy mode '+str(legacy_mode)+' preserves saved waveform style '+str(legacy_style)+' and normalizes mode range')
                choose_mode(1);apply();choose_mode(0);apply();verify(0);click('done');stop()
            report['passed']=True;return
        start();load();open_settings()
        if state()['live_font']!=18:choose_font(18);apply()
        if state()['live_list_font']!=18:choose_list(18);apply()
        choose_theme(0);apply()
        if args.red:
            choose_mode(2);apply();choose_mode(0);apply();click('done');shot('red-spectrum-to-volume');open_settings();verify(0)
            check('switching spectrum to volume clears actual spectrogram');stop();report['passed']=True;return
        choose_mode(0);apply();verify(0);shot('01-volume-setting');click('done');shot('02-volume-main');open_settings()
        choose_mode(1);verify(0,1);click('revert');verify(0)
        check('draft and Revert retain original volume waveform')
        choose_mode(1);click('close');assert state()['prompt'];click('discard');settle();open_settings();verify(0)
        check('close and discard retain original actual waveform')
        choose_mode(1);cmd(cmd='settings_ui',action='fail_save_once');click('apply');settle()
        assert state()['error'] and state()['pending'];verify(0,1);shot('03-save-failure')
        check('failed save retains real volume renderer and pending loudness choice')
        click('apply');value=verify(1);shot('04-loudness-setting');click('done');shot('05-loudness-main')
        check('retry Apply switches actual waveform to RMS loudness style',**value)
        stop();start();load();open_settings();verify(1)
        check('normal restart preserves loudness choice and actual RMS rendering')
        choose_mode(2);apply();value=verify(2);shot('06-spectrum-setting');click('done');shot('07-spectrum-main')
        check('frequency mode produces actual current-file spectrogram image',**value)
        stop();start();load();open_settings();verify(2)
        check('normal restart preserves spectrogram mode and actual texture rendering')
        choose_mode(0);apply();value=verify(0);click('done');shot('08-back-to-volume');open_settings()
        check('switching spectrum back to volume removes spectrogram and draws solid waveform',**value)
        choose_mode(2);apply();verify(2);choose_mode(1);apply();value=verify(1)
        check('switching spectrum directly to loudness removes spectrogram and draws RMS',**value)
        for size,width,height in ((18,1120,800),(24,880,600)):
            choose_font(size);apply();choose_list(size);apply()
            for theme in range(5):
                choose_theme(theme);apply();cmd(cmd='settings_ui',action='resize',width=width,height=height);settle()
                for mode in range(3):
                    choose_mode(mode);apply();value=verify(mode);item('waveMode');item('close')
                    for nav in range(1,9):item('nav-'+str(nav))
                    shot(f'09-settings-font{size}-theme{theme}-mode{mode}');click('done');shot(f'10-main-font{size}-theme{theme}-mode{mode}');open_settings()
                    cmd(cmd='settings_ui',action='resize',width=width,height=height);settle()
                    check(f'theme {theme} font {size}px {width}x{height}: mode {mode} reaches actual drawing',**value)
        choose_font(18);apply();choose_list(18);apply();choose_theme(0);apply();choose_mode(0);apply();click('done');stop();report['passed']=True
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
        if original_profile is not None:profile.write_bytes(original_profile)
        desktop.close();(out/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--exe',type=Path,required=True);p.add_argument('--isolation-root',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--red',action='store_true');p.add_argument('--compat-only',action='store_true')
    run(p.parse_args())
