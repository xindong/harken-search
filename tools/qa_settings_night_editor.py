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
        def pointer(x,y):
            ox,oy=win32gui.ClientToScreen(identity['hwnd'],(0,0))
            for phase in ('hover','down','up'):
                cmd(cmd='theme_schedule',action='pointer',phase=phase,x=x-ox,y=y-oy);settle()
        def rect_click(rect):
            a,b,c,d=rect;pointer((a+c)/2,(b+d)/2)
        def key(name):
            for down in (True,False):cmd(cmd='settings_ui',action='key',key=name,down=down);settle()
        def editor_item(id):
            item=next(i for i in schedule()['editor_ui']['items'] if i['id']==id)
            a,b,c,d=item['bounds'];l,t,r,bot=item['clip']
            assert a>=l-1 and b>=t-1 and c<=r+1 and d<=bot+1,item
            return item
        def button(label):
            first=cmd(cmd='ordinary_button_audit')['current_frame']
            for _ in range(30):
                audit=cmd(cmd='ordinary_button_audit')
                if audit['frame']>first+2:break
                settle()
            items=[i for i in audit['checks'] if i['label']==label and not i['disabled']]
            assert len(items)==1,(label,items)
            a,b,c,d=items[0]['bounds'];l,t,r,bot=items[0]['clip']
            assert a>=l-1 and b>=t-1 and c<=r+1 and d<=bot+1,items[0]
            rect_click(items[0]['bounds'])
        def open_editor():
            open_settings();click('night-editor');settle()
            assert schedule()['editor_open'] and schedule()['editor_period']==1
        def editor_shot(name):
            shot(name);a,b,c,d=schedule()['editor_ui']['bounds']
            ox,oy=win32gui.ClientToScreen(identity['hwnd'],(0,0))
            with Image.open(out/(name+'.png')) as im:im.crop(tuple(round(v) for v in (a-ox,b-oy,c-ox,d-oy))).save(out/(name+'-editor.png'))
        def pick(hexvalue,name):
            rect_click(editor_item('mainWindowBG')['bounds'])
            popups=schedule()['editor_ui']['popups'];assert len(popups)==1,popups
            a,b,c,d=popups[0]['bounds']
            # The real ImGui picker has its hexadecimal input in the final row.
            pointer(a+100,d-24)
            cmd(cmd='settings_ui',action='key',key='Ctrl',down=True);settle();key('A')
            cmd(cmd='settings_ui',action='key',key='Ctrl',down=False);settle()
            cmd(cmd='info_edit_input',action='text',text='#'+hexvalue);settle();key('Enter')
            editor_shot(name+'-picker')
            ea,eb,ec,ed=schedule()['editor_ui']['bounds'];pointer(ec-30,eb+45);settle()
            assert schedule()['editor_open']
            actual=schedule()['editor_ui']['draft_bg']
            assert actual.lower()=='ff'+hexvalue[:6].lower(),(actual,hexvalue)
            return actual
        schedule(1140);open_settings()
        if schedule()['mode']==0:click('automatic');click('apply')
        original=schedule();original_files=snapshot_files();manual_theme=state()['draft_theme']
        assert original_files.get('theme_schedule_day.json') and original_files.get('theme_schedule_night.json')
        shot('01-night-entry');open_editor();editor_shot('02-editor-open')
        assert schedule()['editor_ui']['live_preview']
        color=pick('253F59FF','03-custom-background')
        assert schedule()['active_bg']==color and snapshot_files()==original_files
        editor_shot('04-live-preview-unsaved');check('real color picker changes the rendered draft without writing saved files',background=color)
        rect_click(editor_item('live-preview')['bounds']);assert not schedule()['editor_ui']['live_preview']
        assert schedule()['active_bg']==original['night_bg'];editor_shot('05-preview-off')
        rect_click(editor_item('live-preview')['bounds']);assert schedule()['active_bg']==color
        button('取消修改');settle();assert not schedule()['editor_open']
        assert snapshot_files()==original_files and schedule()['active_bg']==original['night_bg']
        check('preview on/off and cancel restore the saved night palette')
        open_editor();color=pick('30485FFF','06-save-draft');button('保存配色');settle()
        assert not schedule()['editor_open'] and schedule()['active_bg']==color
        saved=snapshot_files();assert saved['theme_schedule_day.json']==original_files['theme_schedule_day.json']
        colors=json.loads(saved['theme_schedule_night.json'])['colors']['mainWindowBG']
        assert all(abs(v-w/255)<.00001 for v,w in zip(colors,[48,72,95,255])),colors
        assert state()['draft_theme']==manual_theme and not schedule()['night_warning']
        if state()['open']:click('done')
        count=check_pixels('07-saved-actual-main',color)
        check('save persists the exact picker RGBA and actual main-window background',background=color,matchingPixels=count)
        open_editor();pick('75654FFF','08-close-unsaved')
        a,b,c,d=schedule()['editor_ui']['bounds'];pointer(c-13,b+12);settle()
        assert not schedule()['editor_open'] and snapshot_files()==saved and schedule()['active_bg']==color
        check('editor title close discards unsaved changes')
        for minute,period in ((0,'night'),(479,'night'),(480,'day'),(959,'day'),(960,'night'),(1439,'night')):
            observed=schedule(minute);expected=color if period=='night' else original['day_bg']
            assert observed['active_bg']==expected and snapshot_files()==saved
            if state()['open']:click('done')
            count=check_pixels('boundary-'+str(minute),expected)
            check('custom night palette respects clock '+str(minute),period=period,matchingPixels=count)
        schedule(1140);stop();start();observed=schedule(1140)
        assert observed['active_bg']==color and snapshot_files()==saved and not observed['night_warning']
        check_pixels('09-restart-retained',color);check('real restart reloads the edited night palette and retains independent day data')
        open_settings();click('automatic');click('apply')
        assert state()['draft_theme']==manual_theme and snapshot_files()==saved
        click('done');check_pixels('10-manual-restored',original['active_bg'] if original['mode']==0 else 'ff0f141b')
        open_settings();click('automatic');click('apply');click('done');schedule(1140)
        check('schedule off keeps the stored manual theme and both palette files')
        open_settings()
        def query(value):
            click('search');cmd(cmd='settings_ui',action='key',key='Ctrl',down=True);settle();key('A')
            cmd(cmd='settings_ui',action='key',key='Ctrl',down=False);settle()
            cmd(cmd='info_edit_input',action='text',text=value);settle()
        for font,option in ((18,6),(24,9)):
            query('system font');click('result-systemFont');click('systemFont');click('option-'+str(option))
            if state()['pending']:click('apply')
            assert state()['live_font']==font;click('nav-1')
            for width,height in ((1120,800),(880,600)):
                cmd(cmd='settings_ui',action='resize',width=width,height=height);settle()
                require_window_size(state()['bounds'],width,height)
                find('night-editor');shot(f'layout-entry-{font}-{width}x{height}')
                click('night-editor');assert schedule()['editor_period']==1
                editor_item('mainWindowBG');editor_shot(f'layout-editor-{font}-{width}x{height}')
                button('取消修改');settle();assert snapshot_files()==saved
                check(f'editor entry and real save/cancel controls available at {font}px {width}x{height}')
                open_settings()
        query('system font');click('result-systemFont');click('systemFont');click('option-6');click('apply')
        click('done');schedule(-1);stop();report['passed']=True
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
