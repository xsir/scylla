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

#include "api.hh"
#include "http/file_handler.hh"
#include "http/transformers.hh"
#include "http/api_docs.hh"
#include "storage_service.hh"
#include "commitlog.hh"
#include "gossiper.hh"
#include "failure_detector.hh"
#include "column_family.hh"
#include "lsa.hh"
#include "messaging_service.hh"
#include "storage_proxy.hh"
#include "cache_service.hh"
#include "collectd.hh"
#include "endpoint_snitch.hh"
#include "compaction_manager.hh"
#include "hinted_handoff.hh"
#include "http/exception.hh"
#include "stream_manager.hh"
#include "system.hh"

namespace api {

static std::unique_ptr<reply> exception_reply(std::exception_ptr eptr) {
    try {
        std::rethrow_exception(eptr);
    } catch (const no_such_keyspace& ex) {
        throw bad_param_exception(ex.what());
    }
    // We never going to get here
    return std::make_unique<reply>();
}

future<> set_server_init(http_context& ctx) {
    auto rb = std::make_shared < api_registry_builder > (ctx.api_doc);

    return ctx.http_server.set_routes([rb, &ctx](routes& r) {
        r.register_exeption_handler(exception_reply);
        r.put(GET, "/ui", new httpd::file_handler(ctx.api_dir + "/index.html",
                new content_replace("html")));
        r.add(GET, url("/ui").remainder("path"), new httpd::directory_handler(ctx.api_dir,
                new content_replace("html")));
        rb->register_function(r, "system",
                "The system related API");
        set_system(ctx, r);
        rb->set_api_doc(r);
    });
}

static future<> register_api(http_context& ctx, const sstring& api_name,
        const sstring api_desc,
        std::function<void(http_context& ctx, routes& r)> f) {
    auto rb = std::make_shared < api_registry_builder > (ctx.api_doc);

    return ctx.http_server.set_routes([rb, &ctx, api_name, api_desc, f](routes& r) {
        rb->register_function(r, api_name, api_desc);
        f(ctx,r);
    });
}

future<> set_server_storage_service(http_context& ctx) {
    return register_api(ctx, "storage_service", "The storage service API", set_storage_service);
}

future<> set_server_gossip(http_context& ctx) {
    return register_api(ctx, "gossiper",
                "The gossiper API", set_gossiper);
}

future<> set_server_load_sstable(http_context& ctx) {
    return register_api(ctx, "column_family",
                "The column family API", set_column_family);
}

future<> set_server_messaging_service(http_context& ctx) {
    return register_api(ctx, "messaging_service",
                "The messaging service API", set_messaging_service);
}

future<> set_server_storage_proxy(http_context& ctx) {
    return register_api(ctx, "storage_proxy",
                "The storage proxy API", set_storage_proxy);
}

future<> set_server_stream_manager(http_context& ctx) {
    return register_api(ctx, "stream_manager",
                "The stream manager API", set_stream_manager);
}

future<> set_server_gossip_settle(http_context& ctx) {
    auto rb = std::make_shared < api_registry_builder > (ctx.api_doc);

    return ctx.http_server.set_routes([rb, &ctx](routes& r) {
        rb->register_function(r, "failure_detector",
                "The failure detector API");
        set_failure_detector(ctx,r);
        rb->register_function(r, "cache_service",
                "The cache service API");
        set_cache_service(ctx,r);

        rb->register_function(r, "endpoint_snitch_info",
                "The endpoint snitch info API");
        set_endpoint_snitch(ctx, r);
    });
}

future<> set_server_done(http_context& ctx) {
    auto rb = std::make_shared < api_registry_builder > (ctx.api_doc);

    return ctx.http_server.set_routes([rb, &ctx](routes& r) {
        rb->register_function(r, "compaction_manager",
                "The Compaction manager API");
        set_compaction_manager(ctx, r);
        rb->register_function(r, "lsa", "Log-structured allocator API");
        set_lsa(ctx, r);

        rb->register_function(r, "commitlog",
                "The commit log API");
        set_commitlog(ctx,r);
        rb->register_function(r, "hinted_handoff",
                "The hinted handoff API");
        set_hinted_handoff(ctx, r);
        rb->register_function(r, "collectd",
                "The collectd API");
        set_collectd(ctx, r);
    });
}

}

