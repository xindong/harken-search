"""Measure settings value centering and Interface theme row alignment on the real Appearance page."""
from pathlib import Path
import argparse,hashlib,json,time,traceback,uuid
from PIL import Image
from qa_required_library_setup import PrivateDesktop,capture,wait_state
from qa_fresh_result_driver import FreshResultDriver as Driver

COMBO_GAP_TOLERANCE=2.0
THEME_DELTA_TOLERANCE=1.0
COMBOS=('systemFont','listFont','language')

def _bg(img,box):
    crop=img.crop(box)
    colors=crop.getcolors(crop.width*crop.height) or []
    if not colors:return (0,0,0)
    return max(colors,key=lambda c:c[0])[1][:3]

def _ink(img,box,bg,tol=70):
    crop=img.crop(box).convert('RGB')
    px=crop.load()
    xs=[];ys=[]
    for y in range(crop.height):
        for x in range(crop.width):
            r,g,b=px[x,y]
            if abs(r-bg[0])+abs(g-bg[1])+abs(b-bg[2])>tol:
                xs.append(x);ys.append(y)
    if not xs:return None
    return (min(xs),min(ys),max(xs),max(ys))

def run(args):
    import win32con,win32event,win32process,win32gui
    exe=args.exe.resolve();root=args.isolation_root.resolve();out=args.output.resolve()
    out.mkdir(parents=True,exist_ok=False)
    (root/'profile/runtime').mkdir(parents=True,exist_ok=True)
    driver=Driver(root/'profile/runtime',out);desktop=PrivateDesktop()
    profile=root/'profile/Local/HarkenSearch/settings.ini'
    report=dict(tag=args.tag,exe=str(exe),sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),checks=[],shots=[])
    process=None;identity=None;verified=False;origin=[0.0,0.0];dialog=[0.0,0.0]
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
    def raw(id):
        return next(i for i in settle()['items'] if i['id']==id)['bounds']
    def local(id):
        a,b,c,d=raw(id)
        return (a-dialog[0],b-dialog[1],c-dialog[0],d-dialog[1])
    def pointer(x,y):
        ox,oy=win32gui.ClientToScreen(identity['hwnd'],(0,0))
        for phase in ('hover','down','up'):
            cmd(cmd='theme_schedule',action='pointer',phase=phase,x=x-ox,y=y-oy);settle()
    def click(id):
        a,b,c,d=item(id)['bounds'];pointer((a+c)/2,(b+d)/2)
    def open_settings():
        if not state()['open']:cmd(cmd='ordinary_button_window',module='settings',action='open');settle()
        click('nav-1');settle()
    def resize(w,h):
        cmd(cmd='settings_ui',action='resize',width=w,height=h);settle()
    def shot(name):
        capture(driver,out/(name+'.png'))
        v=state()
        crop=None
        if v['open']:
            dx,dy,ex,ey=v['bounds']
            dialog[0],dialog[1]=dx,dy
            crop=tuple(round(x) for x in (dx-origin[0],dy-origin[1],ex-origin[0],ey-origin[1]))
        report['shots'].append(name)
        return crop
    def check(label,**details):report['checks'].append(dict(case=label,**details));print(label,flush=True)
    def start():
        nonlocal process,identity,verified
        verified=False;process,pid=desktop.start(exe,environment_overrides={'AURA_RUNTIME_ROOT':str(root/'profile/runtime')})
        identity=cmd(cmd='magic_eq_identity')
        assert identity['pid']==pid and Path(identity['exe_path']).resolve()==exe and Path(identity['isolation_root']).resolve()==root
        verified=True
        ready=wait_state(driver,lambda v:v['main_ui_ready'],'main ui',90)
        report.setdefault('launches',[]).append(dict(pid=pid,count=ready.get('total_count'),database=ready.get('db_path')))
        return ready
    def stop():
        nonlocal process
        if process is None:return
        if verified:
            cmd(cmd='info_edit_input',action='tray_exit')
            assert win32event.WaitForSingleObject(process,15000)==win32con.WAIT_OBJECT_0
            assert win32process.GetExitCodeProcess(process)==0
        elif win32event.WaitForSingleObject(process,0)==win32con.WAIT_TIMEOUT:win32process.TerminateProcess(process,1)
        process.Close();process=None
    def choose_theme(index):
        click('theme');settle();click('stock-'+str(index));settle()
        if state()['pending']:click('apply');settle()
    def choose_font(size):
        fonts=(10,11,12,13,14,16,18,20,22,24)
        click('systemFont');settle();click('option-'+str(fonts.index(size)));settle()
        if state()['pending']:click('apply');settle()
    def measure_combo(img,id):
        a,b,c,d=local(id)
        box=(round(a),round(b),round(c),round(d))
        arrow=round(d-b)
        textLeft=box[0]+12
        textRight=box[2]-arrow
        bg=_bg(img,(textLeft+4,box[1]+4,textRight-4,box[3]-4))
        inner=(textLeft+4,box[1]+4,textRight-4,box[3]-4)
        ink=_ink(img,inner,bg)
        if ink is None:raise AssertionError('no combo ink '+id)
        ix0,ix1=ink[0]+inner[0],ink[2]+inner[0]
        return dict(id=id,frame=[box[0],box[1],box[2],box[3]],textLeft=textLeft,textRight=textRight,
            ink=[ix0,ink[1]+inner[1],ix1,ink[3]+inner[1]],
            leftGap=ix0-textLeft,rightGap=textRight-ix1,
            inkCenterY=((ink[1]+ink[3])/2)+inner[1],frameCenterY=(box[1]+box[3])/2)
    def measure_theme(img,font=18):
        a,b,c,d=local('stock-0')
        ga,gb,gc,gd=local('theme')
        left=round(a)
        rowTop=round(gb)
        band=(0,rowTop+6,left-12,rowTop+int(font)+12)
        bg=_bg(img,(left-70,rowTop+4,left-20,rowTop+30))
        ink=_ink(img,band,bg)
        if ink is None:raise AssertionError('no theme label ink')
        labelCenter=((ink[1]+ink[3])/2)+band[1]
        swatchCenter=(b+d)/2
        descBand=(left+2,round(d)+2,left+140,round(d)+40)
        descInk=_ink(img,descBand,_bg(img,(left+2,round(d)+2,left+140,round(d)+40)))
        return dict(rowTop=rowTop,swatchTop=b,swatchBottom=d,swatchCenter=swatchCenter,labelCenter=labelCenter,
            delta=labelCenter-swatchCenter,
            descTop=(descBand[1]+descInk[1]) if descInk else None,descBelowSwatch=bool(descInk and descBand[1]+descInk[1]>=d),
            layoutNote='group rect '+str([ga,gb,gc,gd]))
    def capture_state(tag,w,h,theme,font=18,page=1,combos_ids=COMBOS,with_theme=True):
        resize(w,h);settle()
        click('nav-'+str(page));settle()
        if theme is not None:choose_theme(theme);settle()
        if state().get('live_font')!=font:
            if page!=1:click('nav-1');settle()
            choose_font(font)
            if page!=1:click('nav-'+str(page));settle()
        name=f'{args.tag}-{tag}'
        crop=shot(name)
        with Image.open(out/(name+'.png')) as raw:
            img=raw.crop(crop).copy()
        img.save(out/(name+'-settings.png'))
        combos=[measure_combo(img,id) for id in combos_ids]
        themeRow=measure_theme(img,font) if with_theme else None
        geom={id:local(id) for id in combos_ids+('nav-'+str(page),)}
        return dict(shot=name,size=[w,h],theme=theme,font=font,page=page,combos=combos,themeRow=themeRow,geometry=geom)
    try:
        ready=start()
        origin[0],origin[1]=win32gui.ClientToScreen(identity['hwnd'],(0,0))
        report['clientOrigin']=origin
        lib=root/'fixture-library'
        if not ready.get('db_path'):
            name='ValueAlign-'+uuid.uuid4().hex[:8]
            cmd(cmd='info_edit_input',action='new_db_name',text=name)
            cmd(cmd='info_edit_input',action='new_db_create')
            ready=wait_state(driver,lambda v:bool(v.get('db_path')) and not v.get('db_switch_in_progress') and not v.get('new_db_dialog_open'),'created empty library',90)
            report['library']=name
        if ready.get('total_count',0)==0:
            scanned=cmd(cmd='scan_folder',path=str(lib),recursive=True,update_existing=True,remove_missing=False,cache_waveforms=False,build_similarity_index=False,worker_threads=2)
            report['scan']=scanned
            try:
                wait_state(driver,lambda v:v['total_count']>0,'fixture library',180)
            except TimeoutError:
                report['libraryEmpty']=True
        open_settings()
        if state()['live_font']!=18:choose_font(18)
        results=[]
        for theme in (0,4):
            results.append(capture_state('dark' if theme==0 else 'light',1120,800,theme))
        for theme in (0,4):
            results.append(capture_state('narrow-'+('dark' if theme==0 else 'light'),880,600,theme))
        for font in (14,24):
            results.append(capture_state('dark-font'+str(font),1120,800,0,font))
        results.append(capture_state('audio-page-dark',1120,800,None,18,2,('device',),False))
        click('nav-1');settle()
        report['states']=results
        check('captured appearance states',states=len(results))
        # A recording-only report let a misaligned row pass silently; fail the
        # run instead when the measured gaps leave the agreed tolerance.
        gaps=[abs(c['leftGap']-c['rightGap']) for st in results for c in st.get('combos') or []]
        deltas=[abs(st['themeRow']['delta']) for st in results if st.get('themeRow')]
        worst_gap=max(gaps) if gaps else None
        worst_delta=max(deltas) if deltas else None
        check('combo value centering',worstGapPx=worst_gap,tolerancePx=COMBO_GAP_TOLERANCE)
        check('theme row midline',worstDeltaPx=worst_delta,tolerancePx=THEME_DELTA_TOLERANCE)
        assert worst_gap is not None and worst_gap<=COMBO_GAP_TOLERANCE,(
            f'combo value off center by {worst_gap}px (tolerance {COMBO_GAP_TOLERANCE}px)')
        assert worst_delta is None or worst_delta<=THEME_DELTA_TOLERANCE,(
            f'theme swatch and label off the midline by {worst_delta}px (tolerance {THEME_DELTA_TOLERANCE}px)')
        try:
            click('done');settle()
        except Exception:
            pass
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
    p=argparse.ArgumentParser();p.add_argument('--exe',type=Path,required=True);p.add_argument('--isolation-root',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--tag',default='run')
    run(p.parse_args())

