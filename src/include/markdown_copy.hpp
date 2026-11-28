#pragma once

#include "duckdb.hpp"
#include "duckdb_compat.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/formats/csv_encoder.hpp"
#include <string>
#include <vector>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Markdown Copy Bind Data
//===--------------------------------------------------------------------===//

struct WriteMarkdownBindData : public FunctionData {
	// Mode (explicit, no auto-detect)
	enum class MarkdownMode { TABLE, DOCUMENT, BLOCKS };
	MarkdownMode markdown_mode = MarkdownMode::TABLE;

	// Common options
	string null_value = "";

	// Table mode options
	bool header = true;
	bool escape_pipes = true;
	bool escape_newlines = true;

	// Document mode options (mirror reader)
	string frontmatter = ""; // Raw YAML or empty
	string content_column = "content";
	string title_column = "title";
	string level_column = "level";
	int32_t blank_lines = 1;

	// Blocks mode column names (configurable) - uses duck_block naming
	string kind_column = "kind";
	string element_type_column = "element_type";
	string encoding_column = "encoding";
	string attributes_column = "attributes";

	// Resolved schema info
	idx_t level_col_idx = DConstants::INVALID_INDEX;
	idx_t title_col_idx = DConstants::INVALID_INDEX;
	idx_t content_col_idx = DConstants::INVALID_INDEX;
	// Blocks mode column indices
	idx_t kind_col_idx = DConstants::INVALID_INDEX;
	idx_t element_type_col_idx = DConstants::INVALID_INDEX;
	idx_t encoding_col_idx = DConstants::INVALID_INDEX;
	idx_t attributes_col_idx = DConstants::INVALID_INDEX;
	vector<string> alignments; // Per-column alignment for table mode
	vector<string> column_names;
	vector<LogicalType> column_types;

public:
	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other) const override;
};

//===--------------------------------------------------------------------===//
// Markdown Copy Global State
//===--------------------------------------------------------------------===//

struct WriteMarkdownGlobalState : public GlobalFunctionData {
	//! File handle for writing
	unique_ptr<FileHandle> handle;
	//! Lock for thread-safe writes
	mutex write_lock;
	//! Whether the header has been written (table mode)
	bool header_written = false;
	//! Whether frontmatter has been written (document mode)
	bool frontmatter_written = false;
};

//===--------------------------------------------------------------------===//
// Markdown Copy Local State
//===--------------------------------------------------------------------===//

enum class MarkdownCellFormat : uint8_t {
	TEXT,
	BOOLEAN,
	INT8,
	INT16,
	INT32,
	INT64,
	UINT8,
	UINT16,
	UINT32,
	UINT64,
	DECIMAL16,
	DECIMAL32,
	DECIMAL64,
	DECIMAL128,
	DATE,
	TIME,
	TIME_NS,
	TIMESTAMP,
	TIMESTAMP_NS,
	INTERVAL,
	UUID
};

struct MarkdownCellColumn {
	MarkdownCellFormat format = MarkdownCellFormat::TEXT;
	bool check = true;
	uint8_t width = 0;
	uint8_t scale = 0;
	idx_t max_length = 0;
};

struct WriteMarkdownLocalState : public LocalFunctionData {
	//! Local buffer for accumulating output
	string buffer;
	//! Blocks mode: the elements seen so far, rendered as ONE list in Combine.
	//! Not rendered per row: a block absorbs the inline run that follows it, and
	//! a row-at-a-time renderer has already emitted the block's trailing newlines
	//! before it can see whether inlines follow. Accumulating across the local
	//! state also keeps a run intact when it spans two DataChunks.
	vector<Value> elements;
	LogicalType block_type;
	CsvSpecials cell_specials;
	vector<MarkdownCellColumn> cell_columns;
	string escaped_null;
};

//===--------------------------------------------------------------------===//
// Markdown Copy Functions
//===--------------------------------------------------------------------===//

class MarkdownCopyFunction {
public:
	//! Register the markdown copy function
	static void Register(ExtensionLoader &loader);

	//! Bind function - parse options and set up schema info
	static unique_ptr<FunctionData> Bind(ClientContext &context, CopyFunctionBindInput &input,
	                                     const vector<CompatName> &names, const vector<LogicalType> &sql_types);

	//! Initialize global state - open file for writing
	static unique_ptr<GlobalFunctionData> InitializeGlobal(ClientContext &context, FunctionData &bind_data,
	                                                       const string &file_path);

	//! Initialize local state - create buffer
	static unique_ptr<LocalFunctionData> InitializeLocal(ExecutionContext &context, FunctionData &bind_data);

	//! Sink function - write data chunks
	static void Sink(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
	                 LocalFunctionData &lstate, DataChunk &input);

	//! Combine function - flush local buffers to global
	static void Combine(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
	                    LocalFunctionData &lstate);

	//! Finalize function - close file
	static void Finalize(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate);

	//! Copy options registration
	static void CopyOptions(ClientContext &context, CopyOptionsInput &input);

private:
	//===--------------------------------------------------------------------===//
	// Table Mode Helpers
	//===--------------------------------------------------------------------===//

	//! Detect alignment based on column type
	static string DetectAlignment(const LogicalType &type);

	//! Render the header row for table mode
	static string RenderTableHeader(const WriteMarkdownBindData &bind_data);

	//! Render the separator row with alignment indicators
	static string RenderTableSeparator(const WriteMarkdownBindData &bind_data);

	static void AppendTableRows(WriteMarkdownLocalState &lstate, const vector<UnifiedVectorFormat> &columns,
	                            idx_t count, const WriteMarkdownBindData &bind_data);

	static void WriteBuffer(const WriteMarkdownBindData &bind_data, WriteMarkdownGlobalState &gstate,
	                        WriteMarkdownLocalState &lstate);

	//===--------------------------------------------------------------------===//
	// Document Mode Helpers
	//===--------------------------------------------------------------------===//

	//! Render frontmatter YAML block
	static string RenderFrontmatter(const WriteMarkdownBindData &bind_data);

	//! Render a section heading and content
	static string RenderSection(int32_t level, const string &title, const string &content,
	                            const WriteMarkdownBindData &bind_data);

	//===--------------------------------------------------------------------===//
	// Blocks Mode
	//
	// No render helpers here: blocks mode uses the SHARED duck_block renderer
	// (DuckBlockFunctions::RenderDuckBlocksToMarkdown), called once per local
	// state in Combine. Sink only rebuilds each row into a duck_block struct.
};

} // namespace duckdb
