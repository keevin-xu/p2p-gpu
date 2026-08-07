// Scene text parser — step 5.2's format. TRUSTED input only; see scene.hpp.

#include "p2pgpu/scene/scene.hpp"

#include <charconv>
#include <fstream>
#include <locale>
#include <sstream>

namespace p2pgpu::scene {
namespace {

using protocol::ErrorCode;
using protocol::MakeError;

/// Strip a `#` comment and surrounding whitespace.
std::string_view Trim(std::string_view s) {
    if (const auto hash = s.find('#'); hash != std::string_view::npos) {
        s = s.substr(0, hash);
    }
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

/// Split on runs of whitespace.
std::vector<std::string_view> Tokens(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
        const std::size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
            ++i;
        }
        if (i > start) {
            out.push_back(line.substr(start, i - start));
        }
    }
    return out;
}

// LOCALE-INDEPENDENT, and that is the whole requirement: a scene that read
// differently under `LANG=de_DE` (comma decimal separator) would build a
// different BVH and therefore a different CONTENT HASH from byte-identical
// input, silently splitting one asset into two.
//
// `std::from_chars` would be the right tool and is unavailable: **Apple Clang's
// libc++ still declares the floating-point overload deleted**, and Emscripten's
// is no better, so using it would have compiled on neither of this project's
// two targets. `strtof` is out for the opposite reason — it honours the global
// C locale, which is exactly the bug above.
//
// A `classic()`-imbued stream is the portable option that is right by
// construction. Reused rather than constructed per token: a 3000-primitive
// scene parses ~30k numbers, and this runs at coordinator startup.
bool ParseFloat(std::string_view tok, float& out) {
    static thread_local std::istringstream stream;
    static thread_local bool imbued = false;
    if (!imbued) {
        stream.imbue(std::locale::classic());
        imbued = true;
    }
    stream.clear();
    stream.str(std::string(tok));
    float v = 0.0F;
    stream >> v;
    // `eof` and not merely `!fail`: trailing junk such as "1.5x" must be
    // rejected, or a typo becomes a silently different scene.
    if (stream.fail() || !stream.eof()) {
        return false;
    }
    out = v;
    return true;
}

bool ParseU32(std::string_view tok, std::uint32_t& out) {
    const auto* begin = tok.data();
    const auto* end = begin + tok.size();
    const auto r = std::from_chars(begin, end, out);
    return r.ec == std::errc{} && r.ptr == end;
}

bool ParseVec(std::span<const std::string_view> toks, std::size_t at, Vec3& out) {
    return at + 2 < toks.size() && ParseFloat(toks[at], out.x) &&
           ParseFloat(toks[at + 1], out.y) && ParseFloat(toks[at + 2], out.z);
}

}  // namespace

protocol::Result<Scene> ParseScene(std::string_view text, std::size_t* error_line) {
    // `protocol::Error::message` is a NON-OWNING string_view documented "static
    // text only". Building messages like `"line " + to_string(n)` would hand it
    // a view of a temporary that dies at the end of the full expression — a
    // dangling read, and precisely what that comment forbids. `kernels.cpp` sets
    // the precedent for trusted-file parsers: literal messages only.
    //
    // The line number a human editing a scene actually needs therefore travels
    // OUT OF BAND rather than inside the error.
    const auto fail = [&](std::size_t line, protocol::Error e) {
        if (error_line != nullptr) { *error_line = line; }
        return e;
    };
    Scene scene;
    // Materials arrive with EXPLICIT indices and may be sparse or out of order,
    // so they are collected into a map-like vector and compacted at the end.
    // Positional numbering would silently renumber every primitive when someone
    // inserts a material — a whole-scene miscolouring from a one-line edit.
    std::vector<std::pair<std::uint32_t, Material>> materials;

    bool seen_version = false;
    std::size_t line_no = 0;

    std::string_view rest = text;
    while (!rest.empty()) {
        const auto nl = rest.find('\n');
        const std::string_view raw = rest.substr(0, nl);
        rest = (nl == std::string_view::npos) ? std::string_view{} : rest.substr(nl + 1);
        ++line_no;

        const std::string_view line = Trim(raw);
        if (line.empty()) {
            continue;
        }
        const auto toks = Tokens(line);
        const std::string_view kind = toks[0];

        if (!seen_version && kind != "version") {
            return fail(line_no, MakeError(ErrorCode::Internal,
                                        "scene: first directive must be `version` — a future format must be rejected, not misread"));
        }

        if (kind == "version") {
            std::uint32_t v = 0;
            if (toks.size() != 2 || !ParseU32(toks[1], v)) {
                return fail(line_no, MakeError(ErrorCode::Internal,
                                        "scene: malformed version directive"));
            }
            if (v != 1) {
                return fail(line_no, MakeError(ErrorCode::Internal,
                                        "scene: unsupported version (this parser is v1)"));
            }
            seen_version = true;
        } else if (kind == "camera") {
            // camera origin X Y Z target X Y Z up X Y Z vfov_deg F
            Camera cam;
            for (std::size_t i = 1; i < toks.size();) {
                const std::string_view key = toks[i];
                if (key == "origin" && ParseVec(toks, i + 1, cam.origin)) {
                    i += 4;
                } else if (key == "target" && ParseVec(toks, i + 1, cam.target)) {
                    i += 4;
                } else if (key == "up" && ParseVec(toks, i + 1, cam.up)) {
                    i += 4;
                } else if (key == "vfov_deg" && i + 1 < toks.size() &&
                           ParseFloat(toks[i + 1], cam.vfov_deg)) {
                    i += 2;
                } else {
                    return fail(line_no, MakeError(ErrorCode::Internal,
                                        "scene: bad camera key"));
                }
            }
            scene.camera = cam;
        } else if (kind == "material") {
            std::uint32_t index = 0;
            if (toks.size() < 6 || !ParseU32(toks[1], index)) {
                return fail(line_no, MakeError(ErrorCode::Internal,
                                        "scene: malformed material directive"));
            }
            Material m;
            const std::string_view what = toks[2];
            if (!ParseVec(toks, 3, m.albedo)) {
                return fail(line_no, MakeError(ErrorCode::Internal,
                                        "scene: bad albedo"));
            }
            if (what == "lambertian") {
                m.kind = MaterialKind::Lambertian;
            } else if (what == "metal") {
                m.kind = MaterialKind::Metal;
                if (toks.size() < 7 || !ParseFloat(toks[6], m.fuzz)) {
                    return fail(line_no, MakeError(ErrorCode::Internal,
                                        "scene: metal material needs a fuzz value"));
                }
            } else if (what == "emissive") {
                m.kind = MaterialKind::Emissive;
            } else {
                return fail(line_no, MakeError(ErrorCode::Internal,
                                        "scene: unknown material kind"));
            }
            materials.emplace_back(index, m);
        } else if (kind == "sphere") {
            Sphere s;
            if (toks.size() != 6 || !ParseVec(toks, 1, s.center) ||
                !ParseFloat(toks[4], s.radius) || !ParseU32(toks[5], s.material)) {
                return fail(line_no, MakeError(ErrorCode::Internal,
                                        "scene: malformed sphere directive"));
            }
            if (!(s.radius > 0.0F)) {
                // Also rejects NaN, which `<= 0` would not.
                return fail(line_no, MakeError(ErrorCode::Internal,
                                        "scene: sphere radius must be positive"));
            }
            scene.spheres.push_back(s);
        } else if (kind == "tri") {
            Triangle t;
            if (toks.size() != 11 || !ParseVec(toks, 1, t.a) || !ParseVec(toks, 4, t.b) ||
                !ParseVec(toks, 7, t.c) || !ParseU32(toks[10], t.material)) {
                return fail(line_no, MakeError(ErrorCode::Internal,
                                        "scene: malformed tri directive"));
            }
            scene.triangles.push_back(t);
        } else {
            // REJECT, never ignore. A v1 parser that skipped unknown directives
            // would render a scene missing geometry and report success — the
            // same shape as a validator that cannot fail.
            return fail(line_no, MakeError(ErrorCode::Internal,
                                        "scene: unknown directive"));
        }
    }

    if (!seen_version) {
        return MakeError(ErrorCode::Internal, "scene has no `version` directive");
    }

    // Compact explicit indices into a dense table, remapping primitives.
    std::uint32_t highest = 0;
    for (const auto& [idx, m] : materials) {
        highest = std::max(highest, idx);
    }
    if (materials.empty()) {
        return MakeError(ErrorCode::Internal, "scene declares no materials");
    }
    scene.materials.assign(static_cast<std::size_t>(highest) + 1, Material{});
    std::vector<bool> defined(static_cast<std::size_t>(highest) + 1, false);
    for (const auto& [idx, m] : materials) {
        scene.materials[idx] = m;
        defined[idx] = true;
    }

    const auto defined_ok = [&](std::uint32_t mat) {
        return mat < scene.materials.size() && defined[mat];
    };
    for (const auto& sp : scene.spheres) {
        if (!defined_ok(sp.material)) {
            return fail(line_no, MakeError(ErrorCode::Internal,
                                        "scene: sphere references an undefined material"));
        }
    }
    for (const auto& t : scene.triangles) {
        if (!defined_ok(t.material)) {
            return fail(line_no, MakeError(ErrorCode::Internal,
                                        "scene: tri references an undefined material"));
        }
    }

    if (scene.primitive_count() == 0) {
        return MakeError(ErrorCode::Internal, "scene has no primitives");
    }
    return scene;
}

protocol::Result<Scene> LoadSceneFile(const std::string& path, std::size_t* error_line) {
    std::ifstream f(path);
    if (!f) {
        return MakeError(ErrorCode::Internal, "cannot open scene file: " + path);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ParseScene(ss.str(), error_line);
}

}  // namespace p2pgpu::scene
