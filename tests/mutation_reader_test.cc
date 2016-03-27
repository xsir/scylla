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

#define BOOST_TEST_DYN_LINK

#include <boost/test/unit_test.hpp>

#include "tests/test-utils.hh"
#include "tests/mutation_assertions.hh"
#include "tests/mutation_reader_assertions.hh"

#include "mutation_reader.hh"
#include "core/do_with.hh"
#include "core/thread.hh"
#include "schema_builder.hh"

#include "disk-error-handler.hh"

thread_local disk_error_signal_type commit_error;
thread_local disk_error_signal_type general_disk_error;

static schema_ptr make_schema() {
    return schema_builder("ks", "cf")
        .with_column("pk", bytes_type, column_kind::partition_key)
        .with_column("v", bytes_type, column_kind::regular_column)
        .build();
}

SEASTAR_TEST_CASE(test_combining_two_readers_with_the_same_row) {
    return seastar::async([] {
        auto s = make_schema();

        mutation m1(partition_key::from_single_value(*s, "key1"), s);
        m1.set_clustered_cell(clustering_key::make_empty(*s), "v", data_value(bytes("v1")), 1);

        mutation m2(partition_key::from_single_value(*s, "key1"), s);
        m2.set_clustered_cell(clustering_key::make_empty(*s), "v", data_value(bytes("v2")), 2);

        assert_that(make_combined_reader(make_reader_returning(m1), make_reader_returning(m2)))
            .produces(m2)
            .produces_end_of_stream();
    });
}

SEASTAR_TEST_CASE(test_combining_two_non_overlapping_readers) {
    return seastar::async([] {
        auto s = make_schema();

        mutation m1(partition_key::from_single_value(*s, "keyB"), s);
        m1.set_clustered_cell(clustering_key::make_empty(*s), "v", data_value(bytes("v1")), 1);

        mutation m2(partition_key::from_single_value(*s, "keyA"), s);
        m2.set_clustered_cell(clustering_key::make_empty(*s), "v", data_value(bytes("v2")), 2);

        auto cr = make_combined_reader(make_reader_returning(m1), make_reader_returning(m2));
        assert_that(std::move(cr))
            .produces(m2)
            .produces(m1)
            .produces_end_of_stream();
    });
}

SEASTAR_TEST_CASE(test_combining_two_partially_overlapping_readers) {
    return seastar::async([] {
        auto s = make_schema();

        mutation m1(partition_key::from_single_value(*s, "keyA"), s);
        m1.set_clustered_cell(clustering_key::make_empty(*s), "v", data_value(bytes("v1")), 1);

        mutation m2(partition_key::from_single_value(*s, "keyB"), s);
        m2.set_clustered_cell(clustering_key::make_empty(*s), "v", data_value(bytes("v2")), 1);

        mutation m3(partition_key::from_single_value(*s, "keyC"), s);
        m3.set_clustered_cell(clustering_key::make_empty(*s), "v", data_value(bytes("v3")), 1);

        assert_that(make_combined_reader(make_reader_returning_many({m1, m2}), make_reader_returning_many({m2, m3})))
            .produces(m1)
            .produces(m2)
            .produces(m3)
            .produces_end_of_stream();
    });
}

SEASTAR_TEST_CASE(test_combining_one_reader_with_many_partitions) {
    return seastar::async([] {
        auto s = make_schema();

        mutation m1(partition_key::from_single_value(*s, "keyA"), s);
        m1.set_clustered_cell(clustering_key::make_empty(*s), "v", data_value(bytes("v1")), 1);

        mutation m2(partition_key::from_single_value(*s, "keyB"), s);
        m2.set_clustered_cell(clustering_key::make_empty(*s), "v", data_value(bytes("v2")), 1);

        mutation m3(partition_key::from_single_value(*s, "keyC"), s);
        m3.set_clustered_cell(clustering_key::make_empty(*s), "v", data_value(bytes("v3")), 1);

        std::vector<mutation_reader> v;
        v.push_back(make_reader_returning_many({m1, m2, m3}));
        assert_that(make_combined_reader(std::move(v)))
            .produces(m1)
            .produces(m2)
            .produces(m3)
            .produces_end_of_stream();
    });
}

static mutation make_mutation_with_key(schema_ptr s, sstring key) {
    mutation m(partition_key::from_single_value(*s, to_bytes(key)), s);
    m.set_clustered_cell(clustering_key::make_empty(*s), "v", data_value(bytes("v1")), 1);
    return m;
}

SEASTAR_TEST_CASE(test_filtering) {
    return seastar::async([] {
        auto s = make_schema();

        auto m1 = make_mutation_with_key(s, "key1");
        auto m2 = make_mutation_with_key(s, "key2");
        auto m3 = make_mutation_with_key(s, "key3");
        auto m4 = make_mutation_with_key(s, "key4");

        // All pass
        assert_that(make_filtering_reader(make_reader_returning_many({m1, m2, m3, m4}),
                 [] (const mutation& m) { return true; }))
            .produces(m1)
            .produces(m2)
            .produces(m3)
            .produces(m4)
            .produces_end_of_stream();

        // None pass
        assert_that(make_filtering_reader(make_reader_returning_many({m1, m2, m3, m4}),
                 [] (const mutation& m) { return false; }))
            .produces_end_of_stream();

        // Trim front
        assert_that(make_filtering_reader(make_reader_returning_many({m1, m2, m3, m4}),
                [&] (const mutation& m) { return !m.key().equal(*s, m1.key()); }))
            .produces(m2)
            .produces(m3)
            .produces(m4)
            .produces_end_of_stream();

        assert_that(make_filtering_reader(make_reader_returning_many({m1, m2, m3, m4}),
            [&] (const mutation& m) { return !m.key().equal(*s, m1.key()) && !m.key().equal(*s, m2.key()); }))
            .produces(m3)
            .produces(m4)
            .produces_end_of_stream();

        // Trim back
        assert_that(make_filtering_reader(make_reader_returning_many({m1, m2, m3, m4}),
                 [&] (const mutation& m) { return !m.key().equal(*s, m4.key()); }))
            .produces(m1)
            .produces(m2)
            .produces(m3)
            .produces_end_of_stream();

        assert_that(make_filtering_reader(make_reader_returning_many({m1, m2, m3, m4}),
                 [&] (const mutation& m) { return !m.key().equal(*s, m4.key()) && !m.key().equal(*s, m3.key()); }))
            .produces(m1)
            .produces(m2)
            .produces_end_of_stream();

        // Trim middle
        assert_that(make_filtering_reader(make_reader_returning_many({m1, m2, m3, m4}),
                 [&] (const mutation& m) { return !m.key().equal(*s, m3.key()); }))
            .produces(m1)
            .produces(m2)
            .produces(m4)
            .produces_end_of_stream();

        assert_that(make_filtering_reader(make_reader_returning_many({m1, m2, m3, m4}),
                 [&] (const mutation& m) { return !m.key().equal(*s, m2.key()) && !m.key().equal(*s, m3.key()); }))
            .produces(m1)
            .produces(m4)
            .produces_end_of_stream();
    });
}

SEASTAR_TEST_CASE(test_combining_two_readers_with_one_reader_empty) {
    return seastar::async([] {
        auto s = make_schema();
        mutation m1(partition_key::from_single_value(*s, "key1"), s);
        m1.set_clustered_cell(clustering_key::make_empty(*s), "v", data_value(bytes("v1")), 1);

        assert_that(make_combined_reader(make_reader_returning(m1), make_empty_reader()))
            .produces(m1)
            .produces_end_of_stream();
    });
}

SEASTAR_TEST_CASE(test_combining_two_empty_readers) {
    return seastar::async([] {
        assert_that(make_combined_reader(make_empty_reader(), make_empty_reader()))
            .produces_end_of_stream();
    });
}

SEASTAR_TEST_CASE(test_combining_one_empty_reader) {
    return seastar::async([] {
        std::vector<mutation_reader> v;
        v.push_back(make_empty_reader());
        assert_that(make_combined_reader(std::move(v)))
            .produces_end_of_stream();
    });
}

SEASTAR_TEST_CASE(test_joining_reader) {
    return seastar::async([] {
        auto s = make_schema();

        mutation m1(partition_key::from_single_value(*s, "keyB"), s);
        m1.set_clustered_cell(clustering_key::make_empty(*s), "v", data_value(bytes("v1")), 1);

        mutation m2(partition_key::from_single_value(*s, "keyA"), s);
        m2.set_clustered_cell(clustering_key::make_empty(*s), "v", data_value(bytes("v2")), 2);

        std::vector<mutation_reader> v;
        v.push_back(make_reader_returning(m1));
        v.push_back(make_empty_reader());
        v.push_back(make_reader_returning(m2));
        auto cr = make_joining_reader(std::move(v));
        assert_that(std::move(cr))
            .produces(m1)
            .produces(m2)
            .produces_end_of_stream();
    });
}
