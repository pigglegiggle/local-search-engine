#include "lse/engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <random>
#include <set>
#include <stdexcept>

namespace {
int checks = 0;
void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
template<class F> void rejects(F action, const std::string& message) {
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    check(failed, message);
}
lse::Index corpus(const std::vector<std::string>& contents) {
    lse::Index index; index.root = "/unit-test";
    for (const auto& content : contents) {
        auto id = static_cast<lse::DocId>(index.documents.size());
        auto tokens = lse::analyze(content);
        index.documents.push_back({std::to_string(10000 + id) + (id % 2 ? ".cpp" : ".md"), content,
                                   static_cast<std::uint32_t>(tokens.size())});
        std::map<std::string, std::vector<std::uint32_t>> local;
        for (const auto& token : tokens) local[token.text].push_back(token.position);
        for (auto& [word, positions] : local) index.terms[word].push_back({id, std::move(positions)});
    }
    index.prepare(); index.validate(); return index;
}
std::set<lse::DocId> ids(const lse::Index& index, const std::string& query) {
    std::set<lse::DocId> result;
    for (const auto& hit : index.search(query, 10000).hits) result.insert(hit.document);
    return result;
}
void tokenizer() {
    auto tokens = lse::analyze("Cache, MEMORY\nไทย café a_b 123!");
    check(tokens.size() == 6, "UTF-8 tokenizer count");
    check(tokens[0].text == "cache" && tokens[1].text == "memory", "ASCII case folding");
    check(tokens[2].text == "ไทย" && tokens[3].text == "café", "Unicode retained");
    for (std::size_t i = 0; i < tokens.size(); ++i) check(tokens[i].position == i, "contiguous positions");
    rejects([] { lse::analyze(std::string("\xc0\x80", 2)); }, "overlong UTF-8 rejected");
    rejects([] { lse::analyze(std::string("\xed\xa0\x80", 3)); }, "surrogate rejected");
    rejects([] { lse::analyze(std::string("\xf4\x90\x80\x80", 4)); }, "out of range rejected");
    rejects([] { lse::analyze(std::string("\xe0", 1)); }, "truncated UTF-8 rejected");
    check(lse::edit_distance("café", "cafe") == 1, "distance uses codepoints");
    check(lse::edit_distance("ไทย", "ไท") == 1, "Thai distance");
    const std::string mixed = "  A, café\nไทย!";
    auto offsets = lse::analyze(mixed);
    check(offsets.size() == 3, "mixed-width token count");
    const std::vector<std::pair<std::size_t, std::size_t>> expected_offsets{{2,3}, {5,10}, {11,20}};
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        check(offsets[i].begin == expected_offsets[i].first && offsets[i].end == expected_offsets[i].second,
              "streaming analysis preserves UTF-8 byte offsets");
        check(offsets[i].position == i, "streaming analysis preserves ordinal positions");
    }
    check(lse::codepoints(mixed).size() == 14, "codepoint decoder shares streaming UTF-8 validation");
    for (const std::string& invalid : {std::string("\x80", 1), std::string("\xe0\x80\x80", 3),
             std::string("\xf0\x80\x80\x80", 4), std::string("\xe2x\x80", 3)}) {
        rejects([&] { lse::analyze("valid prefix " + invalid); }, "invalid UTF-8 after token boundary rejected");
        rejects([&] { lse::codepoints(invalid); }, "codepoint collection rejects invalid UTF-8");
        auto damaged = corpus({"valid"});
        damaged.documents[0].content += invalid;
        rejects([&] { damaged.validate(); }, "allocation-free content validation rejects malformed UTF-8");
    }

}
void queries() {
    auto index = corpus({"memory allocation cache cache", "allocation memory scheduler", "memory memory allocation", "disk storage", ""});
    check(ids(index, "memory") == std::set<lse::DocId>({0,1,2}), "term membership");
    check(ids(index, "\"memory allocation\"") == std::set<lse::DocId>({0,2}), "ordered adjacent phrase");
    check(ids(index, "\"memory memory\"") == std::set<lse::DocId>({2}), "repeated-word phrase");
    check(ids(index, "cache OR scheduler AND storage") == std::set<lse::DocId>({0}), "AND precedes OR");
    check(ids(index, "(cache OR scheduler) NOT ext:md") == std::set<lse::DocId>({1}), "group and implicit AND");
    check(ids(index, "NOT memory") == std::set<lse::DocId>({3,4}), "NOT includes empty docs");
    check(ids(index, "NOT NOT memory") == ids(index, "memory"), "double negation");
    check(ids(index, "mem*") == ids(index, "memory"), "prefix expansion");
    check(ids(index, "memry~1") == ids(index, "memory"), "fuzzy expansion");
    check(ids(index, "ext:.cpp path:10001") == std::set<lse::DocId>({1}), "combined filters");
    check(ids(index, "no_such_term").empty(), "missing term");
    check(ids(index, "\"memory no_such_term\"").empty(), "missing phrase member");
    auto result = index.search("memory", 2);
    check(result.total == 3 && result.hits.size() == 2, "top k preserves total");
    check(result.hits[0].document == 2, "BM25 rewards saturation and length normalization");
    double expected = std::log1p((5.0 - 3 + .5) / (3 + .5)) * (2 * 2.2) /
        (2 + 1.2 * (.25 + .75 * 3 / (12.0/5)));
    check(std::abs(result.hits[0].score - expected) < 1e-12, "BM25 matches independent arithmetic");
    for (const auto& hit : result.hits) {
        double sum = 0; for (const auto& e : hit.explanation) sum += e.score;
        check(std::abs(sum - hit.score) < 1e-12, "explanation sums to score");
    }
    for (const auto& query : {"", "AND memory", "memory OR", "(memory", "memory)", "\"memory", "*", "memory~3", "title:x", "\"\""})
        rejects([&] { index.search(query); }, "malformed query rejected: " + std::string(query));
    rejects([&] { index.search(std::string(70, '(') + "memory" + std::string(70, ')')); }, "depth bound");
    rejects([&] { index.search("memory", 0); }, "zero limit");
    check(index.search("disk").hits.front().snippet.find("disk") != std::string::npos, "context snippet");
    auto ties = corpus({"same", "same", "same"});
    check(ties.search("same", 1).hits[0].document == 0, "deterministic heap tie breaking");
    auto unicode = corpus({"ไทย สวัสดี café", "ไท สวัสดี cafe"});
    check(ids(unicode, "\"ไทย สวัสดี\"") == std::set<lse::DocId>({0}), "Unicode phrase");
    check(ids(unicode, "café~1").size() == 2, "Unicode fuzzy retrieval");
    std::string wide_dictionary;
    for (unsigned i = 0; i < 140; ++i) wide_dictionary += "prefix_" + std::to_string(i) + " ";
    auto wide = corpus({wide_dictionary});
    rejects([&] { wide.search("prefix_*"); }, "broad prefix rejects incomplete expansion");
    std::string fuzzy_dictionary;
    lse::BKTree broad_tree;
    for (unsigned i = 0; i < 140; ++i) {
        auto word = std::string("term_") + static_cast<char>('a' + i / 26) + static_cast<char>('a' + i % 26);
        fuzzy_dictionary += word + " "; broad_tree.insert(word);
    }
    auto broad_fuzzy = corpus({fuzzy_dictionary});
    rejects([&] { broad_fuzzy.search("term_aa~2"); }, "broad fuzzy query rejects incomplete expansion");
    check(broad_tree.within("term_aa", 2).size() == 140, "unlimited BK-tree lookup remains complete");
    const auto capped = broad_tree.within("term_aa", 2, 129);
    check(capped.size() == 129 && std::is_sorted(capped.begin(), capped.end()), "bounded fuzzy expansion retains a sorted overflow witness");
    for (const auto& word : capped) check(lse::edit_distance("term_aa", word) <= 2, "bounded BK-tree results retain metric membership");
    check(broad_tree.within("term_aa", 2, 0).empty(), "zero BK-tree match budget returns no results");

    check(wide.search("prefix_139").total == 1, "exact lookup unaffected by expansion bound");
    rejects([&] { index.search(std::string(4097, 'a')); }, "query byte bound");
    std::string many_elements;
    for (unsigned i = 0; i < 257; ++i) many_elements += "x ";
    rejects([&] { index.search(many_elements); }, "query element bound");
}
void differential() {
    std::mt19937 random(20261030);
    const std::vector<std::string> vocabulary{"alpha", "beta", "gamma", "delta", "omega", "cache", "memory"};
    std::vector<std::string> texts;
    std::vector<std::vector<std::string>> reference;
    for (unsigned i = 0; i < 180; ++i) {
        std::vector<std::string> words;
        std::string text;
        for (unsigned j = 0, n = random() % 45; j < n; ++j) {
            auto word = vocabulary[random() % vocabulary.size()];
            words.push_back(word); text += word + " ";
        }
        texts.push_back(text); reference.push_back(words);
    }
    auto index = corpus(texts);
    for (unsigned trial = 0; trial < 1200; ++trial) {
        const auto& a = vocabulary[random() % vocabulary.size()];
        const auto& b = vocabulary[random() % vocabulary.size()];
        const auto& c = vocabulary[random() % vocabulary.size()];
        unsigned operation = random() % 5;
        std::string query = operation == 0 ? a + " " + b : operation == 1 ? a + " OR " + b :
            operation == 2 ? "NOT " + a : operation == 3 ? "\"" + a + " " + b + "\"" : "(" + a + " OR " + b + ") NOT " + c;
        std::set<lse::DocId> expected;
        for (lse::DocId id = 0; id < reference.size(); ++id) {
            const auto& words = reference[id];
            auto contains = [&](const std::string& w) { return std::find(words.begin(), words.end(), w) != words.end(); };
            bool match;
            if (operation == 0) match = contains(a) && contains(b);
            else if (operation == 1) match = contains(a) || contains(b);
            else if (operation == 2) match = !contains(a);
            else if (operation == 4) match = (contains(a) || contains(b)) && !contains(c);
            else {
                match = false;
                for (std::size_t j = 1; j < words.size(); ++j) if (words[j-1] == a && words[j] == b) match = true;
            }
            if (match) expected.insert(id);
        }
        check(ids(index, query) == expected, "query differs from brute-force oracle: " + query);
    }
    lse::BKTree tree;
    std::set<std::string> dictionary;
    for (unsigned i = 0; i < 250; ++i) {
        std::string word;
        for (unsigned j = 0, n = random() % 9 + 1; j < n; ++j) word += static_cast<char>('a' + random() % 6);
        dictionary.insert(word); tree.insert(word);
    }
    for (unsigned i = 0; i < 150; ++i) {
        std::string word = *std::next(dictionary.begin(), random() % dictionary.size());
        unsigned radius = random() % 3;
        std::vector<std::string> expected;
        for (const auto& candidate : dictionary) if (lse::edit_distance(word, candidate) <= radius) expected.push_back(candidate);
        check(tree.within(word, radius) == expected, "BK-tree differs from exhaustive edit distance");
    }
    // Random malformed input must either return a bounded result or throw, never crash.
    const std::string alphabet = "ab ()\"*~:012ANDORNOT\t";
    for (unsigned i = 0; i < 1000; ++i) {
        std::string query;
        for (unsigned j = 0, n = random() % 40; j < n; ++j) query += alphabet[random() % alphabet.size()];
        try { auto result = index.search(query); check(result.hits.size() <= 10, "bounded fuzz results"); }
        catch (const std::runtime_error&) { ++checks; }
    }
}
}
int main() {
    try { tokenizer(); queries(); differential(); std::cout << checks << " C++ checks passed (seed 20261030).\n"; }
    catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
