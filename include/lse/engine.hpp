#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace lse {
namespace fs = std::filesystem;
using DocId = std::uint32_t;

struct Token {
    std::string text;
    std::uint32_t position;
    std::size_t begin, end;
};
std::vector<Token> analyze(std::string_view text);
std::vector<std::uint32_t> codepoints(std::string_view text);
unsigned edit_distance(std::string_view a, std::string_view b);

struct Posting {
    DocId document;
    std::vector<std::uint32_t> positions;
};
struct Document {
    std::string path;
    std::string content;
    std::uint32_t length = 0;
};
struct Explanation {
    std::string term;
    std::uint32_t frequency, document_frequency;
    double score;
};
struct Hit {
    DocId document;
    double score;
    std::string snippet;
    std::vector<Explanation> explanation;
};
struct SearchResult {
    std::vector<Hit> hits;
    std::size_t total = 0;
    std::vector<std::string> expanded_terms;
    double milliseconds = 0;
};

class BKTree {
public:
    void insert(const std::string& word);
    std::vector<std::string> within(const std::string& word, unsigned radius) const;
private:
    struct Node {
        std::string word;
        std::map<unsigned, std::size_t> children;
    };
    std::vector<Node> nodes_;
};

struct Index {
    std::string root;
    std::vector<Document> documents;
    std::map<std::string, std::vector<Posting>> terms;
    BKTree fuzzy;
    std::uint64_t token_count = 0;
    void prepare();
    void validate() const;
    SearchResult search(const std::string& query, std::size_t limit = 10) const;
};

struct BuildOptions {
    unsigned threads = 0;
    std::uint64_t max_file_bytes = 8 * 1024 * 1024;
};
struct BuildReport {
    std::size_t added = 0, changed = 0, unchanged = 0, removed = 0, skipped = 0;
    double milliseconds = 0;
    std::vector<std::string> warnings;
};
struct BuildResult { Index index; BuildReport report; };
BuildResult build_index(const fs::path& root, const fs::path& index_path,
                        const Index* previous, BuildOptions options = {});

Index load_index(const fs::path& path);
void save_index(const Index& index, const fs::path& path);

class WriteLock {
public:
    explicit WriteLock(const fs::path& index_path);
    ~WriteLock();
    WriteLock(const WriteLock&) = delete;
    WriteLock& operator=(const WriteLock&) = delete;
private:
    int descriptor_ = -1;
};

std::string json_string(std::string_view text);
}
