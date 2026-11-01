"""Run real scan pause/revoke/empty/similarity workflows on a private desktop.

Requires the same pinned isolation EXE and dependencies as qa_scan_dialog_simple.
All newly created libraries/audio stay below the pinned root. Existing QA data
is retained. No fabricated app states, shared mouse or installed process access.
"""
from __future__ import annotations
import argparse
import json
import os
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
    report = {'status':'running','started_at':utc_now(),'exe':str(exe),'exe_sha256':sha256(exe),
              'checks':[], 'human_visual_acceptance':'未验收',
              'not_covered':['网络失联、真实磁盘故障、撤销失败和拖入通知尚未做实机故障注入',
                             '未用此脚本证明每个瞬时阶段都已截图']}
    verified = False
    desktop = process = ui = None
    process_exited = True  # No process exists until start() returns our handle.
    lock = runtime / 'qa_scan_dialog_simple.lock'
    lock_contents = json.dumps({'helper_pid':os.getpid(), 'output':str(out), 'owner':uuid.uuid4().hex})
    lock_identity = None
    lock_written = False
    # Same atomic create as smoke QA: an exists() preflight is not ownership.
    # Failure here must never enter the owner's cleanup or remove another lock.
    lock_file = lock.open('x', encoding='utf-8')
    try:
        with lock_file:
            lock_identity = os.fstat(lock_file.fileno())
            lock_file.write(lock_contents)
            lock_file.flush()
            lock_written = True
        driver = Driver(runtime, out)
        desktop = PrivateDesktop()
        process, pid = desktop.start(exe)
        process_exited = False
        ui = Smoke(driver, root, out, 180)
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
            # A new result label can be published one frame before the modal
            # joins the observed active-window list. Wait for its real bounds.
            state = ui.wait(lambda s:s['surface']=='result' and ui.target(s,'scan.result.done','##ScanResult') is not None, '等待结果 '+phase)
            require(state['phase']==phase and not state['is_scanning'] and not state['completion_pending'], '结果类型错误 '+str(state))
            require(ui.target(state,'scan.metrics.elapsed','##ScanResult') is not None, '结果页没有直接显示总用时')
            require(state['metrics']['elapsed_ms'] >= 0, '实际扫描总用时无效')
            time.sleep(.35)
            require(ui.state()['metrics']['elapsed_ms']==state['metrics']['elapsed_ms'], '结果页的总用时仍在增长')
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
        measured = ui.wait(lambda s:s['surface']=='progress' and s['phase']=='ingesting'
                           and s.get('metrics',{}).get('speed_known')
                           and s['metrics']['files_per_second']>0, '等待真实速度采样', 25)
        require(ui.target(measured,'scan.metrics.speed') is not None and ui.target(measured,'scan.metrics.elapsed') is not None,
                '进度页速度或总用时不可见')
        require(ui.target(measured,'scan.dialog.file') is not None, '入库阶段丢失当前文件行')
        time.sleep(.3)
        require(ui.state()['metrics']['elapsed_ms']>measured['metrics']['elapsed_ms'],'扫描用时没有随处理增长')
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

        # Reproduce the user-reported boundary: ingestion has finished, but
        # required mini waveforms have not. Never show its stale ScanDone text
        # as a waveform filename. Keep real metrics and completion gates.
        waveform_db = new_library('waveform-label')
        waveforms = fixtures/'waveform-label'
        make_wavs(waveforms,7093,seconds=4)
        driver.command({'cmd':'scan_dialog_ui','action':'theme','theme':10})
        ui.setup([waveforms]); ui.click('scan.dialog.start')
        state = ui.wait(lambda s:s['surface']=='progress' and s['phase']=='waveforms'
                        and s['completion_pending'] and not s['is_scanning'],
                        '等待入库结束后的真实波形阶段')
        require(state['progress']['known'] and 0 <= state['progress']['current'] < state['progress']['total'],
                '波形进度不是未完成的实际计数')
        for key in ('scan.metrics.speed','scan.metrics.elapsed'):
            require(ui.target(state,key) is not None, '波形阶段指标丢失 '+key)
        require(not ui.has(state,'scan.dialog.file'), '波形阶段仍显示入库文件或上一阶段的处理完成')
        save_json(out/'waveform-scan-status.json',driver.command({'cmd':'scan_status'}))
        shot = ui.shot('waveform-progress','scan.dialog.stop')
        after = ui.state()
        require(after['phase']=='waveforms' and after['completion_pending']
                and not ui.has(after,'scan.dialog.file'), '截图期间已离开待完成的波形阶段')
        remember('waveforms-hide-stale-ingest-text-keep-metrics',state,shot)
        state = result('completed')
        status = driver.command({'cmd':'scan_status'})
        require(state['succeeded']==7093 and state['failed']==0
                and ui.rows(waveform_db)=={path_key(p) for p in waveforms.glob('*.wav')}
                and status['mini_waveforms_completed']==7093 and status['mini_waveforms_total']==7093
                and not status['scan_completion_waveforms_pending']
                and status['scan_completion_mini_unresolved']==0, '波形尚未真正完成或入库记录不符')
        remember('waveforms-reach-real-completion',state,ui.shot('waveform-completed','scan.result.done','##ScanResult'))
        done()
        driver.command({'cmd':'scan_dialog_ui','action':'theme','theme':0})

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
        ui.setup(folders); ui.click('scan.dialog.start')
        state = ui.wait(lambda s:s['surface']=='progress' and s.get('metrics',{}).get('speed_known'), '窄窗口真实速度')
        for key in ('scan.metrics.speed','scan.metrics.elapsed','scan.dialog.stop'):
            require(ui.target(state,key) is not None,'窄窗口指标或按钮被裁切 '+key)
        remember('narrow-progress-metrics',state,ui.shot('narrow-progress','scan.dialog.stop'))
        ui.click('scan.dialog.stop'); ui.click('scan.dialog.pause','##ScanStopChoice')
        result('paused'); done()
        report['status']='passed'
    except BaseException as error:
        report.update(status='failed',error=str(error),traceback=traceback.format_exc())
        if ui is not None: report['last_observed'] = ui.last
        if verified:
            try: capture(driver,out/'failure-diagnostic.png')
            except Exception as capture_error: report['capture_error']=str(capture_error)
        if not isinstance(error, Exception):
            raise  # Keep Ctrl+C/SystemExit semantics; finally still owns cleanup.
    finally:
        try:
            if process is not None:
                wait_result = win32event.WaitForSingleObject(process,0)
                if wait_result == win32con.WAIT_TIMEOUT:
                    if verified:
                        try: driver.command({'cmd':'info_edit_input','action':'exit'},timeout=10)
                        except Exception as error: report['exit_command_error'] = str(error)
                    wait_result = win32event.WaitForSingleObject(process,15000)
                    if wait_result == win32con.WAIT_TIMEOUT:
                        # Only the handle returned by our private-desktop launch.
                        win32process.TerminateProcess(process,1)
                        report.update(status='failed',forced_private_exit=True)
                        wait_result = win32event.WaitForSingleObject(process,5000)
                require(wait_result == win32con.WAIT_OBJECT_0, '私有 QA 进程未确认退出，保留锁和隔离现场')
                process_exited = True
                report['exit_code']=win32process.GetExitCodeProcess(process)
                require(report['exit_code']==0, '程序退出失败')
            require(sha256(exe)==report['exe_sha256'],'测试期间 EXE 被替换')
        except Exception as error: report.update(status='failed',exit_error=str(error))
        finally:
            # A handle/desktop cleanup failure must not skip the other cleanup,
            # nor confuse "handle closed" with "process exit confirmed".
            if process is not None:
                try: process.Close()
                except Exception as error: report.update(status='failed',handle_close_error=str(error))
            if desktop is not None:
                try: desktop.close()
                except Exception as error: report.update(status='failed',desktop_close_error=str(error))
            try:
                if process_exited:
                    require(lock_identity is not None
                            and os.path.samestat(lock_identity, lock.stat(follow_symlinks=False))
                            and (not lock_written or lock.read_text(encoding='utf-8') == lock_contents),
                            '锁的归属已变化，保留锁，不删除其他运行的锁')
                    lock.unlink()
                else:
                    report['lock_retained'] = str(lock)
            except Exception as error: report.update(status='failed',lock_cleanup_error=str(error))
            report['finished_at']=utc_now()
            save_json(out/'report.json',report)
    print(json.dumps({'status':report['status'],'report':str(out/'report.json')},ensure_ascii=False),flush=True)
    return 0 if report['status']=='passed' else 1


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for option in ('exe','isolation-root','output'): parser.add_argument('--'+option,type=Path,required=True)
    raise SystemExit(run(parser.parse_args()))
