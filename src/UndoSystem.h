#pragma once

// UndoSystem — Global Undo/Redo Stack (F-025)

//
// Unified undo for all destructive operations:
//   - File rename (single + batch)
//   - Metadata patch (single + batch)
//   - Tag add/remove
//   - Workflow execution (batch metadata)
//   - Folder rename
//
// Design: Command pattern with polymorphic Operation base class.
// Stack depth limited to prevent unbounded memory growth.

// @stage7-contract
// API contract: UndoOperation implementations borrow the caller-owned
// Database only for the synchronous undo/redo call and return false on a
// failed transaction. UndoStack takes ownership of pushed operations,
// serializes history mutation on its owning UI/application thread, and keeps
// at most kMaxDepth entries; callers must not invoke it concurrently.

#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <unordered_map>
#include <cstdint>
#include <functional>

// Forward declarations
class Database;

// Operation Types

enum class UndoOpType {
    FileRename,         // Single or batch file rename
    FolderRename,       // Folder rename (affects all children)
    MetadataPatch,      // DB field update (single or batch)
    TagAdd,             // Tag added to files
    TagRemove,          // Tag removed from files
    WorkflowExec,       // Workflow batch execution
};

// Operation Base Class

class UndoOperation {
public:
    virtual ~UndoOperation() = default;

    // Human-readable description for UI display
    virtual std::string description() const = 0;

    // Operation type
    virtual UndoOpType type() const = 0;

    // Execute undo — returns true on success
    virtual bool undo(Database& db) = 0;

    // Execute redo — returns true on success
    virtual bool redo(Database& db) = 0;

    // Timestamp (ms since epoch)
    int64_t timestamp = 0;
};

// Concrete Operations

// --- File Rename ---
struct RenameEntry {
    int64_t fileId;
    std::string oldPath;
    std::string newPath;
};

class FileRenameOp : public UndoOperation {
public:
    std::vector<RenameEntry> entries;

    std::string description() const override;
    UndoOpType type() const override { return UndoOpType::FileRename; }
    bool undo(Database& db) override;
    bool redo(Database& db) override;
};

// --- Folder Rename ---
class UndoFolderRenameOp : public UndoOperation {
public:
    std::string oldFolderPath;
    std::string newFolderPath;

    std::string description() const override;
    UndoOpType type() const override { return UndoOpType::FolderRename; }
    bool undo(Database& db) override;
    bool redo(Database& db) override;
};

// --- Metadata Patch ---
struct MetadataPatchEntry {
    int64_t fileId;
    std::unordered_map<std::string, std::string> oldValues;
    std::unordered_map<std::string, std::string> newValues;
};

enum class MetadataUndoKind {
    Edit,
    BatchEdit,
    WriteUcsCategory,
};

class MetadataPatchOp : public UndoOperation {
public:
    std::vector<MetadataPatchEntry> entries;
    MetadataUndoKind editKind = MetadataUndoKind::Edit;
    std::string targetColumn;

    std::string description() const override;
    UndoOpType type() const override { return UndoOpType::MetadataPatch; }
    bool undo(Database& db) override;
    bool redo(Database& db) override;
};

// --- Tag Add/Remove ---
class TagOp : public UndoOperation {
public:
    std::vector<int64_t> fileIds;
    std::string tagName;
    bool wasAdd; // true = originally added, false = originally removed

    std::string description() const override;
    UndoOpType type() const override { return wasAdd ? UndoOpType::TagAdd : UndoOpType::TagRemove; }
    bool undo(Database& db) override;
    bool redo(Database& db) override;
};

// --- Workflow Execution ---
class WorkflowExecOp : public UndoOperation {
public:
    std::string workflowName;
    std::vector<MetadataPatchEntry> entries; // reuse same structure

    std::string description() const override;
    UndoOpType type() const override { return UndoOpType::WorkflowExec; }
    bool undo(Database& db) override;
    bool redo(Database& db) override;
};

// Undo Stack

class UndoStack {
public:
    static constexpr int kMaxDepth = 50;

    // Push a new operation (clears redo stack)
    void push(std::unique_ptr<UndoOperation> op);

    // Undo the last operation
    bool undo(Database& db);

    // Redo the last undone operation
    bool redo(Database& db);

    // Query state
    bool canUndo() const { return !m_undoStack.empty(); }
    bool canRedo() const { return !m_redoStack.empty(); }

    // Get descriptions for UI
    std::string undoDescription() const;
    std::string redoDescription() const;

    // Get full history for display
    const std::deque<std::unique_ptr<UndoOperation>>& undoHistory() const { return m_undoStack; }

    // Clear all history
    void clear();

    // Stats
    int undoCount() const { return (int)m_undoStack.size(); }
    int redoCount() const { return (int)m_redoStack.size(); }

private:
    std::deque<std::unique_ptr<UndoOperation>> m_undoStack;
    std::deque<std::unique_ptr<UndoOperation>> m_redoStack;
};
