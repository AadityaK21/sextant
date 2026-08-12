// The CSV reader.
//
// Every test here is a case that a "split on commas" parser gets wrong, and
// gets wrong SILENTLY - it shifts every column after the offending one, so a
// port ends up with half its name in the country field. That failure survives
// all the way to entity resolution, where it looks like a data quality problem
// rather than a parsing one, which is why these are worth being thorough about.

#include "csv.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

using namespace sextant::connectors;

namespace {

std::unique_ptr<CsvReader> Parse(const std::string& text) {
  std::unique_ptr<CsvReader> reader;
  const Status s = CsvReader::OpenFromString(text, &reader);
  EXPECT_TRUE(s.ok()) << s.ToString();
  return reader;
}

std::string Cell(const CsvReader& reader, const char* column) {
  std::string out;
  EXPECT_TRUE(reader.current().Get(column, &out)) << "no column " << column;
  return out;
}

}  // namespace

TEST(Csv, ReadsAHeaderAndRows) {
  auto reader = Parse("a,b,c\n1,2,3\n4,5,6\n");
  ASSERT_NE(nullptr, reader);
  EXPECT_EQ(std::vector<std::string>({"a", "b", "c"}), reader->header());

  ASSERT_TRUE(reader->Next());
  EXPECT_EQ(1u, reader->row_seq());
  EXPECT_EQ("1", Cell(*reader, "a"));
  EXPECT_EQ("3", Cell(*reader, "c"));

  ASSERT_TRUE(reader->Next());
  EXPECT_EQ(2u, reader->row_seq());
  EXPECT_EQ("4", Cell(*reader, "a"));

  EXPECT_FALSE(reader->Next());
  EXPECT_TRUE(reader->status().ok());
}

TEST(Csv, QuotedFieldsHoldCommas) {
  auto reader = Parse(
      "name,alternates,code\n"
      "ROTTERDAM,\"Europoort, Botlek, Maasvlakte\",NLRTM\n");
  ASSERT_TRUE(reader->Next());
  EXPECT_EQ("ROTTERDAM", Cell(*reader, "name"));
  EXPECT_EQ("Europoort, Botlek, Maasvlakte", Cell(*reader, "alternates"));
  EXPECT_EQ("NLRTM", Cell(*reader, "code")) << "the comma shifted a column";
}

TEST(Csv, DoubledQuotesAreOneQuote) {
  auto reader = Parse(
      "name,note\n"
      "\"The \"\"Old\"\" Harbour\",\"he said \"\"hello\"\"\"\n");
  ASSERT_TRUE(reader->Next());
  EXPECT_EQ("The \"Old\" Harbour", Cell(*reader, "name"));
  EXPECT_EQ("he said \"hello\"", Cell(*reader, "note"));
}

TEST(Csv, QuotedFieldsHoldNewlines) {
  auto reader = Parse(
      "name,remarks\n"
      "ROTTERDAM,\"first line\nsecond line\"\n"
      "HAMBURG,plain\n");
  ASSERT_TRUE(reader->Next());
  EXPECT_EQ("first line\nsecond line", Cell(*reader, "remarks"));
  // The embedded newline must not have been read as a record separator.
  ASSERT_TRUE(reader->Next());
  EXPECT_EQ("HAMBURG", Cell(*reader, "name"));
  EXPECT_FALSE(reader->Next());
}

TEST(Csv, HandlesCrlfAndBareCr) {
  auto reader = Parse("a,b\r\n1,2\r\n3,4\r\n");
  ASSERT_TRUE(reader->Next());
  EXPECT_EQ("1", Cell(*reader, "a"));
  EXPECT_EQ("2", Cell(*reader, "b")) << "a stray CR ended up inside the value";
  ASSERT_TRUE(reader->Next());
  EXPECT_EQ("4", Cell(*reader, "b"));
  EXPECT_FALSE(reader->Next());

  auto mac = Parse("a,b\r1,2\r");
  ASSERT_TRUE(mac->Next());
  EXPECT_EQ("2", Cell(*mac, "b"));
}

// Excel writes a byte-order mark on every CSV it exports. Left in place it
// becomes part of the first column's NAME, so every later lookup of that column
// quietly returns nothing.
TEST(Csv, StripsTheExcelByteOrderMark) {
  auto reader = Parse("\xEF\xBB\xBF"
                      "World Port Index Number,name\n14370,ROTTERDAM\n");
  EXPECT_EQ("World Port Index Number", reader->header().front());
  ASSERT_TRUE(reader->Next());
  EXPECT_EQ("14370", Cell(*reader, "World Port Index Number"));
}

TEST(Csv, LastRecordNeedsNoTrailingNewline) {
  auto reader = Parse("a,b\n1,2");
  ASSERT_TRUE(reader->Next());
  EXPECT_EQ("2", Cell(*reader, "b"));
  EXPECT_FALSE(reader->Next());
}

TEST(Csv, SkipsBlankLines) {
  auto reader = Parse("a,b\n\n1,2\n\n\n3,4\n\n");
  ASSERT_TRUE(reader->Next());
  EXPECT_EQ("1", Cell(*reader, "a"));
  ASSERT_TRUE(reader->Next());
  EXPECT_EQ("3", Cell(*reader, "a"));
  EXPECT_FALSE(reader->Next());
  EXPECT_EQ(2u, reader->row_seq());
}

// Real exports have rows with fewer cells than the header declares. The column
// exists in the file; this row just does not fill it, which is different from
// the column not existing at all.
TEST(Csv, RaggedRowsReportEmptyRatherThanMissing) {
  auto reader = Parse("a,b,c\n1,2\n");
  ASSERT_TRUE(reader->Next());
  std::string out;
  ASSERT_TRUE(reader->current().Get("c", &out));
  EXPECT_EQ("", out);
  EXPECT_FALSE(reader->current().Get("nosuchcolumn", &out));
}

TEST(Csv, EmptyFieldsAreEmptyStrings) {
  auto reader = Parse("a,b,c\n,,\n1,,3\n");
  ASSERT_TRUE(reader->Next());
  EXPECT_EQ("", Cell(*reader, "a"));
  EXPECT_EQ("", Cell(*reader, "b"));
  ASSERT_TRUE(reader->Next());
  EXPECT_EQ("1", Cell(*reader, "a"));
  EXPECT_EQ("", Cell(*reader, "b"));
  EXPECT_EQ("3", Cell(*reader, "c"));
}

// Raw() feeds the RAW keyspace and is what a lineage answer eventually shows.
// Re-serialising the parsed fields would produce something that looks right and
// is not what the file says.
TEST(Csv, RawRecordIsVerbatim) {
  auto reader = Parse(
      "name,alternates\n"
      "ROTTERDAM,\"Europoort, Botlek\"\n");
  ASSERT_TRUE(reader->Next());
  EXPECT_EQ("ROTTERDAM,\"Europoort, Botlek\"", reader->current().Raw())
      << "quoting must survive into the archive";
}

TEST(Csv, RejectsAnEmptyFile) {
  std::unique_ptr<CsvReader> reader;
  EXPECT_FALSE(CsvReader::OpenFromString("", &reader).ok());
  EXPECT_FALSE(CsvReader::Open("/nonexistent/path/to/nothing.csv", &reader).ok());
}

// A record wider than the read buffer has to be assembled across chunk
// boundaries. Getting this wrong truncates a value at exactly 64 KB, which is
// the kind of thing a small test file never finds.
TEST(Csv, RecordsSpanReadBuffers) {
  const std::string wide(200 * 1024, 'x');
  auto reader = Parse("a,b\n" + wide + ",tail\n");
  ASSERT_TRUE(reader->Next());
  EXPECT_EQ(wide, Cell(*reader, "a"));
  EXPECT_EQ("tail", Cell(*reader, "b"));
}

TEST(Csv, ReadsTheShippedWorldPortIndexSample) {
  std::unique_ptr<CsvReader> reader;
  const Status s = CsvReader::Open(
      std::string(SEXTANT_SOURCE_DIR) + "/data/snapshots/wpi/UpdatedPub150.csv",
      &reader);
  ASSERT_TRUE(s.ok()) << s.ToString();

  int rows = 0;
  bool saw_rotterdam = false;
  while (reader->Next()) {
    ++rows;
    std::string name;
    ASSERT_TRUE(reader->current().Get("Main Port Name", &name));
    if (name != "ROTTERDAM") continue;
    saw_rotterdam = true;
    std::string alternates;
    ASSERT_TRUE(reader->current().Get("Alternate Port Name", &alternates));
    // Quoted, semicolon separated, with commas nowhere in sight - exactly the
    // shape split_semicolon expects.
    EXPECT_EQ("Rotterdam Botlek; Europoort; Maasvlakte", alternates);
    std::string locode;
    ASSERT_TRUE(reader->current().Get("UN/LOCODE", &locode));
    EXPECT_EQ("NLRTM", locode);
  }
  EXPECT_TRUE(saw_rotterdam);
  EXPECT_GT(rows, 5);
  EXPECT_TRUE(reader->status().ok());
}

TEST(Csv, ReadsTheShippedUnlocodeSample) {
  std::unique_ptr<CsvReader> reader;
  const Status s = CsvReader::Open(
      std::string(SEXTANT_SOURCE_DIR) + "/data/snapshots/unlocode/code-list.csv",
      &reader);
  ASSERT_TRUE(s.ok()) << s.ToString();

  bool saw_goteborg = false;
  while (reader->Next()) {
    std::string name;
    ASSERT_TRUE(reader->current().Get("Name", &name));
    if (name != "G\xC3\xB6teborg") continue;
    saw_goteborg = true;
    std::string plain, coords;
    ASSERT_TRUE(reader->current().Get("NameWoDiacritics", &plain));
    ASSERT_TRUE(reader->current().Get("Coordinates", &coords));
    EXPECT_EQ("Goteborg", plain);
    EXPECT_EQ("5742N 01156E", coords);
  }
  EXPECT_TRUE(saw_goteborg) << "UTF-8 did not survive the reader";
}
