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
struct Workload { std::string a, b; bool phrase; };
struct Scored { lse::DocId id; double score; };
struct Reference {
    std::vector<std::vector<std::string>> words;
    std::map<std::string, std::size_t> df;
    double average = 0;
    explicit Reference(const lse::Index& index) {
        for (const auto& doc : index.documents) {
            std::vector<std::string> tokens;
            std::set<std::string> unique;
            for (const auto& token : lse::analyze(doc.content)) { tokens.push_back(token.text); unique.insert(token.text); }
            average += tokens.size(); words.push_back(std::move(tokens));
            for (const auto& word : unique) ++df[word];
        }
        average /= std::max<std::size_t>(words.size(), 1);
    }
    std::pair<std::vector<Scored>, std::size_t> search(const Workload& query, std::size_t limit) const {
        if (!df.count(query.a) || !df.count(query.b)) return {{}, 0};
        std::vector<Scored> scores;
        for (lse::DocId id = 0; id < words.size(); ++id) {
            unsigned a = 0, b = 0; bool phrase = false;
            const auto& tokens = words[id];
            for (std::size_t i = 0; i < tokens.size(); ++i) {
                if (tokens[i] == query.a) ++a;
                if (tokens[i] == query.b) ++b;
                if (i && tokens[i-1] == query.a && tokens[i] == query.b) phrase = true;
            }
            if (!a || !b || (query.phrase && !phrase)) continue;
            auto bm25 = [&](const std::string& term, unsigned count) {
                auto frequency = static_cast<double>(df.at(term));
                auto idf = std::log(1 + (words.size() - frequency + .5) / (frequency + .5));
                return idf * (count * 2.2) / (count + 1.2 * (.25 + .75 * tokens.size() / average));
            };
            scores.push_back({id, bm25(query.a, a) + bm25(query.b, b)});
        }
        auto total = scores.size();
        std::sort(scores.begin(), scores.end(), [](const Scored& a, const Scored& b) {
            return a.score != b.score ? a.score > b.score : a.id < b.id;
        });
        if (scores.size() > limit) scores.resize(limit);
        return {scores, total};
    }
};
double percentile(std::vector<double> samples, double p) {
    std::sort(samples.begin(), samples.end());
    return samples[static_cast<std::size_t>(std::ceil(p * samples.size())) - 1];
}
volatile double sink = 0;
}
int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("usage: benchmark_runner INDEX REPETITIONS");
        auto index = lse::load_index(argv[1]);
        auto repeats = std::stoul(argv[2]);
        if (!repeats || repeats > 1000) throw std::runtime_error("invalid repetitions");
        Reference reference(index);
        const std::vector<Workload> workloads{
            {"memory", "allocation", false}, {"cache", "locality", false},
            {"scheduler", "latency", false}, {"kernel", "thread", false},
            {"term_0001", "memory", false}, {"term_0099", "cache", false},
            {"term_0999", "term_0500", false}, {"missing", "memory", false},
            {"memory", "allocation", true}, {"cache", "locality", true},
            {"kernel", "thread", true}, {"term_0001", "term_0002", true}};
        std::vector<double> indexed_times, baseline_times;
        for (const auto& w : workloads) {
            auto query = w.phrase ? '"' + w.a + " " + w.b + '"' : w.a + " " + w.b;
            auto expected = reference.search(w, 10);
            auto actual = index.search(query, 10);
            if (actual.total != expected.second || actual.hits.size() != expected.first.size())
                throw std::runtime_error("benchmark result count differs: " + query);
            for (std::size_t i = 0; i < actual.hits.size(); ++i)
                if (actual.hits[i].document != expected.first[i].id || std::abs(actual.hits[i].score - expected.first[i].score) > 1e-9)
                    throw std::runtime_error("benchmark ranking or BM25 differs: " + query);
            for (unsigned repetition = 0; repetition < repeats; ++repetition) {
                auto indexed = [&] {
                    auto start = Clock::now(); auto result = index.search(query, 10);
                    indexed_times.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
                    sink += result.total + (result.hits.empty() ? 0 : result.hits[0].score);
                };
                auto scanned = [&] {
                    auto start = Clock::now(); auto result = reference.search(w, 10);
                    baseline_times.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
                    sink += result.second + (result.first.empty() ? 0 : result.first[0].score);
                };
                if (repetition % 2) { scanned(); indexed(); } else { indexed(); scanned(); }
            }
        }
        std::cout << "{\"documents\":" << index.documents.size() << ",\"workloads\":" << workloads.size()
                  << ",\"samples_per_method\":" << indexed_times.size()
                  << ",\"rankings_verified\":true,\"indexed_p50_ms\":" << percentile(indexed_times, .5)
                  << ",\"indexed_p95_ms\":" << percentile(indexed_times, .95)
                  << ",\"scan_p50_ms\":" << percentile(baseline_times, .5)
                  << ",\"scan_p95_ms\":" << percentile(baseline_times, .95) << "}\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
