#include "acestudio/core/UndoStack.h"
#include "acestudio/core/Document.h"

namespace acestudio {

namespace {
class CompositeCommand final : public Command {
public:
    CompositeCommand(std::string label, std::vector<std::unique_ptr<Command>> parts)
        : m_label(std::move(label)), m_parts(std::move(parts)) {}
    std::string label() const override { return m_label; }
    Status redo(Document& doc) override {
        for (std::size_t i = 0; i < m_parts.size(); ++i)
            if (auto st = m_parts[i]->redo(doc); !st) {
                while (i-- > 0) m_parts[i]->undo(doc);
                return st;
            }
        return {};
    }
    void undo(Document& doc) override {
        for (std::size_t i = m_parts.size(); i-- > 0;) m_parts[i]->undo(doc);
    }
    std::size_t memoryBytes() const override {
        std::size_t b = 0;
        for (const auto& p : m_parts) b += p->memoryBytes();
        return b;
    }

private:
    std::string m_label;
    std::vector<std::unique_ptr<Command>> m_parts;
};
} // namespace

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
    m_dropped += drop;
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

Status UndoStack::squash(std::size_t mark, std::string label) {
    if (mark > m_dropped + m_index) return Error{ErrorCode::InvalidArgument, "Commands since the mark were undone"};
    const std::size_t from = mark < m_dropped ? 0 : mark - m_dropped;   // older steps may have been trimmed
    const std::size_t end = m_index;
    if (end - from < 2) return {};
    if (m_commands.size() != end) return Error{ErrorCode::InvalidArgument, "Cannot group commands with a redo branch"};
    std::vector<std::unique_ptr<Command>> parts;
    for (std::size_t i = from; i < end; ++i) parts.push_back(std::move(m_commands[i]));
    m_commands.erase(m_commands.begin() + static_cast<std::ptrdiff_t>(from), m_commands.end());
    m_commands.push_back(std::make_unique<CompositeCommand>(std::move(label), std::move(parts)));
    m_index = m_commands.size();
    if (m_cleanValid) {
        if (m_clean == end) m_clean = m_index;
        else if (m_clean > from) m_cleanValid = false;   // saved state was inside the group
    }
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
