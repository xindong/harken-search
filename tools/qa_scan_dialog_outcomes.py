"""Run real scan pause/revoke/empty/similarity workflows on a private desktop.

Requires the same pinned isolation EXE and dependencies as qa_scan_dialog_simple.
All newly created libraries/audio stay below the pinned root. Existing QA data
is retained. No fabricated app states, shared mouse or installed process access.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import time
import traceback
import uuid
import wave

from qa_required_library_setup import PrivateDesktop, capture
from qa_transport_themes import Driver
from qa_scan_dialog_simple import Smoke, require, inside, path_key, sha256, save_json, utc_now, make_wavs


def run(args):
    import win32con, win32event, win32process, win32gui
    exe, root, out = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    require(exe.is_file() and inside(root, Path('D:/Harken-QA')) and inside(out, Path('D:/Harken-QA')), '仅接受本机 QA 根目录内的固定隔离包')
    require(not out.exists(), '保留旧证据，请使用新输出目录')
    runtime = root / 'profile/runtime'
    require(not (runtime/'qa_cmd.json').exists() and not (runtime/'qa_scan_dialog_simple.lock').exists(), '另一条 QA 命令仍在运行')
    out.mkdir(parents=True)
    runtime.mkdir(parents=True, exist_ok=True)
    driver = Driver(runtime, out)
    desktop = PrivateDesktop()
    process, pid = desktop.start(exe)
    report = {'status':'running','started_at':utc_now(),'exe':str(exe),'exe_sha256':sha256(exe),
              'checks':[], 'human_visual_acceptance':'未验收',
              'not_covered':['网络失联、真实磁盘故障、撤销失败和拖入通知尚未做实机故障注入',
                             '未用此脚本证明每个瞬时阶段都已截图']}
    verified = False
    ui = Smoke(driver, root, out, 180)
    try:
        identity = driver.command({'cmd':'magic_eq_identity'}, timeout=60)
        require(identity['pid'] == pid and path_key(identity['exe_path']) == path_key(exe)
                and path_key(identity['isolation_root']) == path_key(root), '启动身份不符')
        verified = True
        report['identity'] = identity
        ui.wait(lambda s:s['main_ui_ready'] and not s['db_switch_in_progress'], '等待主界面', 60,
                lambda:driver.command({'cmd':'get_state'}))

        def new_library(label):
            state = driver.command({'cmd':'get_state'})
            if not state['new_db_dialog_open']:
                driver.command({'cmd':'info_edit_input','action':'new_db_dialog'})
                ui.wait(lambda s:s['new_db_dialog_open'], '打开新建库', 15, lambda:driver.command({'cmd':'get_state'}))
            name = 'ScanDialogQA-' + label + '-' + uuid.uuid4().hex[:8]
            driver.command({'cmd':'info_edit_input','action':'new_db_name','text':name})
            driver.command({'cmd':'info_edit_input','action':'new_db_create'})
            state = ui.wait(lambda s:bool(s['db_path']) and not s['db_switch_in_progress'] and not s['new_db_dialog_open'],
                            '创建独立测试库', 40, lambda:driver.command({'cmd':'get_state'}))
            database = Path(state['db_path']).resolve()
            require(inside(database,root/'profile') and database.parent.name == name, '新库身份不符')
            require(not ui.rows(database), '新库不是空库')
            driver.command({'cmd':'scan_dialog_ui','action':'theme','theme':0})
            return database

        def remember(name, state, image=None):
            report['checks'].append({'name':name,'state':state,'image':image})
            save_json(out/'report.json',report)
            print(name + ' 通过', flush=True)

        def result(phase):
            state = ui.wait(lambda s:s['surface']=='result' and ui.has(s,'scan.result.done'), '等待结果 '+phase)
            require(state['phase']==phase and not state['is_scanning'] and not state['completion_pending'], '结果类型错误 '+str(state))
            return state

        def done():
            ui.click('scan.result.done','##ScanResult')
            ui.wait(lambda s:s['surface']=='setup' and not ui.has(s,'scan.result.done'), '关闭结果')

        database = new_library('outcomes')
        fixtures = root / ('scan-outcomes-' + uuid.uuid4().hex[:10])
        empty = fixtures/'empty'
        empty.mkdir(parents=True)
        ui.setup([empty]); ui.click('scan.dialog.start')
        state = result('empty')
        require(not ui.rows(database),'空扫描改变了库')
        remember('empty-folder',state,ui.shot('empty-result','scan.result.done','##ScanResult'))
        done()

        damaged = fixtures/'failures'
        make_wavs(damaged,3)
        broken = damaged/'bad (1).wav'
        broken.write_bytes(b'')
        ui.setup([damaged]); ui.click('scan.dialog.start')
        state = result('partial')
        # The existing scanner deliberately persists failed-file placeholders;
        # UI success must exclude that row, not silently change persistence.
        expected_rows = {path_key(p) for p in damaged.glob('*.wav')}
        require(state['succeeded']==3 and state['failed']==1 and ui.rows(database)==expected_rows,'失败被计入成功或记录身份不符')
        remember('actual-empty-audio-failure',state,ui.shot('partial-result','scan.result.failures','##ScanResult'))
        ui.click('scan.result.failures','##ScanResult')
        ui.shot('failure-details','scan.failures.back','##ScanFailureDetails')
        ui.click('scan.failures.back','##ScanFailureDetails')
        done()

        missing = damaged/'sample-0000.wav'
        preserved = damaged/'sample-0000.wav.retained'
        require(inside(missing,fixtures) and inside(preserved,fixtures) and not preserved.exists(),'夹具移动范围不符')
        digest = sha256(missing)
        missing.rename(preserved)
        try:
            driver.command({'cmd':'scan_dialog_ui','action':'cleanup'})
            state = result('completed')
            require(ui.rows(database)==expected_rows-{path_key(missing)} and sha256(preserved)==digest,'清理没有保留其他记录或原音频')
            remember('legacy-cleanup-result',state,ui.shot('cleanup-result','scan.result.done','##ScanResult'))
            done()
        finally:
            preserved.rename(missing)
        baseline = ui.rows(database)

        folders = [fixtures/'pause-a',fixtures/'pause-b']
        for folder in folders:
            make_wavs(folder,3000)
        before = {str(p):sha256(p) for folder in folders for p in folder.glob('*.wav')}
        ui.setup(folders); ui.click('scan.dialog.start')
        ui.wait(lambda s:s['surface']=='progress' and s['can_stop'], '等待可停止的真实扫描')
        ui.shot('scan-progress','scan.dialog.stop')
        ui.click('scan.dialog.stop')
        ui.shot('scan-stop-choice','scan.dialog.pause','##ScanStopChoice')
        ui.click('scan.dialog.continue','##ScanStopChoice')
        require(ui.state()['stop_disposition']==0,'继续扫描误触发停止')
        ui.click('scan.dialog.stop')
        ui.click('scan.dialog.pause','##ScanStopChoice')
        paused = result('paused')
        kept = ui.rows(database)
        require(baseline <= kept and len(kept-baseline)==paused['succeeded'] and paused['stop_disposition']==1,'暂停后的入库保留计数不符')
        remember('pause-keep',paused,ui.shot('paused-result','scan.result.done','##ScanResult'))
        ui.click('scan.result.rescan','##ScanResult')
        retried = ui.wait(lambda s:ui.has(s,'scan.dialog.start') and s['surface']=='setup','再次扫描入口')
        require([path_key(p) for p in retried['pending_folders']]==[path_key(p) for p in folders], '暂停重扫漏了目录')
        ui.cancel()
        require(ui.state()['stop_disposition']==1 and ui.rows(database)==kept,'关闭重扫入口改变暂停保留状态')
        remember('rescan-keeps-both-folders-and-paused-latch',retried)

        revoked = fixtures/'revoke'
        make_wavs(revoked,6000)
        before.update({str(p):sha256(p) for p in revoked.glob('*.wav')})
        ui.setup([revoked]); ui.click('scan.dialog.start')
        ui.wait(lambda s:s['surface']=='progress' and s['can_stop'], '等待第二次扫描')
        ui.click('scan.dialog.stop'); ui.click('scan.dialog.revoke','##ScanStopChoice')
        state = result('cancelled')
        require(ui.rows(database)==kept, '撤销未恢复本次扫描前的库记录')
        require(all(Path(p).is_file() and sha256(Path(p))==digest for p,digest in before.items()),'暂停或撤销修改了原音频')
        remember('revoke-preserves-existing-rows-and-source-audio',state,ui.shot('revoked-result','scan.result.done','##ScanResult'))
        done()

        similar_db = new_library('similar')
        similar = fixtures/'similar'
        make_wavs(similar,12)
        driver.command({'cmd':'scan_dialog_ui','action':'setup','folders':[str(similar)],'fast':False,'similar':True})
        ui.wait(lambda s:ui.has(s,'scan.dialog.start'),'相似分析入口')
        ui.click('scan.dialog.start')
        state = result('completed')
        require(state['succeeded']==12 and len(ui.rows(similar_db))==12,'相似分析前的真实入库数量不符')
        done()
        state = ui.wait(lambda s:ui.has(s,'scan.similar.done') and s['similarity']['available'] and not s['similarity']['running'], '等待真实相似分析', 180)
        require(state['similarity']['total']==12 and state['similarity']['succeeded']==12
                and state['similarity']['failed']==0 and state['similarity']['remaining']==0, '相似分析把失败当成成功或计数不符')
        remember('post-scan-similarity',state,ui.shot('similar-completed','scan.similar.done','##ScanSimilarityResult'))
        ui.click('scan.similar.done','##ScanSimilarityResult')
        ui.wait(lambda s:not ui.has(s,'scan.similar.done'),'关闭相似结果')

        # Resize only this verified private-desktop HWND, never activate it.
        require(win32process.GetWindowThreadProcessId(identity['hwnd'])[1]==pid,'窗口身份改变')
        win32gui.SetWindowPos(identity['hwnd'],0,30,30,390,700,win32con.SWP_NOACTIVATE|win32con.SWP_NOZORDER)
        time.sleep(.4)
        ui.setup([])
        state = ui.state()
        for key in ('scan.dialog.start','scan.dialog.cancel','scan.dialog.close'):
            require(ui.target(state,key) is not None,'窄窗口控件越界 '+key)
        remember('narrow-entry',state,ui.shot('narrow-setup','scan.dialog.start'))
        ui.cancel()
        report['status']='passed'
    except Exception as error:
        report.update(status='failed',error=str(error),traceback=traceback.format_exc(),last_observed=ui.last)
        if verified:
            try: capture(driver,out/'failure-diagnostic.png')
            except Exception as capture_error: report['capture_error']=str(capture_error)
    finally:
        try:
            if win32event.WaitForSingleObject(process,0)==win32con.WAIT_TIMEOUT:
                if verified: driver.command({'cmd':'info_edit_input','action':'exit'},timeout=10)
                if win32event.WaitForSingleObject(process,15000)!=win32con.WAIT_OBJECT_0:
                    win32process.TerminateProcess(process,1)
                    win32event.WaitForSingleObject(process,5000)
                    report.update(status='failed',forced_private_exit=True)
            report['exit_code']=win32process.GetExitCodeProcess(process)
            require(report['exit_code']==0 and sha256(exe)==report['exe_sha256'],'程序退出失败或测试期间 EXE 被替换')
        except Exception as error: report.update(status='failed',exit_error=str(error))
        finally:
            process.Close(); desktop.close()
            report['finished_at']=utc_now()
            save_json(out/'report.json',report)
    print(json.dumps({'status':report['status'],'report':str(out/'report.json')},ensure_ascii=False),flush=True)
    return 0 if report['status']=='passed' else 1


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for option in ('exe','isolation-root','output'): parser.add_argument('--'+option,type=Path,required=True)
    raise SystemExit(run(parser.parse_args()))
