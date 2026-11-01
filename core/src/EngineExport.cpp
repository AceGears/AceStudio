#include "acestudio/core/EngineExport.h"

#include "acestudio/core/Document.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <numeric>
#include <sstream>

namespace acestudio {

namespace {

std::uint64_t fnv1a(const std::string& s, std::uint64_t h = 1469598103934665603ull) {
    for (char c : s) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    return h;
}

std::string hex64(std::uint64_t v) {
    char b[17];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}

// Godot/Tiled strings: quotes and backslashes escaped.
std::string quoted(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        if (c == '\n') { o += "\\n"; continue; }
        o += c;
    }
    return o + "\"";
}

std::string xmlEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        default: o += c;
        }
    }
    return o;
}

// Unity YAML scalars: names are quoted when they could be misread.
std::string yamlName(const std::string& s) {
    const bool plain = !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    }) && !(s[0] >= '0' && s[0] <= '9');
    if (plain) return s;
    std::string o = "'";
    for (char c : s) o += c == '\'' ? std::string("''") : std::string(1, c);
    return o + "'";
}

std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.6g", v);
    std::string s = b;
    return s;
}

// Godot floats always carry a decimal point.
std::string gdFloat(double v) {
    std::string s = num(v);
    if (s.find_first_of(".e") == std::string::npos) s += ".0";
    return s;
}

int gcd(int a, int b) { return b == 0 ? a : gcd(b, a % b); }

} // namespace

// ------------------------------------------------------------------ sheets

EngineSheet engineSheet(const Document& doc, const SpriteSheet& sheet, const std::string& baseName) {
    EngineSheet e;
    e.width = sheet.image.width();
    e.height = sheet.image.height();
    std::map<int, int> byDocFrame;   // document frame -> sheet frame index
    for (std::size_t i = 0; i < sheet.frames.size(); ++i) {
        const SheetFrame& f = sheet.frames[i];
        EngineFrame ef;
        ef.name = baseName + "_" + std::to_string(i);
        ef.rect = f.rect;
        // Pivot at the bottom centre of the untrimmed frame, expressed in the
        // trimmed rect (engines place the rect, not the full frame).
        if (f.rect.w > 0 && f.rect.h > 0) {
            const double fullW = sheet.frameWidth, fullH = sheet.frameHeight;
            ef.pivotX = (fullW / 2.0 - f.source.x) / f.rect.w;
            ef.pivotY = (fullH - f.source.y) / f.rect.h;
        }
        e.frames.push_back(ef);
        byDocFrame[f.frame] = static_cast<int>(i);
    }
    auto dur = [&](int sheetIndex) { return std::max(1, sheet.frames[static_cast<std::size_t>(sheetIndex)].durationMs); };
    for (const AnimationTag& t : doc.tags()) {
        EngineAnimation a;
        a.name = t.name;
        std::vector<int> fwd;
        for (int f = t.fromFrame; f <= t.toFrame; ++f)
            if (auto it = byDocFrame.find(f); it != byDocFrame.end()) fwd.push_back(it->second);
        if (fwd.empty()) continue;
        if (t.direction == PlaybackDirection::Reverse) std::reverse(fwd.begin(), fwd.end());
        if (t.direction == PlaybackDirection::PingPong && fwd.size() > 2) {
            std::vector<int> back(fwd.rbegin() + 1, fwd.rend() - 1);
            fwd.insert(fwd.end(), back.begin(), back.end());
        }
        a.frames = fwd;
        for (int i : fwd) a.durationsMs.push_back(dur(i));
        a.loop = t.repeat == 0;
        e.animations.push_back(std::move(a));
    }
    if (e.animations.empty() && !e.frames.empty()) {
        EngineAnimation a;
        a.name = "default";
        for (int i = 0; i < static_cast<int>(e.frames.size()); ++i) {
            a.frames.push_back(i);
            a.durationsMs.push_back(dur(i));
        }
        e.animations.push_back(std::move(a));
    }
    return e;
}

EngineSheet engineSheet(const Atlas& atlas, int page, const std::vector<NineSlice>& borders) {
    EngineSheet e;
    if (page < 0 || page >= static_cast<int>(atlas.pages.size())) return e;
    e.width = atlas.pages[static_cast<std::size_t>(page)].width();
    e.height = atlas.pages[static_cast<std::size_t>(page)].height();
    for (std::size_t i = 0; i < atlas.sprites.size(); ++i) {
        const AtlasSprite& s = atlas.sprites[i];
        if (s.page != page) continue;
        EngineFrame f;
        f.name = s.name;
        f.rect = s.rect;
        f.pivotX = 0.5;
        f.pivotY = 0.5;
        if (i < borders.size()) f.border = borders[i];
        e.frames.push_back(f);
    }
    return e;
}

// ------------------------------------------------------------------ Godot

std::string godotSpriteFrames(const EngineSheet& sheet, const std::string& imagePath) {
    std::ostringstream o;
    const std::size_t steps = sheet.frames.size() + 2;
    o << "[gd_resource type=\"SpriteFrames\" load_steps=" << steps << " format=3]\n\n";
    o << "[ext_resource type=\"Texture2D\" path=" << quoted(imagePath) << " id=\"1_sheet\"]\n\n";
    for (std::size_t i = 0; i < sheet.frames.size(); ++i) {
        const IntRect& r = sheet.frames[i].rect;
        o << "[sub_resource type=\"AtlasTexture\" id=\"AtlasTexture_" << i << "\"]\n";
        o << "atlas = ExtResource(\"1_sheet\")\n";
        o << "region = Rect2(" << r.x << ", " << r.y << ", " << r.w << ", " << r.h << ")\n\n";
    }
    o << "[resource]\nanimations = [";
    for (std::size_t a = 0; a < sheet.animations.size(); ++a) {
        const EngineAnimation& an = sheet.animations[a];
        // Godot plays at `speed` frames per second, each frame lasting
        // `duration` of those: the speed is the greatest common step.
        int step = 0;
        for (int d : an.durationsMs) step = gcd(step, std::max(1, d));
        step = std::max(1, step);
        o << (a ? ", " : "") << "{\n\"frames\": [";
        for (std::size_t k = 0; k < an.frames.size(); ++k) {
            o << (k ? ", " : "") << "{\n\"duration\": " << gdFloat(double(an.durationsMs[k]) / step)
              << ",\n\"texture\": SubResource(\"AtlasTexture_" << an.frames[k] << "\")\n}";
        }
        o << "],\n\"loop\": " << (an.loop ? "true" : "false") << ",\n\"name\": &" << quoted(an.name)
          << ",\n\"speed\": " << gdFloat(1000.0 / step) << "\n}";
    }
    o << "]\n";
    return o.str();
}

namespace {

// Godot peering bits for a tile from its samples (TL, T, TR, L, C, R, BL, B, BR).
struct Peering {
    const char* bit;
    int sample;
};
constexpr Peering kSides[] = {{"top_side", 1}, {"right_side", 5}, {"bottom_side", 7}, {"left_side", 3}};
constexpr Peering kCorners[] = {{"top_left_corner", 0}, {"top_right_corner", 2}, {"bottom_right_corner", 8}, {"bottom_left_corner", 6}};

std::string colorFor(std::size_t i) {
    static const char* const colors[] = {"Color(0.55, 0.4, 0.25, 1)", "Color(0.3, 0.65, 0.25, 1)", "Color(0.55, 0.55, 0.6, 1)",
                                         "Color(0.9, 0.9, 0.95, 1)", "Color(0.2, 0.4, 0.8, 1)"};
    return colors[i % 5];
}

} // namespace

std::string godotTileSet(const std::vector<Tileset>& levels, const std::vector<std::string>& imagePaths,
                         const std::string& baseImagePath, const GodotTerrainNames& names) {
    std::ostringstream o;
    if (levels.empty()) return {};
    const Tileset& first = levels.front();
    const int T = first.tileSize;
    const auto [cols, rows] = tilesetGrid(first.scheme);
    (void)rows;
    const bool hasBase = !baseImagePath.empty() && first.scheme != AutotileScheme::Wang16;
    const int mode = first.scheme == AutotileScheme::Blob47 ? 0 : first.scheme == AutotileScheme::Wang16 ? 1 : 2;
    const std::size_t sources = levels.size() + (hasBase ? 1 : 0);
    o << "[gd_resource type=\"TileSet\" load_steps=" << (2 * sources + 1) << " format=3]\n\n";
    for (std::size_t i = 0; i < levels.size(); ++i)
        o << "[ext_resource type=\"Texture2D\" path=" << quoted(i < imagePaths.size() ? imagePaths[i] : std::string()) << " id=\"" << (i + 1)
          << "_level\"]\n";
    if (hasBase) o << "[ext_resource type=\"Texture2D\" path=" << quoted(baseImagePath) << " id=\"0_base\"]\n";
    o << "\n";
    auto levelName = [&](std::size_t i) {
        return i < names.levels.size() && !names.levels[i].empty() ? names.levels[i] : "terrain " + std::to_string(i + 1);
    };
    for (std::size_t i = 0; i < levels.size(); ++i) {
        const Tileset& t = levels[i];
        // Level 1: terrain 0 = base, 1 = the terrain. Higher levels: terrain 0
        // = the terrain, the rest is empty (-1).
        const int on = i == 0 ? 1 : 0, off = i == 0 ? 0 : -1;
        o << "[sub_resource type=\"TileSetAtlasSource\" id=\"TileSetAtlasSource_" << (i + 1) << "\"]\n";
        o << "texture = ExtResource(\"" << (i + 1) << "_level\")\n";
        o << "texture_region_size = Vector2i(" << T << ", " << T << ")\n";
        for (int k = 0; k < tileCount(t.scheme); ++k) {
            const TileSamples s = tileSamples(t.scheme, k);
            const std::string id = std::to_string(k % cols) + ":" + std::to_string(k / cols) + "/0";
            auto v = [&](std::uint8_t sample) { return sample >= 128 ? on : off; };
            // The centre: the terrain for Blob/Cardinal; for corners the
            // majority of the four corners (Godot uses it when painting).
            int centre = v(s[4]);
            if (t.scheme == AutotileScheme::Wang16) {
                const int n = (s[0] >= 128) + (s[2] >= 128) + (s[6] >= 128) + (s[8] >= 128);
                centre = n >= 2 ? on : off;
            }
            o << id << " = 0\n";
            o << id << "/terrain_set = " << i << "\n";
            o << id << "/terrain = " << centre << "\n";
            if (t.scheme != AutotileScheme::Wang16)
                for (const Peering& p : kSides) o << id << "/terrains_peering_bit/" << p.bit << " = " << v(s[static_cast<std::size_t>(p.sample)]) << "\n";
            if (t.scheme != AutotileScheme::Cardinal16)
                for (const Peering& p : kCorners) o << id << "/terrains_peering_bit/" << p.bit << " = " << v(s[static_cast<std::size_t>(p.sample)]) << "\n";
        }
        o << "\n";
    }
    if (hasBase) {
        o << "[sub_resource type=\"TileSetAtlasSource\" id=\"TileSetAtlasSource_0\"]\n";
        o << "texture = ExtResource(\"0_base\")\n";
        o << "texture_region_size = Vector2i(" << T << ", " << T << ")\n";
        const std::string id = "0:0/0";
        o << id << " = 0\n" << id << "/terrain_set = 0\n" << id << "/terrain = 0\n";
        if (first.scheme != AutotileScheme::Wang16)
            for (const Peering& p : kSides) o << id << "/terrains_peering_bit/" << p.bit << " = 0\n";
        if (first.scheme == AutotileScheme::Blob47)
            for (const Peering& p : kCorners) o << id << "/terrains_peering_bit/" << p.bit << " = 0\n";
        o << "\n";
    }
    o << "[resource]\n";
    o << "tile_size = Vector2i(" << T << ", " << T << ")\n";
    for (std::size_t i = 0; i < levels.size(); ++i) {
        o << "terrain_set_" << i << "/mode = " << mode << "\n";
        std::size_t k = 0;
        if (i == 0) {
            o << "terrain_set_0/terrain_0/name = " << quoted(names.base) << "\n";
            o << "terrain_set_0/terrain_0/color = " << colorFor(0) << "\n";
            k = 1;
        }
        o << "terrain_set_" << i << "/terrain_" << k << "/name = " << quoted(levelName(i)) << "\n";
        o << "terrain_set_" << i << "/terrain_" << k << "/color = " << colorFor(i + 1) << "\n";
    }
    if (hasBase) o << "sources/0 = SubResource(\"TileSetAtlasSource_0\")\n";
    for (std::size_t i = 0; i < levels.size(); ++i) o << "sources/" << (i + 1) << " = SubResource(\"TileSetAtlasSource_" << (i + 1) << "\")\n";
    return o.str();
}

std::string godotStyleBox(const std::string& imagePath, const IntRect& r, const NineSlice& n) {
    std::ostringstream o;
    o << "[gd_resource type=\"StyleBoxTexture\" load_steps=2 format=3]\n\n";
    o << "[ext_resource type=\"Texture2D\" path=" << quoted(imagePath) << " id=\"1_atlas\"]\n\n";
    o << "[resource]\n";
    o << "texture = ExtResource(\"1_atlas\")\n";
    o << "texture_margin_left = " << gdFloat(n.left) << "\n";
    o << "texture_margin_top = " << gdFloat(n.top) << "\n";
    o << "texture_margin_right = " << gdFloat(n.right) << "\n";
    o << "texture_margin_bottom = " << gdFloat(n.bottom) << "\n";
    o << "region_rect = Rect2(" << r.x << ", " << r.y << ", " << r.w << ", " << r.h << ")\n";
    return o.str();
}

// ------------------------------------------------------------------ Unity

std::string unityGuid(const std::string& seed) {
    return hex64(fnv1a(seed)) + hex64(fnv1a(seed, 0x84222325cbf29ce4ull));
}

std::int64_t unityFileId(const std::string& seed) {
    // Positive, non-zero and below 2^62 so it never collides with Unity's
    // reserved small IDs.
    return static_cast<std::int64_t>((fnv1a(seed) & 0x3fffffffffffffffull) | 0x1000000000000000ull);
}

std::string unityTextureMeta(const EngineSheet& sheet, const std::string& guid, int ppu) {
    std::ostringstream o;
    o << "fileFormatVersion: 2\nguid: " << guid << "\nTextureImporter:\n";
    o << "  internalIDToNameTable: []\n  externalObjects: {}\n  serializedVersion: 13\n";
    o << "  mipmaps:\n    mipMapMode: 0\n    enableMipMap: 0\n    sRGBTexture: 1\n    linearTexture: 0\n"
         "    fadeOut: 0\n    borderMipMap: 0\n    mipMapsPreserveCoverage: 0\n    alphaTestReferenceValue: 0.5\n"
         "    mipMapFadeDistanceStart: 1\n    mipMapFadeDistanceEnd: 3\n";
    o << "  bumpmap:\n    convertToNormalMap: 0\n    externalNormalMap: 0\n    heightScale: 0.25\n    normalMapFilter: 0\n"
         "    flipGreenChannel: 0\n";
    o << "  isReadable: 0\n  streamingMipmaps: 0\n  streamingMipmapsPriority: 0\n  vTOnly: 0\n  ignoreMipmapLimit: 0\n"
         "  grayScaleToAlpha: 0\n  generateCubemap: 6\n  cubemapConvolution: 0\n  seamlessCubemap: 0\n"
         "  textureFormat: 1\n  maxTextureSize: 8192\n";
    o << "  textureSettings:\n    serializedVersion: 2\n    filterMode: 0\n    aniso: 1\n    mipBias: 0\n    wrapU: 1\n"
         "    wrapV: 1\n    wrapW: 1\n";
    o << "  nPOTScale: 0\n  lightmap: 0\n  compressionQuality: 50\n  spriteMode: 2\n  spriteExtrude: 1\n"
         "  spriteMeshType: 0\n  alignment: 0\n  spritePivot: {x: 0.5, y: 0.5}\n  spritePixelsToUnits: "
      << ppu << "\n  spriteBorder: {x: 0, y: 0, z: 0, w: 0}\n  spriteGenerateFallbackPhysicsShape: 1\n"
         "  alphaUsage: 1\n  alphaIsTransparency: 1\n  spriteTessellationDetail: -1\n  textureType: 8\n"
         "  textureShape: 1\n  singleChannelComponent: 0\n  flipbookRows: 1\n  flipbookColumns: 1\n"
         "  maxTextureSizeSet: 0\n  compressionQualitySet: 0\n  textureFormatSet: 0\n  ignorePngGamma: 0\n"
         "  applyGammaDecoding: 0\n  swizzle: 50462976\n  cookieLightType: 0\n";
    o << "  platformSettings:\n  - serializedVersion: 3\n    buildTarget: DefaultTexturePlatform\n    maxTextureSize: 8192\n"
         "    resizeAlgorithm: 0\n    textureFormat: -1\n    textureCompression: 0\n    compressionQuality: 50\n"
         "    crunchedCompression: 0\n    allowsAlphaSplitting: 0\n    overridden: 0\n    ignorePlatformSupport: 0\n"
         "    androidETC2FallbackOverride: 0\n    forceMaximumCompressionQuality_BC6H_BC7: 0\n";
    o << "  spriteSheet:\n    serializedVersion: 2\n    sprites:\n";
    for (const EngineFrame& f : sheet.frames) {
        // Unity rects start at the bottom-left of the texture.
        const int uy = sheet.height - (f.rect.y + f.rect.h);
        o << "    - serializedVersion: 2\n      name: " << yamlName(f.name) << "\n      rect:\n        serializedVersion: 2\n"
          << "        x: " << f.rect.x << "\n        y: " << uy << "\n        width: " << f.rect.w << "\n        height: " << f.rect.h << "\n"
          << "      alignment: 9\n      pivot: {x: " << num(f.pivotX) << ", y: " << num(1.0 - f.pivotY) << "}\n"
          << "      border: {x: " << f.border.left << ", y: " << f.border.bottom << ", z: " << f.border.right << ", w: " << f.border.top << "}\n"
          << "      customData: \n      outline: []\n      physicsShape: []\n      tessellationDetail: -1\n      bones: []\n"
          << "      spriteID: " << unityGuid(guid + "/" + f.name) << "\n      internalID: " << unityFileId(guid + "/" + f.name) << "\n"
          << "      vertices: []\n      indices: \n      edges: []\n      weights: []\n";
    }
    if (sheet.frames.empty()) o << "      []\n";
    o << "    outline: []\n    customData: \n    physicsShape: []\n    bones: []\n    spriteID: \n    internalID: 0\n"
         "    vertices: []\n    indices: \n    edges: []\n    weights: []\n    secondaryTextures: []\n";
    o << "    spriteCustomMetadata:\n      entries: []\n";
    o << "    nameFileIdTable:\n";
    for (const EngineFrame& f : sheet.frames) o << "      " << yamlName(f.name) << ": " << unityFileId(guid + "/" + f.name) << "\n";
    if (sheet.frames.empty()) o << "      {}\n";
    o << "  mipmapLimitGroupName: \n  pSDRemoveMatte: 0\n  userData: \n  assetBundleName: \n  assetBundleVariant: \n";
    return o.str();
}

std::string unityAnimationClip(const EngineSheet& sheet, std::size_t index, const std::string& textureGuid) {
    if (index >= sheet.animations.size()) return {};
    const EngineAnimation& a = sheet.animations[index];
    int step = 0;
    for (int d : a.durationsMs) step = gcd(step, std::max(1, d));
    step = std::max(1, step);
    const double rate = 1000.0 / step;
    std::ostringstream keys;
    double t = 0;
    for (std::size_t k = 0; k < a.frames.size(); ++k) {
        const EngineFrame& f = sheet.frames[static_cast<std::size_t>(a.frames[k])];
        keys << "      - time: " << num(t) << "\n        value: {fileID: " << unityFileId(textureGuid + "/" + f.name)
             << ", guid: " << textureGuid << ", type: 3}\n";
        t += a.durationsMs[k] / 1000.0;
    }
    // Hold the last frame for its duration: a final key at the end repeats it.
    if (!a.frames.empty()) {
        const EngineFrame& f = sheet.frames[static_cast<std::size_t>(a.frames.back())];
        keys << "      - time: " << num(t - 1.0 / rate) << "\n        value: {fileID: " << unityFileId(textureGuid + "/" + f.name)
             << ", guid: " << textureGuid << ", type: 3}\n";
    }
    std::ostringstream o;
    o << "%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:\n--- !u!74 &7400000\nAnimationClip:\n  m_ObjectHideFlags: 0\n"
         "  m_CorrespondingSourceObject: {fileID: 0}\n  m_PrefabInstance: {fileID: 0}\n  m_PrefabAsset: {fileID: 0}\n"
      << "  m_Name: " << yamlName(a.name) << "\n  serializedVersion: 7\n  m_Legacy: 0\n  m_Compressed: 0\n  m_UseHighQualityCurve: 1\n"
      << "  m_RotationCurves: []\n  m_CompressedRotationCurves: []\n  m_EulerCurves: []\n  m_PositionCurves: []\n"
         "  m_ScaleCurves: []\n  m_FloatCurves: []\n";
    o << "  m_PPtrCurves:\n  - curve:\n" << keys.str() << "    attribute: m_Sprite\n    path: \n    classID: 212\n"
         "    script: {fileID: 0}\n    flags: 2\n";
    o << "  m_SampleRate: " << num(rate) << "\n  m_WrapMode: 0\n  m_Bounds:\n    m_Center: {x: 0, y: 0, z: 0}\n"
         "    m_Extent: {x: 0, y: 0, z: 0}\n";
    o << "  m_ClipBindingConstant:\n    genericBindings:\n    - serializedVersion: 2\n      path: 0\n      attribute: 0\n"
         "      script: {fileID: 0}\n      typeID: 212\n      customType: 23\n      isPPtrCurve: 1\n      isIntCurve: 0\n"
         "      isSerializeReferenceCurve: 0\n    pptrCurveMapping:\n";
    for (int fi : a.frames)
        o << "    - {fileID: " << unityFileId(textureGuid + "/" + sheet.frames[static_cast<std::size_t>(fi)].name) << ", guid: " << textureGuid
          << ", type: 3}\n";
    if (!a.frames.empty())
        o << "    - {fileID: " << unityFileId(textureGuid + "/" + sheet.frames[static_cast<std::size_t>(a.frames.back())].name)
          << ", guid: " << textureGuid << ", type: 3}\n";
    o << "  m_AnimationClipSettings:\n    serializedVersion: 2\n    m_AdditiveReferencePoseClip: {fileID: 0}\n"
         "    m_AdditiveReferencePoseTime: 0\n    m_StartTime: 0\n    m_StopTime: "
      << num(t) << "\n    m_OrientationOffsetY: 0\n    m_Level: 0\n    m_CycleOffset: 0\n    m_HasAdditiveReferencePose: 0\n"
      << "    m_LoopTime: " << (a.loop ? 1 : 0) << "\n    m_LoopBlend: 0\n    m_LoopBlendOrientation: 0\n"
         "    m_LoopBlendPositionY: 0\n    m_LoopBlendPositionXZ: 0\n    m_KeepOriginalOrientation: 0\n"
         "    m_KeepOriginalPositionY: 1\n    m_KeepOriginalPositionXZ: 0\n    m_HeightFromFeet: 0\n    m_Mirror: 0\n";
    o << "  m_EditorCurves: []\n  m_EulerEditorCurves: []\n  m_HasGenericRootTransform: 0\n  m_HasMotionFloatCurves: 0\n"
         "  m_Events: []\n";
    return o.str();
}

std::string unityAssetMeta(const std::string& guid, std::int64_t mainObjectFileId) {
    std::ostringstream o;
    o << "fileFormatVersion: 2\nguid: " << guid << "\nNativeFormatImporter:\n  externalObjects: {}\n  mainObjectFileID: "
      << mainObjectFileId << "\n  userData: \n  assetBundleName: \n  assetBundleVariant: \n";
    return o.str();
}

namespace {

// RuleTile neighbour values (RuleTile.TilingRuleOutput.Neighbor).
constexpr int kThis = 1, kNotThis = 2, kDontCare = 0;

std::string unityRef(std::int64_t fileId, const std::string& guid) {
    return "{fileID: " + std::to_string(fileId) + ", guid: " + guid + ", type: 3}";
}

std::string monoHeader(const std::string& name, const std::string& script) {
    return "%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:\n--- !u!114 &11400000\nMonoBehaviour:\n  m_ObjectHideFlags: 0\n"
           "  m_CorrespondingSourceObject: {fileID: 0}\n  m_PrefabInstance: {fileID: 0}\n  m_PrefabAsset: {fileID: 0}\n"
           "  m_GameObject: {fileID: 0}\n  m_Enabled: 1\n  m_EditorHideFlags: 0\n  m_Script: " + script + "\n  m_Name: " + name +
           "\n  m_EditorClassIdentifier: \n";
}

} // namespace

std::string unityRuleTile(const Tileset& t, const std::string& name, const std::string& spriteStem, const std::string& textureGuid) {
    if (t.scheme == AutotileScheme::Wang16) return {};
    const bool blob = t.scheme == AutotileScheme::Blob47;
    const int n = tileCount(t.scheme);
    auto spriteId = [&](int k) { return unityFileId(textureGuid + "/" + spriteStem + "_" + std::to_string(k)); };
    // Unity's neighbour order: (-1,1) (0,1) (1,1) (-1,0) (1,0) (-1,-1) (0,-1) (1,-1), y up
    // (paint map row y at Unity y = -y). Blob bits: N=1 NE=2 E=4 SE=8 S=16 SW=32 W=64 NW=128.
    struct Pos { int x, y; };
    const Pos pos[8] = {{-1, 1}, {0, 1}, {1, 1}, {-1, 0}, {1, 0}, {-1, -1}, {0, -1}, {1, -1}};
    std::ostringstream o;
    o << monoHeader(name, std::string("{fileID: 11500000, guid: ") + kUnityRuleTileScriptGuid + ", type: 3}");
    int full = n - 1;
    if (blob) {
        const auto& masks = blobMasks();
        full = static_cast<int>(std::find(masks.begin(), masks.end(), std::uint8_t{255}) - masks.begin());
    }
    o << "  m_DefaultSprite: " << unityRef(spriteId(full), textureGuid) << "\n  m_DefaultGameObject: {fileID: 0}\n"
         "  m_DefaultColliderType: 1\n  m_TilingRules:\n";
    for (int k = 0; k < n; ++k) {
        int v[8];
        if (blob) {
            const int m = blobMasks()[static_cast<std::size_t>(k)];
            const bool N = m & 1, E = m & 4, S = m & 16, W = m & 64;
            auto side = [](bool b) { return b ? kThis : kNotThis; };
            auto corner = [&](bool a, bool b, int bit) { return a && b ? ((m & bit) ? kThis : kNotThis) : kDontCare; };
            v[0] = corner(N, W, 128); v[1] = side(N); v[2] = corner(N, E, 2);
            v[3] = side(W);                           v[4] = side(E);
            v[5] = corner(S, W, 32);  v[6] = side(S); v[7] = corner(S, E, 8);
        } else {   // Cardinal16: index = mask, N=1 E=2 S=4 W=8
            auto side = [&](int bit) { return (k & bit) ? kThis : kNotThis; };
            v[0] = v[2] = v[5] = v[7] = kDontCare;
            v[1] = side(1); v[4] = side(2); v[6] = side(4); v[3] = side(8);
        }
        o << "  - m_Id: " << k << "\n    m_Sprites:\n    - " << unityRef(spriteId(k), textureGuid) << "\n"
          << "    m_GameObject: {fileID: 0}\n    m_MinAnimationSpeed: 1\n    m_MaxAnimationSpeed: 1\n    m_PerlinScale: 0.5\n"
             "    m_Output: 0\n    m_ColliderType: 1\n    m_RandomTransform: 0\n    m_Neighbors:\n";
        for (int i = 0; i < 8; ++i) o << "    - " << v[i] << "\n";
        o << "    m_NeighborPositions:\n";
        for (int i = 0; i < 8; ++i) o << "    - {x: " << pos[i].x << ", y: " << pos[i].y << ", z: 0}\n";
        o << "    m_RuleTransform: 0\n";
    }
    return o.str();
}

std::string unityTile(const std::string& name, std::int64_t spriteFileId, const std::string& textureGuid) {
    std::ostringstream o;
    o << monoHeader(name, "{fileID: 13312, guid: 0000000000000000e000000000000000, type: 0}");
    o << "  m_Sprite: " << unityRef(spriteFileId, textureGuid) << "\n  m_Color: {r: 1, g: 1, b: 1, a: 1}\n"
         "  m_Transform:\n    e00: 1\n    e01: 0\n    e02: 0\n    e03: 0\n    e10: 0\n    e11: 1\n    e12: 0\n    e13: 0\n"
         "    e20: 0\n    e21: 0\n    e22: 1\n    e23: 0\n    e30: 0\n    e31: 0\n    e32: 0\n    e33: 1\n"
         "  m_InstancedGameObject: {fileID: 0}\n  m_Flags: 1\n  m_ColliderType: 1\n";
    return o.str();
}

// ------------------------------------------------------------------ Tiled

std::string tiledTileset(const Tileset& t, const std::string& imageFile, const std::string& name, const std::string& terrainName,
                         const std::string& baseName) {
    const auto [cols, rows] = tilesetGrid(t.scheme);
    const int T = t.tileSize;
    const bool hasBase = !t.base.isNull() && t.base.pixel(0, 0).a != 0;
    const char* type = t.scheme == AutotileScheme::Blob47 ? "mixed" : t.scheme == AutotileScheme::Wang16 ? "corner" : "edge";
    std::ostringstream o;
    o << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    // With phases the image holds P x P copies of the grid; Tiled sees one
    // big grid and the Wang set uses the first copy.
    const int P = std::max(1, t.phases), allCols = cols * P;
    o << "<tileset version=\"1.10\" tiledversion=\"1.11.0\" name=\"" << xmlEscape(name) << "\" tilewidth=\"" << T << "\" tileheight=\"" << T
      << "\" tilecount=\"" << allCols * rows * P << "\" columns=\"" << allCols << "\">\n";
    o << " <image source=\"" << xmlEscape(imageFile) << "\" width=\"" << allCols * T << "\" height=\"" << rows * P * T << "\"/>\n";
    o << " <wangsets>\n  <wangset name=\"" << xmlEscape(terrainName) << "\" type=\"" << type << "\" tile=\"-1\">\n";
    o << "   <wangcolor name=\"" << xmlEscape(terrainName) << "\" color=\"#4caf50\" tile=\"-1\" probability=\"1\"/>\n";
    if (hasBase) o << "   <wangcolor name=\"" << xmlEscape(baseName) << "\" color=\"#8d6e63\" tile=\"-1\" probability=\"1\"/>\n";
    const int off = hasBase ? 2 : 0;
    for (int k = 0; k < tileCount(t.scheme); ++k) {
        const TileSamples s = tileSamples(t.scheme, k);
        auto c = [&](int i) { return s[static_cast<std::size_t>(i)] >= 128 ? 1 : off; };
        // Tiled order: top, top-right, right, bottom-right, bottom, bottom-left, left, top-left.
        int w[8] = {c(1), c(2), c(5), c(8), c(7), c(6), c(3), c(0)};
        if (t.scheme == AutotileScheme::Wang16) { w[0] = w[2] = w[4] = w[6] = 0; }
        if (t.scheme == AutotileScheme::Cardinal16) { w[1] = w[3] = w[5] = w[7] = 0; }
        o << "   <wangtile tileid=\"" << (k / cols) * allCols + k % cols << "\" wangid=\"" << w[0];
        for (int i = 1; i < 8; ++i) o << "," << w[i];
        o << "\"/>\n";
    }
    o << "  </wangset>\n </wangsets>\n</tileset>\n";
    return o.str();
}

std::string tiledSpriteSheet(const EngineSheet& sheet, const std::string& imageFile, const std::string& name) {
    if (sheet.frames.empty()) return {};
    const int w = sheet.frames.front().rect.w, h = sheet.frames.front().rect.h;
    if (w < 1 || h < 1 || sheet.width % w != 0 || sheet.height % h != 0) return {};
    const int cols = sheet.width / w, rows = sheet.height / h;
    std::vector<int> tileOf;
    for (const EngineFrame& f : sheet.frames) {
        if (f.rect.w != w || f.rect.h != h || f.rect.x % w != 0 || f.rect.y % h != 0) return {};
        tileOf.push_back((f.rect.y / h) * cols + f.rect.x / w);
    }
    std::ostringstream o;
    o << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    o << "<tileset version=\"1.10\" tiledversion=\"1.11.0\" name=\"" << xmlEscape(name) << "\" tilewidth=\"" << w << "\" tileheight=\"" << h
      << "\" tilecount=\"" << cols * rows << "\" columns=\"" << cols << "\">\n";
    o << " <image source=\"" << xmlEscape(imageFile) << "\" width=\"" << sheet.width << "\" height=\"" << sheet.height << "\"/>\n";
    // Tiled allows one animation per tile: an animation whose first frame
    // already carries one goes on the next free tile of its frames.
    std::vector<bool> used(static_cast<std::size_t>(cols * rows), false);
    for (const EngineAnimation& a : sheet.animations) {
        if (a.frames.empty()) continue;
        int host = -1;
        for (int fi : a.frames) {
            const int t = tileOf[static_cast<std::size_t>(fi)];
            if (!used[static_cast<std::size_t>(t)]) { host = t; break; }
        }
        if (host < 0) continue;
        used[static_cast<std::size_t>(host)] = true;
        o << " <tile id=\"" << host << "\" type=\"" << xmlEscape(a.name) << "\">\n  <properties>\n   <property name=\"animation\" value=\""
          << xmlEscape(a.name) << "\"/>\n  </properties>\n  <animation>\n";
        for (std::size_t i = 0; i < a.frames.size(); ++i)
            o << "   <frame tileid=\"" << tileOf[static_cast<std::size_t>(a.frames[i])] << "\" duration=\""
              << (i < a.durationsMs.size() ? a.durationsMs[i] : 100) << "\"/>\n";
        o << "  </animation>\n </tile>\n";
    }
    o << "</tileset>\n";
    return o.str();
}

} // namespace acestudio
