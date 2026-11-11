"""Verify unsaved appearance previews through actual fonts, rows and global colors."""
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
    def choose_row_height(value):
        query('row height');click('result-rowHeight')
        a,b,c,d=item('rowHeight')['bounds']
        cmd(cmd='settings_ui',action='key',key='Ctrl',down=True);settle();pointer((a+c)/2,(b+d)/2)
        cmd(cmd='settings_ui',action='key',key='Ctrl',down=False);settle()
        cmd(cmd='settings_ui',action='key',key='Ctrl',down=True);settle();key('A')
        cmd(cmd='settings_ui',action='key',key='Ctrl',down=False);settle()
        cmd(cmd='info_edit_input',action='text',text=str(value));settle();key('Enter');settle()
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
    try:
        start();load();open_settings()
        choose_font(18);apply();choose_list(18);apply();choose_theme(0);apply()
        before=state();theme0=before['rendered_colors'];disk=profile.read_bytes()
        shot('00-baseline')
        if args.red:
            for size in fonts:
                choose_font(size);v=settle()
                sample=next((i for i in v['items'] if i['id']=='system-font-preview'),None)
                assert sample is not None and abs(sample['font_pixels']-size)<.1,dict(size=size,sample=sample)
                assert v['reading_font_pixels']==size and v['live_font']==18,v
                search=next((i for i in v['items'] if i['id']=='search'),None)
                assert search is not None and abs(search['font_pixels']-size)<.1,dict(size=size,search=search)
                if size in (10,24):
                    click('nav-1');settle();shot('font-preview-'+str(size))
                check('system font label and main reading surface preview '+str(size)+'px without Apply')
            report['passed']=True;return
        for size in fonts:
            choose_font(size);choose_list(size);v=settle()
            assert v['reading_font_pixels']==size and v['drawn_list_font_pixels']==size and v['drawn_list_frame']>=v['current_frame']-3,v
            assert v['live_font']==18 and v['live_list_font']==18 and profile.read_bytes()==disk,v
            check('both actual fonts preview '+str(size)+'px without applying')
        click('revert');settle()
        row_before=state()['live_row_height'];choose_row_height(48);v=settle();shot('00-row-preview')
        origins=v['drawn_list_row_origins'];assert len(origins)>2
        assert all(abs(b-a-48)<1 for a,b in zip(origins,origins[1:])) and v['live_row_height']==row_before and profile.read_bytes()==disk,v
        check('actual row spacing immediately previews 48px without committing')
        click('revert');settle()
        choose_font(24);v=settle();shot('01-font-immediate')
        assert v['reading_font_pixels']==24 and v['live_font']==18 and profile.read_bytes()==disk,v
        check('system font immediately renders at 24px without applying or saving')
        choose_list(24);v=settle();shot('02-list-immediate')
        assert v['drawn_list_font_pixels']==24 and v['live_list_font']==18 and profile.read_bytes()==disk,v
        check('actual file list immediately renders at 24px without applying or saving')
        choose_theme(1);v=settle();shot('03-theme-immediate')
        assert v['rendered_colors']['bg']!=theme0['bg'] and v['live_theme']==0 and profile.read_bytes()==disk,v
        check('main interface theme changes immediately while committed and saved theme stay unchanged')
        cmd(cmd='settings_ui',action='save_preview_frame');v=settle()
        assert v['preview_save_result'] and saved('readingFontSize')==18 and saved('listFontSize')==18 and saved('theme')==0 and saved('rowHeight')==row_before,v
        assert v['reading_font_pixels']==24 and v['drawn_list_font_pixels']==24,v
        disk=profile.read_bytes()
        check('actual save during a preview frame persists committed appearance only')
        click('revert');v=settle();shot('04-revert')
        assert v['reading_font_pixels']==18 and v['drawn_list_font_pixels']==18 and v['rendered_colors']==theme0,v
        check('Revert restores actual fonts and main interface colors')
        choose_font(24);choose_list(22);choose_theme(2);click('close');click('discard');settle();open_settings();v=settle()
        assert v['reading_font_pixels']==18 and v['drawn_list_font_pixels']==18 and v['rendered_colors']==theme0 and profile.read_bytes()==disk,v
        check('close and discard restore appearance without persisting preview')
        choose_font(24);choose_list(22);choose_theme(3)
        cmd(cmd='settings_ui',action='fail_save_once');click('apply');v=settle();shot('05-save-failure')
        assert v['error'] and v['pending'] and v['reading_font_pixels']==24 and v['drawn_list_font_pixels']==22 and v['live_font']==18 and profile.read_bytes()==disk,v
        check('failed save retains preview and draft for retry without committing')
        click('apply');v=settle();assert v['live_font']==24 and v['live_list_font']==22 and v['live_theme']!=0 and not v['pending'],v
        check('Apply commits the visible preview')
        click('done');stop();start();load();open_settings();v=settle()
        assert v['reading_font_pixels']==24 and v['drawn_list_font_pixels']==22 and not v['pending'],v
        check('normal restart preserves applied theme and both fonts')
        choose_font(18);apply();choose_list(18);apply();choose_theme(0);apply();disk=profile.read_bytes()
        for theme in range(5):
            choose_theme(theme);choose_font(24);choose_list(24)
            cmd(cmd='settings_ui',action='resize',width=880,height=600);v=settle()
            assert v['reading_font_pixels']==24 and v['drawn_list_font_pixels']==24 and profile.read_bytes()==disk,v
            item('close');item('apply')
            for nav in range(1,9):item('nav-'+str(nav))
            shot('06-preview-theme'+str(theme));check('theme '+str(theme)+' 24px small-window unsaved actual preview')
            click('revert');settle()
        choose_font(24);choose_list(24);choose_theme(4);shot('07-unsaved-before-exit');stop()
        start();load();open_settings();v=settle();assert v['reading_font_pixels']==18 and v['drawn_list_font_pixels']==18 and v['live_theme']==0,v
        check('normal exit with unsaved preview does not persist it on next process start')
        click('done');stop();report['passed']=True
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
    p=argparse.ArgumentParser();p.add_argument('--exe',type=Path,required=True);p.add_argument('--isolation-root',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--red',action='store_true')
    run(p.parse_args())
