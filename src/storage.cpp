#include "lse/engine.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lse {
namespace {
constexpr std::uint64_t max_index_bytes = 512ULL * 1024 * 1024;
constexpr std::string_view magic = "LSEIDX01";
std::runtime_error system_error(const std::string& action) {
    return std::runtime_error(action + ": " + std::strerror(errno));
}
std::uint32_t crc32(std::string_view data) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t i = 0; i < result.size(); ++i) {
            auto c = i;
            for (unsigned bit = 0; bit < 8; ++bit) c = (c >> 1) ^ ((c & 1) ? 0xedb88320u : 0);
            result[i] = c;
        }
        return result;
    }();
    std::uint32_t c = 0xffffffffu;
    for (unsigned char byte : data) c = table[(c ^ byte) & 255] ^ (c >> 8);
    return c ^ 0xffffffffu;
}
void varint(std::string& out, std::uint64_t value) {
    do {
        auto byte = static_cast<unsigned char>(value & 127); value >>= 7;
        out += static_cast<char>(byte | (value ? 128 : 0));
    } while (value);
}
void string(std::string& out, const std::string& value) { varint(out, value.size()); out += value; }
void fixed(std::string& out, std::uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) out += static_cast<char>((value >> (i * 8)) & 255);
}
class Reader {
public:
    explicit Reader(std::string_view data) : data_(data) {}
    std::uint64_t integer() {
        std::uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            auto byte = take();
            if (shift == 63 && byte > 1) throw std::runtime_error("overflowing index varint");
            value |= static_cast<std::uint64_t>(byte & 127) << shift;
            if (!(byte & 128)) return value;
        }
        throw std::runtime_error("invalid index varint");
    }
    std::uint32_t u32() {
        auto value = integer();
        if (value > std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("index integer overflow");
        return static_cast<std::uint32_t>(value);
    }
    std::size_t count(std::uint64_t limit) {
        auto value = integer();
        if (value > limit || value > remaining()) throw std::runtime_error("invalid index count");
        return static_cast<std::size_t>(value);
    }
    std::string text(std::size_t limit = 64 * 1024 * 1024) {
        auto size = count(limit);
        auto out = std::string(data_.substr(cursor_, size)); cursor_ += size;
        return out;
    }
    std::uint64_t fixed(unsigned bytes) {
        std::uint64_t value = 0;
        for (unsigned i = 0; i < bytes; ++i) value |= static_cast<std::uint64_t>(take()) << (i * 8);
        return value;
    }
    std::size_t remaining() const { return data_.size() - cursor_; }
private:
    std::string_view data_;
    std::size_t cursor_ = 0;
    unsigned char take() {
        if (cursor_ == data_.size()) throw std::runtime_error("truncated index");
        return static_cast<unsigned char>(data_[cursor_++]);
    }
};
}

Index load_index(const fs::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("cannot open index: " + path.string());
    auto size = input.tellg();
    if (size < 20 || static_cast<std::uint64_t>(size) > max_index_bytes + 20)
        throw std::runtime_error("invalid index size");
    input.seekg(0);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), size);
    if (!input) throw std::runtime_error("cannot read entire index");
    if (bytes.compare(0, magic.size(), magic) != 0) throw std::runtime_error("unsupported index format (expected LSEIDX01)");
    Reader header(std::string_view(bytes).substr(8, 12));
    auto length = header.fixed(8);
    auto checksum = header.fixed(4);
    auto payload = std::string_view(bytes).substr(20);
    if (length != payload.size() || crc32(payload) != checksum)
        throw std::runtime_error("index checksum or length mismatch; rebuild from source files");
    Reader reader(payload);
    Index index;
    index.root = reader.text(1024 * 1024);
    const auto documents = reader.count(100000);
    index.documents.reserve(documents);
    std::uint64_t total_bytes = 0;
    for (std::size_t i = 0; i < documents; ++i) {
        Document doc;
        doc.path = reader.text(1024 * 1024);
        doc.content = reader.text();
        total_bytes += doc.content.size();
        if (total_bytes > 256ULL * 1024 * 1024) throw std::runtime_error("index content exceeds 256 MiB");
        doc.length = reader.u32();
        if (doc.length > doc.content.size()) throw std::runtime_error("invalid document length");
        index.documents.push_back(std::move(doc));
    }
    auto terms = reader.count(max_index_bytes);
    std::string prior_term;
    for (std::size_t i = 0; i < terms; ++i) {
        auto word = reader.text();
        if (word.empty() || (!prior_term.empty() && word <= prior_term)) throw std::runtime_error("unordered dictionary");
        prior_term = word;
        auto entries = reader.count(documents);
        std::vector<Posting> list;
        list.reserve(entries);
        std::uint64_t doc_id = 0;
        for (std::size_t j = 0; j < entries; ++j) {
            doc_id += reader.u32();
            if (doc_id >= documents) throw std::runtime_error("posting references nonexistent document");
            Posting p{static_cast<DocId>(doc_id), {}};
            auto count = reader.count(index.documents[p.document].length);
            p.positions.reserve(count);
            std::uint64_t position = 0;
            for (std::size_t k = 0; k < count; ++k) {
                position += reader.u32();
                if (position >= index.documents[p.document].length) throw std::runtime_error("invalid token position");
                p.positions.push_back(static_cast<std::uint32_t>(position));
            }
            list.push_back(std::move(p));
        }
        index.terms.emplace(std::move(word), std::move(list));
    }
    if (reader.remaining()) throw std::runtime_error("unexpected trailing index data");
    index.validate(); index.prepare();
    return index;
}

WriteLock::WriteLock(const fs::path& path) {
    auto parent = fs::absolute(path).parent_path();
    fs::create_directories(parent);
    auto lock_path = path.string() + ".lock";
    descriptor_ = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    if (descriptor_ < 0) throw system_error("cannot open writer lock");
    if (::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
        ::close(descriptor_); descriptor_ = -1;
        throw std::runtime_error("another writer holds the index lock");
    }
}
WriteLock::~WriteLock() { if (descriptor_ >= 0) ::close(descriptor_); }

void save_index(const Index& index, const fs::path& path) {
    index.validate();
    std::string payload;
    string(payload, index.root); varint(payload, index.documents.size());
    for (const auto& doc : index.documents) {
        string(payload, doc.path); string(payload, doc.content); varint(payload, doc.length);
    }
    varint(payload, index.terms.size());
    for (const auto& [word, list] : index.terms) {
        string(payload, word); varint(payload, list.size());
        DocId prior_doc = 0;
        for (const auto& p : list) {
            varint(payload, p.document - prior_doc); prior_doc = p.document;
            varint(payload, p.positions.size());
            std::uint32_t prior_position = 0;
            for (auto position : p.positions) { varint(payload, position - prior_position); prior_position = position; }
        }
    }
    if (payload.size() > max_index_bytes) throw std::runtime_error("serialized index exceeds 512 MiB");
    std::string header(magic);
    fixed(header, payload.size(), 8); fixed(header, crc32(payload), 4);
    auto target = fs::absolute(path);
    fs::create_directories(target.parent_path());
    std::string temp = target.string() + ".tmp.XXXXXX";
    int descriptor = ::mkstemp(temp.data());
    if (descriptor < 0) throw system_error("cannot create index temporary file");
    try {
        auto write_all = [&](std::string_view data) {
            while (!data.empty()) {
                auto written = ::write(descriptor, data.data(), data.size());
                if (written < 0 && errno == EINTR) continue;
                if (written <= 0) throw system_error("index write failed");
                data.remove_prefix(static_cast<std::size_t>(written));
            }
        };
        write_all(header); write_all(payload);
        if (::fsync(descriptor) != 0) throw system_error("index fsync failed");
        if (::close(descriptor) != 0) { descriptor = -1; throw system_error("index close failed"); }
        descriptor = -1;
        if (::rename(temp.c_str(), target.c_str()) != 0) throw system_error("atomic index replacement failed");
        int directory = ::open(target.parent_path().c_str(), O_RDONLY);
        if (directory < 0) throw system_error("cannot open index directory for sync");
        auto sync_result = ::fsync(directory);
        auto sync_errno = errno;
        ::close(directory);
        if (sync_result != 0) { errno = sync_errno; throw system_error("index replaced but directory fsync failed"); }
    } catch (...) {
        if (descriptor >= 0) ::close(descriptor);
        ::unlink(temp.c_str()); throw;
    }
}
}
