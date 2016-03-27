/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Copyright 2014 Cloudius Systems
 *
 * Modified by Cloudius Systems
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

#include "aggregate_function.hh"
#include "native_aggregate_function.hh"

namespace cql3 {
namespace functions {

/**
 * Factory methods for aggregate functions.
 */
namespace aggregate_fcts {

class impl_count_function : public aggregate_function::aggregate {
    int64_t _count;
public:
    virtual void reset() override {
        _count = 0;
    }
    virtual opt_bytes compute(cql_serialization_format sf) override {
        return long_type->decompose(_count);
    }
    virtual void add_input(cql_serialization_format sf, const std::vector<opt_bytes>& values) override {
        ++_count;
    }
};

    /**
     * The function used to count the number of rows of a result set. This function is called when COUNT(*) or COUNT(1)
     * is specified.
     */
inline
shared_ptr<aggregate_function>
make_count_rows_function() {
    return make_native_aggregate_function_using<impl_count_function>("countRows", long_type);
}

template <typename Type>
class impl_sum_function_for final : public aggregate_function::aggregate {
   Type _sum{};
public:
    virtual void reset() override {
        _sum = {};
    }
    virtual opt_bytes compute(cql_serialization_format sf) override {
        return data_type_for<Type>()->decompose(_sum);
    }
    virtual void add_input(cql_serialization_format sf, const std::vector<opt_bytes>& values) override {
        if (!values[0]) {
            return;
        }
        _sum += value_cast<Type>(data_type_for<Type>()->deserialize(*values[0]));
    }
};

template <typename Type>
class sum_function_for final : public native_aggregate_function {
public:
    sum_function_for() : native_aggregate_function("sum", data_type_for<Type>(), { data_type_for<Type>() }) {}
    virtual std::unique_ptr<aggregate> new_aggregate() override {
        return std::make_unique<impl_sum_function_for<Type>>();
    }
};


template <typename Type>
inline
shared_ptr<aggregate_function>
make_sum_function() {
    return make_shared<sum_function_for<Type>>();
}

template <typename Type>
class impl_avg_function_for final : public aggregate_function::aggregate {
   Type _sum{};
   int64_t _count = 0;
public:
    virtual void reset() override {
        _sum = {};
        _count = 0;
    }
    virtual opt_bytes compute(cql_serialization_format sf) override {
        Type ret = 0;
        if (_count) {
            ret = _sum / _count;
        }
        return data_type_for<Type>()->decompose(ret);
    }
    virtual void add_input(cql_serialization_format sf, const std::vector<opt_bytes>& values) override {
        if (!values[0]) {
            return;
        }
        ++_count;
        _sum += value_cast<Type>(data_type_for<Type>()->deserialize(*values[0]));
    }
};

template <typename Type>
class avg_function_for final : public native_aggregate_function {
public:
    avg_function_for() : native_aggregate_function("avg", data_type_for<Type>(), { data_type_for<Type>() }) {}
    virtual std::unique_ptr<aggregate> new_aggregate() override {
        return std::make_unique<impl_avg_function_for<Type>>();
    }
};

template <typename Type>
inline
shared_ptr<aggregate_function>
make_avg_function() {
    return make_shared<avg_function_for<Type>>();
}

template <typename Type>
class impl_max_function_for final : public aggregate_function::aggregate {
   std::experimental::optional<Type> _max{};
public:
    virtual void reset() override {
        _max = {};
    }
    virtual opt_bytes compute(cql_serialization_format sf) override {
        if (!_max) {
            return {};
        }
        return data_type_for<Type>()->decompose(*_max);
    }
    virtual void add_input(cql_serialization_format sf, const std::vector<opt_bytes>& values) override {
        if (!values[0]) {
            return;
        }
        auto val = value_cast<Type>(data_type_for<Type>()->deserialize(*values[0]));
        if (!_max) {
            _max = val;
        } else {
            _max = std::max(*_max, val);
        }
    }
};

template <typename Type>
class max_function_for final : public native_aggregate_function {
public:
    max_function_for() : native_aggregate_function("max", data_type_for<Type>(), { data_type_for<Type>() }) {}
    virtual std::unique_ptr<aggregate> new_aggregate() override {
        return std::make_unique<impl_max_function_for<Type>>();
    }
};

    /**
     * Creates a MAX function for the specified type.
     *
     * @param inputType the function input and output type
     * @return a MAX function for the specified type.
     */
template <typename Type>
shared_ptr<aggregate_function>
make_max_function() {
    return make_shared<max_function_for<Type>>();
}

template <typename Type>
class impl_min_function_for final : public aggregate_function::aggregate {
   std::experimental::optional<Type> _min{};
public:
    virtual void reset() override {
        _min = {};
    }
    virtual opt_bytes compute(cql_serialization_format sf) override {
        if (!_min) {
            return {};
        }
        return data_type_for<Type>()->decompose(*_min);
    }
    virtual void add_input(cql_serialization_format sf, const std::vector<opt_bytes>& values) override {
        if (!values[0]) {
            return;
        }
        auto val = value_cast<Type>(data_type_for<Type>()->deserialize(*values[0]));
        if (!_min) {
            _min = val;
        } else {
            _min = std::min(*_min, val);
        }
    }
};

template <typename Type>
class min_function_for final : public native_aggregate_function {
public:
    min_function_for() : native_aggregate_function("min", data_type_for<Type>(), { data_type_for<Type>() }) {}
    virtual std::unique_ptr<aggregate> new_aggregate() override {
        return std::make_unique<impl_min_function_for<Type>>();
    }
};


    /**
     * Creates a MIN function for the specified type.
     *
     * @param inputType the function input and output type
     * @return a MIN function for the specified type.
     */
template <typename Type>
shared_ptr<aggregate_function>
make_min_function() {
    return make_shared<min_function_for<Type>>();
}


template <typename Type>
class impl_count_function_for final : public aggregate_function::aggregate {
   int64_t _count = 0;
public:
    virtual void reset() override {
        _count = 0;
    }
    virtual opt_bytes compute(cql_serialization_format sf) override {
        return long_type->decompose(_count);
    }
    virtual void add_input(cql_serialization_format sf, const std::vector<opt_bytes>& values) override {
        if (!values[0]) {
            return;
        }
        ++_count;
    }
};

template <typename Type>
class count_function_for final : public native_aggregate_function {
public:
    count_function_for() : native_aggregate_function("count", long_type, { data_type_for<Type>() }) {}
    virtual std::unique_ptr<aggregate> new_aggregate() override {
        return std::make_unique<impl_count_function_for<Type>>();
    }
};

    /**
     * Creates a COUNT function for the specified type.
     *
     * @param inputType the function input type
     * @return a COUNT function for the specified type.
     */
template <typename Type>
shared_ptr<aggregate_function>
make_count_function() {
    return make_shared<count_function_for<Type>>();
}

}
}
}

