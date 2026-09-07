#include "docx/merger.hpp"
#include "docx/image.hpp"
#include "textfabric/error.hpp"

#include <zip.h>

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <regex>
#include <sstream>

namespace textfabric::docx {

namespace {

/// RAII helper around libzip's zip_t*.
class ZipFile {
public:
    explicit ZipFile(zip_t* z) noexcept : z_(z) {}
    ~ZipFile() { if (z_) zip_discard(z_); }
    ZipFile(const ZipFile&) = delete;
    ZipFile& operator=(const ZipFile&) = delete;

    zip_t*  get()   const noexcept { return z_; }
    zip_t*  release() noexcept { auto* t = z_; z_ = nullptr; return t; }

private:
    zip_t* z_;
};

/// Read every file from an open zip archive into a name→bytes map.
std::unordered_map<std::string, std::string>
slurp_archive(zip_t* z) {
    std::unordered_map<std::string, std::string> out;
    const zip_int64_t n = zip_get_num_entries(z, 0);
    if (n < 0) {
        throw ReportException(ReportError::CantCopyDocxTemplate,
                              "zip_get_num_entries failed");
    }

    for (zip_int64_t i = 0; i < n; ++i) {
        zip_stat_t st{};
        zip_stat_init(&st);
        if (zip_stat_index(z, static_cast<zip_uint64_t>(i), 0, &st) != 0) {
            throw ReportException(ReportError::CantCopyDocxTemplate,
                                  fmt::format("zip_stat_index({}) failed", i));
        }
        const std::string name = st.name ? st.name : "";
        // Skip directory entries
        if (!name.empty() && name.back() == '/') continue;

        zip_file_t* f = zip_fopen_index(z, static_cast<zip_uint64_t>(i), 0);
        if (!f) {
            throw ReportException(ReportError::CantCopyDocxTemplate,
                                  fmt::format("zip_fopen_index({}) failed", i));
        }

        std::string buf;
        buf.resize(static_cast<std::size_t>(st.size));
        zip_int64_t read = zip_fread(f, buf.data(), st.size);
        zip_fclose(f);

        if (read < 0 || static_cast<zip_uint64_t>(read) != st.size) {
            throw ReportException(ReportError::CantCopyDocxTemplate,
                                  fmt::format("zip_fread({}) short read", name));
        }

        out.emplace(std::move(const_cast<std::string&>(name)), std::move(buf));
    }
    return out;
}

} // namespace

// ────────────────────────────────────────────────────────────────────────────

DocxMerger::DocxMerger()  = default;
DocxMerger::~DocxMerger() = default;

void DocxMerger::setCodePage(const std::string& code_page) {
    // v1 supports UTF-8 only; this is effectively a no-op guard.
    if (code_page != "UTF-8") {
        throw ReportException(
            ReportError::NotImplemented,
            fmt::format("only UTF-8 is supported in v1 (got {:?})", code_page));
    }
    code_page_ = code_page;
}

// ── load / save ─────────────────────────────────────────────────────────────

void DocxMerger::read_archive(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        throw ReportException(
            ReportError::CantOpenTemplate,
            fmt::format("template not found: {}", path.string()));
    }

    int err = 0;
    zip_t* raw = zip_open(path.string().c_str(), ZIP_RDONLY, &err);
    if (!raw) {
        zip_error_t zerr;
        zip_error_init_with_code(&zerr, err);
        const std::string msg = fmt::format("zip_open failed: {}",
                                             zip_error_strerror(&zerr));
        zip_error_fini(&zerr);
        throw ReportException(ReportError::CantOpenTemplate, msg);
    }
    ZipFile zf{raw};

    parts_ = slurp_archive(zf.get());

    if (parts_.find(kDocumentPart) == parts_.end()) {
        throw ReportException(
            ReportError::CantCopyDocxTemplate,
            fmt::format("missing required part: {}", kDocumentPart));
    }
}

void DocxMerger::load(const std::string& path) {
    parts_.clear();
    document_.reset();
    pasted_.clear();
    loaded_ = false;

    read_archive(std::filesystem::path(path));
    reparse_document();
    loaded_ = true;
}

void DocxMerger::reparse_document() {
    const std::string& xml = parts_.at(kDocumentPart);
    const auto result = document_.load_buffer(xml.data(), xml.size());
    if (!result) {
        throw ReportException(
            ReportError::CantCopyDocxTemplate,
            fmt::format("pugixml parse error: {}", result.description()));
    }
}

void DocxMerger::reserialize_document() {
    std::ostringstream oss;
    document_.save(oss, "", pugi::format_raw | pugi::format_no_declaration);
    // DOCX requires an XML declaration — pugixml emits one unless disabled.
    // We emit our own to preserve exactly the standalone attribute MS Word uses.
    std::string xml = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)";
    xml += oss.str();
    parts_[kDocumentPart] = std::move(xml);
}

void DocxMerger::write_archive(const std::filesystem::path& path) const {
    // Overwrite target (libzip opens append-mode otherwise).
    std::error_code ec;
    std::filesystem::remove(path, ec);

    int err = 0;
    zip_t* raw = zip_open(path.string().c_str(), ZIP_CREATE | ZIP_EXCL, &err);
    if (!raw) {
        zip_error_t zerr;
        zip_error_init_with_code(&zerr, err);
        const std::string msg = fmt::format("zip_open(create) failed: {}",
                                             zip_error_strerror(&zerr));
        zip_error_fini(&zerr);
        throw ReportException(ReportError::SaveFailed, msg);
    }

    // libzip's zip_source_buffer keeps a pointer into `data` until
    // zip_close() — we must keep all buffers alive until then.
    // Since parts_ is stable (std::unordered_map, not mutated during save),
    // we pass `freep=0` and rely on parts_'s lifetime.
    for (const auto& [name, bytes] : parts_) {
        zip_source_t* src = zip_source_buffer(raw, bytes.data(), bytes.size(), 0);
        if (!src) {
            zip_discard(raw);
            throw ReportException(
                ReportError::SaveFailed,
                fmt::format("zip_source_buffer failed for {}", name));
        }
        const zip_int64_t idx = zip_file_add(raw, name.c_str(), src,
                                             ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8);
        if (idx < 0) {
            zip_source_free(src);
            const std::string msg = fmt::format("zip_file_add({}) failed: {}",
                                                 name, zip_strerror(raw));
            zip_discard(raw);
            throw ReportException(ReportError::SaveFailed, msg);
        }
    }

    if (zip_close(raw) != 0) {
        const std::string msg = fmt::format("zip_close failed: {}",
                                             zip_strerror(raw));
        zip_discard(raw);
        throw ReportException(ReportError::SaveFailed, msg);
    }
}

// ── Converter dispatch (Stage 6) ────────────────────────────────────────────
//
// `.docx → .pdf/.html` goes through one of two external converters. The
// preferred order, left to right:
//
//   1. Microsoft Word (Windows only) — via a generated VBScript driven by
//      `cscript //B`. Chosen when Word is registered under
//      HKCR\Word.Application and `TEXTFABRIC_NO_MSWORD` is not set.
//      Rationale: on machines that already have Office installed the result
//      matches what the user sees in Word interactively.
//   2. LibreOffice `soffice --headless --convert-to`. Detected via
//      `TEXTFABRIC_SOFFICE` override, Program Files lookups (Windows), and
//      finally a `soffice --version` PATH probe.
//
// Env overrides:
//   TEXTFABRIC_DISABLE_CONVERTERS — any non-empty value forces None.
//                                   Used by tests to assert the NoConverter path.
//   TEXTFABRIC_NO_MSWORD          — any non-empty value skips the Word probe
//                                   (forces LibreOffice-only on Windows).
//   TEXTFABRIC_SOFFICE            — absolute path to soffice. If set non-empty
//                                   it is used as-is (no other LibreOffice
//                                   probe). Empty value disables the
//                                   LibreOffice branch entirely.
//
// If every branch returns None, save() throws `NoConverter` with a message
// that names both dependencies.

namespace {

enum class ConverterKind { None, MSWord, LibreOffice };

struct Converter {
    ConverterKind kind = ConverterKind::None;
    std::string   soffice_path;  // populated only when kind == LibreOffice
};

[[nodiscard]] bool env_nonempty(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && *v != '\0';
}

#ifdef _WIN32
[[nodiscard]] bool msword_registered() {
    // HKCR\Word.Application exists iff some Office version is installed and
    // has registered its COM class. `reg query` is present in every Windows
    // install and exits 0 on hit, 1 on miss.
    return std::system("reg query HKCR\\Word.Application >nul 2>nul") == 0;
}
#endif

Converter find_converter() {
    if (env_nonempty("TEXTFABRIC_DISABLE_CONVERTERS")) return {};

#ifdef _WIN32
    if (!env_nonempty("TEXTFABRIC_NO_MSWORD") && msword_registered()) {
        return {ConverterKind::MSWord, {}};
    }
#endif

    const char* const override_path = std::getenv("TEXTFABRIC_SOFFICE");
    if (override_path != nullptr) {
        if (*override_path == '\0') return {};  // explicit opt-out
        if (std::filesystem::exists(override_path)) {
            return {ConverterKind::LibreOffice, override_path};
        }
        return {};
    }

#ifdef _WIN32
    constexpr const char* kWinCandidates[] = {
        "C:\\Program Files\\LibreOffice\\program\\soffice.exe",
        "C:\\Program Files (x86)\\LibreOffice\\program\\soffice.exe",
    };
    for (const char* c : kWinCandidates) {
        if (std::filesystem::exists(c)) return {ConverterKind::LibreOffice, c};
    }
    const char* const probe = "soffice --version >nul 2>nul";
#else
    const char* const probe = "soffice --version >/dev/null 2>&1";
#endif
    if (std::system(probe) == 0) {
        return {ConverterKind::LibreOffice, "soffice"};
    }
    return {};
}

// Minimal shell-safe quoting. Rejects embedded double quotes rather than
// trying to escape them portably (cmd.exe and POSIX sh have different rules).
std::string shell_quote(const std::string& s) {
    if (s.find('"') != std::string::npos) {
        throw ReportException(
            ReportError::SaveFailed,
            fmt::format("path contains a double-quote character, refusing: {}", s));
    }
    return "\"" + s + "\"";
}

// Run `soffice --headless --convert-to <format> --outdir <outdir> <input>`.
// Produces `<outdir>/<input-stem>.<format>`. Returns true on exit-code 0.
bool run_soffice_conversion(const std::string& soffice,
                            const std::string& format,  // "pdf" or "html"
                            const std::filesystem::path& input,
                            const std::filesystem::path& outdir)
{
#ifdef _WIN32
    constexpr const char* kSilence = " >nul 2>nul";
#else
    constexpr const char* kSilence = " >/dev/null 2>&1";
#endif
    const std::string cmd = fmt::format(
        "{} --headless --convert-to {} --outdir {} {}{}",
        shell_quote(soffice),
        format,
        shell_quote(outdir.string()),
        shell_quote(input.string()),
        kSilence);
    return std::system(cmd.c_str()) == 0;
}

#ifdef _WIN32
// Run Word via a generated VBScript. Exit codes:
//   0 — ok
//   2 — CreateObject("Word.Application") failed (unlikely after reg probe)
//   3 — Documents.Open failed (corrupted .docx, locked, etc.)
//   4 — SaveAs2 failed (unknown format, read-only target)
// cscript is always present on Windows 10/11 (no PowerShell ExecutionPolicy
// headaches) and can run in //B (batch, suppress dialogs) mode.
bool run_msword_conversion(const std::string& format,  // "pdf" or "html"
                           const std::filesystem::path& input,
                           const std::filesystem::path& output)
{
    // wdFormatPDF = 17, wdFormatFilteredHTML = 10 (both stable since Word 2007)
    const int fmt_code = (format == "pdf") ? 17 : 10;
    const std::filesystem::path vbs = output.parent_path() / "tf_msword.vbs";
    {
        std::ofstream os(vbs, std::ios::binary);
        if (!os) return false;
        // CRLF is what cscript expects; using binary mode so ofstream doesn't
        // double-convert on Windows.
        os << "Option Explicit\r\n"
              "Dim word, doc\r\n"
              "On Error Resume Next\r\n"
              "Set word = CreateObject(\"Word.Application\")\r\n"
              "If Err.Number <> 0 Then WScript.Quit 2\r\n"
              "word.Visible = False\r\n"
              "word.DisplayAlerts = 0\r\n"
              "Set doc = word.Documents.Open(WScript.Arguments(0), False, True)\r\n"
              "If Err.Number <> 0 Then\r\n"
              "    word.Quit\r\n"
              "    WScript.Quit 3\r\n"
              "End If\r\n"
              "doc.SaveAs2 WScript.Arguments(1), " << fmt_code << "\r\n"
              "If Err.Number <> 0 Then\r\n"
              "    doc.Close False\r\n"
              "    word.Quit\r\n"
              "    WScript.Quit 4\r\n"
              "End If\r\n"
              "doc.Close False\r\n"
              "word.Quit\r\n"
              "WScript.Quit 0\r\n";
    }
    const std::string cmd = fmt::format(
        "cscript //B //Nologo {} {} {} >nul 2>nul",
        shell_quote(vbs.string()),
        shell_quote(std::filesystem::absolute(input).string()),
        shell_quote(std::filesystem::absolute(output).string()));
    const int rc = std::system(cmd.c_str());
    std::error_code ec;
    std::filesystem::remove(vbs, ec);
    return rc == 0;
}
#endif  // _WIN32

// Unique per-call temp dir under the OS temp location.
std::filesystem::path make_scratch_dir() {
    const auto ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    auto dir = std::filesystem::temp_directory_path() /
               fmt::format("textfabric-{}", ts);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

} // namespace

void DocxMerger::save(const std::string& path) {
    if (!loaded_) {
        throw ReportException(ReportError::SaveFailed,
                              "save() called before load()");
    }

    const std::filesystem::path p(path);
    const std::string ext = p.extension().string();

    if (ext == ".pdf" || ext == ".html" || ext == ".htm") {
        const Converter conv = find_converter();
        if (conv.kind == ConverterKind::None) {
            throw ReportException(
                ReportError::NoConverter,
                fmt::format(
                    "No converter available to produce {}. On Windows install "
                    "Microsoft Word or LibreOffice; on Linux/macOS install "
                    "LibreOffice. Set TEXTFABRIC_SOFFICE to an absolute path "
                    "to override LibreOffice detection.",
                    ext));
        }

        // Two-step: serialize to .docx in a scratch dir, then convert.
        const std::filesystem::path scratch = make_scratch_dir();
        const std::filesystem::path scratch_docx = scratch / "output.docx";

        std::error_code cleanup_ec;
        auto cleanup = [&]() {
            std::filesystem::remove_all(scratch, cleanup_ec);
        };

        reserialize_document();
        try {
            write_archive(scratch_docx);
        } catch (...) {
            cleanup();
            throw;
        }

        const std::string conv_format = (ext == ".pdf") ? "pdf" : "html";
        const std::filesystem::path produced =
            scratch / ("output." + conv_format);

        bool        ok         = false;
        const char* conv_label = "unknown";
        switch (conv.kind) {
            case ConverterKind::LibreOffice:
                conv_label = "LibreOffice";
                ok = run_soffice_conversion(conv.soffice_path, conv_format,
                                            scratch_docx, scratch);
                break;
#ifdef _WIN32
            case ConverterKind::MSWord:
                conv_label = "Microsoft Word";
                ok = run_msword_conversion(conv_format, scratch_docx, produced);
                break;
#endif
            default:
                break;  // unreachable: None was rejected above
        }

        if (!ok) {
            cleanup();
            throw ReportException(
                ReportError::SaveFailed,
                fmt::format("{} failed while converting to {}",
                            conv_label, ext));
        }
        if (!std::filesystem::exists(produced)) {
            cleanup();
            throw ReportException(
                ReportError::SaveFailed,
                fmt::format("{} returned 0 but expected output {} is missing",
                            conv_label, produced.string()));
        }

        // Move into place. Cross-device move is possible (tmp on a different
        // filesystem than the target) so fall back to copy.
        std::filesystem::remove(p, cleanup_ec);
        std::error_code move_ec;
        std::filesystem::rename(produced, p, move_ec);
        if (move_ec) {
            std::filesystem::copy_file(
                produced, p,
                std::filesystem::copy_options::overwrite_existing, move_ec);
            if (move_ec) {
                cleanup();
                throw ReportException(
                    ReportError::SaveFailed,
                    fmt::format("failed to place converted file at {}: {}",
                                p.string(), move_ec.message()));
            }
        }
        cleanup();
        return;
    }
    if (ext != ".docx") {
        throw ReportException(
            ReportError::SaveFailed,
            fmt::format("unsupported extension: {}", ext));
    }

    reserialize_document();
    write_archive(p);
}

std::string DocxMerger::document_xml() const {
    // Serialize the live DOM — parts_[kDocumentPart] is only refreshed
    // during save(), so it can be stale after mutations.
    if (!loaded_) return {};
    std::ostringstream oss;
    document_.save(oss, "", pugi::format_raw | pugi::format_no_declaration);
    return oss.str();
}

// ── Bookmark scanning ───────────────────────────────────────────────────────

std::vector<pugi::xml_node>
DocxMerger::find_bookmark_starts(const std::string& name) const {
    std::vector<pugi::xml_node> out;
    // Walk the whole document — bookmarks can live anywhere
    // inside <w:body>, tables, headers etc.
    struct Walker : pugi::xml_tree_walker {
        const std::string& target;
        std::vector<pugi::xml_node>& out;
        Walker(const std::string& t, std::vector<pugi::xml_node>& o)
            : target(t), out(o) {}
        bool for_each(pugi::xml_node& n) override {
            if (std::strcmp(n.name(), "w:bookmarkStart") == 0) {
                const auto attr = n.attribute("w:name");
                if (attr && target == attr.value()) out.push_back(n);
            }
            return true;
        }
    } walker{name, out};

    // pugixml's const-walker pattern requires a non-const document reference.
    const_cast<pugi::xml_document&>(document_).traverse(walker);
    return out;
}

// ── setClipboardValue ───────────────────────────────────────────────────────
//
// Strategy for v1:
//   1. Find the <w:bookmarkStart w:name="..."> node matching `bookmark`.
//   2. Walk forward in document order (up to the matching <w:bookmarkEnd>)
//      collecting <w:t> nodes.
//   3. Concatenate their text; if `field` appears as a substring, replace
//      the first occurrence. Redistribute the result:
//        - write the whole replaced text into the first <w:t>, then
//        - clear the remaining <w:t>s that held the tail of `field`.
//      This preserves run-property formatting (the <w:rPr> on the first run).
//
// This is intentionally conservative — it handles both `kFIELD`-style
// and `{{field}}` placeholders inside a single bookmark.

namespace {

// Collect <w:t> nodes between a bookmarkStart and its matching bookmarkEnd.
std::vector<pugi::xml_node>
collect_text_nodes_in_bookmark(pugi::xml_node bookmark_start) {
    std::vector<pugi::xml_node> texts;

    // Walk forward from bookmark_start in document order until we hit
    // <w:bookmarkEnd> with matching w:id.
    const auto id_attr = bookmark_start.attribute("w:id");
    const std::string wanted_id = id_attr ? id_attr.value() : "";

    // Simple depth-first traversal using explicit stack over the subtree
    // that *follows* the bookmark inside its parent chain.
    pugi::xml_node body = bookmark_start;
    while (body && std::strcmp(body.name(), "w:body") != 0 && body.parent()) {
        body = body.parent();
    }
    if (!body) return texts;

    struct Walker : pugi::xml_tree_walker {
        const std::string& wanted_id;
        std::vector<pugi::xml_node>& texts;
        bool started = false;
        bool done    = false;
        pugi::xml_node start_node;

        Walker(const std::string& id, std::vector<pugi::xml_node>& t,
               pugi::xml_node s)
            : wanted_id(id), texts(t), start_node(s) {}

        bool for_each(pugi::xml_node& n) override {
            if (done) return false;
            if (!started) {
                if (n == start_node) started = true;
                return true;
            }
            if (std::strcmp(n.name(), "w:bookmarkEnd") == 0) {
                const auto id = n.attribute("w:id");
                if (id && wanted_id == id.value()) {
                    done = true;
                    return false;
                }
            }
            if (std::strcmp(n.name(), "w:t") == 0) {
                texts.push_back(n);
            }
            return true;
        }
    } walker{wanted_id, texts, bookmark_start};

    body.traverse(walker);
    return texts;
}

// Collect every <w:t> descendant of `root` in document order.
// Used by setTableRow to scope replacements to a single <w:tc>.
std::vector<pugi::xml_node>
collect_text_descendants(pugi::xml_node root) {
    std::vector<pugi::xml_node> out;
    struct Walker : pugi::xml_tree_walker {
        std::vector<pugi::xml_node>& o;
        explicit Walker(std::vector<pugi::xml_node>& o_) : o(o_) {}
        bool for_each(pugi::xml_node& n) override {
            if (std::strcmp(n.name(), "w:t") == 0) o.push_back(n);
            return true;
        }
    } w{out};
    root.traverse(w);
    return out;
}

// Preserve leading / trailing spaces on a <w:t> by setting xml:space="preserve".
// Without this attribute Word collapses boundary whitespace on open.
void ensure_xml_space_preserve(pugi::xml_node t, const std::string& text) {
    if (text.empty()) return;
    if (text.front() != ' ' && text.back() != ' ') return;
    auto attr = t.attribute("xml:space");
    if (!attr) attr = t.append_attribute("xml:space");
    attr.set_value("preserve");
}

// Replace the first occurrence of `field` across the concatenated text of
// `texts` with `value`. Only the <w:t> nodes that actually contributed to
// the matched substring are rewritten: the first affected node receives
// its own prefix + `value`, the last receives its own suffix, any
// fully-consumed intermediates are cleared. Text nodes outside the match
// span (including ones that belong to unrelated overlapping bookmarks) are
// left byte-for-byte untouched. Returns true when a replacement happened.
bool replace_placeholder_in(std::vector<pugi::xml_node>& texts,
                            const std::string& field,
                            const std::string& value) {
    if (texts.empty()) return false;

    // Record where each contributor starts in the joined string so we can
    // map the match back to individual nodes later.
    std::string joined;
    std::vector<std::size_t> offsets;
    offsets.reserve(texts.size());
    for (auto t : texts) {
        offsets.push_back(joined.size());
        joined += t.text().get();
    }

    const auto pos = joined.find(field);
    if (pos == std::string::npos) return false;
    const auto end = pos + field.size();

    auto node_end = [&](std::size_t i) {
        return (i + 1 < offsets.size()) ? offsets[i + 1] : joined.size();
    };

    std::size_t first = 0;
    while (first + 1 < texts.size() && node_end(first) <= pos) ++first;
    std::size_t last = first;
    while (last + 1 < texts.size() && node_end(last) < end) ++last;

    const std::string prefix = joined.substr(offsets[first], pos - offsets[first]);
    const std::string suffix = joined.substr(end, node_end(last) - end);

    if (first == last) {
        const std::string combined = prefix + value + suffix;
        texts[first].text().set(combined.c_str());
        ensure_xml_space_preserve(texts[first], combined);
    } else {
        const std::string first_text = prefix + value;
        texts[first].text().set(first_text.c_str());
        ensure_xml_space_preserve(texts[first], first_text);
        for (std::size_t i = first + 1; i < last; ++i) {
            texts[i].text().set("");
        }
        texts[last].text().set(suffix.c_str());
        ensure_xml_space_preserve(texts[last], suffix);
    }
    return true;
}

// Walk up the parent chain from `node` until a tag named `tag_name` is found.
// Returns an empty node if no such ancestor exists.
pugi::xml_node ancestor_named(pugi::xml_node node, const char* tag_name) {
    for (auto cur = node; cur; cur = cur.parent()) {
        if (std::strcmp(cur.name(), tag_name) == 0) return cur;
    }
    return pugi::xml_node{};
}

// Delete every <w:bookmarkStart>/<w:bookmarkEnd> descendant of `root`.
// Prevents duplicate `w:id` attributes when a template row is cloned —
// Word otherwise refuses to open the document.
void strip_bookmarks(pugi::xml_node root) {
    std::vector<pugi::xml_node> to_remove;
    struct Walker : pugi::xml_tree_walker {
        std::vector<pugi::xml_node>& kill;
        explicit Walker(std::vector<pugi::xml_node>& k) : kill(k) {}
        bool for_each(pugi::xml_node& n) override {
            const char* nm = n.name();
            if (std::strcmp(nm, "w:bookmarkStart") == 0 ||
                std::strcmp(nm, "w:bookmarkEnd") == 0) {
                kill.push_back(n);
            }
            return true;
        }
    } w{to_remove};
    root.traverse(w);
    for (auto n : to_remove) n.parent().remove_child(n);
}

// Recognize "word/header<N>.xml" / "word/footer<N>.xml" archive parts — the
// only shape Word produces for these header/footer parts, none of which
// carry bookmarks of their own.
bool is_header_footer_part(const std::string& part_name, std::string_view prefix) {
    if (part_name.size() < prefix.size() + 5) return false;   // prefix + digit + ".xml"
    if (part_name.compare(0, prefix.size(), prefix) != 0) return false;
    if (part_name.compare(part_name.size() - 4, 4, ".xml") != 0) return false;
    for (std::size_t i = prefix.size(); i < part_name.size() - 4; ++i) {
        if (part_name[i] < '0' || part_name[i] > '9') return false;
    }
    return true;
}

} // namespace

void DocxMerger::setClipboardValue(const std::string& bookmark,
                                   const std::string& field,
                                   const std::string& value) {
    if (!loaded_) {
        throw ReportException(ReportError::CantOpenTemplate,
                              "setClipboardValue before load()");
    }

    // ── "_Header" / "_Footer" pseudo-bookmarks (data contract §3.1) ────────
    // Word keeps header/footer content in separate parts
    // (word/header{N}.xml, word/footer{N}.xml) that document.xml only
    // references from section properties — they typically carry no
    // <w:bookmarkStart> of their own. There's nothing to
    // resolve via find_bookmark_starts, so instead we walk every part whose
    // name matches the requested half and replace `field` in each one that
    // contains it, using the same split-run-safe replacement as everywhere
    // else.
    if (bookmark == "_Header" || bookmark == "_Footer") {
        const std::string_view prefix =
            (bookmark == "_Header") ? std::string_view("word/header")
                                     : std::string_view("word/footer");

        bool any_part      = false;
        bool any_replaced  = false;
        for (auto& [name, bytes] : parts_) {
            if (!is_header_footer_part(name, prefix)) continue;
            any_part = true;

            pugi::xml_document part_doc;
            if (!part_doc.load_buffer(bytes.data(), bytes.size())) continue;

            auto texts = collect_text_descendants(part_doc.document_element());
            if (replace_placeholder_in(texts, field, value)) {
                std::ostringstream oss;
                part_doc.save(oss, "", pugi::format_raw);
                bytes = oss.str();
                any_replaced = true;
            }
        }

        if (!any_part) {
            throw ReportException(
                ReportError::InvalidBookmark,
                fmt::format("no {} parts in archive", bookmark));
        }
        if (!any_replaced) {
            throw ReportException(
                ReportError::InvalidField,
                fmt::format("field '{}' not found in any {} part", field, bookmark));
        }
        return;
    }

    const auto starts = find_bookmark_starts(bookmark);
    if (starts.empty()) {
        throw ReportException(
            ReportError::InvalidBookmark,
            fmt::format("bookmark not found: {}", bookmark));
    }

    bool any_replaced = false;

    for (auto bmk : starts) {
        auto texts = collect_text_nodes_in_bookmark(bmk);
        if (replace_placeholder_in(texts, field, value)) {
            any_replaced = true;
        }
    }

    if (!any_replaced) {
        throw ReportException(
            ReportError::InvalidField,
            fmt::format("field '{}' not found in bookmark '{}'", field, bookmark));
    }
}

// ── setTableRow ─────────────────────────────────────────────────────────────
//
// Stage 3 — clones a <w:tr> template row once per entry in `rows`,
// substituting `fields[j]` with `rows[i][j]` inside the j-th <w:tc> that
// contains the placeholder.
//   - bookmark must be inside exactly one template <w:tr>
//   - clones go immediately after the template row, in input order
//   - the template row itself is removed after cloning (so rows.empty()
//     yields an empty table body)
//   - cloned rows have their <w:bookmarkStart/End> stripped to keep w:id unique

void DocxMerger::setTableRow(
    const std::string& bookmark,
    const std::vector<std::string>& fields,
    const std::vector<std::vector<std::string>>& rows)
{
    if (!loaded_) {
        throw ReportException(ReportError::CantOpenTemplate,
                              "setTableRow before load()");
    }

    // Validate row widths up-front so we never leave the document in a
    // half-mutated state on mismatched input.
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].size() != fields.size()) {
            throw ReportException(
                ReportError::InvalidField,
                fmt::format("setTableRow: rows[{}].size={} does not match fields.size={}",
                            i, rows[i].size(), fields.size()));
        }
    }

    const auto starts = find_bookmark_starts(bookmark);
    if (starts.empty()) {
        throw ReportException(
            ReportError::InvalidBookmark,
            fmt::format("bookmark not found: {}", bookmark));
    }

    pugi::xml_node template_tr = ancestor_named(starts.front(), "w:tr");
    if (!template_tr) {
        throw ReportException(
            ReportError::InvalidBookmark,
            fmt::format("bookmark '{}' is not inside a <w:tr> row", bookmark));
    }
    pugi::xml_node tbl = template_tr.parent();
    if (!tbl || std::strcmp(tbl.name(), "w:tbl") != 0) {
        throw ReportException(
            ReportError::InvalidBookmark,
            fmt::format("row for bookmark '{}' has no <w:tbl> parent", bookmark));
    }

    pugi::xml_node anchor = template_tr;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        pugi::xml_node clone = tbl.insert_copy_after(template_tr, anchor);
        strip_bookmarks(clone);

        // Replace each field inside the first <w:tc> that still contains it.
        // Per-cell scoping is required because replace_placeholder_in merges
        // all text into the first <w:t> of its input; spanning cells would
        // destroy their content.
        for (std::size_t j = 0; j < fields.size(); ++j) {
            bool replaced = false;
            for (auto tc = clone.child("w:tc"); tc;
                 tc = tc.next_sibling("w:tc")) {
                auto texts = collect_text_descendants(tc);
                if (replace_placeholder_in(texts, fields[j], rows[i][j])) {
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                throw ReportException(
                    ReportError::InvalidField,
                    fmt::format("field '{}' not found in any cell of row template",
                                fields[j]));
            }
        }
        anchor = clone;
    }

    tbl.remove_child(template_tr);
}

// ── setImage (Stage 5) ──────────────────────────────────────────────────────
//
// Inserts a <w:drawing> into the paragraph holding `bookmark`, dropping any
// text children the bookmark used to contain (so a template placeholder like
// "{{image}}" is fully replaced). Adds the PNG bytes as a new archive part
// `word/media/imageN.png`, registers a relationship in
// `word/_rels/document.xml.rels`, and ensures `[Content_Types].xml`
// advertises `png`.

namespace {

constexpr const char* kDocRelsPart     = "word/_rels/document.xml.rels";
constexpr const char* kContentTypesPart = "[Content_Types].xml";
constexpr const char* kImageRelType =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image";

// 914400 EMU/inch ÷ 96 px/inch. Word renders `<wp:extent>` at this scale
// by default — image appears at 1:1 with pixel size on a 96-DPI display.
constexpr std::uint32_t kEmuPerPx = 9525;

// Pick the next free `word/media/image{N}.<ext>` that doesn't collide with an
// existing archive part.
std::string next_media_name(
    const std::unordered_map<std::string, std::string>& parts,
    const std::string& ext /* "png" */)
{
    for (int n = 1; n < 10000; ++n) {
        auto candidate = fmt::format("word/media/image{}.{}", n, ext);
        if (parts.find(candidate) == parts.end()) return candidate;
    }
    throw ReportException(ReportError::SaveFailed,
                          "unable to allocate a free image part name");
}

// Parse existing rels file to find the highest rId numeric suffix so new
// relationships can safely append. Returns 0 when no numeric rIds are present.
int max_numeric_rid(pugi::xml_document& rels) {
    int m = 0;
    for (auto rel : rels.child("Relationships").children("Relationship")) {
        const auto id = rel.attribute("Id");
        if (!id) continue;
        const std::string v = id.value();
        if (v.size() > 3 && v.compare(0, 3, "rId") == 0) {
            try {
                const int n = std::stoi(v.substr(3));
                if (n > m) m = n;
            } catch (...) { /* non-numeric, skip */ }
        }
    }
    return m;
}

// Make sure `word/_rels/document.xml.rels` exists in the archive. Creates a
// minimal one when the template didn't ship any relationships.
void ensure_doc_rels_present(
    std::unordered_map<std::string, std::string>& parts)
{
    if (parts.find(kDocRelsPart) != parts.end()) return;
    parts[kDocRelsPart] =
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
        R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"/>)";
}

// Register a new <Relationship Id=... Type=... Target=.../> and return the id.
std::string add_image_relationship(
    std::unordered_map<std::string, std::string>& parts,
    const std::string& target_inside_word /* e.g. "media/image1.png" */)
{
    ensure_doc_rels_present(parts);
    pugi::xml_document rels;
    const auto& xml = parts[kDocRelsPart];
    if (!rels.load_buffer(xml.data(), xml.size())) {
        throw ReportException(
            ReportError::CantCopyDocxTemplate,
            "document.xml.rels is not valid XML");
    }
    auto root = rels.child("Relationships");
    if (!root) {
        root = rels.append_child("Relationships");
        root.append_attribute("xmlns").set_value(
            "http://schemas.openxmlformats.org/package/2006/relationships");
    }
    const int next_id = max_numeric_rid(rels) + 1;
    const std::string rid = fmt::format("rId{}", next_id);
    auto r = root.append_child("Relationship");
    r.append_attribute("Id").set_value(rid.c_str());
    r.append_attribute("Type").set_value(kImageRelType);
    r.append_attribute("Target").set_value(target_inside_word.c_str());

    std::ostringstream oss;
    rels.save(oss, "", pugi::format_raw);
    parts[kDocRelsPart] = oss.str();
    return rid;
}

// Ensure [Content_Types].xml has <Default Extension="png" ContentType="image/png"/>.
// No-op when the entry is already present; appends otherwise.
void ensure_content_type(
    std::unordered_map<std::string, std::string>& parts,
    const std::string& extension     /* "png" */,
    const std::string& content_type  /* "image/png" */)
{
    auto it = parts.find(kContentTypesPart);
    if (it == parts.end()) {
        throw ReportException(
            ReportError::CantCopyDocxTemplate,
            "[Content_Types].xml is missing from the archive");
    }
    pugi::xml_document ct;
    if (!ct.load_buffer(it->second.data(), it->second.size())) {
        throw ReportException(
            ReportError::CantCopyDocxTemplate,
            "[Content_Types].xml is not valid XML");
    }
    auto types = ct.child("Types");
    if (!types) {
        throw ReportException(
            ReportError::CantCopyDocxTemplate,
            "[Content_Types].xml has no <Types> root");
    }
    for (auto def : types.children("Default")) {
        const auto ext = def.attribute("Extension");
        if (ext && extension == ext.value()) return;  // already declared
    }
    auto def = types.append_child("Default");
    def.append_attribute("Extension").set_value(extension.c_str());
    def.append_attribute("ContentType").set_value(content_type.c_str());

    std::ostringstream oss;
    ct.save(oss, "", pugi::format_raw);
    it->second = oss.str();
}

// Compute a single uniform scale factor that maps (native_w, native_h)
// inside the user's ImageSize bounds, preserving aspect ratio.
//
// The problem reduces to a feasible range [s_lo, s_hi] where:
//   s_hi = min(max_w / w, max_h / h)   — tightest max constraint (∞ if none)
//   s_lo = max(min_w / w, min_h / h)   — tightest min constraint (0 if none)
//
// If s_lo > s_hi, the bounds contradict through the aspect ratio (e.g. user
// asked for `min_width_px=400, max_height_px=200` on a square image).
// We surface that as InvalidField rather than silently picking a scale.
// Otherwise we clamp the intrinsic 1.0 scale into [s_lo, s_hi].
double compute_display_scale(std::uint32_t w, std::uint32_t h,
                             const ImageSize& b)
{
    if (w == 0 || h == 0) {
        throw ReportException(ReportError::CantCopyDocxTemplate,
                              "image has zero dimensions");
    }

    double s_hi = std::numeric_limits<double>::infinity();
    if (b.max_width_px  > 0) s_hi = std::min(s_hi, static_cast<double>(b.max_width_px)  / w);
    if (b.max_height_px > 0) s_hi = std::min(s_hi, static_cast<double>(b.max_height_px) / h);

    double s_lo = 0.0;
    if (b.min_width_px  > 0) s_lo = std::max(s_lo, static_cast<double>(b.min_width_px)  / w);
    if (b.min_height_px > 0) s_lo = std::max(s_lo, static_cast<double>(b.min_height_px) / h);

    if (s_lo > s_hi) {
        throw ReportException(
            ReportError::InvalidField,
            fmt::format("ImageSize bounds have empty feasible range: "
                        "min scale {:.4f} > max scale {:.4f} for a "
                        "{}×{} image", s_lo, s_hi, w, h));
    }

    // Intrinsic scale = 1.0. Keep it if it's inside the feasible band,
    // otherwise clamp to the nearest border (the one the user actually hit).
    if (1.0 < s_lo) return s_lo;
    if (1.0 > s_hi) return s_hi;
    return 1.0;
}

// Build the inline <w:drawing> XML fragment that embeds a picture.
// Standard OOXML DrawingML boilerplate — the parameters are the only moving
// parts: unique doc-pr id, relationship id, and EMU-scale extent.
std::string make_drawing_xml(std::uint32_t uid,
                             const std::string& rid,
                             std::uint32_t cx_emu,
                             std::uint32_t cy_emu)
{
    return fmt::format(
R"(<w:drawing>
  <wp:inline distT="0" distB="0" distL="0" distR="0" xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing">
    <wp:extent cx="{cx}" cy="{cy}"/>
    <wp:effectExtent l="0" t="0" r="0" b="0"/>
    <wp:docPr id="{id}" name="Picture {id}"/>
    <wp:cNvGraphicFramePr>
      <a:graphicFrameLocks xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" noChangeAspect="1"/>
    </wp:cNvGraphicFramePr>
    <a:graphic xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main">
      <a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/picture">
        <pic:pic xmlns:pic="http://schemas.openxmlformats.org/drawingml/2006/picture">
          <pic:nvPicPr>
            <pic:cNvPr id="{id}" name="Picture {id}"/>
            <pic:cNvPicPr/>
          </pic:nvPicPr>
          <pic:blipFill>
            <a:blip xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" r:embed="{rid}"/>
            <a:stretch><a:fillRect/></a:stretch>
          </pic:blipFill>
          <pic:spPr>
            <a:xfrm>
              <a:off x="0" y="0"/>
              <a:ext cx="{cx}" cy="{cy}"/>
            </a:xfrm>
            <a:prstGeom prst="rect"><a:avLst/></a:prstGeom>
          </pic:spPr>
        </pic:pic>
      </a:graphicData>
    </a:graphic>
  </wp:inline>
</w:drawing>)",
        fmt::arg("cx", cx_emu),
        fmt::arg("cy", cy_emu),
        fmt::arg("id", uid),
        fmt::arg("rid", rid));
}

} // namespace

void DocxMerger::setImage(const std::string& bookmark,
                          const std::filesystem::path& image_path,
                          const ImageSize& bounds)
{
    if (!loaded_) {
        throw ReportException(ReportError::CantOpenTemplate,
                              "setImage before load()");
    }

    const auto starts = find_bookmark_starts(bookmark);
    if (starts.empty()) {
        throw ReportException(
            ReportError::InvalidBookmark,
            fmt::format("bookmark not found: {}", bookmark));
    }
    pugi::xml_node bmk_start = starts.front();
    pugi::xml_node para = ancestor_named(bmk_start, "w:p");
    if (!para) {
        throw ReportException(
            ReportError::InvalidBookmark,
            fmt::format("bookmark '{}' is not inside a <w:p> paragraph — "
                        "setImage requires a paragraph-scoped bookmark",
                        bookmark));
    }

    // Read + normalise the source image (PNG pass-through in Phase 1).
    const PngBuffer img = load_as_png(image_path);

    // Resolve bounds → single aspect-preserving scale factor. Must happen
    // BEFORE any archive mutation so an invalid ImageSize doesn't leave the
    // document with a dangling media part / relationship.
    const double scale = compute_display_scale(img.width, img.height, bounds);

    // Commit to the archive: pick media path, register relationship, ensure
    // content-type. Any throw past this point leaves the document untouched
    // from the caller's perspective (the archive mutations happen via parts_
    // which is only written back on save()).
    const std::string media_part = next_media_name(parts_, "png");
    parts_[media_part] = img.bytes;

    // Target inside a Word relationship is relative to the `word/` folder,
    // so strip the "word/" prefix when referencing the media.
    const std::string target_inside_word = media_part.substr(std::strlen("word/"));
    const std::string rid = add_image_relationship(parts_, target_inside_word);
    ensure_content_type(parts_, "png", "image/png");

    // Compute EMU extents (9525 EMU/px at 96 DPI) from the scaled pixel size.
    // Cap at a sane maximum so a pathological 50000×50000 image doesn't
    // produce values Word rejects.
    constexpr std::uint64_t kMaxEmu = 360'000'000ULL;  // ~394 inches
    const double scaled_w = static_cast<double>(img.width)  * scale;
    const double scaled_h = static_cast<double>(img.height) * scale;
    auto cx = static_cast<std::uint64_t>(std::llround(scaled_w * kEmuPerPx));
    auto cy = static_cast<std::uint64_t>(std::llround(scaled_h * kEmuPerPx));
    if (cx == 0) cx = kEmuPerPx;          // never emit a zero-size extent
    if (cy == 0) cy = kEmuPerPx;
    if (cx > kMaxEmu) cx = kMaxEmu;
    if (cy > kMaxEmu) cy = kMaxEmu;

    // Build the <w:drawing> once and parse it into the live document. A
    // counter-based uid prevents collisions with previously-inserted pictures.
    static std::uint32_t docPrCounter = 1000;
    const std::uint32_t uid = ++docPrCounter;
    const std::string draw_xml = make_drawing_xml(
        uid, rid, static_cast<std::uint32_t>(cx), static_cast<std::uint32_t>(cy));

    pugi::xml_document drawing_doc;
    // The fragment uses `w:`, `wp:`, `a:`, `pic:`, `r:` prefixes. These
    // namespaces are already declared on the root <w:document> element, so
    // parsing as a fragment with default options is safe.
    const auto parse_result = drawing_doc.load_buffer(
        draw_xml.data(), draw_xml.size(),
        pugi::parse_default | pugi::parse_ws_pcdata_single);
    if (!parse_result) {
        throw ReportException(
            ReportError::SaveFailed,
            fmt::format("internal: failed to parse drawing XML: {}",
                        parse_result.description()));
    }

    // Strip all <w:r> inside the paragraph whose bookmark we hit. This covers
    // the common case of a placeholder like "{{image}}" inside the bookmark —
    // the text runs are replaced in-full by the picture.
    std::vector<pugi::xml_node> to_remove;
    for (auto child = para.first_child(); child; child = child.next_sibling()) {
        const char* nm = child.name();
        if (std::strcmp(nm, "w:r") == 0) to_remove.push_back(child);
    }
    for (auto n : to_remove) para.remove_child(n);

    // Insert a fresh <w:r> containing the <w:drawing> right before the
    // bookmarkEnd (fallback: after bookmarkStart) so it lands between the
    // bookmark markers.
    pugi::xml_node bmk_end;
    const auto bmk_id = bmk_start.attribute("w:id");
    if (bmk_id) {
        for (auto sib = bmk_start; sib; sib = sib.next_sibling()) {
            if (std::strcmp(sib.name(), "w:bookmarkEnd") == 0) {
                const auto id = sib.attribute("w:id");
                if (id && std::strcmp(id.value(), bmk_id.value()) == 0) {
                    bmk_end = sib;
                    break;
                }
            }
        }
    }
    pugi::xml_node run = bmk_end
        ? para.insert_child_before("w:r", bmk_end)
        : para.insert_child_after("w:r", bmk_start);
    run.append_copy(drawing_doc.first_child());
}

// ── setChartValue (Stage 4) ─────────────────────────────────────────────────
//
// Updates a single numeric point inside an embedded chart. The flow:
//
//   bookmark → <w:p> ancestor → descendant <c:chart r:id="rIdX"/>
//            → resolve rIdX via word/_rels/document.xml.rels → chart part
//            → parse XML → match (series, category) in any series container
//              that uses the <c:cat>/<c:val> shape (bar/line/pie/area/…)
//            → rewrite <c:val>/<c:numRef>/<c:numCache>/<c:pt idx=N>/<c:v>
//
// The `field` parameter is accepted for API symmetry with the other setters
// but does not participate in the match — callers typically pass a literal
// constant that never varies per call, so using it for selection would just
// couple implementation to templates without adding discriminating power.
//
// Embedded spreadsheets (word/embeddings/*.xlsx) are not touched. Word may
// rebuild the chart cache from them when the template is manually edited;
// report templates should keep the embedding read-only or omit it.

namespace {

// Look up the Target of the Relationship with matching Id in the given rels
// XML blob. Returns the empty string when the rId isn't present.
std::string resolve_doc_rid(const std::string& rels_xml,
                            const std::string& rid)
{
    pugi::xml_document rels;
    if (!rels.load_buffer(rels_xml.data(), rels_xml.size())) return {};
    for (auto rel : rels.child("Relationships").children("Relationship")) {
        if (rid == rel.attribute("Id").value()) {
            return rel.attribute("Target").value();
        }
    }
    return {};
}

// Find the first <c:chart> node reachable from `root` (inclusive). pugi is
// xmlns-unaware so we compare the literal prefixed tag name.
pugi::xml_node find_descendant_chart(pugi::xml_node root) {
    struct Finder : pugi::xml_tree_walker {
        pugi::xml_node hit;
        bool for_each(pugi::xml_node& n) override {
            if (std::strcmp(n.name(), "c:chart") == 0) {
                hit = n;
                return false;
            }
            return true;
        }
    } f;
    root.traverse(f);
    return f.hit;
}

// Read <c:pt idx="0"><c:v>NAME</c:v></c:pt> — the canonical layout for a
// series display name under <c:tx>/<c:strRef>/<c:strCache>.
std::string read_series_name(pugi::xml_node ser) {
    auto pt = ser.child("c:tx").child("c:strRef").child("c:strCache").child("c:pt");
    if (!pt) {
        // Rare: literal <c:tx><c:v>Name</c:v></c:tx> form.
        return ser.child("c:tx").child_value("c:v");
    }
    return pt.child_value("c:v");
}

// Locate the <c:pt idx="I"> whose <c:v> text equals `category` inside the
// series' <c:cat> block. Categories may be string- or numeric-typed; we try
// both. Returns -1 when no match.
int find_category_idx(pugi::xml_node ser, const std::string& category) {
    auto cat = ser.child("c:cat");
    if (!cat) return -1;
    auto cache = cat.child("c:strRef").child("c:strCache");
    if (!cache) cache = cat.child("c:numRef").child("c:numCache");
    if (!cache) return -1;
    for (auto pt : cache.children("c:pt")) {
        if (category == pt.child_value("c:v")) {
            return pt.attribute("idx").as_int(-1);
        }
    }
    return -1;
}

// Rewrite the numeric cache point at `cat_idx` for `value`. Throws
// CantCopyDocxTemplate if the cache shape is unexpected.
void write_value_point(pugi::xml_node ser, int cat_idx, double value) {
    auto cache = ser.child("c:val").child("c:numRef").child("c:numCache");
    if (!cache) {
        throw ReportException(
            ReportError::CantCopyDocxTemplate,
            "series is missing <c:val>/<c:numRef>/<c:numCache>");
    }
    std::ostringstream ss;
    ss.imbue(std::locale::classic());
    ss << value;
    const std::string formatted = ss.str();
    for (auto pt : cache.children("c:pt")) {
        if (pt.attribute("idx").as_int(-1) == cat_idx) {
            if (auto v = pt.child("c:v")) {
                v.text().set(formatted.c_str());
            } else {
                pt.append_child("c:v").text().set(formatted.c_str());
            }
            return;
        }
    }
    throw ReportException(
        ReportError::CantCopyDocxTemplate,
        fmt::format("no <c:val>/<c:pt idx='{}'> to update", cat_idx));
}

bool is_supported_chart_type(std::string_view tag) {
    return tag == "c:barChart"      || tag == "c:lineChart"
        || tag == "c:pieChart"      || tag == "c:areaChart"
        || tag == "c:doughnutChart" || tag == "c:radarChart"
        || tag == "c:ofPieChart"
        || tag == "c:bar3DChart"    || tag == "c:line3DChart"
        || tag == "c:pie3DChart"    || tag == "c:area3DChart";
}

bool is_unsupported_xy_chart(std::string_view tag) {
    return tag == "c:scatterChart" || tag == "c:bubbleChart"
        || tag == "c:stockChart"
        || tag == "c:surfaceChart" || tag == "c:surface3DChart";
}

} // namespace

void DocxMerger::setChartValue(const std::string& bookmark,
                               const std::string& /*field*/,
                               const std::string& series,
                               const std::string& category,
                               double             value)
{
    if (!loaded_) {
        throw ReportException(ReportError::CantOpenTemplate,
                              "setChartValue before load()");
    }

    const auto starts = find_bookmark_starts(bookmark);
    if (starts.empty()) {
        throw ReportException(
            ReportError::InvalidBookmark,
            fmt::format("bookmark not found: {}", bookmark));
    }
    pugi::xml_node para = ancestor_named(starts.front(), "w:p");
    if (!para) {
        throw ReportException(
            ReportError::InvalidBookmark,
            fmt::format("bookmark '{}' is not inside a <w:p> paragraph",
                        bookmark));
    }

    pugi::xml_node chart_ref = find_descendant_chart(para);
    if (!chart_ref) {
        throw ReportException(
            ReportError::InvalidBookmark,
            fmt::format("bookmark '{}' has no <c:chart> inside its paragraph",
                        bookmark));
    }
    const std::string rid = chart_ref.attribute("r:id").value();
    if (rid.empty()) {
        throw ReportException(
            ReportError::CantCopyDocxTemplate,
            "<c:chart> element missing r:id attribute");
    }

    auto rels_it = parts_.find(kDocRelsPart);
    if (rels_it == parts_.end()) {
        throw ReportException(
            ReportError::CantCopyDocxTemplate,
            "document.xml.rels missing — cannot resolve chart rId");
    }
    const std::string target = resolve_doc_rid(rels_it->second, rid);
    if (target.empty()) {
        throw ReportException(
            ReportError::CantCopyDocxTemplate,
            fmt::format("relationship '{}' not declared in document.xml.rels", rid));
    }

    // Relationship Target is relative to word/ — e.g. "charts/chart1.xml".
    const std::string chart_part = "word/" + target;
    auto chart_it = parts_.find(chart_part);
    if (chart_it == parts_.end()) {
        throw ReportException(
            ReportError::CantCopyDocxTemplate,
            fmt::format("chart part not in archive: {}", chart_part));
    }

    pugi::xml_document chart_doc;
    if (!chart_doc.load_buffer(chart_it->second.data(), chart_it->second.size())) {
        throw ReportException(
            ReportError::CantCopyDocxTemplate,
            fmt::format("chart part is not valid XML: {}", chart_part));
    }
    auto plot_area = chart_doc.child("c:chartSpace").child("c:chart").child("c:plotArea");
    if (!plot_area) {
        throw ReportException(
            ReportError::CantCopyDocxTemplate,
            "chart has no <c:plotArea>");
    }

    bool saw_unsupported = false;
    for (auto type_node : plot_area.children()) {
        const std::string_view tag = type_node.name();
        if (is_unsupported_xy_chart(tag)) {
            saw_unsupported = true;
            continue;
        }
        if (!is_supported_chart_type(tag)) continue;

        for (auto ser : type_node.children("c:ser")) {
            if (read_series_name(ser) != series) continue;

            const int cat_idx = find_category_idx(ser, category);
            if (cat_idx < 0) {
                throw ReportException(
                    ReportError::InvalidField,
                    fmt::format("category '{}' not found in series '{}'",
                                category, series));
            }
            write_value_point(ser, cat_idx, value);

            std::ostringstream oss;
            chart_doc.save(oss, "", pugi::format_raw);
            parts_[chart_part] = oss.str();
            return;
        }
    }

    if (saw_unsupported) {
        throw ReportException(
            ReportError::NotImplemented,
            "chart uses scatter/bubble/stock/surface plot — setChartValue only "
            "supports cat/val shapes (bar/line/pie/area/radar/doughnut/3D variants)");
    }
    throw ReportException(
        ReportError::InvalidField,
        fmt::format("series '{}' not found in chart '{}'", series, chart_part));
}

// ── paste ───────────────────────────────────────────────────────────────────
//
// v1 semantics: paste() commits the current staged values. Because
// setClipboardValue() writes directly into the DOM, paste() is essentially
// a bookmark-existence check plus a record in pasted_bookmarks() that
// tests can assert against.
//
// A later stage will extend this to clone the section between
// bookmarkStart/bookmarkEnd (needed for table-row and page repetition).

void DocxMerger::paste(const std::string& bookmark) {
    if (!loaded_) {
        throw ReportException(ReportError::CantOpenTemplate,
                              "paste() before load()");
    }
    const auto starts = find_bookmark_starts(bookmark);
    if (starts.empty()) {
        throw ReportException(
            ReportError::InvalidBookmark,
            fmt::format("bookmark not found: {}", bookmark));
    }
    pasted_.push_back(bookmark);
}

} // namespace textfabric::docx

// ── factory ────────────────────────────────────────────────────────────────

namespace textfabric {

std::unique_ptr<IReportMerger> make_docx_merger() {
    return std::make_unique<docx::DocxMerger>();
}

} // namespace textfabric
