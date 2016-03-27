/*
 * Copyright 2015 ScyllaDB
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

#include "json/json_elements.hh"
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include "api/api-doc/utils.json.hh"
#include "utils/histogram.hh"
#include "http/exception.hh"
#include "api_init.hh"

namespace api {

template<class T>
std::vector<sstring> container_to_vec(const T& container) {
    std::vector<sstring> res;
    for (auto i : container) {
        res.push_back(boost::lexical_cast<std::string>(i));
    }
    return res;
}

template<class T>
std::vector<T> map_to_key_value(const std::map<sstring, sstring>& map) {
    std::vector<T> res;
    for (auto i : map) {
        res.push_back(T());
        res.back().key = i.first;
        res.back().value = i.second;
    }
    return res;
}

template<class T, class MAP>
std::vector<T>& map_to_key_value(const MAP& map, std::vector<T>& res) {
    for (auto i : map) {
        T val;
        val.key = boost::lexical_cast<std::string>(i.first);
        val.value = boost::lexical_cast<std::string>(i.second);
        res.push_back(val);
    }
    return res;
}
template <typename T, typename S = T>
T map_sum(T&& dest, const S& src) {
    for (auto i : src) {
        dest[i.first] += i.second;
    }
    return dest;
}

template <typename MAP>
std::vector<sstring> map_keys(const MAP& map) {
    std::vector<sstring> res;
    for (const auto& i : map) {
        res.push_back(boost::lexical_cast<std::string>(i.first));
    }
    return res;
}

/**
 * General sstring splitting function
 */
inline std::vector<sstring> split(const sstring& text, const char* separator) {
    if (text == "") {
        return std::vector<sstring>();
    }
    std::vector<sstring> tokens;
    return boost::split(tokens, text, boost::is_any_of(separator));
}

/**
 * Split a column family parameter
 */
inline std::vector<sstring> split_cf(const sstring& cf) {
    return split(cf, ",");
}

/**
 * A helper function to sum values on an a distributed object that
 * has a get_stats method.
 *
 */
template<class T, class F, class V>
future<json::json_return_type>  sum_stats(distributed<T>& d, V F::*f) {
    return d.map_reduce0([f](const T& p) {return p.get_stats().*f;}, 0,
            std::plus<V>()).then([](V val) {
        return make_ready_future<json::json_return_type>(val);
    });
}

inline double pow2(double a) {
    return a * a;
}

// FIXME: Move to utils::ihistogram::operator+=()
inline utils::ihistogram add_histogram(utils::ihistogram res,
        const utils::ihistogram& val) {
    if (res.count == 0) {
        return val;
    }
    if (val.count == 0) {
        return std::move(res);
    }
    if (res.min > val.min) {
        res.min = val.min;
    }
    if (res.max < val.max) {
        res.max = val.max;
    }
    double ncount = res.count + val.count;
    // To get an estimated sum we take the estimated mean
    // and multiply it by the true count
    res.sum = res.sum + val.mean * val.count;
    double a = res.count/ncount;
    double b = val.count/ncount;

    double mean =  a * res.mean + b * val.mean;

    res.variance = (res.variance + pow2(res.mean - mean) )* a +
            (val.variance + pow2(val.mean -mean))* b;

    res.mean = mean;
    res.count = res.count + val.count;
    for (auto i : val.sample) {
        res.sample.push_back(i);
    }
    return res;
}

inline
httpd::utils_json::histogram to_json(const utils::ihistogram& val) {
    httpd::utils_json::histogram h;
    h = val;
    return h;
}

template<class T, class F>
future<json::json_return_type>  sum_histogram_stats(distributed<T>& d, utils::ihistogram F::*f) {

    return d.map_reduce0([f](const T& p) {return p.get_stats().*f;}, utils::ihistogram(),
            add_histogram).then([](const utils::ihistogram& val) {
        return make_ready_future<json::json_return_type>(to_json(val));
    });
}

inline int64_t min_int64(int64_t a, int64_t b) {
    return std::min(a,b);
}

inline int64_t max_int64(int64_t a, int64_t b) {
    return std::max(a,b);
}

/**
 * A helper struct for ratio calculation
 * It combine total and the sub set for the ratio and its
 * to_json method return the ration sub/total
 */
struct ratio_holder : public json::jsonable {
    double total = 0;
    double sub = 0;
    virtual std::string to_json() const {
        if (total == 0) {
            return "0";
        }
        return std::to_string(sub/total);
    }
    ratio_holder() = default;
    ratio_holder& add(double _total, double _sub) {
        total += _total;
        sub += _sub;
        return *this;
    }
    ratio_holder(double _total, double _sub) {
        total = _total;
        sub = _sub;
    }
    ratio_holder& operator+=(const ratio_holder& a) {
        return add(a.total, a.sub);
    }
    friend ratio_holder operator+(ratio_holder a, const ratio_holder& b) {
        return a += b;
    }
};


class unimplemented_exception : public base_exception {
public:
    unimplemented_exception()
            : base_exception("API call is not supported yet", reply::status_type::internal_server_error) {
    }
};

inline void unimplemented() {
    throw unimplemented_exception();
}

template <class T>
std::vector<T> concat(std::vector<T> a, std::vector<T>&& b) {
    a.reserve( a.size() + b.size());
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

}
