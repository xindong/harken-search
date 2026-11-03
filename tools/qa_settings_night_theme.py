"""Verify the nighttime setting through real controls, rendered pixels and restart."""
from pathlib import Path
import argparse,hashlib,json,time,traceback
from PIL import Image
from qa_required_library_setup import PrivateDesktop,capture,wait_state
from qa_transport_themes import Driver

def require_saved_palette(profile,period):
    path=profile/('theme_schedule_'+period+'.json')
    assert path.is_file(),'Prepare an existing saved '+period+' palette before verification'
    data=path.read_bytes();assert data
    value=json.loads(data)
    assert isinstance(value,dict) and isinstance(value.get('colors'),dict) and value['colors'],'Saved palette has no colors'
    return data

def require_loaded_palette(state,period):
    assert not state[period+'_warning'],state[period+'_warning']

def require_window_size(bounds,width,height):
    a,b,c,d=bounds
    assert abs(c-a-width)<=1 and abs(d-b-height)<=1,(bounds,width,height)

def stop_owned_process(process,identity_verified,send_exit):
    import win32con,win32event,win32process
    if identity_verified:
        send_exit()
        assert win32event.WaitForSingleObject(process,15000)==win32con.WAIT_OBJECT_0
        assert win32process.GetExitCodeProcess(process)==0
    else:
        if win32event.WaitForSingleObject(process,0)==win32con.WAIT_TIMEOUT:
            win32process.TerminateProcess(process,1)
            assert win32event.WaitForSingleObject(process,15000)==win32con.WAIT_OBJECT_0
    process.Close()

def run(args):
    import win32con,win32event,win32process,win32gui
    exe=args.exe.resolve();root=args.isolation_root.resolve();out=args.output.resolve()
    out.mkdir(parents=True,exist_ok=False)
    profile=root/'profile/Local/HarkenSearch'
    (root/'profile/runtime').mkdir(parents=True,exist_ok=True)
    driver=Driver(root/'profile/runtime',out);desktop=PrivateDesktop()
    report={'exe':str(exe),'sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'checks':[]}
    process=None;identity=None;identity_verified=False
    def cmd(**p):return driver.command(p)
    def state():return cmd(cmd='settings_ui')
    def settle():
        first=state()['frame'];deadline=time.monotonic()+8
        while time.monotonic()<deadline:
            v=state()
            if v['frame']>first+2:return v
            time.sleep(.04)
        raise AssertionError('No fresh settings frame')
    def schedule(minute=None):
        if minute is not None:cmd(cmd='theme_schedule',action='clock',minute=minute);settle()
        return cmd(cmd='theme_schedule',action='state')
    def find(id):
        for item in settle()['items']:
            if item['id']==id:
                a,b,c,d=item['bounds'];l,t,r,bot=item['clip']
                if a>=l-1 and b>=t-1 and c<=r+1 and d<=bot+1:return item
        raise AssertionError((id,state()))
    def click(id):
        a,b,c,d=find(id)['bounds'];ox,oy=win32gui.ClientToScreen(identity['hwnd'],(0,0))
        for phase in ('hover','down','up'):
            cmd(cmd='theme_schedule',action='pointer',phase=phase,x=(a+c)/2-ox,y=(b+d)/2-oy);settle()
    def open_settings():
        if not state()['open']:cmd(cmd='ordinary_button_window',module='settings',action='open');settle()
        cmd(cmd='settings_ui',action='resize',width=1120,height=800);settle()
        click('nav-1')
    def choose_night(index):click('night-combo');click('theme-option-'+str(index))
    def shot(name):
        capture(driver,out/(name+'.png'))
        v=state()
        if v['open']:
            ox,oy=win32gui.ClientToScreen(identity['hwnd'],(0,0));a,b,c,d=v['bounds']
            with Image.open(out/(name+'.png')) as im:im.crop(tuple(round(x) for x in (a-ox,b-oy,c-ox,d-oy))).save(out/(name+'-settings.png'))
    def check(label,**details):report['checks'].append(dict(case=label,**details));print(label,flush=True)
    def snapshot_files():
        return {p.name:p.read_bytes() for p in profile.glob('theme_schedule_*.json')}
    def check_pixels(name,expected):
        shot(name)
        rgb=tuple(int(expected[-6:][i:i+2],16) for i in (0,2,4))
        with Image.open(out/(name+'.png')) as im:count=sum(n for n,color in im.convert('RGB').getcolors(im.width*im.height) if color==rgb)
        assert count>128,(name,'scheduled background missing from actual rendered frame',expected,count)
        return count
    def start():
        nonlocal process,identity,identity_verified
        identity_verified=False
        process,pid=desktop.start(exe)
        identity=cmd(cmd='magic_eq_identity')
        assert identity['pid']==pid and Path(identity['exe_path']).resolve()==exe and Path(identity['isolation_root']).resolve()==root
        identity_verified=True
        ready=wait_state(driver,lambda v:v['main_ui_ready'] and v['total_count']>0,'real library ready',90)
        report.setdefault('launches',[]).append(dict(pid=pid,count=ready['total_count'],database=ready['db_path']))
    def stop():
        nonlocal process
        if process is None:return
        stop_owned_process(process,identity_verified,lambda:cmd(cmd='info_edit_input',action='tray_exit'))
        process=None
    try:
        start()
        if args.layout_only:
            schedule(1140);open_settings();saved=snapshot_files()
            assert schedule()['mode']==1
            def key(name):
                for down in (True,False):cmd(cmd='settings_ui',action='key',key=name,down=down);settle()
            def query(value):
                click('search');cmd(cmd='settings_ui',action='key',key='Ctrl',down=True);settle()
                key('A');cmd(cmd='settings_ui',action='key',key='Ctrl',down=False);settle()
                cmd(cmd='info_edit_input',action='text',text=value);settle()
            for font,option in ((18,6),(24,9)):
                cmd(cmd='settings_ui',action='resize',width=1120,height=800);settle()
                query('system font');click('result-systemFont');click('systemFont');click('option-'+str(option))
                if state()['pending']:click('apply')
                assert state()['live_font']==font
                click('nav-1')
                for width,height in ((1120,800),(880,600)):
                    cmd(cmd='settings_ui',action='resize',width=width,height=height);settle()
                    require_window_size(state()['bounds'],width,height)
                    find('night-combo');find('day-combo');click('night-combo')
                    for index in range(5):find('theme-option-'+str(index))
                    shot(f'layout-{font}-{width}x{height}-options');key('Escape')
                    shot(f'layout-{font}-{width}x{height}')
                    assert snapshot_files()==saved
                    check(f'night selector and all five options fit at {font}px {width}x{height}')
            cmd(cmd='settings_ui',action='resize',width=1120,height=800);settle()
            query('system font');click('result-systemFont');click('systemFont');click('option-6');click('apply')
            click('done');schedule(-1);stop();report['passed']=True;return
        require_saved_palette(profile,'day')
        manual=schedule();require_loaded_palette(manual,'day');assert manual['mode']==0
        manual_bg=manual['active_bg'];schedule(1140);open_settings()
        assert not any(x['id']=='night-combo' for x in state()['items'])
        click('automatic');click('apply');assert schedule()['mode']==1
        day=schedule()['day_bg'];day_file=require_saved_palette(profile,'day')
        check('night choice becomes available after enabling the existing schedule')
        colors=[]
        for index in range(5):
            choose_night(index)
            if state()['pending']:click('apply')
            observed=schedule(1140)
            assert state()['draft_night']==index and observed['period']=='night'
            assert observed['active_bg']==observed['night_bg'] and observed['day_bg']==day
            assert snapshot_files().get('theme_schedule_day.json')==day_file
            shot('night-'+str(index)+'-controls')
            click('done');count=check_pixels('night-'+str(index)+'-actual-main',observed['night_bg'])
            colors.append(observed['active_bg']);check('night preset '+str(index)+' reaches the actual main-window renderer',background=observed['active_bg'],matchingPixels=count)
            open_settings()
        assert len(set(colors))==5
        baseline=snapshot_files();active=schedule()['active_bg']
        choose_night(1);assert state()['pending'] and snapshot_files()==baseline and schedule()['active_bg']==active
        shot('unapplied-night-choice');click('revert')
        assert snapshot_files()==baseline and schedule()['active_bg']==active
        choose_night(1);click('close');assert state()['prompt'];click('discard')
        assert snapshot_files()==baseline and schedule()['active_bg']==active
        check('selection waits for Apply; revert and close-discard preserve the live palette and files')
        open_settings();choose_night(1);click('apply');night=schedule()['night_bg']
        for minute,period,color in ((0,'night',night),(479,'night',night),(480,'day',day),(959,'day',day),(960,'night',night),(1439,'night',night)):
            observed=schedule(minute);assert observed['period']==period and observed['active_bg']==color
            click('done');count=check_pixels('boundary-'+str(minute),color);open_settings()
            check('application test clock '+str(minute)+' selects '+period,background=color,matchingPixels=count)
        schedule(1140);saved=snapshot_files();click('automatic');click('apply')
        assert schedule()['mode']==0 and schedule()['active_bg']==manual_bg and snapshot_files()==saved
        click('done');check_pixels('schedule-off-manual-restored',manual_bg)
        check('schedule off restores the original manual theme without replacing either saved palette')
        open_settings();click('automatic');click('apply');click('done');stop()
        start();observed=schedule(1140);assert observed['mode']==1 and observed['active_bg']==night and observed['day_bg']==day
        require_loaded_palette(observed,'day')
        assert snapshot_files()==saved
        check_pixels('restart-night-retained',night);open_settings();assert state()['draft_night']==1
        shot('restart-night-controls');click('done')
        check('real process restart preserves the chosen night palette and independent day palette')
        schedule(-1);stop();report['passed']=True
    except Exception:
        report['error']=traceback.format_exc();print(report['error'],flush=True)
        try:
            if identity_verified:shot('failure')
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
    p.add_argument('--layout-only',action='store_true');run(p.parse_args())
