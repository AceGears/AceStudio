#include "TestSupport.h"

#include "acestudio/core/Document.h"
#include "acestudio/core/Editor.h"
#include "acestudio/core/PngCodec.h"
#include "acestudio/core/Script.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <fstream>
#include <thread>

using namespace acestudio;

namespace {

Result<ScriptResult> run(const std::string& src, ScriptOptions o = {}) { return runScript(src, "test.lua", o); }

std::string errorOf(const Result<ScriptResult>& r) { return r ? std::string() : r.error().message; }

} // namespace

TEST_CASE("Lua runs and prints", "[script]") {
    auto r = run("print('hello', 1 + 2, ace.version ~= nil)\nace.log('x')");
    REQUIRE(r);
    CHECK(r.value().output == "hello\t3\ttrue\nx\n");
    CHECK(luaVersion() == "Lua 5.4.9");
    // Script errors carry the chunk name and line.
    auto e = run("local a = 1\nerror('boom')");
    REQUIRE_FALSE(e);
    CHECK(errorOf(e).find("test.lua:2") != std::string::npos);
    CHECK(errorOf(e).find("boom") != std::string::npos);
}

TEST_CASE("Lua sandbox blocks escapes", "[script]") {
    // No io, package, debug, require, dofile, loadfile, os.execute, string.dump.
    for (const char* src : {"return io.open('x')", "return package.loadlib", "return debug.getinfo(1)", "return require('os')",
                            "return dofile('x')", "return loadfile('x')", "return os.execute('ls')", "return os.remove('x')",
                            "return os.getenv('HOME')", "return string.dump(print)"}) {
        INFO(src);
        auto r = run(std::string("local v = ") + (src + 7) + "\nif v == nil then error('blocked') end");
        CHECK_FALSE(r);
    }
    // os.time / os.clock still work.
    CHECK(run("assert(os.time() > 0 and os.clock() >= 0)"));
    // load: text works (with and without env), binary chunks are refused.
    CHECK(run("assert(load('return 1 + 1')() == 2)"));
    CHECK(run("local f = load('return x', 'c', 't', {x = 5}); assert(f() == 5)"));
    CHECK(run("local f, err = load('\\27Lua...'); assert(f == nil and err:find('binary'))"));
    // Files: none allowed by default.
    auto noFiles = run("ace.image(2, 2):save('a.png')");
    REQUIRE_FALSE(noFiles);
    CHECK(errorOf(noFiles).find("may not read or write") != std::string::npos);
}

TEST_CASE("Lua file access stays inside the allowed folders", "[script]") {
    TempDir dir;
    std::filesystem::create_directories(dir / "inside");
    ScriptOptions o;
    o.allowedRoots = {dir / "inside"};
    REQUIRE(run("ace.image(4, 3, '#ff0000'):save('a.png')", o));
    auto img = readPng(dir / "inside" / "a.png");
    REQUIRE(img);
    CHECK(img.value().width() == 4);
    CHECK(img.value().pixel(1, 1) == Rgba8{255, 0, 0, 255});
    for (const char* bad : {"ace.image(1, 1):save('../out.png')", "ace.load_png('../../etc/passwd')", "ace.write_text('/tmp/x.txt', 'x')",
                            "ace.image(1, 1):save('sub/../../x.png')"}) {
        INFO(bad);
        auto r = run(bad, o);
        REQUIRE_FALSE(r);
        CHECK(errorOf(r).find("outside the allowed folders") != std::string::npos);
    }
    CHECK_FALSE(std::filesystem::exists(dir / "out.png"));
    CHECK_FALSE(std::filesystem::exists(dir / "x.png"));
}

TEST_CASE("Lua memory and time limits and cancel", "[script]") {
    ScriptOptions o;
    o.limits.memoryBytes = 16u << 20;
    auto mem = run("local t = {} for i = 1, 1e8 do t[i] = string.rep('x', 100000) .. i end", o);
    REQUIRE_FALSE(mem);
    CHECK(errorOf(mem).find("not enough memory") != std::string::npos);

    ScriptOptions t;
    t.limits.seconds = 0.3;
    auto slow = run("while true do end", t);
    REQUIRE_FALSE(slow);
    CHECK(errorOf(slow).find("longer than") != std::string::npos);
    // pcall cannot swallow the limit forever: it fires again at the next check.
    auto sneaky = run("while true do pcall(function() while true do end end) end", t);
    REQUIRE_FALSE(sneaky);

    std::atomic<bool> stop{false};
    ScriptOptions c;
    c.limits.seconds = 0;
    c.cancel = &stop;
    std::thread th([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        stop = true;
    });
    auto cancelled = run("while true do end", c);
    th.join();
    REQUIRE_FALSE(cancelled);
    CHECK(errorOf(cancelled).find("cancelled") != std::string::npos);
}

TEST_CASE("Lua image API", "[script]") {
    auto r = run(R"(
        local img = ace.image(8, 8)
        assert(img.width == 8 and img.height == 8)
        img:set(1, 2, '#11223344')
        local r, g, b, a = img:get(1, 2)
        assert(r == 0x11 and g == 0x22 and b == 0x33 and a == 0x44)
        img:set(0, 0, {255, 0, 0})
        assert(img:hex(0, 0) == '#ff0000ff')
        img:rect(2, 2, 3, 3, '#00ff00')
        img:line(0, 7, 7, 7, '#0000ff')
        local _, _, bb = img:get(5, 7); assert(bb == 255)
        local big = img:scale(16, 16); assert(big.width == 16)
        local f = img:flip(); local fr = f:get(7, 0); assert(fr == 255)
        local rot = ace.image(4, 2):rotate(90); assert(rot.width == 2 and rot.height == 4)
        local part = img:copy(2, 2, 3, 3); local _, pg = part:get(0, 0); assert(pg == 255)
        local o = part:outline('#000000'); assert(o.width == 3)
        assert(not pcall(img.get, img, 8, 0))
        assert(not pcall(img.set, img, 0, 0, 'nope'))
    )");
    INFO(errorOf(r));
    CHECK(r);
}

TEST_CASE("Lua edits the host document in one undo step", "[script]") {
    auto doc = Document::create(16, 16);
    REQUIRE(doc);
    REQUIRE(doc.value().addLayer("Base"));
    Editor ed(std::move(doc).value());
    const std::size_t before = ed.history().mark();
    ScriptOptions o;
    o.document = &ed;
    auto r = run(R"(
        local d = ace.document()
        assert(d.width == 16 and d.frames == 1)
        local img = ace.image(16, 16, '#ff00ff')
        d:set_cel(0, 0, img)
        local f = d:add_frame(80)
        assert(f == 1 and d.frames == 2)
        d:set_cel(0, f, img:flip())
        d:add_tag('walk', 0, 1, 'pingpong')
        local l = d:add_layer('Top')
        assert(#d:layers() == 2 and d:layers()[l + 1].name == 'Top')
        assert(d:duration(1) == 80)
        local c = d:composite(0); local r = c:get(3, 3); assert(r == 255)
    )", o);
    INFO(errorOf(r));
    REQUIRE(r);
    CHECK(r.value().documentChanged);
    CHECK(ed.document().frameCount() == 2);
    CHECK(ed.document().tags().size() == 1);
    CHECK(ed.history().mark() == before + 1);   // one undo step
    REQUIRE(ed.undo());
    CHECK(ed.document().frameCount() == 1);
    CHECK(ed.document().layers().size() == 1);

    // A failing script leaves the document as it was.
    auto bad = run("local d = ace.document(); d:add_frame(); d:add_layer('x'); error('stop')", o);
    REQUIRE_FALSE(bad);
    CHECK(ed.document().frameCount() == 1);
    CHECK(ed.document().layers().size() == 1);
    CHECK(ed.history().mark() == before);
}

TEST_CASE("Lua documents and asset tools", "[script]") {
    TempDir dir;
    ScriptOptions o;
    o.allowedRoots = {dir.path()};
    auto r = run(R"(
        assert(ace.document() == nil)
        local d = ace.new_document(8, 8, 3)
        assert(d.frames == 3)
        for f = 0, 2 do d:set_cel(0, f, ace.image(8, 8, {f * 100, 0, 0})) end
        d:save('hero.acestudio')
        local again = ace.open('hero.acestudio'); assert(again.frames == 3)
        assert(d:export_sheet('hero.png', {engines = 'godot,unity'}) >= 3)

        local grass = ace.image(16, 16, '#3a8a3a')
        local sand = ace.image(16, 16, '#d8c070')
        local ts = ace.tileset{base = sand, terrains = {grass}, scheme = 'blob47', tile = 16}
        assert(ts.levels == 1 and ts:seams() == 0)
        local n, pairs = ace.check_seams(ts:image(1), 'blob47')
        assert(n == 0 and pairs > 0)
        assert(ts:write('ground.png', {engines = 'godot,tiled'}) >= 3)

        local atlas = ace.atlas({{name = 'a', image = grass}, {name = 'b', image = ace.image(4, 4, "#ffffff")}}, {padding = 1})
        assert(#atlas.sprites == 2 and atlas.json:find('"a"'))
        local sheet = ace.image(20, 8)
        sheet:rect(1, 1, 4, 4, '#ff0000'); sheet:rect(12, 2, 5, 5, '#00ff00')
        local s = ace.slice(sheet)
        assert(#s == 2, 'slices: ' .. #s)
        assert(#ace.slice(sheet, {grid = '10x8'}) == 2)
        assert(ace.write_uikit('ui.png', {accent = '#ff8800'}) >= 2)
        print(#arg)
    )", o);
    INFO(errorOf(r));
    REQUIRE(r);
    CHECK(std::filesystem::exists(dir / "hero.acestudio"));
    CHECK(std::filesystem::exists(dir / "hero.png"));
    CHECK(std::filesystem::exists(dir / "ground.png"));
    CHECK(std::filesystem::exists(dir / "ui.png"));
    CHECK(r.value().written.size() >= 9);
    // Writing over files needs overwrite = true.
    auto again = run("ace.write_uikit('ui.png')", o);
    REQUIRE_FALSE(again);
    auto forced = run("ace.write_uikit('ui.png', {overwrite = true})", o);
    CHECK(forced);
}

TEST_CASE("Lua script files get their folder and arguments", "[script]") {
    TempDir dir;
    {
        std::ofstream f(dir / "make.lua");
        f << "ace.image(2, 2, arg[1]):save(arg[2])\nprint(#arg)\n";
    }
    ScriptOptions o;
    o.args = {"#00ff00", "out.png"};
    auto r = runScriptFile(dir / "make.lua", o);
    INFO(errorOf(r));
    REQUIRE(r);
    CHECK(r.value().output == "2\n");
    auto img = readPng(dir / "out.png");
    REQUIRE(img);
    CHECK(img.value().pixel(0, 0) == Rgba8{0, 255, 0, 255});
}

TEST_CASE("Editor::replaceDocument is one undo step", "[script]") {
    auto doc = Document::create(8, 8);
    REQUIRE(doc);
    REQUIRE(doc.value().addLayer("Base"));
    Editor ed(std::move(doc).value());
    // The app runs scripts on a copy, then swaps the result in.
    Editor copy(ed.document());
    ScriptOptions o;
    o.document = &copy;
    REQUIRE(run("local d = ace.document(); d:add_frame(); d:add_layer('Lua')", o));
    REQUIRE(ed.replaceDocument(copy.document(), "Run Script"));
    CHECK(ed.document().frameCount() == 2);
    CHECK(ed.document().layers().size() == 2);
    REQUIRE(ed.undo());
    CHECK(ed.document().frameCount() == 1);
    CHECK(ed.document().layers().size() == 1);
    REQUIRE(ed.redo());
    CHECK(ed.document().layers().size() == 2);
}
