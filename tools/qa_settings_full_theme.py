"""Verify the nighttime setting through real controls, rendered pixels and restart."""
from pathlib import Path
import argparse,hashlib,json,time,traceback
from PIL import Image
from qa_required_library_setup import PrivateDesktop,capture,wait_state
from qa_fresh_result_driver import FreshResultDriver as Driver

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
            open_settings();button('编辑主题');settle()
            assert schedule()['editor_open'] and schedule()['editor_period']==-1
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
        if args.review_edges:
            from qa_owned_theme_dialog import start_file_dialog_worker
            schedule(-1);open_editor();original=schedule()['active_bg']
            custom=exe.parent/'theme_custom.json'
            before=custom.read_bytes() if custom.exists() else None
            if not args.import_only:
                pick('61372BFF','save-failure-draft')
                import os,stat
                if not custom.exists():custom.write_text('{}',encoding='utf-8')
                mode=custom.stat().st_mode
                os.chmod(custom,stat.S_IREAD)
                try:
                    button('保存为新主题');settle()
                    a,b,c,d=editor_item('theme-name')['bounds'];pointer(c+15,(b+d)/2);settle()
                    editor_shot('save-failure-preview-off')
                    assert schedule()['active_bg']==original,'Failed save replaced the restore baseline'
                    check('a real read-only theme file does not replace the saved restore baseline')
                finally:
                    os.chmod(custom,mode)
                    if before is None:custom.unlink()
                    else:custom.write_bytes(before)
                ea,eb,ec,ed=schedule()['editor_ui']['bounds'];pointer(ec-14,eb+13);settle()
                assert schedule()['active_bg']==original
                open_editor()
            if not schedule()['editor_ui']['live_preview']:
                a,b,c,d=editor_item('theme-name')['bounds'];pointer(c+15,(b+d)/2);settle()
            for index,document in enumerate((
                {'mainWindowBG':'ff45634a','extra':{'mainWindowBG':'xyz'}},
                {'wwiseConnectOk':'ff00ff00'},
                {'mainWindowBG':'ff45634a','wwiseConnectOk':'xyz'},
            )):
                target=out/f'edge-import-{index}.json';target.write_text(json.dumps(document),encoding='utf-8')
                prior=schedule()['active_bg']
                thread,result=start_file_dialog_worker(desktop,identity['pid'],target,out/f'edge-dialog-{index}.json')
                button('导入 JSON');thread.join(20);assert not thread.is_alive() and result.get('passed'),result
                settle();editor_shot(f'edge-import-{index}')
                actual=schedule()['editor_ui']
                if index==0:
                    assert actual['draft_bg']=='ff45634a' and schedule()['active_bg']=='ff45634a','Nested metadata overwrote the validated top-level color'
                    check('nested metadata cannot overwrite a validated top-level color')
                elif index==1:
                    assert actual['draft_wwise_ok']=='ff00ff00','Valid Wwise-only palette was rejected'
                    check('a valid Wwise-only partial palette remains importable')
                else:
                    assert schedule()['active_bg']==prior and actual['draft_wwise_ok']=='ff00ff00','Invalid Wwise color bypassed validation'
                    check('invalid Wwise colors are rejected without changing the draft')
                assert (custom.read_bytes() if custom.exists() else None)==before
            ea,eb,ec,ed=schedule()['editor_ui']['bounds'];pointer(ec-14,eb+13);settle()
            assert schedule()['active_bg']==original
            stop();report['passed']=True;return
        if args.preview_only:
            schedule(-1);open_editor();original=schedule()['active_bg']
            color=pick('253F59FF','preview-on')
            a,b,c,d=editor_item('theme-name')['bounds'];pointer(c+15,(b+d)/2);settle()
            assert not schedule()['editor_ui']['live_preview']
            editor_shot('preview-off')
            assert schedule()['active_bg']==original,'Preview off should restore the opened appearance'
            check('disabling live preview restores the opened theme')
            pick('705030FF','preview-disabled')
            assert schedule()['active_bg']==original
            pointer(c+15,(b+d)/2);settle()
            assert schedule()['editor_ui']['live_preview'] and schedule()['active_bg']=='ff705030'
            editor_shot('preview-resumed')
            check('enabling live preview immediately applies the current draft')
            ea,eb,ec,ed=schedule()['editor_ui']['bounds'];pointer(ec-14,eb+13);settle()
            assert schedule()['active_bg']==original
            stop();report['passed']=True;return
        schedule(-1);open_settings();shot('01-theme-entry');open_editor();editor_shot('02-editor-open')
        audit=cmd(cmd='ordinary_button_audit')
        (out/'editor-buttons.json').write_text(json.dumps(audit,ensure_ascii=False,indent=2),encoding='utf-8')
        check('actual full-theme editor opens from settings',period=schedule()['editor_period'])
        original=schedule()['active_bg'];before=(exe.parent/'theme_custom.json').read_bytes() if (exe.parent/'theme_custom.json').exists() else None
        color=pick('253F59FF','03-live-color')
        assert schedule()['active_bg']==color
        check_pixels('04-actual-live-preview',color)
        assert ((exe.parent/'theme_custom.json').read_bytes() if (exe.parent/'theme_custom.json').exists() else None)==before
        check('real picker previews main-window color before saving without writing a custom theme')
        button('放弃修改');settle();assert schedule()['active_bg']==original
        check('discard restores the original actual main-window palette')
        editor_shot('05-discarded')
        color=pick('30485FFF','06-save-color');button('保存为新主题');settle()
        custom=exe.parent/'theme_custom.json'
        assert custom.is_file()
        saved=custom.read_bytes();colors=json.loads(saved)
        assert colors['mainWindowBG']==color
        assert schedule()['active_bg']==color
        check_pixels('07-saved-main-window',color)
        check('save as new theme writes exact picker RGBA and applies it to actual main window')
        pick('75654FFF','08-unsaved-after-save')
        ea,eb,ec,ed=schedule()['editor_ui']['bounds'];pointer(ec-14,eb+13);settle()
        assert not schedule()['editor_open']
        assert schedule()['active_bg']==color and custom.read_bytes()==saved,'Closing after a save kept a later unsaved preview'
        check_pixels('08-closed-after-save',color)
        check('closing after save discards subsequent unsaved color changes')
        stop();start();assert schedule()['active_bg']==color and custom.read_bytes()==saved
        check_pixels('09-restart-retained',color)
        check('real process restart retains saved custom theme')
        from qa_owned_theme_dialog import start_file_dialog_worker
        def file_action(label,target,case):
            thread,result=start_file_dialog_worker(desktop,identity['pid'],target,out/(case+'-dialog.json'))
            button(label);thread.join(20)
            assert not thread.is_alive() and result.get('passed'),result
            settle();editor_shot(case)
        open_editor()
        exported=out/'exported-theme.json'
        file_action('导出 JSON',exported,'10-export-json')
        assert exported.is_file() and json.loads(exported.read_text('utf-8'))==json.loads(saved)
        check('real native save dialog exports the complete saved palette')
        file_action('导出 JSON',None,'11-cancel-export')
        assert custom.read_bytes()==saved
        check('cancelling export leaves the saved theme unchanged')
        pick('705030FF','12-before-import')
        file_action('导入 JSON',exported,'13-import-json')
        assert schedule()['active_bg']==color and custom.read_bytes()==saved
        check('native import restores the exported complete palette in live preview without resaving')
        for index,text in enumerate(('{broken json','{}','[]','{"mainWindowBG":"xyz"}')):
            invalid=out/f'invalid-theme-{index}.json';invalid.write_text(text,encoding='utf-8')
            file_action('导入 JSON',invalid,f'14-invalid-import-{index}')
            assert schedule()['active_bg']==color and custom.read_bytes()==saved
            check(f'invalid import {index} preserves live colors and the saved theme')
        for index,value in enumerate(('47612b','ff49366a')):
            legacy=out/f'partial-theme-{index}.json';legacy.write_text(json.dumps({'mainWindowBG':value}),encoding='utf-8')
            file_action('导入 JSON',legacy,f'14-partial-import-{index}')
            expected='ff'+value[-6:]
            assert schedule()['active_bg']==expected and schedule()['editor_ui']['draft_bg']==expected and custom.read_bytes()==saved
            check_pixels(f'14-partial-import-{index}-pixels',expected)
            check(f'valid partial theme with {len(value)} digit color remains importable without saving')
        created=out/'Imported full theme.json';new_palette=json.loads(saved);new_palette['mainWindowBG']='ff294f49'
        created.write_text(json.dumps(new_palette),encoding='utf-8')
        file_action('导入并创建主题',created,'15-import-create')
        assert schedule()['active_bg']=='ff294f49' and json.loads(custom.read_text('utf-8'))==new_palette
        check('native import and create applies and persists the complete imported theme')
        ea,eb,ec,ed=schedule()['editor_ui']['bounds'];pointer(ec-14,eb+13);settle()
        stop();start();assert schedule()['active_bg']=='ff294f49'
        check_pixels('16-import-created-after-restart','ff294f49')
        check('imported custom theme remains active after a real process restart')
        open_editor()
        baseline=custom.read_bytes()
        for index in range(8):
            rect_click(editor_item('preset-combo')['bounds']);settle()
            rect_click(editor_item('preset-option-'+str(index))['bounds']);settle()
            assert schedule()['active_bg']==schedule()['editor_ui']['draft_bg']
            editor_shot('preset-'+str(index))
            assert custom.read_bytes()==baseline
            check(f'editor preset {index} previews actual main window without overwriting the saved theme')
        button('放弃修改');settle();assert schedule()['active_bg']=='ff294f49'
        ea,eb,ec,ed=schedule()['editor_ui']['bounds'];pointer(ec-14,eb+13);settle()
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
                shot(f'layout-entry-{font}-{width}x{height}')
                button('编辑主题');assert schedule()['editor_period']==-1
                editor_item('mainWindowBG');editor_shot(f'layout-editor-{font}-{width}x{height}')
                ea,eb,ec,ed=schedule()['editor_ui']['bounds'];pointer(ec-14,eb+13);settle()
                assert custom.read_bytes()==baseline
                check(f'actual full-theme editor opens from {font}px {width}x{height} settings and closes without saving')
                open_settings()
        query('system font');click('result-systemFont');click('systemFont');click('option-6')
        if state()['pending']:click('apply')
        click('done')
        stop();report['passed']=True
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
    p.add_argument('--preview-only',action='store_true');p.add_argument('--review-edges',action='store_true');p.add_argument('--import-only',action='store_true');run(p.parse_args())
