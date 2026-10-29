#pragma once

// Linear undo history. Every user-visible document change is a Command so
// it can be undone, redone, and counted toward the "modified" state.

#include "acestudio/core/Result.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace acestudio {

class Document;

class Command {
public:
    virtual ~Command() = default;
    virtual std::string label() const = 0;
    virtual Status redo(Document& doc) = 0;     // apply (first time and on redo)
    virtual void undo(Document& doc) = 0;       // must exactly restore the prior state
    virtual std::size_t memoryBytes() const { return 0; }
    // Commands with the same non-negative mergeKey may coalesce (e.g. an
    // opacity slider drag becomes one undo step). Return true if `next`
    // was absorbed into this command.
    virtual long long mergeKey() const { return -1; }
    virtual bool mergeWith(const Command& next) { (void)next; return false; }
};

class UndoStack {
public:
    using Listener = std::function<void()>;

    explicit UndoStack(std::size_t memoryBudgetBytes = 512u * 1024u * 1024u, std::size_t maxSteps = 500);

    // Applies the command, then records it. Nothing is recorded on failure.
    Status push(Document& doc, std::unique_ptr<Command> cmd, bool allowMerge = false);
    // Records a command whose effect is already present in the document
    // (used by interactive painting, which edits live).
    void pushApplied(std::unique_ptr<Command> cmd);

    bool canUndo() const noexcept { return m_index > 0; }
    bool canRedo() const noexcept { return m_index < m_commands.size(); }
    Status undo(Document& doc);
    Status redo(Document& doc);
    std::string undoLabel() const;
    std::string redoLabel() const;

    std::size_t size() const noexcept { return m_commands.size(); }
    std::size_t index() const noexcept { return m_index; }
    std::size_t memoryBytes() const noexcept { return m_bytes; }

    // "Clean" = matches what is on disk. Saving calls setClean().
    bool isClean() const noexcept { return m_cleanValid && m_clean == m_index; }
    void setClean() noexcept { m_clean = m_index; m_cleanValid = true; notify(); }
    void clear();

    void setListener(Listener l) { m_listener = std::move(l); }

private:
    void record(std::unique_ptr<Command> cmd, bool allowMerge);
    void trim();
    void notify() { if (m_listener) m_listener(); }

    std::vector<std::unique_ptr<Command>> m_commands;
    std::size_t m_index = 0;            // commands [0, m_index) are applied
    std::size_t m_clean = 0;
    bool m_cleanValid = true;
    std::size_t m_bytes = 0;
    std::size_t m_budget;
    std::size_t m_maxSteps;
    Listener m_listener;
};

} // namespace acestudio
