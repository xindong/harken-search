from pathlib import Path
import ast
import pytest

SCRIPT = Path(__file__).resolve().parents[1] / 'tools' / 'qa_similarity_single_action.py'

def test_semantic_coverage_accepts_model_presence_and_absence():
    tree = ast.parse(SCRIPT.read_text(encoding='utf-8'))
    funcs = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'check_semantic_coverage']
    assert funcs, 'QA should allow valid coverage with or without a model'
    scope = {}
    exec(compile(ast.Module(body=funcs, type_ignores=[]), str(SCRIPT), 'exec'), scope)
    check = scope['check_semantic_coverage']
    for count in (0, 1, 2):
        check({'indexed': 2, 'semantic_indexed': count})
    with pytest.raises(AssertionError):
        check({'indexed': 2, 'semantic_indexed': 3})
