#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace tf_test {

/// A (zip-path, bytes) entry that will be packed into a DOCX.
struct DocxPart {
    std::string                     name;  ///< e.g. "word/document.xml"
    std::string                     data;  ///< file bytes
};

/// Generate a minimal valid .docx at `out` containing `parts`.
/// The caller is responsible for supplying at least [Content_Types].xml,
/// _rels/.rels, word/_rels/document.xml.rels, word/document.xml.
void write_docx(const std::filesystem::path& out,
                const std::vector<DocxPart>& parts);

/// Convenience: writes a minimal DOCX containing the given body XML.
/// The body XML must be the contents of the root <w:body> element
/// (i.e. a sequence of <w:p>, <w:tbl>, <w:bookmarkStart/End> etc.).
void write_minimal_docx(const std::filesystem::path& out,
                        std::string_view             body_xml);

/// Minimal DOCX with a 2-column table (header row + template row).
/// Template row has bookmark `TableRow` covering both cells and two
/// placeholders — `{{name}}` in column 0, `{{score}}` in column 1.
void write_table_template_docx(const std::filesystem::path& out);

/// Chart types the fixture generator can emit. Maps to the top-level element
/// under <c:plotArea> — bar/line/pie/area share the <c:cat>/<c:val> shape,
/// scatter uses <c:xVal>/<c:yVal> and is used to verify NotImplemented paths.
enum class ChartKind { Bar, Line, Pie, Area, Scatter };

/// Parameterises the chart DOCX fixture. Default = 2×2 bar chart.
struct ChartFixture {
    ChartKind   kind            = ChartKind::Bar;
    std::string bookmark_name   = "kStatChart";
    std::vector<std::string>          series_names    = {"Sales", "Costs"};
    std::vector<std::string>          category_names  = {"Q1",    "Q2"};
    std::vector<std::vector<double>>  data            = {{10, 20}, {5, 15}};

    /// When non-empty, build_chart_xml() emits a <c:title> with this text,
    /// styled with a distinctive <a:rPr>/<a:pPr>/<a:bodyPr> so a test can
    /// assert that setChartTitle preserves the existing formatting instead
    /// of just the new text.
    std::string initial_title;

    /// When true, series_names[0]'s <c:ser> gets a <c:dPt idx="0"> color
    /// override — used to verify setChartData clears stale per-point
    /// overrides on reshape instead of leaving them pointing at whatever
    /// category now happens to sit at that index.
    bool with_series0_data_point_override = false;
};

/// Minimal DOCX with one embedded chart. The bookmark `fx.bookmark_name`
/// wraps a <w:drawing> that references `word/charts/chart1.xml` via rIdChart1.
/// Content_Types includes the chart override; document rels point at the
/// chart part. `fx.data` is a series-major matrix (data[s][c] = value for
/// series s and category c) — sizes must match series_names × category_names.
void write_chart_template_docx(const std::filesystem::path& out,
                               const ChartFixture&          fx = {});

/// Same as write_chart_template_docx, but also embeds a minimal .xlsx
/// workbook at word/embeddings/Microsoft_Excel_Worksheet1.xlsx, wired up
/// via a <c:externalData> relationship on the chart part (chart1.xml.rels)
/// — the shape setChartData's embedded-workbook sync path looks for. The
/// workbook's Sheet1 seeds A1:C3 with fx's default categories/series so a
/// test can assert on either the pre- or post-setChartData content.
void write_chart_template_docx_with_workbook(const std::filesystem::path& out,
                                             const ChartFixture&          fx = {});

/// A valid 2×2 RGB PNG (75 bytes, colours: red/green/blue/white).
/// Generated once by hand-crafting IHDR + IDAT + IEND chunks with correct
/// CRCs; embedded here so tests remain self-contained (no binary fixtures
/// tracked in git). Width and height are both 2.
[[nodiscard]] std::string_view tiny_png_bytes();

/// Write the tiny PNG returned by `tiny_png_bytes()` to disk at `path` and
/// return the same path for chaining. Creates parent directories as needed.
std::filesystem::path write_tiny_png(const std::filesystem::path& path);

/// A valid 2×2 24-bit BMP (70 bytes). Uses the same bottom-up row layout and
/// 4-byte row alignment that all BMPs ≥ Win 3.x carry.
[[nodiscard]] std::string_view tiny_bmp_bytes();

std::filesystem::path write_tiny_bmp(const std::filesystem::path& path);

} // namespace tf_test
