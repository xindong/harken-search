"""Verify saved avatar pixels appear on the settings page immediately and survive reopening and restart."""
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
        verified=False;process,pid=desktop.start(exe,environment_overrides={'AURA_RUNTIME_ROOT':str(root/'profile/runtime')})
        wait_state(driver,lambda v:v['main_ui_ready'],'main UI ready before identity',90)
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
    from qa_owned_theme_dialog import start_file_dialog_worker
    from PIL import ImageChops,ImageStat
    source=out/'source.png'
    fixture=Image.new('RGB',(800,600))
    fixture.putdata([(x*255//799,y*255//599,80) for y in range(600) for x in range(800)])
    fixture.save(source)
    avatar=profile.with_name('avatar.png');original=avatar.read_bytes() if avatar.is_file() else None
    def select_image():
        thread,result=start_file_dialog_worker(desktop,identity['pid'],source,out/('picker-'+str(time.time_ns())+'.json'))
        click('avatar-change');thread.join(20)
        assert not thread.is_alive() and result.get('passed'),result
        settle();item('avatar-canvas')
    def zoom(value):
        a,b,c,d=item('avatar-zoom')['bounds']
        cmd(cmd='settings_ui',action='key',key='Ctrl',down=True);settle();pointer((a+c)/2,(b+d)/2)
        cmd(cmd='settings_ui',action='key',key='Ctrl',down=False);settle()
        cmd(cmd='settings_ui',action='key',key='Ctrl',down=True);settle();key('A')
        cmd(cmd='settings_ui',action='key',key='Ctrl',down=False);settle()
        cmd(cmd='info_edit_input',action='text',text=str(value));settle();key('Enter');settle()
    def drag(dx,dy):
        a,b,c,d=item('avatar-canvas')['bounds'];ox,oy=win32gui.ClientToScreen(identity['hwnd'],(0,0));x=(a+c)/2-ox;y=(b+d)/2-oy
        for phase,px,py in [('hover',x,y),('down',x,y),('move',max(1,x+dx),max(1,y+dy)),('up',max(1,x+dx),max(1,y+dy))]:
            cmd(cmd='theme_schedule',action='pointer',phase=phase,x=px,y=py);settle()
    def compare_saved(rect,label):
        with Image.open(avatar) as im:
            assert im.size==(512,512)
            expected=fixture.crop(rect).resize((512,512),Image.Resampling.BILINEAR)
            error=sum(ImageStat.Stat(ImageChops.difference(im.convert('RGB'),expected)).mean)/3
            assert error<2,(label,error)
            check(label,meanPixelError=error)
    def show_page():
        if any(i['id']=='tool-close' for i in settle()['items']):click('tool-close')
        query('avatar');click('result-avatar');settle()
    def verify_visible(name):
        a,b,c,d=item('avatar-current')['bounds'];ox,oy=win32gui.ClientToScreen(identity['hwnd'],(0,0));shot(name)
        with Image.open(out/(name+'.png')) as shot_image, Image.open(avatar) as png:
            actual=shot_image.convert('RGB').crop(tuple(round(v) for v in (a-ox,b-oy,c-ox,d-oy)))
            expected=png.convert('RGB').resize(actual.size,Image.Resampling.BILINEAR)
            actual.save(out/(name+'-avatar.png'))
            error=sum(ImageStat.Stat(ImageChops.difference(actual,expected)).mean)/3
            assert error<3,(name,error)
            check(name,actualImageMeanError=error)
    try:
        start();open_settings();query('avatar');click('result-avatar');click('avatar');select_image();zoom(2);drag(-64,0);click('avatar-save')
        compare_saved((310,150,610,450),'crop produces expected source region')
        assert 'hasCustomAvatar=1' in profile.read_text('utf-8-sig')
        check('avatar custom flag is persisted immediately at crop Save')
        show_page();verify_visible('01-saved-on-settings-page')
        saved=avatar.read_bytes();click('avatar');select_image();zoom(3);click('avatar-cancel');show_page();verify_visible('02-cancel-keeps-avatar');assert avatar.read_bytes()==saved
        click('done');open_settings();query('avatar');click('result-avatar');verify_visible('03-reopened-settings')
        click('done');stop();start();open_settings();query('avatar');click('result-avatar');verify_visible('04-restarted-process')
        click('avatar');select_image();click('avatar-reset');click('avatar-save');compare_saved((100,0,700,600),'second save produces different source region');show_page();verify_visible('05-replaced-avatar-refreshes-immediately')
        assert avatar.read_bytes()!=saved
        click('done');stop();report['passed']=True
    except Exception:
        report['error']=traceback.format_exc();raise
    finally:
        if process is not None:stop()
        if avatar.is_dir():avatar.rmdir()
        if original is not None:avatar.write_bytes(original)
        desktop.close();(out/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--exe',type=Path,required=True);p.add_argument('--isolation-root',type=Path,required=True);p.add_argument('--output',type=Path,required=True);run(p.parse_args())
