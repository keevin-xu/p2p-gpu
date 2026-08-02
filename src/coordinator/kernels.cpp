// Kernel registry — step 1.12. Parses kernels/manifest.toml and loads the WGSL.
//
// The manifest is a TRUSTED, in-repo, developer-authored file read once at
// startup. It is not network input, so R11's constraints do not apply here
// (D-0030). If this parser is ever pointed at peer-supplied data it must be
// replaced with a real one, not patched.

#include "p2pgpu/coordinator/kernel_registry.hpp"

#include <charconv>
#include <fstream>
#include <sstream>

namespace p2pgpu::coordinator {
namespace {

using protocol::ErrorCode;
using protocol::MakeError;

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) { s.remove_prefix(1); }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

std::string_view StripQuotes(std::string_view s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

/// One `key = value` pair, values kept as raw text and converted on demand.
using Table = std::map<std::string, std::string, std::less<>>;

/// The restricted TOML subset we accept: `[section.name]` headers, `key = value`
/// pairs, `#` comments, `[a, b, c]` arrays, and `"""..."""` multi-line strings.
/// Anything else is ignored rather than rejected — this is a config loader, not
/// a validator, and the schema check happens in Parse() below where the errors
/// can be specific.
std::map<std::string, Table, std::less<>> ParseToml(std::string_view text) {
    std::map<std::string, Table, std::less<>> out;
    std::string section;
    std::size_t pos = 0;

    auto next_line = [&]() -> std::optional<std::string_view> {
        if (pos >= text.size()) { return std::nullopt; }
        const std::size_t nl = text.find('\n', pos);
        const std::string_view line =
            text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = (nl == std::string_view::npos) ? text.size() : nl + 1;
        return line;
    };

    while (auto maybe = next_line()) {
        std::string_view line = Trim(*maybe);
        if (line.empty() || line.front() == '#') { continue; }

        if (line.front() == '[') {
            const std::size_t close = line.find(']');
            if (close != std::string_view::npos) {
                section = std::string(Trim(line.substr(1, close - 1)));
                out.try_emplace(section);
            }
            continue;
        }

        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) { continue; }
        const std::string key(Trim(line.substr(0, eq)));
        std::string_view rest = Trim(line.substr(eq + 1));

        // Multi-line string: consume until the closing delimiter. Needed because
        // r5_note is where the R5 derivation lives, and that has to be readable
        // in the manifest rather than compressed onto one line.
        if (rest.starts_with("\"\"\"")) {
            std::string acc;
            std::string_view first = rest.substr(3);
            if (const std::size_t end = first.find("\"\"\""); end != std::string_view::npos) {
                acc = std::string(first.substr(0, end));
            } else {
                acc = std::string(first);
                while (auto more = next_line()) {
                    std::string_view l = *more;
                    // `close`, not `end` — the outer scope already has an `end`
                    // for the same delimiter on the FIRST line, and shadowing it
                    // here means a reader has to work out which one a given
                    // `end` refers to. Harmless today; -Wshadow exists because
                    // it is not always.
                    if (const std::size_t close = l.find("\"\"\""); close != std::string_view::npos) {
                        acc += "\n";
                        acc += l.substr(0, close);
                        break;
                    }
                    acc += "\n";
                    acc += l;
                }
            }
            out[section][key] = acc;
            continue;
        }

        // Strip a trailing comment, but not one inside a quoted string.
        if (!rest.starts_with('"')) {
            if (const std::size_t hash = rest.find('#'); hash != std::string_view::npos) {
                rest = Trim(rest.substr(0, hash));
            }
        }
        out[section][key] = std::string(StripQuotes(Trim(rest)));
    }
    return out;
}

std::optional<std::uint64_t> AsUint(const Table& t, std::string_view key) {
    const auto it = t.find(key);
    if (it == t.end()) { return std::nullopt; }
    const std::string& v = it->second;
    std::uint64_t out = 0;
    // Scientific notation (4.0e5) is legal TOML float syntax and the manifest
    // uses it for r5_min_units, so fall back to a double parse.
    if (v.find('e') != std::string::npos || v.find('.') != std::string::npos) {
        try {
            return static_cast<std::uint64_t>(std::stod(v));
        } catch (...) {
            return std::nullopt;
        }
    }
    const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
    if (ec != std::errc{}) { return std::nullopt; }
    return out;
}

std::optional<double> AsDouble(const Table& t, std::string_view key) {
    const auto it = t.find(key);
    if (it == t.end()) { return std::nullopt; }
    try {
        return std::stod(it->second);
    } catch (...) {
        return std::nullopt;
    }
}

std::string AsString(const Table& t, std::string_view key, std::string fallback = {}) {
    const auto it = t.find(key);
    return it == t.end() ? std::move(fallback) : it->second;
}

bool AsBool(const Table& t, std::string_view key, bool fallback = false) {
    const auto it = t.find(key);
    return it == t.end() ? fallback : (it->second == "true");
}

/// `[256, 1, 1]` -> three uints.
bool ParseWorkgroup(const Table& t, std::uint32_t (&out)[3]) {
    const auto it = t.find("workgroup_size");
    if (it == t.end()) { return false; }
    std::string_view v = Trim(it->second);
    if (v.size() < 2 || v.front() != '[' || v.back() != ']') { return false; }
    v = v.substr(1, v.size() - 2);

    int i = 0;
    while (!v.empty() && i < 3) {
        const std::size_t comma = v.find(',');
        std::string_view tok = Trim(comma == std::string_view::npos ? v : v.substr(0, comma));
        std::uint32_t n = 0;
        if (std::from_chars(tok.data(), tok.data() + tok.size(), n).ec != std::errc{}) {
            return false;
        }
        out[i++] = n;
        if (comma == std::string_view::npos) { break; }
        v = v.substr(comma + 1);
    }
    return i == 3;
}

}  // namespace

protocol::Result<KernelRegistry> KernelRegistry::Load(
    const std::filesystem::path& manifest, const std::filesystem::path& kernel_dir) {
    std::ifstream f(manifest);
    if (!f) {
        return MakeError(ErrorCode::Internal, "cannot open kernels/manifest.toml");
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();

    KernelRegistry reg;
    for (const auto& [section, table] : ParseToml(text)) {
        // Only `[kernels.<id>]` sections describe kernels.
        constexpr std::string_view kPrefix = "kernels.";
        if (!std::string_view(section).starts_with(kPrefix)) { continue; }

        KernelSpec spec;
        spec.id = section.substr(kPrefix.size());
        spec.file = AsString(table, "file");
        spec.entry_point = AsString(table, "entry_point", "main");

        if (spec.file.empty()) {
            return MakeError(ErrorCode::Internal, "kernel entry has no `file`");
        }
        if (!ParseWorkgroup(table, spec.workgroup_size)) {
            return MakeError(ErrorCode::Internal, "kernel entry has no valid workgroup_size");
        }

        // STRICT: an unrecognised determinism is an error, never a default.
        // Silently defaulting would risk declaring a float kernel `exact` and
        // blacklisting every honest worker (R6, RISKS.md §2).
        const std::string det = AsString(table, "determinism");
        if (det == "exact") {
            spec.determinism = Determinism::Exact;
        } else if (det == "tolerant") {
            spec.determinism = Determinism::Tolerant;
        } else if (det == "statistical") {
            spec.determinism = Determinism::Statistical;
        } else {
            return MakeError(ErrorCode::Internal,
                             "kernel `determinism` must be exact|tolerant|statistical");
        }

        spec.rel_eps = static_cast<float>(AsDouble(table, "rel_eps").value_or(0.0));
        spec.abs_eps = static_cast<float>(AsDouble(table, "abs_eps").value_or(0.0));
        spec.output_bytes = static_cast<std::uint32_t>(AsUint(table, "output_bytes").value_or(0));
        spec.output_dtype = AsString(table, "output_dtype");
        spec.param_layout = AsString(table, "param_layout");
        spec.accumulates = AsBool(table, "accumulates");
        spec.min_iterations = AsUint(table, "min_iterations").value_or(0);
        spec.flop_per_unit = AsUint(table, "flop_per_unit").value_or(0);
        spec.r5_min_units = AsUint(table, "r5_min_units").value_or(0);
        spec.r5_ratio = AsDouble(table, "r5_ratio").value_or(0.0);

        // A `tolerant` kernel with no epsilons compares everything as equal,
        // which is worse than useless — it would accept any answer at all.
        if (spec.determinism == Determinism::Tolerant &&
            spec.rel_eps == 0.0F && spec.abs_eps == 0.0F) {
            return MakeError(ErrorCode::Internal,
                             "tolerant kernel declares no rel_eps/abs_eps");
        }

        // R5 is a hard rule (CLAUDE.md). Refusing to start beats discovering at
        // Phase 7 that a shipped kernel was transfer-bound all along.
        if (spec.r5_ratio > 0.0 && spec.r5_ratio < 1.0e6 && !spec.accumulates) {
            return MakeError(ErrorCode::Internal,
                             "kernel declares an R5 ratio below 1e6 without accumulation");
        }

        std::ifstream kf(kernel_dir / spec.file);
        if (!kf) {
            return MakeError(ErrorCode::Internal, "cannot open kernel WGSL file");
        }
        std::ostringstream ks;
        ks << kf.rdbuf();
        spec.wgsl = ks.str();
        if (spec.wgsl.empty()) {
            return MakeError(ErrorCode::Internal, "kernel WGSL file is empty");
        }

        reg.kernels_.emplace(spec.id, std::move(spec));
    }

    if (reg.kernels_.empty()) {
        return MakeError(ErrorCode::Internal, "manifest declares no kernels");
    }
    return reg;
}

const KernelSpec* KernelRegistry::Find(std::string_view id) const noexcept {
    const auto it = kernels_.find(id);
    return it == kernels_.end() ? nullptr : &it->second;
}

std::vector<const KernelSpec*> KernelRegistry::All() const {
    std::vector<const KernelSpec*> out;
    out.reserve(kernels_.size());
    for (const auto& [_, spec] : kernels_) { out.push_back(&spec); }
    return out;
}

}  // namespace p2pgpu::coordinator
