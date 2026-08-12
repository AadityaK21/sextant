#include "csv.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sextant::connectors {
namespace {

constexpr size_t kChunkSize = 64 * 1024;

}  // namespace

// --- CsvRow -----------------------------------------------------------------

bool CsvRow::Get(const std::string& path, std::string* out) const {
  const int index = reader_->ColumnIndex(path);
  if (index < 0) return false;
  const auto& fields = reader_->fields();
  // A ragged row - fewer cells than the header declares - is common in real
  // exports and is reported as an empty value rather than a missing column. The
  // column exists in the file; this row just does not fill it.
  if (static_cast<size_t>(index) >= fields.size()) {
    out->clear();
    return true;
  }
  *out = fields[static_cast<size_t>(index)];
  return true;
}

std::string CsvRow::Raw() const { return reader_->raw_record(); }

// --- CsvReader --------------------------------------------------------------

Status CsvReader::Open(const std::string& path, std::unique_ptr<CsvReader>* out) {
  std::unique_ptr<lsm::SequentialFile> file;
  Status s = lsm::SequentialFile::Open(path, &file);
  if (!s.ok()) return s;

  std::unique_ptr<CsvReader> reader(new CsvReader());
  reader->file_ = std::move(file);
  reader->buffer_.resize(kChunkSize);

  if (!reader->ReadRecord()) {
    return Status::Corruption("CSV file has no header row: " + path);
  }
  reader->header_ = reader->fields_;
  for (size_t i = 0; i < reader->header_.size(); ++i) {
    reader->column_index_.emplace(reader->header_[i], static_cast<int>(i));
  }
  *out = std::move(reader);
  return Status::OK();
}

Status CsvReader::OpenFromString(std::string contents,
                                 std::unique_ptr<CsvReader>* out) {
  std::unique_ptr<CsvReader> reader(new CsvReader());
  reader->memory_ = std::move(contents);
  reader->from_memory_ = true;

  if (!reader->ReadRecord()) {
    return Status::Corruption("CSV input has no header row");
  }
  reader->header_ = reader->fields_;
  for (size_t i = 0; i < reader->header_.size(); ++i) {
    reader->column_index_.emplace(reader->header_[i], static_cast<int>(i));
  }
  *out = std::move(reader);
  return Status::OK();
}

int CsvReader::ColumnIndex(const std::string& name) const {
  const auto it = column_index_.find(name);
  return it == column_index_.end() ? -1 : it->second;
}

bool CsvReader::Fill() {
  if (from_memory_ || eof_) return false;
  lsm::Slice result;
  const Status s = file_->Read(buffer_.size(), &result, buffer_.data());
  if (!s.ok()) {
    status_ = s;
    eof_ = true;
    return false;
  }
  pos_ = 0;
  end_ = result.size();
  if (end_ == 0) {
    eof_ = true;
    return false;
  }
  return true;
}

bool CsvReader::Next() {
  if (!ReadRecord()) return false;
  ++row_seq_;
  return true;
}

bool CsvReader::ReadRecord() {
  fields_.clear();
  raw_record_.clear();

  const char* data = from_memory_ ? memory_.data() : buffer_.data();
  size_t limit = from_memory_ ? memory_.size() : end_;
  if (from_memory_ && end_ == 0) {
    end_ = memory_.size();
    limit = end_;
  }

  auto peek = [&](char* c) -> bool {
    if (pos_ >= limit) {
      if (!Fill()) return false;
      data = from_memory_ ? memory_.data() : buffer_.data();
      limit = from_memory_ ? memory_.size() : end_;
      if (pos_ >= limit) return false;
    }
    *c = data[pos_];
    return true;
  };
  auto advance = [&]() { ++pos_; };

  // A byte-order mark on the first record. Excel writes one on every CSV it
  // exports, and left in place it becomes part of the first column's name, so
  // every later lookup of that column silently fails.
  if (row_seq_ == 0 && header_.empty()) {
    char c0;
    const size_t saved = pos_;
    if (peek(&c0) && static_cast<uint8_t>(c0) == 0xEF) {
      advance();
      char c1, c2;
      if (peek(&c1) && static_cast<uint8_t>(c1) == 0xBB) {
        advance();
        if (peek(&c2) && static_cast<uint8_t>(c2) == 0xBF) {
          advance();
        } else {
          pos_ = saved;
        }
      } else {
        pos_ = saved;
      }
    }
  }

  std::string field;
  bool in_quotes = false;
  bool any_input = false;
  bool record_done = false;

  while (!record_done) {
    char c;
    if (!peek(&c)) {
      // End of input. A trailing record with no newline is still a record.
      if (!any_input && field.empty() && fields_.empty()) return false;
      fields_.push_back(field);
      break;
    }
    any_input = true;
    advance();
    raw_record_.push_back(c);

    if (in_quotes) {
      if (c != '"') {
        field.push_back(c);
        continue;
      }
      // A quote inside a quoted field is either an escaped quote, written as
      // two in a row, or the end of the field. One byte of lookahead decides.
      char nxt;
      if (peek(&nxt) && nxt == '"') {
        advance();
        raw_record_.push_back(nxt);
        field.push_back('"');
      } else {
        in_quotes = false;
      }
      continue;
    }

    switch (c) {
      case '"':
        in_quotes = true;
        break;
      case ',':
        fields_.push_back(field);
        field.clear();
        break;
      case '\r': {
        // CRLF, or a lone CR from a classic Mac export. Either way the record
        // ends here and the LF, if present, is consumed with it.
        char nxt;
        if (peek(&nxt) && nxt == '\n') {
          advance();
          raw_record_.push_back(nxt);
        }
        fields_.push_back(field);
        record_done = true;
        break;
      }
      case '\n':
        fields_.push_back(field);
        record_done = true;
        break;
      default:
        field.push_back(c);
        break;
    }
  }

  // Strip the record separator from the verbatim bytes. It belongs to the file
  // structure rather than to the record, and keeping it would put a stray
  // newline in the middle of every lineage view.
  while (!raw_record_.empty() &&
         (raw_record_.back() == '\n' || raw_record_.back() == '\r')) {
    raw_record_.pop_back();
  }

  // A single empty field is a blank line, not a record. Real exports have them
  // at the end of the file and sometimes between sections.
  if (fields_.size() == 1 && fields_[0].empty() && raw_record_.empty()) {
    return ReadRecord();
  }
  return true;
}

}  // namespace sextant::connectors
