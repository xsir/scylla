/*
 * Copyright (C) 2015 Cloudius Systems, Ltd.
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

#pragma once

#include "utils/data_input.hh"
#include "utils/data_output.hh"
#include "database_fwd.hh"
#include "mutation_partition_view.hh"
#include "bytes_ostream.hh"

namespace ser {
class writer_of_mutation_partition;
}

class mutation_partition_serializer {
    static size_t size(const schema&, const mutation_partition&);
public:
    using size_type = uint32_t;
private:
    const schema& _schema;
    const mutation_partition& _p;
private:
    template<typename Writer>
    static void write_serialized(Writer&& out, const schema&, const mutation_partition&);
public:
    using count_type = uint32_t;
    mutation_partition_serializer(const schema&, const mutation_partition&);
public:
    void write(bytes_ostream&) const;
    void write(ser::writer_of_mutation_partition&&) const;
};
