#include "markdown_copy.hpp"
#include "duckdb_compat.hpp"
#include "duck_block_vocabulary.hpp"
#include "duck_block_functions.hpp"
#include "markdown_types.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/read_within_page.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/cast_helpers.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/copy_info.hpp"

namespace duckdb {

using Vocab = DuckBlockVocabulary;

//===--------------------------------------------------------------------===//
// WriteMarkdownBindData
//===--------------------------------------------------------------------===//

unique_ptr<FunctionData> WriteMarkdownBindData::Copy() const {
	auto result = make_uniq<WriteMarkdownBindData>();
	result->markdown_mode = markdown_mode;
	result->null_value = null_value;
	result->header = header;
	result->escape_pipes = escape_pipes;
	result->escape_newlines = escape_newlines;
	result->frontmatter = frontmatter;
	result->content_column = content_column;
	result->title_column = title_column;
	result->level_column = level_column;
	result->blank_lines = blank_lines;
	result->kind_column = kind_column;
	result->element_type_column = element_type_column;
	result->encoding_column = encoding_column;
	result->attributes_column = attributes_column;
	result->level_col_idx = level_col_idx;
	result->title_col_idx = title_col_idx;
	result->content_col_idx = content_col_idx;
	result->kind_col_idx = kind_col_idx;
	result->element_type_col_idx = element_type_col_idx;
	result->encoding_col_idx = encoding_col_idx;
	result->attributes_col_idx = attributes_col_idx;
	result->alignments = alignments;
	result->column_names = column_names;
	result->column_types = column_types;
	return std::move(result);
}

bool WriteMarkdownBindData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<WriteMarkdownBindData>();
	return markdown_mode == other.markdown_mode && null_value == other.null_value && header == other.header &&
	       escape_pipes == other.escape_pipes && escape_newlines == other.escape_newlines &&
	       frontmatter == other.frontmatter && content_column == other.content_column &&
	       title_column == other.title_column && level_column == other.level_column && blank_lines == other.blank_lines;
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

void MarkdownCopyFunction::Register(ExtensionLoader &loader) {
	CopyFunction func("markdown");
	func.extension = "md";

	func.copy_to_bind = Bind;
	func.copy_to_initialize_global = InitializeGlobal;
	func.copy_to_initialize_local = InitializeLocal;
	func.copy_to_sink = Sink;
	func.copy_to_combine = Combine;
	func.copy_to_finalize = Finalize;
	func.copy_options = CopyOptions;
	func.parallel_sink = true;

	loader.RegisterFunction(func);
}

//===--------------------------------------------------------------------===//
// Copy Options
//===--------------------------------------------------------------------===//

void MarkdownCopyFunction::CopyOptions(ClientContext &context, CopyOptionsInput &input) {
	// Core options
	input.options["markdown_mode"] = CopyOption(LogicalType::VARCHAR);
	input.options["null_value"] = CopyOption(LogicalType::VARCHAR);

	// Table mode options
	input.options["header"] = CopyOption(LogicalType::BOOLEAN);
	input.options["escape_pipes"] = CopyOption(LogicalType::BOOLEAN);
	input.options["escape_newlines"] = CopyOption(LogicalType::BOOLEAN);

	// Document mode options
	input.options["frontmatter"] = CopyOption(LogicalType::VARCHAR);
	input.options["content_column"] = CopyOption(LogicalType::VARCHAR);
	input.options["title_column"] = CopyOption(LogicalType::VARCHAR);
	input.options["level_column"] = CopyOption(LogicalType::VARCHAR);
	input.options["blank_lines"] = CopyOption(LogicalType::INTEGER);

	// Blocks mode options (uses duck_block naming)
	input.options["kind_column"] = CopyOption(LogicalType::VARCHAR);
	input.options["element_type_column"] = CopyOption(LogicalType::VARCHAR);
	input.options["encoding_column"] = CopyOption(LogicalType::VARCHAR);
	input.options["attributes_column"] = CopyOption(LogicalType::VARCHAR);
}

//===--------------------------------------------------------------------===//
// Bind
//===--------------------------------------------------------------------===//

unique_ptr<FunctionData> MarkdownCopyFunction::Bind(ClientContext &context, CopyFunctionBindInput &input,
                                                    const vector<CompatName> &names,
                                                    const vector<LogicalType> &sql_types) {
	auto result = make_uniq<WriteMarkdownBindData>();
	auto &options = input.info.options;

	// Store schema info. Converted element-wise rather than assigned: on DuckDB
	// v2.0 `names` is a vector<Identifier>, and Identifier does not implicitly
	// convert to string -- deliberately, because it carries case-insensitive
	// comparison semantics that a silent conversion would discard.
	result->column_names.reserve(names.size());
	for (auto &name : names) {
		result->column_names.push_back(CompatNameStr(name));
	}
	result->column_types = sql_types;

	// Parse options
	for (auto &option : options) {
		auto loption = StringUtil::Lower(CompatNameStr(option.first));
		auto &value = option.second;

		if (loption == "markdown_mode") {
			auto mode_str = StringUtil::Lower(StringValue::Get(value[0]));
			if (mode_str == "table") {
				result->markdown_mode = WriteMarkdownBindData::MarkdownMode::TABLE;
			} else if (mode_str == "document") {
				result->markdown_mode = WriteMarkdownBindData::MarkdownMode::DOCUMENT;
			} else if (mode_str == "blocks" || mode_str == "duck_block") {
				result->markdown_mode = WriteMarkdownBindData::MarkdownMode::BLOCKS;
			} else {
				throw InvalidInputException(
				    "Invalid markdown_mode: '%s'. Expected 'table', 'document', 'blocks', or 'duck_block'", mode_str);
			}
		} else if (loption == "null_value") {
			result->null_value = StringValue::Get(value[0]);
		} else if (loption == "header") {
			result->header = BooleanValue::Get(value[0]);
		} else if (loption == "escape_pipes") {
			result->escape_pipes = BooleanValue::Get(value[0]);
		} else if (loption == "escape_newlines") {
			result->escape_newlines = BooleanValue::Get(value[0]);
		} else if (loption == "frontmatter") {
			result->frontmatter = StringValue::Get(value[0]);
		} else if (loption == "content_column") {
			result->content_column = StringValue::Get(value[0]);
		} else if (loption == "title_column") {
			result->title_column = StringValue::Get(value[0]);
		} else if (loption == "level_column") {
			result->level_column = StringValue::Get(value[0]);
		} else if (loption == "blank_lines") {
			result->blank_lines = IntegerValue::Get(value[0]);
		} else if (loption == "kind_column") {
			result->kind_column = StringValue::Get(value[0]);
		} else if (loption == "element_type_column") {
			result->element_type_column = StringValue::Get(value[0]);
		} else if (loption == "encoding_column") {
			result->encoding_column = StringValue::Get(value[0]);
		} else if (loption == "attributes_column") {
			result->attributes_column = StringValue::Get(value[0]);
		}
	}

	// For table mode, detect alignments
	if (result->markdown_mode == WriteMarkdownBindData::MarkdownMode::TABLE) {
		for (const auto &type : sql_types) {
			result->alignments.push_back(DetectAlignment(type));
		}
	}

	// For document mode, resolve column indices
	if (result->markdown_mode == WriteMarkdownBindData::MarkdownMode::DOCUMENT) {
		for (idx_t i = 0; i < names.size(); i++) {
			auto lower_name = StringUtil::Lower(CompatNameStr(names[i]));
			if (lower_name == StringUtil::Lower(result->level_column)) {
				result->level_col_idx = i;
			} else if (lower_name == StringUtil::Lower(result->title_column)) {
				result->title_col_idx = i;
			} else if (lower_name == StringUtil::Lower(result->content_column)) {
				result->content_col_idx = i;
			}
		}

		// Validate required columns for document mode
		if (result->level_col_idx == DConstants::INVALID_INDEX) {
			throw InvalidInputException("Document mode requires a '%s' column", result->level_column);
		}
		if (result->title_col_idx == DConstants::INVALID_INDEX) {
			throw InvalidInputException("Document mode requires a '%s' column", result->title_column);
		}
		// content_column is optional - sections can have empty content
	}

	// For blocks mode, resolve column indices (uses duck_block naming)
	if (result->markdown_mode == WriteMarkdownBindData::MarkdownMode::BLOCKS) {
		for (idx_t i = 0; i < names.size(); i++) {
			auto lower_name = StringUtil::Lower(CompatNameStr(names[i]));
			if (lower_name == StringUtil::Lower(result->kind_column)) {
				result->kind_col_idx = i;
			} else if (lower_name == StringUtil::Lower(result->element_type_column)) {
				result->element_type_col_idx = i;
			} else if (lower_name == StringUtil::Lower(result->content_column)) {
				result->content_col_idx = i;
			} else if (lower_name == StringUtil::Lower(result->level_column)) {
				result->level_col_idx = i;
			} else if (lower_name == StringUtil::Lower(result->encoding_column)) {
				result->encoding_col_idx = i;
			} else if (lower_name == StringUtil::Lower(result->attributes_column)) {
				result->attributes_col_idx = i;
			}
		}

		// Validate required columns for blocks mode
		if (result->element_type_col_idx == DConstants::INVALID_INDEX) {
			throw InvalidInputException("Blocks mode requires a '%s' column", result->element_type_column);
		}
		if (result->content_col_idx == DConstants::INVALID_INDEX) {
			throw InvalidInputException("Blocks mode requires a '%s' column", result->content_column);
		}
		// level, encoding, attributes are optional
	}

	return std::move(result);
}

//===--------------------------------------------------------------------===//
// Initialize Global
//===--------------------------------------------------------------------===//

unique_ptr<GlobalFunctionData> MarkdownCopyFunction::InitializeGlobal(ClientContext &context, FunctionData &bind_data_p,
                                                                      const string &file_path) {
	auto &bind_data = bind_data_p.Cast<WriteMarkdownBindData>();
	auto result = make_uniq<WriteMarkdownGlobalState>(context, file_path);
	string head;
	if (bind_data.markdown_mode == WriteMarkdownBindData::MarkdownMode::TABLE) {
		if (bind_data.header) {
			head = RenderTableHeader(bind_data) + RenderTableSeparator(bind_data);
		}
	} else if (!bind_data.frontmatter.empty()) {
		head = RenderFrontmatter(bind_data);
	}
	result->file.WriteFrame(const_data_ptr_cast(head.data()), head.size());
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// Initialize Local
//===--------------------------------------------------------------------===//

static constexpr idx_t WRITE_BUFFER_BYTES = idx_t(4) << 20;
static constexpr idx_t CELL_SLACK = 64;
static constexpr idx_t MAX_ESCAPED_BYTE = 4;

static inline char *CopyCell(char *out, std::string_view value) {
	CopyWithSlack<CELL_SLACK>(out, value.data(), value.size());
	return out + value.size();
}

static char *WriteEscapedCell(char *out, std::string_view value, const WriteMarkdownBindData &bind_data) {
	for (const char c : value) {
		if (c == '|' && bind_data.escape_pipes) {
			*out++ = '\\';
			*out++ = '|';
		} else if (c == '\n' && bind_data.escape_newlines) {
			memcpy(out, "<br>", MAX_ESCAPED_BYTE);
			out += MAX_ESCAPED_BYTE;
		} else if (c != '\r' || !bind_data.escape_newlines) {
			*out++ = c;
		}
	}
	return out;
}

static MarkdownCellColumn Direct(MarkdownCellFormat format, idx_t max_length) {
	MarkdownCellColumn column;
	column.format = format;
	column.check = false;
	column.max_length = max_length;
	return column;
}

static MarkdownCellColumn CellColumn(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return Direct(MarkdownCellFormat::BOOLEAN, 5);
	case LogicalTypeId::TINYINT:
		return Direct(MarkdownCellFormat::INT8, 4);
	case LogicalTypeId::SMALLINT:
		return Direct(MarkdownCellFormat::INT16, 6);
	case LogicalTypeId::INTEGER:
		return Direct(MarkdownCellFormat::INT32, 11);
	case LogicalTypeId::BIGINT:
		return Direct(MarkdownCellFormat::INT64, 20);
	case LogicalTypeId::UTINYINT:
		return Direct(MarkdownCellFormat::UINT8, 3);
	case LogicalTypeId::USMALLINT:
		return Direct(MarkdownCellFormat::UINT16, 5);
	case LogicalTypeId::UINTEGER:
		return Direct(MarkdownCellFormat::UINT32, 10);
	case LogicalTypeId::UBIGINT:
		return Direct(MarkdownCellFormat::UINT64, 20);
	case LogicalTypeId::DECIMAL: {
		auto format = MarkdownCellFormat::DECIMAL128;
		switch (type.InternalType()) {
		case PhysicalType::INT16:
			format = MarkdownCellFormat::DECIMAL16;
			break;
		case PhysicalType::INT32:
			format = MarkdownCellFormat::DECIMAL32;
			break;
		case PhysicalType::INT64:
			format = MarkdownCellFormat::DECIMAL64;
			break;
		default:
			break;
		}
		auto column = Direct(format, DecimalType::GetWidth(type) + idx_t(3));
		column.width = DecimalType::GetWidth(type);
		column.scale = DecimalType::GetScale(type);
		return column;
	}
	case LogicalTypeId::DATE:
		return Direct(MarkdownCellFormat::DATE, DateToStringCast::MAX_LENGTH);
	case LogicalTypeId::TIME:
		return Direct(MarkdownCellFormat::TIME, TimeToStringCast::MAX_LENGTH);
	case LogicalTypeId::TIME_NS:
		return Direct(MarkdownCellFormat::TIME_NS, TimeToStringCast::MAX_LENGTH);
	case LogicalTypeId::TIMESTAMP:
		return Direct(MarkdownCellFormat::TIMESTAMP, TimestampToStringCast::MAX_LENGTH);
	case LogicalTypeId::TIMESTAMP_NS:
		return Direct(MarkdownCellFormat::TIMESTAMP_NS, TimestampToStringCast::MAX_LENGTH);
	case LogicalTypeId::INTERVAL:
		return Direct(MarkdownCellFormat::INTERVAL, IntervalToStringCast::MAX_LENGTH);
	case LogicalTypeId::UUID:
		return Direct(MarkdownCellFormat::UUID, UUID::STRING_SIZE);
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::TIME_TZ:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS: {
		MarkdownCellColumn column;
		column.check = false;
		return column;
	}
	default:
		return MarkdownCellColumn();
	}
}

unique_ptr<LocalFunctionData> MarkdownCopyFunction::InitializeLocal(ExecutionContext &context,
                                                                    FunctionData &bind_data_p) {
	auto &bind_data = bind_data_p.Cast<WriteMarkdownBindData>();
	auto result = make_uniq<WriteMarkdownLocalState>();
	if (bind_data.escape_pipes) {
		result->cell_specials.Add('|');
	}
	if (bind_data.escape_newlines) {
		result->cell_specials.Add('\n');
		result->cell_specials.Add('\r');
	}
	for (const auto &type : bind_data.column_types) {
		result->cell_columns.push_back(CellColumn(type));
	}
	result->block_type = MarkdownTypes::DuckBlockType();
	const auto &null_value = bind_data.null_value;
	result->escaped_null.resize_and_overwrite(null_value.size() * MAX_ESCAPED_BYTE, [&](char *data, size_t) {
		return static_cast<size_t>(WriteEscapedCell(data, null_value, bind_data) - data);
	});
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//

void MarkdownCopyFunction::Sink(ExecutionContext &context, FunctionData &bind_data_p, GlobalFunctionData &gstate_p,
                                LocalFunctionData &lstate_p, DataChunk &input) {
	auto &bind_data = bind_data_p.Cast<WriteMarkdownBindData>();
	auto &gstate = gstate_p.Cast<WriteMarkdownGlobalState>();
	auto &lstate = lstate_p.Cast<WriteMarkdownLocalState>();

	if (bind_data.markdown_mode == WriteMarkdownBindData::MarkdownMode::TABLE) {
		// Table mode: render rows
		const auto count = input.size();
		vector<Vector> texts;
		texts.reserve(input.ColumnCount());
		vector<UnifiedVectorFormat> columns(input.ColumnCount());
		for (idx_t col_idx = 0; col_idx < input.ColumnCount(); col_idx++) {
			if (lstate.cell_columns[col_idx].format != MarkdownCellFormat::TEXT) {
				CompatToUnifiedFormat(input.data[col_idx], count, columns[col_idx]);
				continue;
			}
			texts.emplace_back(LogicalType::VARCHAR);
			VectorOperations::DefaultCast(input.data[col_idx], texts.back(), count);
			CompatToUnifiedFormat(texts.back(), count, columns[col_idx]);
		}
		AppendTableRows(lstate, columns, count, bind_data);
	} else if (bind_data.markdown_mode == WriteMarkdownBindData::MarkdownMode::DOCUMENT) {
		// Document mode: render sections
		const auto count = input.size();
		Vector levels(LogicalType::INTEGER);
		Vector titles(LogicalType::VARCHAR);
		Vector contents(LogicalType::VARCHAR);
		UnifiedVectorFormat level_data;
		UnifiedVectorFormat title_data;
		UnifiedVectorFormat content_data;
		const bool has_level = bind_data.level_col_idx != DConstants::INVALID_INDEX;
		const bool has_title = bind_data.title_col_idx != DConstants::INVALID_INDEX;
		const bool has_content = bind_data.content_col_idx != DConstants::INVALID_INDEX;
		if (has_level) {
			VectorOperations::DefaultCast(input.data[bind_data.level_col_idx], levels, count);
			CompatToUnifiedFormat(levels, count, level_data);
		}
		if (has_title) {
			VectorOperations::DefaultCast(input.data[bind_data.title_col_idx], titles, count);
			CompatToUnifiedFormat(titles, count, title_data);
		}
		if (has_content) {
			VectorOperations::DefaultCast(input.data[bind_data.content_col_idx], contents, count);
			CompatToUnifiedFormat(contents, count, content_data);
		}
		for (idx_t row_idx = 0; row_idx < count; row_idx++) {
			int32_t level = 1;
			if (has_level) {
				const auto idx = level_data.sel->get_index(row_idx);
				if (level_data.validity.RowIsValid(idx)) {
					level = UnifiedVectorFormat::GetData<int32_t>(level_data)[idx];
				}
			}
			string title;
			if (has_title) {
				const auto idx = title_data.sel->get_index(row_idx);
				if (title_data.validity.RowIsValid(idx)) {
					title = UnifiedVectorFormat::GetData<string_t>(title_data)[idx].GetString();
				}
			}
			string content;
			if (has_content) {
				const auto idx = content_data.sel->get_index(row_idx);
				if (content_data.validity.RowIsValid(idx)) {
					content = UnifiedVectorFormat::GetData<string_t>(content_data)[idx].GetString();
				}
			}

			lstate.buffer += RenderSection(level, title, content, bind_data);
		}
	} else {
		const auto count = input.size();
		const auto &block_type = lstate.block_type;
		const auto &attributes_type = StructType::GetChildType(block_type, Vocab::ATTRIBUTES_IDX);
		Vector kinds(LogicalType::VARCHAR);
		Vector element_types(LogicalType::VARCHAR);
		Vector contents(LogicalType::VARCHAR);
		Vector levels(LogicalType::INTEGER);
		Vector encodings(LogicalType::VARCHAR);
		Vector attributes(attributes_type);
		UnifiedVectorFormat kind_data;
		UnifiedVectorFormat element_type_data;
		UnifiedVectorFormat content_data;
		UnifiedVectorFormat level_data;
		UnifiedVectorFormat encoding_data;
		const auto load = [&](idx_t col_idx, Vector &target, UnifiedVectorFormat &data) {
			if (col_idx == DConstants::INVALID_INDEX) {
				return false;
			}
			VectorOperations::DefaultCast(input.data[col_idx], target, count);
			CompatToUnifiedFormat(target, count, data);
			return true;
		};
		const bool has_kind = load(bind_data.kind_col_idx, kinds, kind_data);
		load(bind_data.element_type_col_idx, element_types, element_type_data);
		load(bind_data.content_col_idx, contents, content_data);
		const bool has_level = load(bind_data.level_col_idx, levels, level_data);
		const bool has_encoding = load(bind_data.encoding_col_idx, encodings, encoding_data);
		UnifiedVectorFormat attribute_data;
		const bool has_attributes = load(bind_data.attributes_col_idx, attributes, attribute_data);
		const auto text = [](bool present, const UnifiedVectorFormat &data, idx_t row_idx, const char *absent) {
			if (present) {
				const auto idx = data.sel->get_index(row_idx);
				if (data.validity.RowIsValid(idx)) {
					return Value(UnifiedVectorFormat::GetData<string_t>(data)[idx].GetString());
				}
			}
			return Value(absent);
		};
		for (idx_t row_idx = 0; row_idx < count; row_idx++) {
			vector<Value> fields;
			fields.reserve(Vocab::ELEMENT_ORDER_IDX + 1);
			fields.push_back(text(has_kind, kind_data, row_idx, "block"));
			fields.push_back(text(true, element_type_data, row_idx, ""));
			fields.push_back(text(true, content_data, row_idx, ""));
			Value level(LogicalType::INTEGER);
			if (has_level) {
				const auto level_idx = level_data.sel->get_index(row_idx);
				if (level_data.validity.RowIsValid(level_idx)) {
					level = Value::INTEGER(UnifiedVectorFormat::GetData<int32_t>(level_data)[level_idx]);
				}
			}
			fields.push_back(std::move(level));
			fields.push_back(text(has_encoding, encoding_data, row_idx, Vocab::ENCODING_TEXT));
			fields.push_back(has_attributes ? attributes.GetValue(row_idx) : Value(attributes_type));
			fields.push_back(Value(LogicalType::INTEGER));
			lstate.elements.push_back(Value::STRUCT(block_type, std::move(fields)));
		}
	}
	if (lstate.buffer.size() >= WRITE_BUFFER_BYTES) {
		WriteBuffer(gstate, lstate);
	}
}

//===--------------------------------------------------------------------===//
// Combine
//===--------------------------------------------------------------------===//

void MarkdownCopyFunction::Combine(ExecutionContext &context, FunctionData &bind_data_p, GlobalFunctionData &gstate_p,
                                   LocalFunctionData &lstate_p) {
	auto &gstate = gstate_p.Cast<WriteMarkdownGlobalState>();
	auto &lstate = lstate_p.Cast<WriteMarkdownLocalState>();

	if (!lstate.elements.empty()) {
		// ONE renderer for duck_blocks, shared with duck_blocks_to_md. This path
		// used to have its own row-at-a-time walk, which could not absorb inline
		// runs and rendered `# H` + empty paragraph + inlines with two spurious
		// blank lines between heading and body.
		JSONTreeParser parser;
		lstate.buffer += DuckBlockFunctions::RenderDuckBlocksToMarkdown(
		    parser, Value::LIST(lstate.block_type, std::move(lstate.elements)));
		lstate.elements.clear();
	}

	WriteBuffer(gstate, lstate);
}

void MarkdownCopyFunction::WriteBuffer(WriteMarkdownGlobalState &gstate, WriteMarkdownLocalState &lstate) {
	if (lstate.buffer.empty()) {
		return;
	}
	gstate.file.WriteRows(const_data_ptr_cast(lstate.buffer.data()), lstate.buffer.size());
	lstate.buffer.clear();
}

//===--------------------------------------------------------------------===//
// Finalize
//===--------------------------------------------------------------------===//

void MarkdownCopyFunction::Finalize(ClientContext &context, FunctionData &bind_data_p, GlobalFunctionData &gstate_p) {
	gstate_p.Cast<WriteMarkdownGlobalState>().file.Close();
}

//===--------------------------------------------------------------------===//
// Table Mode Helpers
//===--------------------------------------------------------------------===//

string MarkdownCopyFunction::DetectAlignment(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
		return "right";
	case LogicalTypeId::BOOLEAN:
		return "center";
	default:
		return "left";
	}
}

string MarkdownCopyFunction::RenderTableHeader(const WriteMarkdownBindData &bind_data) {
	string result = "|";
	for (const auto &name : bind_data.column_names) {
		result += " " + name + " |";
	}
	result += "\n";
	return result;
}

string MarkdownCopyFunction::RenderTableSeparator(const WriteMarkdownBindData &bind_data) {
	string result = "|";
	for (const auto &alignment : bind_data.alignments) {
		if (alignment == "right") {
			result += "---:|";
		} else if (alignment == "center") {
			result += ":---:|";
		} else {
			result += "---|";
		}
	}
	result += "\n";
	return result;
}

template <class T>
static char *WriteInteger(char *out, T value) {
	using UNSIGNED = typename MakeUnsigned<T>::type;
	auto magnitude = static_cast<UNSIGNED>(value);
	idx_t sign = 0;
	if constexpr (std::is_signed<T>::value) {
		if (value < 0) {
			magnitude = static_cast<UNSIGNED>(UNSIGNED(0) - magnitude);
			*out = '-';
			sign = 1;
		}
	}
	const auto end = out + sign + NumericHelper::UnsignedLength<UNSIGNED>(magnitude);
	NumericHelper::FormatUnsigned<UNSIGNED>(magnitude, end);
	return end;
}

template <class T>
static char *WriteDecimal(char *out, T value, const MarkdownCellColumn &column) {
	const auto length = UnsafeNumericCast<idx_t>(DecimalToString::DecimalLength<T>(value, column.width, column.scale));
	DecimalToString::FormatDecimal<T>(value, column.width, column.scale, out, length);
	return out + length;
}

template <class T>
static const T &CellValue(const UnifiedVectorFormat &column, idx_t idx) {
	return UnifiedVectorFormat::GetData<T>(column)[idx];
}

static char *WriteCell(char *out, const MarkdownCellColumn &cell, const UnifiedVectorFormat &column, idx_t idx,
                       const WriteMarkdownLocalState &lstate, const WriteMarkdownBindData &bind_data) {
	switch (cell.format) {
	case MarkdownCellFormat::TEXT: {
		const auto &text = CellValue<string_t>(column, idx);
		const std::string_view value(text.GetData(), text.GetSize());
		if (cell.check && lstate.cell_specials.In(value)) {
			return WriteEscapedCell(out, value, bind_data);
		}
		return CopyCell(out, value);
	}
	case MarkdownCellFormat::BOOLEAN:
		if (CellValue<bool>(column, idx)) {
			memcpy(out, "true", 4);
			return out + 4;
		}
		memcpy(out, "false", 5);
		return out + 5;
	case MarkdownCellFormat::INT8:
		return WriteInteger(out, CellValue<int8_t>(column, idx));
	case MarkdownCellFormat::INT16:
		return WriteInteger(out, CellValue<int16_t>(column, idx));
	case MarkdownCellFormat::INT32:
		return WriteInteger(out, CellValue<int32_t>(column, idx));
	case MarkdownCellFormat::INT64:
		return WriteInteger(out, CellValue<int64_t>(column, idx));
	case MarkdownCellFormat::UINT8:
		return WriteInteger(out, CellValue<uint8_t>(column, idx));
	case MarkdownCellFormat::UINT16:
		return WriteInteger(out, CellValue<uint16_t>(column, idx));
	case MarkdownCellFormat::UINT32:
		return WriteInteger(out, CellValue<uint32_t>(column, idx));
	case MarkdownCellFormat::UINT64:
		return WriteInteger(out, CellValue<uint64_t>(column, idx));
	case MarkdownCellFormat::DECIMAL16:
		return WriteDecimal(out, CellValue<int16_t>(column, idx), cell);
	case MarkdownCellFormat::DECIMAL32:
		return WriteDecimal(out, CellValue<int32_t>(column, idx), cell);
	case MarkdownCellFormat::DECIMAL64:
		return WriteDecimal(out, CellValue<int64_t>(column, idx), cell);
	case MarkdownCellFormat::DECIMAL128:
		return WriteDecimal(out, CellValue<hugeint_t>(column, idx), cell);
	case MarkdownCellFormat::DATE:
		return out + DateToStringCast::Write(CellValue<date_t>(column, idx), out);
	case MarkdownCellFormat::TIME:
		return out + TimeToStringCast::Write(CellValue<dtime_t>(column, idx), out);
	case MarkdownCellFormat::TIME_NS:
		return out + TimeToStringCast::Write(CellValue<dtime_ns_t>(column, idx), out);
	case MarkdownCellFormat::TIMESTAMP:
		return out + TimestampToStringCast::Write(CellValue<timestamp_t>(column, idx), out);
	case MarkdownCellFormat::TIMESTAMP_NS:
		return out + TimestampToStringCast::Write(CellValue<timestamp_ns_t>(column, idx), out);
	case MarkdownCellFormat::INTERVAL:
		return out + IntervalToStringCast::Format(CellValue<interval_t>(column, idx), out);
	case MarkdownCellFormat::UUID:
		UUID::ToString(CellValue<hugeint_t>(column, idx), out);
		return out + UUID::STRING_SIZE;
	}
	throw InternalException("Unknown Markdown cell format");
}

void MarkdownCopyFunction::AppendTableRows(WriteMarkdownLocalState &lstate, const vector<UnifiedVectorFormat> &columns,
                                           idx_t count, const WriteMarkdownBindData &bind_data) {
	idx_t bound = count * (2 + 3 * columns.size()) + CELL_SLACK;
	for (idx_t col_idx = 0; col_idx < columns.size(); col_idx++) {
		const auto &column = columns[col_idx];
		const auto &cell = lstate.cell_columns[col_idx];
		if (cell.format != MarkdownCellFormat::TEXT) {
			bound += count * MaxValue(cell.max_length, lstate.escaped_null.size());
			continue;
		}
		const auto values = UnifiedVectorFormat::GetData<string_t>(column);
		const idx_t growth = cell.check ? MAX_ESCAPED_BYTE : 1;
		for (idx_t row_idx = 0; row_idx < count; row_idx++) {
			const auto idx = column.sel->get_index(row_idx);
			bound += column.validity.RowIsValid(idx) ? values[idx].GetSize() * growth : lstate.escaped_null.size();
		}
	}
	const auto old_size = lstate.buffer.size();
	lstate.buffer.resize_and_overwrite(old_size + bound, [&](char *data, size_t) {
		auto out = data + old_size;
		for (idx_t row_idx = 0; row_idx < count; row_idx++) {
			*out++ = '|';
			for (idx_t col_idx = 0; col_idx < columns.size(); col_idx++) {
				const auto &column = columns[col_idx];
				const auto idx = column.sel->get_index(row_idx);
				*out++ = ' ';
				out = column.validity.RowIsValid(idx)
				          ? WriteCell(out, lstate.cell_columns[col_idx], column, idx, lstate, bind_data)
				          : CopyCell(out, lstate.escaped_null);
				*out++ = ' ';
				*out++ = '|';
			}
			*out++ = '\n';
		}
		return static_cast<size_t>(out - data);
	});
}

//===--------------------------------------------------------------------===//
// Document Mode Helpers
//===--------------------------------------------------------------------===//

string MarkdownCopyFunction::RenderFrontmatter(const WriteMarkdownBindData &bind_data) {
	if (bind_data.frontmatter.empty()) {
		return "";
	}
	return "---\n" + bind_data.frontmatter + "\n---\n\n";
}

string MarkdownCopyFunction::RenderSection(int32_t level, const string &title, const string &content,
                                           const WriteMarkdownBindData &bind_data) {
	string result;

	// Level 0 is treated as frontmatter content (if frontmatter option not set)
	if (level == 0) {
		// Level 0 sections output raw content (typically YAML frontmatter)
		if (!content.empty()) {
			result = "---\n" + content + "\n---\n\n";
		}
		return result;
	}

	// Regular heading (level 1-6)
	if (level > 0 && level <= 6 && !title.empty()) {
		result = string(level, '#') + " " + title + "\n";
	}

	// Content
	if (!content.empty()) {
		result += "\n" + content + "\n";
	}

	// Blank lines between sections
	for (int32_t i = 0; i < bind_data.blank_lines; i++) {
		result += "\n";
	}

	return result;
}

// The blocks-mode renderer that lived here -- RenderInlineElement,
// RenderBlockElement and RenderElement, ~340 lines -- is GONE. It was a second
// implementation of duck_block rendering, and it had drifted: rendering one
// element at a time, it could not absorb a block's inline run, so a paragraph
// with NULL content and its inline children came out as separate blocks with
// spurious blank lines between them. Sink now rebuilds the duck_block and
// Combine calls DuckBlockFunctions::RenderDuckBlocksToMarkdown -- the same
// renderer duck_blocks_to_md uses.
//
// Worth recording what deleting it FOUND: routing this path through the shared
// renderer immediately failed a test this path had always passed, because the
// shared renderer deleted the inline run after an `hr`. Two renderers meant each
// bug was invisible to the tests that would have caught it in the other.

} // namespace duckdb
