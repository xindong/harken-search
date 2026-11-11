#include "SimilarityIndexCountState.h"

#include <cassert>

int main() {
    SimilarityIndexCountState count;
    assert(count.phase() == SimilarityIndexCountState::Phase::Loading);
    assert(!count.CanStart(false, true));
    count.Complete(false, 0, 0, 0, 0);
    assert(count.phase() == SimilarityIndexCountState::Phase::Failed);
    assert(!count.CanStart(false, true));
    count.Retry();
    assert(count.phase() == SimilarityIndexCountState::Phase::Loading);
    count.Complete(true, 0, 0, 0, 0);
    assert(count.phase() == SimilarityIndexCountState::Phase::Ready);
    assert(!count.CanStart(false, true));
    count.Complete(true, 2, 0, 100, 98);
    assert(count.CanStart(false, true));
    assert(!count.CanStart(true, true));
    count.Complete(true, 2, 0, 3, 1);
    assert(count.pending() == 1);
    count.Reset();
    assert(count.phase() == SimilarityIndexCountState::Phase::Loading);
    assert(!count.CanStart(false, true));
}
