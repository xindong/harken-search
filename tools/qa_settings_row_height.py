"""Measure actual row spacing against the row-height control and font minimum."""
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
    def choose_row_height(value):
        query('row height');click('result-rowHeight')
        a,b,c,d=item('rowHeight')['bounds']
        cmd(cmd='settings_ui',action='key',key='Ctrl',down=True);settle()
        pointer((a+c)/2,(b+d)/2)
        cmd(cmd='settings_ui',action='key',key='Ctrl',down=False);settle()
        cmd(cmd='settings_ui',action='key',key='Ctrl',down=True);settle();key('A')
        cmd(cmd='settings_ui',action='key',key='Ctrl',down=False);settle()
        cmd(cmd='info_edit_input',action='text',text=str(value));settle();key('Enter');settle()
    def measured():
        v=settle();ys=v['drawn_list_row_origins']
        assert v['drawn_list_frame']>=v['current_frame']-3 and len(ys)>=3,v
        gaps=[b-a for a,b in zip(ys,ys[1:])]
        assert max(gaps)-min(gaps)<1,gaps
        return v,sum(gaps)/len(gaps)
    def verify_row(value,list_font=18,system=18,draft=None):
        v,gap=measured();expected=max(value,list_font+4,v['mono_font_pixels']+4)
        assert v['live_row_height']==value and v['draft_row_height']==(value if draft is None else draft),v
        assert saved('rowHeight')==value and saved('listFontSize')==list_font and saved('readingFontSize')==system
        assert abs(v['drawn_list_font_pixels']-list_font)<.1 and abs(v['reading_font_pixels']-system)<.1,v
        assert abs(gap-expected)<.6,dict(configured=value,list_font=list_font,expected=expected,actual=gap,origins=v['drawn_list_row_origins'])
        assert all(abs(h-gap)<1 for h in v['drawn_list_hit_heights']),dict(hit=v['drawn_list_hit_heights'],gap=gap)
        return dict(configured=value,font=list_font,actual=gap,hit_heights=v['drawn_list_hit_heights'],origins=v['drawn_list_row_origins'])
    try:
        start();open_settings()
        if state()['live_font']!=18:choose_font(18);apply()
        if state()['live_list_font']!=18:choose_list(18);apply()
        choose_theme(0);apply()
        choose_row_height(32);apply()
        if args.alignment_only:
            choose_list(10);apply();choose_row_height(16);apply();v,gap=measured()
            assert all(abs(h-gap)<1 for h in v['drawn_list_hit_heights']),dict(hit=v['drawn_list_hit_heights'],actual=gap)
            check('small filename font with fixed numeric font: real click area matches actual row spacing',actual=gap,hit_heights=v['drawn_list_hit_heights'])
            choose_list(18);apply();choose_row_height(32);apply();click('done');stop();report['passed']=True
            return
        if args.probe:
            report['probeOnly']=True
            for height in (16,32,100):
                choose_row_height(height);apply();v,gap=measured()
                report['checks'].append(dict(configured=height,actual=gap,origins=v['drawn_list_row_origins'],hit_heights=v['drawn_list_hit_heights']))
                click('done');shot('probe-row'+str(height));open_settings()
                print(report['checks'][-1],flush=True)
            choose_row_height(32);apply();click('done');stop();report['observationsRecorded']=True
            return
        verify_row(32);shot('01-before-32')
        choose_row_height(64);assert state()['draft_row_height']==64 and state()['live_row_height']==32
        assert saved('rowHeight')==32
        shot('02-draft-64');click('revert');verify_row(32)
        check('choosing a row height waits for Apply; Revert preserves actual row spacing')
        choose_row_height(64);click('close');assert state()['prompt'];shot('03-unsaved-prompt');click('discard');settle()
        assert not state()['open'];open_settings();verify_row(32)
        check('closing and discarding preserves actual and saved 32px row height')
        choose_row_height(64);cmd(cmd='settings_ui',action='fail_save_once');click('apply');settle()
        assert state()['pending'] and state()['error'];verify_row(32,draft=64);shot('04-save-failure')
        check('failed configuration save preserves pending draft and original actual row spacing')
        click('apply');verify_row(64);shot('05-applied-64')
        check('retry Apply updates actual row spacing and hit area to 64px without restart')
        click('done');shot('06-main-64');stop();start();open_settings();verify_row(64)
        check('real process restart preserves saved and measured 64px row height')
        for height in (16,17,20,24,28,30,32,48,64,99,100):
            choose_row_height(height);apply();v=verify_row(height)
            click('done');shot('07-main-row'+str(height));open_settings()
            check(f'configured {height}px: actual neighboring rows and click area match font-aware height',**v)
        for size in (10,18,24):
            choose_list(size);apply()
            for height in (16,28,100):
                choose_row_height(height);apply();v=verify_row(height,size)
                click('done');shot(f'08-font{size}-row{height}');open_settings()
                check(f'font {size}px with row {height}px: real spacing respects minimum font height',**v)
        choose_list(18);apply()
        for entered,expected in ((1,16),(1000,100)):
            choose_row_height(entered);apply();v=verify_row(expected)
            check(f'out-of-range input {entered} clamps to {expected}px',**v)
        for height in (16,32,100):
            choose_row_height(height);apply()
            scroll=cmd(cmd='scroll_results',top_row=40);assert scroll['ok'];settle()
            v=verify_row(height);regions=cmd(cmd='get_ui_regions')['list']
            assert abs(regions['scrollTop']-40)<=1,regions
            assert abs(regions['rowH']-v['actual'])<1,regions
            check(f'row {height}px: scrolling to row 40 preserves measured spacing and clipper step',**v)
        cmd(cmd='scroll_results',top_row=0);settle()
        bookmark=cmd(cmd='folder_shortcut',bookmark_path=str(root/'fixture-library'));assert bookmark['ok']
        opened=cmd(cmd='folder_shortcut',open_index=bookmark['shortcut_count']-1);assert opened['ok']
        deadline=time.monotonic()+15
        while cmd(cmd='folder_shortcut')['entries']<100:
            assert time.monotonic()<deadline,'isolated browser entries did not load';settle()
        for size,height in ((18,16),(18,32),(18,100),(24,16),(24,100)):
            choose_list(size);apply();choose_row_height(height);apply();v=verify_row(height,size)
            click('done');shot(f'09-browser-font{size}-row{height}');open_settings()
            check(f'browser font {size}px row {height}px: actual spacing and hit area match database rules',**v)
        assert cmd(cmd='set_view_mode',mode='database')['ok'];settle()
        choose_list(18);apply()
        for height in (16,100):
            size=18 if height==16 else 24
            choose_list(size);apply();choose_font(size);apply()
            choose_row_height(height);apply()
            for theme in range(5):
                choose_theme(theme)
                for width,height_px in ((1120,800),(880,600)):
                    cmd(cmd='settings_ui',action='resize',width=width,height=height_px);settle();v=verify_row(height,size,size)
                    for nav in range(1,9):item('nav-'+str(nav))
                    item('close');query('row height');click('result-rowHeight');item('rowHeight')
                    shot(f'09-setting-row{height}-theme{theme}-{width}x{height_px}')
                    apply();click('done');shot(f'10-main-row{height}-theme{theme}-{width}x{height_px}');open_settings()
                    check(f'row {height}px theme {theme} {width}x{height_px}: actual spacing, slider and navigation fit',**v)
        choose_row_height(32);apply();choose_list(18);apply();choose_font(18);apply();choose_theme(0);apply();click('done');stop();report['passed']=True
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
    p=argparse.ArgumentParser();p.add_argument('--exe',type=Path,required=True);p.add_argument('--isolation-root',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--probe',action='store_true');p.add_argument('--alignment-only',action='store_true')
    run(p.parse_args())
