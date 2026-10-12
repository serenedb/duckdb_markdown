#pragma once

#include "duckdb_compat.hpp"
#include "duckdb/main/error_manager.hpp"
#include "markdown_utils.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace duckdb {
namespace markdown_writer {

inline void WriteNull(Vector &vector, idx_t row) {
	CompatFlatValidityMutable(vector).SetInvalid(row);
}

inline void WriteString(Vector &vector, idx_t row, const std::string &value) {
	CompatFlatDataMutable<string_t>(vector)[row] = StringVector::AddString(vector, value.data(), value.size());
}

inline void WriteCheckedString(Vector &vector, idx_t row, const std::string &value) {
	if (!Value::StringIsValid(value.data(), value.size())) {
		throw ErrorManager::InvalidUnicodeError(value, "value construction");
	}
	WriteString(vector, row, value);
}

inline void WriteOptionalString(Vector &vector, idx_t row, const std::string &value) {
	if (value.empty()) {
		WriteNull(vector, row);
		return;
	}
	WriteString(vector, row, value);
}

template <class T>
inline void WriteNumber(Vector &vector, idx_t row, T value) {
	CompatFlatDataMutable<T>(vector)[row] = value;
}

inline idx_t WriteList(Vector &list, idx_t row, idx_t length) {
	const auto offset = ListVector::GetListSize(list);
	ListVector::Reserve(list, offset + length);
	CompatFlatDataMutable<list_entry_t>(list)[row] = list_entry_t(offset, length);
	ListVector::SetListSize(list, offset + length);
	return offset;
}

inline void WriteStringList(Vector &list, idx_t row, const std::vector<std::string> &values) {
	const auto offset = WriteList(list, row, values.size());
	auto &child = CompatListChildMutable(list);
	for (idx_t i = 0; i < values.size(); i++) {
		WriteString(child, offset + i, values[i]);
	}
}

inline void WriteStringMap(Vector &map, idx_t row, const std::map<std::string, std::string> &fields) {
	const auto offset = WriteList(map, row, fields.size());
	auto &entry = CompatListChildMutable(map);
	auto &keys = CompatStructEntry(entry, 0);
	auto &values = CompatStructEntry(entry, 1);
	idx_t i = offset;
	for (const auto &field : fields) {
		WriteString(keys, i, field.first);
		WriteString(values, i, field.second);
		i++;
	}
}

inline void WriteStats(Vector &result, idx_t row, const markdown_utils::MarkdownStats &stats) {
	WriteNumber(CompatStructEntry(result, 0), row, static_cast<int64_t>(stats.word_count));
	WriteNumber(CompatStructEntry(result, 1), row, static_cast<int64_t>(stats.char_count));
	WriteNumber(CompatStructEntry(result, 2), row, static_cast<int64_t>(stats.line_count));
	WriteNumber(CompatStructEntry(result, 3), row, static_cast<int64_t>(stats.heading_count));
	WriteNumber(CompatStructEntry(result, 4), row, static_cast<int64_t>(stats.code_block_count));
	WriteNumber(CompatStructEntry(result, 5), row, static_cast<int64_t>(stats.link_count));
	WriteNumber(CompatStructEntry(result, 6), row, stats.reading_time_minutes);
}

inline void WriteWikilink(Vector &entry, idx_t row, const markdown_utils::MarkdownWikilink &wikilink) {
	WriteString(CompatStructEntry(entry, 0), row, wikilink.target);
	WriteOptionalString(CompatStructEntry(entry, 1), row, wikilink.alias);
	WriteOptionalString(CompatStructEntry(entry, 2), row, wikilink.anchor);
	WriteNumber(CompatStructEntry(entry, 3), row, wikilink.is_embed);
	WriteNumber(CompatStructEntry(entry, 4), row, static_cast<int64_t>(wikilink.line_number));
}

inline void WriteTag(Vector &entry, idx_t row, const markdown_utils::MarkdownTag &tag) {
	WriteString(CompatStructEntry(entry, 0), row, tag.tag);
	WriteNumber(CompatStructEntry(entry, 1), row, static_cast<int64_t>(tag.line_number));
}

template <class T, class WRITE>
inline void WriteStructList(Vector &list, idx_t row, const std::vector<T> &items, WRITE &&write) {
	const auto offset = WriteList(list, row, items.size());
	auto &entry = CompatListChildMutable(list);
	for (idx_t i = 0; i < items.size(); i++) {
		write(entry, offset + i, items[i]);
	}
}

template <class EXTRACT, class WRITE>
inline void ExecuteListExtraction(DataChunk &args, Vector &result, EXTRACT &&extract, WRITE &&write) {
	const auto count = args.size();
	UnifiedVectorFormat input;
	CompatToUnifiedFormat(args.data[0], count, input);
	const auto inputs = UnifiedVectorFormat::GetData<string_t>(input);
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t row = 0; row < count; row++) {
		const auto idx = input.sel->get_index(row);
		if (!input.validity.RowIsValid(idx)) {
			WriteNull(result, row);
			continue;
		}
		const auto items = extract(inputs[idx].GetString(), row);
		if (!items) {
			WriteNull(result, row);
			continue;
		}
		WriteStructList(result, row, *items, write);
	}
}

} // namespace markdown_writer
} // namespace duckdb
