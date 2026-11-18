#include "duck_block_functions.hpp"
#include "markdown_types.hpp"
#include "markdown_utils.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "fast_float/fast_float.h"
#include "duckdb/common/json_document.hpp"
#include "duck_block_vocabulary.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include <sstream>

namespace duckdb {

static const JSONNode *ReadJson(JSONTreeParser &parser, const string &content) {
	JSONParseError error;
	return parser.Parse(content.c_str(), content.size(), error);
}

// Claims in this file about ANOTHER repository's behaviour carry a date and,
// where they came from someone else, the label RELAYED. A reason has no expiry
// and no attribution unless it is given one: two comments here were true when
// written and wrong within the day -- one measured and then outlived by a spec
// change, one accurately reported and then restated without its provenance.
// Neither was careless, so "be more careful" would not have prevented either.
// A date and a source are actionable; a better sentence is not.

// The duck_block vocabulary, consumed from duck_block_utils as a submodule.
// Spelling these as constants rather than string literals is what makes a
// renamed or removed element type a compile error here instead of output that
// silently stops matching.
using Vocab = DuckBlockVocabulary;

// Defined below, used by both the inline and the block renderer.
static string EscapeMarkdownText(const string &content, bool at_line_start);

// Maximum Pandoc inline-nesting depth walked by ExtractPandocText. Guards
// against unbounded recursion (stack overflow) on adversarially nested JSON.
// Sized so the guard is REACHABLE, which at 1000 it was not. Each frame of
// ExtractPandocTextFromVal costs ~275 bytes (measured: a 512 KB stack dies
// between depth 900 and 950, a 256 KB stack between 400 and 500). A macOS
// worker thread gets 512 KB by default, so a cap of 1000 needed ~275 KB of
// walker plus whatever DuckDB's execution stack was already using -- the
// process died before the check could fire. That is what it looks like from
// CI: SIGBUS/SIGSEGV mid-suite, not an error. 200 keeps the walker under
// ~55 KB on every platform while still allowing ~100 levels of nested
// emphasis, far past any real document.
static constexpr int MAX_PANDOC_DEPTH = 200;

//===--------------------------------------------------------------------===//
// Helper Functions
//===--------------------------------------------------------------------===//

static int ParseIntAttribute(const char *name, const string &value) {
	int result;
	const char *end = value.data() + value.size();
	auto parsed = fast_float::from_chars(value.data(), end, result);
	if (parsed.ec != std::errc() || parsed.ptr != end) {
		throw InvalidInputException("duck_block attribute %s is not an integer: '%s'", name, value);
	}
	return result;
}

string DuckBlockFunctions::GetAttribute(const Value &attributes, const string &key) {
	if (attributes.IsNull() || attributes.type().id() != LogicalTypeId::MAP) {
		return "";
	}
	auto &map_children = MapValue::GetChildren(attributes);
	for (const auto &entry : map_children) {
		auto &entry_children = StructValue::GetChildren(entry);
		if (entry_children.size() == 2 && !entry_children[0].IsNull()) {
			if (entry_children[0].ToString() == key && !entry_children[1].IsNull()) {
				return entry_children[1].ToString();
			}
		}
	}
	return "";
}

vector<string> DuckBlockFunctions::ParseJsonListItems(JSONTreeParser &parser, const string &content) {
	vector<string> items;
	if (content.empty()) {
		return items;
	}
	auto root = ReadJson(parser, content);
	if (!root) {
		return items;
	}
	if (root->type == JSONValueType::ARRAY) {
		for (auto &val : root->Elements()) {
			if (val.type == JSONValueType::STRING) {
				items.emplace_back(val.GetString());
			}
		}
	}
	return items;
}

void DuckBlockFunctions::ParseJsonTable(JSONTreeParser &parser, const string &content, vector<string> &headers,
                                        vector<vector<string>> &rows) {
	if (content.empty()) {
		return;
	}
	auto root = ReadJson(parser, content);
	if (!root) {
		return;
	}
	if (root->type == JSONValueType::OBJECT) {
		auto headers_val = root->Get("headers");
		if (headers_val && headers_val->type == JSONValueType::ARRAY) {
			for (auto &val : headers_val->Elements()) {
				if (val.type == JSONValueType::STRING) {
					headers.emplace_back(val.GetString());
				}
			}
		}

		auto rows_val = root->Get("rows");
		if (rows_val && rows_val->type == JSONValueType::ARRAY) {
			for (auto &row_val : rows_val->Elements()) {
				if (row_val.type == JSONValueType::ARRAY) {
					vector<string> row;
					for (auto &cell_val : row_val.Elements()) {
						if (cell_val.type == JSONValueType::STRING) {
							row.emplace_back(cell_val.GetString());
						}
					}
					if (!row.empty()) {
						rows.push_back(std::move(row));
					}
				}
			}
		}
	}
}

static string ExtractPandocTextFromVal(const JSONNode *val, int depth) {
	if (!val) {
		return "";
	}
	if (depth > MAX_PANDOC_DEPTH) {
		throw InvalidInputException("Pandoc inline nesting exceeds maximum supported depth (%d)", MAX_PANDOC_DEPTH);
	}
	if (val->type == JSONValueType::STRING) {
		return string(val->GetString());
	}
	if (val->type == JSONValueType::ARRAY) {
		string result;
		for (auto &item : val->Elements()) {
			// Counts toward depth like every other recursion. It did not used to, which
			// meant nested ARRAYS added stack frames the guard never saw: no value of
			// MAX_PANDOC_DEPTH could bound them, and deep-enough array nesting crashed
			// the process instead of throwing (measured: 100000 levels segfaulted on an
			// 8 MB stack while the same depth in objects threw cleanly).
			result += ExtractPandocTextFromVal(&item, depth + 1);
		}
		return result;
	}
	if (val->type == JSONValueType::OBJECT) {
		auto t_val = val->Get("t");
		if (!t_val || t_val->type != JSONValueType::STRING) {
			return "";
		}
		auto t = t_val->GetString();
		t = t.substr(0, t.find('\0'));
		auto c_val = val->Get("c");

		if (t == "Str") {
			if (c_val && c_val->type == JSONValueType::STRING) {
				return string(c_val->GetString());
			}
		} else if (t == "Space" || t == "SoftBreak") {
			return " ";
		} else if (t == "LineBreak") {
			return "\n";
		} else if (t == "Strong") {
			return "**" + ExtractPandocTextFromVal(c_val, depth + 1) + "**";
		} else if (t == "Emph") {
			return "*" + ExtractPandocTextFromVal(c_val, depth + 1) + "*";
		} else if (t == "Strikeout") {
			return "~~" + ExtractPandocTextFromVal(c_val, depth + 1) + "~~";
		} else if (t == "Superscript") {
			return "^" + ExtractPandocTextFromVal(c_val, depth + 1) + "^";
		} else if (t == "Subscript") {
			return "~" + ExtractPandocTextFromVal(c_val, depth + 1) + "~";
		} else if (t == "Code") {
			if (c_val && c_val->type == JSONValueType::ARRAY && c_val->size >= 2) {
				auto code_str = c_val->Get(1);
				if (code_str && code_str->type == JSONValueType::STRING) {
					return "`" + string(code_str->GetString()) + "`";
				}
			}
		} else if (t == "Math") {
			if (c_val && c_val->type == JSONValueType::ARRAY && c_val->size >= 2) {
				auto math_str = c_val->Get(1);
				if (math_str && math_str->type == JSONValueType::STRING) {
					return "$" + string(math_str->GetString()) + "$";
				}
			}
		} else if (t == "Link") {
			if (c_val && c_val->type == JSONValueType::ARRAY && c_val->size >= 3) {
				auto inlines = c_val->Get(1);
				auto target = c_val->Get(2);
				string text = ExtractPandocTextFromVal(inlines, depth + 1);
				string url;
				if (target && target->type == JSONValueType::ARRAY && target->size >= 1) {
					auto u = target->Get(0);
					if (u && u->type == JSONValueType::STRING) {
						url = string(u->GetString());
					}
				}
				return "[" + text + "](" + url + ")";
			}
		} else if (t == "Image") {
			if (c_val && c_val->type == JSONValueType::ARRAY && c_val->size >= 3) {
				auto inlines = c_val->Get(1);
				auto target = c_val->Get(2);
				string alt = ExtractPandocTextFromVal(inlines, depth + 1);
				string src;
				if (target && target->type == JSONValueType::ARRAY && target->size >= 1) {
					auto s = target->Get(0);
					if (s && s->type == JSONValueType::STRING) {
						src = string(s->GetString());
					}
				}
				return "![" + alt + "](" + src + ")";
			}
		} else if (t == "Plain" || t == "Para" || t == "Span" || t == "SmallCaps" || t == "Quoted" || t == "Cite") {
			return ExtractPandocTextFromVal(c_val, depth + 1);
		}
	}
	return "";
}

// Pandoc list JSON. BulletList is [[block,...], ...]; OrderedList wraps the same
// item array in [[start, style, delim], [...]]. Neither is the ["a", "b"] shape
// ParseJsonListItems reads, so a real producer's list parsed to nothing and the
// items were dropped without a trace. Sets `start` only for the ordered form,
// so an explicit start attribute keeps precedence for the legacy shape.
static bool ParsePandocListItems(JSONTreeParser &parser, const string &content, vector<string> &items, int &start,
                                 bool start_from_attribute) {
	if (content.empty()) {
		return false;
	}
	auto root = ReadJson(parser, content);
	if (!root) {
		return false;
	}
	auto item_array = root;
	if (root->type == JSONValueType::ARRAY && root->size == 2) {
		auto first = root->Get(0);
		auto second = root->Get(1);
		// The ordered form is distinguishable because its leading triple starts
		// with the list's first number; a bullet list's first item is an array of
		// block objects.
		if (first->type == JSONValueType::ARRAY && first->size == 3 && first->Get(0)->IsInteger() &&
		    second->type == JSONValueType::ARRAY) {
			if (!start_from_attribute) {
				start = static_cast<int>(first->Get(0)->signed_integer);
			}
			item_array = second;
		}
	}
	if (item_array->type == JSONValueType::ARRAY) {
		for (auto &item : item_array->Elements()) {
			string text = ExtractPandocTextFromVal(&item, 0);
			if (!text.empty()) {
				items.push_back(std::move(text));
			}
		}
	}
	return !items.empty();
}

// Flatten a Pandoc block array to text, keeping paragraph breaks between the
// top-level blocks. Returns "" when the content is not a Pandoc block array, so
// callers can fall back rather than replace real content with nothing.
static string ExtractPandocBlocksText(JSONTreeParser &parser, const string &content) {
	if (content.empty()) {
		return "";
	}
	auto root = ReadJson(parser, content);
	if (!root) {
		return "";
	}
	string result;
	if (root->type == JSONValueType::ARRAY) {
		for (auto &block : root->Elements()) {
			string text = ExtractPandocTextFromVal(&block, 0);
			if (text.empty()) {
				continue;
			}
			if (!result.empty()) {
				result += "\n\n";
			}
			result += text;
		}
	}
	return result;
}

// Pandoc DefinitionList JSON: [ [ [term inlines], [ [def blocks], ... ] ], ... ].
// Definitions are lists of blocks; ExtractPandocTextFromVal already flattens Para
// and Plain, so a multi-block definition concatenates rather than being lost.
static bool ParseDefinitionList(JSONTreeParser &parser, const string &content,
                                vector<std::pair<string, vector<string>>> &entries) {
	if (content.empty()) {
		return false;
	}
	auto root = ReadJson(parser, content);
	if (!root) {
		return false;
	}
	if (root->type == JSONValueType::ARRAY) {
		for (auto &entry : root->Elements()) {
			if (entry.type != JSONValueType::ARRAY || entry.size < 2) {
				continue;
			}
			string term = ExtractPandocTextFromVal(entry.Get(0), 0);
			vector<string> definitions;
			auto def_list = entry.Get(1);
			if (def_list->type == JSONValueType::ARRAY) {
				for (auto &def : def_list->Elements()) {
					string text = ExtractPandocTextFromVal(&def, 0);
					if (!text.empty()) {
						definitions.push_back(std::move(text));
					}
				}
			}
			entries.emplace_back(std::move(term), std::move(definitions));
		}
	}
	return !entries.empty();
}

// Make a string safe to sit inside an HTML comment: "--" terminates one early, so
// a source_type from an untrusted document must not be able to close it.
static string SanitizeForHtmlComment(const string &text) {
	string safe;
	safe.reserve(text.size());
	for (char c : text) {
		if (c == '\n' || c == '\r') {
			safe += ' ';
		} else if (c == '-' && !safe.empty() && safe.back() == '-') {
			continue;
		} else {
			safe += c;
		}
	}
	return safe;
}

string DuckBlockFunctions::ExtractPandocText(JSONTreeParser &parser, const string &content, int depth) {
	if (content.empty()) {
		return "";
	}
	auto root = ReadJson(parser, content);
	if (!root) {
		return "";
	}
	return ExtractPandocTextFromVal(root, depth);
}

bool DuckBlockFunctions::IsPandocTableFormat(JSONTreeParser &parser, const string &content) {
	if (content.empty()) {
		return false;
	}
	auto root = ReadJson(parser, content);
	if (!root) {
		return false;
	}
	bool is_table = false;
	if (root->type == JSONValueType::ARRAY) {
		size_t sz = root->size;
		if (sz == 5 || sz == 6) {
			auto e1 = root->Get(1);
			if (e1 && e1->type == JSONValueType::ARRAY && e1->size > 0) {
				auto first = e1->Get(0);
				if (first && first->type == JSONValueType::OBJECT) {
					auto t = first->Get("t");
					if (t && t->type == JSONValueType::STRING && StringUtil::StartsWith(t->GetString(), "Align")) {
						is_table = true;
					}
				}
			}
			if (!is_table && sz == 6) {
				auto e2 = root->Get(2);
				if (e2 && e2->type == JSONValueType::ARRAY) {
					is_table = true;
				}
			}
		}
	}
	return is_table;
}

static string ExtractPandocCellText(const JSONNode *cell_val) {
	if (!cell_val) {
		return "";
	}
	return ExtractPandocTextFromVal(cell_val, 0);
}

void DuckBlockFunctions::ParsePandocTable(JSONTreeParser &parser, const string &content, vector<string> &headers,
                                          vector<vector<string>> &rows) {
	if (content.empty()) {
		return;
	}
	auto root = ReadJson(parser, content);
	if (!root) {
		return;
	}
	if (root->type != JSONValueType::ARRAY) {
		return;
	}

	size_t root_size = root->size;

	if (root_size == 5) {
		// Legacy format: [caption, alignments, widths, headers, rows]
		auto headers_arr = root->Get(3);
		if (headers_arr && headers_arr->type == JSONValueType::ARRAY) {
			for (auto &cell : headers_arr->Elements()) {
				headers.push_back(ExtractPandocCellText(&cell));
			}
		}

		auto rows_arr = root->Get(4);
		if (rows_arr && rows_arr->type == JSONValueType::ARRAY) {
			while (rows_arr->size == 1) {
				auto first_row = rows_arr->Get(0);
				if (!first_row || first_row->type != JSONValueType::ARRAY) {
					break;
				}
				auto first_elem = first_row->Get(0);
				if (!first_elem || first_elem->type != JSONValueType::ARRAY) {
					break;
				}
				auto first_inner = first_elem->Get(0);
				if (!first_inner || first_inner->type != JSONValueType::ARRAY) {
					break;
				}
				rows_arr = first_row;
			}

			for (auto &row : rows_arr->Elements()) {
				if (row.type == JSONValueType::ARRAY) {
					vector<string> r;
					for (auto &cell : row.Elements()) {
						r.push_back(ExtractPandocCellText(&cell));
					}
					if (!r.empty()) {
						rows.push_back(std::move(r));
					}
				}
			}
		}
	} else if (root_size >= 6) {
		// Modern format: [attr, caption, colspec, tableHead, tableBodies, tableFoot]
		auto table_head = root->Get(3);
		if (table_head && table_head->type == JSONValueType::ARRAY && table_head->size >= 2) {
			auto head_rows = table_head->Get(1);
			if (head_rows && head_rows->type == JSONValueType::ARRAY) {
				for (auto &head_row : head_rows->Elements()) {
					if (head_row.type == JSONValueType::ARRAY && head_row.size >= 2) {
						auto cells = head_row.Get(1);
						if (cells && cells->type == JSONValueType::ARRAY) {
							for (auto &cell : cells->Elements()) {
								if (cell.type == JSONValueType::ARRAY && cell.size >= 5) {
									auto blocks = cell.Get(4);
									headers.push_back(ExtractPandocCellText(blocks));
								} else {
									headers.push_back(ExtractPandocCellText(&cell));
								}
							}
						}
					}
				}
			}
		}

		auto table_bodies = root->Get(4);
		if (table_bodies && table_bodies->type == JSONValueType::ARRAY) {
			for (auto &body : table_bodies->Elements()) {
				if (body.type == JSONValueType::ARRAY && body.size >= 4) {
					auto body_rows = body.Get(3);
					if (body_rows && body_rows->type == JSONValueType::ARRAY) {
						for (auto &row : body_rows->Elements()) {
							if (row.type == JSONValueType::ARRAY && row.size >= 2) {
								auto cells = row.Get(1);
								if (cells && cells->type == JSONValueType::ARRAY) {
									vector<string> r;
									for (auto &cell : cells->Elements()) {
										if (cell.type == JSONValueType::ARRAY && cell.size >= 5) {
											auto blocks = cell.Get(4);
											r.push_back(ExtractPandocCellText(blocks));
										} else {
											r.push_back(ExtractPandocCellText(&cell));
										}
									}
									if (!r.empty()) {
										rows.push_back(std::move(r));
									}
								}
							}
						}
					}
				}
			}
		}
	}

	if (headers.empty() && !rows.empty()) {
		headers = rows[0];
		rows.erase(rows.begin());
	}
}

//===--------------------------------------------------------------------===//
// RenderInlineElementToMarkdown (helper for inline elements)
//===--------------------------------------------------------------------===//

// Escape the characters that would otherwise be re-parsed as markup.
//
// cmark DECODES backslash escapes into plain text -- `\\#` becomes a text node
// holding `#` -- so a writer that emits text verbatim loses the escape and the
// document changes meaning on the next parse:
//
//   \\# not a heading   pass 1: paragraph      pass 2: HEADING
//   a \\*x\\* b          pass 1: 0 italics     pass 2: 1 italic
//
// Only characters that can BEGIN a construct are escaped, and `#` only where a
// heading could actually start. Escaping more would be safe for meaning and
// worse for the bytes: prose containing an ordinary `*` would grow a backslash
// that was never in the source.

string DuckBlockFunctions::RenderInlineElementToMarkdown(const string &element_type, const string &content,
                                                         const Value &attributes) {
	if (element_type == Vocab::INLINE_LINK) {
		// [text](href "title")
		string href = GetAttribute(attributes, "href");
		string title = GetAttribute(attributes, "title");
		string result = "[" + content + "](" + href;
		if (!title.empty()) {
			result += " \"" + title + "\"";
		}
		result += ")";
		return result;
	} else if (element_type == Vocab::INLINE_IMAGE) {
		// ![alt](src "title")
		string src = GetAttribute(attributes, "src");
		string title = GetAttribute(attributes, "title");
		string result = "![" + content + "](" + src;
		if (!title.empty()) {
			result += " \"" + title + "\"";
		}
		result += ")";
		return result;
	} else if (element_type == Vocab::INLINE_BOLD || element_type == "strong") {
		// **text**
		return "**" + content + "**";
	} else if (element_type == Vocab::INLINE_ITALIC || element_type == "emphasis" || element_type == "em") {
		// *text*
		return "*" + content + "*";
	} else if (element_type == Vocab::INLINE_CODE) {
		// `text` - inline code
		// Handle content containing backticks
		if (content.find('`') != string::npos) {
			return "`` " + content + " ``";
		}
		return "`" + content + "`";
	} else if (element_type == Vocab::INLINE_TEXT) {
		// Plain text, re-escaped so it cannot be re-parsed as markup.
		//
		// at_line_start=true: an inline run is frequently the first thing in its
		// block, where a leading `#` or `>` WOULD open a heading or a quote. The
		// renderer cannot see its own position, so it assumes the risky one. The
		// cost is a backslash on a `#` that begins a mid-paragraph run, which
		// renders as `#` either way; the alternative is a paragraph that silently
		// becomes a heading on the next parse.
		return EscapeMarkdownText(content, true);
	} else if (element_type == Vocab::INLINE_SPACE) {
		// Word separator
		return " ";
	} else if (element_type == Vocab::INLINE_SOFTBREAK) {
		// Soft line break
		return "\n";
	} else if (element_type == Vocab::INLINE_LINEBREAK || element_type == "br") {
		// Hard line break
		return "  \n";
	} else if (element_type == Vocab::INLINE_STRIKETHROUGH || element_type == "del") {
		// ~~text~~
		return "~~" + content + "~~";
	} else if (element_type == Vocab::INLINE_SUPERSCRIPT || element_type == "sup") {
		// ^text^
		return "^" + content + "^";
	} else if (element_type == Vocab::INLINE_SUBSCRIPT || element_type == "sub") {
		// ~text~
		return "~" + content + "~";
	} else if (element_type == Vocab::INLINE_UNDERLINE) {
		// <u>text</u> (no standard markdown, use HTML)
		return "<u>" + content + "</u>";
	} else if (element_type == Vocab::INLINE_SMALLCAPS) {
		// No standard markdown for small caps, use span with style
		return "<span style=\"font-variant: small-caps\">" + content + "</span>";
	} else if (element_type == Vocab::INLINE_MATH) {
		// $text$ for inline, $$text$$ for display
		string display = GetAttribute(attributes, "display");
		if (display == "block") {
			return "$$" + content + "$$";
		}
		return "$" + content + "$";
	} else if (element_type == Vocab::INLINE_RAW) {
		// Raw content as-is
		return content;
	} else if (element_type == Vocab::INLINE_QUOTED) {
		// Quoted text
		string quote_type = GetAttribute(attributes, "quote_type");
		if (quote_type == "single") {
			return "'" + content + "'";
		}
		return "\"" + content + "\"";
	} else if (element_type == Vocab::INLINE_CITE) {
		// Citation [@key]
		string key = GetAttribute(attributes, "key");
		if (!key.empty()) {
			return "[@" + key + "]";
		}
		return content;
	} else if (element_type == Vocab::INLINE_NOTE) {
		// Footnote [^note]
		return "[^" + content + "]";
	} else if (element_type == Vocab::INLINE_SPAN) {
		// Generic span - just output content
		return content;
	} else if (element_type == Vocab::INLINE_GENERIC) {
		// The inline backstop for a construct outside this vocabulary. Unlike its
		// block counterpart it carries no content of its own -- the text is in its
		// children, which reach us as `content` -- so passing it through loses only
		// the wrapper identity, not the words. A comment would interrupt the prose
		// it sits inside, so this is deliberately quieter than the block case.
		return content;
	} else {
		// Unknown inline type - output as plain text
		return content;
	}
}

//===--------------------------------------------------------------------===//
// RenderBlockElementToMarkdown (helper for block elements)
//===--------------------------------------------------------------------===//

static string EscapeMarkdownText(const string &content, bool at_line_start) {
	string out;
	out.reserve(content.size());
	for (size_t i = 0; i < content.size(); i++) {
		const char c = content[i];
		const bool line_start = (i == 0) ? at_line_start : (content[i - 1] == '\n');
		bool escape = false;
		switch (c) {
		case '\\':
		case '`':
		case '*':
		case '[':
		case ']':
			escape = true;
			break;
		case '_':
			// Intraword underscores do not open emphasis in CommonMark, so a
			// snake_case identifier keeps its bytes.
			escape = (i == 0 || !isalnum(static_cast<unsigned char>(content[i - 1]))) ||
			         (i + 1 >= content.size() || !isalnum(static_cast<unsigned char>(content[i + 1])));
			break;
		case '<':
			// Only where it could open a tag or an autolink.
			escape = i + 1 < content.size() && (isalpha(static_cast<unsigned char>(content[i + 1])) ||
			                                    content[i + 1] == '/' || content[i + 1] == '!');
			break;
		case '#':
		case '>':
			escape = line_start;
			break;
		default:
			break;
		}
		if (escape) {
			out += '\\';
		}
		out += c;
	}
	return out;
}

// Several branches below accept an HTML-flavoured alias beside the canonical
// vocabulary name -- "em"/"emphasis" for italic, "strong" for bold, "del" for
// strikethrough, and so on.
//
// MEASURED 2026-08-31 against duck_block_utils spec 6.1, because this was
// previously described here and to other consumers
// as "legacy names this extension's own parser emits", and that is false: it
// emits none of them. Nor does duck_block_utils, nor webbed -- webbed MAPS the
// HTML tags `em`/`strong`/`del` onto the canonical names and carries the same
// accepting aliases in its own writer, so two consumers hold identical
// leniency that no observed producer exercises.
//
// Kept rather than deleted: accepting an alias costs nothing and removing one
// can only break a producer nobody here has looked at. But it is defensive
// acceptance, not compatibility with anything known, and the difference
// matters if someone later reasons from its presence.

string DuckBlockFunctions::RenderBlockElementToMarkdown(JSONTreeParser &parser, const string &element_type,
                                                        const string &content, int32_t level, const string &encoding,
                                                        const Value &attributes) {
	string result;

	if (element_type == "frontmatter" || element_type == Vocab::TYPE_METADATA) {
		// The FENCE follows the encoding: `+++` for TOML, `---` otherwise. Writing
		// `---` around a TOML body is a worse failure than not recognising TOML at
		// all -- the bytes then parse cleanly as the wrong format instead of
		// failing loudly, and the round-trip silently relabels a Hugo document.
		const string fence = (encoding == Vocab::ENCODING_TOML) ? "+++" : "---";
		result = fence + "\n" + content + "\n" + fence + "\n\n";
	} else if (element_type == Vocab::TYPE_HEADING) {
		// ATX heading with level
		// Per spec: heading_level attribute takes priority, fall back to level field
		int32_t heading_level = 1;
		string heading_level_attr = GetAttribute(attributes, Vocab::ATTR_HEADING_LEVEL);
		if (!heading_level_attr.empty()) {
			heading_level = ParseIntAttribute(Vocab::ATTR_HEADING_LEVEL, heading_level_attr);
		} else if (level > 0 && level <= 6) {
			heading_level = level;
		}
		// Clamp to valid range
		if (heading_level < 1)
			heading_level = 1;
		if (heading_level > 6)
			heading_level = 6;
		result = string(heading_level, '#') + " " + content + "\n\n";
	} else if (element_type == Vocab::TYPE_PLAIN) {
		// Pandoc's Plain: a text run with NO paragraph semantics, so no blank line
		// after it. That single newline is the entire difference between a tight
		// list item and a loose one on the way out (#60) -- a `plain` followed by a
		// sublist renders `- a\n  - b`, while a `paragraph` in the same position
		// renders `- a\n\n  - b`, which is a loose list.
		//
		// Spacing is NOT where tightness lives. A `plain` renders with the same
		// blank line as a paragraph, because at top level or beside a caption it
		// must not merge with what follows -- `x\nnext` is one paragraph in
		// CommonMark. Tightness is decided in the LIST ITEM below, which is the only
		// place that knows whether the run and its sibling belong to a tight list.
		//
		// The first attempt put the single newline here and guarded the merge in the
		// range renderer. That guard could not work: several branches emit and
		// `continue` before reaching it, and the caption branch is one of them.
		result = content + "\n\n";
	} else if (element_type == Vocab::TYPE_PARAGRAPH) {
		// Plain paragraph
		result = content + "\n\n";
	} else if (element_type == Vocab::TYPE_CODE) {
		// Fenced code block
		string language = GetAttribute(attributes, "language");
		result = "```" + language + "\n" + content + "\n```\n\n";
	} else if (element_type == Vocab::TYPE_BLOCKQUOTE) {
		// Block quote - add > prefix to each line.
		// Producers emit the quote's blocks as a Pandoc array rather than as text
		// (the released duck_block_utils and its main both do), and prefixing that
		// verbatim put raw AST into the document behind a '>'.
		string source = content;
		if (encoding == Vocab::ENCODING_JSON) {
			string extracted = ExtractPandocBlocksText(parser, content);
			if (!extracted.empty()) {
				source = extracted;
			}
		}
		string quoted;
		std::istringstream iss(source);
		std::string line;
		while (std::getline(iss, line)) {
			quoted += "> " + line + "\n";
		}
		result = quoted + "\n";
	} else if (element_type == Vocab::TYPE_LIST) {
		// List - content is JSON encoded
		if (encoding == "json" && content.length() > 2 && content[0] == '[') {
			// `list_type` is CANONICAL for orderedness; `ordered` is a legacy
			// alias. A boolean cannot grow, and the type set already wants values
			// -- definition and navigation lists -- that `ordered=false` can only
			// say a list is not. Both producers emit both names, so this decides
			// only which one wins when they disagree.
			const string list_type = GetAttribute(attributes, Vocab::ATTR_LIST_TYPE);
			const bool ordered = !list_type.empty() ? (list_type == Vocab::LIST_TYPE_ORDERED)
			                                        : (GetAttribute(attributes, Vocab::ATTR_ORDERED_LEGACY) == "true");
			int start = 1;
			string start_str = GetAttribute(attributes, "start");
			const bool start_from_attribute = !start_str.empty();
			if (start_from_attribute) {
				start = ParseIntAttribute("start", start_str);
			}

			auto items = ParseJsonListItems(parser, content);
			if (items.empty()) {
				ParsePandocListItems(parser, content, items, start, start_from_attribute);
			}
			int item_num = start;
			for (const auto &item : items) {
				if (ordered) {
					result += std::to_string(item_num++) + ". " + item + "\n";
				} else {
					result += "- " + item + "\n";
				}
			}
			result += "\n";
		} else {
			result = content + "\n\n";
		}
	} else if (element_type == Vocab::TYPE_TABLE) {
		// Table - content is JSON encoded
		vector<string> headers;
		vector<vector<string>> rows;
		bool parsed = false;

		if (encoding == "json") {
			// Try standard {"headers": [...], "rows": [...]} format first
			if (content.find("\"headers\"") != string::npos) {
				ParseJsonTable(parser, content, headers, rows);
				parsed = !headers.empty();
			}
			// Try Pandoc table format if standard format didn't work
			if (!parsed && IsPandocTableFormat(parser, content)) {
				ParsePandocTable(parser, content, headers, rows);
				parsed = !headers.empty() || !rows.empty();
			}
		}

		if (parsed && (!headers.empty() || !rows.empty())) {
			// Determine column count
			size_t col_count = headers.size();
			if (col_count == 0 && !rows.empty()) {
				col_count = rows[0].size();
			}

			// Render headers (or empty header row if we only have body rows)
			result = "|";
			if (!headers.empty()) {
				for (const auto &h : headers) {
					result += " " + h + " |";
				}
			} else {
				for (size_t i = 0; i < col_count; i++) {
					result += " |";
				}
			}
			result += "\n|";
			for (size_t i = 0; i < col_count; i++) {
				result += "---|";
			}
			result += "\n";
			for (const auto &row : rows) {
				result += "|";
				for (const auto &cell : row) {
					result += " " + cell + " |";
				}
				result += "\n";
			}
			result += "\n";
		} else {
			result = content + "\n\n";
		}
	} else if (element_type == Vocab::TYPE_HR) {
		result = "---\n\n";
	} else if (element_type == Vocab::TYPE_LIST_ITEM) {
		// List item - render with bullet prefix
		// Check if ordered from attributes
		bool ordered = GetAttribute(attributes, Vocab::ATTR_ORDERED_LEGACY) == "true";
		string item_num = GetAttribute(attributes, "item_number");
		if (ordered && !item_num.empty()) {
			result = item_num + ". " + content + "\n";
		} else {
			result = "- " + content + "\n";
		}
	} else if (element_type == Vocab::TYPE_IMAGE) {
		// Image: ![alt](src "title")
		string src = GetAttribute(attributes, "src");
		string alt = GetAttribute(attributes, "alt");
		// Fall back to content as alt text if alt attribute is empty
		if (alt.empty() && !content.empty()) {
			alt = content;
		}
		string title = GetAttribute(attributes, "title");
		result = "![" + alt + "](" + src;
		if (!title.empty()) {
			result += " \"" + title + "\"";
		}
		result += ")\n\n";
	} else if (element_type == Vocab::TYPE_RAW || element_type == "html" || element_type == "md:html_block") {
		// Raw content - output as-is
		result = content + "\n\n";
	} else if (element_type == Vocab::TYPE_DIV || element_type == Vocab::TYPE_SECTION ||
	           element_type == Vocab::TYPE_FIGURE || element_type == Vocab::TYPE_CAPTION) {
		// A container's children are usually its content, and they render
		// themselves -- so the marker emits nothing and no stray blank line.
		//
		// But a container whose only child is a text run carries that text in its
		// OWN content (spec 6.0), so dropping content unconditionally lost it. The
		// same rule had list_item dropping the text of every tight item; fixing it
		// only there left this in four more places, which is why the sentinel
		// sweep found them and a round trip never could.
		if (content.empty()) {
			result = "";
		} else if (element_type == Vocab::TYPE_CAPTION) {
			// Marked as a caption, as the scoped form is.
			result = "*" + content + "*\n\n";
		} else {
			result = content + "\n\n";
		}
	} else if (element_type == Vocab::TYPE_LINEBLOCK) {
		// Preserved line breaks. Markdown has no line-block syntax, and a bare
		// newline inside a paragraph is a soft break that collapses to a space --
		// so terminate each line with a hard break instead.
		std::istringstream iss(content);
		string line;
		vector<string> lines;
		while (std::getline(iss, line)) {
			// Trailing whitespace is stripped before the break is re-added: a rich
			// line block arrives as a rendered inline run whose `linebreak`
			// children have ALREADY emitted "  \n", and joining that again gave
			// four trailing spaces. A plain-text one has none to strip, so it is
			// unaffected.
			while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
				line.pop_back();
			}
			lines.push_back(line);
		}
		for (size_t i = 0; i < lines.size(); i++) {
			result += lines[i];
			if (i + 1 < lines.size()) {
				result += "  \n";
			}
		}
		result += "\n\n";
	} else if (element_type == Vocab::TYPE_DEFLIST) {
		// Definition list (Markdown Extra / Pandoc syntax).
		//
		// GATED ON encoding, like blockquote, list and table. It was not, and that made
		// deflist the one block type whose content was reinterpreted as Pandoc JSON
		// whatever the producer declared -- so a deflist saying encoding='text' whose
		// text happened to parse as [[[term],[defs]],...] was silently replaced by the
		// extracted text instead of emitted as written (#77).
		//
		// The gate matches the PRODUCER's own contract rather than being our
		// invention: duck_block_utils checks the same thing before treating a deflist
		// as Pandoc (pandoc_block_convert.cpp:1941-42 and :2484-85, both
		// `... == BlockTypes::TYPE_DEFLIST && GetElementStringField(...) ==
		// BlockTypes::ENCODING_JSON`). So nothing a real producer emits stops being
		// interpreted; only content that was never claimed to be JSON does.
		//
		// Nothing OUR reader emits is affected: cmark does not parse definition lists,
		// so TYPE_DEFLIST only ever arrives from an external producer -- verified, we
		// have exactly one mention of it and it is this consuming branch.
		vector<std::pair<string, vector<string>>> entries;
		if (encoding == Vocab::ENCODING_JSON && ParseDefinitionList(parser, content, entries)) {
			for (const auto &entry : entries) {
				result += entry.first + "\n";
				for (const auto &definition : entry.second) {
					result += ": " + definition + "\n";
				}
				result += "\n";
			}
		} else {
			result = content + "\n\n";
		}
	} else if (element_type == Vocab::TYPE_PAGE) {
		// A physical pagination boundary, carrying no content. Markdown has no
		// page-break syntax, and a `---` here would read as a thematic break (or,
		// at the top of a document, as frontmatter delimiters), so leave the same
		// kind of greppable trace `generic` does rather than lose the boundary.
		string page_number = SanitizeForHtmlComment(GetAttribute(attributes, "page_number"));
		if (page_number.empty()) {
			result = "<!-- page break -->\n\n";
		} else {
			result = "<!-- page break: " + page_number + " -->\n\n";
		}
	} else if (element_type == Vocab::TYPE_GENERIC) {
		// "Structurally valid, type not in this vocabulary." The content is a
		// verbatim source fragment that would be noise in a markdown document, but
		// dropping it silently is what `generic` exists to prevent -- so name the
		// construct and leave a trace a reader can grep for.
		string source_type = SanitizeForHtmlComment(GetAttribute(attributes, Vocab::ATTR_SOURCE_TYPE));
		if (source_type.empty()) {
			result = "<!-- unsupported block -->\n\n";
		} else {
			result = "<!-- unsupported block: " + source_type + " -->\n\n";
		}
	} else {
		// Unknown block type - output content as paragraph
		result = content + "\n\n";
	}

	return result;
}

//===--------------------------------------------------------------------===//
// RenderDuckBlockToMarkdown (unified block/inline rendering)
//===--------------------------------------------------------------------===//

string DuckBlockFunctions::RenderDuckBlockToMarkdown(JSONTreeParser &parser, const string &kind,
                                                     const string &element_type, const string &content, int32_t level,
                                                     const string &encoding, const Value &attributes) {
	if (kind == Vocab::KIND_BLOCK) {
		// Delegate to block rendering
		return RenderBlockElementToMarkdown(parser, element_type, content, level, encoding, attributes);
	} else if (kind == Vocab::KIND_INLINE) {
		// Use inline rendering
		return RenderInlineElementToMarkdown(element_type, content, attributes);
	} else {
		// Unknown kind - try to guess based on element_type
		// Block types
		if (element_type == Vocab::TYPE_HEADING || element_type == Vocab::TYPE_PARAGRAPH ||
		    element_type == Vocab::TYPE_BLOCKQUOTE || element_type == Vocab::TYPE_LIST ||
		    element_type == Vocab::TYPE_TABLE || element_type == Vocab::TYPE_HR ||
		    element_type == Vocab::TYPE_METADATA || element_type == "frontmatter" || element_type == Vocab::TYPE_CODE ||
		    element_type == Vocab::TYPE_IMAGE) {
			return RenderBlockElementToMarkdown(parser, element_type, content, level, encoding, attributes);
		}
		// Assume inline otherwise
		return RenderInlineElementToMarkdown(element_type, content, attributes);
	}
}

//===--------------------------------------------------------------------===//
// Kind filtering (duck_block `kind` is an open discriminator)
//===--------------------------------------------------------------------===//

// The seven core duck_block fields. Two optional trailing fields (source_format,
// file_path) may follow, so this is a minimum, not an exact width.
static constexpr idx_t DUCK_BLOCK_FIELD_COUNT = Vocab::ELEMENT_ORDER_IDX + 1;

// Whether a block renders the content it is given. Three declared block types do
// not: `hr` and `page_break` are rules with no content slot, and `generic`
// deliberately emits a naming comment instead of a foreign-format fragment.
//
// This matters because a block ABSORBS the inline run that follows it. Absorbing
// into a type that then discards the result does not merely lose formatting, it
// DELETES the run: `hr` followed by "Section two" rendered the rule and dropped
// the sentence. The old COPY path got this right by accident, one element at a
// time, and routing it through this renderer is what surfaced it.
//
// Kept as an explicit set rather than derived, because "does this branch use its
// argument" is not a question the code can ask itself. The set is verified
// empirically in scripts/check_conformance.py against EVERY declared type, so a
// new content-discarding type fails there rather than silently eating a run.
static bool BlockAbsorbsInlineRun(const string &element_type) {
	return !(element_type == Vocab::TYPE_HR || element_type == Vocab::TYPE_PAGE || element_type == Vocab::TYPE_GENERIC);
}

static string DuckBlockKind(const Value &block_value) {
	if (block_value.IsNull()) {
		return "";
	}
	auto &fields = StructValue::GetChildren(block_value);
	if (fields.size() < DUCK_BLOCK_FIELD_COUNT || fields[Vocab::KIND_IDX].IsNull()) {
		return "";
	}
	return fields[Vocab::KIND_IDX].ToString();
}

// `level` is structural nesting DEPTH, so a top-level element has depth 1 and
// records NULL because it is not nested. Reading NULL as 0 also makes a scope
// walk work, but it puts a top-level element one level shallower than its
// siblings that do carry a level, so an element at level 1 next to one at NULL
// reads as its child rather than its sibling.
static constexpr int32_t DUCK_BLOCK_TOP_LEVEL = 1;

static int32_t DuckBlockLevel(const Value &block_value) {
	if (block_value.IsNull()) {
		return DUCK_BLOCK_TOP_LEVEL;
	}
	auto &fields = StructValue::GetChildren(block_value);
	if (fields.size() < DUCK_BLOCK_FIELD_COUNT || fields[Vocab::LEVEL_IDX].IsNull()) {
		return DUCK_BLOCK_TOP_LEVEL;
	}
	return fields[Vocab::LEVEL_IDX].GetValue<int32_t>();
}

// `kind` gained a third value ('value', carrying document metadata) after this
// writer was written, and more may follow. Allowlist what we know how to render:
// a blocklist ("not inline, so it must be a block") turns every future kind into
// body prose, which is how document titles ended up appended to documents.
// An empty kind is legacy data and keeps its element_type-based guess.
static bool IsRenderableKind(const string &kind) {
	return kind.empty() || kind == Vocab::KIND_BLOCK || kind == Vocab::KIND_INLINE;
}

// A non-renderable element's children must go with it. Metadata nests by level
// exactly as block containers do -- the scope runs from the marker to the first
// element back at its own level -- and those children are ordinary kinds:
// MetaInlines carries kind='inline' children, MetaBlocks carries kind='block'
// ones. Skipping only the marker leaves them to render as stray prose.
// Returns the index just past the scope opened at `marker_index`, never past `end`.
static idx_t SkipElementScope(const vector<Value> &children, idx_t marker_index, idx_t end) {
	const int32_t scope_level = DuckBlockLevel(children[marker_index]);
	idx_t i = marker_index + 1;
	while (i < end && DuckBlockLevel(children[i]) > scope_level) {
		i++;
	}
	return i;
}

// Caption scopes are rendered by recursing into the range they open. Each level
// consumes at least one element, so depth is bounded by the list length -- which
// an adversarial document controls. Past this depth a caption is treated as a
// plain container: its children still render, just without the emphasis.
static constexpr int MAX_DUCK_BLOCK_NESTING = 64;

//===--------------------------------------------------------------------===//
// RenderDuckBlocksToMarkdown (list of duck_blocks)
//===--------------------------------------------------------------------===//

static string DuckBlockElementType(const Value &block_value) {
	if (block_value.IsNull()) {
		return "";
	}
	auto &fields = StructValue::GetChildren(block_value);
	if (fields.size() < DUCK_BLOCK_FIELD_COUNT || fields[Vocab::ELEMENT_TYPE_IDX].IsNull()) {
		return "";
	}
	return fields[Vocab::ELEMENT_TYPE_IDX].ToString();
}

// Read an element's own content, for the walk. Absent is spelled two ways --
// the builders write NULL, the Pandoc reader writes an empty string.
static string DuckBlockContent(const Value &block_value) {
	if (block_value.IsNull()) {
		return "";
	}
	auto &fields = StructValue::GetChildren(block_value);
	if (fields.size() < DUCK_BLOCK_FIELD_COUNT || fields[Vocab::CONTENT_IDX].IsNull()) {
		return "";
	}
	return fields[Vocab::CONTENT_IDX].ToString();
}

// Read an attribute straight off a duck_block Value, for the walk, which has the
// element rather than its already-extracted attributes map.
static string GetAttributeOf(const Value &block_value, const string &key) {
	if (block_value.IsNull()) {
		return "";
	}
	auto &fields = StructValue::GetChildren(block_value);
	if (fields.size() < DUCK_BLOCK_FIELD_COUNT) {
		return "";
	}
	return DuckBlockFunctions::GetAttribute(fields[Vocab::ATTRIBUTES_IDX], key);
}

// Ordered-list markers. Pandoc carries the style and delimiter separately from
// the number, so a list starting at iii) round-trips as iii) rather than 3.
static string RomanNumeral(int n, bool upper) {
	static const int values[] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
	static const char *lower[] = {"m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i"};
	static const char *upper_s[] = {"M", "CM", "D", "CD", "C", "XC", "L", "XL", "X", "IX", "V", "IV", "I"};
	string out;
	for (size_t i = 0; i < 13 && n > 0; i++) {
		while (n >= values[i]) {
			out += upper ? upper_s[i] : lower[i];
			n -= values[i];
		}
	}
	return out;
}

static string AlphaNumeral(int n, bool upper) {
	string out;
	while (n > 0) {
		const int rem = (n - 1) % 26;
		out.insert(out.begin(), static_cast<char>((upper ? 'A' : 'a') + rem));
		n = (n - 1) / 26;
	}
	return out;
}

static string FormatListMarker(int number, const string &style, const string &delim) {
	string text;
	if (number < 1) {
		number = 1;
	}
	if (style == "LowerRoman") {
		text = RomanNumeral(number, false);
	} else if (style == "UpperRoman") {
		text = RomanNumeral(number, true);
	} else if (style == "LowerAlpha") {
		text = AlphaNumeral(number, false);
	} else if (style == "UpperAlpha") {
		text = AlphaNumeral(number, true);
	} else {
		// Decimal, Example, DefaultStyle, and anything this build does not know.
		text = std::to_string(number);
	}
	return text + ((delim == "OneParen" || delim == "TwoParens") ? ")" : ".") + " ";
}

// Prefix the first line with `marker` and indent the rest to line up under it.
// Blank lines stay genuinely blank rather than becoming trailing whitespace.
static string IndentContinuation(const string &text, const string &marker) {
	const string pad(marker.size(), ' ');
	string out;
	std::istringstream iss(text);
	string line;
	bool first = true;
	while (std::getline(iss, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (first) {
			out += marker + line + "\n";
			first = false;
		} else if (line.empty()) {
			out += "\n";
		} else {
			out += pad + line + "\n";
		}
	}
	if (first) {
		// Nothing to indent; still emit the marker so the item is not lost.
		out += marker + "\n";
	}
	return out;
}

// Render [begin, end) of a duck_block list. Recursive, because a `caption` has to
// be rendered as a unit before it can be decorated -- markdown has no caption
// syntax, so the alternative is prose no reader can tell from body text.
static string RenderDuckBlockRange(JSONTreeParser &parser, const vector<Value> &list_children, idx_t begin, idx_t end,
                                   int depth) {
	string result;
	bool last_was_inline = false;

	for (idx_t i = begin; i < end;) {
		const auto &block_value = list_children[i];
		if (block_value.IsNull()) {
			i++;
			continue;
		}

		auto &struct_children = StructValue::GetChildren(block_value);
		if (struct_children.size() < DUCK_BLOCK_FIELD_COUNT) {
			i++;
			continue;
		}

		// duck_block: kind, element_type, content, level, encoding, attributes, element_order
		string kind = struct_children[Vocab::KIND_IDX].IsNull() ? "" : struct_children[Vocab::KIND_IDX].ToString();

		// Metadata and any future kind are not document content; drop the whole scope.
		if (!IsRenderableKind(kind)) {
			i = SkipElementScope(list_children, i, end);
			continue;
		}
		string element_type = struct_children[Vocab::ELEMENT_TYPE_IDX].IsNull()
		                          ? ""
		                          : struct_children[Vocab::ELEMENT_TYPE_IDX].ToString();
		string content =
		    struct_children[Vocab::CONTENT_IDX].IsNull() ? "" : struct_children[Vocab::CONTENT_IDX].ToString();
		int32_t level =
		    struct_children[Vocab::LEVEL_IDX].IsNull() ? 0 : struct_children[Vocab::LEVEL_IDX].GetValue<int32_t>();
		string encoding =
		    struct_children[Vocab::ENCODING_IDX].IsNull() ? "text" : struct_children[Vocab::ENCODING_IDX].ToString();
		Value attributes = struct_children[Vocab::ATTRIBUTES_IDX];

		// Spec 1.2 made `list` and `blockquote` structural: the container carries
		// no content of its own and its blocks follow as children by level. The
		// older shape packed the whole thing into JSON content and is still what
		// released producers emit, so the block renderer keeps handling that and
		// these paths only take over when there is no content to render.
		if (kind == Vocab::KIND_BLOCK && content.empty() && depth < MAX_DUCK_BLOCK_NESTING) {
			if (element_type == Vocab::TYPE_LIST) {
				const idx_t scope_end = SkipElementScope(list_children, i, end);
				if (scope_end > i + 1) {
					// Canonical `list_type` first, then the legacy `ordered` alias.
					const string list_type = GetAttributeOf(block_value, Vocab::ATTR_LIST_TYPE);
					const bool ordered = !list_type.empty()
					                         ? (list_type == Vocab::LIST_TYPE_ORDERED)
					                         : (GetAttributeOf(block_value, Vocab::ATTR_ORDERED_LEGACY) == "true");
					const string number_style = GetAttributeOf(block_value, "number_style");
					const string number_delim = GetAttributeOf(block_value, "number_delim");
					int number = 1;
					const string start_attr = GetAttributeOf(block_value, "start");
					if (!start_attr.empty()) {
						number = ParseIntAttribute("start", start_attr);
					}
					const int32_t item_level = DuckBlockLevel(list_children[i]) + 1;
					if (last_was_inline) {
						result += "\n\n";
					}

					// A definition list is a `list` whose items are terms and
					// definitions as SIBLINGS at one level, paired by order: a term
					// owns the role='definition' items following it until the next
					// term. One term may own several definitions, which is distinct
					// from one definition of several blocks -- many items versus one
					// item with children. Measured 2026-08-31 off the live producer
					// (duck_block_utils spec 6.1) rather than
					// taken from its description.
					if (list_type == Vocab::LIST_TYPE_DEFINITION) {
						bool first_term = true;
						for (idx_t j = i + 1; j < scope_end;) {
							if (DuckBlockLevel(list_children[j]) != item_level) {
								const idx_t stray_end = SkipElementScope(list_children, j, scope_end);
								result += RenderDuckBlockRange(parser, list_children, j, stray_end, depth + 1);
								j = stray_end;
								continue;
							}
							const idx_t item_end = SkipElementScope(list_children, j, scope_end);
							string text = item_end > j + 1
							                  ? RenderDuckBlockRange(parser, list_children, j + 1, item_end, depth + 1)
							                  : DuckBlockContent(list_children[j]);
							StringUtil::Trim(text);
							// Anything not marked a definition is a label: an item whose
							// role is missing reads as a term rather than disappearing.
							if (GetAttributeOf(list_children[j], Vocab::ATTR_ROLE) == Vocab::ROLE_DEFINITION) {
								// ":" plus three spaces, so a continuation block lands at
								// four. Two was not enough: a reader takes a 2-space block
								// as a new top-level paragraph, which splits the list in
								// half and strands the block between the pieces. This is
								// also Pandoc's own output width, so marker and pad agree.
								result += IndentContinuation(text, ":   ");
							} else {
								if (!first_term) {
									result += "\n";
								}
								result += text + "\n";
								first_term = false;
							}
							j = item_end;
						}
						result += "\n";
						last_was_inline = false;
						i = scope_end;
						continue;
					}

					// LOOSE OR TIGHT, decided from the encoding rather than guessed.
					// An item holding a `paragraph` is Pandoc's Para and the list is
					// loose; an item carrying its text in `content`, or holding a
					// `plain`, is tight (#60). Before the reader emitted `plain` this
					// distinction did not survive the read at all, so the writer had
					// nothing to key on and rendered every list tight -- which
					// cancelled the reader's opposite collapse and kept the round-trip
					// suite green while both halves were broken.
					bool loose = false;
					for (idx_t j = i + 1; j < scope_end && !loose; j++) {
						if (DuckBlockLevel(list_children[j]) == item_level + 1 &&
						    DuckBlockElementType(list_children[j]) == Vocab::TYPE_PARAGRAPH) {
							loose = true;
						}
					}

					bool first_item = true;
					for (idx_t j = i + 1; j < scope_end;) {
						if (DuckBlockLevel(list_children[j]) != item_level) {
							// Not an item -- a producer we do not recognise, or a
							// shape from a spec we have not seen. Render it anyway
							// rather than advancing past it: this decides whether
							// the child EXISTS, and that must not be keyed on it
							// matching a shape we know. Losing the list structure
							// costs formatting; dropping it costs the text.
							const idx_t stray_end = SkipElementScope(list_children, j, scope_end);
							result += RenderDuckBlockRange(parser, list_children, j, stray_end, depth + 1);
							j = stray_end;
							continue;
						}
						const idx_t item_end = SkipElementScope(list_children, j, scope_end);
						// An item's children are its content when it has any; its own
						// content is its content when it does not. Rendering only the
						// child scope dropped the text of every item that carried it
						// directly, which is the shape the live producer emits.
						string inner = item_end > j + 1
						                   ? RenderDuckBlockRange(parser, list_children, j + 1, item_end, depth + 1)
						                   : DuckBlockContent(list_children[j]);
						StringUtil::Trim(inner);
						// TIGHT ITEM: the run and its first sibling have no blank line
						// between them -- that is what `- a\n  - b` means, and a `plain`
						// child is how the reader says the run was a Plain (#60).
						//
						// ONLY THE FIRST BOUNDARY is collapsed, never every "\n\n" in the
						// item. An outer tight item may hold a sublist that is itself
						// LOOSE, and flattening those would destroy the inner list's
						// spacing to fix the outer one's:
						//
						//     - a          outer tight
						//       - x        inner LOOSE -- its own blank lines must stay
						//
						//       - y
						if (!loose && item_end > j + 1 &&
						    DuckBlockElementType(list_children[j + 1]) == Vocab::TYPE_PLAIN) {
							const auto brk = inner.find("\n\n");
							if (brk != string::npos) {
								inner.replace(brk, 2, "\n");
							}
						}
						const string marker = ordered ? FormatListMarker(number++, number_style, number_delim) : "- ";
						if (loose && !first_item) {
							result += "\n";
						}
						first_item = false;
						result += IndentContinuation(inner, marker);
						j = item_end;
					}
					result += "\n";
					last_was_inline = false;
					i = scope_end;
					continue;
				}
			} else if (element_type == Vocab::TYPE_LIST_ITEM) {
				// An item met without its container -- render it as a bullet rather
				// than dropping the blocks it holds.
				const idx_t scope_end = SkipElementScope(list_children, i, end);
				if (scope_end > i + 1) {
					string inner = RenderDuckBlockRange(parser, list_children, i + 1, scope_end, depth + 1);
					StringUtil::Trim(inner);
					if (last_was_inline) {
						result += "\n\n";
					}
					result += IndentContinuation(inner, "- ") + "\n";
					last_was_inline = false;
					i = scope_end;
					continue;
				}
			} else if (element_type == Vocab::TYPE_BLOCKQUOTE) {
				const idx_t scope_end = SkipElementScope(list_children, i, end);
				if (scope_end > i + 1) {
					string inner = RenderDuckBlockRange(parser, list_children, i + 1, scope_end, depth + 1);
					StringUtil::Trim(inner);
					if (last_was_inline) {
						result += "\n\n";
					}
					std::istringstream iss(inner);
					string line;
					while (std::getline(iss, line)) {
						if (!line.empty() && line.back() == '\r') {
							line.pop_back();
						}
						result += line.empty() ? ">\n" : "> " + line + "\n";
					}
					result += "\n";
					last_was_inline = false;
					i = scope_end;
					continue;
				}
			}
		}

		// A figure's caption is often just the image's alt text -- markdown's own
		// `![alt](src)` carries it, and a reader infers BOTH a figure and its
		// caption from a lone image. Emitting the caption separately then says it
		// twice, and re-reading that output yields the inferred caption plus a
		// stray italic paragraph, so another copy accrues on every pass.
		//
		// Suppressed only when the caption IS the alt. A caption saying something
		// the alt does not is information the image syntax cannot carry.
		if (kind == Vocab::KIND_BLOCK && element_type == Vocab::TYPE_FIGURE && content.empty() &&
		    depth < MAX_DUCK_BLOCK_NESTING) {
			const idx_t scope_end = SkipElementScope(list_children, i, end);
			const int32_t child_level = DuckBlockLevel(list_children[i]) + 1;
			idx_t caption_begin = scope_end, caption_end = scope_end;
			string alt;
			for (idx_t j = i + 1; j < scope_end; j++) {
				const string child_type = DuckBlockElementType(list_children[j]);
				if (child_type == Vocab::TYPE_CAPTION && DuckBlockLevel(list_children[j]) == child_level &&
				    caption_begin == scope_end) {
					caption_begin = j;
					caption_end = SkipElementScope(list_children, j, scope_end);
				} else if (alt.empty() && child_type == Vocab::TYPE_IMAGE) {
					alt = GetAttributeOf(list_children[j], "alt");
					if (alt.empty()) {
						alt = DuckBlockContent(list_children[j]);
					}
				}
			}
			if (caption_begin < scope_end && !alt.empty()) {
				string caption_text =
				    caption_end > caption_begin + 1
				        ? RenderDuckBlockRange(parser, list_children, caption_begin + 1, caption_end, depth + 1)
				        : DuckBlockContent(list_children[caption_begin]);
				StringUtil::Trim(caption_text);
				if (caption_text == alt) {
					if (last_was_inline) {
						result += "\n\n";
					}
					result += RenderDuckBlockRange(parser, list_children, i + 1, caption_begin, depth + 1);
					result += RenderDuckBlockRange(parser, list_children, caption_end, scope_end, depth + 1);
					last_was_inline = false;
					i = scope_end;
					continue;
				}
			}
		}

		// A caption's children are its content, so render the scope it opens and
		// mark the result. Position is the producer's choice -- a figure caption
		// follows its content, a disclosure summary precedes its body -- so this
		// renders in place and imposes no order of its own.
		if (kind == Vocab::KIND_BLOCK && element_type == Vocab::TYPE_CAPTION && depth < MAX_DUCK_BLOCK_NESTING) {
			idx_t scope_end = SkipElementScope(list_children, i, end);
			// Children are the caption's text when it has any; its own content is
			// its text when it does not. Without the fallback this branch consumed
			// a childless caption and emitted nothing, which also shadowed the leaf
			// renderer -- so the text was lost twice over.
			string inner = scope_end > i + 1 ? RenderDuckBlockRange(parser, list_children, i + 1, scope_end, depth + 1)
			                                 : DuckBlockContent(list_children[i]);
			StringUtil::Trim(inner);
			if (!inner.empty()) {
				if (last_was_inline) {
					result += "\n\n";
				}
				// Emphasis cannot span a paragraph break, so only a single-block
				// caption gets marked; a multi-block one is emitted as it stands.
				if (inner.find("\n\n") == string::npos) {
					result += "*" + inner + "*";
				} else {
					result += inner;
				}
				result += "\n\n";
			}
			last_was_inline = false;
			i = scope_end;
			continue;
		}

		// An element with no content of its own carries it as kind='inline'
		// children -- a paragraph whose text is a run of inlines, a bold whose
		// text nests deeper. Walking those flat rendered the marker's empty
		// content as an element in its own right, which put a blank paragraph
		// ahead of the text and closed "**" around nothing.
		//
		// Blocks and inlines sit on SEPARATE level scales: the spec puts a
		// top-level inline at level 1 while a top-level block records NULL. And
		// producers disagree about where a block's inline run begins -- the
		// builders emit level 1 (MEASURED 2026-08-31, duck_block_utils spec 6.1);
		// panduck reported the same of its four readers -- RELAYED 2026-08-31,
		// never verified from this side. The Pandoc path
		// emits 2. So a block takes the whole contiguous inline run that follows
		// it rather than trusting the level, which is the only rule that reads
		// both; among inlines the level IS the nesting, so a wrapper there takes
		// what is deeper than itself.
		idx_t scope_end;
		if (kind == Vocab::KIND_BLOCK && BlockAbsorbsInlineRun(element_type)) {
			scope_end = i + 1;
			while (scope_end < end && DuckBlockKind(list_children[scope_end]) == Vocab::KIND_INLINE) {
				scope_end++;
			}
		} else if (kind == Vocab::KIND_BLOCK) {
			// Discards its content, so it takes nothing: the run that follows is
			// rendered in its own right rather than absorbed and dropped.
			scope_end = i + 1;
		} else {
			scope_end = SkipElementScope(list_children, i, end);
		}
		bool consumed_children = false;
		// A HEADING absorbs its inline run even though it has content of its own:
		// its content is the flattened title and the children are the formatted
		// text, so the children win for rendering. Every other element treats
		// non-empty content as "this element already carries its text".
		//
		// DEEPER, not merely following. Requiring the run to be nested below the
		// heading is what separates "these inlines are my children" from "an
		// inline run happens to come after me" -- a hand-built list can put a bare
		// run after a heading, and absorbing it would render the run as the title
		// and destroy the real one. That is the `hr` defect again: taking a run
		// that is not yours and discarding your own content with it. Caught by an
		// existing COPY test rather than by reasoning, which is why this condition
		// is here and not in the first version.
		const bool heading_with_children = element_type == Vocab::TYPE_HEADING && i + 1 < end &&
		                                   DuckBlockLevel(list_children[i + 1]) > DuckBlockLevel(list_children[i]);
		if ((content.empty() || heading_with_children) && scope_end > i + 1 && depth < MAX_DUCK_BLOCK_NESTING &&
		    DuckBlockKind(list_children[i + 1]) == Vocab::KIND_INLINE) {
			content = RenderDuckBlockRange(parser, list_children, i + 1, scope_end, depth + 1);
			consumed_children = true;
		}

		bool is_inline = (kind == Vocab::KIND_INLINE);

		// Handle inline-to-block transition: add paragraph break
		if (last_was_inline && !is_inline) {
			result += "\n\n";
		}

		result += DuckBlockFunctions::RenderDuckBlockToMarkdown(parser, kind, element_type, content, level, encoding,
		                                                        attributes);
		last_was_inline = is_inline;
		i = consumed_children ? scope_end : i + 1;
	}

	return result;
}

//===--------------------------------------------------------------------===//
// Document metadata -> YAML frontmatter
//===--------------------------------------------------------------------===//

// Every scalar is double-quoted. It costs a little noise and buys the guarantee
// that a value out of an untrusted document -- one containing a newline and a
// `---`, say -- cannot forge frontmatter structure around itself.
static string YamlQuote(const string &text) {
	string out = "\"";
	for (unsigned char c : text) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c < 0x20) {
				out += ' ';
			} else {
				out += static_cast<char>(c);
			}
		}
	}
	out += "\"";
	return out;
}

static string YamlKey(const string &key) {
	if (key.empty()) {
		return YamlQuote(key);
	}
	for (size_t i = 0; i < key.size(); i++) {
		const unsigned char c = key[i];
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
		                (i > 0 && ((c >= '0' && c <= '9') || c == '-' || c == '.'));
		if (!ok) {
			return YamlQuote(key);
		}
	}
	return key;
}

// A YAML literal block scalar, for metadata whose value is block content.
static string YamlBlockScalar(const string &text, int indent) {
	string out = "|\n";
	const string pad(indent + 2, ' ');
	std::istringstream iss(text);
	string line;
	while (std::getline(iss, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		out += pad + line + "\n";
	}
	return out;
}

// Emit the YAML for the kind='value' scope opening at `i`, which nests by level
// exactly as block containers do. Returns the index just past that scope.
static idx_t RenderMetaValue(JSONTreeParser &parser, const vector<Value> &children, idx_t i, idx_t end, int indent,
                             bool in_sequence, string &out) {
	const idx_t scope_end = SkipElementScope(children, i, end);
	auto &fields = StructValue::GetChildren(children[i]);
	const string element_type =
	    fields[Vocab::ELEMENT_TYPE_IDX].IsNull() ? "" : fields[Vocab::ELEMENT_TYPE_IDX].ToString();
	const string content = fields[Vocab::CONTENT_IDX].IsNull() ? "" : fields[Vocab::CONTENT_IDX].ToString();
	const string key = DuckBlockFunctions::GetAttribute(fields[Vocab::ATTRIBUTES_IDX], "key");

	// The version marker deliberately carries no key, which is what keeps a
	// storage/exchange stamp out of the document's own metadata on export.
	if (element_type == Vocab::VALUE_VERSION) {
		return scope_end;
	}

	const string prefix = string(indent, ' ') + (in_sequence ? "- " : "");
	const string label = key.empty() ? "" : YamlKey(key) + ":";

	if (element_type == Vocab::VALUE_LIST || element_type == Vocab::VALUE_MAP) {
		out += prefix + (label.empty() ? string("-") : label) + "\n";
		const bool children_in_sequence = (element_type == Vocab::VALUE_LIST);
		idx_t j = i + 1;
		while (j < scope_end) {
			if (DuckBlockKind(children[j]) != Vocab::KIND_VALUE) {
				j++;
				continue;
			}
			j = RenderMetaValue(parser, children, j, scope_end, indent + 2, children_in_sequence, out);
		}
		return scope_end;
	}

	string scalar;
	if (element_type == Vocab::VALUE_BOOL) {
		// The one value type emitted bare rather than quoted.
		scalar = (content == "true") ? "true" : "false";
	} else if (element_type == Vocab::VALUE_INLINES) {
		string rendered = RenderDuckBlockRange(parser, children, i + 1, scope_end, 0);
		StringUtil::Trim(rendered);
		scalar = YamlQuote(rendered);
	} else if (element_type == Vocab::VALUE_BLOCKS) {
		string rendered = RenderDuckBlockRange(parser, children, i + 1, scope_end, 0);
		StringUtil::Trim(rendered);
		out += prefix + (label.empty() ? string("-") : label) + " " + YamlBlockScalar(rendered, indent);
		return scope_end;
	} else {
		// string, generic, and any value type this build does not know: the content
		// is the value verbatim.
		scalar = YamlQuote(content);
	}

	if (label.empty()) {
		out += prefix + scalar + "\n";
	} else {
		out += prefix + label + " " + scalar + "\n";
	}
	return scope_end;
}

// Build the frontmatter for a document's kind='value' elements. Empty when the
// document has no metadata.
static string RenderDocumentFrontmatter(JSONTreeParser &parser, const vector<Value> &children) {
	string body;
	for (idx_t i = 0; i < children.size();) {
		if (DuckBlockKind(children[i]) != Vocab::KIND_VALUE) {
			i++;
			continue;
		}
		i = RenderMetaValue(parser, children, i, children.size(), 0, false, body);
	}
	if (body.empty()) {
		return "";
	}
	return "---\n" + body + "---\n\n";
}

// True when the list already carries frontmatter of its own, in which case
// synthesising a second block would produce a document with two.
static bool HasFrontmatterBlock(const vector<Value> &children) {
	for (const auto &child : children) {
		if (DuckBlockKind(child) != Vocab::KIND_BLOCK) {
			continue;
		}
		auto &fields = StructValue::GetChildren(child);
		const string element_type =
		    fields[Vocab::ELEMENT_TYPE_IDX].IsNull() ? "" : fields[Vocab::ELEMENT_TYPE_IDX].ToString();
		if (element_type == Vocab::TYPE_METADATA || element_type == "frontmatter") {
			return true;
		}
	}
	return false;
}

string DuckBlockFunctions::RenderDuckBlocksToMarkdown(JSONTreeParser &parser, const Value &blocks_value) {
	if (blocks_value.IsNull() || blocks_value.type().id() != LogicalTypeId::LIST) {
		return "";
	}
	auto &raw_children = ListValue::GetChildren(blocks_value);

	// METADATA IS TAILMATTER in duck_blocks: both metadata homes sit AFTER every
	// body block, and there is no frontmatter position in the vocabulary (ruled
	// 2026-09-02). Markdown is the one output format with a place to put it, and
	// that place is the TOP of the file -- so the block is found by TYPE and
	// hoisted, never by where it sits in the list.
	//
	// Rendering it in list order produced a document that was not merely
	// misordered but destroyed: `# T\n\nBody\n\n---\ntitle: T\n---` re-parses
	// as heading, paragraph, HR, heading. The fence at the bottom of a file is a
	// thematic break, not frontmatter, so the metadata came back as structural
	// noise and the round trip lost it entirely.
	vector<Value> reordered;
	reordered.reserve(raw_children.size());
	idx_t metadata_at = raw_children.size();
	for (idx_t i = 0; i < raw_children.size(); i++) {
		const string type = DuckBlockElementType(raw_children[i]);
		if (metadata_at == raw_children.size() && DuckBlockKind(raw_children[i]) == Vocab::KIND_BLOCK &&
		    (type == Vocab::TYPE_METADATA || type == "frontmatter")) {
			metadata_at = i;
		}
	}
	if (metadata_at < raw_children.size()) {
		reordered.push_back(raw_children[metadata_at]);
		for (idx_t i = 0; i < raw_children.size(); i++) {
			if (i != metadata_at) {
				reordered.push_back(raw_children[i]);
			}
		}
	}
	const vector<Value> &list_children = reordered.empty() ? raw_children : reordered;

	string result;
	// Metadata is not body content, but markdown -- alone among the output formats
	// this vocabulary targets -- has somewhere to put it, so it round-trips as
	// frontmatter instead of being dropped.
	if (!HasFrontmatterBlock(list_children)) {
		result = RenderDocumentFrontmatter(parser, list_children);
	}
	result += RenderDuckBlockRange(parser, list_children, 0, list_children.size(), 0);
	return result;
}

//===--------------------------------------------------------------------===//
// duck_block_to_md - Single duck_block to Markdown
//===--------------------------------------------------------------------===//

void DuckBlockFunctions::RegisterDuckBlockToMdFunction(ExtensionLoader &loader) {
	auto duck_block_type = MarkdownTypes::DuckBlockType();
	auto markdown_type = MarkdownTypes::MarkdownType();

	// duck_block_to_md(block) -> MARKDOWN
	ScalarFunction duck_block_to_md(
	    "duck_block_to_md", {duck_block_type}, markdown_type,
	    [](DataChunk &args, ExpressionState &state, Vector &result) {
		    auto &block_vector = args.data[0];
		    JSONTreeParser parser;

		    for (idx_t i = 0; i < args.size(); i++) {
			    auto block_value = block_vector.GetValue(i);

			    if (block_value.IsNull()) {
				    result.SetValue(i, Value());
				    continue;
			    }

			    // Extract fields from the struct
			    auto &struct_children = StructValue::GetChildren(block_value);
			    if (struct_children.size() < 7) {
				    result.SetValue(i, Value(""));
				    continue;
			    }

			    // duck_block: kind, element_type, content, level, encoding, attributes, element_order
			    string kind =
			        struct_children[Vocab::KIND_IDX].IsNull() ? "" : struct_children[Vocab::KIND_IDX].ToString();
			    string element_type = struct_children[Vocab::ELEMENT_TYPE_IDX].IsNull()
			                              ? ""
			                              : struct_children[Vocab::ELEMENT_TYPE_IDX].ToString();
			    string content =
			        struct_children[Vocab::CONTENT_IDX].IsNull() ? "" : struct_children[Vocab::CONTENT_IDX].ToString();
			    int32_t level = struct_children[Vocab::LEVEL_IDX].IsNull()
			                        ? 0
			                        : struct_children[Vocab::LEVEL_IDX].GetValue<int32_t>();
			    string encoding = struct_children[Vocab::ENCODING_IDX].IsNull()
			                          ? "text"
			                          : struct_children[Vocab::ENCODING_IDX].ToString();
			    Value attributes = struct_children[Vocab::ATTRIBUTES_IDX];

			    string markdown =
			        RenderDuckBlockToMarkdown(parser, kind, element_type, content, level, encoding, attributes);
			    result.SetValue(i, Value(markdown));
		    }
	    });

	// Reaches a throwing depth guard, so v2.0 requires it to declare fallibility
	// or the clean InvalidInputException becomes an InternalException -- which
	// would defeat the guard, whose whole purpose is to turn unbounded recursion
	// into a readable message. Found by following the throwing HELPERS, not the
	// literal throws: the guard is two files away from every one of these.
	duck_block_to_md.SetFallible();
	{
		CreateScalarFunctionInfo info(std::move(duck_block_to_md));
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		FunctionDescription desc;
		desc.parameter_names = {"block"};
		desc.description = "Convert a single duck_block struct to Markdown text.";
		desc.examples = {"duck_block_to_md({'kind': 'container', 'element_type': 'paragraph', 'content': 'hello', "
		                 "'level': 0, 'encoding': 'text', 'attributes': map(), 'element_order': 0})"};
		desc.categories = {"markdown"};
		info.descriptions.push_back(desc);
		loader.RegisterFunction(std::move(info));
	}
}

//===--------------------------------------------------------------------===//
// duck_blocks_to_md - List of duck_blocks to Markdown
//===--------------------------------------------------------------------===//

void DuckBlockFunctions::RegisterDuckBlocksToMdFunction(ExtensionLoader &loader) {
	auto duck_block_type = MarkdownTypes::DuckBlockType();
	auto duck_block_list_type = LogicalType::LIST(duck_block_type);
	auto markdown_type = MarkdownTypes::MarkdownType();

	// duck_blocks_to_md(blocks LIST) -> MARKDOWN
	ScalarFunction duck_blocks_to_md("duck_blocks_to_md", {duck_block_list_type}, markdown_type,
	                                 [](DataChunk &args, ExpressionState &state, Vector &result) {
		                                 auto &blocks_vector = args.data[0];
		                                 JSONTreeParser parser;

		                                 for (idx_t i = 0; i < args.size(); i++) {
			                                 auto blocks_value = blocks_vector.GetValue(i);

			                                 if (blocks_value.IsNull()) {
				                                 result.SetValue(i, Value());
				                                 continue;
			                                 }

			                                 string markdown = RenderDuckBlocksToMarkdown(parser, blocks_value);
			                                 result.SetValue(i, Value(markdown));
		                                 }
	                                 });

	duck_blocks_to_md.SetFallible();
	{
		CreateScalarFunctionInfo info(std::move(duck_blocks_to_md));
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		FunctionDescription desc;
		desc.parameter_names = {"blocks"};
		desc.description = "Convert a list of duck_blocks to Markdown text.";
		desc.examples = {"duck_blocks_to_md([])"};
		desc.categories = {"markdown"};
		info.descriptions.push_back(desc);
		loader.RegisterFunction(std::move(info));
	}
}

//===--------------------------------------------------------------------===//
// duck_blocks_to_sections - Convert duck_blocks to sections format
//===--------------------------------------------------------------------===//

void DuckBlockFunctions::RegisterDuckBlocksToSectionsFunction(ExtensionLoader &loader) {
	auto duck_block_type = MarkdownTypes::DuckBlockType();
	auto duck_block_list_type = LogicalType::LIST(duck_block_type);
	auto markdown_type = MarkdownTypes::MarkdownType();

	// Section struct type: (section_id, level, title, content)
	child_list_t<LogicalType> section_struct_types;
	section_struct_types.push_back(std::make_pair("section_id", LogicalType::VARCHAR));
	section_struct_types.push_back(std::make_pair("section_path", LogicalType::VARCHAR));
	section_struct_types.push_back(std::make_pair("level", LogicalType::INTEGER));
	section_struct_types.push_back(std::make_pair("title", LogicalType::VARCHAR));
	section_struct_types.push_back(std::make_pair("content", markdown_type));

	auto section_struct_type = LogicalType::STRUCT(section_struct_types);
	auto section_list_type = LogicalType::LIST(section_struct_type);

	// duck_blocks_to_sections(blocks LIST) -> LIST(section)
	ScalarFunction duck_blocks_to_sections(
	    "duck_blocks_to_sections", {duck_block_list_type}, section_list_type,
	    [](DataChunk &args, ExpressionState &state, Vector &result) {
		    auto &blocks_vector = args.data[0];
		    JSONTreeParser parser;

		    for (idx_t row_idx = 0; row_idx < args.size(); row_idx++) {
			    auto blocks_value = blocks_vector.GetValue(row_idx);

			    if (blocks_value.IsNull()) {
				    result.SetValue(row_idx, Value());
				    continue;
			    }

			    auto &list_children = ListValue::GetChildren(blocks_value);
			    vector<Value> sections;

			    // Track current section
			    string current_title;
			    int32_t current_level = 0;
			    string current_section_id;
			    vector<string> section_path_parts;

			    // Render a section's content through the SHARED range renderer rather
			    // than element by element. The two walks over a block list have to
			    // agree about how structure renders, and every structural rule --
			    // list items, blockquote nesting, caption emphasis, a block's inline
			    // children -- lives in that renderer. Walking elements individually
			    // here produced a bullet with no text followed by loose prose, while
			    // duck_blocks_to_md produced "- LISTWORD" from the same input.
			    idx_t content_begin = 0;
			    auto render_range = [&](idx_t begin, idx_t end) -> string {
				    return end > begin ? RenderDuckBlockRange(parser, list_children, begin, end, 0) : string();
			    };

			    auto flush_section = [&](const string &body) {
				    if (!current_title.empty() || !body.empty()) {
					    // Build section path
					    string section_path;
					    for (size_t i = 0; i < section_path_parts.size(); i++) {
						    if (i > 0)
							    section_path += " > ";
						    section_path += section_path_parts[i];
					    }

					    child_list_t<Value> section_values;
					    section_values.push_back(std::make_pair("section_id", Value(current_section_id)));
					    section_values.push_back(std::make_pair("section_path", Value(section_path)));
					    section_values.push_back(std::make_pair("level", Value::INTEGER(current_level)));
					    section_values.push_back(std::make_pair("title", Value(current_title)));
					    section_values.push_back(std::make_pair("content", Value(body)));

					    sections.push_back(Value::STRUCT(section_values));
				    }
			    };

			    for (idx_t i = 0; i < list_children.size();) {
				    const auto &block_value = list_children[i];
				    if (block_value.IsNull()) {
					    i++;
					    continue;
				    }

				    auto &struct_children = StructValue::GetChildren(block_value);
				    if (struct_children.size() < DUCK_BLOCK_FIELD_COUNT) {
					    i++;
					    continue;
				    }

				    // duck_block: kind, element_type, content, level, encoding, attributes, element_order
				    string kind =
				        struct_children[Vocab::KIND_IDX].IsNull() ? "" : struct_children[Vocab::KIND_IDX].ToString();

				    // Metadata and any future kind are not section content.
				    if (!IsRenderableKind(kind)) {
					    i = SkipElementScope(list_children, i, list_children.size());
					    continue;
				    }
				    string element_type = struct_children[Vocab::ELEMENT_TYPE_IDX].IsNull()
				                              ? ""
				                              : struct_children[Vocab::ELEMENT_TYPE_IDX].ToString();
				    string content = struct_children[Vocab::CONTENT_IDX].IsNull()
				                         ? ""
				                         : struct_children[Vocab::CONTENT_IDX].ToString();
				    int32_t level = struct_children[Vocab::LEVEL_IDX].IsNull()
				                        ? 0
				                        : struct_children[Vocab::LEVEL_IDX].GetValue<int32_t>();
				    string encoding = struct_children[Vocab::ENCODING_IDX].IsNull()
				                          ? "text"
				                          : struct_children[Vocab::ENCODING_IDX].ToString();
				    Value attributes = struct_children[Vocab::ATTRIBUTES_IDX];

				    if (element_type == Vocab::TYPE_HEADING) {
					    // Flush previous section
					    flush_section(render_range(content_begin, i));

					    // Get heading level: attribute takes priority, fall back to level field
					    int32_t heading_level = 1;
					    string heading_level_attr = GetAttribute(attributes, Vocab::ATTR_HEADING_LEVEL);
					    if (!heading_level_attr.empty()) {
						    heading_level = ParseIntAttribute(Vocab::ATTR_HEADING_LEVEL, heading_level_attr);
					    } else if (level > 0 && level <= 6) {
						    heading_level = level;
					    }
					    // Clamp to valid range
					    if (heading_level < 1)
						    heading_level = 1;
					    if (heading_level > 6)
						    heading_level = 6;

					    // Update section path for new heading
					    while (section_path_parts.size() >= (size_t)heading_level) {
						    section_path_parts.pop_back();
					    }
					    section_path_parts.push_back(content);

					    // Start new section
					    current_title = content;
					    current_level = heading_level;
					    current_section_id = GetAttribute(attributes, "id");
					    if (current_section_id.empty()) {
						    // Generate ID from title
						    // ASCII-only lowercase: ::tolower on a raw (signed) char is UB for
						    // any UTF-8 byte >= 0x80, and this id keeps its non-ASCII bytes.
						    current_section_id = StringUtil::Lower(content);
						    std::replace(current_section_id.begin(), current_section_id.end(), ' ', '-');
					    }
					    content_begin = i + 1;
				    } else if (element_type == Vocab::TYPE_METADATA || element_type == "frontmatter") {
					    // Metadata becomes level 0 section
					    flush_section(render_range(content_begin, i));
					    current_title = "";
					    current_level = 0;
					    current_section_id = "frontmatter";
					    flush_section(content);
					    current_title.clear();
					    content_begin = i + 1;
				    }
				    // Anything else is section content, rendered as a range when the
				    // section is flushed rather than one element at a time.
				    i++;
			    }

			    // Flush final section
			    flush_section(render_range(content_begin, list_children.size()));

			    result.SetValue(row_idx, Value::LIST(LogicalType::STRUCT(child_list_t<LogicalType> {
			                                             {"section_id", LogicalType::VARCHAR},
			                                             {"section_path", LogicalType::VARCHAR},
			                                             {"level", LogicalType::INTEGER},
			                                             {"title", LogicalType::VARCHAR},
			                                             {"content", MarkdownTypes::MarkdownType()}}),
			                                         sections));
		    }
	    });

	duck_blocks_to_sections.SetFallible();
	{
		CreateScalarFunctionInfo info(std::move(duck_blocks_to_sections));
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		FunctionDescription desc;
		desc.parameter_names = {"blocks"};
		desc.description = "Convert a list of duck_blocks into structured sections.";
		desc.examples = {"duck_blocks_to_sections([])"};
		desc.categories = {"markdown"};
		info.descriptions.push_back(desc);
		loader.RegisterFunction(std::move(info));
	}
}

//===--------------------------------------------------------------------===//
// Register All Functions
//===--------------------------------------------------------------------===//

// Build a duck_block STRUCT Value from a parsed MarkdownBlock. Empty content
// becomes NULL (the spec's convention for containers with structured children).
static Value MakeDuckBlockValue(const markdown_utils::MarkdownBlock &b) {
	child_list_t<Value> fields;
	fields.emplace_back("kind", Value(b.kind));
	fields.emplace_back("element_type", Value(b.block_type));
	fields.emplace_back("content", b.content.empty() ? Value(LogicalType::VARCHAR) : Value(b.content));
	fields.emplace_back("level", b.level >= 0 ? Value::INTEGER(b.level) : Value(LogicalType::INTEGER));
	fields.emplace_back("encoding", Value(b.encoding));
	vector<Value> keys, vals;
	for (const auto &a : b.attributes) {
		keys.push_back(Value(a.first));
		vals.push_back(Value(a.second));
	}
	fields.emplace_back("attributes", Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, keys, vals));
	fields.emplace_back("element_order", Value::INTEGER(b.block_order));
	return Value::STRUCT(fields);
}

// parse_markdown_to_duck_blocks(md VARCHAR) -> LIST(duck_block)
// Scalar counterpart of read_markdown_blocks that emits the canonical structured
// representation: rich text as kind='inline' children, not markdown-in-content.
static void RegisterParseMarkdownToDuckBlocks(ExtensionLoader &loader) {
	auto duck_block_type = MarkdownTypes::DuckBlockType();
	auto duck_block_list_type = LogicalType::LIST(duck_block_type);

	ScalarFunction fn("parse_markdown_to_duck_blocks", {LogicalType::VARCHAR}, duck_block_list_type,
	                  [](DataChunk &args, ExpressionState &state, Vector &result) {
		                  auto &in = args.data[0];
		                  auto dtype = MarkdownTypes::DuckBlockType();
		                  for (idx_t i = 0; i < args.size(); i++) {
			                  auto v = in.GetValue(i);
			                  if (v.IsNull()) {
				                  result.SetValue(i, Value());
				                  continue;
			                  }
			                  auto blocks = markdown_utils::ParseBlocks(v.ToString(), /*structured_inlines=*/true);
			                  vector<Value> vals;
			                  vals.reserve(blocks.size());
			                  for (const auto &b : blocks) {
				                  vals.push_back(MakeDuckBlockValue(b));
			                  }
			                  result.SetValue(i, Value::LIST(dtype, vals));
		                  }
	                  });
	fn.SetFallible();
	{
		CreateScalarFunctionInfo info(std::move(fn));
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		FunctionDescription desc;
		desc.parameter_names = {"markdown"};
		desc.description = "Parse Markdown text into a list of canonical duck_block structs.";
		desc.examples = {"parse_markdown_to_duck_blocks('# Hello')"};
		desc.categories = {"markdown"};
		info.descriptions.push_back(desc);
		loader.RegisterFunction(std::move(info));
	}
}

void DuckBlockFunctions::Register(ExtensionLoader &loader) {
	RegisterDuckBlockToMdFunction(loader);
	RegisterDuckBlocksToMdFunction(loader);
	RegisterDuckBlocksToSectionsFunction(loader);
	RegisterParseMarkdownToDuckBlocks(loader);
}

} // namespace duckdb
