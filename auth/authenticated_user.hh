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
 * Copyright 2016 Cloudius Systems
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

#include <seastar/core/sstring.hh>

namespace auth {

class authenticated_user {
public:
    static const sstring ANONYMOUS_USERNAME;

    authenticated_user();
    authenticated_user(sstring name);

    const sstring& name() const;

    /**
     * Checks the user's superuser status.
     * Only a superuser is allowed to perform CREATE USER and DROP USER queries.
     * Im most cased, though not necessarily, a superuser will have Permission.ALL on every resource
     * (depends on IAuthorizer implementation).
     */
    bool is_super() const;

    /**
     * If IAuthenticator doesn't require authentication, this method may return true.
     */
    bool is_anonymous() const {
        return _anon;
    }

    bool operator==(const authenticated_user&) const;
private:
    sstring _name;
    bool _anon;
};

}

