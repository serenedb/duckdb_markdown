#include "markdown_reader.hpp"
#include "duckdb_compat.hpp"
#include "markdown_types.hpp"
#include "markdown_vector_writer.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Bind Data Structures
//===--------------------------------------------------------------------===//

struct MarkdownReadDocumentBindData : public TableFunctionData {
	vector<string> files;
	MarkdownReader::MarkdownReadOptions options;
	idx_t current_file_index = 0;
};

struct MarkdownReadSectionBindData : public TableFunctionData {
	vector<string> files;
	MarkdownReader::MarkdownReadOptions options;
	vector<markdown_utils::MarkdownSection> all_sections;
	idx_t current_section_index = 0;
};

struct MarkdownReadBlocksBindData : public TableFunctionData {
	vector<string> files;
	MarkdownReader::MarkdownReadOptions options;
	// Flattened blocks with file path association
	vector<pair<string, markdown_utils::MarkdownBlock>> all_blocks;
	idx_t current_block_index = 0;
};

//===--------------------------------------------------------------------===//
// Helper Functions
//===--------------------------------------------------------------------===//

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

static void ParseMarkdownOptions(TableFunctionBindInput &input, MarkdownReader::MarkdownReadOptions &options) {
	for (const auto &kv : input.named_parameters) {
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

//===--------------------------------------------------------------------===//
// Document Reader Implementation
//===--------------------------------------------------------------------===//

unique_ptr<FunctionData> MarkdownReader::MarkdownReadDocumentsBind(ClientContext &context,
                                                                   TableFunctionBindInput &input,
                                                                   vector<LogicalType> &return_types,
                                                                   vector<CompatName> &names) {
	auto result = make_uniq<MarkdownReadDocumentBindData>();

	// Parse the file path parameter
	if (input.inputs.empty()) {
		throw InvalidInputException("read_markdown requires at least one argument");
	}

	ParseMarkdownOptions(input, result->options);

	auto &path_param = input.inputs[0];
	result->files = GetFiles(context, path_param, result->options.ignore_errors);

	// Define return columns
	names.emplace_back("content");
	if (result->options.content_as_varchar) {
		return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
	} else {
		return_types.emplace_back(MarkdownTypes::MarkdownType());
	}

	if (result->options.extract_metadata) {
		names.emplace_back("metadata");
		return_types.emplace_back(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));
	}

	if (result->options.include_stats) {
		names.emplace_back("stats");

		// Create stats struct type
		child_list_t<LogicalType> stats_struct;
		stats_struct.push_back(make_pair("word_count", LogicalType(LogicalTypeId::BIGINT)));
		stats_struct.push_back(make_pair("char_count", LogicalType(LogicalTypeId::BIGINT)));
		stats_struct.push_back(make_pair("line_count", LogicalType(LogicalTypeId::BIGINT)));
		stats_struct.push_back(make_pair("heading_count", LogicalType(LogicalTypeId::BIGINT)));
		stats_struct.push_back(make_pair("code_block_count", LogicalType(LogicalTypeId::BIGINT)));
		stats_struct.push_back(make_pair("link_count", LogicalType(LogicalTypeId::BIGINT)));
		stats_struct.push_back(make_pair("reading_time_minutes", LogicalType(LogicalTypeId::DOUBLE)));

		return_types.emplace_back(LogicalType::STRUCT(stats_struct));
	}

	// Provenance, trailing. duck_block spec 6.4 puts `filename` after the canonical
	// fields and consumers accept the exact 8-field struct with it LAST, so a leading
	// field would put the path at index 0 where index-based consumers expect `kind`.
	// DuckDB core (read_csv/read_json/read_parquet) also names it `filename` and
	// appends it. Only the boolean form is accepted: a renamed column produces a
	// struct no duck_block consumer accepts.
	if (result->options.include_filepath) {
		names.emplace_back("filename");
		return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
	}

	// Optional add-on extractor columns (extract_extensions param)
	if (result->options.extract_wikilinks) {
		names.emplace_back("wikilinks");
		return_types.emplace_back(LogicalType::LIST(WikilinkStructType()));
	}
	if (result->options.extract_tags) {
		names.emplace_back("tags");
		return_types.emplace_back(LogicalType::LIST(TagStructType()));
	}

	return std::move(result);
}

void MarkdownReader::MarkdownReadDocumentsFunction(ClientContext &context, TableFunctionInput &input,
                                                   DataChunk &output) {
	auto &bind_data = input.bind_data->CastNoConst<MarkdownReadDocumentBindData>();

	if (bind_data.current_file_index >= bind_data.files.size()) {
		output.SetChildCardinality(0);
		return;
	}

	idx_t output_idx = 0;

	while (bind_data.current_file_index < bind_data.files.size() && output_idx < STANDARD_VECTOR_SIZE) {
		auto &file_path = bind_data.files[bind_data.current_file_index];

		try {
			// Read file content
			string content = ReadMarkdownFile(context, file_path, bind_data.options);

			idx_t column_idx = 0;

			// Set content
			markdown_writer::WriteString(output.data[column_idx], output_idx, content);
			column_idx++;

			// Set metadata if requested
			if (bind_data.options.extract_metadata) {
				markdown_writer::WriteStringMap(output.data[column_idx], output_idx,
				                                markdown_utils::ExtractMetadata(content).custom_fields);
				column_idx++;
			}

			// Set stats if requested
			if (bind_data.options.include_stats) {
				markdown_writer::WriteStats(output.data[column_idx], output_idx,
				                            markdown_utils::CalculateStats(content));
				column_idx++;
			}

			// Provenance, trailing -- see the bind for why.
			if (bind_data.options.include_filepath) {
				markdown_writer::WriteCheckedString(output.data[column_idx], output_idx, file_path);
				column_idx++;
			}

			// Optional add-on extractor columns
			if (bind_data.options.extract_wikilinks) {
				WriteWikilinks(output.data[column_idx], output_idx, content);
				column_idx++;
			}
			if (bind_data.options.extract_tags) {
				WriteTags(output.data[column_idx], output_idx, content);
				column_idx++;
			}

			output_idx++;

		} catch (const MarkdownFileTooLarge &e) {
			// The user's own cap, so it propagates rather than skipping. Rebuilt as
			// a clean message here instead of wrapping the inner exception: the old
			// generic handler below used e.what(), which on a DuckDB exception is
			// serialised JSON, so the user saw
			//   Error reading Markdown file x.md: {"exception_type":"Invalid Input",
			//   "exception_message":"File x.md is too large (122 bytes, maximum ..."
			throw InvalidInputException("File %s is too large (%llu bytes, maximum is %llu bytes)", e.file_path,
			                            static_cast<unsigned long long>(e.file_size),
			                            static_cast<unsigned long long>(e.maximum_file_size));
		} catch (const std::exception &e) {
			HandleUnreadableFile(context, bind_data.options, file_path, e);
		}

		bind_data.current_file_index++;
	}

	output.SetChildCardinality(output_idx);
}

//===--------------------------------------------------------------------===//
// Section Reader Implementation
//===--------------------------------------------------------------------===//

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

unique_ptr<FunctionData> MarkdownReader::MarkdownReadSectionsBind(ClientContext &context, TableFunctionBindInput &input,
                                                                  vector<LogicalType> &return_types,
                                                                  vector<CompatName> &names) {
	auto result = make_uniq<MarkdownReadSectionBindData>();

	// Parse the file path parameter
	if (input.inputs.empty()) {
		throw InvalidInputException("read_markdown_sections requires at least one argument");
	}

	// Parse options first to get any section_filter from parameters
	ParseMarkdownOptions(input, result->options);

	// Handle fragment syntax in file paths
	auto &path_param = input.inputs[0];
	string global_fragment;

	if (path_param.type().id() == LogicalTypeId::VARCHAR) {
		// Single path - check for fragment
		auto [clean_path, fragment] = ParseFragmentFromPath(path_param.ToString());
		if (!fragment.empty()) {
			global_fragment = fragment;
			result->files = GetFiles(context, Value(clean_path), result->options.ignore_errors);
		} else {
			result->files = GetFiles(context, path_param, result->options.ignore_errors);
		}
	} else if (path_param.type().id() == LogicalTypeId::LIST) {
		// List of paths - check each for fragments
		auto &file_list = ListValue::GetChildren(path_param);
		vector<Value> clean_paths;
		for (auto &file_value : file_list) {
			if (file_value.type().id() == LogicalTypeId::VARCHAR) {
				auto [clean_path, fragment] = ParseFragmentFromPath(file_value.ToString());
				clean_paths.push_back(Value(clean_path));
				if (!fragment.empty() && global_fragment.empty()) {
					global_fragment = fragment; // Use first fragment found
				}
			}
		}
		result->files =
		    GetFiles(context, Value::LIST(LogicalType::VARCHAR, clean_paths), result->options.ignore_errors);
	} else {
		result->files = GetFiles(context, path_param, result->options.ignore_errors);
	}

	// Use fragment as section_filter if not already set via parameter
	if (result->options.section_filter.empty() && !global_fragment.empty()) {
		result->options.section_filter = global_fragment;
	}

	// Pre-process all files to extract sections
	for (const auto &file_path : result->files) {
		try {
			string content = ReadMarkdownFile(context, file_path, result->options);

			// Add frontmatter as a special section if extract_metadata is enabled
			if (result->options.extract_metadata) {
				string frontmatter = markdown_utils::ExtractRawFrontmatter(content);
				if (!frontmatter.empty()) {
					markdown_utils::MarkdownSection fm_section;
					fm_section.id = "frontmatter";
					fm_section.section_path = "frontmatter";
					fm_section.level = 0; // Special level for frontmatter
					fm_section.title = file_path + "|frontmatter";
					fm_section.content = frontmatter;
					fm_section.parent_id = "";
					fm_section.position = 0;
					fm_section.start_line = 1;
					// Calculate end line from frontmatter content
					fm_section.end_line = static_cast<idx_t>(std::count(frontmatter.begin(), frontmatter.end(), '\n') +
					                                         2); // +2 for --- delimiters
					result->all_sections.push_back(fm_section);
				}
			}

			auto sections = ProcessSections(content, result->options);

			// Add file path to each section for later retrieval
			// Apply section_filter if specified
			for (auto &section : sections) {
				// Check if section matches filter
				if (!SectionMatchesFilter(section.id, section.section_path, result->options.section_filter)) {
					continue;
				}
				section.title = file_path + "|" + section.title; // Store file path temporarily
				result->all_sections.push_back(section);
			}
		} catch (const MarkdownFileTooLarge &e) {
			// The user's own cap: propagate rather than skip. This used to fall into
			// the handler below and vanish, so a file over the limit silently
			// shortened the result while read_markdown raised on the same input.
			throw InvalidInputException("File %s is too large (%llu bytes, maximum is %llu bytes)", e.file_path,
			                            static_cast<unsigned long long>(e.file_size),
			                            static_cast<unsigned long long>(e.maximum_file_size));
		} catch (const std::exception &e) {
			HandleUnreadableFile(context, result->options, file_path, e);
			continue;
		}
	}

	// Define return columns for sections
	names.emplace_back("section_id");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

	names.emplace_back("section_path");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

	names.emplace_back("level");
	return_types.emplace_back(LogicalType(LogicalTypeId::INTEGER));

	names.emplace_back("title");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

	names.emplace_back("content");
	if (result->options.content_as_varchar) {
		return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
	} else {
		return_types.emplace_back(MarkdownTypes::MarkdownType());
	}

	names.emplace_back("parent_id");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

	names.emplace_back("start_line");
	return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));

	names.emplace_back("end_line");
	return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));

	// Provenance, trailing. duck_block spec 6.4 puts `filename` after the canonical
	// fields and consumers accept the exact 8-field struct with it LAST, so a leading
	// field would put the path at index 0 where index-based consumers expect `kind`.
	// DuckDB core (read_csv/read_json/read_parquet) also names it `filename` and
	// appends it. Only the boolean form is accepted: a renamed column produces a
	// struct no duck_block consumer accepts.
	if (result->options.include_filepath) {
		names.emplace_back("filename");
		return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
	}

	// Optional add-on extractor columns (per-section: extracted from section.content)
	if (result->options.extract_wikilinks) {
		names.emplace_back("wikilinks");
		return_types.emplace_back(LogicalType::LIST(WikilinkStructType()));
	}
	if (result->options.extract_tags) {
		names.emplace_back("tags");
		return_types.emplace_back(LogicalType::LIST(TagStructType()));
	}

	return std::move(result);
}

void MarkdownReader::MarkdownReadSectionsFunction(ClientContext &context, TableFunctionInput &input,
                                                  DataChunk &output) {
	auto &bind_data = input.bind_data->CastNoConst<MarkdownReadSectionBindData>();

	if (bind_data.current_section_index >= bind_data.all_sections.size()) {
		output.SetChildCardinality(0);
		return;
	}

	idx_t output_idx = 0;

	while (bind_data.current_section_index < bind_data.all_sections.size() && output_idx < STANDARD_VECTOR_SIZE) {
		const auto &section = bind_data.all_sections[bind_data.current_section_index];

		// Extract file path from temporarily stored title (file_path|actual_title)
		auto pipe_pos = section.title.find('|');
		string file_path = section.title.substr(0, pipe_pos);
		string actual_title = section.title.substr(pipe_pos + 1);

		idx_t column_idx = 0;

		markdown_writer::WriteString(output.data[column_idx], output_idx, section.id);
		column_idx++;
		markdown_writer::WriteString(output.data[column_idx], output_idx, section.section_path);
		column_idx++;
		markdown_writer::WriteNumber(output.data[column_idx], output_idx, static_cast<int32_t>(section.level));
		column_idx++;
		markdown_writer::WriteString(output.data[column_idx], output_idx, actual_title);
		column_idx++;
		markdown_writer::WriteString(output.data[column_idx], output_idx, section.content);
		column_idx++;
		markdown_writer::WriteOptionalString(output.data[column_idx], output_idx, section.parent_id);
		column_idx++;
		markdown_writer::WriteNumber(output.data[column_idx], output_idx, static_cast<int64_t>(section.start_line));
		column_idx++;
		markdown_writer::WriteNumber(output.data[column_idx], output_idx, static_cast<int64_t>(section.end_line));
		column_idx++;

		// Provenance, trailing -- see the bind for why.
		if (bind_data.options.include_filepath) {
			markdown_writer::WriteCheckedString(output.data[column_idx], output_idx, file_path);
			column_idx++;
		}

		// Optional add-on extractor columns (extracted from this section's content)
		if (bind_data.options.extract_wikilinks) {
			WriteWikilinks(output.data[column_idx], output_idx, section.content);
			column_idx++;
		}
		if (bind_data.options.extract_tags) {
			WriteTags(output.data[column_idx], output_idx, section.content);
			column_idx++;
		}

		output_idx++;
		bind_data.current_section_index++;
	}

	output.SetChildCardinality(output_idx);
}

//===--------------------------------------------------------------------===//
// Blocks Reader Implementation
//===--------------------------------------------------------------------===//

unique_ptr<FunctionData> MarkdownReader::MarkdownReadBlocksBind(ClientContext &context, TableFunctionBindInput &input,
                                                                vector<LogicalType> &return_types,
                                                                vector<CompatName> &names) {
	auto result = make_uniq<MarkdownReadBlocksBindData>();

	// Parse the file path parameter
	if (input.inputs.empty()) {
		throw InvalidInputException("read_markdown_blocks requires at least one argument");
	}

	ParseMarkdownOptions(input, result->options);

	auto &path_param = input.inputs[0];
	result->files = GetFiles(context, path_param, result->options.ignore_errors);

	// Pre-process all files to extract blocks (flattened)
	for (const auto &file_path : result->files) {
		try {
			string content = ReadMarkdownFile(context, file_path, result->options);
			// Emit the canonical structured representation: rich text as kind='inline'
			// child rows, not markdown syntax inside `content`.
			auto blocks = markdown_utils::ParseBlocks(content, /*structured_inlines=*/true);
			for (auto &block : blocks) {
				result->all_blocks.push_back({file_path, block});
			}
		} catch (const MarkdownFileTooLarge &e) {
			// The user's own cap: propagate rather than skip. See the matching
			// handler in the sections bind above.
			throw InvalidInputException("File %s is too large (%llu bytes, maximum is %llu bytes)", e.file_path,
			                            static_cast<unsigned long long>(e.file_size),
			                            static_cast<unsigned long long>(e.maximum_file_size));
		} catch (const std::exception &e) {
			HandleUnreadableFile(context, result->options, file_path, e);
			continue;
		}
	}

	// Define return columns (flattened - one row per block)
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

	// Provenance, trailing. duck_block spec 6.4 puts `filename` after the canonical
	// fields and consumers accept the exact 8-field struct with it LAST, so a leading
	// field would put the path at index 0 where index-based consumers expect `kind`.
	// DuckDB core (read_csv/read_json/read_parquet) also names it `filename` and
	// appends it. Only the boolean form is accepted: a renamed column produces a
	// struct no duck_block consumer accepts.
	if (result->options.include_filepath) {
		names.emplace_back("filename");
		return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
	}

	// Optional add-on extractor columns (per-block: extracted from block.content)
	if (result->options.extract_wikilinks) {
		names.emplace_back("wikilinks");
		return_types.emplace_back(LogicalType::LIST(WikilinkStructType()));
	}
	if (result->options.extract_tags) {
		names.emplace_back("tags");
		return_types.emplace_back(LogicalType::LIST(TagStructType()));
	}

	return std::move(result);
}

void MarkdownReader::MarkdownReadBlocksFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &bind_data = input.bind_data->CastNoConst<MarkdownReadBlocksBindData>();

	if (bind_data.current_block_index >= bind_data.all_blocks.size()) {
		output.SetChildCardinality(0);
		return;
	}

	idx_t output_idx = 0;

	while (bind_data.current_block_index < bind_data.all_blocks.size() && output_idx < STANDARD_VECTOR_SIZE) {
		const auto &[file_path, block] = bind_data.all_blocks[bind_data.current_block_index];

		idx_t column_idx = 0;

		// kind ('block', or 'inline' for structured rich-text children)
		markdown_writer::WriteString(output.data[column_idx], output_idx, block.kind);
		column_idx++;

		// element_type (was block_type)
		markdown_writer::WriteString(output.data[column_idx], output_idx, block.block_type);
		column_idx++;

		// content (NULL for containers whose text lives in structured inline children)
		markdown_writer::WriteOptionalString(output.data[column_idx], output_idx, block.content);
		column_idx++;

		// level (NULL if -1, meaning "not applicable")
		if (block.level >= 0) {
			markdown_writer::WriteNumber(output.data[column_idx], output_idx, block.level);
		} else {
			markdown_writer::WriteNull(output.data[column_idx], output_idx);
		}
		column_idx++;

		// encoding
		markdown_writer::WriteString(output.data[column_idx], output_idx, block.encoding);
		column_idx++;

		// attributes MAP
		markdown_writer::WriteStringMap(output.data[column_idx], output_idx, block.attributes);
		column_idx++;

		// element_order (was block_order)
		markdown_writer::WriteNumber(output.data[column_idx], output_idx, block.block_order);
		column_idx++;

		// Provenance, trailing -- see the bind for why.
		if (bind_data.options.include_filepath) {
			markdown_writer::WriteCheckedString(output.data[column_idx], output_idx, file_path);
			column_idx++;
		}

		// Optional add-on extractor columns (extracted from this block's content)
		if (bind_data.options.extract_wikilinks) {
			WriteWikilinks(output.data[column_idx], output_idx, block.content);
			column_idx++;
		}
		if (bind_data.options.extract_tags) {
			WriteTags(output.data[column_idx], output_idx, block.content);
			column_idx++;
		}

		output_idx++;
		bind_data.current_block_index++;
	}

	output.SetChildCardinality(output_idx);
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

// Register a reader under both a single VARCHAR path and a LIST(VARCHAR) of paths.
// MarkdownReader::GetFiles() already resolves either shape; only the binder needs
// to be told the list form is legal (issue #30).
static void RegisterPathAndListVariants(ExtensionLoader &loader, TableFunction function, vector<string> param_names,
                                        string description, vector<string> examples) {
	TableFunctionSet function_set(function.name);

	CompatSetArgumentTypes(function, {LogicalType::VARCHAR});
	function_set.AddFunction(function);

	CompatSetArgumentTypes(function, {LogicalType::LIST(LogicalType::VARCHAR)});
	function_set.AddFunction(function);

	CreateTableFunctionInfo info(std::move(function_set));
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription desc;
	desc.parameter_names = std::move(param_names);
	desc.description = std::move(description);
	desc.examples = std::move(examples);
	desc.categories = {"markdown"};
	info.descriptions.push_back(desc);
	loader.RegisterFunction(std::move(info));
}

void MarkdownReader::RegisterFunction(ExtensionLoader &loader) {
	// Register read_markdown function
	TableFunction read_markdown_func("read_markdown", {LogicalType(LogicalTypeId::VARCHAR)},
	                                 MarkdownReadDocumentsFunction, MarkdownReadDocumentsBind);

	// Add named parameters
	// Named parameters. One call per function: v2.0 CREATES the kwargs group here.
	// EVERY ENTRY BELOW IS EXERCISED by test/sql/named_parameter_binding.test, which
	// asserts each declared name still binds (not what it does) plus a negative
	// control that an unknown name is still rejected.
	//
	// That file exists because an audit found 13 of these 31 declarations exercised
	// nowhere in the suite, five of them dating to the original template-transform
	// commit -- declared when the extension was written and never witnessed since.
	// A parameter nothing exercises is a public promise with no witness, and a port
	// that drops one entry ships green.
	//
	// It also makes the ORDER here safe to change. Before it, the only thing
	// protecting these was that `extract_metadata` happens to be declared first and
	// happens to be tested, so a whole-bag failure would surface -- a property held
	// by accident of ordering that a tidy-up reorder would have silently destroyed.
	// Add a parameter here, add a line there.
	CompatDeclareNamedParams(read_markdown_func,
	                         {
	                             {"extract_metadata", LogicalType(LogicalTypeId::BOOLEAN)},
	                             {"include_stats", LogicalType(LogicalTypeId::BOOLEAN)},
	                             {"normalize_content", LogicalType(LogicalTypeId::BOOLEAN)},
	                             {"maximum_file_size", LogicalType(LogicalTypeId::UBIGINT)},
	                             {"ignore_errors", LogicalType(LogicalTypeId::BOOLEAN)},
	                             {"extract_extensions", LogicalType(LogicalTypeId::VARCHAR)},
	                             {"flavor", LogicalType(LogicalTypeId::VARCHAR)},
	                             {"include_filepath", LogicalType(LogicalTypeId::BOOLEAN)},
	                             {"filename", LogicalType(LogicalTypeId::BOOLEAN)}, // Alias for include_filepath
	                             {"content_as_varchar", LogicalType(LogicalTypeId::BOOLEAN)},
	                         });

	RegisterPathAndListVariants(loader, read_markdown_func, {"path"},
	                            "Read Markdown documents from files into table format with frontmatter and content.",
	                            {"SELECT * FROM read_markdown('README.md')"});

	// Register read_markdown_sections function
	TableFunction read_sections_func("read_markdown_sections", {LogicalType(LogicalTypeId::VARCHAR)},
	                                 MarkdownReadSectionsFunction, MarkdownReadSectionsBind);

	// Add named parameters for sections
	// Named parameters for sections (content-mode options included).
	CompatDeclareNamedParams(read_sections_func,
	                         {
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
	                             {"filename", LogicalType(LogicalTypeId::BOOLEAN)}, // Alias for include_filepath
	                             {"content_as_varchar", LogicalType(LogicalTypeId::BOOLEAN)},
	                             {"content_mode", LogicalType(LogicalTypeId::VARCHAR)},
	                             {"max_depth", LogicalType(LogicalTypeId::INTEGER)},
	                             {"max_content_length", LogicalType(LogicalTypeId::UBIGINT)},
	                         });

	RegisterPathAndListVariants(loader, read_sections_func, {"path"},
	                            "Read Markdown files split by headings into structured sections.",
	                            {"SELECT * FROM read_markdown_sections('README.md')"});

	// Register read_markdown_blocks function
	TableFunction read_blocks_func("read_markdown_blocks", {LogicalType(LogicalTypeId::VARCHAR)},
	                               MarkdownReadBlocksFunction, MarkdownReadBlocksBind);

	// Add named parameters for blocks
	// Named parameters for blocks.
	CompatDeclareNamedParams(read_blocks_func,
	                         {
	                             {"extract_metadata", LogicalType(LogicalTypeId::BOOLEAN)},
	                             {"normalize_content", LogicalType(LogicalTypeId::BOOLEAN)},
	                             {"maximum_file_size", LogicalType(LogicalTypeId::UBIGINT)},
	                             {"ignore_errors", LogicalType(LogicalTypeId::BOOLEAN)},
	                             {"extract_extensions", LogicalType(LogicalTypeId::VARCHAR)},
	                             {"include_filepath", LogicalType(LogicalTypeId::BOOLEAN)},
	                             {"filename", LogicalType(LogicalTypeId::BOOLEAN)}, // Alias for include_filepath
	                         });

	RegisterPathAndListVariants(loader, read_blocks_func, {"path"},
	                            "Read Markdown files parsed into atomic block elements.",
	                            {"SELECT * FROM read_markdown_blocks('README.md')"});
}

} // namespace duckdb
