#include "acestudio/core/UndoStack.h"
#include "acestudio/core/Document.h"

namespace acestudio {

UndoStack::UndoStack(std::size_t memoryBudgetBytes, std::size_t maxSteps)
    : m_budget(memoryBudgetBytes), m_maxSteps(maxSteps == 0 ? 1 : maxSteps) {}

Status UndoStack::push(Document& doc, std::unique_ptr<Command> cmd, bool allowMerge) {
    if (!cmd) return Error{ErrorCode::InvalidArgument, "Null command"};
    if (auto st = cmd->redo(doc); !st) return st;
    record(std::move(cmd), allowMerge);
    return {};
}

void UndoStack::pushApplied(std::unique_ptr<Command> cmd) {
    if (cmd) record(std::move(cmd), false);
}

void UndoStack::record(std::unique_ptr<Command> cmd, bool allowMerge) {
    // Recording after an undo discards the redo branch.
    while (m_commands.size() > m_index) {
        m_bytes -= m_commands.back()->memoryBytes();
        m_commands.pop_back();
    }
    if (m_cleanValid && m_clean > m_index) m_cleanValid = false;   // saved state is unreachable now

    // Merge only into the newest command, and never across the clean point
    // (otherwise undo could not return exactly to the saved state).
    if (allowMerge && m_index > 0 && !(m_cleanValid && m_clean == m_index)) {
        Command& top = *m_commands.back();
        if (top.mergeKey() >= 0 && top.mergeKey() == cmd->mergeKey()) {
            const std::size_t before = top.memoryBytes();
            if (top.mergeWith(*cmd)) {
                m_bytes = m_bytes - before + top.memoryBytes();
                notify();
                return;
            }
        }
    }
    m_bytes += cmd->memoryBytes();
    m_commands.push_back(std::move(cmd));
    m_index = m_commands.size();
    trim();
    notify();
}

void UndoStack::trim() {
    // Drop the oldest steps beyond the limits, always keeping the newest.
    std::size_t drop = 0;
    std::size_t bytes = m_bytes;
    while (m_commands.size() - drop > 1 &&
           (m_commands.size() - drop > m_maxSteps || bytes > m_budget)) {
        bytes -= m_commands[drop]->memoryBytes();
        ++drop;
    }
    if (drop == 0) return;
    m_commands.erase(m_commands.begin(), m_commands.begin() + static_cast<std::ptrdiff_t>(drop));
    m_bytes = bytes;
    m_index -= drop;
    if (m_cleanValid) {
        if (m_clean < drop) m_cleanValid = false;
        else m_clean -= drop;
    }
}

Status UndoStack::undo(Document& doc) {
    if (!canUndo()) return Error{ErrorCode::InvalidArgument, "Nothing to undo"};
    --m_index;
    m_commands[m_index]->undo(doc);
    notify();
    return {};
}

Status UndoStack::redo(Document& doc) {
    if (!canRedo()) return Error{ErrorCode::InvalidArgument, "Nothing to redo"};
    if (auto st = m_commands[m_index]->redo(doc); !st) return st;
    ++m_index;
    notify();
    return {};
}

std::string UndoStack::undoLabel() const { return canUndo() ? m_commands[m_index - 1]->label() : std::string(); }
std::string UndoStack::redoLabel() const { return canRedo() ? m_commands[m_index]->label() : std::string(); }

void UndoStack::clear() {
    m_commands.clear();
    m_index = 0;
    m_clean = 0;
    m_cleanValid = true;
    m_bytes = 0;
    notify();
}

} // namespace acestudio
