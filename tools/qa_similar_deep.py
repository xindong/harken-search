"""Real dual-mode similarity QA on a private desktop and an isolated library.
Requires a prepared real-audio-manifest.json under --isolation-root.
Never sends input to the user's desktop, reads another app, or edits source media.
"""
from __future__ import annotations
import argparse, hashlib, json, time
from pathlib import Path
from PIL import Image
from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_transport_themes import Driver, THEMES
from qa_shared_buttons import audit, contrast
from qa_info_family_themes import resize_similar

def run(args):
    import win32con, win32event, win32gui, win32process
    exe,root,out=args.exe.resolve(),args.isolation_root.resolve(),args.output.resolve()
    library=(args.library_db or root/'library/main.db').resolve()
    if not library.is_relative_to(root) or not library.is_file():
        raise ValueError(f'QA library must be an existing file inside the isolation root: {library}')
    out.mkdir(parents=True,exist_ok=False)
    manifest=json.loads((root/'real-audio-manifest.json').read_text(encoding='utf-8'))
    # The disposable profile must not auto-switch back to its day/night custom
    # palette while this suite measures each of the five stock themes.
    settings=root/'profile/Local/HarkenSearch/settings.ini'
    if settings.exists():
        lines=settings.read_text(encoding='utf-8-sig').splitlines()
        lines=[('themeScheduleMode=0' if line.startswith('themeScheduleMode=') else 'volume=0.0' if line.startswith('volume=') else line) for line in lines]
        settings.write_text('\n'.join(lines)+'\n',encoding='utf-8')
    driver=Driver(root/'profile/runtime',out)
    desktop=PrivateDesktop(); process=None; report={'exe':str(exe),'sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'checks':[],'queries':[],'captures':[],'passed':False}
    def save(): (out/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    def state(): return driver.command({'cmd':'similar_index','action':'state'})
    def until(probe,predicate,timeout=60):
        deadline=time.monotonic()+timeout; last=None; announced=0
        while time.monotonic()<deadline:
            last=probe()
            if predicate(last): return last
            if time.monotonic()-announced>10:
                (out/'progress.json').write_text(json.dumps(last,ensure_ascii=False),encoding='utf-8'); announced=time.monotonic()
            time.sleep(.15)
        raise AssertionError(last)
    def configure(mode,deep,preset=0): return driver.command({'cmd':'similar_index','action':'configure','mode':mode,'deep':deep,'preset':preset})
    def find():
        start=time.monotonic(); driver.command({'cmd':'similar_index','action':'find'})
        result=until(state,lambda s:s['status'] in (3,4),180)
        result['wall_ms']=(time.monotonic()-start)*1000
        assert result['status']==3 and result['error']==0 and result['rows']>0,result
        assert all(r['id']!=result['reference_id'] for r in result['matches']),result
        for row in result['matches']:
            assert abs(sum(p*w for p,w in zip(row['parts'],row['weights']))-row['score'])<1e-5,row
        return result
    def click(bounds):
        assert win32process.GetWindowThreadProcessId(report['identity']['hwnd'])[1]==report['identity']['pid']
        x,y=(bounds[0]+bounds[2])/2,(bounds[1]+bounds[3])/2
        for phase in ('hover','down','up'):
            driver.command({'cmd':'similar_index','action':'pointer','phase':phase,'x':x,'y':y});time.sleep(.08)
    try:
        process,pid=desktop.start(exe)
        identity=driver.command({'cmd':'magic_eq_identity'},timeout=60);report['identity']=identity
        assert identity['pid']==pid and Path(identity['exe_path']).resolve()==exe and Path(identity['isolation_root']).resolve()==root,identity
        wait_state(driver,lambda s:s['main_ui_ready'] and not s['db_switch_in_progress'],'Ready',60)
        win32gui.SetWindowPos(identity['hwnd'],0,40,40,1400,900,win32con.SWP_NOACTIVATE|win32con.SWP_NOZORDER)
        current=driver.command({'cmd':'get_state'})
        switched=None
        if not current['db_path'] or Path(current['db_path']).resolve()!=library.resolve():
            if current['db_path']:
                switched=driver.command({'cmd':'switch_db','path':str(library)})
                assert switched.get('started') or switched.get('skipped'),switched
            else:
                driver.command({'cmd':'info_edit_input','action':'open_existing_db','text':str(library)})
            current=wait_state(driver,lambda s:bool(s['db_path']) and Path(s['db_path']).resolve()==library.resolve() and not s['db_switch_in_progress'],'Open isolated real library',60)
        if current['result_count']<len(manifest['files']):
            driver.command({'cmd':'scan_folder','path':str(root/'audio'),'cache_waveforms':False,'build_similarity_index':False,'worker_threads':2})
            scan=until(lambda:driver.command({'cmd':'scan_status'}),lambda s:not s['scanning'],600)
            report['scan']=scan;assert scan['errors']==0,scan
            driver.command({'cmd':'scan_acknowledge_complete'})
        driver.command({'cmd':'search','text':''})
        current=wait_state(driver,lambda s:s['result_count']==len(manifest['files']),'Scanned real sounds',60)
        if args.check_db_only:
            assert switched and switched.get('started'),switched
            assert current['total_count']==len(manifest['files']),current
            report['scope']='db-switch-only'
            report['db_switch']={'receipt':switched,'path':current['db_path'],'total_count':current['total_count'],'result_count':current['result_count']}
            report['passed']=True;save();print('DB switch QA passed',flush=True)
            return
        driver.command({'cmd':'similar_index','action':'open','floating':True,'width':640,'height':640,'language':0})
        driver.command({'cmd':'select_row','row':0})
        until(state,lambda s:s['reference_id']>0)
        before=state();print('index-before',before['indexed'],before['semantic_indexed'],before['total'],flush=True)
        if before['semantic_indexed']<before['total']:
            driver.command({'cmd':'similar_index','action':'start'})
            until(state,lambda s:s['progress']>=2 or not s['running'],120)
            driver.command({'cmd':'similar_index','action':'cancel'})
            cancelled=until(state,lambda s:not s['running'],60)
            assert cancelled['indexed']>0 and not cancelled['completion_notice'],cancelled
            report['checks'].append({'case':'cancel-retains-completed','state':cancelled});save()
            driver.command({'cmd':'similar_index','action':'start'})
            indexed=until(state,lambda s:not s['running'],1200)
            report['index']=indexed;save()
            assert indexed['indexed']==indexed['total'] and indexed['semantic_indexed']==indexed['total'],indexed
        print('Index ready',state()['indexed'],flush=True)
        baseline=driver.command({'cmd':'get_state'})
        for mode in (0,1):
            for deep in (False,True):
                configure(mode,deep);assert state()['rows']==0
                result=find();report['queries'].append({'mode':mode,'deep':deep,'result':result});save()
                expected=0.4 if mode else 0
                assert all(abs(row['weights'][0]-expected)<1e-6 for row in result['matches'])
                after=driver.command({'cmd':'get_state'})
                assert after['selected_file_id']==baseline['selected_file_id'] and after['result_count']==baseline['result_count']
        report['checks'].append({'case':'four-real-query-combinations-and-main-selection','passed':True})
        configure(0,False);find()
        driver.command({'cmd':'set_theme','theme':0})
        current=audit(driver,0)
        mode=next(c for c in current['checks'] if 'SimilarActionRow' in c['window'] and c['label']=='听感接近')
        click(mode['bounds'])
        popup=audit(driver,0)
        option=next(c for c in popup['checks'] if c['window'].startswith('##Combo_') and c['label']=='同类替代')
        click(option['bounds']);assert state()['mode']==1 and state()['rows']==0
        current=until(state,lambda s:'similar.options.deep' in s['controls'])
        click(current['controls']['similar.options.deep']);assert state()['deep'] and state()['rows']==0
        report['checks'].append({'case':'actual-mode-and-deep-controls','passed':True});save()
        found=find()
        wave=until(state,lambda s:'similar.result.wave.0' in s['controls'])['controls']['similar.result.wave.0']
        click(wave)
        played=wait_state(driver,lambda s:s['audio_loaded'] and s['current_file']==found['matches'][0]['path'] and s['play_state']=='playing','Actual result audition',20)
        assert played['selected_file_id']==baseline['selected_file_id'],played
        driver.command({'cmd':'stop'})
        report['checks'].append({'case':'actual-result-waveform-audition-preserves-main-selection','passed':True})
        current=audit(driver,0)
        next_button=next(c for c in current['checks'] if 'SimilarActionRow' in c['window'] and c['label']=='换一批')
        click(next_button['bounds'])
        next_result=until(state,lambda s:s['status'] in (3,4),180)
        assert (next_result['status']==3 and next_result['rows']>0) or (next_result['status']==4 and next_result['error']==8 and next_result['rows']==0),next_result
        assert next_result['reference_id']==found['reference_id'],(found,next_result)
        assert not ({r['id'] for r in found['matches']} & {r['id'] for r in next_result['matches']}),(found,next_result)
        after=driver.command({'cmd':'get_state'})
        assert after['selected_file_id']==baseline['selected_file_id'] and after['result_count']==baseline['result_count']
        report['checks'].append({'case':'actual-next-batch-after-audition-keeps-reference-and-does-not-repeat','first':found,'second':next_result});save()
        # The small real corpus may exhaust the tighter same-type/deep query
        # in one page. Also exercise a real multi-page perceptual query.
        configure(0,False);first=find()
        click(until(state,lambda s:'similar.result.wave.0' in s['controls'])['controls']['similar.result.wave.0'])
        driver.command({'cmd':'stop'})
        controls=audit(driver,0)
        button=next(c for c in controls['checks'] if 'SimilarActionRow' in c['window'] and c['label']=='换一批')
        click(button['bounds']);second=until(state,lambda s:s['status'] in (3,4),180)
        assert second['status']==3 and second['rows']>0,second
        assert second['reference_id']==first['reference_id'],(first,second)
        assert not ({r['fingerprint'] for r in first['matches']} & {r['fingerprint'] for r in second['matches']}),(first,second)
        assert second['batch_number']==2 and second['query_stats']['detailed_comparisons']==0,second
        report['checks'].append({'case':'actual-perceptual-second-page-has-new-content','first':first,'second':second});save()
        for theme,name in THEMES:
            driver.command({'cmd':'set_theme','theme':theme})
            for width in (640,420):
                resize_similar(driver,width);observations=audit(driver,theme)
                controls=[c for c in observations['checks'] if 'SimilarActionRow' in c['window']]
                for c in controls:
                    b,clip=c['bounds'],c['clip']
                    assert b[0]>=clip[0]-1 and b[1]>=clip[1]-1 and b[2]<=clip[2]+1 and b[3]<=clip[3]+1,c
                    if not c['disabled']: assert contrast(c['fg'],c['fill'])>=4.5,c
                image=out/f'theme-{theme}-{width}.png';capture(driver,image)
                panel=driver.command({'cmd':'dock_state'})['panels']['similar-sounds']
                origin=win32gui.ClientToScreen(identity['hwnd'],(0,0));x,y=round(panel['x']-origin[0]),round(panel['y']-origin[1])
                with Image.open(image) as full:
                    bounds=(x,y,x+round(panel['w']),y+round(panel['h']))
                    assert bounds[0]>=0 and bounds[1]>=0 and bounds[2]<=full.width and bounds[3]<=full.height,(bounds,full.size)
                    full.crop(bounds).save(out/f'panel-{theme}-{width}.png')
                report['captures'].append({'theme':theme,'name':name,'width':width,'image':str(image),'panel':panel,'controls':controls});save()
        configure(0,False);driver.command({'cmd':'similar_index','action':'find'});configure(1,True)
        time.sleep(.5);assert state()['rows']==0 and state()['status']==1
        report['checks'].append({'case':'changed-options-discard-old-results','passed':True})
        driver.command({'cmd':'similar_index','action':'close'});driver.command({'cmd':'similar_index','action':'open','width':640,'height':640})
        assert state()['rows']==0
        report['checks'].append({'case':'reopen-requires-explicit-find','passed':True})
        configure(1,True,2)
        driver.command({'cmd':'info_edit_input','action':'exit'})
        assert win32event.WaitForSingleObject(process,15000)==win32con.WAIT_OBJECT_0
        assert win32process.GetExitCodeProcess(process)==0
        process.Close();process=None
        process,pid=desktop.start(exe)
        restarted=driver.command({'cmd':'magic_eq_identity'},timeout=60);report['identity']=restarted
        assert restarted['pid']==pid and Path(restarted['exe_path']).resolve()==exe
        wait_state(driver,lambda s:s['main_ui_ready'] and not s['db_switch_in_progress'],'Restarted profile',60)
        persisted=state();assert persisted['mode']==1 and persisted['deep'] and persisted['preset']==2,persisted
        report['checks'].append({'case':'mode-depth-preset-persist-after-restart','passed':True})
        report['passed']=True;save();print('Native QA passed',flush=True)
    except Exception as error:
        report['error']=repr(error);save();raise
    finally:
        if process is not None:
            if win32event.WaitForSingleObject(process,0)==win32con.WAIT_TIMEOUT:
                try: driver.command({'cmd':'info_edit_input','action':'exit'})
                except Exception: win32gui.PostMessage(report.get('identity',{}).get('hwnd',0),win32con.WM_CLOSE,0,0)
            if win32event.WaitForSingleObject(process,15000)!=win32con.WAIT_OBJECT_0: raise RuntimeError('Isolated QA process did not exit')
            report['exit_code']=win32process.GetExitCodeProcess(process);process.Close();save()
        desktop.close()
        if settings.exists():
            lines=settings.read_text(encoding='utf-8-sig').splitlines()
            settings.write_text('\n'.join('volume=1.0' if line.startswith('volume=') else line for line in lines)+'\n',encoding='utf-8')
if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--exe',type=Path,required=True);parser.add_argument('--isolation-root',type=Path,required=True);parser.add_argument('--library-db',type=Path);parser.add_argument('--check-db-only',action='store_true');parser.add_argument('--output',type=Path,required=True)
    run(parser.parse_args())
