/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#ifndef DATABASE_HH_
#define DATABASE_HH_

#include "core/sstring.hh"
#include "core/shared_ptr.hh"
#include "net/byteorder.hh"
#include "utils/UUID.hh"
#include "db_clock.hh"
#include "gc_clock.hh"
#include <functional>
#include <boost/any.hpp>
#include <cstdint>
#include <boost/variant.hpp>
#include <unordered_map>
#include <map>
#include <set>
#include <vector>
#include <iostream>
#include <boost/functional/hash.hpp>
#include <experimental/optional>
#include <string.h>
#include "types.hh"
#include "tuple.hh"
#include "core/future.hh"
#include "cql3/column_specification.hh"
#include <limits>
#include <cstddef>
#include "schema.hh"

using partition_key_type = tuple_type<>;
using clustering_key_type = tuple_type<>;
using clustering_prefix_type = tuple_prefix;
using partition_key = bytes;
using clustering_key = bytes;
using clustering_prefix = clustering_prefix_type::value_type;

namespace api {

using timestamp_type = int64_t;
timestamp_type constexpr missing_timestamp = std::numeric_limits<timestamp_type>::min();
timestamp_type constexpr min_timestamp = std::numeric_limits<timestamp_type>::min() + 1;
timestamp_type constexpr max_timestamp = std::numeric_limits<timestamp_type>::max();

}

/**
 * Represents deletion operation. Can be commuted with other tombstones via apply() method.
 * Can be empty.
 */
struct tombstone final {
    api::timestamp_type timestamp;
    gc_clock::time_point ttl;

    tombstone(api::timestamp_type timestamp, gc_clock::time_point ttl)
        : timestamp(timestamp)
        , ttl(ttl)
    { }

    tombstone()
        : tombstone(api::missing_timestamp, {})
    { }

    int compare(const tombstone& t) const {
        if (timestamp < t.timestamp) {
            return -1;
        } else if (timestamp > t.timestamp) {
            return 1;
        } else if (ttl < t.ttl) {
            return -1;
        } else if (ttl > t.ttl) {
            return 1;
        } else {
            return 0;
        }
    }

    bool operator<(const tombstone& t) const {
        return compare(t) < 0;
    }

    bool operator<=(const tombstone& t) const {
        return compare(t) <= 0;
    }

    bool operator>(const tombstone& t) const {
        return compare(t) > 0;
    }

    bool operator>=(const tombstone& t) const {
        return compare(t) >= 0;
    }

    bool operator==(const tombstone& t) const {
        return compare(t) == 0;
    }

    bool operator!=(const tombstone& t) const {
        return compare(t) != 0;
    }

    explicit operator bool() const {
        return timestamp != api::missing_timestamp;
    }

    void apply(const tombstone& t) {
        if (*this < t) {
            *this = t;
        }
    }

    friend std::ostream& operator<<(std::ostream& out, const tombstone& t) {
        return out << "{timestamp=" << t.timestamp << ", ttl=" << t.ttl.time_since_epoch().count() << "}";
    }
};

using ttl_opt = std::experimental::optional<gc_clock::time_point>;

struct atomic_cell final {
    struct dead {
        gc_clock::time_point ttl;
    };
    struct live {
        ttl_opt ttl;
        bytes value;
    };
    api::timestamp_type timestamp;
    boost::variant<dead, live> value;
    bool is_live() const { return value.which() == 1; }
    // Call only when is_live() == true
    const live& as_live() const { return boost::get<live>(value); }
    // Call only when is_live() == false
    const dead& as_dead() const { return boost::get<dead>(value); }
};

using row = std::map<column_id, boost::any>;

struct deletable_row final {
    tombstone t;
    row cells;
};

using row_tombstone_set = std::map<bytes, tombstone, serialized_compare>;

class mutation_partition final {
private:
    tombstone _tombstone;
    row _static_row;
    std::map<clustering_key, deletable_row, key_compare> _rows;
    row_tombstone_set _row_tombstones;
public:
    mutation_partition(schema_ptr s)
        : _rows(key_compare(s->clustering_key_type))
        , _row_tombstones(serialized_compare(s->clustering_key_prefix_type))
    { }
    void apply(tombstone t) { _tombstone.apply(t); }
    void apply_delete(schema_ptr schema, const clustering_prefix& prefix, tombstone t);
    void apply_row_tombstone(schema_ptr schema, bytes prefix, tombstone t) {
        apply_row_tombstone(schema, {std::move(prefix), std::move(t)});
    }
    void apply_row_tombstone(schema_ptr schema, std::pair<bytes, tombstone> row_tombstone);
    void apply(schema_ptr schema, const mutation_partition& p);
    const row_tombstone_set& row_tombstones() const { return _row_tombstones; }
    row& static_row() { return _static_row; }
    row& clustered_row(const clustering_key& key) { return _rows[key].cells; }
    row& clustered_row(clustering_key&& key) { return _rows[std::move(key)].cells; }
    row* find_row(const clustering_key& key);
    tombstone tombstone_for_row(schema_ptr schema, const clustering_key& key);
    friend std::ostream& operator<<(std::ostream& os, const mutation_partition& mp);
};

class mutation final {
public:
    schema_ptr schema;
    partition_key key;
    mutation_partition p;
public:
    mutation(partition_key key_, schema_ptr schema_)
        : schema(std::move(schema_))
        , key(std::move(key_))
        , p(schema)
    { }

    mutation(mutation&&) = default;
    mutation(const mutation&) = default;

    void set_static_cell(const column_definition& def, boost::any value) {
        p.static_row()[def.id] = std::move(value);
    }

    void set_clustered_cell(const clustering_prefix& prefix, const column_definition& def, boost::any value) {
        auto& row = p.clustered_row(serialize_value(*schema->clustering_key_type, prefix));
        row[def.id] = std::move(value);
    }

    void set_clustered_cell(const clustering_key& key, const column_definition& def, boost::any value) {
        auto& row = p.clustered_row(key);
        row[def.id] = std::move(value);
    }
    friend std::ostream& operator<<(std::ostream& os, const mutation& m);
};

struct column_family {
    column_family(schema_ptr schema);
    mutation_partition& find_or_create_partition(const bytes& key);
    row& find_or_create_row(const bytes& partition_key, const bytes& clustering_key);
    mutation_partition* find_partition(const bytes& key);
    row* find_row(const bytes& partition_key, const bytes& clustering_key);
    schema_ptr _schema;
    // partition key -> partition
    std::map<bytes, mutation_partition, key_compare> partitions;
    void apply(const mutation& m);
};

class keyspace {
public:
    std::unordered_map<sstring, column_family> column_families;
    static future<keyspace> populate(sstring datadir);
    schema_ptr find_schema(const sstring& cf_name);
    column_family* find_column_family(const sstring& cf_name);
};

class database {
public:
    std::unordered_map<sstring, keyspace> keyspaces;
    static future<database> populate(sstring datadir);
    keyspace* find_keyspace(const sstring& name);
};


#endif /* DATABASE_HH_ */
