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
 *
 * Modified by Cloudius Systems.
 * Copyright 2015 Cloudius Systems.
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

#include "locator/abstract_replication_strategy.hh"
#include <experimental/optional>

namespace locator {
class everywhere_replication_strategy : public abstract_replication_strategy {
public:
    everywhere_replication_strategy(const sstring& keyspace_name, token_metadata& token_metadata, snitch_ptr& snitch, const std::map<sstring,sstring>& config_options);

    virtual std::vector<inet_address> calculate_natural_endpoints(const token& search_token, token_metadata& tm) const override {
        return tm.get_all_endpoints();
    }

    virtual void validate_options() const override { /* noop */ }

    std::experimental::optional<std::set<sstring>> recognized_options() const override {
        // We explicitely allow all options
        return std::experimental::nullopt;
    }

    virtual size_t get_replication_factor() const override {
        return _token_metadata.get_all_endpoints_count();
    }
};
}
