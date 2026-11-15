// glTF 2.0 / GLB skeletal animation import (Mocap.h, ADR-ANIM-002 stage 3b).
//
// The file is untrusted input. Only what a skeleton's motion needs is read:
// nodes, skins' joint lists, one animation's samplers and the accessors,
// buffer views and buffers those use. Meshes, materials and images are never
// decoded, no extension is followed, and every accessor read is bounds
// checked against its buffer view and buffer before a byte is touched.
//
// Conversion to the BVH-like Motion (positions are what the retargeter uses):
//  - p_j(f): a joint's glTF world position, from the full affine chain
//    (T * R * S of every node from the scene root, ancestors included);
//  - Q_j(f): its world rotation, the product of the rotations alone;
//  - W_j(f) = Q_j(f) * Q_j(rest)^T, the rotation away from the rest pose;
//  - offset_j = p_j(rest) - p_parent(rest) (rest bone, world axes; the root's
//    offset is its rest world position), so restPositions() is the rest pose;
//  - BVH FK gives p_j = p_parent + W_parent * (offset_j + channels), and the
//    local rotation channels hold W_parent^T * W_j as Z, X, Y Euler angles.
// With uniform, unanimated scale and unanimated non-root translation,
// W_parent * offset_j equals p_j - p_parent exactly. Joints where it does not
// (translation or scale animation, non-uniform scale) get position channels
// holding the difference, so the world positions are reproduced in every case.

#include "acestudio/core/Mocap.h"

#include "acestudio/core/FileUtil.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <utility>

namespace acestudio::mocap {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;
using u64 = std::uint64_t;
using i64 = std::int64_t;

constexpr double kPi = 3.14159265358979323846;
constexpr std::uint32_t kGlbMagic = 0x46546C67u;   // "glTF"
constexpr std::uint32_t kChunkJson = 0x4E4F534Au;  // "JSON"
constexpr std::uint32_t kChunkBin = 0x004E4942u;   // "BIN\0"
constexpr int kMaxJsonDepth = 64;
constexpr double kMaxValue = 1e7;
constexpr std::size_t kMaxMotionValues = 32u * 1024u * 1024u;

std::size_t uz(int v) { return static_cast<std::size_t>(v); }
std::size_t uz(u64 v) { return static_cast<std::size_t>(v); }

// ---- Math ---------------------------------------------------------------------------

Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
double length(Vec3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }

struct Quat {
    double x = 0, y = 0, z = 0, w = 1;
};

// Row-major 3x3.
struct M3 {
    double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};

M3 mul(const M3& a, const M3& b) {
    M3 r;
    for (std::size_t i = 0; i < 3; ++i)
        for (std::size_t j = 0; j < 3; ++j) r.m[i * 3 + j] = a.m[i * 3] * b.m[j] + a.m[i * 3 + 1] * b.m[3 + j] + a.m[i * 3 + 2] * b.m[6 + j];
    return r;
}
Vec3 mul(const M3& a, Vec3 v) {
    return {a.m[0] * v.x + a.m[1] * v.y + a.m[2] * v.z, a.m[3] * v.x + a.m[4] * v.y + a.m[5] * v.z,
            a.m[6] * v.x + a.m[7] * v.y + a.m[8] * v.z};
}
M3 transpose(const M3& a) { return M3{{a.m[0], a.m[3], a.m[6], a.m[1], a.m[4], a.m[7], a.m[2], a.m[5], a.m[8]}}; }

Quat normalized(Quat q) {
    const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(n > 1e-12) || !std::isfinite(n)) return {};
    return {q.x / n, q.y / n, q.z / n, q.w / n};
}

M3 fromQuat(Quat q) {
    q = normalized(q);
    const double x = q.x, y = q.y, z = q.z, w = q.w;
    return M3{{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
               2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
               2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
}

Quat slerp(Quat a, Quat b, double s) {
    a = normalized(a);
    b = normalized(b);
    double d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) {   // the shorter way round
        b = {-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    double ka = 1 - s, kb = s;
    if (d < 0.9995) {
        const double th = std::acos(std::clamp(d, -1.0, 1.0));
        const double sn = std::sin(th);
        ka = std::sin((1 - s) * th) / sn;
        kb = std::sin(s * th) / sn;
    }
    return normalized({ka * a.x + kb * b.x, ka * a.y + kb * b.y, ka * a.z + kb * b.z, ka * a.w + kb * b.w});
}

// The rotation of a matrix's columns (Gram-Schmidt; a mirror flips the third
// axis, which no rotation can express: positions stay exact through
// position channels).
M3 orthonormal(const M3& a) {
    Vec3 c0{a.m[0], a.m[3], a.m[6]}, c1{a.m[1], a.m[4], a.m[7]};
    auto unit = [](Vec3 v, Vec3 fallback) {
        const double l = length(v);
        return l > 1e-12 ? Vec3{v.x / l, v.y / l, v.z / l} : fallback;
    };
    c0 = unit(c0, {1, 0, 0});
    const double d = c0.x * c1.x + c0.y * c1.y + c0.z * c1.z;
    c1 = unit(sub(c1, {c0.x * d, c0.y * d, c0.z * d}), std::abs(c0.y) < 0.9 ? Vec3{0, 1, 0} : Vec3{0, 0, 1});
    if (std::abs(c0.x * c1.x + c0.y * c1.y + c0.z * c1.z) > 1e-6) {   // fallback was not orthogonal
        const double e = c0.x * c1.x + c0.y * c1.y + c0.z * c1.z;
        c1 = unit(sub(c1, {c0.x * e, c0.y * e, c0.z * e}), {0, 0, 1});
    }
    const Vec3 c2{c0.y * c1.z - c0.z * c1.y, c0.z * c1.x - c0.x * c1.z, c0.x * c1.y - c0.y * c1.x};
    return M3{{c0.x, c1.x, c2.x, c0.y, c1.y, c2.y, c0.z, c1.z, c2.z}};
}

// r = Rz(z) * Rx(x) * Ry(y), the order of the "Zrotation Xrotation Yrotation"
// channels in jointPositions(); degrees.
void eulerZXY(const M3& r, double& z, double& x, double& y) {
    const double cx = std::hypot(r.m[6], r.m[8]);
    x = std::atan2(r.m[7], cx);
    if (cx > 1e-12) {
        z = std::atan2(-r.m[1], r.m[4]);
        y = std::atan2(-r.m[6], r.m[8]);
    } else {   // gimbal lock: X at +-90 degrees, Y folded into Z
        y = 0;
        z = std::atan2(r.m[3], r.m[0]);
    }
    z *= 180 / kPi;
    x *= 180 / kPi;
    y *= 180 / kPi;
}

struct Affine {
    M3 a;     // rotation * scale (or a node matrix's 3x3)
    Vec3 t;
};
Affine compose(const Affine& p, const Affine& l) { return {mul(p.a, l.a), add(mul(p.a, l.t), p.t)}; }

// ---- Small parsing helpers ---------------------------------------------------------

std::uint32_t le32(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) | (static_cast<std::uint32_t>(b[at + 2]) << 16) |
           (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

// Nesting depth of JSON text (outside strings), checked before parsing so a
// hostile file cannot exhaust the parser's stack.
bool jsonTooDeep(std::string_view s, int maxDepth) {
    int depth = 0;
    bool inString = false, escaped = false;
    for (const char c : s) {
        if (inString) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') inString = true;
        else if (c == '[' || c == '{') {
            if (++depth > maxDepth) return true;
        } else if (c == ']' || c == '}') {
            --depth;
        }
    }
    return false;
}

int base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::optional<std::vector<std::uint8_t>> decodeBase64(std::string_view s) {
    std::size_t n = s.size();
    int pad = 0;
    while (n > 0 && s[n - 1] == '=' && pad < 2) { --n; ++pad; }
    if (n % 4 == 1) return std::nullopt;
    if (pad > 0 && (n + uz(pad)) % 4 != 0) return std::nullopt;
    std::vector<std::uint8_t> out;
    out.reserve(n / 4 * 3 + 3);
    std::uint32_t acc = 0;
    int bits = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const int v = base64Value(s[i]);
        if (v < 0) return std::nullopt;
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xFFu));
        }
    }
    return out;
}

std::optional<std::string> percentDecode(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') { out += s[i]; continue; }
        if (i + 2 >= s.size()) return std::nullopt;
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int h = hex(s[i + 1]), l = hex(s[i + 2]);
        if (h < 0 || l < 0) return std::nullopt;
        out += static_cast<char>(h * 16 + l);
        i += 2;
    }
    return out;
}

fs::path utf8Path(std::string_view s) {
    std::u8string u;
    u.reserve(s.size());
    for (const char c : s) u += static_cast<char8_t>(static_cast<unsigned char>(c));
    return fs::path(u);
}

std::string quote(std::string_view s) {
    std::string out = "'";
    for (const char c : s.substr(0, 120)) out += static_cast<unsigned char>(c) < 0x20 ? '?' : c;
    if (s.size() > 120) out += "...";
    return out + "'";
}

enum class Interp { Linear, Step, Cubic };
enum class PathKind { Translation = 0, Rotation = 1, Scale = 2 };

struct Node {
    std::string name;
    int parent = -1;
    int depth = 0;
    std::vector<int> children;
    bool hasMatrix = false;
    Affine matrix;           // when hasMatrix
    M3 matrixRotation;       // its rotation part
    Vec3 t;
    Quat r;
    Vec3 s{1, 1, 1};
};

struct Track {
    int node = -1;
    PathKind path = PathKind::Rotation;
    Interp interp = Interp::Linear;
    int comps = 3;
    std::vector<double> times;
    std::vector<double> values;   // keys x comps (x3 elements for CUBICSPLINE)
};

struct Buffer {
    bool loaded = false;
    std::vector<std::uint8_t> own;
    std::span<const std::uint8_t> data;
};

int componentSize(i64 ct) {
    switch (ct) {
    case 5120: case 5121: return 1;
    case 5122: case 5123: return 2;
    case 5125: case 5126: return 4;
    default: return 0;
    }
}

const char* componentName(i64 ct) {
    switch (ct) {
    case 5120: return "BYTE";
    case 5121: return "UNSIGNED_BYTE";
    case 5122: return "SHORT";
    case 5123: return "UNSIGNED_SHORT";
    case 5125: return "UNSIGNED_INT";
    case 5126: return "FLOAT";
    default: return "?";
    }
}

class Gltf {
public:
    Gltf(const GltfOptions& opt, fs::path baseDir) : m_opt(opt), m_baseDir(std::move(baseDir)) {}

    bool open(std::span<const std::uint8_t> bytes);
    bool animationNames(std::vector<std::string>& out);
    bool build(Motion& out);
    Error error() const { return Error(m_code, "glTF: " + (m_err.empty() ? std::string("unknown error") : m_err)); }

private:
    bool fail(std::string msg, ErrorCode code = ErrorCode::CorruptData) {
        if (m_err.empty()) {
            m_err = std::move(msg);
            m_code = code;
        }
        return false;
    }
    const json* array(const char* key) const {
        auto it = m_json.find(key);
        return it != m_json.end() && it->is_array() ? &*it : nullptr;
    }
    bool getInt(const json& o, const char* key, const std::string& where, i64 lo, i64 hi, i64& out, bool required);
    bool getNumbers(const json& o, const char* key, const std::string& where, std::size_t n, double* out, bool& present);
    bool parseNodes();
    bool loadBuffer(i64 index, const std::string& where, std::span<const std::uint8_t>& out);
    bool readAccessor(i64 index, const std::string& where, const char* type, int comps, bool allowNormalizedInts, u64 maxCount,
                      std::vector<double>& out);
    bool selectAnimation(int& index);
    bool readTracks(int anim, std::vector<Track>& tracks);
    bool chooseJoints(const std::vector<Track>& tracks, std::vector<int>& joints);

    GltfOptions m_opt;
    fs::path m_baseDir;
    std::string m_err;
    ErrorCode m_code = ErrorCode::CorruptData;
    json m_json;
    std::span<const std::uint8_t> m_bin;
    bool m_hasBin = false;
    std::size_t m_used = 0;   // bytes read so far (file plus external buffers)
    std::vector<Node> m_nodes;
    std::vector<Buffer> m_buffers;
};

bool Gltf::getInt(const json& o, const char* key, const std::string& where, i64 lo, i64 hi, i64& out, bool required) {
    auto it = o.find(key);
    if (it == o.end()) {
        if (required) return fail(where + " has no '" + key + "'");
        return true;
    }
    i64 v = 0;
    if (it->is_number_unsigned()) {
        const u64 u = it->get<u64>();
        if (u > static_cast<u64>(std::numeric_limits<i64>::max())) return fail(where + ": '" + key + "' is out of range");
        v = static_cast<i64>(u);
    } else if (it->is_number_integer()) {
        v = it->get<i64>();
    } else {
        return fail(where + ": '" + key + "' must be an integer");
    }
    if (v < lo || v > hi) return fail(where + ": '" + key + "' = " + std::to_string(v) + " is out of range (" + std::to_string(lo) + ".." + std::to_string(hi) + ")");
    out = v;
    return true;
}

bool Gltf::getNumbers(const json& o, const char* key, const std::string& where, std::size_t n, double* out, bool& present) {
    present = false;
    auto it = o.find(key);
    if (it == o.end()) return true;
    if (!it->is_array() || it->size() != n) return fail(where + ": '" + key + "' must be " + std::to_string(n) + " numbers");
    for (std::size_t i = 0; i < n; ++i) {
        const json& v = (*it)[i];
        if (!v.is_number()) return fail(where + ": '" + key + "' must be " + std::to_string(n) + " numbers");
        const double d = v.get<double>();
        if (!std::isfinite(d) || std::abs(d) > kMaxValue) return fail(where + ": '" + key + "' holds a value that is not finite or too large");
        out[i] = d;
    }
    present = true;
    return true;
}

bool Gltf::open(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > m_opt.maxBytes) return fail("the file is larger than the limit (" + std::to_string(m_opt.maxBytes >> 20) + " MB)");
    m_used = bytes.size();
    std::string_view text;
    if (bytes.size() >= 4 && le32(bytes, 0) == kGlbMagic) {
        if (bytes.size() < 12) return fail("the GLB header is truncated");
        const std::uint32_t version = le32(bytes, 4), total = le32(bytes, 8);
        if (version != 2) return fail("GLB version " + std::to_string(version) + " is not supported (only 2)", ErrorCode::UnsupportedVersion);
        if (total > bytes.size()) return fail("the GLB is truncated: its header says " + std::to_string(total) + " bytes, the file has " + std::to_string(bytes.size()));
        std::size_t pos = 12;
        bool first = true;
        while (pos < total) {
            if (total - pos < 8) return fail("the GLB chunk header at byte " + std::to_string(pos) + " is truncated");
            const std::uint32_t clen = le32(bytes, pos), ctype = le32(bytes, pos + 4);
            pos += 8;
            if (clen > total - pos) {
                return fail("the GLB chunk at byte " + std::to_string(pos - 8) + " says " + std::to_string(clen) + " bytes, but only " +
                            std::to_string(total - pos) + " remain");
            }
            const std::span<const std::uint8_t> chunk = bytes.subspan(pos, clen);
            if (first) {
                if (ctype != kChunkJson) return fail("the first GLB chunk is not JSON");
                text = std::string_view(reinterpret_cast<const char*>(chunk.data()), chunk.size());
                first = false;
            } else if (ctype == kChunkJson) {
                return fail("the GLB has a second JSON chunk");
            } else if (ctype == kChunkBin) {
                if (m_hasBin) return fail("the GLB has a second BIN chunk");
                m_bin = chunk;
                m_hasBin = true;
            }   // other chunk types are skipped, never interpreted
            pos += clen;
        }
        if (first) return fail("the GLB has no JSON chunk");
    } else {
        text = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    }
    if (text.empty()) return fail("the file is empty");
    if (jsonTooDeep(text, kMaxJsonDepth)) return fail("the JSON is nested deeper than " + std::to_string(kMaxJsonDepth) + " levels");
    try {
        m_json = json::parse(text.begin(), text.end(), nullptr, false);
    } catch (const std::exception&) {
        return fail("the JSON could not be read");
    }
    if (m_json.is_discarded() || !m_json.is_object()) return fail("the JSON is not valid (or is not an object)");

    auto asset = m_json.find("asset");
    if (asset == m_json.end() || !asset->is_object()) return fail("there is no 'asset' object (not a glTF file?)");
    auto version = asset->find("version");
    if (version == asset->end() || !version->is_string()) return fail("'asset.version' is missing");
    const std::string ver = version->get<std::string>();
    if (ver.rfind("2.", 0) != 0) return fail("glTF version " + quote(ver) + " is not supported (only 2.x)", ErrorCode::UnsupportedVersion);
    if (auto mv = asset->find("minVersion"); mv != asset->end()) {
        if (!mv->is_string() || mv->get<std::string>() != "2.0")
            return fail("the file needs a newer glTF reader (minVersion " + (mv->is_string() ? quote(mv->get<std::string>()) : std::string("?")) + ")",
                        ErrorCode::UnsupportedVersion);
    }
    if (auto req = m_json.find("extensionsRequired"); req != m_json.end()) {
        if (!req->is_array()) return fail("'extensionsRequired' must be an array");
        if (!req->empty()) {
            std::string names;
            for (const json& e : *req) names += (names.empty() ? "" : ", ") + (e.is_string() ? quote(e.get<std::string>()) : std::string("?"));
            return fail("the file requires extensions that are not supported: " + names +
                            " (export without them, e.g. no Draco or mesh quantization)",
                        ErrorCode::UnsupportedVersion);
        }
    }
    return true;
}

bool Gltf::parseNodes() {
    const json* nodes = array("nodes");
    if (!nodes || nodes->empty()) return fail("the file has no nodes");
    if (nodes->size() > uz(m_opt.maxNodes)) return fail("too many nodes (" + std::to_string(nodes->size()) + ", limit " + std::to_string(m_opt.maxNodes) + ")");
    const i64 n = static_cast<i64>(nodes->size());
    m_nodes.assign(nodes->size(), Node{});
    for (std::size_t i = 0; i < nodes->size(); ++i) {
        const json& o = (*nodes)[i];
        const std::string where = "node " + std::to_string(i);
        if (!o.is_object()) return fail(where + " is not an object");
        Node& nd = m_nodes[i];
        if (auto nm = o.find("name"); nm != o.end() && nm->is_string()) {
            std::string name = nm->get<std::string>().substr(0, 200);
            for (char& c : name)
                if (static_cast<unsigned char>(c) < 0x20) c = '_';
            nd.name = name;
        }
        if (auto ch = o.find("children"); ch != o.end()) {
            if (!ch->is_array()) return fail(where + ": 'children' must be an array");
            for (const json& c : *ch) {
                if (!c.is_number_integer()) return fail(where + ": 'children' must hold node indices");
                const i64 ci = c.get<i64>();
                if (ci < 0 || ci >= n) return fail(where + ": child " + std::to_string(ci) + " does not exist");
                nd.children.push_back(static_cast<int>(ci));
            }
        }
        double m[16] = {}, t[3] = {}, r[4] = {}, s[3] = {};
        bool hasM = false, hasT = false, hasR = false, hasS = false;
        if (!getNumbers(o, "matrix", where, 16, m, hasM) || !getNumbers(o, "translation", where, 3, t, hasT) ||
            !getNumbers(o, "rotation", where, 4, r, hasR) || !getNumbers(o, "scale", where, 3, s, hasS))
            return false;
        if (hasM && (hasT || hasR || hasS)) return fail(where + " has both a matrix and translation/rotation/scale");
        if (hasM) {
            nd.hasMatrix = true;
            // Column-major.
            nd.matrix.a = M3{{m[0], m[4], m[8], m[1], m[5], m[9], m[2], m[6], m[10]}};
            nd.matrix.t = {m[12], m[13], m[14]};
            nd.matrixRotation = orthonormal(nd.matrix.a);
        }
        if (hasT) nd.t = {t[0], t[1], t[2]};
        if (hasR) {
            const double len = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
            if (len < 1e-6) return fail(where + ": 'rotation' is not a unit quaternion");
            nd.r = normalized({r[0], r[1], r[2], r[3]});
        }
        if (hasS) nd.s = {s[0], s[1], s[2]};
    }
    for (std::size_t i = 0; i < m_nodes.size(); ++i)
        for (const int c : m_nodes[i].children) {
            if (c == static_cast<int>(i)) return fail("node " + std::to_string(i) + " is its own child");
            if (m_nodes[uz(c)].parent >= 0) return fail("node " + std::to_string(c) + " has two parents");
            m_nodes[uz(c)].parent = static_cast<int>(i);
        }
    // Depths from the roots; a node never reached is in a cycle.
    std::vector<int> queue;
    for (std::size_t i = 0; i < m_nodes.size(); ++i)
        if (m_nodes[i].parent < 0) queue.push_back(static_cast<int>(i));
    for (std::size_t q = 0; q < queue.size(); ++q)
        for (const int c : m_nodes[uz(queue[q])].children) {
            m_nodes[uz(c)].depth = m_nodes[uz(queue[q])].depth + 1;
            queue.push_back(c);
        }
    if (queue.size() != m_nodes.size()) return fail("the node hierarchy has a cycle");
    return true;
}

bool Gltf::loadBuffer(i64 index, const std::string& where, std::span<const std::uint8_t>& out) {
    const json* buffers = array("buffers");
    if (!buffers || index < 0 || static_cast<u64>(index) >= buffers->size()) return fail(where + ": buffer " + std::to_string(index) + " does not exist");
    if (m_buffers.size() != buffers->size()) m_buffers.assign(buffers->size(), Buffer{});
    Buffer& b = m_buffers[uz(static_cast<u64>(index))];
    if (b.loaded) {
        out = b.data;
        return true;
    }
    const json& o = (*buffers)[uz(static_cast<u64>(index))];
    const std::string bw = "buffer " + std::to_string(index);
    if (!o.is_object()) return fail(bw + " is not an object");
    i64 declared = 0;
    if (!getInt(o, "byteLength", bw, 1, static_cast<i64>(m_opt.maxBytes), declared, true)) return false;
    const std::size_t need = uz(static_cast<u64>(declared));
    std::span<const std::uint8_t> data;
    auto uriIt = o.find("uri");
    if (uriIt == o.end()) {
        if (index != 0 || !m_hasBin) return fail(bw + " has no uri and there is no GLB binary chunk for it");
        data = m_bin;
    } else {
        if (!uriIt->is_string()) return fail(bw + ": 'uri' must be a string");
        const std::string uri = uriIt->get<std::string>();
        if (uri.rfind("data:", 0) == 0) {
            const std::size_t comma = uri.find(',');
            if (comma == std::string::npos || comma < 12 || uri.compare(comma - 7, 7, ";base64") != 0)
                return fail(bw + ": only base64 data: URIs are read");
            auto decoded = decodeBase64(std::string_view(uri).substr(comma + 1));
            if (!decoded) return fail(bw + ": the data: URI is not valid base64");
            b.own = std::move(*decoded);
        } else {
            // A plain relative file name (or path) inside the file's folder.
            if (uri.find(':') != std::string::npos)
                return fail(bw + ": " + quote(uri) + " is a URL or a drive path; only data: URIs and relative file names are read");
            if (m_baseDir.empty()) return fail(bw + ": " + quote(uri) + " is an external file, but there is no folder to read it from");
            auto dec = percentDecode(uri);
            if (!dec || dec->empty()) return fail(bw + ": " + quote(uri) + " is not a valid file name");
            const std::string& name = *dec;
            if (name.find('\\') != std::string::npos || name.find('\0') != std::string::npos || name.find(':') != std::string::npos)
                return fail(bw + ": " + quote(uri) + " is not a plain relative file name");
            if (name.front() == '/') return fail(bw + ": " + quote(uri) + " is an absolute path; only files beside the glTF are read");
            fs::path path = m_baseDir;
            std::size_t s = 0;
            while (s <= name.size()) {
                const std::size_t slash = name.find('/', s);
                const std::string seg = name.substr(s, slash == std::string::npos ? std::string::npos : slash - s);
                if (seg.empty() || seg == "." || seg == "..")
                    return fail(bw + ": " + quote(uri) + " leaves the glTF file's folder (or has an empty part); it is not read");
                path /= utf8Path(seg);
                if (slash == std::string::npos) break;
                s = slash + 1;
            }
            // A link inside the folder must not lead out of it either.
            std::error_code e1, e2;
            const fs::path base = fs::weakly_canonical(m_baseDir, e1), real = fs::weakly_canonical(path, e2);
            if (e1 || e2) return fail(bw + ": " + quote(uri) + " could not be resolved", ErrorCode::NotFound);
            const fs::path rel = real.lexically_relative(base);
            if (rel.empty() || *rel.begin() == "..") return fail(bw + ": " + quote(uri) + " resolves outside the glTF file's folder; it is not read");
            if (m_used >= m_opt.maxBytes) return fail(bw + ": the file and its buffers are larger than the limit");
            auto bytes = readFileBytes(path, m_opt.maxBytes - m_used);
            if (!bytes) return fail(bw + " (" + quote(uri) + "): " + bytes.error().message, bytes.error().code);
            b.own = std::move(bytes).value();
            m_used += b.own.size();
        }
        data = std::span<const std::uint8_t>(b.own);
    }
    if (data.size() < need)
        return fail(bw + " declares " + std::to_string(need) + " bytes but holds " + std::to_string(data.size()));
    b.data = data.first(need);
    b.loaded = true;
    out = b.data;
    return true;
}

// Reads an accessor as doubles (count x comps), normalising integer
// components. Every offset is checked before a byte is read.
bool Gltf::readAccessor(i64 index, const std::string& where, const char* type, int comps, bool allowNormalizedInts, u64 maxCount,
                        std::vector<double>& out) {
    const json* accessors = array("accessors");
    if (!accessors || index < 0 || static_cast<u64>(index) >= accessors->size())
        return fail(where + ": accessor " + std::to_string(index) + " does not exist");
    const json& a = (*accessors)[uz(static_cast<u64>(index))];
    const std::string aw = where + ", accessor " + std::to_string(index);
    if (!a.is_object()) return fail(aw + " is not an object");
    if (a.contains("sparse")) return fail(aw + " is sparse; sparse accessors are not supported", ErrorCode::UnsupportedVersion);
    i64 ct = 0, count = 0, accOffset = 0;
    if (!getInt(a, "componentType", aw, 0, 1 << 20, ct, true)) return false;
    const int csize = componentSize(ct);
    if (csize == 0) return fail(aw + ": componentType " + std::to_string(ct) + " is not a glTF component type");
    bool normalized = false;
    if (auto nz = a.find("normalized"); nz != a.end()) {
        if (!nz->is_boolean()) return fail(aw + ": 'normalized' must be true or false");
        normalized = nz->get<bool>();
    }
    if (normalized && (ct == 5125 || ct == 5126)) return fail(aw + ": " + componentName(ct) + " cannot be normalized");
    const bool isFloat = ct == 5126;
    if (!isFloat && !(allowNormalizedInts && ct != 5125 && normalized))
        return fail(aw + ": componentType " + componentName(ct) + (normalized ? "" : " (not normalized)") + " is not allowed here");
    auto ty = a.find("type");
    if (ty == a.end() || !ty->is_string()) return fail(aw + " has no 'type'");
    if (ty->get<std::string>() != type) return fail(aw + ": type " + quote(ty->get<std::string>()) + " where " + type + " is required");
    if (!getInt(a, "count", aw, 1, static_cast<i64>(maxCount), count, true)) return false;
    const u64 n = static_cast<u64>(count);
    const u64 elem = static_cast<u64>(csize) * static_cast<u64>(comps);
    out.assign(uz(n * static_cast<u64>(comps)), 0.0);
    auto viewIt = a.find("bufferView");
    if (viewIt == a.end()) return true;   // all zeros (glTF)
    i64 viewIndex = 0;
    if (!getInt(a, "bufferView", aw, 0, std::numeric_limits<int>::max(), viewIndex, true)) return false;
    if (!getInt(a, "byteOffset", aw, 0, i64{1} << 53, accOffset, false)) return false;
    const json* views = array("bufferViews");
    if (!views || static_cast<u64>(viewIndex) >= views->size()) return fail(aw + ": bufferView " + std::to_string(viewIndex) + " does not exist");
    const json& v = (*views)[uz(static_cast<u64>(viewIndex))];
    const std::string vw = aw + ", bufferView " + std::to_string(viewIndex);
    if (!v.is_object()) return fail(vw + " is not an object");
    i64 buffer = 0, viewOffset = 0, viewLength = 0, stride = 0;
    if (!getInt(v, "buffer", vw, 0, std::numeric_limits<int>::max(), buffer, true) || !getInt(v, "byteOffset", vw, 0, i64{1} << 53, viewOffset, false) ||
        !getInt(v, "byteLength", vw, 1, i64{1} << 53, viewLength, true) || !getInt(v, "byteStride", vw, 4, 252, stride, false))
        return false;
    if (stride % 4 != 0) return fail(vw + ": byteStride " + std::to_string(stride) + " is not a multiple of 4");
    const u64 step = stride > 0 ? static_cast<u64>(stride) : elem;
    if (step < elem) return fail(vw + ": byteStride " + std::to_string(stride) + " is smaller than one element (" + std::to_string(elem) + " bytes)");
    // accessor.byteOffset + (count - 1) * stride + elementSize <= bufferView.byteLength
    // (count <= maxCount and stride <= 252, so the product cannot overflow).
    const u64 vlen = static_cast<u64>(viewLength), aoff = static_cast<u64>(accOffset);
    const u64 span = (n - 1) * step + elem;
    if (aoff > vlen || span > vlen - aoff)
        return fail(aw + " reads past the end of its bufferView (needs " + std::to_string(span) + " bytes from offset " + std::to_string(aoff) +
                    ", the view has " + std::to_string(vlen) + ")");
    std::span<const std::uint8_t> data;
    if (!loadBuffer(buffer, vw, data)) return false;
    const u64 voff = static_cast<u64>(viewOffset);
    if (vlen > data.size() || voff > data.size() - vlen)
        return fail(vw + " lies outside buffer " + std::to_string(buffer) + " (offset " + std::to_string(voff) + " + " + std::to_string(vlen) +
                    " bytes, the buffer has " + std::to_string(data.size()) + ")");
    const std::uint8_t* base = data.data() + voff + aoff;
    for (u64 i = 0; i < n; ++i)
        for (int c = 0; c < comps; ++c) {
            const std::uint8_t* p = base + i * step + static_cast<u64>(c) * static_cast<u64>(csize);
            double val = 0;
            switch (ct) {
            case 5126: {
                const std::uint32_t bits = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                                           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
                float f = 0;
                std::memcpy(&f, &bits, sizeof f);
                val = static_cast<double>(f);
                break;
            }
            case 5120: val = std::max(static_cast<double>(static_cast<std::int8_t>(p[0])) / 127.0, -1.0); break;
            case 5121: val = static_cast<double>(p[0]) / 255.0; break;
            case 5122: {
                const auto u = static_cast<std::uint16_t>(p[0] | (p[1] << 8));
                val = std::max(static_cast<double>(static_cast<std::int16_t>(u)) / 32767.0, -1.0);
                break;
            }
            case 5123: val = static_cast<double>(static_cast<std::uint16_t>(p[0] | (p[1] << 8))) / 65535.0; break;
            default: break;
            }
            if (!std::isfinite(val) || std::abs(val) > kMaxValue) return fail(aw + ": element " + std::to_string(i) + " is not finite or too large");
            out[uz(i * static_cast<u64>(comps) + static_cast<u64>(c))] = val;
        }
    return true;
}

bool Gltf::animationNames(std::vector<std::string>& out) {
    out.clear();
    const json* anims = array("animations");
    if (!anims) return true;
    for (std::size_t i = 0; i < anims->size(); ++i) {
        const json& a = (*anims)[i];
        std::string name;
        if (a.is_object())
            if (auto nm = a.find("name"); nm != a.end() && nm->is_string()) name = nm->get<std::string>().substr(0, 200);
        for (char& c : name)
            if (static_cast<unsigned char>(c) < 0x20) c = '_';
        out.push_back(name.empty() ? "Animation " + std::to_string(i) : name);
    }
    return true;
}

bool Gltf::selectAnimation(int& index) {
    std::vector<std::string> names;
    animationNames(names);
    if (names.empty()) return fail("the file has no animations (only a skeleton or a mesh)", ErrorCode::InvalidArgument);
    auto list = [&]() {
        std::string s;
        for (std::size_t i = 0; i < names.size() && i < 20; ++i) s += (i ? ", " : "") + std::to_string(i) + " " + quote(names[i]);
        return s + (names.size() > 20 ? ", ..." : "");
    };
    if (m_opt.animationIndex >= 0) {
        if (uz(m_opt.animationIndex) >= names.size())
            return fail("there is no animation " + std::to_string(m_opt.animationIndex) + " (the file has: " + list() + ")", ErrorCode::InvalidArgument);
        index = m_opt.animationIndex;
        return true;
    }
    if (m_opt.animation.empty()) {
        index = 0;
        return true;
    }
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i] == m_opt.animation) {
            index = static_cast<int>(i);
            return true;
        }
    const std::string& s = m_opt.animation;
    if (s.size() <= 6 && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        const std::size_t i = uz(std::stoi(s));
        if (i < names.size()) {
            index = static_cast<int>(i);
            return true;
        }
    }
    return fail("there is no animation " + quote(s) + " (the file has: " + list() + ")", ErrorCode::InvalidArgument);
}

bool Gltf::readTracks(int anim, std::vector<Track>& tracks) {
    const json& a = (*array("animations"))[uz(anim)];
    const std::string aw = "animation " + std::to_string(anim);
    if (!a.is_object()) return fail(aw + " is not an object");
    auto chIt = a.find("channels");
    auto smIt = a.find("samplers");
    if (chIt == a.end() || !chIt->is_array() || smIt == a.end() || !smIt->is_array()) return fail(aw + " needs 'channels' and 'samplers' arrays");
    if (chIt->size() > uz(m_opt.maxChannels)) return fail(aw + " has too many channels (" + std::to_string(chIt->size()) + ")");
    std::set<std::pair<int, int>> seen;
    for (std::size_t c = 0; c < chIt->size(); ++c) {
        const json& ch = (*chIt)[c];
        std::string cw = aw + ", channel " + std::to_string(c);
        if (!ch.is_object()) return fail(cw + " is not an object");
        auto tg = ch.find("target");
        if (tg == ch.end() || !tg->is_object()) return fail(cw + " has no target");
        auto pathIt = tg->find("path");
        if (pathIt == tg->end() || !pathIt->is_string()) return fail(cw + " has no target path");
        const std::string path = pathIt->get<std::string>();
        PathKind kind;
        if (path == "translation") kind = PathKind::Translation;
        else if (path == "rotation") kind = PathKind::Rotation;
        else if (path == "scale") kind = PathKind::Scale;
        else continue;   // morph weights, or a path only an extension defines: ignored
        if (tg->find("node") == tg->end()) continue;   // no target node (an extension's): ignored
        i64 node = 0, sampler = 0;
        if (!getInt(*tg, "node", cw, 0, static_cast<i64>(m_nodes.size()) - 1, node, true)) return false;
        if (!getInt(ch, "sampler", cw, 0, static_cast<i64>(smIt->size()) - 1, sampler, true)) return false;
        const Node& nd = m_nodes[uz(static_cast<u64>(node))];
        cw += " (" + (nd.name.empty() ? "node " + std::to_string(node) : quote(nd.name)) + " " + path + ")";
        if (nd.hasMatrix) return fail(cw + ": the node has a matrix and is animated, which glTF does not allow");
        if (!seen.insert({static_cast<int>(node), static_cast<int>(kind)}).second) return fail(cw + ": a second channel for the same node and path");
        const json& sm = (*smIt)[uz(static_cast<u64>(sampler))];
        if (!sm.is_object()) return fail(cw + ": sampler " + std::to_string(sampler) + " is not an object");
        Track tr;
        tr.node = static_cast<int>(node);
        tr.path = kind;
        tr.comps = kind == PathKind::Rotation ? 4 : 3;
        if (auto ip = sm.find("interpolation"); ip != sm.end()) {
            const std::string s = ip->is_string() ? ip->get<std::string>() : std::string();
            if (s == "LINEAR") tr.interp = Interp::Linear;
            else if (s == "STEP") tr.interp = Interp::Step;
            else if (s == "CUBICSPLINE") tr.interp = Interp::Cubic;
            else return fail(cw + ": interpolation " + quote(s) + " is not LINEAR, STEP or CUBICSPLINE");
        }
        i64 input = 0, output = 0;
        if (!getInt(sm, "input", cw, 0, std::numeric_limits<int>::max(), input, true) ||
            !getInt(sm, "output", cw, 0, std::numeric_limits<int>::max(), output, true))
            return false;
        if (!readAccessor(input, cw + " input", "SCALAR", 1, false, m_opt.maxKeyframes, tr.times)) return false;
        for (std::size_t k = 1; k < tr.times.size(); ++k)
            if (tr.times[k] < tr.times[k - 1]) return fail(cw + " input: the keyframe times go backwards at keyframe " + std::to_string(k));
        const std::size_t keys = tr.times.size();
        const std::size_t per = tr.interp == Interp::Cubic ? 3 : 1;
        if (!readAccessor(output, cw + " output", kind == PathKind::Rotation ? "VEC4" : "VEC3", tr.comps, kind == PathKind::Rotation,
                          3 * static_cast<u64>(m_opt.maxKeyframes), tr.values))
            return false;
        if (tr.values.size() != keys * per * uz(tr.comps)) {
            return fail(cw + " output: " + std::to_string(tr.values.size() / uz(tr.comps)) + " values for " + std::to_string(keys) + " keyframes (" +
                        (per == 3 ? "CUBICSPLINE needs 3 per keyframe" : "needs 1 per keyframe") + ")");
        }
        if (kind == PathKind::Rotation)
            for (std::size_t k = 0; k < keys; ++k) {
                const double* q = &tr.values[(k * per + (per == 3 ? 1 : 0)) * 4];
                if (std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]) < 1e-6)
                    return fail(cw + " output: keyframe " + std::to_string(k) + " is not a rotation (zero quaternion)");
            }
        tracks.push_back(std::move(tr));
    }
    if (tracks.empty()) return fail(aw + " moves no node (it has no translation, rotation or scale channels)");
    return true;
}

// Lowest common ancestor of two nodes (-1 if in different trees).
int commonAncestor(const std::vector<Node>& nodes, int a, int b) {
    while (a >= 0 && b >= 0 && a != b) {
        if (nodes[uz(a)].depth >= nodes[uz(b)].depth) a = nodes[uz(a)].parent;
        else b = nodes[uz(b)].parent;
    }
    return a == b ? a : -1;
}

bool Gltf::chooseJoints(const std::vector<Track>& tracks, std::vector<int>& joints) {
    std::vector<char> in(m_nodes.size(), 0);
    std::vector<char> animated(m_nodes.size(), 0);
    for (const Track& t : tracks) animated[uz(t.node)] = 1;
    int root = -1;
    const json* skins = array("skins");
    if (skins && !skins->empty()) {
        // The first skin the animation moves, else the first skin.
        std::size_t chosen = 0;
        std::vector<std::vector<int>> lists(skins->size());
        for (std::size_t s = 0; s < skins->size(); ++s) {
            const json& sk = (*skins)[s];
            const std::string sw = "skin " + std::to_string(s);
            if (!sk.is_object()) return fail(sw + " is not an object");
            auto js = sk.find("joints");
            if (js == sk.end() || !js->is_array() || js->empty()) return fail(sw + " has no joints");
            if (js->size() > uz(m_opt.maxJoints)) return fail(sw + " has too many joints (" + std::to_string(js->size()) + ", limit " + std::to_string(m_opt.maxJoints) + ")");
            for (const json& j : *js) {
                if (!j.is_number_integer() || j.get<i64>() < 0 || j.get<i64>() >= static_cast<i64>(m_nodes.size()))
                    return fail(sw + ": a joint is not a node index");
                lists[s].push_back(static_cast<int>(j.get<i64>()));
            }
        }
        for (std::size_t s = 0; s < lists.size(); ++s) {
            bool moves = false;
            for (const int j : lists[s]) moves = moves || animated[uz(j)];
            if (moves) {
                chosen = s;
                break;
            }
        }
        for (const int j : lists[chosen]) in[uz(j)] = 1;
        // The skin's roots, joined at their common ancestor (with the nodes between).
        for (const int j : lists[chosen]) {
            if (m_nodes[uz(j)].parent >= 0 && in[uz(m_nodes[uz(j)].parent)]) continue;
            root = root < 0 ? j : commonAncestor(m_nodes, root, j);
            if (root < 0) return fail("skin " + std::to_string(chosen) + "'s joints are in separate node trees");
        }
        for (const int j : lists[chosen])
            for (int k = j; k != root && k >= 0; k = m_nodes[uz(k)].parent) in[uz(k)] = 1;
        in[uz(root)] = 1;
    } else {
        // No skin: the subtree under the animated nodes.
        for (const Track& t : tracks) {
            root = root < 0 ? t.node : commonAncestor(m_nodes, root, t.node);
            if (root < 0) return fail("the animated nodes are in separate node trees and there is no skin to say which is the skeleton");
        }
        std::vector<int> stack{root};
        while (!stack.empty()) {
            const int k = stack.back();
            stack.pop_back();
            in[uz(k)] = 1;
            for (const int c : m_nodes[uz(k)].children) stack.push_back(c);
        }
    }
    // Parent-first order: depth-first from the root, children in file order.
    std::vector<int> stack{root};
    while (!stack.empty()) {
        const int k = stack.back();
        stack.pop_back();
        joints.push_back(k);
        if (joints.size() > uz(m_opt.maxJoints)) return fail("the skeleton has too many joints (limit " + std::to_string(m_opt.maxJoints) + ")");
        const auto& ch = m_nodes[uz(k)].children;
        for (auto it = ch.rbegin(); it != ch.rend(); ++it)
            if (in[uz(*it)]) stack.push_back(*it);
    }
    return true;
}

void sampleTrack(const Track& tr, double t, double* out) {
    const std::size_t n = tr.times.size(), c = uz(tr.comps);
    const bool cubic = tr.interp == Interp::Cubic;
    auto value = [&](std::size_t k) { return &tr.values[(cubic ? 3 * k + 1 : k) * c]; };
    auto copy = [&](const double* v) {
        for (std::size_t i = 0; i < c; ++i) out[i] = v[i];
    };
    if (n == 1 || t <= tr.times[0]) return copy(value(0));
    if (t >= tr.times[n - 1]) return copy(value(n - 1));
    const std::size_t k = uz(static_cast<u64>(std::upper_bound(tr.times.begin(), tr.times.end(), t) - tr.times.begin())) - 1;
    const double td = tr.times[k + 1] - tr.times[k];
    if (!(td > 0)) return copy(value(k + 1));
    const double s = (t - tr.times[k]) / td;
    if (tr.interp == Interp::Step) return copy(value(k));
    if (tr.interp == Interp::Linear) {
        const double *a = value(k), *b = value(k + 1);
        if (tr.path == PathKind::Rotation) {
            const Quat q = slerp({a[0], a[1], a[2], a[3]}, {b[0], b[1], b[2], b[3]}, s);
            out[0] = q.x;
            out[1] = q.y;
            out[2] = q.z;
            out[3] = q.w;
        } else {
            for (std::size_t i = 0; i < c; ++i) out[i] = a[i] + (b[i] - a[i]) * s;
        }
        return;
    }
    // CUBICSPLINE: elements are (in-tangent, value, out-tangent) per keyframe.
    const double* v0 = &tr.values[(3 * k + 1) * c];
    const double* b0 = &tr.values[(3 * k + 2) * c];
    const double* a1 = &tr.values[(3 * (k + 1)) * c];
    const double* v1 = &tr.values[(3 * (k + 1) + 1) * c];
    const double s2 = s * s, s3 = s2 * s;
    const double h00 = 2 * s3 - 3 * s2 + 1, h10 = s3 - 2 * s2 + s, h01 = -2 * s3 + 3 * s2, h11 = s3 - s2;
    for (std::size_t i = 0; i < c; ++i) out[i] = h00 * v0[i] + h10 * td * b0[i] + h01 * v1[i] + h11 * td * a1[i];
    if (tr.path == PathKind::Rotation) {
        const Quat q = normalized({out[0], out[1], out[2], out[3]});
        out[0] = q.x;
        out[1] = q.y;
        out[2] = q.z;
        out[3] = q.w;
    }
}

bool Gltf::build(Motion& m) {
    if (!(m_opt.fps >= 1 && m_opt.fps <= 240)) return fail("the sampling rate must be 1..240 frames per second", ErrorCode::InvalidArgument);
    int anim = 0;
    if (!selectAnimation(anim) || !parseNodes()) return false;
    std::vector<Track> tracks;
    if (!readTracks(anim, tracks)) return false;
    std::vector<int> joints;
    if (!chooseJoints(tracks, joints)) return false;

    // Nodes whose transforms matter: the joints and everything above them.
    std::vector<char> relevant(m_nodes.size(), 0);
    for (const int j : joints)
        for (int k = j; k >= 0 && !relevant[uz(k)]; k = m_nodes[uz(k)].parent) relevant[uz(k)] = 1;
    std::vector<int> order;
    for (std::size_t i = 0; i < m_nodes.size(); ++i)
        if (relevant[i]) order.push_back(static_cast<int>(i));
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return m_nodes[uz(a)].depth < m_nodes[uz(b)].depth; });
    std::vector<std::array<int, 3>> trackOf(m_nodes.size(), {-1, -1, -1});
    for (std::size_t t = 0; t < tracks.size(); ++t) trackOf[uz(tracks[t].node)][static_cast<std::size_t>(tracks[t].path)] = static_cast<int>(t);

    // Time range and frames.
    double t0 = std::numeric_limits<double>::max(), t1 = std::numeric_limits<double>::lowest();
    for (const Track& t : tracks) {
        t0 = std::min(t0, t.times.front());
        t1 = std::max(t1, t.times.back());
    }
    const double span = t1 - t0;
    if (!std::isfinite(span) || span > 1e6) return fail("the animation's time range is not usable");
    const double framesD = span < 1e-9 ? 1.0 : std::max(2.0, std::round(span * m_opt.fps) + 1);
    if (framesD > m_opt.maxFrames)
        return fail("the animation is too long (" + std::to_string(static_cast<long long>(framesD)) + " frames at " + std::to_string(m_opt.fps) +
                    " fps, limit " + std::to_string(m_opt.maxFrames) + ")");
    const int frames = static_cast<int>(framesD);
    const double frameTime = frames > 1 ? span / (frames - 1) : 1.0 / m_opt.fps;

    // World transforms of the relevant nodes at a time (nullopt = rest pose).
    std::vector<Affine> world(m_nodes.size());
    std::vector<M3> worldRot(m_nodes.size());
    auto pose = [&](std::optional<double> time) {
        for (const int k : order) {
            const Node& nd = m_nodes[uz(k)];
            Affine local;
            M3 rot;
            if (nd.hasMatrix) {
                local = nd.matrix;
                rot = nd.matrixRotation;
            } else {
                Vec3 t = nd.t, s = nd.s;
                Quat r = nd.r;
                if (time) {
                    double v[4];
                    const auto& tk = trackOf[uz(k)];
                    if (tk[0] >= 0) { sampleTrack(tracks[uz(tk[0])], *time, v); t = {v[0], v[1], v[2]}; }
                    if (tk[1] >= 0) { sampleTrack(tracks[uz(tk[1])], *time, v); r = {v[0], v[1], v[2], v[3]}; }
                    if (tk[2] >= 0) { sampleTrack(tracks[uz(tk[2])], *time, v); s = {v[0], v[1], v[2]}; }
                }
                rot = fromQuat(r);
                local.a = rot;
                for (std::size_t row = 0; row < 3; ++row) {
                    local.a.m[row * 3] *= s.x;
                    local.a.m[row * 3 + 1] *= s.y;
                    local.a.m[row * 3 + 2] *= s.z;
                }
                local.t = t;
            }
            const int p = nd.parent;
            world[uz(k)] = p >= 0 ? compose(world[uz(p)], local) : local;
            worldRot[uz(k)] = p >= 0 ? mul(worldRot[uz(p)], rot) : rot;
        }
    };

    // The joints in the Motion.
    const std::size_t nj = joints.size();
    std::vector<int> nodeToJoint(m_nodes.size(), -1);
    for (std::size_t i = 0; i < nj; ++i) nodeToJoint[uz(joints[i])] = static_cast<int>(i);
    m.joints.assign(nj, Joint{});
    std::set<std::string> names;
    for (std::size_t i = 0; i < nj; ++i) {
        const int k = joints[i];
        std::string name = m_nodes[uz(k)].name.empty() ? "node" + std::to_string(k) : m_nodes[uz(k)].name;
        if (!names.insert(name).second) {
            name += "_" + std::to_string(k);
            names.insert(name);
        }
        m.joints[i].name = name;
        m.joints[i].parent = i == 0 ? -1 : nodeToJoint[uz(m_nodes[uz(k)].parent)];
    }

    pose(std::nullopt);
    std::vector<Vec3> restPos(nj);
    std::vector<M3> restRotT(nj);
    for (std::size_t i = 0; i < nj; ++i) {
        restPos[i] = world[uz(joints[i])].t;
        restRotT[i] = transpose(worldRot[uz(joints[i])]);
    }
    double extent = 0;
    for (std::size_t i = 0; i < nj; ++i) {
        const int p = m.joints[i].parent;
        m.joints[i].offset = p < 0 ? restPos[i] : sub(restPos[i], restPos[uz(p)]);
        extent = std::max(extent, length(m.joints[i].offset));
    }
    const double tol = std::max(1e-12, 1e-7 * extent);

    // Per frame: positions, rotations away from rest, and what is left for
    // position channels (pass 1 decides who needs them, pass 2 fills values).
    std::vector<Vec3> pos(nj);
    std::vector<M3> w(nj);
    auto frameAt = [&](int f) {
        pose(f == frames - 1 ? t1 : t0 + f * frameTime);
        for (std::size_t i = 0; i < nj; ++i) {
            pos[i] = world[uz(joints[i])].t;
            w[i] = mul(worldRot[uz(joints[i])], restRotT[i]);
        }
    };
    auto residual = [&](std::size_t i) {
        const int p = m.joints[i].parent;
        if (p < 0) return sub(pos[i], m.joints[i].offset);
        return sub(mul(transpose(w[uz(p)]), sub(pos[i], pos[uz(p)])), m.joints[i].offset);
    };
    std::vector<char> needsPos(nj, 0);
    needsPos[0] = 1;
    for (int f = 0; f < frames; ++f) {
        frameAt(f);
        for (std::size_t i = 1; i < nj; ++i) {
            if (needsPos[i]) continue;
            const Vec3 d = residual(i);
            if (std::max({std::abs(d.x), std::abs(d.y), std::abs(d.z)}) > tol) needsPos[i] = 1;
        }
    }
    int channels = 0;
    for (std::size_t i = 0; i < nj; ++i) {
        Joint& j = m.joints[i];
        j.firstChannel = channels;
        if (needsPos[i]) j.channels = {Channel::Xposition, Channel::Yposition, Channel::Zposition};
        j.channels.insert(j.channels.end(), {Channel::Zrotation, Channel::Xrotation, Channel::Yrotation});
        channels += static_cast<int>(j.channels.size());
    }
    if (static_cast<u64>(frames) * static_cast<u64>(channels) > kMaxMotionValues) return fail("too much motion data (frames x channels)");
    m.frameCount = frames;
    m.frameTime = frameTime;
    m.channelCount = channels;
    m.values.assign(uz(frames) * uz(channels), 0.0);
    for (int f = 0; f < frames; ++f) {
        frameAt(f);
        double* row = &m.values[uz(f) * uz(channels)];
        for (std::size_t i = 0; i < nj; ++i) {
            double* v = row + m.joints[i].firstChannel;
            if (needsPos[i]) {
                const Vec3 d = residual(i);
                *v++ = d.x;
                *v++ = d.y;
                *v++ = d.z;
            }
            const int p = m.joints[i].parent;
            const M3 local = p < 0 ? w[i] : mul(transpose(w[uz(p)]), w[i]);
            eulerZXY(local, v[0], v[1], v[2]);
        }
    }
    return true;
}

std::string lowerExtension(const fs::path& p) {
    std::string ext = p.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

} // namespace

Result<Motion> parseGltf(std::span<const std::uint8_t> bytes, const std::filesystem::path& baseDir, const GltfOptions& opt) {
    Gltf g(opt, baseDir);
    Motion m;
    try {
        if (!g.open(bytes) || !g.build(m)) return g.error();
    } catch (const std::exception& e) {   // a JSON type the checks above missed, or out of memory
        return Error(ErrorCode::CorruptData, std::string("glTF: the file could not be read (") + e.what() + ")");
    }
    return m;
}

Result<std::vector<std::string>> gltfAnimationNames(std::span<const std::uint8_t> bytes, const GltfOptions& opt) {
    Gltf g(opt, {});
    std::vector<std::string> names;
    try {
        if (!g.open(bytes) || !g.animationNames(names)) return g.error();
    } catch (const std::exception& e) {
        return Error(ErrorCode::CorruptData, std::string("glTF: the file could not be read (") + e.what() + ")");
    }
    return names;
}

Result<Motion> loadMotion(const std::filesystem::path& path, const GltfOptions& opt) {
    const std::string ext = lowerExtension(path);
    if (ext == ".bvh") {
        BvhLimits lim;
        lim.maxBytes = opt.maxBytes;
        lim.maxJoints = opt.maxJoints;
        return loadBvh(path, lim);
    }
    if (ext == ".glb" || ext == ".gltf") {
        auto bytes = readFileBytes(path, opt.maxBytes);
        if (!bytes) return bytes.error();
        return parseGltf(bytes.value(), path.parent_path(), opt);
    }
    return Error(ErrorCode::InvalidArgument, "motion files must be .bvh, .glb or .gltf (FBX: import it into Blender and export glTF or BVH)");
}

Result<std::vector<std::string>> motionAnimationNames(const std::filesystem::path& path, const GltfOptions& opt) {
    const std::string ext = lowerExtension(path);
    if (ext == ".bvh") return std::vector<std::string>{};
    if (ext == ".glb" || ext == ".gltf") {
        auto bytes = readFileBytes(path, opt.maxBytes);
        if (!bytes) return bytes.error();
        return gltfAnimationNames(bytes.value(), opt);
    }
    return Error(ErrorCode::InvalidArgument, "motion files must be .bvh, .glb or .gltf");
}

} // namespace acestudio::mocap
