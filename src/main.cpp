#include "lse/engine.hpp"

#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

namespace {
const char* help = R"(LOCAL SEARCH ENGINE — positional search in your own files

  lse index <folder> [--index FILE] [--threads N] [--max-file-mb N] [--rebuild]
  lse search '<query>' [--index FILE] [-k N] [--json] [--explain]
  lse repl [--index FILE] [-k N] [--explain]
  lse stats [--index FILE] [--json]
  lse verify [--index FILE]

Default index: .local-search/index.lsi (relative to the current directory).
Query: words use implicit AND; precedence NOT > AND > OR; parentheses supported.
  "memory allocation"       ordered, adjacent token positions
  scheduler~1               Levenshtein distance <= 1 (also ~2)
  sched*                    dictionary prefix
  (cache OR memory) NOT ext:log
  path:notes ext:md

Quote the entire query in your shell. Only uppercase AND/OR/NOT are operators.
Index UTF-8 text and source files; symlinks and build/dependency folders are skipped.
)";
struct Arguments {
    std::string command, operand;
    lse::fs::path index = ".local-search/index.lsi";
    std::size_t limit = 10;
    lse::BuildOptions build;
    bool json = false, explain = false, rebuild = false;
};
std::size_t number(const std::string& input, std::size_t min, std::size_t max) {
    if (input.empty() || input.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("expected a positive integer: " + input);
    auto value = std::stoull(input);
    if (value < min || value > max) throw std::runtime_error("numeric option is out of range");
    return static_cast<std::size_t>(value);
}
Arguments arguments(int argc, char** argv) {
    Arguments a; a.command = argv[1];
    const std::set<std::string> commands{"index", "search", "repl", "stats", "verify"};
    if (!commands.count(a.command)) throw std::runtime_error("unknown command: " + a.command);
    bool operand = false;
    for (int i = 2; i < argc; ++i) {
        std::string option = argv[i];
        auto value = [&]() -> std::string {
            if (++i == argc) throw std::runtime_error("missing value after " + option);
            return argv[i];
        };
        if (option == "--index") a.index = value();
        else if (option == "--rebuild" && a.command == "index") a.rebuild = true;
        else if (option == "-k" && (a.command == "search" || a.command == "repl")) a.limit = number(value(), 1, 10000);
        else if (option == "--threads" && a.command == "index") a.build.threads = static_cast<unsigned>(number(value(), 1, 16));
        else if (option == "--max-file-mb" && a.command == "index") a.build.max_file_bytes = number(value(), 1, 64) * 1024 * 1024;
        else if (option == "--json" && (a.command == "search" || a.command == "stats")) a.json = true;
        else if (option == "--explain" && (a.command == "search" || a.command == "repl")) a.explain = true;
        else if (!operand && (a.command == "search" || a.command == "index") && option.rfind("--", 0) != 0) {
            a.operand = option; operand = true;
        } else throw std::runtime_error("unexpected argument: " + option + "; quote the whole search query");
    }
    if ((a.command == "index" || a.command == "search") && !operand) throw std::runtime_error("missing folder or query");
    if (a.index.empty()) throw std::runtime_error("index path cannot be empty");
    return a;
}
void display(const lse::Index& index, const std::string& query, const Arguments& a) {
    auto result = index.search(query, a.limit);
    if (a.json) {
        std::cout << "{\"query\":" << lse::json_string(query) << ",\"total\":" << result.total
                  << ",\"search_ms\":" << result.milliseconds << ",\"expanded_terms\":[";
        for (std::size_t i = 0; i < result.expanded_terms.size(); ++i) {
            if (i) std::cout << ',';
            std::cout << lse::json_string(result.expanded_terms[i]);
        }
        std::cout << "],\"hits\":[";
        for (std::size_t i = 0; i < result.hits.size(); ++i) {
            if (i) std::cout << ',';
            const auto& h = result.hits[i];
            std::cout << "{\"path\":" << lse::json_string(index.documents[h.document].path)
                << ",\"score\":" << h.score << ",\"snippet\":" << lse::json_string(h.snippet);
            if (a.explain) {
                std::cout << ",\"explanation\":[";
                for (std::size_t j = 0; j < h.explanation.size(); ++j) {
                    if (j) std::cout << ',';
                    const auto& e = h.explanation[j];
                    std::cout << "{\"term\":" << lse::json_string(e.term) << ",\"tf\":" << e.frequency
                              << ",\"df\":" << e.document_frequency << ",\"score\":" << e.score << '}';
                }
                std::cout << ']';
            }
            std::cout << '}';
        }
        std::cout << "]}\n";
    } else {
        std::cout << result.total << " matches · " << result.milliseconds << " ms (in-memory search)\n";
        for (std::size_t i = 0; i < result.hits.size(); ++i) {
            const auto& h = result.hits[i];
            std::cout << '\n' << i + 1 << ". " << lse::json_string(index.documents[h.document].path)
                      << "  score=" << h.score << '\n' << "   " << h.snippet << '\n';
            if (a.explain) for (const auto& e : h.explanation)
                std::cout << "   " << lse::json_string(e.term) << " tf=" << e.frequency << " df=" << e.document_frequency
                          << " contribution=" << e.score << '\n';
        }
        if (!result.expanded_terms.empty()) {
            std::cout << "\nMatched terms:";
            for (const auto& word : result.expanded_terms) std::cout << ' ' << lse::json_string(word);
            std::cout << '\n';
        }
    }
}
void verify(const lse::Index& index) {
    std::map<std::string, std::vector<lse::Posting>> expected;
    for (lse::DocId id = 0; id < index.documents.size(); ++id) {
        auto tokens = lse::analyze(index.documents[id].content);
        if (tokens.size() != index.documents[id].length) throw std::runtime_error("document length failed semantic verification");
        std::map<std::string, std::vector<std::uint32_t>> local;
        for (const auto& token : tokens) local[token.text].push_back(token.position);
        for (auto& [word, positions] : local) expected[word].push_back({id, std::move(positions)});
    }
    if (expected.size() != index.terms.size()) throw std::runtime_error("dictionary failed semantic verification");
    for (const auto& [word, list] : expected) {
        const auto& actual = index.terms.at(word);
        if (list.size() != actual.size()) throw std::runtime_error("posting list failed semantic verification");
        for (std::size_t i = 0; i < list.size(); ++i)
            if (list[i].document != actual[i].document || list[i].positions != actual[i].positions)
                throw std::runtime_error("posting positions failed semantic verification");
    }
}
}

int main(int argc, char** argv) {
    try {
        std::cout << std::setprecision(9);
        if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "help") {
            std::cout << help; return 0;
        }
        auto a = arguments(argc, argv);
        if (a.command == "index") {
            lse::WriteLock lock(a.index);
            lse::Index previous;
            bool existing = lse::fs::exists(a.index) && !a.rebuild;
            if (existing) previous = lse::load_index(a.index);
            auto result = lse::build_index(a.operand, a.index, existing ? &previous : nullptr, a.build);
            lse::save_index(result.index, a.index);
            const auto& r = result.report;
            std::cout << "Indexed " << result.index.documents.size() << " documents · " << result.index.terms.size()
                      << " terms · " << result.index.token_count << " tokens\n"
                      << "Added " << r.added << " · changed " << r.changed << " · unchanged " << r.unchanged
                      << " · removed " << r.removed << " · skipped " << r.skipped << '\n'
                      << "Build " << r.milliseconds << " ms (excludes old-index load and save)\n"
                      << "Index: " << lse::json_string(lse::fs::absolute(a.index).string()) << '\n';
            for (const auto& warning : r.warnings) std::cerr << "warning: " << lse::json_string(warning) << '\n';
            return 0;
        }
        auto start = std::chrono::steady_clock::now();
        auto index = lse::load_index(a.index);
        auto load_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (a.command == "search") display(index, a.operand, a);
        else if (a.command == "stats") {
            auto bytes = lse::fs::file_size(a.index);
            std::uint64_t source_bytes = 0, postings = 0;
            for (const auto& doc : index.documents) source_bytes += doc.content.size();
            for (const auto& term : index.terms) postings += term.second.size();
            if (a.json) std::cout << "{\"documents\":" << index.documents.size() << ",\"terms\":" << index.terms.size()
                << ",\"tokens\":" << index.token_count << ",\"postings\":" << postings << ",\"source_bytes\":" << source_bytes
                << ",\"index_bytes\":" << bytes << ",\"load_ms\":" << load_ms << ",\"root\":" << lse::json_string(index.root) << "}\n";
            else std::cout << "Root: " << lse::json_string(index.root) << "\nDocuments: " << index.documents.size()
                << "\nVocabulary: " << index.terms.size() << "\nTokens: " << index.token_count << "\nPostings: " << postings
                << "\nSource bytes: " << source_bytes << "\nIndex bytes: " << bytes << "\nLoad: " << load_ms << " ms\n";
        } else if (a.command == "verify") {
            verify(index);
            std::cout << "Verified checksum, structure, and every token position in " << index.documents.size() << " documents.\n";
        } else {
            std::cout << "Loaded " << index.documents.size() << " documents in " << load_ms << " ms. Type :quit to exit.\n";
            std::string line;
            while (std::cout << "search> " && std::getline(std::cin, line)) {
                if (line == ":quit" || line == ":q") break;
                if (line.empty()) continue;
                try { display(index, line, a); }
                catch (const std::exception& e) { std::cerr << "query error: " << e.what() << '\n'; }
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n'; return 1;
    }
}
