"""Exercise five simple custom colors through actual pickers in an isolated EXE."""
from pathlib import Path
import argparse,hashlib,json,time,traceback
from PIL import Image
from qa_required_library_setup import PrivateDesktop,capture,wait_state
from qa_fresh_result_driver import FreshResultDriver as Driver

def run(args):
    import win32con,win32event,win32process,win32gui
    exe=args.exe.resolve();root=args.isolation_root.resolve();out=args.output.resolve()
    out.mkdir(parents=True,exist_ok=False)
    runtime=root/'profile/runtime';runtime.mkdir(parents=True,exist_ok=True)
    profile=root/'profile/Local/HarkenSearch'
    driver=Driver(runtime,out);desktop=PrivateDesktop()
    report=dict(exe=str(exe),sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),checks=[])
    process=None;identity=None;verified=False
    def cmd(**p):return driver.command(p)
    def state():return cmd(cmd='settings_ui')
    def settle():
        first=state()['frame'];deadline=time.monotonic()+8
        while time.monotonic()<deadline:
            value=state()
            if value['frame']>first+2:return value
            time.sleep(.04)
        raise AssertionError('No fresh settings observation')
    def item(id):
        value=next(i for i in settle()['items'] if i['id']==id)
        a,b,c,d=value['bounds'];l,t,r,bot=value['clip']
        assert a>=l-1 and b>=t-1 and c<=r+1 and d<=bot+1,value
        return value
    def pointer(x,y):
        ox,oy=win32gui.ClientToScreen(identity['hwnd'],(0,0))
        for phase in ('hover','down','up'):
            cmd(cmd='theme_schedule',action='pointer',phase=phase,x=x-ox,y=y-oy);settle()
    def click(id):
        a,b,c,d=item(id)['bounds'];pointer((a+c)/2,(b+d)/2)
    def key(name):
        for down in (True,False):cmd(cmd='settings_ui',action='key',key=name,down=down);settle()
    def button(label):
        first=cmd(cmd='ordinary_button_audit')['current_frame']
        for _ in range(30):
            audit=cmd(cmd='ordinary_button_audit')
            if audit['frame']>first+2:break
            settle()
        (out/'last-button-audit.json').write_text(json.dumps(audit,ensure_ascii=False,indent=2),encoding='utf-8')
        items=[i for i in audit['checks'] if i['label']==label and not i['disabled']]
        assert len(items)==1,(label,items)
        a,b,c,d=items[0]['bounds'];l,t,r,bot=items[0]['clip']
        assert a>=l-1 and b>=t-1 and c<=r+1 and d<=bot+1,items[0]
        pointer((a+c)/2,(b+d)/2)
    def open_settings():
        if not state()['open']:cmd(cmd='ordinary_button_window',module='settings',action='open');settle()
        cmd(cmd='settings_ui',action='resize',width=1120,height=800);settle();click('nav-1')
    def open_tool():
        open_settings();button('自定义颜色…');settle()
        assert state()['tool']=='custom-colors'
    def shot(name,crop=False):
        capture(driver,out/(name+'.png'))
        if crop:
            modal=next(p for p in state()['color_popups'] if 'SettingsTool' in p['name'])
            a,b,c,d=modal['bounds'];ox,oy=win32gui.ClientToScreen(identity['hwnd'],(0,0))
            with Image.open(out/(name+'.png')) as im:im.crop(tuple(round(v) for v in (a-ox,b-oy,c-ox,d-oy))).save(out/(name+'-tool.png'))
    def pick(field,value,name):
        a,b,c,d=item('custom-'+field)['bounds'];pointer(a+12,(b+d)/2)
        popups=state()['color_popups'];(out/(name+'-popups.json')).write_text(json.dumps(popups,ensure_ascii=False),encoding='utf-8')
        picker=next(p for p in popups if p['name'].startswith('##Popup_'))
        a,b,c,d=picker['bounds'];pointer(a+100,d-40)
        cmd(cmd='settings_ui',action='key',key='Ctrl',down=True);settle();key('A')
        cmd(cmd='settings_ui',action='key',key='Ctrl',down=False);settle()
        cmd(cmd='info_edit_input',action='text',text='#'+value);settle();key('Enter')
        shot(name+'-picker')
        modal=next(p for p in state()['color_popups'] if 'SettingsTool' in p['name'])
        a,b,c,d=modal['bounds'];pointer(a+30,b+25);settle()
        expected='ff'+value.lower()
        assert state()['custom_colors'][field]==expected,(field,state())
        assert state()['rendered_colors'][field]==expected,(field,state())
        shot(name,True)
        return expected
    def close_tool():
        click('tool-close');settle();assert not state()['tool'];click('done');settle()
    def pixels(name,expected,field=None):
        cmd(cmd='select_row',row=0);settle();cmd(cmd='pause');settle()
        shot(name)
        rgb=tuple(int(expected[-6:][i:i+2],16) for i in (0,2,4))
        with Image.open(out/(name+'.png')) as im:
            colors=im.convert('RGB').getcolors(im.width*im.height)
            if field=='volume':
                # The actual volume control deliberately paints an opaque gradient.
                # Match that gradient, rather than requiring the unblended picker RGB.
                first=cmd(cmd='transport_audit')['current_frame']
                for _ in range(30):
                    audit=cmd(cmd='transport_audit')
                    if audit['frame']>first+2 and audit['checks']:break
                    settle()
                bounds=next(c['bounds'] for c in audit['checks'] if c['id']=='volume.outline')
                ox,oy=win32gui.ClientToScreen(identity['hwnd'],(0,0))
                crop=im.convert('RGB').crop(tuple(round(v) for v in (bounds[0]-ox,bounds[1]-oy,bounds[2]-ox,bounds[3]-oy)))
                crop.save(out/(name+'-volume.png'))
                colors=crop.getcolors(crop.width*crop.height)
                r,g,b=rgb
                count=sum(n for n,c in colors if abs(c[1]-g)<=1 and int(r*200/255)-1<=c[0]<=r+1 and int(b*100/255)-1<=c[2]<=int(b*200/255)+1)
            elif field=='accent':
                # Selected rows use 48/255 alpha. Observe actual changed pixels
                # against the same selected row before editing the accent.
                with Image.open(out/'accent-before-main.png') as before:
                    delta=[(rgb[i]-v)*48/255 for i,v in enumerate((85,156,213))]
                    count=sum(1 for a,b in zip(before.convert('RGB').getdata(),im.convert('RGB').getdata())
                              if all(abs((b[i]-a[i])-delta[i])<=3 for i in range(3)))
            else:
                count=sum(n for n,color in colors if all(abs(color[i]-rgb[i])<=1 for i in range(3)))
        assert count>20,(name,expected,count)
        return count
    def check(label,**details):report['checks'].append(dict(case=label,**details));print(label,flush=True)
    def saved_colors_match(actual,expected):
        # INI decimal floats and the diagnostic hex truncation may differ by
        # one channel step; actual screenshot matching uses the same tolerance.
        return actual.keys()==expected.keys() and all(
            abs(int(actual[k][i:i+2],16)-int(expected[k][i:i+2],16))<=1
            for k in expected for i in (0,2,4,6))
    def start():
        nonlocal process,identity,verified
        verified=False;process,pid=desktop.start(exe)
        identity=cmd(cmd='magic_eq_identity')
        assert identity['pid']==pid and Path(identity['exe_path']).resolve()==exe and Path(identity['isolation_root']).resolve()==root
        verified=True
        ready=wait_state(driver,lambda s:s['main_ui_ready'] and s['total_count']>0,'real copied library',90)
        report.setdefault('launches',[]).append(dict(pid=pid,count=ready['total_count'],database=ready['db_path']))
        cmd(cmd='select_row',row=0);settle();time.sleep(.6);cmd(cmd='pause');settle()
    def stop():
        nonlocal process
        if process is None:return
        if verified:
            cmd(cmd='info_edit_input',action='tray_exit')
            assert win32event.WaitForSingleObject(process,15000)==win32con.WAIT_OBJECT_0
            assert win32process.GetExitCodeProcess(process)==0
        else:
            if win32event.WaitForSingleObject(process,0)==win32con.WAIT_TIMEOUT:win32process.TerminateProcess(process,1)
        process.Close();process=None
    try:
        start();open_tool();click('custom-reset');settle();shot('01-custom-colors-open',True)
        baseline={'bg':'ff1e1e1e','text':'ffcccccc','accent':'ff559cd5','waveform':'ffaed6ff','volume':'ff31c750'}
        assert state()['custom_colors']==baseline
        values={'bg':'253F59','text':'E8D5A9','accent':'D47491','waveform':'66C7B2','volume':'D9A54D'}
        for field,value in values.items():
            if field=='accent':
                close_tool();cmd(cmd='select_row',row=0);settle();cmd(cmd='pause');settle();shot('accent-before-main');open_tool()
            expected=pick(field,value,'02-'+field)
            check(field+' actual picker updates the stored value and live drawing colors',value=expected)
            close_tool();count=pixels('03-'+field+'-actual-main',expected,field)
            check(field+' custom color appears in actual main-window pixels without a save click',matchingPixels=count)
            open_tool()
        close_tool();stop();start()
        expected={k:'ff'+v.lower() for k,v in values.items()}
        assert state()['custom_enabled'] and saved_colors_match(state()['custom_colors'],expected) and saved_colors_match(state()['rendered_colors'],expected),state()
        for field,value in expected.items():pixels('04-restart-'+field,value,field)
        check('all five colors survive real process restart without clicking Apply')
        open_tool();click('custom-reset');settle();shot('05-reset',True)
        assert saved_colors_match(state()['custom_colors'],baseline) and saved_colors_match(state()['rendered_colors'],baseline)
        close_tool();stop();start()
        assert saved_colors_match(state()['custom_colors'],baseline) and saved_colors_match(state()['rendered_colors'],baseline)
        shot('06-reset-after-restart');check('reset restores all five default colors and survives restart')
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
                bounds=state()['bounds'];assert abs(bounds[2]-bounds[0]-width)<2 and abs(bounds[3]-bounds[1]-height)<2
                shot(f'07-entry-{font}-{width}x{height}')
                button('自定义颜色…');settle()
                for field in values:item('custom-'+field)
                item('custom-reset');item('tool-close');shot(f'08-tool-{font}-{width}x{height}',True)
                click('tool-close');settle()
                check(f'all five color controls and reset/close are visible at {font}px in {width}x{height} settings')
        query('system font');click('result-systemFont');click('systemFont');click('option-6')
        if state()['pending']:click('apply')
        click('done')
        stop();report['passed']=True
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
