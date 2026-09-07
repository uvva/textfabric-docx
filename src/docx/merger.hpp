#pragma once

#include "textfabric/merger.hpp"

#include <pugixml.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace textfabric::docx {

/// Name of the main document part inside every .docx archive.
inline constexpr const char* kDocumentPart = "word/document.xml";

/// In-memory representation of a loaded .docx.
/// We keep the full archive contents as a (path → bytes) map so we can
/// rewrite any part and repack on save() while preserving everything else
/// (styles, fonts, images, chart xml, etc.).
class DocxMerger final : public IReportMerger {
public:
    DocxMerger();
    ~DocxMerger() override;

    // ── IReportMerger ────────────────────────────────────────────────────
    void setCodePage(const std::string& code_page) override;
    void load(const std::string& path) override;
    void save(const std::string& path) override;

    void setClipboardValue(const std::string& bookmark,
                           const std::string& field,
                           const std::string& value) override;

    void setTableRow(const std::string& bookmark,
                     const std::vector<std::string>& fields,
                     const std::vector<std::vector<std::string>>& rows) override;

    void setImage(const std::string& bookmark,
                  const std::filesystem::path& image_path,
                  const ImageSize& bounds = {}) override;

    void setChartValue(const std::string& bookmark,
                       const std::string& field,
                       const std::string& series,
                       const std::string& category,
                       double             value) override;

    void paste(const std::string& bookmark) override;

    // ── Introspection (used by tests) ────────────────────────────────────
    [[nodiscard]] bool loaded() const noexcept { return loaded_; }
    [[nodiscard]] std::string document_xml() const;
    [[nodiscard]] const std::vector<std::string>& pasted_bookmarks() const noexcept {
        return pasted_;
    }

private:
    /// Raw archive: part name (zip path) → file bytes.
    std::unordered_map<std::string, std::string> parts_;

    /// Parsed word/document.xml (re-serialized into parts_ on save()).
    pugi::xml_document document_;

    bool        loaded_     = false;
    std::string code_page_  = "UTF-8";
    std::vector<std::string> pasted_;

    // ── Internals ────────────────────────────────────────────────────────
    void read_archive(const std::filesystem::path& path);
    void write_archive(const std::filesystem::path& path) const;
    void reparse_document();
    void reserialize_document();

    /// Find all <w:bookmarkStart> nodes matching `name`.
    /// Returns nodes in document order.
    std::vector<pugi::xml_node> find_bookmark_starts(const std::string& name) const;
};

} // namespace textfabric::docx
