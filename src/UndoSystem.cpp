
// UndoSystem.cpp — Global Undo/Redo Implementation (F-025)

#include "UndoSystem.h"
#include "Database.h"
#include "Localization.h"
#include "platform/filesystem.h"

#include <filesystem>
#include <chrono>

namespace fs = std::filesystem;

// Helpers

static int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Reverse a file rename on disk + DB
static bool ReverseFileRename(Database& db, const std::string& currentPath, const std::string& targetPath) {
    if (!Platform::FileSystem::Exists(currentPath)) return false;

    // Suppress watcher events for our own rename
    db.noteExpectedFileRename(currentPath, targetPath);

    if (!Platform::FileSystem::Rename(currentPath, targetPath)) return false;

    std::string newFileName = fs::u8path(targetPath).filename().u8string();
    db.updateFilePath(currentPath, targetPath, newFileName);
    return true;
}

// Apply a metadata patch (old values) to revert
static bool ApplyMetadataRevert(Database& db, const std::vector<MetadataPatchEntry>& entries, bool useOld) {
    db.beginTransaction();
    for (const auto& e : entries) {
        MetadataEditPatch patch;
        const auto& vals = useOld ? e.oldValues : e.newValues;
        for (const auto& [k, v] : vals) {
            patch.fields[k] = v;
        }
        db.applyMetadataPatch(e.fileId, patch);
    }
    db.commitTransaction();
    return true;
}

// FileRenameOp

std::string FileRenameOp::description() const {
    LedgerText label;
    if (entries.size() == 1) {
        label.setKeyStr(LStr::Undo_RenameFileFmt, fs::u8path(entries[0].oldPath).filename().u8string());
    } else {
        label.setKeyInt(LStr::Undo_BatchRenameFilesFmt, static_cast<int>(entries.size()));
    }
    return label.Render();
}

bool FileRenameOp::undo(Database& db) {
    // Reverse: rename newPath → oldPath
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (!ReverseFileRename(db, it->newPath, it->oldPath)) return false;
    }
    return true;
}

bool FileRenameOp::redo(Database& db) {
    for (const auto& e : entries) {
        if (!ReverseFileRename(db, e.oldPath, e.newPath)) return false;
    }
    return true;
}

// UndoFolderRenameOp

std::string UndoFolderRenameOp::description() const {
    auto fn = fs::u8path(oldFolderPath).filename().u8string();
    return "Rename folder " + fn;
}

bool UndoFolderRenameOp::undo(Database& db) {
    if (!Platform::FileSystem::Exists(newFolderPath)) return false;
    db.noteExpectedFolderRename(newFolderPath, oldFolderPath);
    if (!Platform::FileSystem::Rename(newFolderPath, oldFolderPath)) return false;
    db.updatePathPrefix(newFolderPath, oldFolderPath);
    return true;
}

bool UndoFolderRenameOp::redo(Database& db) {
    if (!Platform::FileSystem::Exists(oldFolderPath)) return false;
    db.noteExpectedFolderRename(oldFolderPath, newFolderPath);
    if (!Platform::FileSystem::Rename(oldFolderPath, newFolderPath)) return false;
    db.updatePathPrefix(oldFolderPath, newFolderPath);
    return true;
}

// MetadataPatchOp

static const char* MetadataUndoColumnLabel(const std::string& column) {
    if (column == "title") return L(Batch_EditTargetTitle);
    if (column == "description") return L(Batch_EditTargetDescription);
    if (column == "keywords") return L(Batch_EditTargetKeywords);
    if (column == "category") return L(Batch_EditTargetCategory);
    if (column == "rating") return L(Col_Rating);
    return nullptr;
}

std::string MetadataPatchOp::description() const {
    if (editKind == MetadataUndoKind::WriteUcsCategory) return L(ModuleUI_UcsWriteTitle);
    const bool batch = editKind == MetadataUndoKind::BatchEdit;
    if (const char* column = MetadataUndoColumnLabel(targetColumn)) {
        LedgerText label;
        label.setKeyStr(batch ? LStr::Undo_BatchEditColumnFmt : LStr::Undo_EditColumnFmt, column);
        return label.Render();
    }
    return batch ? L(Undo_BatchEditMetadata) : L(MetaEdit_DialogTitle);
}

bool MetadataPatchOp::undo(Database& db) {
    return ApplyMetadataRevert(db, entries, true); // apply oldValues
}

bool MetadataPatchOp::redo(Database& db) {
    return ApplyMetadataRevert(db, entries, false); // apply newValues
}

// TagOp

std::string TagOp::description() const {
    std::string action = wasAdd ? "Add tag" : "Remove tag";
    return action + " \"" + tagName + "\" (" + std::to_string(fileIds.size()) + " files)";
}

bool TagOp::undo(Database& db) {
    if (wasAdd) {
        // Was add → undo = remove
        db.removeTagFromFiles(fileIds, tagName);
    } else {
        // Was remove → undo = add back
        db.addTagToFiles(fileIds, tagName);
    }
    return true;
}

bool TagOp::redo(Database& db) {
    if (wasAdd) {
        db.addTagToFiles(fileIds, tagName);
    } else {
        db.removeTagFromFiles(fileIds, tagName);
    }
    return true;
}

// WorkflowExecOp

std::string WorkflowExecOp::description() const {
    return "Workflow \"" + workflowName + "\" (" + std::to_string(entries.size()) + " files)";
}

bool WorkflowExecOp::undo(Database& db) {
    return ApplyMetadataRevert(db, entries, true); // apply oldValues
}

bool WorkflowExecOp::redo(Database& db) {
    return ApplyMetadataRevert(db, entries, false); // apply newValues
}

// UndoStack

void UndoStack::push(std::unique_ptr<UndoOperation> op) {
    if (!op) return;
    op->timestamp = NowMs();
    m_redoStack.clear(); // new action invalidates redo history
    m_undoStack.push_back(std::move(op));
    while ((int)m_undoStack.size() > kMaxDepth) {
        m_undoStack.pop_front();
    }
}

bool UndoStack::undo(Database& db) {
    if (m_undoStack.empty()) return false;
    auto op = std::move(m_undoStack.back());
    m_undoStack.pop_back();
    bool ok = op->undo(db);
    if (ok) {
        m_redoStack.push_back(std::move(op));
    }
    return ok;
}

bool UndoStack::redo(Database& db) {
    if (m_redoStack.empty()) return false;
    auto op = std::move(m_redoStack.back());
    m_redoStack.pop_back();
    bool ok = op->redo(db);
    if (ok) {
        m_undoStack.push_back(std::move(op));
    }
    return ok;
}

std::string UndoStack::undoDescription() const {
    if (m_undoStack.empty()) return "";
    return m_undoStack.back()->description();
}

std::string UndoStack::redoDescription() const {
    if (m_redoStack.empty()) return "";
    return m_redoStack.back()->description();
}

void UndoStack::clear() {
    m_undoStack.clear();
    m_redoStack.clear();
}
