"""Exercise the real QA polling loops with delayed and fast batch receipts."""
import ast
from pathlib import Path

import pytest


SOURCE = Path(__file__).resolve().parents[1] / "tools/qa_similarity_pending.py"


def run_wait(kind, states):
    tree = ast.parse(SOURCE.read_text(encoding="utf-8"))
    message = ("pending analysis did not finish" if kind == "pending"
               else "explicit full recheck did not finish")
    loop = next(node for node in ast.walk(tree) if isinstance(node, ast.While)
                and message in ast.unparse(ast.Module(body=node.orelse, type_ignores=[])))

    class Clock:
        now = 0.0

        def monotonic(self):
            return self.now

        def sleep(self, seconds):
            self.now += seconds

    receipts = iter(states)
    clock = Clock()
    env = dict(time=clock, deadline=1.0, observed=[], generation_before=7,
               before={"files": 164}, state=lambda: next(receipts, states[-1]))
    exec(compile(ast.fix_missing_locations(ast.Module(body=[loop], type_ignores=[])),
                 str(SOURCE), "exec"), env)
    return env["current" if kind == "pending" else "rechecked"]


def receipt(kind, generation, *, running=False):
    total = 1 if kind == "pending" else 164
    return dict(batch_start_generation=generation, running=running,
                batch_total=total, progress=total, reused=0, analyzed=0)


@pytest.mark.parametrize("kind", ["pending", "recheck"])
def test_click_without_a_new_batch_cannot_reuse_previous_completion(kind):
    with pytest.raises(TimeoutError):
        run_wait(kind, [receipt(kind, 7)])


@pytest.mark.parametrize("kind", ["pending", "recheck"])
def test_fast_new_batch_can_finish_between_polls(kind):
    completed = receipt(kind, 8)
    assert run_wait(kind, [completed]) is completed


@pytest.mark.parametrize("kind", ["pending", "recheck"])
def test_waits_past_old_completion_until_the_new_batch_finishes(kind):
    completed = receipt(kind, 8)
    assert run_wait(kind, [receipt(kind, 7), receipt(kind, 8, running=True), completed]) is completed


@pytest.mark.parametrize("kind", ["pending", "recheck"])
def test_a_started_batch_must_finish_before_it_is_accepted(kind):
    with pytest.raises(TimeoutError):
        run_wait(kind, [receipt(kind, 8, running=True)])
