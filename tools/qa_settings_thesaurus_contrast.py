"""Measure actual thesaurus glyph contrast and exercise native edit/cancel controls."""
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
    def choose_mode(mode):
        query('waveform');click('result-waveMode');click('waveMode');click('option-'+str(mode));settle()
    def choose_row_height(value):
        query('row height');click('result-rowHeight')
        a,b,c,d=item('rowHeight')['bounds']
        cmd(cmd='settings_ui',action='key',key='Ctrl',down=True);settle();pointer((a+c)/2,(b+d)/2)
        cmd(cmd='settings_ui',action='key',key='Ctrl',down=False);settle()
        cmd(cmd='settings_ui',action='key',key='Ctrl',down=True);settle();key('A')
        cmd(cmd='settings_ui',action='key',key='Ctrl',down=False);settle()
        cmd(cmd='info_edit_input',action='text',text=str(value));settle();key('Enter');settle()
    def verify(mode,draft=None):
        v=settle();assert v['live_wave_mode']==mode and v['draft_wave_mode']==(mode if draft is None else draft),v
        assert saved('waveformDisplayMode')==mode
        assert v['live_spectrum']==(mode==2),v
        if mode<2:
            assert v['drawn_wave_frame']>=v['current_frame']-3,v
            assert v['drawn_wave_style']==(3 if mode==0 else 2),dict(mode=mode,actual_style=v['drawn_wave_style'])
            assert v['drawn_spectrum_frame']<v['current_frame']-3,v
        else:
            deadline=time.monotonic()+45
            while v['drawn_spectrum_frame']<v['current_frame']-3:
                assert time.monotonic()<deadline,'actual spectrogram texture was not drawn'
                time.sleep(.1);v=settle()
        return dict(mode=mode,style=v['drawn_wave_style'],wave_frame=v['drawn_wave_frame'],spectrum_frame=v['drawn_spectrum_frame'],current_frame=v['current_frame'])
    def load():
        assert cmd(cmd='load_path',path=str(next((root/'fixture-library').glob('*.wav'))))['ok']
        deadline=time.monotonic()+45
        while True:
            v=settle()
            if max(v['drawn_wave_frame'],v['drawn_spectrum_frame'])>=v['current_frame']-3:break
            assert time.monotonic()<deadline,'real waveform or spectrogram did not render';time.sleep(.1)
        cmd(cmd='pause');settle()
    def luminance(rgb):
        c=[v/255 for v in rgb]
        c=[v/12.92 if v<=.04045 else ((v+.055)/1.055)**2.4 for v in c]
        return .2126*c[0]+.7152*c[1]+.0722*c[2]
    def rules_snapshot(name,old=False):
        from collections import Counter
        v=settle();a,b,c,d=item('thesaurus-edit-first')['bounds']
        popup=next(w for w in v['color_popups'] if 'SettingsTool' in w['name'])
        left,top,right,bottom=popup['bounds'];ox,oy=win32gui.ClientToScreen(identity['hwnd'],(0,0))
        shot(name)
        with Image.open(out/(name+'.png')) as im:
            roi=im.convert('RGB').crop((round(left+38-ox),round(b-5-oy),round(a-12-ox),round(d+4-oy)))
            counts=Counter(roi.getdata());bg=counts.most_common(1)[0][0]
            fg=(102,204,255) if old else tuple(bytes.fromhex(v['thesaurus_text_color'][2:]))
            pixels=counts[fg]
            roi.save(out/(name+'-rules.png'))
        contrast=(max(luminance(fg),luminance(bg))+.05)/(min(luminance(fg),luminance(bg))+.05)
        evidence=dict(theme=v['live_theme'],foreground=fg,background=bg,contrast=contrast,actualGlyphPixels=pixels)
        report.setdefault('measurements',[]).append(evidence)
        print(evidence,flush=True)
        assert pixels>=5,'No actual foreground glyph pixels in first rule'
        assert contrast>=4.5,evidence
        return evidence
    try:
        start();load();open_settings();choose_font(18);apply();choose_list(18);apply()
        if not args.red:
            choose_font(24);choose_list(22);v=settle()
            assert v['reading_font_pixels']==24 and v['drawn_list_font_pixels']==22 and v['live_font']==18 and v['live_list_font']==18,v
            check('appearance font preview remains immediate in the contrast-fix package')
            click('revert');v=settle();assert v['reading_font_pixels']==18 and v['drawn_list_font_pixels']==18,v
            check('appearance preview Revert still restores both actual fonts')
        for theme in ([4] if args.red else [4,1,0,2,3]):
            choose_theme(theme);apply();query('thesaurus');click('result-dictionary');click('dictionary');settle()
            evidence=rules_snapshot('theme'+str(theme),args.red)
            check('theme '+str(theme)+' actual rule text contrast at least 4.5',**evidence)
            click('thesaurus-edit-first');item('thesaurus-save');item('thesaurus-cancel');shot('theme'+str(theme)+'-editing')
            click('thesaurus-cancel');click('tool-close');settle();assert state()['open'] and not state()['pending']
            check('theme '+str(theme)+' Edit, Cancel and Close retain settings workflow')
        choose_theme(0);apply();click('done');stop();report['passed']=True
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
    p=argparse.ArgumentParser();p.add_argument('--exe',type=Path,required=True);p.add_argument('--isolation-root',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--red',action='store_true');run(p.parse_args())
