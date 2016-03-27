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

#include "locator/abstract_replication_strategy.hh"
#include "utils/class_registrator.hh"
#include "exceptions/exceptions.hh"

namespace locator {

logging::logger abstract_replication_strategy::logger("replication_strategy");

abstract_replication_strategy::abstract_replication_strategy(
    const sstring& ks_name,
    token_metadata& token_metadata,
    snitch_ptr& snitch,
    const std::map<sstring, sstring>& config_options,
    replication_strategy_type my_type)
        : _ks_name(ks_name)
        , _config_options(config_options)
        , _token_metadata(token_metadata)
        , _snitch(snitch)
        , _my_type(my_type) {}

static void unwrap_first_range(std::vector<range<token>>& ret) {
    if (ret.empty()) {
        return;
    }
    // Make ret contain no wrap-around range by unwrapping the first element.
    auto& r = ret.front();
    if (r.is_wrap_around(dht::token_comparator())) {
        auto split_ranges = r.unwrap();
        r = std::move(split_ranges.first);
        ret.push_back(std::move(split_ranges.second));
    }
}

std::unique_ptr<abstract_replication_strategy> abstract_replication_strategy::create_replication_strategy(const sstring& ks_name, const sstring& strategy_name, token_metadata& tk_metadata, const std::map<sstring, sstring>& config_options) {
    assert(locator::i_endpoint_snitch::get_local_snitch_ptr());

    return create_object<abstract_replication_strategy,
                         const sstring&,
                         token_metadata&,
                         snitch_ptr&,
                         const std::map<sstring, sstring>&>
        (strategy_name, ks_name, tk_metadata,
         locator::i_endpoint_snitch::get_local_snitch_ptr(), config_options);
}

void abstract_replication_strategy::validate_replication_strategy(const sstring& ks_name,
                                                                  const sstring& strategy_name,
                                                                  token_metadata& token_metadata,
                                                                  const std::map<sstring, sstring>& config_options)
{
    auto strategy = create_replication_strategy(ks_name, strategy_name, token_metadata, config_options);
    strategy->validate_options();
    auto expected = strategy->recognized_options();
    if (expected) {
        for (auto&& item : config_options) {
            sstring key = item.first;
            if (!expected->count(key)) {
                 throw exceptions::configuration_exception(sprint("Unrecognized strategy option {%s} passed to %s for keyspace %s", key, strategy_name, ks_name));
            }
        }
    }
}

std::vector<inet_address> abstract_replication_strategy::get_natural_endpoints(const token& search_token) {
    const token& key_token = _token_metadata.first_token(search_token);
    auto& cached_endpoints = get_cached_endpoints();
    auto res = cached_endpoints.find(key_token);

    if (res == cached_endpoints.end()) {
        auto endpoints = calculate_natural_endpoints(search_token, _token_metadata);
        cached_endpoints.emplace(key_token, endpoints);

        return std::move(endpoints);
    }

    ++_cache_hits_count;
    return res->second;
}

void abstract_replication_strategy::validate_replication_factor(sstring rf) const
{
    try {
        if (std::stol(rf) < 0) {
            throw exceptions::configuration_exception(
               sstring("Replication factor must be non-negative; found ") + rf);
        }
    } catch (...) {
        throw exceptions::configuration_exception(
            sstring("Replication factor must be numeric; found ") + rf);
    }
}

inline std::unordered_map<token, std::vector<inet_address>>&
abstract_replication_strategy::get_cached_endpoints() {
    if (_last_invalidated_ring_version != _token_metadata.get_ring_version()) {
        _cached_endpoints.clear();
        _last_invalidated_ring_version = _token_metadata.get_ring_version();
    }

    return _cached_endpoints;
}

std::vector<range<token>>
abstract_replication_strategy::get_ranges(inet_address ep) const {
    std::vector<range<token>> ret;
    auto prev_tok = _token_metadata.sorted_tokens().back();
    for (auto tok : _token_metadata.sorted_tokens()) {
        for (inet_address a : calculate_natural_endpoints(tok, _token_metadata)) {
            if (a == ep) {
                ret.emplace_back(
                        range<token>::bound(prev_tok, false),
                        range<token>::bound(tok, true));
                break;
            }
        }
        prev_tok = tok;
    }
    unwrap_first_range(ret);
    return ret;
}

std::vector<range<token>>
abstract_replication_strategy::get_primary_ranges(inet_address ep) {
    std::vector<range<token>> ret;
    auto prev_tok = _token_metadata.sorted_tokens().back();
    for (auto tok : _token_metadata.sorted_tokens()) {
        auto&& eps = calculate_natural_endpoints(tok, _token_metadata);
        if (eps.size() > 0 && eps[0] == ep) {
            ret.emplace_back(
                    range<token>::bound(prev_tok, false),
                    range<token>::bound(tok, true));
        }
        prev_tok = tok;
    }
    unwrap_first_range(ret);
    return ret;
}

std::unordered_multimap<inet_address, range<token>>
abstract_replication_strategy::get_address_ranges(token_metadata& tm) const {
    std::unordered_multimap<inet_address, range<token>> ret;
    for (auto& t : tm.sorted_tokens()) {
        range<token> r = tm.get_primary_range_for(t);
        auto eps = calculate_natural_endpoints(t, tm);
        logger.debug("token={}, primary_range={}, address={}", t, r, eps);
        bool wrap = r.is_wrap_around(dht::token_comparator());
        if (wrap) {
            auto split_ranges = r.unwrap();
            for (auto ep : eps) {
                ret.emplace(ep, split_ranges.first);
                ret.emplace(ep, split_ranges.second);
            }
        } else {
            for (auto ep : eps) {
                ret.emplace(ep, r);
            }
        }
    }
    return ret;
}

std::unordered_multimap<range<token>, inet_address>
abstract_replication_strategy::get_range_addresses(token_metadata& tm) const {
    std::unordered_multimap<range<token>, inet_address> ret;
    for (auto& t : tm.sorted_tokens()) {
        range<token> r = tm.get_primary_range_for(t);
        auto eps = calculate_natural_endpoints(t, tm);
        bool wrap = r.is_wrap_around(dht::token_comparator());
        if (wrap) {
            auto split_ranges = r.unwrap();
            for (auto ep : eps) {
                ret.emplace(split_ranges.first, ep);
                ret.emplace(split_ranges.second, ep);
            }
        } else {
            for (auto ep : eps) {
                ret.emplace(r, ep);
            }
        }
    }
    return ret;
}

std::vector<range<token>>
abstract_replication_strategy::get_pending_address_ranges(token_metadata& tm, token pending_token, inet_address pending_address) {
    return get_pending_address_ranges(tm, std::unordered_set<token>{pending_token}, pending_address);
}

std::vector<range<token>>
abstract_replication_strategy::get_pending_address_ranges(token_metadata& tm, std::unordered_set<token> pending_tokens, inet_address pending_address) {
    std::vector<range<token>> ret;
    auto temp = tm.clone_only_token_map();
    temp.update_normal_tokens(pending_tokens, pending_address);
    for (auto& x : get_address_ranges(temp)) {
        if (x.first == pending_address) {
            ret.push_back(x.second);
        }
    }
    return ret;
}

} // namespace locator
