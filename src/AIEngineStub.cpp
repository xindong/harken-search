// AIEngineStub — no-op implementation when AURA_ENABLE_AI=OFF
// Provides all symbols from AIEngine.h so callers compile without changes.

#include "AIEngine.h"

AIEngine::AIEngine() {}
AIEngine::~AIEngine() { Shutdown(); }

bool AIEngine::Initialize(const AIEngineConfig&) { return false; }
void AIEngine::Shutdown() {}

bool AIEngine::LoadModel(const std::string&) { return false; }
void AIEngine::UnloadModel() {}
std::string AIEngine::GetModelPath() const { return {}; }

std::string AIEngine::GetModelUrl(const std::string&) { return {}; }
int64_t AIEngine::GetModelSize(const std::string&) { return 0; }
bool AIEngine::DownloadModel(const std::string&, const std::string&, std::function<void(float)>) { return false; }

TranscriptionResult AIEngine::Transcribe(const std::string&, const std::string&,
                                       const std::string&, bool) {
    return { false, {}, {}, {}, "AI engine disabled" };
}

int AIEngine::TranscribeAsync(const std::string&, const std::string&, AITaskCallback,
                              const std::string&, bool) { return -1; }
int AIEngine::EnhanceAudio(const std::string&, const std::string&, AITaskCallback) { return -1; }
int AIEngine::ConvertToStereo(const std::string&, const std::string&, AITaskCallback) { return -1; }
int AIEngine::NormalizeAudio(const std::string&, const std::string&, float, AITaskCallback) { return -1; }
std::vector<int> AIEngine::BatchTranscribe(const std::vector<std::string>&, const std::string&,
                                        AITaskCallback, const std::string&, bool) { return {}; }
int AIEngine::GenerateCoverAsync(const std::string&, const std::string&, AITaskCallback) { return -1; }

AITask AIEngine::GetTask(int) const { return {}; }
std::vector<AITask> AIEngine::GetAllTasks() const { return {}; }
std::vector<AITask> AIEngine::GetPendingTasks() const { return {}; }
bool AIEngine::CancelTask(int) { return false; }
void AIEngine::CancelAllTasks() {}
void AIEngine::ClearCompletedTasks() {}
void AIEngine::SetProgressCallback(AIProgressCallback) {}

int AIEngine::CreateTask(AITaskType, const std::string&) { return -1; }
int AIEngine::CreateCoverTaskWithoutQueue(const std::string&) { return -1; }
 void AIEngine::UpdateTaskStatus(int, AITaskStatus, const std::string&) {}
void AIEngine::UpdateTaskProgress(int, float, const std::string&) {}
void AIEngine::UpdateTaskProgress(int, float, LStr) {}
void AIEngine::ProcessTaskQueue() {}
void AIEngine::ExecuteTask(AITask&) {}
TranscriptionResult AIEngine::TranscribeFromPCM(const std::vector<float>&, const std::string&,
                                              const std::string&, bool) {
    return { false, {}, {}, {}, "AI engine disabled" };
}
bool AIEngine::LoadAudioFile(const std::string&, std::vector<float>&) { return false; }
bool AIEngine::SaveAudioFile(const std::string&, const std::vector<float>&, int, int) { return false; }
void AIEngine::ApplyNoiseReduction(std::vector<float>&, int) {}
void AIEngine::ConvertMonoToStereo(const std::vector<float>&, std::vector<float>&) {}
void AIEngine::NormalizePCM(std::vector<float>&, float) {}
std::string AIEngine::GenerateOutputPath(const std::string&, const std::string&) { return {}; }

AIEngine& GetAIEngine() {
    static AIEngine instance;
    return instance;
}

const char* GetAITaskTypeName(AITaskType) { return "Disabled"; }
const char* GetAITaskStatusName(AITaskStatus) { return "Disabled"; }
// IsAudioFileSupported is defined in Utils.cpp (shared with the real AIEngine build).
std::string GetDefaultModelDir() { return "models"; }
