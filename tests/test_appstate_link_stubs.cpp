#include "../src/AppState.h"

FileBrowser::FileBrowser() = default;
FileBrowser::~FileBrowser() = default;
void FileBrowser::setScope(BrowserScope scope) { m_scope = scope; }

#ifndef AURO_TEST_REAL_ASYNC_WORKER
WaveformWorker::WaveformWorker() = default;
WaveformWorker::~WaveformWorker() = default;

SearchWorker::SearchWorker() = default;
SearchWorker::~SearchWorker() = default;
#endif

#ifndef AURO_TEST_REAL_SIMILARITY_WORKFLOW
HarkenSearch::SimilarityWorkflow::~SimilarityWorkflow() = default;
#endif

void MusicAnalyzer::stop() {}
void GaplessEngine::shutdown() {}
