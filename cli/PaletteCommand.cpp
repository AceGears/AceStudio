// acestudio-cli palette: show and check a project's palette, lock or unlock
// it, and conform every cel to it (ADR-ANIM-006).

#include "CliCommon.h"
#include "ProjectArgs.h"

#include "acestudio/core/Editor.h"

namespace acestudio::cli {

namespace {

json paletteJson(const Document& d) {
    json colors = json::array();
    for (const Rgba8& c : d.palette().colors()) colors.push_back(c.toHex(true));
    return {{"name", d.palette().name()}, {"colors", colors}, {"locked", d.paletteLocked()}, {"offPalettePixels", d.offPalettePixels()}};
}

std::string summary(const Document& d) {
    return "'" + d.palette().name() + "': " + std::to_string(d.palette().size()) + " colours, " +
           (d.paletteLocked() ? "locked" : "not locked") + ", " + std::to_string(d.offPalettePixels()) +
           " pixels off the palette\n";
}

// show: always succeeds; check: fails (exit 1) when any pixel is off the palette.
int paletteShow(const Args& a, Output& out, bool check) {
    const std::string cmd = check ? "palette check" : "palette show";
    if (a.positional.size() != 3) return out.fail(kUsage, cmd + ": expected one project file");
    auto doc = loadProject(a.positional[2]);
    if (!doc) return out.fail(doc.error());
    const std::size_t off = doc.value().offPalettePixels();
    if (check && off > 0)
        return out.fail(kUsage, a.positional[2] + ": " + std::to_string(off) + " pixels are not palette colours (palette conform fixes them)");
    return out.success(paletteJson(doc.value()), a.positional[2] + ": " + summary(doc.value()));
}

// lock / unlock / conform, written to --out.
int paletteEdit(const Args& a, Output& out, const std::string& sub) {
    const std::string cmd = "palette " + sub;
    if (a.positional.size() != 3) return out.fail(kUsage, cmd + ": expected one project file");
    const auto outPath = a.opt("out");
    if (!outPath) return out.fail(kUsage, cmd + ": --out is required");
    auto doc = loadProject(a.positional[2]);
    if (!doc) return out.fail(doc.error());
    Editor ed(std::move(doc).value());
    std::size_t changed = 0;
    if (sub == "conform") {
        auto r = ed.conformToPalette();
        if (!r) return out.fail(r.error());
        changed = r.value();
    } else if (auto st = ed.setPaletteLocked(sub == "lock"); !st) {
        return out.fail(st.error());
    }
    if (int c = writeProject(a, out, ed.document(), *outPath); c != kOk) return c;
    json j = paletteJson(ed.document());
    j["path"] = *outPath;
    if (sub == "conform") j["changedPixels"] = changed;
    const std::string what = sub == "conform" ? "Conformed " + std::to_string(changed) + " pixels; "
                             : sub == "lock"  ? "Locked the palette; "
                                              : "Unlocked the palette; ";
    return out.success(j, what + summary(ed.document()) + "Wrote " + *outPath + "\n");
}

} // namespace

int cmdPalette(const Args& a, Output& out) {
    const std::string sub = a.positional.size() > 1 ? a.positional[1] : "";
    if (sub == "show") return paletteShow(a, out, false);
    if (sub == "check") return paletteShow(a, out, true);
    if (sub == "lock" || sub == "unlock" || sub == "conform") return paletteEdit(a, out, sub);
    return out.fail(kUsage, "palette: expected show, check, lock, unlock or conform");
}

} // namespace acestudio::cli
