"""The fixed-width QA sweep must not pass when a visible page control vanishes."""
import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location('info_bounds', Path(__file__).resolve().parents[1] / 'tools/check_info_bounds.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


def report(visible_mask=1):
    return {'cases': [{'name': 'fixture', 'state': {
        'visible_mask': visible_mask,
        'zones': {'info.metadataSummary': {'y': 2}, 'info.pageVisibility': {'y': 2}},
        'layout_windows': [
            {'name': 'Main/RightDockColumn_fixture', 'x': 0, 'y': 0, 'w': 390, 'h': 300, 'scroll_max_y': 0},
            {'name': 'Main/RightDockColumn_fixture/FileInfoContent_fixture', 'x': 8, 'y': 36, 'w': 374, 'h': 252, 'scroll_max_y': 0},
            {'name': 'Main/RightDockColumn_fixture/FileInfoContent_fixture/##ArtworkScroll_fixture', 'x': 8, 'y': 36, 'w': 374, 'h': 224},
        ],
    }}]}


def test_visible_controls_required_but_hidden_pages_are_not():
    assert audit.violations(report()) == []
    missing = report(visible_mask=3)
    assert audit.violations(missing) == ['fixture: info.artwork present']


def test_missing_overflow_menu_is_a_failure():
    missing = report()
    del missing['cases'][0]['state']['zones']['info.pageVisibility']
    assert audit.violations(missing) == ['fixture: info.pageVisibility present']


def test_vertical_info_requires_the_same_bounded_content():
    vertical = report()
    for window in vertical['cases'][0]['state']['layout_windows']:
        window['name'] = window['name'].replace('Main/RightDockColumn_fixture', 'Info##DockedFileInfo')
    assert audit.violations(vertical) == []
    vertical['cases'][0]['state']['layout_windows'][1]['w'] += 8
    assert audit.violations(vertical) == ['fixture: right inset']


def test_missing_bounded_content_fails_instead_of_skipping_the_dock_route():
    missing = report()
    del missing['cases'][0]['state']['layout_windows'][1]
    assert audit.violations(missing) == ['fixture: bounded content present']
