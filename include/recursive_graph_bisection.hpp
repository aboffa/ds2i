#pragma once

#include <cmath>
#include <fstream>
#include <vector>

#include <pstl/algorithm>
#include <pstl/execution>

#include "binary_collection.hpp"
#include "binary_freq_collection.hpp"
#include "codec/block_codecs.hpp"
#include "codec/varintgb.hpp"
#include "util/index_build_utils.hpp"
#include "util/log.hpp"
#include "util/progress.hpp"

namespace ds2i {
const Log2<1024> log2;

namespace bp {

inline double expb(double logn1, double logn2, size_t deg1, size_t deg2) {
    double a = deg1 * (logn1 - log2(deg1 + 1));
    double b = deg2 * (logn2 - log2(deg2 + 1));
    return a + b;
};

struct doc_entry {
    uint32_t              id    = 0u;
    double                gain  = 0.0;
    size_t                term_count = 0u;
    std::vector<uint8_t>  terms_compressed{};
    std::vector<uint32_t> terms() const {
        std::vector<uint32_t> terms(term_count);
        VarIntGB<true> varintgb_codec;
        varintgb_codec.decodeArray(terms_compressed.data(), term_count, terms.data());
        return terms;
    }
};

class forward_index : public std::vector<doc_entry>{
   public:
    forward_index(size_t term_count)
        : m_term_count(term_count) {}

    const size_t &        term_count() const { return m_term_count; }
    static forward_index &compress(forward_index &fwd);
    static forward_index  from_inverted_index(const std::string &input_basename, size_t min_len);
    static forward_index  read(const std::string &input_file);
    static void           write(const forward_index &fwd, const std::string &output_file);

   private:
    size_t                 m_term_count;
};

void forward_index::write(const forward_index &fwd, const std::string &output_file)
{
    std::ofstream out(output_file.c_str());
    size_t size = fwd.size();
    out.write(reinterpret_cast<const char *>(&fwd.m_term_count), sizeof(fwd.m_term_count));
    out.write(reinterpret_cast<const char *>(&size), sizeof(size));
    for (const auto& doc : fwd) {
        size = doc.terms_compressed.size();
        out.write(reinterpret_cast<const char *>(&doc.term_count), sizeof(doc.term_count));
        out.write(reinterpret_cast<const char *>(&size), sizeof(size));
        out.write(reinterpret_cast<const char *>(doc.terms_compressed.data()), size);
    }
}

forward_index forward_index::read(const std::string &input_file)
{
    std::ifstream in(input_file.c_str());
    size_t term_count, docs_count;
    in.read(reinterpret_cast<char *>(&term_count), sizeof(term_count));
    in.read(reinterpret_cast<char *>(&docs_count), sizeof(docs_count));
    forward_index fwd(term_count);
    fwd.resize(docs_count);
    for (uint32_t doc_id = 0; doc_id < docs_count; ++doc_id) {
        size_t block_size;
        in.read(reinterpret_cast<char *>(&term_count), sizeof(term_count));
        in.read(reinterpret_cast<char *>(&block_size), sizeof(block_size));
        fwd[doc_id] = doc_entry{doc_id, 0.0, term_count, std::vector<uint8_t>(block_size)};
        in.read(reinterpret_cast<char *>(fwd[doc_id].terms_compressed.data()), block_size);
    }
    return fwd;
}

forward_index &forward_index::compress(forward_index &fwd)
{
    progress p("Compressing forward index", fwd.size());
    for (uint32_t doc = 0u; doc < fwd.size(); ++doc) {
        auto& terms_compressed = fwd[doc].terms_compressed;
        std::vector<uint32_t> terms(terms_compressed.size() * 5);
        size_t n = 0;
        TightVariableByte::decode(
            terms_compressed.data(), terms.data(), terms_compressed.size(), n);
        fwd[doc].term_count = n;
        terms_compressed.clear();
        terms_compressed.resize(2 * n * sizeof(uint32_t));
        VarIntGB<false> varintgb_codec;
        size_t byte_size = varintgb_codec.encodeArray(terms.data(), n, terms_compressed.data());
        terms_compressed.resize(byte_size);
        terms_compressed.shrink_to_fit();
        p.update(1);
    }
    return fwd;
}

forward_index forward_index::from_inverted_index(const std::string &input_basename,
                                                 size_t             min_len) {
    binary_collection coll((input_basename + ".docs").c_str());

    auto firstseq = *coll.begin();
    if (firstseq.size() != 1) {
        throw std::invalid_argument("First sequence should only contain number of documents");
    }
    auto num_docs  = *firstseq.begin();
    auto num_terms = std::distance(++coll.begin(), coll.end());

    forward_index fwd(num_terms);
    fwd.resize(num_docs);
    {
        progress p("Building forward index", num_terms);
        uint32_t tid = 0;
        std::vector<uint32_t> prev(num_docs, 0u);
        for (auto it = ++coll.begin(); it != coll.end(); ++it) {
            for (const auto &d : *it) {
                fwd[d].id = d;
                if (it->size() >= min_len) {
                    TightVariableByte::encode_single(tid - prev[d], fwd[d].terms_compressed);
                    prev[d] = tid;
                }
            }
            p.update(1);
            ++tid;
        }
    }
    compress(fwd);

    return fwd;
}

} // namespace bp

struct doc_ref {
    explicit doc_ref(bp::doc_entry *d) : ref(d) {}
    doc_ref()                = default;
    doc_ref(const doc_ref &) = default;
    doc_ref(doc_ref &&)      = default;
    doc_ref &operator=(const doc_ref &) = default;
    doc_ref &operator=(doc_ref &&) = default;
    ~doc_ref()                     = default;

    bp::doc_entry *ref;

    uint32_t id() const { return ref->id; }
    double   gain() const { return ref->gain; }
    void     update_gain(double gain) { ref->gain = gain; }

    std::vector<uint32_t> terms() const { return ref->terms(); }

    static bool by_gain(const doc_ref &lhs, const doc_ref &rhs) { return lhs.gain() > rhs.gain(); }
    static auto by_id(const doc_ref &lhs, const doc_ref &rhs) { return lhs.id() < rhs.id(); }
};


struct degree_map_pair {
    std::vector<size_t> left;
    std::vector<size_t> right;
};

template <class Iterator>
struct document_partition;

template <class Iterator>
struct document_range {
    int      id;
    Iterator first;
    Iterator last;
    size_t   term_count;

    Iterator       begin() { return first; }
    Iterator       end() { return last; }
    std::ptrdiff_t size() const { return std::distance(first, last); }

    document_partition<Iterator> split() const {
        auto     left_id  = 2 * id;
        auto     right_id = left_id + 1;
        Iterator mid      = std::next(first, std::distance(first, last) / 2);
        return {
            {left_id, first, mid, term_count}, {right_id, mid, last, term_count}, term_count};
    }
};

template <class Iterator>
struct document_partition {
    document_range<Iterator> left;
    document_range<Iterator> right;
    size_t                   term_count;
};

auto get_mapping = [](const auto &collection) {
    std::vector<uint32_t> mapping(collection.size(), 0u);
    size_t                p = 0;
    for (const auto &i : collection) {
        mapping[i.id()] = p++;
    }
    return mapping;
};

template <class Iterator>
std::vector<size_t> compute_degrees(document_range<Iterator> &range) {
    std::vector<size_t> deg_map(range.term_count);
    for (const auto &document : range) {
        for (const auto &term : document.terms()) {
            deg_map[term] += 1;
        }
    }
    return deg_map;
}

template <class Iterator>
degree_map_pair compute_degrees(document_partition<Iterator> &partition) {
    std::vector<size_t> left_degree;
    std::vector<size_t> right_degree;
    tbb::parallel_invoke([&] { left_degree = compute_degrees(partition.left); },
                         [&] { right_degree = compute_degrees(partition.right); });
    return degree_map_pair{left_degree, right_degree};
}

template <typename Iter>
using gain_function_t = std::function<void(Iter,
                                           Iter,
                                           const std::ptrdiff_t,
                                           const std::ptrdiff_t,
                                           const std::vector<size_t> &,
                                           const std::vector<size_t> &)>;

template<class T>
class cache_entry {
   public:
    cache_entry() : m_value(), m_has_value(false) {}

    const T &value() { return m_value; }
    bool     has_value() { return m_has_value; }
    void     operator=(const T &v) {
        m_value     = v;
        m_has_value = true;
    }

   private:
    T m_value;
    bool m_has_value;
};

template <typename Iter>
void compute_move_gains_caching(Iter                       begin,
                                Iter                       end,
                                const std::ptrdiff_t       from_n,
                                const std::ptrdiff_t       to_n,
                                const std::vector<size_t> &from_lex,
                                const std::vector<size_t> &to_lex) {
    const auto logn1 = log2(from_n);
    const auto logn2 = log2(to_n);

    std::vector<cache_entry<double>> gain_cache(from_lex.size());
    auto       compute_document_gain = [&](auto &d) {
        double gain = 0.0;
        for (const auto &t : d.terms()) {
            if (not gain_cache[t].has_value()) {
                auto from_deg = from_lex[t];
                auto to_deg   = to_lex[t];
                assert(from_deg > 0);

                auto term_gain = bp::expb(logn1, logn2, from_deg, to_deg) -
                                 bp::expb(logn1, logn2, from_deg - 1, to_deg + 1);
                gain_cache[t] = term_gain;
            }
            gain += gain_cache[t].value();
        }
        return d.update_gain(gain);
    };
    std::for_each(begin, end, compute_document_gain);
}

template <typename Iter>
void compute_move_gains(Iter                       begin,
                        Iter                       end,
                        const std::ptrdiff_t       from_n,
                        const std::ptrdiff_t       to_n,
                        const std::vector<size_t> &from_lex,
                        const std::vector<size_t> &to_lex) {
    const auto logn1 = log2(from_n);
    const auto logn2 = log2(to_n);

    auto compute_document_gain = [&](auto &d) {
        double gain = 0.0;
        for (const auto &t : d.terms()) {
            auto from_deg = from_lex[t];
            auto to_deg   = to_lex[t];
            assert(from_deg > 0);

            auto term_gain = bp::expb(logn1, logn2, from_deg, to_deg) -
                             bp::expb(logn1, logn2, from_deg - 1, to_deg + 1);
            gain += term_gain;
        }
        return d.update_gain(gain);
    };
    std::for_each(std::execution::par_unseq, begin, end, compute_document_gain);
}

template <class Iterator>
void compute_gains(document_partition<Iterator> &partition,
                   const degree_map_pair &       degrees,
                   gain_function_t<Iterator>     gain_function) {

    auto n1 = partition.left.size();
    auto n2 = partition.right.size();
    tbb::parallel_invoke(
        [&] {
            gain_function(partition.left.begin(),
                          partition.left.end(),
                          n1,
                          n2,
                          degrees.left,
                          degrees.right);
        },
        [&] {
            gain_function(partition.right.begin(),
                          partition.right.end(),
                          n2,
                          n1,
                          degrees.right,
                          degrees.left);
        });
}

template <class Iterator>
void swap(document_partition<Iterator> &partition, degree_map_pair &degrees) {
    auto left  = partition.left.begin();
    auto right = partition.right.begin();
    for (; left != partition.left.end() && right != partition.right.end(); ++left, ++right) {
        if (left->gain() + right->gain() <= 0) {
            break;
        }
        for (auto &term : left->terms()) {
            degrees.left[term]--;
            degrees.right[term]++;
        }
        for (auto &term : right->terms()) {
            degrees.left[term]++;
            degrees.right[term]--;
        }
        std::iter_swap(left, right);
    }
}

template <class Iterator>
void process_partition(document_partition<Iterator> &partition,
                       gain_function_t<Iterator>     gain_function) {

    auto degrees = compute_degrees(partition);
    for (int iteration = 0; iteration < 20; ++iteration) {
        compute_gains(partition, degrees, gain_function);
        tbb::parallel_invoke(
            [&] {
                std::sort(std::execution::par_unseq,
                          partition.left.begin(),
                          partition.left.end(),
                          doc_ref::by_gain);
            },
            [&] {
                std::sort(std::execution::par_unseq,
                          partition.right.begin(),
                          partition.right.end(),
                          doc_ref::by_gain);
            });
        swap(partition, degrees);
    }
}

template <class Iterator>
void recursive_graph_bisection(document_range<Iterator> documents, int depth, progress &p) {
    auto partition = documents.split();
    if (documents.size() > 1024) {
      process_partition(partition, compute_move_gains_caching<Iterator>);
    } else {
      process_partition(partition, compute_move_gains<Iterator>);
    }
    p.update(documents.size());
    if (depth > 1 && documents.size() > 2) {
        tbb::parallel_invoke([&] { recursive_graph_bisection(partition.left, depth - 1, p); },
                             [&] { recursive_graph_bisection(partition.right, depth - 1, p); });
    } else {
        std::sort(partition.left.begin(), partition.left.end(), doc_ref::by_id);
        std::sort(partition.right.begin(), partition.right.end(), doc_ref::by_id);
    }
}

} // namespace ds2i
