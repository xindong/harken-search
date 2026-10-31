"""A real-window audit must not pass on unrelated or incomplete observations."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import qa_shared_buttons as qa


def button(label, window='设置', **overrides):
    return dict(label=label, window=window, bounds=[1, 1, 21, 21],
                clip=[0, 0, 100, 100], fg=[255, 255, 255, 255],
                fill=[20, 20, 20, 255], disabled=False, **overrides)


def validate(module, checks):
    validator = getattr(qa, 'validate_observations', None)
    assert callable(validator), 'The audit must validate target-window and required-button coverage'
    return validator(module, checks)


SETTINGS = ['清除', '保存并关闭', '取消', '恢复默认主题', '导出设置...', '导入设置...', '恢复全部默认']


def test_unrelated_window_does_not_satisfy_target_coverage():
    checks, failures = validate('settings', [button(label, '主界面') for label in SETTINGS])
    assert not checks
    assert any(f['kind'] == 'missing-buttons' for f in failures)


def test_missing_fully_clipped_button_is_a_failure():
    checks, failures = validate('settings', [button(label) for label in SETTINGS if label != '取消'])
    assert any(f['kind'] == 'missing-buttons' and '取消' in f['labels'] for f in failures)


def test_target_and_child_buttons_are_checked_without_unrelated_controls():
    source = [button(label, '设置/Child_0123') for label in SETTINGS]
    checks, failures = validate('settings', source + [button('Elsewhere', '设置旁边')])
    assert len(checks) == len(SETTINGS)
    assert not failures


def test_partial_clipping_and_low_contrast_remain_failures():
    source = [button(label) for label in SETTINGS]
    source[0]['bounds'] = [-4, 1, 21, 21]
    source[1]['fg'] = source[1]['fill']
    _, failures = validate('settings', source)
    assert {f['kind'] for f in failures} == {'clipped-button', 'contrast'}


def test_missing_wwise_folder_and_download_actions_fail_coverage():
    labels = ('连接', '', '另存为...', '重命名', '删除', '添加父层', '添加子层', '传输到 Wwise', '从 Wwise 刷新')
    _, failures = validate('wwise', [button(label, '传输到 Wwise (F-034)') for label in labels])
    assert any(f['kind'] == 'missing-buttons' and {'', ''} <= set(f['labels'])
               for f in failures)


def test_every_supported_window_has_an_explicit_button_contract():
    contracts = getattr(qa, 'WINDOW_BUTTONS', {})
    assert set(contracts) == {'settings', 'library', 'batch-rename', 'transfer', 'workflow',
                              'theme-editor', 'update', 'similar', 'wwise'}
    for module, (window, required) in contracts.items():
        checks, failures = validate(module, [button(label, window + '/Child_123') for label in required])
        assert checks and not failures, module
        _, failures = validate(module, [])
        assert any(f['kind'] == 'missing-buttons' for f in failures), module


def test_thesaurus_edit_selection_is_passed_to_shared_button():
    source = (Path(__file__).resolve().parents[1] / 'src/main.cpp').read_text(encoding='utf-8')
    block = source[source.index('// Highlight the row currently being edited.'): ]
    block = block[:block.index('ImGui::SameLine();')]
    assert 'isEditingThisRow ? aura::ui::ButtonVariant::Selected' in block
    assert 'ImGui::PushStyleColor' not in block, 'Outer button color is overwritten by the common paint scope'
