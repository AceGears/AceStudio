#include <catch2/catch_test_macros.hpp>

#include "acestudio/core/Document.h"
#include "acestudio/core/UndoStack.h"

using namespace acestudio;

namespace {
// Adds `delta` to the document's frame-0 duration; reversible.
class AddDuration final : public Command {
public:
    AddDuration(int delta, std::size_t bytes = 0, long long key = -1) : m_delta(delta), m_bytes(bytes), m_key(key) {}
    std::string label() const override { return "Add " + std::to_string(m_delta); }
    Status redo(Document& d) override { return d.setFrameDuration(0, d.frames()[0].durationMs + m_delta); }
    void undo(Document& d) override { (void)d.setFrameDuration(0, d.frames()[0].durationMs - m_delta); }
    std::size_t memoryBytes() const override { return m_bytes; }
    long long mergeKey() const override { return m_key; }
    bool mergeWith(const Command& next) override {
        m_delta += static_cast<const AddDuration&>(next).m_delta;
        return true;
    }
private:
    int m_delta;
    std::size_t m_bytes;
    long long m_key;
};

int duration(const Document& d) { return d.frames()[0].durationMs; }
} // namespace

TEST_CASE("UndoStack basic undo/redo and labels", "[undo]") {
    auto doc = Document::create(1, 1).value();
    UndoStack s;
    int notifications = 0;
    s.setListener([&] { ++notifications; });
    CHECK_FALSE(s.canUndo());
    CHECK_FALSE(s.undo(doc));

    REQUIRE(s.push(doc, std::make_unique<AddDuration>(10)));
    REQUIRE(s.push(doc, std::make_unique<AddDuration>(5)));
    CHECK(duration(doc) == 115);
    CHECK(s.undoLabel() == "Add 5");
    REQUIRE(s.undo(doc));
    CHECK(duration(doc) == 110);
    CHECK(s.redoLabel() == "Add 5");
    REQUIRE(s.redo(doc));
    CHECK(duration(doc) == 115);
    CHECK(notifications == 4);
}

TEST_CASE("Failed commands are not recorded", "[undo]") {
    auto doc = Document::create(1, 1).value();
    UndoStack s;
    CHECK_FALSE(s.push(doc, std::make_unique<AddDuration>(-1000)));   // invalid duration
    CHECK(s.size() == 0);
    CHECK(duration(doc) == 100);
}

TEST_CASE("Pushing after undo discards the redo branch", "[undo]") {
    auto doc = Document::create(1, 1).value();
    UndoStack s;
    REQUIRE(s.push(doc, std::make_unique<AddDuration>(1)));
    REQUIRE(s.push(doc, std::make_unique<AddDuration>(2)));
    REQUIRE(s.undo(doc));
    REQUIRE(s.push(doc, std::make_unique<AddDuration>(3)));
    CHECK_FALSE(s.canRedo());
    CHECK(s.size() == 2);
    CHECK(duration(doc) == 104);
}

TEST_CASE("Clean state tracks the saved point", "[undo]") {
    auto doc = Document::create(1, 1).value();
    UndoStack s;
    CHECK(s.isClean());
    REQUIRE(s.push(doc, std::make_unique<AddDuration>(1)));
    CHECK_FALSE(s.isClean());
    s.setClean();
    CHECK(s.isClean());
    REQUIRE(s.undo(doc));
    CHECK_FALSE(s.isClean());
    REQUIRE(s.redo(doc));
    CHECK(s.isClean());
    // Branching away from the saved state makes it unreachable.
    REQUIRE(s.undo(doc));
    REQUIRE(s.push(doc, std::make_unique<AddDuration>(7)));
    REQUIRE(s.undo(doc));
    CHECK_FALSE(s.isClean());
}

TEST_CASE("Merging coalesces but never across the clean point", "[undo]") {
    auto doc = Document::create(1, 1).value();
    UndoStack s;
    REQUIRE(s.push(doc, std::make_unique<AddDuration>(1, 0, 42), true));
    REQUIRE(s.push(doc, std::make_unique<AddDuration>(1, 0, 42), true));
    CHECK(s.size() == 1);
    s.setClean();
    REQUIRE(s.push(doc, std::make_unique<AddDuration>(1, 0, 42), true));
    CHECK(s.size() == 2);
    REQUIRE(s.undo(doc));
    CHECK(s.isClean());
    CHECK(duration(doc) == 102);
}

TEST_CASE("Step and memory limits drop the oldest steps", "[undo]") {
    auto doc = Document::create(1, 1).value();
    UndoStack steps(1u << 30, 3);
    for (int i = 0; i < 5; ++i) REQUIRE(steps.push(doc, std::make_unique<AddDuration>(1)));
    CHECK(steps.size() == 3);
    CHECK(duration(doc) == 105);

    UndoStack mem(100, 1000);
    for (int i = 0; i < 5; ++i) REQUIRE(mem.push(doc, std::make_unique<AddDuration>(1, 40)));
    CHECK(mem.size() == 2);
    CHECK(mem.memoryBytes() == 80);
    // A single command larger than the budget is still kept (newest wins).
    REQUIRE(mem.push(doc, std::make_unique<AddDuration>(1, 500)));
    CHECK(mem.size() == 1);
}
