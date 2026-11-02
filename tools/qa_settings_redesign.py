"""Exercise real native settings using only the isolated app's input queue.

The caller supplies an isolated EXE/profile with a real library snapshot.
Screenshots come from the application's framebuffer on a private desktop.
"""
from pathlib import Path
import argparse, hashlib, json, time, traceback
from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_transport_themes import Driver

def run(args):
    import win32con,win32event,win32process,win32gui
    exe=args.exe.resolve();root=args.isolation_root.resolve();out=args.output.resolve()
    out.mkdir(parents=True,exist_ok=False)
    runtime=root/'profile/runtime';runtime.mkdir(parents=True,exist_ok=True)
    driver=Driver(runtime,out);desktop=PrivateDesktop();process=None
    report={'exe':str(exe),'sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'checks':[]}
    def command(**payload):return driver.command(payload)
    def state():return command(cmd='settings_ui')
    def settle():
        first=state()['frame'];deadline=time.monotonic()+8
        while time.monotonic()<deadline:
            value=state()
            if value['frame']>first+2:
                assert not value.get('imgui_errors'),value.get('imgui_log')
                return value
            time.sleep(.05)
        raise AssertionError('No fresh settings frame')
    def visible(item):
        a,b,c,d=item['bounds'];l,t,r,bot=item['clip']
        return a>=l-1 and b>=t-1 and c<=r+1 and d<=bot+1 and c>a and d>b
    def find(id):
        found=[x for x in settle()['items'] if x['id']==id and visible(x)]
        assert len(found)==1,(id,found,state())
        return found[0]
    def pointer(x,y):
        origin=win32gui.ClientToScreen(identity['hwnd'],(0,0))
        x-=origin[0];y-=origin[1]
        for phase in ('hover','down','up'):
            command(cmd='theme_schedule',action='pointer',phase=phase,x=x,y=y);settle()
    def click(id,fraction=.5):
        a,b,c,d=find(id)['bounds'];pointer(a+(c-a)*fraction,(b+d)/2)
    def key(name):
        command(cmd='settings_ui',action='key',key=name,down=True);settle()
        command(cmd='settings_ui',action='key',key=name,down=False);settle()
    def query(value):
        click('search')
        command(cmd='settings_ui',action='key',key='Ctrl',down=True);settle()
        key('A')
        command(cmd='settings_ui',action='key',key='Ctrl',down=False);settle()
        command(cmd='info_edit_input',action='text',text=value);settle()
    def check(name,**details):report['checks'].append({'case':name,**details});print(name,flush=True)
    def screenshot(name):
        from PIL import Image
        observed=settle();path=out/(name+'.png');capture(driver,path)
        if observed['open']:
            ox,oy=win32gui.ClientToScreen(identity['hwnd'],(0,0))
            a,b,c,d=observed['bounds'];box=tuple(round(n) for n in (a-ox,b-oy,c-ox,d-oy))
            with Image.open(path) as source:source.crop(box).save(out/(name+'-crop.png'))
            report.setdefault('screenshots',[]).append({'file':path.name,'crop':name+'-crop.png','bounds':box,'frame':observed['frame']})
    try:
        process,pid=desktop.start(exe)
        identity=driver.command({'cmd':'magic_eq_identity'},timeout=60)
        assert identity['pid']==pid and Path(identity['exe_path']).resolve()==exe
        assert Path(identity['isolation_root']).resolve()==root
        report['identity']=identity
        origin=win32gui.ClientToScreen(identity['hwnd'],(0,0))
        current=wait_state(driver,lambda x:x['main_ui_ready'] and x['total_count']>0,'real library startup',90)
        report['library']={'path':current['db_path'],'count':current['total_count']}
        if (exe.parent/'qa-narration.json').is_file():
            note=command(cmd='qa_narration',action='state')
            assert note['open'] and not note['error'],note
            report['narration']=note;capture(driver,out/'00-package-notes.png')
            command(cmd='qa_narration',action='close');settle()
        command(cmd='dock_pointer',phase='focus')
        command(cmd='ordinary_button_window',module='settings',action='open');settle()
        command(cmd='settings_ui',action='resize',width=1120,height=800);settle()
        assert state()['open'];screenshot('01-common')
        for id in ('nav-0','search','device','volume','done'):assert find(id)['font_pixels']>=18
        for id in ('settings-title','page-title'):assert find(id)['font_pixels']>=24
        report['typography']={'bodyMinimum':18,'titleMinimum':24,'savedGlobalFont':state()['live_font']}
        if args.expected_volume is not None:assert abs(state()['live_volume']-args.expected_volume)<.001
        if args.smoke:
            check('native settings open',state=state())
            return
        initial=state()['live_volume']
        click('volume',.25);edited=settle();assert edited['pending']>0 and edited['live_volume']==initial
        draft=edited['draft_volume'];assert abs(draft-initial)>.01
        click('nav-2');assert abs(settle()['draft_volume']-draft)<.0001
        click('revert');assert state()['pending']==0 and state()['live_volume']==initial
        check('draft shared across Common and Audio; revert writes nothing')
        click('nav-0');click('volume',.8 if initial<.6 else .2)
        command(cmd='settings_ui',action='fail_save_once');click('apply')
        failed=settle();assert failed['pending']>0 and failed['error'] and failed['live_volume']==initial
        screenshot('02-save-failure');click('apply');saved=settle()
        assert saved['pending']==0 and not saved['error'] and saved['live_volume']!=initial
        check('failed save keeps draft; retry applies',volume=saved['live_volume'])
        click('volume',.6);click('close');assert settle()['prompt'];screenshot('03-unsaved-close')
        click('keep-editing');assert state()['open'] and state()['pending']>0
        click('close');click('discard');assert not settle()['open']
        command(cmd='ordinary_button_window',module='settings',action='open');settle()
        assert state()['pending']==0 and state()['live_volume']==saved['live_volume']
        check('close offers keep editing or discard; reopen preserves applied value')
        for i in range(9):
            click('nav-'+str(i));assert state()['page']==i
            screenshot('page-'+str(i))
        check('all nine categories render')
        query('FFT');assert find('result-fft');screenshot('04-search')
        click('result-fft');assert state()['page']==1 and find('fft-control')
        check('search opens appearance and unfolds spectrum controls')
        query('monitor');click('result-monitor');click('monitor')
        choice=1 if state()['live_monitor']!=1 else 2
        click('option-'+str(choice));assert state()['draft_monitor']==choice
        click('apply');assert state()['live_monitor']==choice
        check('channel monitor dropdown remains selectable inside scrolling page')
        click('nav-0');click('device');find('device-default');key('Escape')
        assert state()['open'] and not state()['prompt']
        assert not any(item['id']=='device-default' for item in state()['items'])
        click('searchMode');click('option-1' if state()['draft_realtime'] else 'option-0')
        key('Escape');assert state()['prompt'];click('apply-close');assert not settle()['open']
        command(cmd='ordinary_button_window',module='settings',action='open');settle()
        key('Escape');assert not settle()['open']
        command(cmd='ordinary_button_window',module='settings',action='open');settle()
        check('Escape closes dropdown only; dirty Escape prompts; apply-and-close saves; clean Escape closes')
        click('nav-1')
        if state()['schedule']==0:click('automatic')
        click('day-combo');click('theme-option-2')
        click('night-combo');click('theme-option-4')
        assert state()['draft_day']==2 and state()['draft_night']==4
        click('apply');assert state()['draft_day']==2 and state()['draft_night']==4
        screenshot('06-day-night');click('automatic');click('apply')
        assert state()['schedule']==0
        check('day and night offer all five themes; selections persist and schedule can be disabled')
        click('nav-0');click('volume',.45);query('thesaurus');click('result-dictionary');click('dictionary')
        assert state()['prompt'];click('discard');assert state()['tool']=='dictionary'
        screenshot('07-thesaurus');click('tool-close');assert state()['open'] and not state()['pending']
        check('specialty tool resolves pending changes and returns to settings')
        click('nav-0')
        for index in range(5):
            click('stock-'+str(index))
            if state()['pending']:click('apply')
            screenshot('theme-'+str(index))
        check('five native themes')
        command(cmd='settings_ui',action='resize',width=880,height=600);settle();screenshot('05-small-window')
        for id in ('search','close','done','nav-0','nav-8'):find(id)
        check('small-window navigation and footer remain accessible')
        command(cmd='settings_ui',action='resize',width=1120,height=800);settle()
        query('system font');click('result-systemFont');click('systemFont');click('option-9');click('apply')
        assert state()['live_font']==24
        assert find('search')['font_pixels']>=24 and find('done')['font_pixels']>=24
        screenshot('08-large-font');find('search');find('close');find('done')
        query('system font');click('result-systemFont');click('systemFont');click('option-4');click('apply')
        assert state()['live_font']==14
        check('24 px reading font keeps search, close and footer usable; restored 14 px')
        report['final_volume']=state()['live_volume']
        click('done');assert not settle()['open']
    except Exception:
        report['error']=traceback.format_exc();print(report['error'],flush=True)
        try:screenshot('failure')
        except Exception:pass
        raise
    finally:
        if process is not None:
            try:
                if win32event.WaitForSingleObject(process,0)==win32con.WAIT_TIMEOUT:command(cmd='info_edit_input',action='exit')
                assert win32event.WaitForSingleObject(process,15000)==win32con.WAIT_OBJECT_0
                report['exit_code']=win32process.GetExitCodeProcess(process);process.Close()
            except Exception:report['exit_error']=traceback.format_exc()
        desktop.close();(out/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--exe',type=Path,required=True);parser.add_argument('--isolation-root',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);parser.add_argument('--smoke',action='store_true');parser.add_argument('--expected-volume',type=float)
    run(parser.parse_args())
