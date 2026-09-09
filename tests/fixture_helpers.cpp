#include "fixture_helpers.hpp"

#include <zip.h>

#include <array>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace tf_test {

void write_docx(const std::filesystem::path& out,
                const std::vector<DocxPart>& parts)
{
    std::error_code ec;
    std::filesystem::remove(out, ec);

    int err = 0;
    zip_t* z = zip_open(out.string().c_str(), ZIP_CREATE | ZIP_EXCL, &err);
    if (!z) throw std::runtime_error("zip_open(create) failed for fixture");

    for (const auto& p : parts) {
        zip_source_t* src = zip_source_buffer(z, p.data.data(), p.data.size(), 0);
        if (!src) {
            zip_discard(z);
            throw std::runtime_error("zip_source_buffer failed for " + p.name);
        }
        if (zip_file_add(z, p.name.c_str(), src,
                          ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8) < 0) {
            zip_source_free(src);
            zip_discard(z);
            throw std::runtime_error("zip_file_add failed for " + p.name);
        }
    }

    if (zip_close(z) != 0) {
        zip_discard(z);
        throw std::runtime_error("zip_close failed for fixture");
    }
}

void write_minimal_docx(const std::filesystem::path& out,
                        std::string_view             body_xml)
{
    // [Content_Types].xml — minimal viable content-types mapping.
    static constexpr const char* kContentTypes = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
  <Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
</Types>)";

    // Package-level relationships pointing to word/document.xml
    static constexpr const char* kPackageRels = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
</Relationships>)";

    // Document-level relationships — empty for this fixture.
    static constexpr const char* kDocRels = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
</Relationships>)";

    std::string document;
    document.reserve(512 + body_xml.size());
    document += R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">
  <w:body>
)";
    document.append(body_xml);
    document += R"(
  </w:body>
</w:document>)";

    std::vector<DocxPart> parts = {
        {"[Content_Types].xml",            kContentTypes},
        {"_rels/.rels",                    kPackageRels},
        {"word/_rels/document.xml.rels",   kDocRels},
        {"word/document.xml",              std::move(document)},
    };
    write_docx(out, parts);
}

void write_table_template_docx(const std::filesystem::path& out)
{
    static constexpr const char* kTableBody = R"(
    <w:tbl>
      <w:tblPr><w:tblW w:w="0" w:type="auto"/></w:tblPr>
      <w:tblGrid>
        <w:gridCol w:w="4000"/>
        <w:gridCol w:w="4000"/>
      </w:tblGrid>
      <w:tr>
        <w:tc><w:tcPr><w:tcW w:w="4000" w:type="dxa"/></w:tcPr>
          <w:p><w:r><w:rPr><w:b/></w:rPr><w:t>Name</w:t></w:r></w:p>
        </w:tc>
        <w:tc><w:tcPr><w:tcW w:w="4000" w:type="dxa"/></w:tcPr>
          <w:p><w:r><w:rPr><w:b/></w:rPr><w:t>Score</w:t></w:r></w:p>
        </w:tc>
      </w:tr>
      <w:tr>
        <w:tc><w:tcPr><w:tcW w:w="4000" w:type="dxa"/></w:tcPr>
          <w:p>
            <w:bookmarkStart w:id="10" w:name="TableRow"/>
            <w:r><w:t>{{name}}</w:t></w:r>
          </w:p>
        </w:tc>
        <w:tc><w:tcPr><w:tcW w:w="4000" w:type="dxa"/></w:tcPr>
          <w:p>
            <w:r><w:t>{{score}}</w:t></w:r>
            <w:bookmarkEnd w:id="10"/>
          </w:p>
        </w:tc>
      </w:tr>
    </w:tbl>
)";
    write_minimal_docx(out, kTableBody);
}

// ─── Chart fixture ──────────────────────────────────────────────────────────

namespace {

const char* chart_element_name(ChartKind k) {
    switch (k) {
        case ChartKind::Bar:     return "c:barChart";
        case ChartKind::Line:    return "c:lineChart";
        case ChartKind::Pie:     return "c:pieChart";
        case ChartKind::Area:    return "c:areaChart";
        case ChartKind::Scatter: return "c:scatterChart";
    }
    return "c:barChart";
}

std::string format_double(double v) {
    std::ostringstream ss;
    ss.imbue(std::locale::classic());
    ss << v;
    return ss.str();
}

std::string build_ser_cat_val(const ChartFixture& fx, std::size_t s_idx) {
    std::ostringstream ss;
    ss << "<c:ser>"
       << "<c:idx val=\"" << s_idx << "\"/>"
       << "<c:order val=\"" << s_idx << "\"/>"
       << "<c:tx><c:strRef><c:f>Sheet1!$B$1</c:f><c:strCache>"
       << "<c:ptCount val=\"1\"/>"
       << "<c:pt idx=\"0\"><c:v>" << fx.series_names[s_idx] << "</c:v></c:pt>"
       << "</c:strCache></c:strRef></c:tx>"
       << "<c:cat><c:strRef><c:f>Sheet1!$A$2:$A$" << (fx.category_names.size() + 1)
       << "</c:f><c:strCache>"
       << "<c:ptCount val=\"" << fx.category_names.size() << "\"/>";
    for (std::size_t i = 0; i < fx.category_names.size(); ++i) {
        ss << "<c:pt idx=\"" << i << "\"><c:v>" << fx.category_names[i] << "</c:v></c:pt>";
    }
    ss << "</c:strCache></c:strRef></c:cat>"
       << "<c:val><c:numRef><c:f>Sheet1!$B$2:$B$" << (fx.category_names.size() + 1)
       << "</c:f><c:numCache>"
       << "<c:formatCode>General</c:formatCode>"
       << "<c:ptCount val=\"" << fx.category_names.size() << "\"/>";
    for (std::size_t i = 0; i < fx.category_names.size(); ++i) {
        ss << "<c:pt idx=\"" << i << "\"><c:v>"
           << format_double(fx.data[s_idx][i]) << "</c:v></c:pt>";
    }
    ss << "</c:numCache></c:numRef></c:val>"
       << "</c:ser>";
    return ss.str();
}

std::string build_ser_xval_yval(const ChartFixture& fx, std::size_t s_idx) {
    std::ostringstream ss;
    ss << "<c:ser>"
       << "<c:idx val=\"" << s_idx << "\"/>"
       << "<c:order val=\"" << s_idx << "\"/>"
       << "<c:tx><c:strRef><c:f>Sheet1!$B$1</c:f><c:strCache>"
       << "<c:ptCount val=\"1\"/>"
       << "<c:pt idx=\"0\"><c:v>" << fx.series_names[s_idx] << "</c:v></c:pt>"
       << "</c:strCache></c:strRef></c:tx>"
       << "<c:xVal><c:numRef><c:f>Sheet1!$A$2:$A$" << (fx.category_names.size() + 1)
       << "</c:f><c:numCache>"
       << "<c:ptCount val=\"" << fx.category_names.size() << "\"/>";
    for (std::size_t i = 0; i < fx.category_names.size(); ++i) {
        ss << "<c:pt idx=\"" << i << "\"><c:v>" << i + 1 << "</c:v></c:pt>";
    }
    ss << "</c:numCache></c:numRef></c:xVal>"
       << "<c:yVal><c:numRef><c:f>Sheet1!$B$2:$B$" << (fx.category_names.size() + 1)
       << "</c:f><c:numCache>"
       << "<c:ptCount val=\"" << fx.category_names.size() << "\"/>";
    for (std::size_t i = 0; i < fx.category_names.size(); ++i) {
        ss << "<c:pt idx=\"" << i << "\"><c:v>"
           << format_double(fx.data[s_idx][i]) << "</c:v></c:pt>";
    }
    ss << "</c:numCache></c:numRef></c:yVal>"
       << "</c:ser>";
    return ss.str();
}

std::string build_chart_xml(const ChartFixture& fx, bool with_external_data = false) {
    const char* elem   = chart_element_name(fx.kind);
    const bool is_bar  = (fx.kind == ChartKind::Bar);
    const bool is_pie  = (fx.kind == ChartKind::Pie);
    const bool is_scat = (fx.kind == ChartKind::Scatter);

    std::ostringstream ss;
    ss << R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
       << R"(<c:chartSpace xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart")"
       << R"( xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main")"
       << R"( xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">)"
       << "<c:chart><c:plotArea><c:layout/>"
       << "<" << elem << ">";

    if (is_bar)  ss << R"(<c:barDir val="col"/><c:grouping val="clustered"/>)";
    if (is_pie)  ss << R"(<c:varyColors val="1"/>)";
    if (is_scat) ss << R"(<c:scatterStyle val="lineMarker"/>)";

    for (std::size_t s = 0; s < fx.series_names.size(); ++s) {
        ss << (is_scat ? build_ser_xval_yval(fx, s)
                       : build_ser_cat_val(fx, s));
    }

    if (!is_pie) {
        ss << R"(<c:axId val="111111111"/><c:axId val="222222222"/>)";
    }
    ss << "</" << elem << ">";

    if (!is_pie) {
        ss << R"(<c:catAx><c:axId val="111111111"/>)"
           << R"(<c:scaling><c:orientation val="minMax"/></c:scaling>)"
           << R"(<c:delete val="0"/><c:axPos val="b"/><c:crossAx val="222222222"/></c:catAx>)"
           << R"(<c:valAx><c:axId val="222222222"/>)"
           << R"(<c:scaling><c:orientation val="minMax"/></c:scaling>)"
           << R"(<c:delete val="0"/><c:axPos val="l"/><c:crossAx val="111111111"/></c:valAx>)";
    }

    ss << "</c:plotArea></c:chart>";
    if (with_external_data) {
        ss << R"(<c:externalData r:id="rIdData"><c:autoUpdate val="0"/></c:externalData>)";
    }
    ss << "</c:chartSpace>";
    return ss.str();
}

std::string build_document_with_chart(const std::string& bookmark_name) {
    std::ostringstream ss;
    ss << R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
       << R"(<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main")"
       << R"( xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships")"
       << R"( xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing")"
       << R"( xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main")"
       << R"( xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart">)"
       << "<w:body><w:p>"
       << R"(<w:bookmarkStart w:id="100" w:name=")" << bookmark_name << R"("/>)"
       << R"(<w:r><w:drawing>)"
       << R"(<wp:inline distT="0" distB="0" distL="0" distR="0">)"
       << R"(<wp:extent cx="5486400" cy="3200400"/>)"
       << R"(<wp:docPr id="1" name="Chart 1"/>)"
       << R"(<wp:cNvGraphicFramePr/>)"
       << R"(<a:graphic>)"
       << R"(<a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/chart">)"
       << R"(<c:chart r:id="rIdChart1"/>)"
       << R"(</a:graphicData></a:graphic></wp:inline>)"
       << R"(</w:drawing></w:r>)"
       << R"(<w:bookmarkEnd w:id="100"/>)"
       << "</w:p></w:body></w:document>";
    return ss.str();
}

constexpr const char* kChartContentTypes = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
  <Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
  <Override PartName="/word/charts/chart1.xml" ContentType="application/vnd.openxmlformats-officedocument.drawingml.chart+xml"/>
</Types>)";

constexpr const char* kPackageRels = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
</Relationships>)";

constexpr const char* kDocRelsWithChart = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rIdChart1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/chart" Target="charts/chart1.xml"/>
</Relationships>)";

constexpr const char* kChartContentTypesWithWorkbook = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
  <Default Extension="xlsx" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"/>
  <Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
  <Override PartName="/word/charts/chart1.xml" ContentType="application/vnd.openxmlformats-officedocument.drawingml.chart+xml"/>
</Types>)";

constexpr const char* kChartRelsWithExternalData = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rIdData" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/package" Target="../embeddings/Microsoft_Excel_Worksheet1.xlsx"/>
</Relationships>)";

// ─── Minimal embedded workbook (nested .xlsx) ──────────────────────────────

constexpr const char* kXlsxContentTypes = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
  <Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>
  <Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>
</Types>)";

constexpr const char* kXlsxPackageRels = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>
</Relationships>)";

constexpr const char* kXlsxWorkbook = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
  <sheets>
    <sheet name="Sheet1" sheetId="1" r:id="rIdSheet1"/>
  </sheets>
</workbook>)";

constexpr const char* kXlsxWorkbookRels = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rIdSheet1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>
</Relationships>)";

// Mirrors ChartFixture's default (Sales/Costs × Q1/Q2, data {{10,20},{5,15}})
// — column A = categories, B/C = the two series, row 1 = series-name header.
constexpr const char* kXlsxSheet1 = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
  <dimension ref="A1:C3"/>
  <sheetData>
    <row r="1"><c r="B1" t="inlineStr"><is><t>Sales</t></is></c><c r="C1" t="inlineStr"><is><t>Costs</t></is></c></row>
    <row r="2"><c r="A2" t="inlineStr"><is><t>Q1</t></is></c><c r="B2"><v>10</v></c><c r="C2"><v>5</v></c></row>
    <row r="3"><c r="A3" t="inlineStr"><is><t>Q2</t></is></c><c r="B3"><v>20</v></c><c r="C3"><v>15</v></c></row>
  </sheetData>
</worksheet>)";

} // namespace

void write_chart_template_docx(const std::filesystem::path& out,
                               const ChartFixture&          fx)
{
    if (fx.series_names.size() != fx.data.size()) {
        throw std::runtime_error("chart fixture: data rows != series count");
    }
    for (const auto& row : fx.data) {
        if (row.size() != fx.category_names.size()) {
            throw std::runtime_error("chart fixture: data row width != category count");
        }
    }

    std::vector<DocxPart> parts = {
        {"[Content_Types].xml",            kChartContentTypes},
        {"_rels/.rels",                    kPackageRels},
        {"word/_rels/document.xml.rels",   kDocRelsWithChart},
        {"word/document.xml",              build_document_with_chart(fx.bookmark_name)},
        {"word/charts/chart1.xml",         build_chart_xml(fx)},
    };
    write_docx(out, parts);
}

void write_chart_template_docx_with_workbook(const std::filesystem::path& out,
                                             const ChartFixture&          fx)
{
    if (fx.series_names.size() != fx.data.size()) {
        throw std::runtime_error("chart fixture: data rows != series count");
    }
    for (const auto& row : fx.data) {
        if (row.size() != fx.category_names.size()) {
            throw std::runtime_error("chart fixture: data row width != category count");
        }
    }

    // Build the nested .xlsx via the same generic zip packer, round-tripped
    // through a temp file so we get real, valid zip bytes to embed.
    const auto xlsx_tmp = out.string() + ".embedded.xlsx";
    write_docx(xlsx_tmp, {
        {"[Content_Types].xml",         kXlsxContentTypes},
        {"_rels/.rels",                 kXlsxPackageRels},
        {"xl/workbook.xml",             kXlsxWorkbook},
        {"xl/_rels/workbook.xml.rels",  kXlsxWorkbookRels},
        {"xl/worksheets/sheet1.xml",    kXlsxSheet1},
    });
    std::string xlsx_bytes;
    {
        std::ifstream f(xlsx_tmp, std::ios::binary);
        std::ostringstream oss;
        oss << f.rdbuf();
        xlsx_bytes = oss.str();
    }
    std::filesystem::remove(xlsx_tmp);

    std::vector<DocxPart> parts = {
        {"[Content_Types].xml",                        kChartContentTypesWithWorkbook},
        {"_rels/.rels",                                kPackageRels},
        {"word/_rels/document.xml.rels",               kDocRelsWithChart},
        {"word/document.xml",                          build_document_with_chart(fx.bookmark_name)},
        {"word/charts/chart1.xml",                     build_chart_xml(fx, /*with_external_data=*/true)},
        {"word/charts/_rels/chart1.xml.rels",          kChartRelsWithExternalData},
        {"word/embeddings/Microsoft_Excel_Worksheet1.xlsx", xlsx_bytes},
    };
    write_docx(out, parts);
}

namespace {
// 2×2 RGB PNG (red, green / blue, white) — generated by:
//   python3 -c 'zlib.crc32, struct.pack, IHDR+IDAT+IEND'
// Exactly 75 bytes; Word and every conforming PNG decoder opens it.
constexpr std::array<unsigned char, 75> kTinyPngBytes = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
    0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
    0x08, 0x02, 0x00, 0x00, 0x00, 0xfd, 0xd4, 0x9a,
    0x73, 0x00, 0x00, 0x00, 0x12, 0x49, 0x44, 0x41,
    0x54, 0x78, 0xda, 0x63, 0xf8, 0xcf, 0xc0, 0xc0,
    0x00, 0xc2, 0x0c, 0xff, 0x81, 0x00, 0x00, 0x1f,
    0xee, 0x05, 0xfb, 0xf1, 0xab, 0xba, 0x77, 0x00,
    0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae,
    0x42, 0x60, 0x82,
};
} // namespace

std::string_view tiny_png_bytes() {
    return {reinterpret_cast<const char*>(kTinyPngBytes.data()),
            kTinyPngBytes.size()};
}

std::filesystem::path write_tiny_png(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("failed to open " + path.string() +
                                 " for writing PNG fixture");
    }
    const auto view = tiny_png_bytes();
    out.write(view.data(), static_cast<std::streamsize>(view.size()));
    if (!out) {
        throw std::runtime_error("failed to write PNG fixture to " +
                                 path.string());
    }
    return path;
}

namespace {
// 2×2 24-bit RGB BMP (70 bytes). BM header + BITMAPINFOHEADER + 2 BGR rows
// (bottom-up), padded to 4-byte row stride.
constexpr std::array<unsigned char, 70> kTinyBmpBytes = {
    0x42, 0x4d, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x36, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x02, 0x00,
    0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x18, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x13, 0x0b,
    0x00, 0x00, 0x13, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0x00, 0x00,
};
} // namespace

std::string_view tiny_bmp_bytes() {
    return {reinterpret_cast<const char*>(kTinyBmpBytes.data()),
            kTinyBmpBytes.size()};
}

std::filesystem::path write_tiny_bmp(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("failed to open " + path.string() +
                                 " for writing BMP fixture");
    }
    const auto view = tiny_bmp_bytes();
    out.write(view.data(), static_cast<std::streamsize>(view.size()));
    if (!out) {
        throw std::runtime_error("failed to write BMP fixture to " +
                                 path.string());
    }
    return path;
}

} // namespace tf_test
