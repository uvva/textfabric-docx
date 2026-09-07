#pragma once

#include "textfabric/export.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace textfabric {

/// Display-size bounds for `setImage`. All four fields are in pixels at a
/// 96 DPI baseline (the same unit Word uses when importing images through
/// drag-and-drop). A value of `0` means "no constraint on this dimension".
/// Aspect ratio is always preserved — the merger picks a single uniform
/// scale that satisfies every non-zero bound simultaneously.
///
/// Resolution rules (see `IReportMerger::setImage` for the full algorithm):
///   - all zeros    → image is inserted at its intrinsic pixel size;
///   - image fits   → intrinsic size is kept;
///   - image bigger → shrunk to the tightest of the max bounds;
///   - image smaller → enlarged to the loosest of the min bounds;
///   - min > max    → throws `ReportError::InvalidField` (conflicting bounds).
struct ImageSize {
    std::uint32_t min_width_px  = 0;
    std::uint32_t min_height_px = 0;
    std::uint32_t max_width_px  = 0;
    std::uint32_t max_height_px = 0;
};

/// Abstract merger API for filling DOCX templates by bookmark name.
///
/// Implementations:
///   - docx::DocxMerger — uses libzip + pugixml directly.
///
/// All string parameters are UTF-8.
class TEXTFABRIC_API IReportMerger {
public:
    virtual ~IReportMerger() = default;

    /// Set the code page used for input/output. Only "UTF-8" is
    /// supported in v1 — anything else throws ReportException.
    virtual void setCodePage(const std::string& code_page) = 0;

    /// Load a DOCX template from disk.
    /// Throws ReportException{CantOpenTemplate | CantCopyDocxTemplate}.
    virtual void load(const std::string& path) = 0;

    /// Save the current document to disk.
    /// Format is inferred from the file extension:
    ///   .docx → native save
    ///   .pdf / .html → routed through LibreOffice (Stage 6 — throws NotImplemented in v1)
    /// Throws ReportException{SaveFailed | NoConverter | NotImplemented}.
    virtual void save(const std::string& path) = 0;

    /// Substitute a text field inside a bookmark.
    /// `bookmark` — DOCX bookmark name (e.g. "_Header.User")
    /// `field`    — placeholder token inside the bookmark (e.g. "{{user}}" or "kUSER")
    /// `value`    — replacement string (UTF-8)
    /// Throws ReportException{InvalidBookmark | InvalidField}.
    virtual void setClipboardValue(const std::string& bookmark,
                                   const std::string& field,
                                   const std::string& value) = 0;

    /// Clone a table row and fill its cells.
    ///
    /// The bookmark must live inside a `<w:tr>` template row that serves as
    /// the prototype. For each element of `rows`, the row is cloned and the
    /// `fields[j]` placeholder in column j is replaced by `rows[i][j]`.
    /// The template row itself is removed after cloning; an empty `rows`
    /// therefore yields an empty body for that table. Bookmarks inside
    /// cloned rows are stripped to avoid duplicate `w:id` attributes.
    ///
    /// `bookmark` — DOCX bookmark inside the template `<w:tr>`.
    /// `fields`   — column placeholders (e.g. `{"{{name}}", "{{score}}"}`).
    /// `rows`     — values; `rows[i].size()` must equal `fields.size()`.
    /// Throws ReportException{InvalidBookmark | InvalidField}.
    virtual void setTableRow(const std::string& bookmark,
                             const std::vector<std::string>& fields,
                             const std::vector<std::vector<std::string>>& rows) = 0;

    /// Embed a raster image at `bookmark`.
    ///
    /// Accepted formats are detected by magic bytes:
    ///   - PNG: passes through as-is and goes into `word/media/image*.png`.
    ///   - JPEG / BMP / TIFF: transcoded to PNG at call time
    ///     (requires the stb_image + libtiff support enabled in the build).
    ///
    /// The merger inserts a `<w:drawing>` element inside the paragraph that
    /// holds `bookmark`, displacing any text the bookmark already contained
    /// (so the template can carry a placeholder like "{{image}}" that gets
    /// fully replaced by the picture). A new `<Relationship Id="rIdX" ... />`
    /// is added to `word/_rels/document.xml.rels` and `[Content_Types].xml`
    /// gains a `<Default Extension="png" ContentType="image/png"/>` entry
    /// if not already present.
    ///
    /// Image dimensions are read from the source file; the displayed size in
    /// `<wp:extent>` defaults to 1 px ≈ 9525 EMU (96 DPI baseline). The
    /// optional `bounds` argument clamps the displayed size between user-
    /// supplied minima/maxima while preserving aspect ratio — see `ImageSize`.
    ///
    /// Throws `ReportException`:
    ///   - InvalidBookmark:        bookmark not found or not inside `<w:p>`
    ///   - InvalidField:           `bounds` are internally contradictory
    ///                             (e.g. min_width > max_width, or the
    ///                             aspect-preserving scale range is empty)
    ///   - CantOpenTemplate:       image file missing / unreadable
    ///   - CantCopyDocxTemplate:   file is not a recognised image format
    ///   - NotImplemented:         JPEG/BMP/TIFF hit the baseline (PNG-only)
    ///                             build — convert to PNG first or enable the
    ///                             Phase 2 decoders.
    virtual void setImage(const std::string& bookmark,
                          const std::filesystem::path& image_path,
                          const ImageSize& bounds = {}) = 0;

    /// Update a numeric value inside an embedded chart.
    /// v1: not implemented — throws ReportException{NotImplemented}.
    virtual void setChartValue(const std::string& bookmark,
                               const std::string& field,
                               const std::string& series,
                               const std::string& category,
                               double             value) = 0;

    /// Paste/activate a template section anchored at a bookmark.
    /// Calling paste() materializes the currently-staged field values
    /// into the document.
    /// Throws ReportException{InvalidBookmark}.
    virtual void paste(const std::string& bookmark) = 0;
};

/// Factory: builds a DOCX-backed merger.
TEXTFABRIC_API [[nodiscard]] std::unique_ptr<IReportMerger> make_docx_merger();

} // namespace textfabric
