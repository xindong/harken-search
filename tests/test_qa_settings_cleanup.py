"""A mismatched runtime queue must never receive UI or shutdown commands."""
from pathlib import Path
import json
import sys
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import qa_settings_redesign as qa


@pytest.mark.parametrize('mismatch', ['pid', 'exe', 'root', 'unavailable'])
def test_unverified_queue_is_not_used_for_capture_or_exit(tmp_path, monkeypatch, mismatch):
    exe = tmp_path/'preview.exe'
    exe.write_bytes(b'not an executable')
    args = SimpleNamespace(exe=exe,isolation_root=tmp_path/'isolation',output=tmp_path/'report')
    process = SimpleNamespace(terminated=False,closed=False)
    process.Close = lambda: setattr(process,'closed',True)
    desktop = SimpleNamespace(start=lambda path:(process,123),closed=False)
    desktop.close = lambda: setattr(desktop,'closed',True)
    calls=[]

    class Driver:
        def __init__(self,*args): pass
        def command(self,payload,**kwargs):
            calls.append(payload)
            if payload['cmd']=='magic_eq_identity':
                if mismatch=='unavailable': raise RuntimeError('identity unavailable')
                return {'pid':456 if mismatch=='pid' else 123,
                    'exe_path':str(tmp_path/'other.exe' if mismatch=='exe' else exe),
                    'isolation_root':str(tmp_path/'other-root' if mismatch=='root' else args.isolation_root)}
            return {}

    monkeypatch.setattr(qa,'Driver',Driver)
    monkeypatch.setattr(qa,'PrivateDesktop',lambda:desktop)
    monkeypatch.setattr(qa,'capture',lambda *args:pytest.fail('unverified screenshot'))
    monkeypatch.setitem(sys.modules,'win32con',SimpleNamespace(WAIT_OBJECT_0=0,WAIT_TIMEOUT=258))
    monkeypatch.setitem(sys.modules,'win32event',SimpleNamespace(WaitForSingleObject=lambda handle,timeout:0 if handle.terminated else 258))
    monkeypatch.setitem(sys.modules,'win32process',SimpleNamespace(TerminateProcess=lambda handle,code:setattr(handle,'terminated',True),GetExitCodeProcess=lambda handle:1))
    monkeypatch.setitem(sys.modules,'win32gui',SimpleNamespace())
    with pytest.raises((AssertionError,RuntimeError)):
        qa.run(args)
    assert calls == [{'cmd':'magic_eq_identity'}]
    assert process.terminated and process.closed and desktop.closed
    report=json.loads((args.output/'report.json').read_text(encoding='utf-8'))
    assert report['error'] and report['forced_termination']
