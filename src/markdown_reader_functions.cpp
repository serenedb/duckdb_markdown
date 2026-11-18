#include "markdown_reader.hpp"
#include "duckdb_compat.hpp"
#include "markdown_types.hpp"
#include "markdown_vector_writer.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/multi_file/table_function_multi_file.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

namespace duckdb {

enum class MarkdownReadKind : uint8_t { DOCUMENTS, SECTIONS, BLOCKS };

struct MarkdownFileBindData : public TableFunctionData {
	MarkdownReader::MarkdownReadOptions options;
	string file;
	vector<LogicalType> types;
};

struct MarkdownFileGlobalState : public GlobalTableFunctionState {
	bool loaded = false;
	string content;
	vector<markdown_utils::MarkdownSection> sections;
	vector<markdown_utils::MarkdownBlock> blocks;
	idx_t rows = 0;
	idx_t position = 0;
	bool projected = false;
	vector<column_t> column_ids;
	DataChunk all_columns;
};

//===--------------------------------------------------------------------===//
// Optional add-on extractor columns (extract_extensions param)
//===--------------------------------------------------------------------===//
// Shared between binds (which add LIST<STRUCT> columns to the output schema
// when the corresponding option is enabled) and executes (which populate those
// columns from each row's content via the same primitives the scalar
// md_extract_wikilinks / md_extract_tags functions use).

static LogicalType WikilinkStructType() {
	child_list_t<LogicalType> fields;
	fields.push_back({"target", LogicalType(LogicalTypeId::VARCHAR)});
	fields.push_back({"alias", LogicalType(LogicalTypeId::VARCHAR)});
	fields.push_back({"anchor", LogicalType(LogicalTypeId::VARCHAR)});
	fields.push_back({"is_embed", LogicalType(LogicalTypeId::BOOLEAN)});
	fields.push_back({"line_number", LogicalType(LogicalTypeId::BIGINT)});
	return LogicalType::STRUCT(fields);
}

static LogicalType TagStructType() {
	child_list_t<LogicalType> fields;
	fields.push_back({"tag", LogicalType(LogicalTypeId::VARCHAR)});
	fields.push_back({"line_number", LogicalType(LogicalTypeId::BIGINT)});
	return LogicalType::STRUCT(fields);
}

// Strip backslash-escapes from the inline characters our regex matches on
// (`[`, `]`, `#`, `^`, `|`, `!`). `ExtractSections` / block extraction routes
// per-row content through cmark's plaintext rendering, which escapes markdown
// special characters — so `[[Meeting^abc]]` arrives as `\[\[Meeting\^abc\]\]`
// and the wikilink regex won't match. Per-file content (read_markdown's raw
// file bytes) has no escapes, so this is idempotent there.
static std::string UnescapeMarkdownInline(const std::string &in) {
	std::string out;
	out.reserve(in.size());
	for (size_t i = 0; i < in.size(); i++) {
		if (in[i] == '\\' && i + 1 < in.size()) {
			char next = in[i + 1];
			if (next == '[' || next == ']' || next == '#' || next == '^' || next == '|' || next == '!' ||
			    next == '\\') {
				out.push_back(next);
				i++;
				continue;
			}
		}
		out.push_back(in[i]);
	}
	return out;
}

static void WriteWikilinks(Vector &result, idx_t row, const std::string &content) {
	markdown_writer::WriteStructList(result, row, markdown_utils::ExtractWikilinks(UnescapeMarkdownInline(content)),
	                                 markdown_writer::WriteWikilink);
}

static void WriteTags(Vector &result, idx_t row, const std::string &content) {
	markdown_writer::WriteStructList(result, row, markdown_utils::ExtractTags(UnescapeMarkdownInline(content)),
	                                 markdown_writer::WriteTag);
}

static void HandleUnreadableFile(ClientContext &context, const MarkdownReader::MarkdownReadOptions &options,
                                 const string &file_path, const std::exception &e) {
	const auto reason = ErrorData(e).RawMessage();
	if (!options.ignore_errors) {
		throw InvalidInputException("Error reading Markdown file %s: %s", file_path, reason);
	}
	context.WarnUser(StringUtil::Format("Skipped %s, it could not be read: %s", file_path, reason));
}

static void ParseMarkdownOptions(const named_argument_map_t &named_parameters,
                                 MarkdownReader::MarkdownReadOptions &options) {
	for (const auto &kv : named_parameters) {
		// v2.0 keys this map by Identifier, not string: CompatNameStr spans both lines,
		// so every comparison and the unknown-key throw below stay as they were.
		const auto key = CompatNameStr(kv.first);
		if (key == "extract_metadata") {
			options.extract_metadata = BooleanValue::Get(kv.second);
		} else if (key == "include_stats") {
			options.include_stats = BooleanValue::Get(kv.second);
		} else if (key == "normalize_content") {
			options.normalize_content = BooleanValue::Get(kv.second);
		} else if (key == "maximum_file_size") {
			options.maximum_file_size = UBigIntValue::Get(kv.second);
		} else if (key == "ignore_errors") {
			options.ignore_errors = BooleanValue::Get(kv.second);
		} else if (key == "flavor") {
			const auto flavor_str = StringValue::Get(kv.second);
			if (flavor_str == "gfm") {
				options.flavor = markdown_utils::MarkdownFlavor::GFM;
			} else if (flavor_str == "commonmark") {
				options.flavor = markdown_utils::MarkdownFlavor::COMMONMARK;
			} else if (flavor_str == "multimarkdown") {
				options.flavor = markdown_utils::MarkdownFlavor::MULTIMARKDOWN;
			} else {
				throw InvalidInputException("Unknown markdown flavor: %s", flavor_str);
			}
		} else if (key == "include_content") {
			options.include_content = BooleanValue::Get(kv.second);
		} else if (key == "min_level") {
			options.min_level = IntegerValue::Get(kv.second);
		} else if (key == "max_level") {
			options.max_level = IntegerValue::Get(kv.second);
		} else if (key == "include_empty_sections") {
			options.include_empty_sections = BooleanValue::Get(kv.second);
		} else if (key == "include_filepath" || key == "filename") {
			options.include_filepath = BooleanValue::Get(kv.second);
		} else if (key == "content_as_varchar") {
			options.content_as_varchar = BooleanValue::Get(kv.second);
		} else if (key == "content_mode") {
			auto mode = StringValue::Get(kv.second);
			if (mode != "minimal" && mode != "full" && mode != "smart") {
				throw InvalidInputException("content_mode must be 'minimal', 'full', or 'smart', got: %s", mode);
			}
			options.content_mode = mode;
		} else if (key == "max_depth") {
			options.max_depth = IntegerValue::Get(kv.second);
			if (options.max_depth < 1 || options.max_depth > 6) {
				throw InvalidInputException("max_depth must be between 1 and 6");
			}
		} else if (key == "max_content_length") {
			options.max_content_length = UBigIntValue::Get(kv.second);
		} else if (key == "section_filter") {
			options.section_filter = StringValue::Get(kv.second);
		} else if (key == "extract_extensions") {
			// Opt-in add-on extractors. Comma-separated VARCHAR — each token is a flavor
			// ('obsidian' → wikilinks + tags) or a feature ('wikilinks', 'tags').
			// NULL/unset is the default (no extensions); unknown tokens throw, matching
			// the existing `flavor` param's strictness.
			if (!kv.second.IsNull()) {
				auto raw = StringValue::Get(kv.second);
				auto tokens = StringUtil::Split(raw, ",");
				for (auto &tok : tokens) {
					markdown_utils::TrimWhitespace(tok);
					if (tok.empty()) {
						continue;
					}
					if (tok == "obsidian") {
						options.extract_wikilinks = true;
						options.extract_tags = true;
					} else if (tok == "wikilinks") {
						options.extract_wikilinks = true;
					} else if (tok == "tags") {
						options.extract_tags = true;
					} else {
						throw InvalidInputException("Unknown extract_extensions feature: '%s'. Known: 'obsidian' "
						                            "(flavor expanding to wikilinks+tags), 'wikilinks', 'tags'.",
						                            tok);
					}
				}
			}
		} else {
			throw InvalidInputException("Unknown parameter for read_markdown: %s", key);
		}
	}
}

// Helper to parse section fragment from path (e.g., "doc.md#section-id" -> {"doc.md", "section-id"})
static std::pair<string, string> ParseFragmentFromPath(const string &path) {
	auto hash_pos = path.find('#');
	if (hash_pos != string::npos && hash_pos > 0) {
		return {path.substr(0, hash_pos), path.substr(hash_pos + 1)};
	}
	return {path, ""};
}

// Helper to check if a section matches the filter (exact match or is a child of the filter)
static bool SectionMatchesFilter(const string &section_id, const string &section_path, const string &filter) {
	if (filter.empty()) {
		return true;
	}
	// Exact match on section_id
	if (section_id == filter) {
		return true;
	}
	// Check if section_path starts with filter (is a child of the filtered section)
	if (StringUtil::StartsWith(section_path, filter + "/")) {
		return true;
	}
	// Check if section_path contains the filter as a component
	if (section_path.find("/" + filter + "/") != string::npos ||
	    section_path.find("/" + filter) == section_path.length() - filter.length() - 1) {
		return true;
	}
	return false;
}

//===--------------------------------------------------------------------===//
// Schemas
//===--------------------------------------------------------------------===//

static void AddTrailingColumns(const MarkdownReader::MarkdownReadOptions &options, vector<LogicalType> &return_types,
                               vector<CompatName> &names) {
	// Provenance, trailing. duck_block spec 6.4 puts `filename` after the canonical
	// fields and consumers accept the exact 8-field struct with it LAST, so a leading
	// field would put the path at index 0 where index-based consumers expect `kind`.
	// DuckDB core (read_csv/read_json/read_parquet) also names it `filename` and
	// appends it. Only the boolean form is accepted: a renamed column produces a
	// struct no duck_block consumer accepts.
	if (options.include_filepath) {
		names.emplace_back("filename");
		return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
	}
	// Optional add-on extractor columns (extract_extensions param)
	if (options.extract_wikilinks) {
		names.emplace_back("wikilinks");
		return_types.emplace_back(LogicalType::LIST(WikilinkStructType()));
	}
	if (options.extract_tags) {
		names.emplace_back("tags");
		return_types.emplace_back(LogicalType::LIST(TagStructType()));
	}
}

static LogicalType ContentType(const MarkdownReader::MarkdownReadOptions &options) {
	return options.content_as_varchar ? LogicalType(LogicalTypeId::VARCHAR) : MarkdownTypes::MarkdownType();
}

static void MarkdownSchema(MarkdownReadKind kind, const MarkdownReader::MarkdownReadOptions &options,
                           vector<LogicalType> &return_types, vector<CompatName> &names) {
	switch (kind) {
	case MarkdownReadKind::DOCUMENTS: {
		names.emplace_back("content");
		return_types.emplace_back(ContentType(options));
		if (options.extract_metadata) {
			names.emplace_back("metadata");
			return_types.emplace_back(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));
		}
		if (options.include_stats) {
			child_list_t<LogicalType> stats_struct;
			stats_struct.push_back(make_pair("word_count", LogicalType(LogicalTypeId::BIGINT)));
			stats_struct.push_back(make_pair("char_count", LogicalType(LogicalTypeId::BIGINT)));
			stats_struct.push_back(make_pair("line_count", LogicalType(LogicalTypeId::BIGINT)));
			stats_struct.push_back(make_pair("heading_count", LogicalType(LogicalTypeId::BIGINT)));
			stats_struct.push_back(make_pair("code_block_count", LogicalType(LogicalTypeId::BIGINT)));
			stats_struct.push_back(make_pair("link_count", LogicalType(LogicalTypeId::BIGINT)));
			stats_struct.push_back(make_pair("reading_time_minutes", LogicalType(LogicalTypeId::DOUBLE)));
			names.emplace_back("stats");
			return_types.emplace_back(LogicalType::STRUCT(stats_struct));
		}
		break;
	}
	case MarkdownReadKind::SECTIONS:
		names.emplace_back("section_id");
		return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
		names.emplace_back("section_path");
		return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
		names.emplace_back("level");
		return_types.emplace_back(LogicalType(LogicalTypeId::INTEGER));
		names.emplace_back("title");
		return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
		names.emplace_back("content");
		return_types.emplace_back(ContentType(options));
		names.emplace_back("parent_id");
		return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
		names.emplace_back("start_line");
		return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
		names.emplace_back("end_line");
		return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
		break;
	case MarkdownReadKind::BLOCKS:
		// Uses duck_block shape: kind, element_type, content, level, encoding, attributes, element_order
		names.emplace_back("kind");
		return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
		names.emplace_back("element_type");
		return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
		names.emplace_back("content");
		return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
		names.emplace_back("level");
		return_types.emplace_back(LogicalType(LogicalTypeId::INTEGER));
		names.emplace_back("encoding");
		return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
		names.emplace_back("attributes");
		return_types.emplace_back(
		    LogicalType::MAP(LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR)));
		names.emplace_back("element_order");
		return_types.emplace_back(LogicalType(LogicalTypeId::INTEGER));
		break;
	}
	AddTrailingColumns(options, return_types, names);
}

//===--------------------------------------------------------------------===//
// One file
//===--------------------------------------------------------------------===//

template <MarkdownReadKind KIND>
static unique_ptr<FunctionData> MarkdownFileBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<CompatName> &names) {
	auto result = make_uniq<MarkdownFileBindData>();
	ParseMarkdownOptions(input.named_parameters, result->options);
	result->file = TableFunctionFileBindInput::GetFile(input).path;
	MarkdownSchema(KIND, result->options, return_types, names);
	result->types = return_types;
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> MarkdownFileInitGlobal(ClientContext &context,
                                                                   TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<MarkdownFileBindData>();
	auto result = make_uniq<MarkdownFileGlobalState>();
	bool identity = input.column_ids.size() == bind.types.size();
	for (idx_t i = 0; identity && i < input.column_ids.size(); i++) {
		identity = input.column_ids[i] == i;
	}
	if (!identity) {
		result->projected = true;
		result->column_ids = input.column_ids;
		result->all_columns.Initialize(BufferAllocator::Get(context), bind.types);
	}
	return std::move(result);
}

template <MarkdownReadKind KIND>
static void LoadMarkdownFile(ClientContext &context, const MarkdownFileBindData &bind, MarkdownFileGlobalState &state) {
	const auto &options = bind.options;
	const auto &file_path = bind.file;
	try {
		string content = MarkdownReader::ReadMarkdownFile(context, file_path, options);
		switch (KIND) {
		case MarkdownReadKind::DOCUMENTS:
			state.content = std::move(content);
			state.rows = 1;
			break;
		case MarkdownReadKind::SECTIONS: {
			// Add frontmatter as a special section if extract_metadata is enabled
			if (options.extract_metadata) {
				string frontmatter = markdown_utils::ExtractRawFrontmatter(content);
				if (!frontmatter.empty()) {
					markdown_utils::MarkdownSection fm_section;
					fm_section.id = "frontmatter";
					fm_section.section_path = "frontmatter";
					fm_section.level = 0; // Special level for frontmatter
					fm_section.title = "frontmatter";
					fm_section.content = frontmatter;
					fm_section.parent_id = "";
					fm_section.position = 0;
					fm_section.start_line = 1;
					// Calculate end line from frontmatter content
					fm_section.end_line = static_cast<idx_t>(std::count(frontmatter.begin(), frontmatter.end(), '\n') +
					                                         2); // +2 for --- delimiters
					state.sections.push_back(fm_section);
				}
			}
			for (auto &section : MarkdownReader::ProcessSections(content, options)) {
				if (SectionMatchesFilter(section.id, section.section_path, options.section_filter)) {
					state.sections.push_back(std::move(section));
				}
			}
			state.rows = state.sections.size();
			break;
		}
		case MarkdownReadKind::BLOCKS:
			// Emit the canonical structured representation: rich text as kind='inline'
			// child rows, not markdown syntax inside `content`.
			state.blocks = markdown_utils::ParseBlocks(content, /*structured_inlines=*/true);
			state.rows = state.blocks.size();
			break;
		}
	} catch (const MarkdownFileTooLarge &e) {
		// The user's own cap, so it propagates rather than skipping. Rebuilt as a
		// clean message here instead of wrapping the inner exception.
		throw InvalidInputException("File %s is too large (%llu bytes, maximum is %llu bytes)", e.file_path,
		                            static_cast<unsigned long long>(e.file_size),
		                            static_cast<unsigned long long>(e.maximum_file_size));
	} catch (const std::exception &e) {
		HandleUnreadableFile(context, options, file_path, e);
		state.rows = 0;
	}
}

static void WriteDocumentRow(const MarkdownFileBindData &bind, MarkdownFileGlobalState &state, DataChunk &output,
                             idx_t output_idx) {
	const auto &options = bind.options;
	const auto &content = state.content;
	idx_t column_idx = 0;
	markdown_writer::WriteString(output.data[column_idx++], output_idx, content);
	if (options.extract_metadata) {
		markdown_writer::WriteStringMap(output.data[column_idx++], output_idx,
		                                markdown_utils::ExtractMetadata(content).custom_fields);
	}
	if (options.include_stats) {
		markdown_writer::WriteStats(output.data[column_idx++], output_idx, markdown_utils::CalculateStats(content));
	}
	if (options.include_filepath) {
		markdown_writer::WriteCheckedString(output.data[column_idx++], output_idx, bind.file);
	}
	if (options.extract_wikilinks) {
		WriteWikilinks(output.data[column_idx++], output_idx, content);
	}
	if (options.extract_tags) {
		WriteTags(output.data[column_idx++], output_idx, content);
	}
}

static void WriteSectionRow(const MarkdownFileBindData &bind, const markdown_utils::MarkdownSection &section,
                            DataChunk &output, idx_t output_idx) {
	const auto &options = bind.options;
	idx_t column_idx = 0;
	markdown_writer::WriteString(output.data[column_idx++], output_idx, section.id);
	markdown_writer::WriteString(output.data[column_idx++], output_idx, section.section_path);
	markdown_writer::WriteNumber(output.data[column_idx++], output_idx, static_cast<int32_t>(section.level));
	markdown_writer::WriteString(output.data[column_idx++], output_idx, section.title);
	markdown_writer::WriteString(output.data[column_idx++], output_idx, section.content);
	markdown_writer::WriteOptionalString(output.data[column_idx++], output_idx, section.parent_id);
	markdown_writer::WriteNumber(output.data[column_idx++], output_idx, static_cast<int64_t>(section.start_line));
	markdown_writer::WriteNumber(output.data[column_idx++], output_idx, static_cast<int64_t>(section.end_line));
	if (options.include_filepath) {
		markdown_writer::WriteCheckedString(output.data[column_idx++], output_idx, bind.file);
	}
	if (options.extract_wikilinks) {
		WriteWikilinks(output.data[column_idx++], output_idx, section.content);
	}
	if (options.extract_tags) {
		WriteTags(output.data[column_idx++], output_idx, section.content);
	}
}

static void WriteBlockRow(const MarkdownFileBindData &bind, const markdown_utils::MarkdownBlock &block,
                          DataChunk &output, idx_t output_idx) {
	const auto &options = bind.options;
	idx_t column_idx = 0;
	// kind ('block', or 'inline' for structured rich-text children)
	markdown_writer::WriteString(output.data[column_idx++], output_idx, block.kind);
	markdown_writer::WriteString(output.data[column_idx++], output_idx, block.block_type);
	// content (NULL for containers whose text lives in structured inline children)
	markdown_writer::WriteOptionalString(output.data[column_idx++], output_idx, block.content);
	// level (NULL if -1, meaning "not applicable")
	if (block.level >= 0) {
		markdown_writer::WriteNumber(output.data[column_idx], output_idx, block.level);
	} else {
		markdown_writer::WriteNull(output.data[column_idx], output_idx);
	}
	column_idx++;
	markdown_writer::WriteString(output.data[column_idx++], output_idx, block.encoding);
	markdown_writer::WriteStringMap(output.data[column_idx++], output_idx, block.attributes);
	markdown_writer::WriteNumber(output.data[column_idx++], output_idx, block.block_order);
	if (options.include_filepath) {
		markdown_writer::WriteCheckedString(output.data[column_idx++], output_idx, bind.file);
	}
	if (options.extract_wikilinks) {
		WriteWikilinks(output.data[column_idx++], output_idx, block.content);
	}
	if (options.extract_tags) {
		WriteTags(output.data[column_idx++], output_idx, block.content);
	}
}

template <MarkdownReadKind KIND>
static void MarkdownFileScan(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &bind = input.bind_data->Cast<MarkdownFileBindData>();
	auto &state = input.global_state->Cast<MarkdownFileGlobalState>();
	if (!state.loaded) {
		state.loaded = true;
		LoadMarkdownFile<KIND>(context, bind, state);
	}
	const bool projected = state.projected;
	auto &target = projected ? state.all_columns : output;
	if (projected) {
		target.Reset();
	}
	idx_t output_idx = 0;
	for (; state.position < state.rows && output_idx < STANDARD_VECTOR_SIZE; state.position++, output_idx++) {
		switch (KIND) {
		case MarkdownReadKind::DOCUMENTS:
			WriteDocumentRow(bind, state, target, output_idx);
			break;
		case MarkdownReadKind::SECTIONS:
			WriteSectionRow(bind, state.sections[state.position], target, output_idx);
			break;
		case MarkdownReadKind::BLOCKS:
			WriteBlockRow(bind, state.blocks[state.position], target, output_idx);
			break;
		}
	}
	target.SetChildCardinality(output_idx);
	if (!projected) {
		return;
	}
	for (idx_t i = 0; i < state.column_ids.size(); i++) {
		const auto column_id = state.column_ids[i];
		if (IsVirtualColumn(column_id)) {
			output.data[i].Reference(Value(output.data[i].GetType()), count_t(output_idx));
		} else {
			output.data[i].Reference(target.data[column_id]);
		}
	}
	output.SetChildCardinality(output_idx);
}

//===--------------------------------------------------------------------===//
// Files, through the multi-file reader
//===--------------------------------------------------------------------===//

static TableFunction MarkdownFileFunction(MarkdownReadKind kind) {
	TableFunction function;
	switch (kind) {
	case MarkdownReadKind::DOCUMENTS:
		function =
		    TableFunction("read_markdown_file", {LogicalType::VARCHAR}, MarkdownFileScan<MarkdownReadKind::DOCUMENTS>,
		                  MarkdownFileBind<MarkdownReadKind::DOCUMENTS>, MarkdownFileInitGlobal);
		// Named parameters. One call per function: v2.0 CREATES the kwargs group here.
		// EVERY ENTRY BELOW IS EXERCISED by test/sql/named_parameter_binding.test, which
		// asserts each declared name still binds (not what it does) plus a negative
		// control that an unknown name is still rejected. Add a parameter here, add a
		// line there.
		CompatDeclareNamedParams(function, {
		                                       {"extract_metadata", LogicalType(LogicalTypeId::BOOLEAN)},
		                                       {"include_stats", LogicalType(LogicalTypeId::BOOLEAN)},
		                                       {"normalize_content", LogicalType(LogicalTypeId::BOOLEAN)},
		                                       {"maximum_file_size", LogicalType(LogicalTypeId::UBIGINT)},
		                                       {"ignore_errors", LogicalType(LogicalTypeId::BOOLEAN)},
		                                       {"extract_extensions", LogicalType(LogicalTypeId::VARCHAR)},
		                                       {"flavor", LogicalType(LogicalTypeId::VARCHAR)},
		                                       {"include_filepath", LogicalType(LogicalTypeId::BOOLEAN)},
		                                       {"content_as_varchar", LogicalType(LogicalTypeId::BOOLEAN)},
		                                   });
		break;
	case MarkdownReadKind::SECTIONS:
		function = TableFunction("read_markdown_sections_file", {LogicalType::VARCHAR},
		                         MarkdownFileScan<MarkdownReadKind::SECTIONS>,
		                         MarkdownFileBind<MarkdownReadKind::SECTIONS>, MarkdownFileInitGlobal);
		CompatDeclareNamedParams(function, {
		                                       {"extract_metadata", LogicalType(LogicalTypeId::BOOLEAN)},
		                                       {"include_stats", LogicalType(LogicalTypeId::BOOLEAN)},
		                                       {"normalize_content", LogicalType(LogicalTypeId::BOOLEAN)},
		                                       {"maximum_file_size", LogicalType(LogicalTypeId::UBIGINT)},
		                                       {"ignore_errors", LogicalType(LogicalTypeId::BOOLEAN)},
		                                       {"extract_extensions", LogicalType(LogicalTypeId::VARCHAR)},
		                                       {"flavor", LogicalType(LogicalTypeId::VARCHAR)},
		                                       {"include_content", LogicalType(LogicalTypeId::BOOLEAN)},
		                                       {"min_level", LogicalType(LogicalTypeId::INTEGER)},
		                                       {"max_level", LogicalType(LogicalTypeId::INTEGER)},
		                                       {"include_empty_sections", LogicalType(LogicalTypeId::BOOLEAN)},
		                                       {"include_filepath", LogicalType(LogicalTypeId::BOOLEAN)},
		                                       {"content_as_varchar", LogicalType(LogicalTypeId::BOOLEAN)},
		                                       {"content_mode", LogicalType(LogicalTypeId::VARCHAR)},
		                                       {"max_depth", LogicalType(LogicalTypeId::INTEGER)},
		                                       {"max_content_length", LogicalType(LogicalTypeId::UBIGINT)},
		                                       {"section_filter", LogicalType(LogicalTypeId::VARCHAR)},
		                                   });
		break;
	case MarkdownReadKind::BLOCKS:
		function = TableFunction("read_markdown_blocks_file", {LogicalType::VARCHAR},
		                         MarkdownFileScan<MarkdownReadKind::BLOCKS>, MarkdownFileBind<MarkdownReadKind::BLOCKS>,
		                         MarkdownFileInitGlobal);
		CompatDeclareNamedParams(function, {
		                                       {"extract_metadata", LogicalType(LogicalTypeId::BOOLEAN)},
		                                       {"normalize_content", LogicalType(LogicalTypeId::BOOLEAN)},
		                                       {"maximum_file_size", LogicalType(LogicalTypeId::UBIGINT)},
		                                       {"ignore_errors", LogicalType(LogicalTypeId::BOOLEAN)},
		                                       {"extract_extensions", LogicalType(LogicalTypeId::VARCHAR)},
		                                       {"include_filepath", LogicalType(LogicalTypeId::BOOLEAN)},
		                                   });
		break;
	}
	function.projection_pushdown = true;
	return function;
}

static TableFunctionMultiFileSettings MarkdownFileSettings() {
	TableFunctionMultiFileSettings settings;
	settings.glob_input = FileGlobOptions::ALLOW_EMPTY;
	settings.reader_type = "Markdown";
	return settings;
}

static optional<Value> TakeNamedParameter(named_argument_map_t &named_parameters, const string &name) {
	for (auto it = named_parameters.begin(); it != named_parameters.end(); ++it) {
		if (StringUtil::CIEquals(CompatNameStr(it->first), name)) {
			auto value = it->second;
			named_parameters.erase(it);
			return value;
		}
	}
	return nullopt;
}

template <MarkdownReadKind KIND>
static unique_ptr<FunctionData> MarkdownBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<CompatName> &names) {
	if (input.inputs.empty()) {
		throw InvalidInputException("read_markdown requires at least one argument");
	}
	auto &named_parameters = input.named_parameters;
	auto filename = TakeNamedParameter(named_parameters, "filename");
	if (filename) {
		named_parameters[CompatMakeName("include_filepath")] = *filename;
	}
	MarkdownReader::MarkdownReadOptions options;
	ParseMarkdownOptions(named_parameters, options);

	Value paths = input.inputs[0];
	if (KIND == MarkdownReadKind::SECTIONS) {
		string fragment;
		if (paths.type().id() == LogicalTypeId::VARCHAR) {
			auto parsed = ParseFragmentFromPath(paths.ToString());
			paths = Value(parsed.first);
			fragment = parsed.second;
		} else if (paths.type().id() == LogicalTypeId::LIST) {
			vector<Value> clean_paths;
			for (auto &file_value : ListValue::GetChildren(paths)) {
				auto parsed = ParseFragmentFromPath(file_value.ToString());
				clean_paths.emplace_back(parsed.first);
				if (fragment.empty()) {
					fragment = parsed.second;
				}
			}
			paths = Value::LIST(LogicalType::VARCHAR, std::move(clean_paths));
		}
		if (options.section_filter.empty() && !fragment.empty()) {
			named_parameters[CompatMakeName("section_filter")] = Value(fragment);
		}
	}

	vector<Value> files;
	for (auto &file : MarkdownReader::GetFiles(context, paths, options.ignore_errors)) {
		files.emplace_back(file);
	}
	if (files.empty()) {
		MarkdownSchema(KIND, options, return_types, names);
	}
	input.inputs[0] = Value::LIST(LogicalType::VARCHAR, std::move(files));
	return TableFunctionMultiFileWrapper::MultiFileBindWith(context, input, return_types, names,
	                                                        MarkdownFileFunction(KIND), MarkdownFileSettings());
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

template <MarkdownReadKind KIND>
static void RegisterReader(ExtensionLoader &loader, const string &name, const string &description,
                           const string &example) {
	auto function_set = TableFunctionMultiFileWrapper::CreateFunctionSet(
	    MarkdownFileFunction(KIND), CompatMakeName(name), MarkdownFileSettings(), MarkdownBind<KIND>);
	CreateTableFunctionInfo info(std::move(function_set));
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription desc;
	desc.parameter_names = {"path"};
	desc.description = description;
	desc.examples = {example};
	desc.categories = {"markdown"};
	info.descriptions.push_back(desc);
	loader.RegisterFunction(std::move(info));
}

void MarkdownReader::RegisterFunction(ExtensionLoader &loader) {
	RegisterReader<MarkdownReadKind::DOCUMENTS>(
	    loader, "read_markdown", "Read Markdown documents from files into table format with frontmatter and content.",
	    "SELECT * FROM read_markdown('README.md')");
	RegisterReader<MarkdownReadKind::SECTIONS>(loader, "read_markdown_sections",
	                                           "Read Markdown files split by headings into structured sections.",
	                                           "SELECT * FROM read_markdown_sections('README.md')");
	RegisterReader<MarkdownReadKind::BLOCKS>(loader, "read_markdown_blocks",
	                                         "Read Markdown files parsed into atomic block elements.",
	                                         "SELECT * FROM read_markdown_blocks('README.md')");
}

} // namespace duckdb
