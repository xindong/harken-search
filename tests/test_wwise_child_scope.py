"""A clipped Wwise child still requires EndChild, per the ImGui contract."""
from pathlib import Path


def test_selected_properties_child_closes_even_when_clipped():
    source = (Path(__file__).resolve().parents[1] / 'src/WwiseTransferPanel.cpp').read_text(encoding='utf-8')
    start = source.index('if (ImGui::BeginChild("wwiseSelectedRowPropertiesCard"')
    opening = source.index('{', start)
    depth = 1
    cursor = opening + 1
    while depth:
        depth += (source[cursor] == '{') - (source[cursor] == '}')
        cursor += 1
    visible_body = source[opening:cursor]
    assert 'ImGui::EndChild();' not in visible_body, 'Clipped child must still close its window scope'
    remainder = source[cursor:].lstrip()
    while remainder.startswith('//'):
        remainder = remainder.split('\n', 1)[1].lstrip()
    assert remainder.startswith('ImGui::EndChild();')
