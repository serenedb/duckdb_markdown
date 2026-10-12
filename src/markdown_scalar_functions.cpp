#include "markdown_scalar_functions.hpp"
#include "duckdb_compat.hpp"
#include "duckdb/main/client_context.hpp"
#include "markdown_types.hpp"
#include "markdown_utils.hpp"
#include "markdown_vector_writer.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/common/error_data.hpp"

namespace duckdb {

namespace {

[[noreturn]] void ThrowMarkdownError(const char *action, const std::exception &e) {
	throw InvalidInputException("Error %s: %s", action, ErrorData(e).RawMessage());
}

} // namespace

void MarkdownFunctions::Register(ExtensionLoader &loader) {
	RegisterValidationFunction(loader);
	RegisterConversionFunctions(loader);
	RegisterMarkdownTypeFunctions(loader);
	RegisterStatsFunctions(loader);
	RegisterMetadataFunctions(loader);
}

void MarkdownFunctions::RegisterValidationFunction(ExtensionLoader &loader) {
	// md_valid function - validates markdown content
	// Only register VARCHAR version since md type is a VARCHAR alias and will auto-cast
	ScalarFunction md_valid_fun("md_valid", {LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                            [](DataChunk &args, ExpressionState &state, Vector &result) {
		                            auto &input_vector = args.data[0];

		                            // Was UnaryExecutor::ExecuteWithNulls, which DuckDB v2.0
		                            // removed. A plain Execute is the exact equivalent here,
		                            // not an approximation: ExecuteWithNulls copies the input
		                            // validity into the result mask and then SKIPS the lambda
		                            // for null rows, so the old lambda's `if (!RowIsValid)
		                            // return false` branch was unreachable and a NULL input
		                            // has always produced NULL. Execute propagates nulls the
		                            // same way, so this preserves that exactly.
		                            //
		                            // Do not "simplify" this into a loop that writes false for
		                            // null inputs -- that silently turns NULL into FALSE for
		                            // non-constant inputs, which is the regression this
		                            // comment exists to prevent.
		                            UnaryExecutor::Execute<string_t, bool>(input_vector, result, args.size(),
		                                                                   [](string_t md_str) {
			                                                                   try {
				                                                                   return !md_str.GetString().empty();
			                                                                   } catch (...) {
				                                                                   return false;
			                                                                   }
		                                                                   });
	                            });

	CreateScalarFunctionInfo info(std::move(md_valid_fun));
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription desc;
	desc.parameter_names = {"markdown"};
	desc.description = "Validate markdown content.";
	desc.examples = {"md_valid('# Hello')"};
	desc.categories = {"markdown"};
	info.descriptions.push_back(desc);
	loader.RegisterFunction(std::move(info));
}

void MarkdownFunctions::RegisterConversionFunctions(ExtensionLoader &loader) {
	const auto markdown_type = MarkdownTypes::MarkdownType();

	// md_to_html function
	ScalarFunction md_to_html_fun(
	    "md_to_html", {markdown_type}, LogicalType::VARCHAR,
	    [](DataChunk &args, ExpressionState &state, Vector &result) {
		    UnaryExecutor::Execute<string_t, string_t>(
		        args.data[0], result, args.size(), [&](string_t md_str) -> string_t {
			        if (md_str.GetSize() == 0) {
				        return string_t();
			        }

			        try {
				        const std::string html_str = markdown_utils::MarkdownToHTML(md_str.GetString());
				        return StringVector::AddString(result, html_str.c_str(), html_str.length());
			        } catch (const std::exception &e) {
				        ThrowMarkdownError("converting Markdown to HTML", e);
			        }
		        });
	    });

	// md_to_text function (for FTS)
	ScalarFunction md_to_text_fun(
	    "md_to_text", {markdown_type}, LogicalType::VARCHAR,
	    [](DataChunk &args, ExpressionState &state, Vector &result) {
		    UnaryExecutor::Execute<string_t, string_t>(
		        args.data[0], result, args.size(), [&](string_t md_str) -> string_t {
			        if (md_str.GetSize() == 0) {
				        return string_t();
			        }

			        try {
				        const std::string text_str = markdown_utils::MarkdownToText(md_str.GetString());
				        return StringVector::AddString(result, text_str.c_str(), text_str.length());
			        } catch (const std::exception &e) {
				        ThrowMarkdownError("converting Markdown to text", e);
			        }
		        });
	    });

	// Both convert through a third-party renderer and rethrow its failures as
	// InvalidInputException. v2.0 requires a scalar function that can throw at
	// execution time to declare it, or the throw becomes an InternalException
	// complaining the function is not marked fallible.
	//
	// Called directly, NOT through a compat shim: SetFallible() is identical on
	// v1.5 (function.hpp:211) and on main, so a probe would take the same branch
	// on both -- feature detection that detects nothing.
	//
	// It is NOT inert on v1.5 either. `errors` feeds BoundFunctionExpression::
	// CanThrow(), which gates conjunct reordering (expression_heuristics,
	// adaptive_filter), filter pushdown (pushdown_get/_projection/_outer_join)
	// and dictionary-expression caching (execute_function.cpp). Declaring it
	// makes the shipped planner strictly more conservative around these two
	// functions -- safe in direction, but a real change, and measured rather
	// than assumed (see test/sql/markdown_fallible_planner.test).
	md_to_html_fun.SetFallible();
	md_to_text_fun.SetFallible();

	{
		CreateScalarFunctionInfo info(std::move(md_to_html_fun));
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		FunctionDescription desc;
		desc.parameter_names = {"markdown"};
		desc.description = "Convert Markdown text to HTML.";
		desc.examples = {"md_to_html('# Title')"};
		desc.categories = {"markdown"};
		info.descriptions.push_back(desc);
		loader.RegisterFunction(std::move(info));
	}
	{
		CreateScalarFunctionInfo info(std::move(md_to_text_fun));
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		FunctionDescription desc;
		desc.parameter_names = {"markdown"};
		desc.description = "Convert Markdown text to plain text.";
		desc.examples = {"md_to_text('# Title')"};
		desc.categories = {"markdown"};
		info.descriptions.push_back(desc);
		loader.RegisterFunction(std::move(info));
	}
}

void MarkdownFunctions::RegisterMarkdownTypeFunctions(ExtensionLoader &loader) {
	auto markdown_type = MarkdownTypes::MarkdownType();

	// value_to_md function (convert any value to Markdown)
	ScalarFunction value_to_md_fun(
	    "value_to_md", {LogicalType::ANY}, markdown_type, [](DataChunk &args, ExpressionState &state, Vector &result) {
		    const auto count = args.size();
		    Vector text(LogicalType::VARCHAR, count);
		    VectorOperations::DefaultCast(args.data[0], text, count);
		    UnifiedVectorFormat texts;
		    CompatToUnifiedFormat(text, count, texts);
		    const auto data = UnifiedVectorFormat::GetData<string_t>(texts);
		    result.SetVectorType(VectorType::FLAT_VECTOR);
		    auto result_data = CompatFlatDataMutable<string_t>(result);
		    for (idx_t row = 0; row < count; row++) {
			    const auto idx = texts.sel->get_index(row);
			    result_data[row] =
			        texts.validity.RowIsValid(idx) ? StringVector::AddString(result, data[idx]) : string_t();
		    }
	    });

	CreateScalarFunctionInfo info(std::move(value_to_md_fun));
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription desc;
	desc.parameter_names = {"val"};
	desc.description = "Convert any value to a Markdown formatted string.";
	desc.examples = {"value_to_md(42)"};
	desc.categories = {"markdown"};
	info.descriptions.push_back(desc);
	loader.RegisterFunction(std::move(info));
}

namespace {

// `has_exact_arg` says whether this is the two-argument overload. The flag is
// read per row, so a non-constant expression works. Both overloads use DuckDB's
// default null handling, so a NULL in either argument yields a NULL struct
// without the function running at all; the IsNull guard below is belt and
// braces for the paths that do not fold.
void ExecuteStats(DataChunk &args, Vector &result, bool has_exact_arg) {
	const auto count = args.size();
	UnifiedVectorFormat markdowns;
	UnifiedVectorFormat exacts;
	CompatToUnifiedFormat(args.data[0], count, markdowns);
	if (has_exact_arg) {
		CompatToUnifiedFormat(args.data[1], count, exacts);
	}
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t row = 0; row < count; row++) {
		try {
			const auto idx = markdowns.sel->get_index(row);
			if (!markdowns.validity.RowIsValid(idx)) {
				FlatVector::SetNull(result, row, true);
				continue;
			}

			bool exact = false;
			if (has_exact_arg) {
				const auto exact_idx = exacts.sel->get_index(row);
				exact = exacts.validity.RowIsValid(exact_idx) && UnifiedVectorFormat::GetData<bool>(exacts)[exact_idx];
			}

			const auto md_str = UnifiedVectorFormat::GetData<string_t>(markdowns)[idx].GetString();
			markdown_utils::MarkdownStats stats {};
			if (!md_str.empty()) {
				stats = markdown_utils::CalculateStats(md_str, exact);
			}
			markdown_writer::WriteStats(result, row, stats);
		} catch (const std::exception &e) {
			ThrowMarkdownError("computing Markdown stats", e);
		}
	}
}

} // namespace

void MarkdownFunctions::RegisterStatsFunctions(ExtensionLoader &loader) {
	auto markdown_type = MarkdownTypes::MarkdownType();

	// md_stats function - returns a struct with document statistics
	child_list_t<LogicalType> stats_struct_types;
	stats_struct_types.push_back(std::make_pair("word_count", LogicalType::BIGINT));
	stats_struct_types.push_back(std::make_pair("char_count", LogicalType::BIGINT));
	stats_struct_types.push_back(std::make_pair("line_count", LogicalType::BIGINT));
	stats_struct_types.push_back(std::make_pair("heading_count", LogicalType::BIGINT));
	stats_struct_types.push_back(std::make_pair("code_block_count", LogicalType::BIGINT));
	stats_struct_types.push_back(std::make_pair("link_count", LogicalType::BIGINT));
	stats_struct_types.push_back(std::make_pair("reading_time_minutes", LogicalType::DOUBLE));

	auto stats_struct_type = LogicalType::STRUCT(stats_struct_types);

	ScalarFunction md_stats_fun(
	    "md_stats", {markdown_type}, stats_struct_type,
	    [](DataChunk &args, ExpressionState &state, Vector &result) { ExecuteStats(args, result, false); });

	ScalarFunction md_stats_exact_fun(
	    "md_stats", {markdown_type, LogicalType::BOOLEAN}, stats_struct_type,
	    [](DataChunk &args, ExpressionState &state, Vector &result) { ExecuteStats(args, result, true); });

	md_stats_fun.SetFallible();
	md_stats_exact_fun.SetFallible();

	ScalarFunctionSet stats_set("md_stats");
	stats_set.AddFunction(md_stats_fun);
	stats_set.AddFunction(md_stats_exact_fun);

	CreateScalarFunctionInfo stats_info(std::move(stats_set));
	stats_info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription stats_desc1;
	stats_desc1.parameter_names = {"markdown"};
	stats_desc1.description = "Calculate document statistics for Markdown content.";
	stats_desc1.examples = {"md_stats('# Heading\nText')"};
	stats_desc1.categories = {"markdown"};
	stats_info.descriptions.push_back(stats_desc1);

	FunctionDescription stats_desc2;
	stats_desc2.parameter_names = {"markdown", "exact"};
	stats_desc2.description = "Calculate document statistics for Markdown content with exact counts.";
	stats_desc2.examples = {"md_stats('# Heading\nText', true)"};
	stats_desc2.categories = {"markdown"};
	stats_info.descriptions.push_back(stats_desc2);
	loader.RegisterFunction(std::move(stats_info));

	// Register md_extract_section function (2-arg version: uses minimal mode)
	ScalarFunction md_extract_section(
	    "md_extract_section", {markdown_type, LogicalType::VARCHAR}, markdown_type,
	    [](DataChunk &args, ExpressionState &state, Vector &result) {
		    auto &markdown_vector = args.data[0];
		    auto &section_id_vector = args.data[1];

		    BinaryExecutor::Execute<string_t, string_t, string_t>(
		        markdown_vector, section_id_vector, result, args.size(),
		        [&](string_t markdown_str, string_t section_id_str) -> string_t {
			        if (markdown_str.GetSize() == 0 || section_id_str.GetSize() == 0) {
				        return string_t();
			        }

			        try {
				        const std::string section_content =
				            markdown_utils::ExtractSection(markdown_str.GetString(), section_id_str.GetString(), false);
				        return StringVector::AddString(result, section_content);
			        } catch (const std::exception &e) {
				        ThrowMarkdownError("extracting a Markdown section", e);
			        }
		        });
	    });

	// Register md_extract_section overload with include_subsections parameter
	// include_subsections=true uses 'full' mode, false uses 'minimal' mode
	ScalarFunction md_extract_section_with_subsections(
	    "md_extract_section", {markdown_type, LogicalType::VARCHAR, LogicalType::BOOLEAN}, markdown_type,
	    [](DataChunk &args, ExpressionState &state, Vector &result) {
		    const auto count = args.size();
		    UnifiedVectorFormat markdowns;
		    UnifiedVectorFormat section_ids;
		    UnifiedVectorFormat include_subsections;
		    CompatToUnifiedFormat(args.data[0], count, markdowns);
		    CompatToUnifiedFormat(args.data[1], count, section_ids);
		    CompatToUnifiedFormat(args.data[2], count, include_subsections);
		    result.SetVectorType(VectorType::FLAT_VECTOR);

		    for (idx_t row = 0; row < count; row++) {
			    const auto markdown_idx = markdowns.sel->get_index(row);
			    const auto section_idx = section_ids.sel->get_index(row);
			    if (!markdowns.validity.RowIsValid(markdown_idx) || !section_ids.validity.RowIsValid(section_idx)) {
				    markdown_writer::WriteNull(result, row);
				    continue;
			    }

			    const auto markdown_str = UnifiedVectorFormat::GetData<string_t>(markdowns)[markdown_idx].GetString();
			    const auto section_id_str =
			        UnifiedVectorFormat::GetData<string_t>(section_ids)[section_idx].GetString();
			    const auto include_idx = include_subsections.sel->get_index(row);
			    const bool include = include_subsections.validity.RowIsValid(include_idx) &&
			                         UnifiedVectorFormat::GetData<bool>(include_subsections)[include_idx];

			    if (markdown_str.empty() || section_id_str.empty()) {
				    markdown_writer::WriteString(result, row, std::string());
				    continue;
			    }

			    try {
				    markdown_writer::WriteString(result, row,
				                                 markdown_utils::ExtractSection(markdown_str, section_id_str, include));
			    } catch (const std::exception &e) {
				    ThrowMarkdownError("extracting a Markdown section", e);
			    }
		    }
	    });

	ScalarFunctionSet extract_sec_set("md_extract_section");
	md_extract_section.SetFallible();
	md_extract_section_with_subsections.SetFallible();
	extract_sec_set.AddFunction(md_extract_section);
	extract_sec_set.AddFunction(md_extract_section_with_subsections);

	CreateScalarFunctionInfo sec_info(std::move(extract_sec_set));
	sec_info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription sec_desc1;
	sec_desc1.parameter_names = {"markdown", "section_id"};
	sec_desc1.description = "Extract content of a specific section by heading title or ID.";
	sec_desc1.examples = {"md_extract_section('# Intro\nText', 'Intro')"};
	sec_desc1.categories = {"markdown"};
	sec_info.descriptions.push_back(sec_desc1);

	FunctionDescription sec_desc2;
	sec_desc2.parameter_names = {"markdown", "section_id", "include_subsections"};
	sec_desc2.description =
	    "Extract content of a specific section by heading title or ID, optionally including subsections.";
	sec_desc2.examples = {"md_extract_section('# Intro\n## Sub\nText', 'Intro', true)"};
	sec_desc2.categories = {"markdown"};
	sec_info.descriptions.push_back(sec_desc2);
	loader.RegisterFunction(std::move(sec_info));

	// Register md_section_breadcrumb function
	ScalarFunction md_section_breadcrumb(
	    "md_section_breadcrumb", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::VARCHAR,
	    [](DataChunk &args, ExpressionState &state, Vector &result) {
		    auto &file_path_vector = args.data[0];
		    auto &section_id_vector = args.data[1];

		    BinaryExecutor::Execute<string_t, string_t, string_t>(
		        file_path_vector, section_id_vector, result, args.size(),
		        [&](string_t file_path_str, string_t section_id_str) -> string_t {
			        if (file_path_str.GetSize() == 0 || section_id_str.GetSize() == 0) {
				        return string_t();
			        }

			        const std::string breadcrumb =
			            markdown_utils::GenerateBreadcrumb(file_path_str.GetString(), section_id_str.GetString());
			        return StringVector::AddString(result, breadcrumb);
		        });
	    });

	CreateScalarFunctionInfo breadcrumb_info(std::move(md_section_breadcrumb));
	breadcrumb_info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription breadcrumb_desc;
	breadcrumb_desc.parameter_names = {"file_path", "section_id"};
	breadcrumb_desc.description = "Generate a breadcrumb string combining file path and section identifier.";
	breadcrumb_desc.examples = {"md_section_breadcrumb('doc.md', 'intro')"};
	breadcrumb_desc.categories = {"markdown"};
	breadcrumb_info.descriptions.push_back(breadcrumb_desc);
	loader.RegisterFunction(std::move(breadcrumb_info));
}

void MarkdownFunctions::RegisterMetadataFunctions(ExtensionLoader &loader) {
	auto markdown_type = MarkdownTypes::MarkdownType();

	// md_extract_metadata function - extract frontmatter as MAP(VARCHAR, VARCHAR)
	auto map_type = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);
	ScalarFunction md_extract_metadata_fun(
	    "md_extract_metadata", {markdown_type}, map_type, [](DataChunk &args, ExpressionState &state, Vector &result) {
		    const auto count = args.size();
		    UnifiedVectorFormat markdowns;
		    CompatToUnifiedFormat(args.data[0], count, markdowns);
		    result.SetVectorType(VectorType::FLAT_VECTOR);
		    const std::map<std::string, std::string> no_fields;

		    for (idx_t row = 0; row < count; row++) {
			    const auto idx = markdowns.sel->get_index(row);
			    if (!markdowns.validity.RowIsValid(idx)) {
				    markdown_writer::WriteStringMap(result, row, no_fields);
				    continue;
			    }

			    const auto md_str = UnifiedVectorFormat::GetData<string_t>(markdowns)[idx].GetString();
			    if (md_str.empty()) {
				    markdown_writer::WriteStringMap(result, row, no_fields);
				    continue;
			    }

			    try {
				    markdown_writer::WriteStringMap(result, row, markdown_utils::ExtractMetadata(md_str).custom_fields);
			    } catch (const std::exception &e) {
				    ThrowMarkdownError("extracting Markdown metadata", e);
			    }
		    }
	    });

	md_extract_metadata_fun.SetFallible();
	CreateScalarFunctionInfo metadata_info(std::move(md_extract_metadata_fun));
	metadata_info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription metadata_desc;
	metadata_desc.parameter_names = {"markdown"};
	metadata_desc.description = "Extract YAML frontmatter from Markdown text as a MAP.";
	metadata_desc.examples = {"md_extract_metadata('---\ntitle: Test\n---\n# Body')"};
	metadata_desc.categories = {"markdown"};
	metadata_info.descriptions.push_back(metadata_desc);
	loader.RegisterFunction(std::move(metadata_info));

	// md_extract_frontmatter - the RAW frontmatter block (text between the --- fences) as VARCHAR,
	// NULL when there is no frontmatter. This is the lightweight-markdown / real-YAML seam: pair it
	// with duckdb_yaml (yaml(...) / read_yaml_frontmatter) when you need full YAML fidelity, without
	// markdown itself carrying a YAML parser.
	ScalarFunction md_extract_frontmatter_fun(
	    "md_extract_frontmatter", {markdown_type}, LogicalType::VARCHAR,
	    [](DataChunk &args, ExpressionState &state, Vector &result) {
		    const auto count = args.size();
		    UnifiedVectorFormat markdowns;
		    CompatToUnifiedFormat(args.data[0], count, markdowns);
		    result.SetVectorType(VectorType::FLAT_VECTOR);

		    for (idx_t row = 0; row < count; row++) {
			    const auto idx = markdowns.sel->get_index(row);
			    if (!markdowns.validity.RowIsValid(idx)) {
				    markdown_writer::WriteNull(result, row);
				    continue;
			    }

			    try {
				    markdown_writer::WriteOptionalString(
				        result, row,
				        markdown_utils::ExtractRawFrontmatter(
				            UnifiedVectorFormat::GetData<string_t>(markdowns)[idx].GetString()));
			    } catch (const std::exception &e) {
				    ThrowMarkdownError("extracting Markdown frontmatter", e);
			    }
		    }
	    });

	md_extract_frontmatter_fun.SetFallible();
	CreateScalarFunctionInfo frontmatter_info(std::move(md_extract_frontmatter_fun));
	frontmatter_info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription frontmatter_desc;
	frontmatter_desc.parameter_names = {"markdown"};
	frontmatter_desc.description = "Extract the raw YAML frontmatter text block from Markdown.";
	frontmatter_desc.examples = {"md_extract_frontmatter('---\ntitle: Test\n---\n# Body')"};
	frontmatter_desc.categories = {"markdown"};
	frontmatter_info.descriptions.push_back(frontmatter_desc);
	loader.RegisterFunction(std::move(frontmatter_info));
}

} // namespace duckdb
