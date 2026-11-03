"""Run real similarity interruption, folder rescan, and resumption in a pinned QA EXE.
Reads original media; owns only the isolated package/profile specified in the receipt.
Requires pywin32 and Pillow. The user desktop is never switched or controlled.
"""
from pathlib import Path
import argparse, sys, json, sqlite3, hashlib, time, traceback
from datetime import datetime, timezone
sys.path.insert(0,str(Path(__file__).resolve().parent))
from qa_required_library_setup import PrivateDesktop,capture,wait_state
from qa_transport_themes import Driver
import win32event,win32process,win32con,win32gui
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("--package-receipt",type=Path,required=True)
parser.add_argument("--source",type=Path,required=True)
parser.add_argument("--output",type=Path,required=True)
parser.add_argument("--interrupt-after",type=int,default=32)
parser.add_argument("--timeout-hours",type=float,default=48)
args=parser.parse_args()
assert args.interrupt_after>0 and args.source.is_dir()
package=json.loads(args.package_receipt.read_text(encoding="utf-8"))
exe=Path(package["exe"]);root=Path(package["root"]);db=Path(package["database"])
assert db.resolve().is_relative_to(root.resolve()), "Database must belong to the pinned isolation profile"
assert hashlib.sha256(exe.read_bytes()).hexdigest()==package["sha256"], "QA EXE differs from its receipt"
assert Path(package["source"]).resolve()==args.source.resolve(), "Source differs from its package receipt"
out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
driver=Driver(root/"profile/runtime",out)
report={"package":package,"started_at":datetime.now(timezone.utc).isoformat(),"phase":"starting","checks":[],"passed":False}
desktop=None;process=None;identity_verified=False
def save():
    report["updated_at"]=datetime.now(timezone.utc).isoformat()
    temp=out/"report.next.json";temp.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding="utf-8");temp.replace(out/"report.json")
def emit(phase,data=None):
    report["phase"]=phase
    if data is not None: report["latest"]=data
    save();print(json.dumps({"phase":phase,"data":data},ensure_ascii=False),flush=True)
def cmd(**kwargs):return driver.command(kwargs,timeout=60)
def until(probe,predicate,label,timeout=args.timeout_hours*3600,interval=2):
    start=time.monotonic();last_print=0
    while time.monotonic()-start<timeout:
        s=probe()
        if time.monotonic()-last_print>10:
            emit(label,s);last_print=time.monotonic()
        if predicate(s):return s
        time.sleep(interval)
    raise TimeoutError(label)
def snapshot(label):
    with sqlite3.connect(db.as_uri()+"?mode=ro",uri=True,timeout=10) as c:
        c.execute("pragma query_only=on");c.execute("begin")
        tables={r[0] for r in c.execute("select name from sqlite_master where type='table'")}
        result={"files":c.execute("select count(*) from audio_files").fetchone()[0],"rows":{}}
        if "harken_similarity_v2" in tables:
            result["revision"]=c.execute("select revision from harken_similarity_revision where singleton=1").fetchone()[0]
            for row in c.execute("select file_id,file_path,file_bytes,modified,fingerprint,version,summary,acoustic,embedding,model_version from harken_similarity_v2 order by file_id"):
                h=hashlib.sha256()
                for value in row:
                    b=value if isinstance(value,bytes) else str(value).encode("utf-8");h.update(len(b).to_bytes(8,"little"));h.update(b)
                result["rows"][str(row[0])]=h.hexdigest()
        result["indexed"]=len(result["rows"])
    (out/(label+"-db.json")).write_text(json.dumps(result,indent=2),encoding="utf-8")
    return result
def scan(label):
    start=time.monotonic()
    cmd(cmd="scan_folder",path=str(args.source.resolve()),recursive=True,update_existing=True,remove_missing=False,cache_waveforms=False,build_similarity_index=False,worker_threads=2)
    s=until(lambda:cmd(cmd="scan_status"),lambda s:not s["scanning"] and not s["scan_completion_pending"],label)
    s["wall_seconds"]=time.monotonic()-start;report[label]=s;save()
    capture(driver,out/(label+".png"))
    cmd(cmd="scan_acknowledge_complete")
    # Acknowledging completion does not dismiss the scan modal. Refresh its
    # actual button zone before each pointer event, as with manual operation.
    until(lambda:cmd(cmd="info_page_state"),
          lambda s:"info.qaScanCancel" in s["zones"],
          label+"-close-button",30,.1)
    for phase in ("hover","down","up"):
        cmd(cmd="info_page_state")
        cmd(cmd="info_page_pointer",key="info.qaScanCancel",phase=phase);time.sleep(.15)
    until(lambda:cmd(cmd="info_page_state"),
          lambda s:"info.qaScanCancel" not in s["zones"],
          label+"-modal-dismissed",30,.1)
    return s
def state():return cmd(cmd="similar_index",action="state")
try:
    desktop=PrivateDesktop();process,pid=desktop.start(exe)
    identity=cmd(cmd="magic_eq_identity");report["identity"]=identity
    assert identity["pid"]==pid and Path(identity["exe_path"]).resolve()==exe.resolve() and Path(identity["isolation_root"]).resolve()==root.resolve(),identity
    identity_verified=True
    wait_state(driver,lambda s:s["main_ui_ready"] and not s["db_switch_in_progress"],"ready",60)
    win32gui.SetWindowPos(identity["hwnd"],0,40,40,1480,940,win32con.SWP_NOACTIVATE|win32con.SWP_NOZORDER)
    current=cmd(cmd="get_state")
    if not current["db_path"]: cmd(cmd="info_edit_input",action="open_existing_db",text=str(db))
    wait_state(driver,lambda s:bool(s["db_path"]) and Path(s["db_path"]).resolve()==db.resolve() and not s["db_switch_in_progress"],"isolated-db",60)
    emit("initial-scan-started",{"pid":pid,"source":str(args.source.resolve())})
    scan("initial_scan")
    initial=snapshot("initial");assert initial["files"]>args.interrupt_after,initial
    cmd(cmd="similar_index",action="open",floating=True,width=900,height=620,language=0)
    cmd(cmd="search",text="")
    wait_state(driver,lambda s:s["result_count"]>0 and not s["db_switch_in_progress"],"populated-reference-list",120)
    cmd(cmd="select_row",row=0)
    cmd(cmd="similar_index",action="start")
    first=until(state,lambda s:s.get("analyzed",0)>=args.interrupt_after or not s["running"],"first_analysis")
    assert first["running"],"Analysis finished before interrupt threshold"
    capture(driver,out/"before-interrupt.png")
    cmd(cmd="similar_index",action="cancel")
    cancelled=until(state,lambda s:not s["running"],"cancelling",180,1)
    report["cancelled"]=cancelled;save()
    before=snapshot("after-interrupt")
    assert before["indexed"]>0 and cancelled["batch_summary"]["cancelled"] and not cancelled["completion_notice"],cancelled
    report["checks"].append({"case":"cancel-preserves-completed-records","indexed":before["indexed"],"passed":True});save()
    capture(driver,out/"after-interrupt.png")
    scan("rescan")
    after=snapshot("after-rescan")
    unchanged=all(after["rows"].get(k)==v for k,v in before["rows"].items())
    report["checks"].append({"case":"rescan-preserves-analysis-records","before":before["indexed"],"after":after["indexed"],"unchanged":unchanged,"passed":unchanged});save()
    assert unchanged,"Rescan invalidated completed similarity records"
    coverage=state()
    expected_pending=coverage["total"]-min(coverage["indexed"],coverage["semantic_indexed"])
    cmd(cmd="similar_index",action="start")
    resumed=until(state,lambda s:s.get("batch_total",0)>0 or not s["running"],"resumed_analysis",interval=1)
    assert resumed["batch_total"]==expected_pending and resumed["reused"]==0,resumed
    report["resume_checkpoint"]=resumed
    resumed_db=snapshot("resume-checkpoint")
    assert all(resumed_db["rows"].get(k)==v for k,v in before["rows"].items()),"Resumed batch rewrote previously completed records"
    report["checks"].append({"case":"resume-excludes-previously-completed-analysis","pending":expected_pending,"previously_completed":before["indexed"],"newly_analyzed":resumed["analyzed"],"passed":True});save()
    capture(driver,out/"resumed-analysis.png")
    emit("resume-verified-continuing-full-analysis",report["checks"][-1])
    final=until(state,lambda s:not s["running"],"full_analysis",interval=3)
    report["final_index"]=final
    capture(driver,out/"final-analysis.png")
    final_db=snapshot("final")
    assert all(final_db["rows"].get(k)==v for k,v in before["rows"].items()),"Completed batch rewrote previously completed records"
    report["checks"].append({"case":"completed-batch-preserves-reused-records",
        "preserved":before["indexed"],"passed":True});save()
    assert not final["batch_summary"]["cancelled"] and not final["batch_summary"]["error"] and not final["batch_summary"]["model_error"],final
    report["full_coverage"]=final["indexed"]==final["total"] and final["semantic_indexed"]==final["total"]
    if final["batch_summary"]["failed"]:report["uncovered"]="Some source files failed analysis; see batch_summary and command log."
    for mode in (0,1):
        for deep in (False,True):
            cmd(cmd="similar_index",action="configure",mode=mode,deep=deep)
            cmd(cmd="similar_index",action="find")
            found=until(state,lambda s:s["status"] in (3,4),"query",300,1)
            report.setdefault("queries",[]).append({"mode":mode,"deep":deep,"state":found});save()
            capture(driver,out/("query-"+str(mode)+"-"+str(int(deep))+".png"))
            assert ((found["status"]==3 and found["error"]==0) or
                    (found["status"]==4 and found["error"] in (3,8))),found
            report["checks"].append({"case":"query-completes-with-results-or-no-candidates",
                "mode":mode,"deep":deep,"rows":found["rows"],"error":found["error"],"passed":True});save()
    report["passed"]=all(c["passed"] for c in report["checks"])
    emit("complete",{"passed":report["passed"],"full_coverage":report["full_coverage"]})
except BaseException as e:
    report["error"]=repr(e);report["traceback"]=traceback.format_exc();emit("failed",{"error":repr(e)});raise
finally:
    if process is not None:
        if win32event.WaitForSingleObject(process,0)==win32con.WAIT_TIMEOUT:
            if identity_verified:
                try:cmd(cmd="similar_index",action="cancel");cmd(cmd="info_edit_input",action="exit")
                except Exception:win32gui.PostMessage(report["identity"]["hwnd"],win32con.WM_CLOSE,0,0)
            else:
                # Never send packet commands to an unverified process. Close
                # only windows belonging to the process this run just created.
                def close_own_window(hwnd,_):
                    if win32process.GetWindowThreadProcessId(hwnd)[1]==pid:
                        win32gui.PostMessage(hwnd,win32con.WM_CLOSE,0,0)
                win32gui.EnumWindows(close_own_window,None)
        if win32event.WaitForSingleObject(process,30000)==win32con.WAIT_OBJECT_0:
            report["exit_code"]=win32process.GetExitCodeProcess(process);process.Close()
        else:report["exit_error"]="QA process did not exit within 30 seconds"
    if desktop:desktop.close()
    save()
