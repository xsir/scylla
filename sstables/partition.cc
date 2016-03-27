/*
 * Copyright 2015 Cloudius Systems
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "mutation.hh"
#include "sstables.hh"
#include "types.hh"
#include "core/future-util.hh"
#include "key.hh"
#include "keys.hh"
#include "core/do_with.hh"
#include "unimplemented.hh"

#include "dht/i_partitioner.hh"

namespace sstables {

/**
 * @returns: >= 0, if key is found. That is the index where the key is found.
 *             -1, if key is not found, and is smaller than the first key in the list.
 *          <= -2, if key is not found, but is greater than one of the keys. By adding 2 and
 *                 negating, one can determine the index before which the key would have to
 *                 be inserted.
 *
 * Origin uses this slightly modified binary search for the Summary, that will
 * indicate in which bucket the element would be in case it is not a match.
 *
 * For the Index entries, it uses a "normal", java.lang binary search. Because
 * we have made the explicit decision to open code the comparator for
 * efficiency, using a separate binary search would be possible, but very
 * messy.
 *
 * It's easier to reuse the same code for both binary searches, and just ignore
 * the extra information when not needed.
 *
 * This code should work in all kinds of vectors in whose's elements is possible to aquire
 * a key view via get_key().
 */
template <typename T>
int sstable::binary_search(const T& entries, const key& sk, const dht::token& token) {
    int low = 0, mid = entries.size(), high = mid - 1, result = -1;

    auto& partitioner = dht::global_partitioner();

    while (low <= high) {
        // The token comparison should yield the right result most of the time.
        // So we avoid expensive copying operations that happens at key
        // creation by keeping only a key view, and then manually carrying out
        // both parts of the comparison ourselves.
        mid = low + ((high - low) >> 1);
        key_view mid_key = entries[mid].get_key();
        auto mid_token = partitioner.get_token(mid_key);

        if (token == mid_token) {
            result = sk.tri_compare(mid_key);
        } else {
            result = token < mid_token ? -1 : 1;
        }

        if (result > 0) {
            low = mid + 1;
        } else if (result < 0) {
            high = mid - 1;
        } else {
            return mid;
        }
    }

    return -mid - (result < 0 ? 1 : 2);
}

// Force generation, so we make it available outside this compilation unit without moving that
// much code to .hh
template int sstable::binary_search<>(const std::vector<summary_entry>& entries, const key& sk);
template int sstable::binary_search<>(const std::vector<index_entry>& entries, const key& sk);

static inline bytes pop_back(std::vector<bytes>& vec) {
    auto b = std::move(vec.back());
    vec.pop_back();
    return std::move(b);
}

class mp_row_consumer : public row_consumer {
    schema_ptr _schema;
    key_view _key;
    const io_priority_class* _pc = nullptr;
    std::function<future<> (mutation&& m)> _mutation_to_subscription;

    struct column {
        bool is_static;
        bytes_view col_name;
        std::vector<bytes> clustering;
        // see is_collection. collections have an extra element aside from the name.
        // This will be non-zero size if this is a collection, and zero size othersize.
        bytes collection_extra_data;
        bytes cell;
        const column_definition *cdef;

        static constexpr size_t static_size = 2;

        // For every normal column, we expect the clustering key, followed by the
        // extra element for the column name.
        //
        // For a collection, some auxiliary data will be embedded into the
        // column_name as seen by the row consumer. This means that if our
        // exploded clustering keys has more rows than expected, we are dealing
        // with a collection.
        bool is_collection(const schema& s) {
            auto expected_normal = s.clustering_key_size() + 1;
            // Note that we can have less than the expected. That is the case for
            // incomplete prefixes, for instance.
            if (clustering.size() <= expected_normal) {
                return false;
            } else if (clustering.size() == (expected_normal + 1)) {
                return true;
            }
            throw malformed_sstable_exception(sprint("Found %d clustering elements in column name. Was not expecting that!", clustering.size()));
        }

        bool is_present(api::timestamp_type timestamp) {
            return cdef && timestamp > cdef->dropped_at();
        }

        static bool check_static(bytes_view col) {
            static bytes static_row(static_size, 0xff);
            return col.compare(0, static_size, static_row) == 0;
        }

        static bytes_view fix_static_name(bytes_view col) {
            if (check_static(col)) {
                col.remove_prefix(static_size);
            }
            return col;
        }

        std::vector<bytes> extract_clustering_key(const schema& schema) {
            if (!schema.is_compound()) {
                return { to_bytes(col_name) };
            } else {
                return composite_view(col_name).explode();
            }
        }
        column(const schema& schema, bytes_view col)
            : is_static(check_static(col))
            , col_name(fix_static_name(col))
            , clustering(extract_clustering_key(schema))
            , collection_extra_data(is_collection(schema) ? pop_back(clustering) : bytes()) // collections are not supported with COMPACT STORAGE, so this is fine
            , cell(!schema.is_dense() ? pop_back(clustering) : (*(schema.regular_begin())).name()) // dense: cell name is not provided. It is the only regular column
            , cdef(schema.get_column_definition(cell))
        {

            if (is_static) {
                for (auto& e: clustering) {
                    if (e.size() != 0) {
                        throw malformed_sstable_exception("Static row has clustering key information. I didn't expect that!");
                    }
                }
            }
        }
    };

    // Notes for collection mutation:
    //
    // While we could in theory generate the mutation for the elements as they
    // appear, that would be costly.  We would need to keep deserializing and
    // serializing them, either explicitly or through a merge.
    //
    // The best way forward is to accumulate the collection data into a data
    // structure, and later on serialize it fully when this (sstable) row ends.
    class collection_mutation {
        const column_definition *_cdef;
        exploded_clustering_prefix _clustering_prefix;
    public:
        collection_type_impl::mutation cm;

        // We need to get a copy of the prefix here, because the outer object may be short lived.
        collection_mutation(exploded_clustering_prefix prefix, const column_definition *cdef)
            : _cdef(cdef)
            , _clustering_prefix(std::move(prefix)) { }

        collection_mutation() : _cdef(nullptr) {}

        bool is_new_collection(const exploded_clustering_prefix& prefix, const column_definition *c) {
            if (prefix.components() != _clustering_prefix.components()) {
                return true;
            }
            if (!_cdef || ((_cdef->id != c->id) || (_cdef->kind != c->kind))) {
                return true;
            }
            return false;
        };

        void flush(const schema& s, mutation& mut) {
            if (!_cdef) {
                return;
            }
            auto ctype = static_pointer_cast<const collection_type_impl>(_cdef->type);
            auto ac = atomic_cell_or_collection::from_collection_mutation(ctype->serialize_mutation_form(cm));
            if (_cdef->is_static()) {
                mut.set_static_cell(*_cdef, std::move(ac));
            } else {
                auto ckey = clustering_key::from_clustering_prefix(s, _clustering_prefix);
                mut.set_clustered_cell(ckey, *_cdef, std::move(ac));
            }
        }
    };
    std::experimental::optional<collection_mutation> _pending_collection = {};

    collection_mutation& pending_collection(const exploded_clustering_prefix& clustering_prefix, const column_definition *cdef) {
        if (!_pending_collection || _pending_collection->is_new_collection(clustering_prefix, cdef)) {
            flush_pending_collection(*_schema, *mut);

            if (!cdef->type->is_multi_cell()) {
                throw malformed_sstable_exception("frozen set should behave like a cell\n");
            }
            _pending_collection = collection_mutation(clustering_prefix, cdef);
        }
        return *_pending_collection;
    }

    void update_pending_collection(const exploded_clustering_prefix& clustering_prefix, const column_definition *cdef,
                                   bytes&& col, atomic_cell&& ac) {
        pending_collection(clustering_prefix, cdef).cm.cells.emplace_back(std::move(col), std::move(ac));
    }

    void update_pending_collection(const exploded_clustering_prefix& clustering_prefix, const column_definition *cdef, tombstone&& t) {
        pending_collection(clustering_prefix, cdef).cm.tomb = std::move(t);
    }

    void flush_pending_collection(const schema& s, mutation& mut) {
        if (_pending_collection) {
            _pending_collection->flush(s, mut);
            _pending_collection = {};
        }
    }
public:
    mutation_opt mut;

    mp_row_consumer(const key& key, const schema_ptr _schema, const io_priority_class& pc)
            : _schema(_schema)
            , _key(key_view(key))
            , _pc(&pc)
            , mut(mutation(partition_key::from_exploded(*_schema, key.explode(*_schema)), _schema))
    { }

    mp_row_consumer(const schema_ptr _schema, const io_priority_class& pc)
            : _schema(_schema)
            , _pc(&pc)
    { }

    mp_row_consumer(const schema_ptr _schema, std::function<future<> (mutation&& m)> sub_fn,
                    const io_priority_class& pc)
            : _schema(_schema)
            , _pc(&pc)
            , _mutation_to_subscription(sub_fn)
    { }

    mp_row_consumer() {}

    virtual void consume_row_start(sstables::key_view key, sstables::deletion_time deltime) override {
        if (_key.empty()) {
            mut = mutation(partition_key::from_exploded(*_schema, key.explode(*_schema)), _schema);
        } else if (key != _key) {
            throw malformed_sstable_exception(sprint("Key mismatch. Got %s while processing %s", to_hex(bytes_view(key)).c_str(), to_hex(bytes_view(_key)).c_str()));
        }

        if (!deltime.live()) {
            mut->partition().apply(tombstone(deltime));
        }
    }

    atomic_cell make_atomic_cell(uint64_t timestamp, bytes_view value, uint32_t ttl, uint32_t expiration) {
        if (ttl) {
            return atomic_cell::make_live(timestamp, value,
                gc_clock::time_point(gc_clock::duration(expiration)), gc_clock::duration(ttl));
        } else {
            return atomic_cell::make_live(timestamp, value);
        }
    }

    virtual void consume_cell(bytes_view col_name, bytes_view value, int64_t timestamp, int32_t ttl, int32_t expiration) override {
        struct column col(*_schema, col_name);

        auto clustering_prefix = exploded_clustering_prefix(std::move(col.clustering));

        if (col.cell.size() == 0) {
            auto clustering_key = clustering_key::from_clustering_prefix(*_schema, clustering_prefix);
            auto& dr = mut->partition().clustered_row(clustering_key);
            row_marker rm(timestamp, gc_clock::duration(ttl), gc_clock::time_point(gc_clock::duration(expiration)));
            dr.apply(rm);
            return;
        }

        if (!col.is_present(timestamp)) {
            return;
        }

        auto ac = make_atomic_cell(timestamp, value, ttl, expiration);

        bool is_multi_cell = col.collection_extra_data.size();
        if (is_multi_cell != col.cdef->type->is_multi_cell()) {
            return;
        }
        if (is_multi_cell) {
            update_pending_collection(clustering_prefix, col.cdef, std::move(col.collection_extra_data), std::move(ac));
            return;
        }

        if (col.is_static) {
            mut->set_static_cell(*(col.cdef), std::move(ac));
            return;
        }

        mut->set_cell(clustering_prefix, *(col.cdef), atomic_cell_or_collection(std::move(ac)));
    }

    virtual void consume_deleted_cell(bytes_view col_name, sstables::deletion_time deltime) override {
        struct column col(*_schema, col_name);
        gc_clock::duration secs(deltime.local_deletion_time);

        consume_deleted_cell(col, deltime.marked_for_delete_at, gc_clock::time_point(secs));
    }

    void consume_deleted_cell(column &col, int64_t timestamp, gc_clock::time_point ttl) {
        auto clustering_prefix = exploded_clustering_prefix(std::move(col.clustering));
        if (col.cell.size() == 0) {
            auto clustering_key = clustering_key::from_clustering_prefix(*_schema, clustering_prefix);
            auto& dr = mut->partition().clustered_row(clustering_key);
            row_marker rm(tombstone(timestamp, ttl));
            dr.apply(rm);
            return;
        }
        if (!col.is_present(timestamp)) {
            return;
        }

        auto ac = atomic_cell::make_dead(timestamp, ttl);

        bool is_multi_cell = col.collection_extra_data.size();
        if (is_multi_cell != col.cdef->type->is_multi_cell()) {
            return;
        }

        if (is_multi_cell) {
            update_pending_collection(clustering_prefix, col.cdef, std::move(col.collection_extra_data), std::move(ac));
        } else if (col.is_static) {
            mut->set_static_cell(*(col.cdef), atomic_cell_or_collection(std::move(ac)));
        } else {
            mut->set_cell(clustering_prefix, *(col.cdef), atomic_cell_or_collection(std::move(ac)));
        }
    }
    virtual proceed consume_row_end() override {
        if (mut) {
            flush_pending_collection(*_schema, *mut);
        }
        return proceed::no;
    }

    virtual void consume_range_tombstone(
            bytes_view start_col, bytes_view end_col,
            sstables::deletion_time deltime) override {
        check_marker(end_col, composite_marker::end_range);
        // Some versions of Cassandra will write a 0 to mark the start of the range.
        // CASSANDRA-7593 discusses that.
        check_marker(start_col, composite_marker::start_range, composite_marker::none);

        // FIXME: CASSANDRA-6237 says support will be added to things like this.
        //
        // The check below represents a range with a different start and end
        // clustering key.  Cassandra-generated files (to the moment) will
        // generate multi-row deletes, but they always have the same clustering
        // key. This is basically because one can't (yet) write delete
        // statements in which the WHERE clause looks like WHERE clustering_key >= x.
        //
        // We don't really have it in our model ATM, so let's just mark this unimplemented.
        //
        // The only expected difference between them, is the final marker. We
        // will remove it from end_col to ease the comparison, but will leave
        // start_col untouched to make sure explode() still works.
        end_col.remove_suffix(1);
        if (start_col.compare(0, end_col.size(), end_col)) {
            fail(unimplemented::cause::RANGE_DELETES);
        }

        auto start = composite_view(column::fix_static_name(start_col)).explode();
        // Note how this is slightly different from the check in is_collection. Collection tombstones
        // do not have extra data.
        //
        // Still, it is enough to check if we're dealing with a collection, since any other tombstone
        // won't have a full clustering prefix (otherwise it isn't a range)
        if (start.size() <= _schema->clustering_key_size()) {
            mut->partition().apply_delete(*_schema, exploded_clustering_prefix(std::move(start)), tombstone(deltime));
        } else {
            auto&& column = pop_back(start);
            auto cdef = _schema->get_column_definition(column);
            if (cdef && cdef->type->is_multi_cell() && deltime.marked_for_delete_at > cdef->dropped_at()) {
                auto clustering_prefix = exploded_clustering_prefix(std::move(start));
                update_pending_collection(clustering_prefix, cdef, tombstone(deltime));
            }
        }
    }
    virtual const io_priority_class& io_priority() override {
        assert (_pc != nullptr);
        return *_pc;
    }
};

static int adjust_binary_search_index(int idx) {
    if (idx < 0) {
        // binary search gives us the first index _greater_ than the key searched for,
        // i.e., its insertion position
        auto gt = (idx + 1) * -1;
        idx = gt - 1;
    }
    return idx;
}

future<uint64_t> sstables::sstable::data_end_position(uint64_t summary_idx, uint64_t index_idx, const index_list& il,
                                                      const io_priority_class& pc) {
    if (uint64_t(index_idx + 1) < il.size()) {
        return make_ready_future<uint64_t>(il[index_idx + 1].position());
    }

    return data_end_position(summary_idx, pc);
}

future<uint64_t> sstables::sstable::data_end_position(uint64_t summary_idx, const io_priority_class& pc) {
    // We should only go to the end of the file if we are in the last summary group.
    // Otherwise, we will determine the end position of the current data read by looking
    // at the first index in the next summary group.
    if (size_t(summary_idx + 1) >= _summary.entries.size()) {
        return make_ready_future<uint64_t>(data_size());
    }

    return read_indexes(summary_idx + 1, pc).then([] (auto next_il) {
        return next_il.front().position();
    });
}

future<mutation_opt>
sstables::sstable::read_row(schema_ptr schema, const sstables::key& key, const io_priority_class& pc) {

    assert(schema);

    if (!filter_has_key(key)) {
        return make_ready_future<mutation_opt>();
    }

    auto& partitioner = dht::global_partitioner();
    auto token = partitioner.get_token(key_view(key));

    auto& summary = _summary;
    auto summary_idx = adjust_binary_search_index(binary_search(summary.entries, key, token));
    if (summary_idx < 0) {
        _filter_tracker.add_false_positive();
        return make_ready_future<mutation_opt>();
    }

    return read_indexes(summary_idx, pc).then([this, schema, &key, token, summary_idx, &pc] (auto index_list) {
        auto index_idx = this->binary_search(index_list, key, token);
        if (index_idx < 0) {
            _filter_tracker.add_false_positive();
            return make_ready_future<mutation_opt>();
        }
        _filter_tracker.add_true_positive();

        auto position = index_list[index_idx].position();
        return this->data_end_position(summary_idx, index_idx, index_list, pc).then([&key, schema, this, position, &pc] (uint64_t end) {
            return do_with(mp_row_consumer(key, schema, pc), [this, position, end] (auto& c) {
                return this->data_consume_rows_at_once(c, position, end).then([&c] {
                    return make_ready_future<mutation_opt>(std::move(c.mut));
                });
            });
        });
    });
}

class mutation_reader::impl {
private:
    mp_row_consumer _consumer;
    std::experimental::optional<data_consume_context> _context;
    std::function<future<data_consume_context> ()> _get_context;
public:
    impl(sstable& sst, schema_ptr schema, uint64_t start, uint64_t end,
         const io_priority_class &pc)
        : _consumer(schema, pc)
        , _get_context([&sst, this, start, end] {
            return make_ready_future<data_consume_context>(sst.data_consume_rows(_consumer, start, end));
        }) { }
    impl(sstable& sst, schema_ptr schema,
         const io_priority_class &pc)
        : _consumer(schema, pc)
        , _get_context([this, &sst] {
            return make_ready_future<data_consume_context>(sst.data_consume_rows(_consumer));
        }) { }
    impl(sstable& sst, schema_ptr schema, std::function<future<uint64_t>()> start, std::function<future<uint64_t>()> end, const io_priority_class& pc)
        : _consumer(schema, pc)
        , _get_context([this, &sst, start = std::move(start), end = std::move(end)] () {
            return start().then([this, &sst, end = std::move(end)] (uint64_t start) {
                return end().then([this, &sst, start] (uint64_t end) {
                    return make_ready_future<data_consume_context>(sst.data_consume_rows(_consumer, start, end));
                });
            });
        }) { }
    impl() : _consumer(), _get_context() { }

    // Reference to _consumer is passed to data_consume_rows() in the constructor so we must not allow move/copy
    impl(impl&&) = delete;
    impl(const impl&) = delete;

    future<mutation_opt> read() {
        if (!_get_context) {
            // empty mutation reader returns EOF immediately
            return make_ready_future<mutation_opt>();
        }

        if (_context) {
            return do_read();
        }
        return (_get_context)().then([this] (data_consume_context context) {
            _context = std::move(context);
            return do_read();
        });
    }
private:
    future<mutation_opt> do_read() {
        return _context->read().then([this] {
            // We want after returning a mutation that _consumer.mut()
            // will be left in unengaged state (so on EOF we return an
            // unengaged optional). Moving _consumer.mut is *not* enough.
            auto ret = std::move(_consumer.mut);
            _consumer.mut = {};
            return std::move(ret);
        });
    }
};

mutation_reader::~mutation_reader() = default;
mutation_reader::mutation_reader(mutation_reader&&) = default;
mutation_reader& mutation_reader::operator=(mutation_reader&&) = default;
mutation_reader::mutation_reader(std::unique_ptr<impl> p)
    : _pimpl(std::move(p)) { }
future<mutation_opt> mutation_reader::read() {
    return _pimpl->read();
}

mutation_reader sstable::read_rows(schema_ptr schema, const io_priority_class& pc) {
    return std::make_unique<mutation_reader::impl>(*this, schema, pc);
}

// Less-comparator for lookups in the partition index.
class index_comparator {
    const schema& _s;
public:
    index_comparator(const schema& s) : _s(s) {}

    int tri_cmp(key_view k2, const dht::ring_position& pos) const {
        auto k2_token = dht::global_partitioner().get_token(k2);

        if (k2_token == pos.token()) {
            if (pos.has_key()) {
                return k2.tri_compare(_s, *pos.key());
            } else {
                return -pos.relation_to_keys();
            }
        } else {
            return k2_token < pos.token() ? -1 : 1;
        }
    }

    bool operator()(const summary_entry& e, const dht::ring_position& rp) const {
        return tri_cmp(e.get_key(), rp) < 0;
    }

    bool operator()(const index_entry& e, const dht::ring_position& rp) const {
        return tri_cmp(e.get_key(), rp) < 0;
    }

    bool operator()(const dht::ring_position& rp, const summary_entry& e) const {
        return tri_cmp(e.get_key(), rp) > 0;
    }

    bool operator()(const dht::ring_position& rp, const index_entry& e) const {
        return tri_cmp(e.get_key(), rp) > 0;
    }
};

future<uint64_t> sstable::lower_bound(schema_ptr s, const dht::ring_position& pos, const io_priority_class& pc) {
    uint64_t summary_idx = std::distance(std::begin(_summary.entries),
        std::lower_bound(_summary.entries.begin(), _summary.entries.end(), pos, index_comparator(*s)));

    if (summary_idx == 0) {
        return make_ready_future<uint64_t>(0);
    }

    --summary_idx;

    return read_indexes(summary_idx, pc).then([this, s, pos, summary_idx, &pc] (index_list il) {
        auto i = std::lower_bound(il.begin(), il.end(), pos, index_comparator(*s));
        if (i == il.end()) {
            return this->data_end_position(summary_idx, pc);
        }
        return make_ready_future<uint64_t>(i->position());
    });
}

future<uint64_t> sstable::upper_bound(schema_ptr s, const dht::ring_position& pos, const io_priority_class& pc) {
    uint64_t summary_idx = std::distance(std::begin(_summary.entries),
        std::upper_bound(_summary.entries.begin(), _summary.entries.end(), pos, index_comparator(*s)));

    if (summary_idx == 0) {
        return make_ready_future<uint64_t>(0);
    }

    --summary_idx;

    return read_indexes(summary_idx, pc).then([this, s, pos, summary_idx, &pc] (index_list il) {
        auto i = std::upper_bound(il.begin(), il.end(), pos, index_comparator(*s));
        if (i == il.end()) {
            return this->data_end_position(summary_idx, pc);
        }
        return make_ready_future<uint64_t>(i->position());
    });
}

mutation_reader sstable::read_range_rows(schema_ptr schema,
        const dht::token& min_token, const dht::token& max_token, const io_priority_class& pc) {
    if (max_token < min_token) {
        return std::make_unique<mutation_reader::impl>();
    }
    return read_range_rows(std::move(schema),
        query::range<dht::ring_position>::make(
            dht::ring_position::starting_at(min_token),
            dht::ring_position::ending_at(max_token)), pc);
}

mutation_reader
sstable::read_range_rows(schema_ptr schema, const query::partition_range& range, const io_priority_class& pc) {
    if (query::is_wrap_around(range, *schema)) {
        fail(unimplemented::cause::WRAP_AROUND);
    }

    auto start = [this, range, schema, &pc] {
        return range.start() ? (range.start()->is_inclusive()
                 ? lower_bound(schema, range.start()->value(), pc)
                 : upper_bound(schema, range.start()->value(), pc))
        : make_ready_future<uint64_t>(0);
    };

    auto end = [this, range, schema, &pc] {
        return range.end() ? (range.end()->is_inclusive()
                 ? upper_bound(schema, range.end()->value(), pc)
                 : lower_bound(schema, range.end()->value(), pc))
        : make_ready_future<uint64_t>(data_size());
    };

    return std::make_unique<mutation_reader::impl>(
        *this, std::move(schema), std::move(start), std::move(end), pc);
}


class key_reader final : public ::key_reader::impl {
    schema_ptr _s;
    shared_sstable _sst;
    index_list _bucket;
    int64_t _current_bucket_id;
    int64_t _end_bucket_id;
    int64_t _begin_bucket_id;
    int64_t _position_in_bucket = 0;
    int64_t _end_of_bucket = 0;
    query::partition_range _range;
    const io_priority_class& _pc;
private:
    dht::decorated_key decorate(const index_entry& ie) {
        auto pk = partition_key::from_exploded(*_s, ie.get_key().explode(*_s));
        return dht::global_partitioner().decorate_key(*_s, std::move(pk));
    }
public:
    key_reader(schema_ptr s, shared_sstable sst, const query::partition_range& range, const io_priority_class& pc)
        : _s(s), _sst(std::move(sst)), _range(range), _pc(pc)
    {
        auto& summary = _sst->_summary;
        using summary_entries_type = std::decay_t<decltype(summary.entries)>;

        _begin_bucket_id = 0;
        if (range.start()) {
            summary_entries_type::iterator pos;
            if (range.start()->is_inclusive()) {
                pos = std::lower_bound(summary.entries.begin(), summary.entries.end(),
                    range.start()->value(), index_comparator(*s));

            } else {
                pos = std::upper_bound(summary.entries.begin(), summary.entries.end(),
                    range.start()->value(), index_comparator(*s));
            }
            _begin_bucket_id = std::distance(summary.entries.begin(), pos);
            if (_begin_bucket_id) {
                _begin_bucket_id--;
            }
        }
        _current_bucket_id = _begin_bucket_id - 1;

        _end_bucket_id = summary.header.size;
        if (range.end()) {
            summary_entries_type::iterator pos;
            if (range.end()->is_inclusive()) {
                pos = std::upper_bound(summary.entries.begin(), summary.entries.end(),
                    range.end()->value(), index_comparator(*s));
            } else {
                pos = std::lower_bound(summary.entries.begin(), summary.entries.end(),
                    range.end()->value(), index_comparator(*s));
            }
            _end_bucket_id = std::distance(summary.entries.begin(), pos);
            if (_end_bucket_id) {
                _end_bucket_id--;
            }
        }
    }
    virtual future<dht::decorated_key_opt> operator()() override;
};

future<dht::decorated_key_opt> key_reader::operator()()
{
    if (_position_in_bucket < _end_of_bucket) {
        auto& ie = _bucket[_position_in_bucket++];
        return make_ready_future<dht::decorated_key_opt>(decorate(ie));
    }
    if (_current_bucket_id == _end_bucket_id) {
        return make_ready_future<dht::decorated_key_opt>();
    }
    return _sst->read_indexes(++_current_bucket_id, _pc).then([this] (index_list il) mutable {
        _bucket = std::move(il);

        if (_range.start() && _current_bucket_id == _begin_bucket_id) {
            index_list::const_iterator pos;
            if (_range.start()->is_inclusive()) {
                pos = std::lower_bound(_bucket.begin(), _bucket.end(), _range.start()->value(), index_comparator(*_s));
            } else {
                pos = std::upper_bound(_bucket.begin(), _bucket.end(), _range.start()->value(), index_comparator(*_s));
            }
            _position_in_bucket = std::distance(_bucket.cbegin(), pos);
        } else {
            _position_in_bucket = 0;
        }

        if (_range.end() && _current_bucket_id == _end_bucket_id) {
            index_list::const_iterator pos;
            if (_range.end()->is_inclusive()) {
                pos = std::upper_bound(_bucket.begin(), _bucket.end(), _range.end()->value(), index_comparator(*_s));
            } else {
                pos = std::lower_bound(_bucket.begin(), _bucket.end(), _range.end()->value(), index_comparator(*_s));
            }
            _end_of_bucket = std::distance(_bucket.cbegin(), pos);
        } else {
            _end_of_bucket = _bucket.size();
        }

        return operator()();
    });
}

::key_reader make_key_reader(schema_ptr s, shared_sstable sst, const query::partition_range& range, const io_priority_class& pc)
{
    return ::make_key_reader<key_reader>(std::move(s), std::move(sst), range, pc);
}

}
