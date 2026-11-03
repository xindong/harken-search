"""Check every system-font choice against actual font bindings, rendered labels and restart."""
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
    def verify_fonts(system,list_size=18):
        v=settle()
        assert v['live_font']==system and abs(v['reading_font_pixels']-system)<.1,v
        assert v['live_list_font']==list_size and abs(v['list_font_pixels']-list_size)<.1,v
        assert abs(item('search')['font_pixels']-max(18,system))<.1,v
        assert saved('readingFontSize')==system and saved('listFontSize')==list_size
        assert len(v['rendered_texts'])==8
        for text in v['rendered_texts']:
            a,b,c,d=text['bounds'];l,t,r,bot=text['clip']
            assert a>=l-1 and b>=t-1 and c<=r+1 and d<=bot+1,text
    def apply():
        if state()['pending']:click('apply');settle()
    try:
        start();open_settings()
        if state()['live_font']!=18:choose_font(18);apply()
        if state()['live_list_font']!=18:choose_list(18);apply()
        choose_theme(0);apply();click('nav-1')
        verify_fonts(18);shot('01-before-18')
        choose_font(24)
        assert state()['draft_font']==24 and state()['live_font']==18 and state()['reading_font_pixels']==18
        assert saved('readingFontSize')==18
        shot('02-draft-24');click('revert');verify_fonts(18)
        check('choosing 24px waits for Apply; Revert restores draft without changing actual fonts')
        choose_font(24);click('close');assert state()['prompt'];shot('03-unsaved-prompt');click('discard');settle()
        assert not state()['open'] and saved('readingFontSize')==18
        open_settings();verify_fonts(18)
        check('closing and discarding a changed font preserves the actual and saved original size')
        choose_font(24);cmd(cmd='settings_ui',action='fail_save_once');click('apply');settle()
        assert state()['pending'] and state()['error'] and state()['draft_font']==24
        verify_fonts(18);shot('04-save-failure')
        check('a configuration save failure keeps the pending size and original actual fonts')
        click('apply');verify_fonts(24);shot('05-applied-24')
        check('retrying Apply changes actual reading font and rendered settings to 24px without changing list font')
        click('done');shot('06-main-24');stop();start();open_settings();verify_fonts(24);shot('07-restart-24')
        check('a real process restart retains 24px reading font and independent 18px list font')
        for size in fonts:
            choose_font(size);apply();verify_fonts(size);click('nav-1')
            shot('08-font-'+str(size))
            click('done');shot('09-main-'+str(size));open_settings()
            check(f'{size}px: actual bound reading font matches saved choice; list stays 18px and settings obey 18px readability minimum')
        choose_list(22);apply();verify_fonts(24,22)
        choose_font(10);apply();verify_fonts(10,22);shot('10-independent-10-and-22')
        choose_font(24);apply();verify_fonts(24,22)
        check('system font changes preserve a separately chosen 22px list font')
        choose_list(18);apply();verify_fonts(24)
        for size in (18,24):
            choose_font(size);apply()
            for theme in range(5):
                choose_theme(theme)
                for width,height in ((1120,800),(880,600)):
                    cmd(cmd='settings_ui',action='resize',width=width,height=height);settle();verify_fonts(size)
                    bounds=state()['bounds'];assert abs(bounds[2]-bounds[0]-width)<2 and abs(bounds[3]-bounds[1]-height)<2
                    for nav in range(1,9):item('nav-'+str(nav))
                    choose_font(size);click('systemFont');settle()
                    # Observe the actual selected popup item. Offscreen items
                    # are not treated as clickable hit boxes.
                    item('option-'+str(fonts.index(size)))
                    shot(f'11-options-{size}-theme{theme}-{width}x{height}');key('Escape');settle()
                    check(f'{size}px theme {theme} {width}x{height}: navigation, font selector and close control fit actual window')
                cmd(cmd='settings_ui',action='resize',width=1120,height=800);settle()
        choose_font(18);apply();choose_theme(0);apply();click('done');stop();report['passed']=True
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
    p=argparse.ArgumentParser();p.add_argument('--exe',type=Path,required=True);p.add_argument('--isolation-root',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    run(p.parse_args())
