"""Verify list-font choices against actual filename drawing, row height and restart."""
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
    def verify_fonts(list_size,system=18,preview_list=None):
        v=settle()
        visible_list=list_size if preview_list is None else preview_list
        assert v['live_list_font']==list_size and abs(v['list_font_pixels']-visible_list)<.1,v
        assert v['live_font']==system and abs(v['reading_font_pixels']-system)<.1,v
        assert abs(item('search')['font_pixels']-system)<.1,v
        assert saved('listFontSize')==list_size and saved('readingFontSize')==system
        assert v['drawn_list_sample'] and v['drawn_list_frame']>=v['current_frame']-3,v
        assert abs(v['drawn_list_font_pixels']-visible_list)<.1,v
        # This is the content height; the table adds its own cell padding.
        assert v['drawn_list_row_height']>=v['drawn_list_line_height']-.1,v
        return v
    def apply():
        if state()['pending']:click('apply');settle()
    def choose_row_height(value):
        query('row height');click('result-rowHeight')
        a,b,c,d=item('rowHeight')['bounds']
        cmd(cmd='settings_ui',action='key',key='Ctrl',down=True);settle()
        pointer((a+c)/2,(b+d)/2)
        cmd(cmd='settings_ui',action='key',key='Ctrl',down=False);settle()
        cmd(cmd='settings_ui',action='key',key='Ctrl',down=True);settle();key('A')
        cmd(cmd='settings_ui',action='key',key='Ctrl',down=False);settle()
        cmd(cmd='info_edit_input',action='text',text=str(value));settle();key('Enter');settle()
    try:
        start();open_settings()
        if state()['live_font']!=18:choose_font(18);apply()
        if state()['live_list_font']!=18:choose_list(18);apply()
        choose_theme(0);apply();click('nav-1')
        if args.row_height_only:
            original_row=state()['live_row_height']
            try:
                choose_row_height(16);apply();assert state()['live_row_height']==16
                if args.inject_row_failure:raise AssertionError('injected failure after saving 16px row')
                for size in (10,18,24):
                    choose_list(size);apply();v=verify_fonts(size)
                    assert v['live_row_height']==16 and v['drawn_list_row_height']>=size-.1,v
                    click('done');shot('row16-list'+str(size));open_settings()
                    check(f'configured 16px row with list {size}px: actual row expands to fit filename text',drawn_font=v['drawn_list_font_pixels'],line_height=v['drawn_list_line_height'],row_height=v['drawn_list_row_height'])
            finally:
                try:
                    open_settings()
                    if state()['pending']:click('revert')
                    choose_row_height(original_row);apply();choose_list(18);apply()
                    assert saved('rowHeight')==original_row
                    report['rowHeightRestored']=True
                except Exception:
                    report['rowHeightRestoreError']=traceback.format_exc()
                    raise
            click('done');stop();report['passed']=True
            return
        verify_fonts(18);shot('01-before-18')
        choose_list(24)
        assert state()['draft_list_font']==24 and state()['live_list_font']==18
        verify_fonts(18,preview_list=24);shot('02-draft-24');click('revert');verify_fonts(18)
        check('choosing 24px previews the actual file list; Revert restores 18px')
        choose_list(24);click('close');assert state()['prompt'];shot('03-unsaved-prompt');click('discard');settle()
        assert not state()['open'] and saved('listFontSize')==18
        open_settings();verify_fonts(18)
        check('closing and discarding a list-font change preserves original actual and saved size')
        choose_list(24);cmd(cmd='settings_ui',action='fail_save_once');click('apply');settle()
        assert state()['pending'] and state()['error'] and state()['draft_list_font']==24
        verify_fonts(18,preview_list=24);shot('04-save-failure')
        check('save failure preserves the pending file-list preview and saved original size')
        click('apply');verify_fonts(24);shot('05-applied-24')
        check('retry Apply changes actual filename draw font to 24px and retains independent 18px system font')
        click('done');shot('06-main-24');stop();start();open_settings();verify_fonts(24);shot('07-restart-24')
        check('real process restart retains list 24px and system 18px')
        for size in fonts:
            choose_list(size);apply();v=verify_fonts(size);click('nav-1')
            shot('08-font-'+str(size));click('done');shot('09-main-'+str(size));open_settings()
            check(f'{size}px: actual filename font, saved choice and row height match; system stays 18px',drawn_font=v['drawn_list_font_pixels'],line_height=v['drawn_list_line_height'],row_height=v['drawn_list_row_height'],sample=v['drawn_list_sample'])
        choose_font(24);apply();verify_fonts(24,24)
        choose_list(10);apply();verify_fonts(10,24);shot('10-independent-system24-list10')
        choose_list(24);apply();verify_fonts(24,24)
        check('changing list font preserves separately selected 24px system font')
        choose_font(10);apply();verify_fonts(24,10);shot('10-independent-system10-list24')
        choose_font(18);apply();verify_fonts(24)
        check('changing system font preserves 24px filename draw font')
        for size in (18,24):
            choose_list(size);apply()
            for theme in range(5):
                choose_theme(theme)
                for width,height in ((1120,800),(880,600)):
                    cmd(cmd='settings_ui',action='resize',width=width,height=height);settle();v=verify_fonts(size)
                    for nav in range(1,9):item('nav-'+str(nav))
                    item('close');choose_list(size);click('listFont');item('option-'+str(fonts.index(size)))
                    shot(f'11-options-{size}-theme{theme}-{width}x{height}')
                    click('search');settle()
                    apply();click('done');shot(f'12-main-{size}-theme{theme}-{width}x{height}');open_settings()
                    check(f'{size}px theme {theme} {width}x{height}: actual list rendering, row height, selector, navigation and close control fit')
                cmd(cmd='settings_ui',action='resize',width=1120,height=800);settle()
        choose_list(18);apply();choose_font(18);apply();choose_theme(0);apply();click('done');stop();report['passed']=True
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
    p=argparse.ArgumentParser();p.add_argument('--exe',type=Path,required=True);p.add_argument('--isolation-root',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--row-height-only',action='store_true');p.add_argument('--inject-row-failure',action='store_true')
    run(p.parse_args())
