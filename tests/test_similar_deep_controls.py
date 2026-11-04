from pathlib import Path
root = Path(__file__).resolve().parents[1]
window = (root / 'src/SimilarSoundsWindow.cpp').read_text(encoding='utf-8')
main = (root / 'src/main.cpp').read_text(encoding='utf-8')
app_state = (root / 'src/AppState.h').read_text(encoding='utf-8')
qa = (root / 'src/WindowsMain.cpp').read_text(encoding='utf-8')
assert '##SimilarMode' not in window, 'fixed substitute search must not show a mode switch'
assert 'Similar_ModePerceptual' not in window and 'Similar_ModeSubstitute' not in window, 'do not replace the switch with a mode label'
assert 'similar.options.mode' not in window and 'similarSoundsMode' not in app_state
assert 'searchOptions.mode = HarkenSearch::SimilarityMode::Substitute' in window
assert 'options.mode = HarkenSearch::SimilarityMode::Substitute' in qa
assert '"similarMode"' not in main, 'legacy saved perceptual mode must not override the fixed search'
assert 'Similar_DeepThink' in window
assert 'MakeSimilarityQueryV2Handler' in window
assert 'workflow.RequestSearch(searchOptions, 50)' in window
for key in ('similarDeep', 'similarPreset'):
    assert main.count('"' + key + '"') >= 2, key + ' must load and save'
assert 'ImGui::Text("%d%%", score)' not in window
assert 'row.breakdown.parts' in window and 'row.breakdown.weights' in window
assert 'Similar_ModelUnavailable' in window and 'Similar_IndexRequired' in window
assert 'BuildSimilarityIndex' in main
print('PASS: fixed substitute mode, hidden mode controls, explicit query, persistent depth/preset and honest unavailable states')
