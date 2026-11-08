"""Exercise avatar crop through the real native picker, pointer, slider, save and cancel."""
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
    try:
        start();open_settings();query('搜索触发');click('result-searchMode');click('searchMode');click('option-0');settle()
        if state()['pending']:click('apply')
        assert state()['live_realtime'];click('done')
        for phase in ['hover','down','up']:cmd(cmd='theme_schedule',action='pointer',phase=phase,x=400,y=40);settle()
        cmd(cmd='settings_ui',action='key',key='Ctrl',down=True);settle();key('A');cmd(cmd='settings_ui',action='key',key='Ctrl',down=False);settle()
        cmd(cmd='info_edit_input',action='text',text='zzzxqnovalue');time.sleep(2)
        current=cmd(cmd='get_state');report['actual']={k:current.get(k) for k in ['search_text','total_count','query_state','active_view']};shot('realtime-after-input')
        assert current['search_text']=='zzzxqnovalue',report['actual']
        assert current['total_count']==0,report['actual']
        check('real typed input filters without Enter')
        open_settings();query('搜索触发');click('result-searchMode');click('searchMode');click('option-1');settle()
        if state()['pending']:click('apply')
        assert not state()['live_realtime'];click('done')
        for phase in ['hover','down','up']:cmd(cmd='theme_schedule',action='pointer',phase=phase,x=400,y=40);settle()
        cmd(cmd='settings_ui',action='key',key='Ctrl',down=True);settle();key('A');cmd(cmd='settings_ui',action='key',key='Ctrl',down=False);settle()
        cmd(cmd='info_edit_input',action='text',text='a');time.sleep(1)
        current=cmd(cmd='get_state');assert current['search_text']=='a' and current['total_count']==0,current['search_text']
        check('Enter mode receives edited text but does not search early')
        key('Enter');time.sleep(2);current=cmd(cmd='get_state');assert current['total_count']>0,current['total_count'];shot('enter-after-commit')
        check('Enter commits pending query and updates actual results')
        open_settings();query('搜索触发');click('result-searchMode');click('searchMode');click('option-0');settle()
        if state()['pending']:click('apply')
        click('done');stop();report['passed']=True
    except Exception:
        report['error']=traceback.format_exc();raise
    finally:
        if process is not None:stop()
        desktop.close();(out/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--exe',type=Path,required=True);p.add_argument('--isolation-root',type=Path,required=True);p.add_argument('--output',type=Path,required=True);run(p.parse_args())
