#include "lse/engine.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <mutex>
#include <numeric>
#include <queue>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace lse {
namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
struct Rune {
    std::uint32_t value;
    std::size_t begin, end;
};
template <class Visitor> void visit_runes(std::string_view text, Visitor visit) {
    for (std::size_t i = 0; i < text.size();) {
        const auto begin = i;
        auto first = static_cast<unsigned char>(text[i++]);
        std::uint32_t cp = first;
        unsigned following = 0;
        if (first < 0x80) {
        } else if (first >= 0xc2 && first <= 0xdf) {
            cp &= 0x1f;
            following = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            cp &= 0x0f;
            following = 2;
        } else if (first >= 0xf0 && first <= 0xf4) {
            cp &= 0x07;
            following = 3;
        } else {
            throw std::runtime_error("invalid UTF-8");
        }
        if (i + following > text.size()) {
            throw std::runtime_error("truncated UTF-8");
        }
        for (unsigned j = 0; j < following; ++j) {
            auto c = static_cast<unsigned char>(text[i++]);
            if ((c & 0xc0) != 0x80) {
                throw std::runtime_error("invalid UTF-8 continuation");
            }
            cp = (cp << 6) | (c & 0x3f);
        }
        if ((following == 1 && cp < 0x80) || (following == 2 && cp < 0x800) ||
            (following == 3 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
            throw std::runtime_error("invalid UTF-8 codepoint");
        }
        visit(Rune{cp, begin, i});
    }
}
bool word_character(std::uint32_t c) {
    if (c < 128) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_';
    }
    // Preserve common scripts and combining marks; this is not linguistic segmentation.
    return (c >= 0xc0 && c <= 0x2af && c != 0xd7 && c != 0xf7) || (c >= 0x300 && c <= 0x52f) ||
           (c >= 0x590 && c <= 0x8ff) || (c >= 0x900 && c <= 0x1fff) ||
           (c >= 0x3040 && c <= 0x30ff) || (c >= 0x3400 && c <= 0x9fff) ||
           (c >= 0xac00 && c <= 0xd7af);
}
std::string ascii_lower(std::string value) {
    for (auto& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c += 'a' - 'A';
        }
    }
    return value;
}
unsigned distance(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b) {
    std::vector<unsigned> previous(b.size() + 1), current(b.size() + 1);
    std::iota(previous.begin(), previous.end(), 0);
    for (std::size_t i = 1; i <= a.size(); ++i) {
        current[0] = static_cast<unsigned>(i);
        for (std::size_t j = 1; j <= b.size(); ++j) {
            current[j] = std::min(
                {current[j - 1] + 1, previous[j] + 1, previous[j - 1] + (a[i - 1] != b[j - 1])});
        }
        previous.swap(current);
    }
    return previous.back();
}

struct Match {
    DocId id;
    double score;
    std::vector<Explanation> explanation;
};
using Matches = std::vector<Match>;
Matches merge(Matches left, Matches right, bool conjunction) {
    Matches result;
    std::size_t a = 0, b = 0;
    while (a < left.size() && b < right.size()) {
        if (left[a].id == right[b].id) {
            left[a].score += right[b].score;
            auto& e = left[a].explanation;
            e.insert(e.end(), right[b].explanation.begin(), right[b].explanation.end());
            result.push_back(std::move(left[a]));
            ++a;
            ++b;
        } else if (left[a].id < right[b].id) {
            if (!conjunction) {
                result.push_back(std::move(left[a]));
            }
            ++a;
        } else {
            if (!conjunction) {
                result.push_back(std::move(right[b]));
            }
            ++b;
        }
    }
    if (!conjunction) {
        while (a < left.size()) {
            result.push_back(std::move(left[a++]));
        }
        while (b < right.size()) {
            result.push_back(std::move(right[b++]));
        }
    }
    return result;
}
enum class Kind {
    Atom,
    Phrase,
    And,
    Or,
    Not,
    Left,
    Right,
    End
};
struct Lexeme {
    Kind kind;
    std::string text;
};
std::vector<Lexeme> lex(const std::string& query) {
    if (query.size() > 4096) {
        throw std::runtime_error("query exceeds 4096 bytes");
    }
    std::vector<Lexeme> output;
    for (std::size_t i = 0; i < query.size();) {
        unsigned char c = query[i];
        if (c <= 32) {
            ++i;
            continue;
        }
        if (c == '(' || c == ')') {
            output.push_back({c == '(' ? Kind::Left : Kind::Right, ""});
            ++i;
        } else if (c == '"') {
            std::string value;
            ++i;
            while (i < query.size() && query[i] != '"') {
                value += query[i++];
            }
            if (i == query.size()) {
                throw std::runtime_error("unclosed quote");
            }
            ++i;
            output.push_back({Kind::Phrase, std::move(value)});
        } else {
            auto start = i;
            while (i < query.size() && static_cast<unsigned char>(query[i]) > 32 &&
                   query[i] != '(' && query[i] != ')' && query[i] != '"') {
                ++i;
            }
            auto text = query.substr(start, i - start);
            Kind kind = text == "AND"   ? Kind::And
                        : text == "OR"  ? Kind::Or
                        : text == "NOT" ? Kind::Not
                                        : Kind::Atom;
            output.push_back({kind, std::move(text)});
        }
        if (output.size() > 256) {
            throw std::runtime_error("query exceeds 256 elements");
        }
    }
    output.push_back({Kind::End, ""});
    return output;
}

class Query {
  public:
    Query(const Index& index, const std::string& query) : index_(index), tokens_(lex(query)) {}
    Matches run() {
        if (peek() == Kind::End) {
            throw std::runtime_error("query is empty");
        }
        auto result = disjunction(0);
        if (peek() != Kind::End) {
            throw std::runtime_error("unexpected query element");
        }
        return result;
    }
    std::set<std::string> expanded;

  private:
    const Index& index_;
    std::vector<Lexeme> tokens_;
    std::size_t cursor_ = 0;
    Kind peek() const {
        return tokens_[cursor_].kind;
    }
    Matches disjunction(unsigned depth) {
        auto result = conjunction(depth);
        while (peek() == Kind::Or) {
            ++cursor_;
            result = merge(std::move(result), conjunction(depth), false);
        }
        return result;
    }
    Matches conjunction(unsigned depth) {
        auto result = unary(depth);
        while (peek() == Kind::And || peek() == Kind::Atom || peek() == Kind::Phrase ||
               peek() == Kind::Left || peek() == Kind::Not) {
            if (peek() == Kind::And) {
                ++cursor_;
            }
            result = merge(std::move(result), unary(depth), true);
        }
        return result;
    }
    Matches unary(unsigned depth) {
        if (depth > 64) {
            throw std::runtime_error("query nesting exceeds 64");
        }
        if (peek() == Kind::Not) {
            ++cursor_;
            auto excluded = unary(depth + 1);
            Matches output;
            std::size_t j = 0;
            for (DocId id = 0; id < index_.documents.size(); ++id) {
                if (j < excluded.size() && excluded[j].id == id) {
                    ++j;
                } else {
                    output.push_back({id, 0, {}});
                }
            }
            return output;
        }
        if (peek() == Kind::Left) {
            ++cursor_;
            auto result = disjunction(depth + 1);
            if (peek() != Kind::Right) {
                throw std::runtime_error("missing closing parenthesis");
            }
            ++cursor_;
            return result;
        }
        if (peek() != Kind::Atom && peek() != Kind::Phrase) {
            throw std::runtime_error("expected term, phrase, or parenthesis");
        }
        auto token = tokens_[cursor_++];
        return token.kind == Kind::Phrase ? phrase(token.text) : atom(token.text);
    }
    Matches term(const std::string& word, double weight = 1) {
        auto it = index_.terms.find(word);
        if (it == index_.terms.end()) {
            return {};
        }
        expanded.insert(word);
        const double n = static_cast<double>(index_.documents.size());
        const double df = static_cast<double>(it->second.size());
        const double idf = std::log1p((n - df + 0.5) / (df + 0.5));
        const double average = n ? static_cast<double>(index_.token_count) / n : 1;
        Matches result;
        for (const auto& posting : it->second) {
            auto tf = static_cast<std::uint32_t>(posting.positions.size());
            const double length = index_.documents[posting.document].length;
            const double norm = 1.2 * (0.25 + 0.75 * length / std::max(average, 1e-9));
            const double score = weight * idf * (tf * 2.2) / (tf + norm);
            result.push_back(
                {posting.document, score, {{word, tf, static_cast<std::uint32_t>(df), score}}});
        }
        return result;
    }
    Matches atom(std::string value) {
        auto colon = value.find(':');
        if (colon != std::string::npos) {
            auto field = value.substr(0, colon);
            auto filter = ascii_lower(value.substr(colon + 1));
            if ((field != "ext" && field != "path") || filter.empty()) {
                throw std::runtime_error("supported filters: ext:cpp and path:folder");
            }
            if (field == "ext" && filter.front() != '.') {
                filter = '.' + filter;
            }
            Matches result;
            for (DocId id = 0; id < index_.documents.size(); ++id) {
                auto path = ascii_lower(index_.documents[id].path);
                bool matches = field == "ext"
                                   ? ascii_lower(fs::path(path).extension().string()) == filter
                                   : path.find(filter) != std::string::npos;
                if (matches) {
                    result.push_back({id, 0, {}});
                }
            }
            return result;
        }
        bool prefix = !value.empty() && value.back() == '*';
        unsigned radius = 0;
        if (prefix) {
            value.pop_back();
        } else if (value.size() >= 2 && value[value.size() - 2] == '~' &&
                   (value.back() == '1' || value.back() == '2')) {
            radius = static_cast<unsigned>(value.back() - '0');
            value.resize(value.size() - 2);
        }
        auto tokens = analyze(value);
        if (tokens.size() != 1 || tokens.front().text.size() != value.size()) {
            throw std::runtime_error(
                "expected one word; use quotes for phrases (fuzzy syntax: word~1 or word~2)");
        }
        auto word = tokens.front().text;
        if (codepoints(word).size() > 128) {
            throw std::runtime_error("query word exceeds 128 codepoints");
        }
        if (!prefix && !radius) {
            return term(word);
        }
        std::vector<std::string> words;
        if (prefix) {
            for (auto it = index_.terms.lower_bound(word);
                 it != index_.terms.end() && it->first.compare(0, word.size(), word) == 0; ++it) {
                words.push_back(it->first);
                if (words.size() > 128) {
                    break;
                }
            }
        } else {
            words = index_.fuzzy.within(word, radius, 129);
        }
        if (words.size() > 128) {
            throw std::runtime_error(
                "expansion exceeds 128 terms; use a longer word or smaller edit distance");
        }
        Matches result;
        for (const auto& w : words) {
            result = merge(std::move(result),
                           term(w, radius ? 1.0 / (1 + edit_distance(word, w)) : 1), false);
        }
        return result;
    }
    Matches phrase(const std::string& value) {
        auto tokens = analyze(value);
        if (tokens.empty() || tokens.size() > 32) {
            throw std::runtime_error("phrase must contain 1 to 32 words");
        }
        Matches candidates;
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            auto current = term(tokens[i].text);
            candidates = i == 0 ? std::move(current)
                                : merge(std::move(candidates), std::move(current), true);
        }
        auto posting = [&](const std::string& word, DocId id) -> const Posting& {
            const auto& list = index_.terms.at(word);
            return *std::lower_bound(list.begin(), list.end(), id, [](const Posting& p, DocId d) {
                return p.document < d;
            });
        };
        Matches result;
        for (auto& candidate : candidates) {
            bool found = false;
            for (auto start : posting(tokens[0].text, candidate.id).positions) {
                bool consecutive = true;
                for (std::size_t i = 1; i < tokens.size(); ++i) {
                    const auto& positions = posting(tokens[i].text, candidate.id).positions;
                    if (!std::binary_search(positions.begin(), positions.end(), start + i)) {
                        consecutive = false;
                        break;
                    }
                }
                if (consecutive) {
                    found = true;
                    break;
                }
            }
            if (found) {
                result.push_back(std::move(candidate));
            }
        }
        return result;
    }
};

std::string snippet(const Document& doc, const std::set<std::string>& words) {
    auto tokens = analyze(doc.content);
    if (tokens.empty()) {
        return {};
    }
    std::size_t center = 0;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (words.count(tokens[i].text)) {
            center = i;
            break;
        }
    }
    auto first = center > 8 ? center - 8 : 0;
    auto last = std::min(tokens.size(), first + 32);
    auto text = doc.content.substr(tokens[first].begin, tokens[last - 1].end - tokens[first].begin);
    std::string output;
    for (unsigned char c : text) {
        if (c <= 32 || c == 127) {
            if (!output.empty() && output.back() != ' ') {
                output += ' ';
            }
        } else {
            output += static_cast<char>(c);
        }
    }
    return (first ? "… " : "") + output + (last < tokens.size() ? " …" : "");
}
} // namespace

std::vector<Token> analyze(std::string_view text) {
    std::vector<Token> output;
    std::size_t begin = 0, end = 0;
    bool active = false;
    auto flush = [&] {
        if (!active) {
            return;
        }
        auto value = ascii_lower(std::string(text.substr(begin, end - begin)));
        output.push_back({std::move(value), static_cast<std::uint32_t>(output.size()), begin, end});
        active = false;
    };
    // Decode one rune at a time instead of retaining a second full copy of the document.
    visit_runes(text, [&](const Rune& rune) {
        if (word_character(rune.value)) {
            if (!active) {
                begin = rune.begin;
                active = true;
            }
            end = rune.end;
        } else {
            flush();
        }
    });
    flush();
    return output;
}
std::vector<std::uint32_t> codepoints(std::string_view text) {
    std::vector<std::uint32_t> output;
    visit_runes(text, [&](const Rune& rune) {
        output.push_back(rune.value);
    });
    return output;
}
unsigned edit_distance(std::string_view a, std::string_view b) {
    return distance(codepoints(a), codepoints(b));
}
void BKTree::insert(const std::string& word) {
    if (nodes_.empty()) {
        nodes_.push_back({word, {}});
        return;
    }
    std::size_t node = 0;
    while (true) {
        auto d = edit_distance(word, nodes_[node].word);
        if (!d) {
            return;
        }
        auto child = nodes_[node].children.find(d);
        if (child != nodes_[node].children.end()) {
            node = child->second;
        } else {
            auto next = nodes_.size();
            nodes_[node].children.emplace(d, next);
            nodes_.push_back({word, {}});
            return;
        }
    }
}
std::vector<std::string> BKTree::within(const std::string& word, unsigned radius,
                                        std::size_t max_matches) const {
    std::vector<std::string> result;
    if (nodes_.empty() || max_matches == 0) {
        return result;
    }
    std::vector<std::size_t> stack{0};
    while (!stack.empty()) {
        auto id = stack.back();
        stack.pop_back();
        const auto& node = nodes_[id];
        auto d = edit_distance(word, node.word);
        if (d <= radius) {
            result.push_back(node.word);
            // Query callers need one overflow witness, not an unbounded expansion list.
            if (result.size() == max_matches) {
                break;
            }
        }
        auto lower = d > radius ? d - radius : 0;
        for (auto it = node.children.lower_bound(lower);
             it != node.children.end() && it->first <= d + radius; ++it) {
            stack.push_back(it->second);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}
void Index::prepare() {
    fuzzy = BKTree{};
    token_count = 0;
    for (const auto& doc : documents) {
        token_count += doc.length;
    }
    // Long identifiers remain searchable exactly; keep edit-distance work bounded.
    for (const auto& item : terms) {
        if (codepoints(item.first).size() <= 128) {
            fuzzy.insert(item.first);
        }
    }
}
void Index::validate() const {
    if (!fs::path(root).is_absolute()) {
        throw std::runtime_error("index root must be absolute");
    }
    std::string previous;
    for (const auto& doc : documents) {
        codepoints(doc.path);
        auto path = fs::path(doc.path);
        if (doc.path.empty() || path.is_absolute() || path.lexically_normal() != path ||
            doc.path.find('\0') != std::string::npos ||
            (!previous.empty() && previous >= doc.path)) {
            throw std::runtime_error("invalid or unordered document path");
        }
        for (const auto& part : path) {
            if (part == "..") {
                throw std::runtime_error("unsafe document path");
            }
        }
        if (doc.content.find('\0') != std::string::npos) {
            throw std::runtime_error("binary document in index");
        }
        visit_runes(doc.content, [](const Rune&) {});
        previous = doc.path;
    }
    std::vector<std::uint64_t> counts(documents.size());
    for (const auto& [word, list] : terms) {
        auto normalized = analyze(word);
        if (normalized.size() != 1 || normalized.front().text != word || list.empty()) {
            throw std::runtime_error("invalid dictionary term");
        }
        std::uint64_t prior_doc = 0;
        bool first_doc = true;
        for (const auto& p : list) {
            if (p.document >= documents.size() || (!first_doc && p.document <= prior_doc) ||
                p.positions.empty()) {
                throw std::runtime_error("invalid posting document");
            }
            std::uint64_t prior_position = 0;
            bool first_position = true;
            for (auto position : p.positions) {
                if (position >= documents[p.document].length ||
                    (!first_position && position <= prior_position)) {
                    throw std::runtime_error("invalid posting position");
                }
                prior_position = position;
                first_position = false;
            }
            counts[p.document] += p.positions.size();
            prior_doc = p.document;
            first_doc = false;
        }
    }
    for (std::size_t i = 0; i < counts.size(); ++i) {
        if (counts[i] != documents[i].length) {
            throw std::runtime_error("document token count mismatch");
        }
    }
}
SearchResult Index::search(const std::string& query, std::size_t limit) const {
    auto start = Clock::now();
    if (limit == 0 || limit > 10000) {
        throw std::runtime_error("limit must be between 1 and 10000");
    }
    Query parser(*this, query);
    auto matches = parser.run();
    auto better = [](const Match* a, const Match* b) {
        return a->score != b->score ? a->score > b->score : a->id < b->id;
    };
    std::priority_queue<const Match*, std::vector<const Match*>, decltype(better)> heap(better);
    for (const auto& match : matches) {
        heap.push(&match);
        if (heap.size() > limit) {
            heap.pop();
        }
    }
    std::vector<const Match*> selected;
    while (!heap.empty()) {
        selected.push_back(heap.top());
        heap.pop();
    }
    std::sort(selected.begin(), selected.end(), better);
    SearchResult result;
    result.total = matches.size();
    for (auto match : selected) {
        result.hits.push_back({match->id, match->score,
                               snippet(documents[match->id], parser.expanded), match->explanation});
    }
    result.expanded_terms.assign(parser.expanded.begin(), parser.expanded.end());
    result.milliseconds = elapsed(start);
    return result;
}

BuildResult build_index(const fs::path& input_root, const fs::path& index_path,
                        const Index* previous, BuildOptions options) {
    auto start = Clock::now();
    auto root = fs::canonical(input_root);
    if (!fs::is_directory(root)) {
        throw std::runtime_error("index root must be a directory");
    }
    if (options.max_file_bytes == 0 || options.max_file_bytes > 64 * 1024 * 1024) {
        throw std::runtime_error("file limit must be between 1 byte and 64 MiB");
    }
    const std::set<std::string> extensions{
        ".txt", ".md",  ".markdown", ".cpp",  ".hpp",  ".cc",  ".c",    ".h",   ".py",
        ".rs",  ".go",  ".java",     ".json", ".yaml", ".yml", ".toml", ".csv", ".log",
        ".sh",  ".sql", ".js",       ".ts",   ".tsx",  ".jsx", ".html", ".css", ".rst"};
    const std::set<std::string> ignored{".git", ".local-search", "node_modules", "build",
                                        "dist", "__pycache__",   ".venv"};
    std::vector<fs::path> files;
    auto target = fs::weakly_canonical(fs::absolute(index_path));
    // Fail closed on directory traversal errors; never silently remove unreadable documents.
    for (fs::recursive_directory_iterator it(root), end; it != end; ++it) {
        auto status = it->symlink_status();
        // Do not inspect targets: even a dangling or cyclic symlink is simply skipped.
        if (fs::is_symlink(status)) {
            it.disable_recursion_pending();
            continue;
        }
        if (fs::is_directory(status)) {
            if (ignored.count(it->path().filename().string())) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (fs::is_regular_file(status) &&
            extensions.count(ascii_lower(it->path().extension().string())) &&
            it->path().lexically_normal() != target) {
            files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end(), [&](const fs::path& a, const fs::path& b) {
        return a.lexically_relative(root).generic_string() <
               b.lexically_relative(root).generic_string();
    });
    if (files.size() > 100000) {
        throw std::runtime_error("document limit is 100000 files");
    }
    std::unordered_map<std::string, DocId> old_ids;
    if (previous && previous->root == root.string()) {
        for (DocId id = 0; id < previous->documents.size(); ++id) {
            old_ids.emplace(previous->documents[id].path, id);
        }
    }
    struct Prepared {
        Document doc;
        std::vector<Token> tokens;
        bool skipped = false, reused = false;
        std::string warning;
    };
    std::vector<Prepared> prepared(files.size());
    std::atomic<std::size_t> next{0};
    std::atomic<std::uint64_t> total_bytes{0};
    std::exception_ptr failure;
    std::mutex failure_mutex;
    unsigned threads =
        options.threads ? options.threads : std::max(1u, std::thread::hardware_concurrency());
    threads =
        std::min({threads, 16u, static_cast<unsigned>(std::max<std::size_t>(files.size(), 1))});
    auto worker = [&] {
        try {
            for (auto i = next.fetch_add(1); i < files.size(); i = next.fetch_add(1)) {
                auto& p = prepared[i];
                p.doc.path = files[i].lexically_relative(root).generic_string();
                if (fs::file_size(files[i]) > options.max_file_bytes) {
                    p.skipped = true;
                    p.warning = p.doc.path + ": exceeds file size limit";
                    continue;
                }
                std::ifstream input(files[i], std::ios::binary);
                if (!input) {
                    throw std::runtime_error("cannot read " + files[i].string());
                }
                // Read with a bound even if a file grows after the initial size check.
                char buffer[8192];
                while (input) {
                    input.read(buffer, sizeof(buffer));
                    p.doc.content.append(buffer, static_cast<std::size_t>(input.gcount()));
                    if (p.doc.content.size() > options.max_file_bytes) {
                        break;
                    }
                }
                if (input.bad()) {
                    throw std::runtime_error("read failed: " + p.doc.path);
                }
                if (p.doc.content.size() > options.max_file_bytes ||
                    p.doc.content.find('\0') != std::string::npos) {
                    p.skipped = true;
                    p.warning = p.doc.path + ": oversized or binary file";
                    p.doc.content.clear();
                    continue;
                }
                if (total_bytes.fetch_add(p.doc.content.size()) + p.doc.content.size() >
                    256ULL * 1024 * 1024) {
                    throw std::runtime_error("corpus exceeds the 256 MiB content limit");
                }
                auto old = old_ids.find(p.doc.path);
                if (old != old_ids.end() &&
                    previous->documents[old->second].content == p.doc.content) {
                    p.reused = true;
                    p.doc.length = previous->documents[old->second].length;
                } else {
                    try {
                        p.tokens = analyze(p.doc.content);
                    } catch (const std::runtime_error&) {
                        p.skipped = true;
                        p.warning = p.doc.path + ": not valid UTF-8";
                        p.doc.content.clear();
                        continue;
                    }
                    p.doc.length = static_cast<std::uint32_t>(p.tokens.size());
                }
            }
        } catch (...) {
            std::lock_guard<std::mutex> guard(failure_mutex);
            if (!failure) {
                failure = std::current_exception();
            }
        }
    };
    std::vector<std::thread> pool;
    try {
        for (unsigned i = 0; i < threads; ++i) {
            pool.emplace_back(worker);
        }
    } catch (...) {
        for (auto& thread : pool) {
            thread.join();
        }
        throw;
    }
    for (auto& thread : pool) {
        thread.join();
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
    BuildResult output;
    output.index.root = root.string();
    const auto missing = std::numeric_limits<DocId>::max();
    std::vector<DocId> remap(previous ? previous->documents.size() : 0, missing);
    std::set<std::string> live;
    for (auto& p : prepared) {
        if (p.skipped) {
            ++output.report.skipped;
            output.report.warnings.push_back(p.warning);
            continue;
        }
        auto id = static_cast<DocId>(output.index.documents.size());
        auto old = old_ids.find(p.doc.path);
        live.insert(p.doc.path);
        if (p.reused) {
            ++output.report.unchanged;
            remap[old->second] = id;
        } else {
            if (old == old_ids.end()) {
                ++output.report.added;
            } else {
                ++output.report.changed;
            }
            std::map<std::string, std::vector<std::uint32_t>> local;
            for (auto& token : p.tokens) {
                local[token.text].push_back(token.position);
            }
            for (auto& [word, positions] : local) {
                output.index.terms[word].push_back({id, std::move(positions)});
            }
        }
        output.index.documents.push_back(std::move(p.doc));
    }
    for (const auto& [path, id] : old_ids) {
        (void)id;
        if (!live.count(path)) {
            ++output.report.removed;
        }
    }
    if (previous) {
        for (const auto& [word, postings] : previous->terms) {
            for (const auto& p : postings) {
                if (remap[p.document] != missing) {
                    output.index.terms[word].push_back({remap[p.document], p.positions});
                }
            }
        }
    }
    for (auto& [word, list] : output.index.terms) {
        (void)word;
        std::sort(list.begin(), list.end(), [](const Posting& a, const Posting& b) {
            return a.document < b.document;
        });
    }
    output.index.prepare();
    output.index.validate();
    output.report.milliseconds = elapsed(start);
    return output;
}

std::string json_string(std::string_view text) {
    std::string out = "\"";
    const char* hex = "0123456789abcdef";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 32 || c == 127) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + '"';
}
} // namespace lse
