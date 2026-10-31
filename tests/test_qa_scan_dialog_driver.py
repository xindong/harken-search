import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from qa_scan_dialog_simple import Smoke


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
