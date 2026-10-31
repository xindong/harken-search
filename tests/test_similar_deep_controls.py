from pathlib import Path
root = Path(__file__).resolve().parents[1]
window = (root / 'src/SimilarSoundsWindow.cpp').read_text(encoding='utf-8')
main = (root / 'src/main.cpp').read_text(encoding='utf-8')
assert '##SimilarMode' in window and 'Similar_DeepThink' in window
assert 'MakeSimilarityQueryV2Handler' in window
assert 'workflow.RequestSearch(searchOptions, 50)' in window
for key in ('similarMode', 'similarDeep', 'similarPreset'):
    assert main.count('"' + key + '"') >= 2, key + ' must load and save'
assert 'ImGui::Text("%d%%", score)' not in window
assert 'row.breakdown.parts' in window and 'row.breakdown.weights' in window
assert 'Similar_ModelUnavailable' in window and 'Similar_IndexRequired' in window
assert 'BuildSimilarityIndex' in main
print('PASS: dual modes, explicit query, persistent choices, score details and honest unavailable states')
