// Character profiles (Phase 5).
#include <catch2/catch_test_macros.hpp>

#include "TestSupport.h"
#include "acestudio/core/Character.h"

#include <fstream>

using namespace acestudio;

namespace {
CharacterProfile knight() {
    CharacterProfile c;
    c.name = "Red Knight";
    c.id = newCharacterId(c.name);
    c.prompt = "knight in red armour, side view";
    c.negative = "blurry";
    (void)c.palette.add({200, 40, 40, 255});
    (void)c.palette.add({20, 20, 30, 255});
    Image ref(16, 16, Rgba8{200, 40, 40, 255});
    ref.setPixel(3, 3, Rgba8{0, 0, 0, 128});
    c.references = {ref, Image(8, 4, Rgba8{1, 2, 3, 255})};
    c.width = 96;
    c.height = 128;
    c.seed = 1234567;
    c.loraStrength = 80;
    c.referenceStrength = 45;
    c.facingRight = false;
    c.notes = "main character";
    return c;
}
} // namespace

TEST_CASE("Character profiles round-trip exactly", "[character]") {
    const CharacterProfile c = knight();
    CHECK(c.id.rfind("red-knight-", 0) == 0);
    auto bytes = serializeCharacter(c);
    REQUIRE(bytes);
    auto back = deserializeCharacter(bytes.value());
    REQUIRE(back);
    const CharacterProfile& r = back.value();
    CHECK(r.id == c.id);
    CHECK(r.name == c.name);
    CHECK(r.prompt == c.prompt);
    CHECK(r.negative == c.negative);
    CHECK(r.palette.colors() == c.palette.colors());
    REQUIRE(r.references.size() == 2);
    CHECK(r.references[0] == c.references[0]);
    CHECK(r.references[1] == c.references[1]);
    CHECK(r.width == 96);
    CHECK(r.height == 128);
    CHECK(r.seed == 1234567);
    CHECK(r.loraStrength == 80);
    CHECK(r.referenceStrength == 45);
    CHECK_FALSE(r.facingRight);
    CHECK(r.notes == "main character");
}

TEST_CASE("Character validation and the library folder", "[character]") {
    CharacterProfile c = knight();
    c.name.clear();
    CHECK_FALSE(validateCharacter(c));
    c = knight();
    c.references.resize(5, Image(2, 2));
    CHECK_FALSE(serializeCharacter(c));
    c = knight();
    c.id = "../evil";
    CHECK_FALSE(validateCharacter(c));
    c = knight();
    c.width = 4;
    CHECK_FALSE(validateCharacter(c));
    CHECK(newCharacterId("!!!").rfind("character-", 0) == 0);

    TempDir dir;
    CharacterProfile a = knight();
    CharacterProfile b = knight();
    b.name = "Archer";
    b.id = newCharacterId(b.name);
    REQUIRE(saveCharacter(a, dir.path() / (a.id + ".acechar")));
    REQUIRE(saveCharacter(b, dir.path() / (b.id + ".acechar")));
    {
        std::ofstream bad(dir.path() / "broken.acechar", std::ios::binary);
        bad << "not a zip";
    }
    const auto list = listCharacters(dir.path());
    REQUIRE(list.size() == 2);
    CHECK(list[0].name == "Archer");
    CHECK(list[1].name == "Red Knight");
    auto loaded = loadCharacter(list[1].path);
    REQUIRE(loaded);
    CHECK(loaded.value().seed == 1234567);
    CHECK_FALSE(deserializeCharacter(std::vector<std::uint8_t>{1, 2, 3}));
}
