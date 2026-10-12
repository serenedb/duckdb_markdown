#include "markdown_extraction_functions.hpp"
#include "markdown_types.hpp"
#include "markdown_utils.hpp"
#include "markdown_vector_writer.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Code Block Extraction - Scalar Function
//===--------------------------------------------------------------------===//

static void CodeBlockExtractionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	markdown_writer::ExecuteListExtraction(
	    args, result,
	    [](const std::string &markdown, idx_t) {
		    return std::optional(markdown_utils::ExtractCodeBlocks(markdown, ""));
	    },
	    [](Vector &block, idx_t row, const markdown_utils::CodeBlock &code_block) {
		    markdown_writer::WriteString(CompatStructEntry(block, 0), row, code_block.language);
		    markdown_writer::WriteString(CompatStructEntry(block, 1), row, code_block.code);
		    markdown_writer::WriteNumber(CompatStructEntry(block, 2), row,
		                                 static_cast<int64_t>(code_block.line_number));
		    markdown_writer::WriteString(CompatStructEntry(block, 3), row, code_block.info_string);
	    });
}

//===--------------------------------------------------------------------===//
// Link Extraction - Scalar Function
//===--------------------------------------------------------------------===//

static void LinkExtractionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	markdown_writer::ExecuteListExtraction(
	    args, result,
	    [](const std::string &markdown, idx_t) { return std::optional(markdown_utils::ExtractLinks(markdown)); },
	    [](Vector &entry, idx_t row, const markdown_utils::MarkdownLink &link) {
		    markdown_writer::WriteString(CompatStructEntry(entry, 0), row, link.text);
		    markdown_writer::WriteString(CompatStructEntry(entry, 1), row, link.url);
		    markdown_writer::WriteOptionalString(CompatStructEntry(entry, 2), row, link.title);
		    markdown_writer::WriteNumber(CompatStructEntry(entry, 3), row, link.is_reference);
		    markdown_writer::WriteNumber(CompatStructEntry(entry, 4), row, static_cast<int64_t>(link.line_number));
	    });
}

//===--------------------------------------------------------------------===//
// Image Extraction - Scalar Function
//===--------------------------------------------------------------------===//

static void ImageExtractionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	markdown_writer::ExecuteListExtraction(
	    args, result,
	    [](const std::string &markdown, idx_t) { return std::optional(markdown_utils::ExtractImages(markdown)); },
	    [](Vector &entry, idx_t row, const markdown_utils::MarkdownImage &image) {
		    markdown_writer::WriteString(CompatStructEntry(entry, 0), row, image.alt_text);
		    markdown_writer::WriteString(CompatStructEntry(entry, 1), row, image.url);
		    markdown_writer::WriteOptionalString(CompatStructEntry(entry, 2), row, image.title);
		    markdown_writer::WriteNumber(CompatStructEntry(entry, 3), row, static_cast<int64_t>(image.line_number));
	    });
}

//===--------------------------------------------------------------------===//
// Wikilink Extraction - Scalar Function (Obsidian / wiki-style [[...]], ![[...]])
//===--------------------------------------------------------------------===//

static void WikilinkExtractionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	markdown_writer::ExecuteListExtraction(
	    args, result,
	    [](const std::string &markdown, idx_t) { return std::optional(markdown_utils::ExtractWikilinks(markdown)); },
	    markdown_writer::WriteWikilink);
}

//===--------------------------------------------------------------------===//
// Tag Extraction - Scalar Function (inline #tags)
//===--------------------------------------------------------------------===//

static void TagExtractionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	markdown_writer::ExecuteListExtraction(
	    args, result,
	    [](const std::string &markdown, idx_t) { return std::optional(markdown_utils::ExtractTags(markdown)); },
	    markdown_writer::WriteTag);
}

//===--------------------------------------------------------------------===//
// Table Row Extraction - Scalar Function (renamed from md_extract_tables)
//===--------------------------------------------------------------------===//

struct TableCell {
	idx_t table_index;
	bool header;
	idx_t row_index;
	idx_t column_index;
	const std::string *value;
	const markdown_utils::MarkdownTable *table;
};

static void TableRowExtractionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	vector<markdown_utils::MarkdownTable> tables;
	markdown_writer::ExecuteListExtraction(
	    args, result,
	    [&](const std::string &markdown, idx_t) {
		    tables = markdown_utils::ExtractTables(markdown);
		    vector<TableCell> cells;
		    for (idx_t table_idx = 0; table_idx < tables.size(); table_idx++) {
			    const auto &table = tables[table_idx];
			    for (idx_t col_idx = 0; col_idx < table.headers.size(); col_idx++) {
				    cells.push_back({table_idx, true, 0, col_idx, &table.headers[col_idx], &table});
			    }
			    for (idx_t row_idx = 0; row_idx < table.rows.size(); row_idx++) {
				    const auto &row = table.rows[row_idx];
				    for (idx_t col_idx = 0; col_idx < row.size(); col_idx++) {
					    cells.push_back({table_idx, false, row_idx + 1, col_idx, &row[col_idx], &table});
				    }
			    }
		    }
		    return std::optional(std::move(cells));
	    },
	    [](Vector &entry, idx_t row, const TableCell &cell) {
		    static const std::string header_type = "header";
		    static const std::string data_type = "data";
		    markdown_writer::WriteNumber(CompatStructEntry(entry, 0), row, static_cast<int64_t>(cell.table_index));
		    markdown_writer::WriteString(CompatStructEntry(entry, 1), row, cell.header ? header_type : data_type);
		    markdown_writer::WriteNumber(CompatStructEntry(entry, 2), row, static_cast<int64_t>(cell.row_index));
		    markdown_writer::WriteNumber(CompatStructEntry(entry, 3), row, static_cast<int64_t>(cell.column_index));
		    markdown_writer::WriteString(CompatStructEntry(entry, 4), row, *cell.value);
		    markdown_writer::WriteNumber(CompatStructEntry(entry, 5), row,
		                                 static_cast<int64_t>(cell.table->line_number));
		    markdown_writer::WriteNumber(CompatStructEntry(entry, 6), row,
		                                 static_cast<int64_t>(cell.table->num_columns));
		    markdown_writer::WriteNumber(CompatStructEntry(entry, 7), row, static_cast<int64_t>(cell.table->num_rows));
	    });
}

//===--------------------------------------------------------------------===//
// Section Extraction - Scalar Function
//===--------------------------------------------------------------------===//

static void WriteSection(Vector &entry, idx_t row, const markdown_utils::MarkdownSection &section) {
	markdown_writer::WriteString(CompatStructEntry(entry, 0), row, section.id);
	markdown_writer::WriteString(CompatStructEntry(entry, 1), row, section.section_path);
	markdown_writer::WriteNumber(CompatStructEntry(entry, 2), row, static_cast<int32_t>(section.level));
	markdown_writer::WriteString(CompatStructEntry(entry, 3), row, section.title);
	markdown_writer::WriteString(CompatStructEntry(entry, 4), row, section.content);
	markdown_writer::WriteOptionalString(CompatStructEntry(entry, 5), row, section.parent_id);
	markdown_writer::WriteNumber(CompatStructEntry(entry, 6), row, static_cast<int64_t>(section.start_line));
	markdown_writer::WriteNumber(CompatStructEntry(entry, 7), row, static_cast<int64_t>(section.end_line));
}

static void SectionExtractionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	markdown_writer::ExecuteListExtraction(
	    args, result,
	    [](const std::string &markdown, idx_t) {
		    return std::optional(markdown_utils::ExtractSections(markdown, 1, 6, true));
	    },
	    WriteSection);
}

static void SectionExtractionFunctionWithLevels(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto count = args.size();
	UnifiedVectorFormat min_levels;
	UnifiedVectorFormat max_levels;
	CompatToUnifiedFormat(args.data[1], count, min_levels);
	CompatToUnifiedFormat(args.data[2], count, max_levels);
	markdown_writer::ExecuteListExtraction(
	    args, result,
	    [&](const std::string &markdown, idx_t row) -> std::optional<vector<markdown_utils::MarkdownSection>> {
		    const auto min_idx = min_levels.sel->get_index(row);
		    const auto max_idx = max_levels.sel->get_index(row);
		    if (!min_levels.validity.RowIsValid(min_idx) || !max_levels.validity.RowIsValid(max_idx)) {
			    return std::nullopt;
		    }
		    return markdown_utils::ExtractSections(markdown, UnifiedVectorFormat::GetData<int32_t>(min_levels)[min_idx],
		                                           UnifiedVectorFormat::GetData<int32_t>(max_levels)[max_idx], true);
	    },
	    WriteSection);
}

static void SectionExtractionFunctionWithContentMode(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto count = args.size();
	UnifiedVectorFormat min_levels;
	UnifiedVectorFormat max_levels;
	UnifiedVectorFormat content_modes;
	CompatToUnifiedFormat(args.data[1], count, min_levels);
	CompatToUnifiedFormat(args.data[2], count, max_levels);
	CompatToUnifiedFormat(args.data[3], count, content_modes);
	markdown_writer::ExecuteListExtraction(
	    args, result,
	    [&](const std::string &markdown, idx_t row) -> std::optional<vector<markdown_utils::MarkdownSection>> {
		    const auto min_idx = min_levels.sel->get_index(row);
		    const auto max_idx = max_levels.sel->get_index(row);
		    if (!min_levels.validity.RowIsValid(min_idx) || !max_levels.validity.RowIsValid(max_idx)) {
			    return std::nullopt;
		    }
		    std::string content_mode = "minimal";
		    const auto mode_idx = content_modes.sel->get_index(row);
		    if (content_modes.validity.RowIsValid(mode_idx)) {
			    content_mode = UnifiedVectorFormat::GetData<string_t>(content_modes)[mode_idx].GetString();
		    }
		    return markdown_utils::ExtractSections(markdown, UnifiedVectorFormat::GetData<int32_t>(min_levels)[min_idx],
		                                           UnifiedVectorFormat::GetData<int32_t>(max_levels)[max_idx], true,
		                                           content_mode);
	    },
	    WriteSection);
}

//===--------------------------------------------------------------------===//
// Table JSON Extraction - Scalar Function
//===--------------------------------------------------------------------===//

static void TableJSONExtractionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	vector<markdown_utils::MarkdownTable> tables;
	markdown_writer::ExecuteListExtraction(
	    args, result,
	    [&](const std::string &markdown, idx_t) {
		    tables = markdown_utils::ExtractTables(markdown);
		    vector<idx_t> indexes(tables.size());
		    for (idx_t table_idx = 0; table_idx < tables.size(); table_idx++) {
			    indexes[table_idx] = table_idx;
		    }
		    return std::optional(std::move(indexes));
	    },
	    [&](Vector &entry, idx_t row, idx_t table_idx) {
		    const auto &table = tables[table_idx];
		    markdown_writer::WriteNumber(CompatStructEntry(entry, 0), row, static_cast<int64_t>(table_idx));
		    markdown_writer::WriteNumber(CompatStructEntry(entry, 1), row, static_cast<int64_t>(table.line_number));
		    markdown_writer::WriteNumber(CompatStructEntry(entry, 2), row, static_cast<int64_t>(table.headers.size()));
		    markdown_writer::WriteNumber(CompatStructEntry(entry, 3), row, static_cast<int64_t>(table.rows.size()));
		    markdown_writer::WriteStringList(CompatStructEntry(entry, 4), row, table.headers);
		    auto &table_data = CompatStructEntry(entry, 5);
		    const auto offset = markdown_writer::WriteList(table_data, row, table.rows.size());
		    auto &cells = CompatListChildMutable(table_data);
		    for (idx_t row_idx = 0; row_idx < table.rows.size(); row_idx++) {
			    markdown_writer::WriteStringList(cells, offset + row_idx, table.rows[row_idx]);
		    }
	    });
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

void MarkdownExtractionFunctions::Register(ExtensionLoader &loader) {
	// Define return types for scalar functions
	auto code_block_struct_type = LogicalType::STRUCT({{"language", LogicalType(LogicalTypeId::VARCHAR)},
	                                                   {"code", LogicalType(LogicalTypeId::VARCHAR)},
	                                                   {"line_number", LogicalType(LogicalTypeId::BIGINT)},
	                                                   {"info_string", LogicalType(LogicalTypeId::VARCHAR)}});

	auto link_struct_type = LogicalType::STRUCT({{"text", LogicalType(LogicalTypeId::VARCHAR)},
	                                             {"url", LogicalType(LogicalTypeId::VARCHAR)},
	                                             {"title", LogicalType(LogicalTypeId::VARCHAR)},
	                                             {"is_reference", LogicalType(LogicalTypeId::BOOLEAN)},
	                                             {"line_number", LogicalType(LogicalTypeId::BIGINT)}});

	auto image_struct_type = LogicalType::STRUCT({{"alt_text", LogicalType(LogicalTypeId::VARCHAR)},
	                                              {"url", LogicalType(LogicalTypeId::VARCHAR)},
	                                              {"title", LogicalType(LogicalTypeId::VARCHAR)},
	                                              {"line_number", LogicalType(LogicalTypeId::BIGINT)}});

	auto wikilink_struct_type = LogicalType::STRUCT({{"target", LogicalType(LogicalTypeId::VARCHAR)},
	                                                 {"alias", LogicalType(LogicalTypeId::VARCHAR)},
	                                                 {"anchor", LogicalType(LogicalTypeId::VARCHAR)},
	                                                 {"is_embed", LogicalType(LogicalTypeId::BOOLEAN)},
	                                                 {"line_number", LogicalType(LogicalTypeId::BIGINT)}});

	auto tag_struct_type = LogicalType::STRUCT(
	    {{"tag", LogicalType(LogicalTypeId::VARCHAR)}, {"line_number", LogicalType(LogicalTypeId::BIGINT)}});

	auto table_row_struct_type = LogicalType::STRUCT({{"table_index", LogicalType(LogicalTypeId::BIGINT)},
	                                                  {"row_type", LogicalType(LogicalTypeId::VARCHAR)},
	                                                  {"row_index", LogicalType(LogicalTypeId::BIGINT)},
	                                                  {"column_index", LogicalType(LogicalTypeId::BIGINT)},
	                                                  {"cell_value", LogicalType(LogicalTypeId::VARCHAR)},
	                                                  {"line_number", LogicalType(LogicalTypeId::BIGINT)},
	                                                  {"num_columns", LogicalType(LogicalTypeId::BIGINT)},
	                                                  {"num_rows", LogicalType(LogicalTypeId::BIGINT)}});

	auto table_json_struct_type = LogicalType::STRUCT(
	    {{"table_index", LogicalType(LogicalTypeId::BIGINT)},
	     {"line_number", LogicalType(LogicalTypeId::BIGINT)},
	     {"num_columns", LogicalType(LogicalTypeId::BIGINT)},
	     {"num_rows", LogicalType(LogicalTypeId::BIGINT)},
	     {"headers", LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR))},
	     {"table_data", LogicalType::LIST(LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR)))}});

	// Register md_extract_code_blocks scalar function
	ScalarFunction code_blocks_func("md_extract_code_blocks", {MarkdownTypes::MarkdownType()},
	                                LogicalType::LIST(code_block_struct_type), CodeBlockExtractionFunction);
	{
		CreateScalarFunctionInfo info(std::move(code_blocks_func));
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		FunctionDescription desc;
		desc.parameter_names = {"markdown"};
		desc.description = "Extract fenced and indented code blocks from Markdown content.";
		desc.examples = {"md_extract_code_blocks('```sql\nSELECT 1;\n```')"};
		desc.categories = {"markdown"};
		info.descriptions.push_back(desc);
		loader.RegisterFunction(std::move(info));
	}

	// Register md_extract_links scalar function
	ScalarFunction links_func("md_extract_links", {MarkdownTypes::MarkdownType()}, LogicalType::LIST(link_struct_type),
	                          LinkExtractionFunction);
	{
		CreateScalarFunctionInfo info(std::move(links_func));
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		FunctionDescription desc;
		desc.parameter_names = {"markdown"};
		desc.description = "Extract hyperlinks from Markdown content.";
		desc.examples = {"md_extract_links('[DuckDB](https://duckdb.org)')"};
		desc.categories = {"markdown"};
		info.descriptions.push_back(desc);
		loader.RegisterFunction(std::move(info));
	}

	// Register md_extract_images scalar function
	ScalarFunction images_func("md_extract_images", {MarkdownTypes::MarkdownType()},
	                           LogicalType::LIST(image_struct_type), ImageExtractionFunction);
	{
		CreateScalarFunctionInfo info(std::move(images_func));
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		FunctionDescription desc;
		desc.parameter_names = {"markdown"};
		desc.description = "Extract images from Markdown content.";
		desc.examples = {"md_extract_images('![Logo](logo.png)')"};
		desc.categories = {"markdown"};
		info.descriptions.push_back(desc);
		loader.RegisterFunction(std::move(info));
	}

	// Register md_extract_wikilinks scalar function (Obsidian / wiki-style links + embeds)
	ScalarFunction wikilinks_func("md_extract_wikilinks", {MarkdownTypes::MarkdownType()},
	                              LogicalType::LIST(wikilink_struct_type), WikilinkExtractionFunction);
	{
		CreateScalarFunctionInfo info(std::move(wikilinks_func));
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		FunctionDescription desc;
		desc.parameter_names = {"markdown"};
		desc.description = "Extract wiki-style links and embeds from Markdown content.";
		desc.examples = {"md_extract_wikilinks('[[Page Name]]')"};
		desc.categories = {"markdown"};
		info.descriptions.push_back(desc);
		loader.RegisterFunction(std::move(info));
	}

	// Register md_extract_tags scalar function (inline #tags)
	ScalarFunction tags_func("md_extract_tags", {MarkdownTypes::MarkdownType()}, LogicalType::LIST(tag_struct_type),
	                         TagExtractionFunction);
	{
		CreateScalarFunctionInfo info(std::move(tags_func));
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		FunctionDescription desc;
		desc.parameter_names = {"markdown"};
		desc.description = "Extract hashtag tags from Markdown content.";
		desc.examples = {"md_extract_tags('Tag #important text')"};
		desc.categories = {"markdown"};
		info.descriptions.push_back(desc);
		loader.RegisterFunction(std::move(info));
	}

	// Register md_extract_table_rows scalar function (renamed from md_extract_tables)
	ScalarFunction table_rows_func("md_extract_table_rows", {MarkdownTypes::MarkdownType()},
	                               LogicalType::LIST(table_row_struct_type), TableRowExtractionFunction);

	// Register md_extract_tables_json scalar function
	ScalarFunction tables_json_func("md_extract_tables_json", {MarkdownTypes::MarkdownType()},
	                                LogicalType::LIST(table_json_struct_type), TableJSONExtractionFunction);

	// Both extract through ExtractTables, which calls GetInlineText on every cell,
	// and GetInlineText throws InvalidInputException past MAX_INLINE_DEPTH. On
	// v2.0 an unmarked scalar function that throws an execution error has the
	// throw rewritten into an InternalException, which defeats the depth cap's
	// readable message (measured on v2.0-cyanoptera). The other extractors here
	// reach no such throw.
	//
	// As with md_to_html/md_to_text, this is not inert on v1.5: it makes the
	// planner more conservative around these two functions.
	// See test/sql/markdown_table_extraction_fallible.test.
	table_rows_func.SetFallible();
	tables_json_func.SetFallible();

	{
		CreateScalarFunctionInfo info(std::move(table_rows_func));
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		FunctionDescription desc;
		desc.parameter_names = {"markdown"};
		desc.description = "Extract table cell rows from Markdown content.";
		desc.examples = {"md_extract_table_rows('| a | b |\n|---|---|\n| 1 | 2 |')"};
		desc.categories = {"markdown"};
		info.descriptions.push_back(desc);
		loader.RegisterFunction(std::move(info));
	}
	{
		CreateScalarFunctionInfo info(std::move(tables_json_func));
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		FunctionDescription desc;
		desc.parameter_names = {"markdown"};
		desc.description = "Extract tables from Markdown content as structured objects.";
		desc.examples = {"md_extract_tables_json('| a | b |\n|---|---|\n| 1 | 2 |')"};
		desc.categories = {"markdown"};
		info.descriptions.push_back(desc);
		loader.RegisterFunction(std::move(info));
	}

	// Register md_extract_sections scalar function
	LogicalType section_struct_type = LogicalType::STRUCT({{"section_id", LogicalType::VARCHAR},
	                                                       {"section_path", LogicalType::VARCHAR},
	                                                       {"level", LogicalType::INTEGER},
	                                                       {"title", LogicalType::VARCHAR},
	                                                       {"content", MarkdownTypes::MarkdownType()},
	                                                       {"parent_id", LogicalType::VARCHAR},
	                                                       {"start_line", LogicalType::BIGINT},
	                                                       {"end_line", LogicalType::BIGINT}});

	// Register main function with MARKDOWN type
	ScalarFunction sections_func("md_extract_sections", {MarkdownTypes::MarkdownType()},
	                             LogicalType::LIST(section_struct_type), SectionExtractionFunction);

	// Register overload for VARCHAR with level filtering
	ScalarFunction sections_levels_func("md_extract_sections",
	                                    {LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::INTEGER},
	                                    LogicalType::LIST(section_struct_type), SectionExtractionFunctionWithLevels);

	// Register overload for VARCHAR with level filtering and content_mode
	ScalarFunction sections_content_mode_func(
	    "md_extract_sections", {LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::VARCHAR},
	    LogicalType::LIST(section_struct_type), SectionExtractionFunctionWithContentMode);

	ScalarFunctionSet sections_set("md_extract_sections");
	sections_set.AddFunction(sections_func);
	sections_set.AddFunction(sections_levels_func);
	sections_set.AddFunction(sections_content_mode_func);

	CreateScalarFunctionInfo sec_info(std::move(sections_set));
	sec_info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription sec_desc1;
	sec_desc1.parameter_names = {"markdown"};
	sec_desc1.description = "Extract all sections from Markdown content.";
	sec_desc1.examples = {"md_extract_sections('# Header\nContent')"};
	sec_desc1.categories = {"markdown"};
	sec_info.descriptions.push_back(sec_desc1);

	FunctionDescription sec_desc2;
	sec_desc2.parameter_names = {"markdown", "min_level", "max_level"};
	sec_desc2.description = "Extract sections within a heading level range from Markdown content.";
	sec_desc2.examples = {"md_extract_sections('# Header\n## Sub\nContent', 1, 2)"};
	sec_desc2.categories = {"markdown"};
	sec_info.descriptions.push_back(sec_desc2);

	FunctionDescription sec_desc3;
	sec_desc3.parameter_names = {"markdown", "min_level", "max_level", "content_mode"};
	sec_desc3.description = "Extract sections with specified level range and content mode.";
	sec_desc3.examples = {"md_extract_sections('# Header\n## Sub\nContent', 1, 2, 'minimal')"};
	sec_desc3.categories = {"markdown"};
	sec_info.descriptions.push_back(sec_desc3);

	loader.RegisterFunction(std::move(sec_info));
}

} // namespace duckdb
