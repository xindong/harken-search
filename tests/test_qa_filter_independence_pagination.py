"""The native QA result reader must compare against every loaded result."""

from pathlib import Path
import sys


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import qa_filter_independence


def test_reads_all_pages_when_cover_has_more_than_one_page():
    expected = [{"path": f"sample-{index}.wav"} for index in range(135)]
    requested_offsets = []

    class Driver:
        def command(self, command):
            assert command["cmd"] == "get_results"
            offset = command["offset"]
            requested_offsets.append(offset)
            return {"total": len(expected),
                    "items": expected[offset:offset + min(command["limit"], 100)]}

    assert qa_filter_independence.all_results(Driver()) == expected
    assert requested_offsets == [0, 100]
