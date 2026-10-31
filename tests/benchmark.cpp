#include "lse/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

namespace {
using Clock = std::chrono::steady_clock;
struct Workload {
    std::string first_term, second_term;
    bool is_phrase;
};
struct Scored {
    lse::DocId id;
    double score;
};
struct Reference {
    std::vector<std::vector<std::string>> document_words;
    std::map<std::string, std::size_t> document_frequencies;
    double average_length = 0;
    explicit Reference(const lse::Index& index) {
        for (const auto& doc : index.documents) {
            std::vector<std::string> tokens;
            std::set<std::string> unique;
            for (const auto& token : lse::analyze(doc.content)) {
                tokens.push_back(token.text);
                unique.insert(token.text);
            }
            average_length += tokens.size();
            document_words.push_back(std::move(tokens));
            for (const auto& word : unique) {
                ++document_frequencies[word];
            }
        }
        average_length /= std::max<std::size_t>(document_words.size(), 1);
    }
    std::pair<std::vector<Scored>, std::size_t> search(const Workload& query,
                                                       std::size_t limit) const {
        if (!document_frequencies.count(query.first_term) ||
            !document_frequencies.count(query.second_term)) {
            return {{}, 0};
        }
        std::vector<Scored> scores;
        for (lse::DocId id = 0; id < document_words.size(); ++id) {
            unsigned first_count = 0, second_count = 0;
            bool phrase_found = false;
            const auto& tokens = document_words[id];
            for (std::size_t i = 0; i < tokens.size(); ++i) {
                if (tokens[i] == query.first_term) {
                    ++first_count;
                }
                if (tokens[i] == query.second_term) {
                    ++second_count;
                }
                if (i && tokens[i - 1] == query.first_term && tokens[i] == query.second_term) {
                    phrase_found = true;
                }
            }
            if (!first_count || !second_count || (query.is_phrase && !phrase_found)) {
                continue;
            }
            auto bm25 = [&](const std::string& term, unsigned count) {
                auto frequency = static_cast<double>(document_frequencies.at(term));
                auto idf =
                    std::log(1 + (document_words.size() - frequency + .5) / (frequency + .5));
                return idf * (count * 2.2) /
                       (count + 1.2 * (.25 + .75 * tokens.size() / average_length));
            };
            scores.push_back(
                {id, bm25(query.first_term, first_count) + bm25(query.second_term, second_count)});
        }
        auto total = scores.size();
        std::sort(scores.begin(), scores.end(), [](const Scored& left, const Scored& right) {
            return left.score != right.score ? left.score > right.score : left.id < right.id;
        });
        if (scores.size() > limit) {
            scores.resize(limit);
        }
        return {scores, total};
    }
};
void verify_rankings(const lse::Index& index, const Reference& reference, const Workload& workload,
                     const std::string& query) {
    auto expected = reference.search(workload, 10);
    auto actual = index.search(query, 10);
    if (actual.total != expected.second || actual.hits.size() != expected.first.size()) {
        throw std::runtime_error("benchmark result count differs: " + query);
    }
    for (std::size_t i = 0; i < actual.hits.size(); ++i) {
        if (actual.hits[i].document != expected.first[i].id ||
            std::abs(actual.hits[i].score - expected.first[i].score) > 1e-9) {
            throw std::runtime_error("benchmark ranking or BM25 differs: " + query);
        }
    }
}

double percentile(std::vector<double> samples, double p) {
    std::sort(samples.begin(), samples.end());
    return samples[static_cast<std::size_t>(std::ceil(p * samples.size())) - 1];
}
volatile double sink = 0;
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc != 3) {
            throw std::runtime_error("usage: benchmark_runner INDEX REPETITIONS");
        }
        auto index = lse::load_index(argv[1]);
        auto repeats = std::stoul(argv[2]);
        if (!repeats || repeats > 1000) {
            throw std::runtime_error("invalid repetitions");
        }
        Reference reference(index);
        const std::vector<Workload> workloads{
            {"memory", "allocation", false},   {"cache", "locality", false},
            {"scheduler", "latency", false},   {"kernel", "thread", false},
            {"term_0001", "memory", false},    {"term_0099", "cache", false},
            {"term_0999", "term_0500", false}, {"missing", "memory", false},
            {"memory", "allocation", true},    {"cache", "locality", true},
            {"kernel", "thread", true},        {"term_0001", "term_0002", true}};
        std::vector<double> indexed_times, baseline_times;
        for (const auto& workload : workloads) {
            auto query = workload.is_phrase
                             ? '"' + workload.first_term + " " + workload.second_term + '"'
                             : workload.first_term + " " + workload.second_term;
            verify_rankings(index, reference, workload, query);
            for (unsigned repetition = 0; repetition < repeats; ++repetition) {
                auto indexed = [&] {
                    auto start = Clock::now();
                    auto result = index.search(query, 10);
                    indexed_times.push_back(
                        std::chrono::duration<double, std::milli>(Clock::now() - start).count());
                    sink += result.total + (result.hits.empty() ? 0 : result.hits[0].score);
                };
                auto scanned = [&] {
                    auto start = Clock::now();
                    auto result = reference.search(workload, 10);
                    baseline_times.push_back(
                        std::chrono::duration<double, std::milli>(Clock::now() - start).count());
                    sink += result.second + (result.first.empty() ? 0 : result.first[0].score);
                };
                if (repetition % 2) {
                    scanned();
                    indexed();
                } else {
                    indexed();
                    scanned();
                }
            }
        }
        std::cout << "{\"documents\":" << index.documents.size()
                  << ",\"workloads\":" << workloads.size()
                  << ",\"samples_per_method\":" << indexed_times.size()
                  << ",\"rankings_verified\":true,\"indexed_p50_ms\":"
                  << percentile(indexed_times, .5)
                  << ",\"indexed_p95_ms\":" << percentile(indexed_times, .95)
                  << ",\"scan_p50_ms\":" << percentile(baseline_times, .5)
                  << ",\"scan_p95_ms\":" << percentile(baseline_times, .95) << "}\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
