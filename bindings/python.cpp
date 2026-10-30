#include "lse/engine.hpp"

#include <chrono>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

class NativeIndex {
public:
    explicit NativeIndex(const std::string& path) {
        const auto begin = std::chrono::steady_clock::now();
        {
            py::gil_scoped_release release;
            index_ = lse::load_index(path);
            for (const auto& doc : index_.documents) source_bytes_ += doc.content.size();
            for (const auto& term : index_.terms) postings_ += term.second.size();
        }
        load_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    }

    py::dict stats() const {
        py::dict result;
        result["root"] = index_.root;
        result["documents"] = index_.documents.size();
        result["terms"] = index_.terms.size();
        result["tokens"] = index_.token_count;
        result["postings"] = postings_;
        result["source_bytes"] = source_bytes_;
        result["load_ms"] = load_ms_;
        return result;
    }

    py::dict search(const std::string& query, std::size_t limit, bool explain) const {
        lse::SearchResult found;
        try {
            py::gil_scoped_release release;
            found = index_.search(query, limit);
        } catch (const std::runtime_error& error) {
            throw py::value_error(error.what());
        }
        py::list hits;
        for (const auto& hit : found.hits) {
            py::dict item;
            item["path"] = index_.documents[hit.document].path;
            item["score"] = hit.score;
            item["snippet"] = hit.snippet;
            if (explain) {
                py::list explanation;
                for (const auto& part : hit.explanation) {
                    py::dict entry;
                    entry["term"] = part.term;
                    entry["tf"] = part.frequency;
                    entry["df"] = part.document_frequency;
                    entry["score"] = part.score;
                    explanation.append(entry);
                }
                item["explanation"] = explanation;
            }
            hits.append(item);
        }
        py::dict result;
        result["query"] = query;
        result["total"] = found.total;
        result["search_ms"] = found.milliseconds;
        result["expanded_terms"] = found.expanded_terms;
        result["hits"] = hits;
        return result;
    }

private:
    lse::Index index_;
    std::uint64_t source_bytes_ = 0, postings_ = 0;
    double load_ms_ = 0;
};

PYBIND11_MODULE(_native, module) {
    module.doc() = "Immutable resident-memory snapshots of the C++ local search engine.";
    module.def("build_index", [](const std::string& root, const std::string& destination) {
        py::gil_scoped_release release;
        auto built = lse::build_index(root, destination, nullptr);
        lse::save_index(built.index, destination);
    }, py::arg("root"), py::arg("destination"));
    py::class_<NativeIndex>(module, "NativeIndex")
        .def(py::init<const std::string&>(), py::arg("path"))
        .def("stats", &NativeIndex::stats)
        .def("search", &NativeIndex::search, py::arg("query"), py::arg("limit") = 10, py::arg("explain") = false);
}
