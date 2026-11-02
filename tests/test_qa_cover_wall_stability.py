"""A failed cover capture must never leave a passing QA report."""
import json
from pathlib import Path
import subprocess
import sys

import pytest
from PIL import Image

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
from qa_cover_wall_stability import audit_pixels


def prepare_capture(tmp_path):
    output, runtime = tmp_path / 'output', tmp_path / 'runtime'
    output.mkdir()
    runtime.mkdir()
    frame = Image.new('RGB', (120, 160))
    for y in range(20, 120):
        for x in range(40, 120):
            frame.putpixel((x, y), (x, y, 64))
    screenshot = output / 'scene.png'
    frame.crop((40, 20, 120, 120)).save(screenshot)
    for i in range(24):
        frame.save(runtime / f'capture-{i}.ppm')
    report = dict(passed=True, error=None, cases=[dict(
        name='scene', screenshot=str(screenshot), trace_key='capture')])
    (output / 'report.json').write_text(json.dumps(report), encoding='utf-8')
    return output, runtime


def test_changed_pixels_mark_main_report_failed(tmp_path):
    output, runtime = prepare_capture(tmp_path)
    changed = runtime / 'capture-10.ppm'
    with Image.open(changed) as source:
        frame = source.copy()
    frame.putpixel((60, 70), (255, 0, 0))
    frame.save(changed)
    with pytest.raises(AssertionError, match='Cover pixels changed'):
        audit_pixels(output, runtime)
    report = json.loads((output / 'report.json').read_text('utf-8'))
    assert report['passed'] is False
    assert 'Cover pixels changed' in report['error']


def test_missing_image_marks_main_report_failed(tmp_path):
    output, runtime = prepare_capture(tmp_path)
    (runtime / 'capture-10.ppm').unlink()
    with pytest.raises(FileNotFoundError):
        audit_pixels(output, runtime)
    report = json.loads((output / 'report.json').read_text('utf-8'))
    assert report['passed'] is False
    assert 'capture-10.ppm' in report['error']


def test_stable_pixels_pass_both_reports(tmp_path):
    output, runtime = prepare_capture(tmp_path)
    audit_pixels(output, runtime)
    for name in ('report.json', 'pixel-audit.json'):
        assert json.loads((output / name).read_text('utf-8'))['passed'] is True


@pytest.mark.parametrize('frames', [0, 10, 23])
def test_short_capture_rejected_before_starting_app(tmp_path, frames):
    output = tmp_path / 'output'
    result = subprocess.run([sys.executable, str(TOOLS / 'qa_cover_wall_stability.py'),
        '--exe', str(tmp_path / 'missing.exe'), '--isolation-root', str(tmp_path / 'profile'),
        '--output', str(output), '--frames', str(frames)], text=True, capture_output=True)
    assert result.returncode == 2
    assert 'at least 24' in result.stderr
    assert not output.exists()
