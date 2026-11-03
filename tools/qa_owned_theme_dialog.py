"""Operate only a native file dialog belonging to the isolated QA process."""
import ctypes,threading,time,json
from pathlib import Path

def start_file_dialog_worker(desktop,pid,target,output):
    result={}
    def work():
        import win32gui,win32process,win32con
        try:
            assert desktop.user32.SetThreadDesktop(desktop.handle)
            deadline=time.monotonic()+15
            dialog=None
            while time.monotonic()<deadline:
                matches=[]
                def visit(hwnd,_):
                    if win32process.GetWindowThreadProcessId(hwnd)[1]==pid and win32gui.GetClassName(hwnd)=='#32770' and win32gui.IsWindowVisible(hwnd):matches.append(hwnd)
                    return True
                win32gui.EnumWindows(visit,None)
                if matches:dialog=matches[0];break
                time.sleep(.05)
            assert dialog,'Owned native file dialog did not appear'
            children=[]
            def child(hwnd,_):
                children.append(dict(hwnd=hwnd,id=win32gui.GetDlgCtrlID(hwnd),type=win32gui.GetClassName(hwnd),text=win32gui.GetWindowText(hwnd)))
                return True
            win32gui.EnumChildWindows(dialog,child,None)
            result.update(title=win32gui.GetWindowText(dialog),children=children)
            if target is None:
                win32gui.PostMessage(dialog,win32con.WM_COMMAND,2,0)
            else:
                edits=[c for c in children if c['type']=='Edit' and c['id'] in (1148,1001)]
                assert len(edits)==1,children
                win32gui.SendMessage(edits[0]['hwnd'],win32con.WM_SETTEXT,0,str(Path(target).resolve()))
                win32gui.PostMessage(dialog,win32con.WM_COMMAND,1,0)
            deadline=time.monotonic()+12
            while win32gui.IsWindow(dialog) and time.monotonic()<deadline:time.sleep(.05)
            assert not win32gui.IsWindow(dialog),'Owned file dialog did not close'
            result['passed']=True
        except Exception as e:
            result['error']=repr(e)
            if 'dialog' in locals() and dialog and win32gui.IsWindow(dialog):win32gui.PostMessage(dialog,win32con.WM_COMMAND,2,0)
        finally:
            Path(output).write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    thread=threading.Thread(target=work,daemon=True);thread.start()
    return thread,result
