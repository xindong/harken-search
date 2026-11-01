import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from qa_scan_dialog_simple import Smoke


def test_result_probe_waits_for_the_visible_window_and_accepts_exact_zero_duration():
    source = (Path(__file__).resolve().parents[1] / 'tools/qa_scan_dialog_outcomes.py').read_text(encoding='utf-8')
    result_probe = source.split('def result(phase):',1)[1].split('def done():',1)[0]
    assert "ui.target(s,'scan.result.done','##ScanResult') is not None" in result_probe
    assert "state['metrics']['elapsed_ms'] >= 0" in result_probe


def test_click_tracks_the_same_control_after_a_scan_phase_changes(monkeypatch):
    monkeypatch.setattr('qa_scan_dialog_simple.time.sleep', lambda _: None)
    class Driver:
        left = 0
        phases = []
        def command(self, request):
            assert request['x'] == self.left + 50, 'stale button position'
            self.phases.append(request['phase'])
            if request['phase'] == 'hover': self.left = 200
            return {'ok':True}
    driver = Driver()
    class MovingScan(Smoke):
        frame = 0
        def state(self):
            self.frame += 1
            return {'frame':self.frame, 'items':[{'id':'scan.dialog.stop', 'window':'scan',
                'bounds':[driver.left,0,driver.left+100,32]}],
                'windows':[{'name':'scan','bounds':[0,0,400,200]}]}
    MovingScan(driver, Path('.'), Path('.'), 1).click('scan.dialog.stop')
    assert driver.phases == ['hover','down','up']
