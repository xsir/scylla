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

#include <core/distributed.hh>
#include <core/app-template.hh>
#include <core/sstring.hh>
#include <core/thread.hh>

#include "utils/managed_bytes.hh"
#include "utils/logalloc.hh"
#include "utils/managed_ref.hh"
#include "tests/perf/perf.hh"
#include "log.hh"

#include "disk-error-handler.hh"

thread_local disk_error_signal_type commit_error;
thread_local disk_error_signal_type general_disk_error;

void print_stats() {
    std::cout << "free " << memory::stats().free_memory()
        << " used " << logalloc::shard_tracker().occupancy().used_space()
        << " total " << logalloc::shard_tracker().occupancy().total_space()
        << std::endl;
}

int main(int argc, char** argv) {
    namespace bpo = boost::program_options;
    app_template app;
    app.add_options()
        ("size", bpo::value<unsigned>()->default_value(1024), "size of allocated object")
        ("batch", bpo::value<unsigned>()->default_value(1024), "number of allocated objects between deferring points")
        ("count", bpo::value<unsigned>()->default_value(1024*200), "number of objects to allocate before exiting");

    logging::logger_registry().set_all_loggers_level(logging::log_level::debug);

    return app.run(argc, argv, [&app] {
        auto obj_size = app.configuration()["size"].as<unsigned>();
        auto obj_count = app.configuration()["count"].as<unsigned>();
        auto objects_in_batch = app.configuration()["batch"].as<unsigned>();

        return seastar::async([obj_size, obj_count, objects_in_batch] {
            logalloc::region r;

            with_allocator(r.allocator(), [&] {
                std::deque<managed_bytes> refs;

                r.make_evictable([&] {
                    return with_allocator(r.allocator(), [&] {
                        if (refs.empty()) {
                            return memory::reclaiming_result::reclaimed_nothing;
                        }
                        refs.pop_front();
                        return memory::reclaiming_result::reclaimed_something;
                    });
                });

                uint64_t counter = 0;
                bool stop = false;

                engine().at_exit([&stop] {
                    stop = true;
                    return make_ready_future<>();
                });

                while (!stop) {
                    refs.push_back(managed_bytes(managed_bytes::initialized_later(), obj_size));

                    ++counter;

                    if (counter >= obj_count) {
                        break;
                    }

                    if (counter % objects_in_batch == 0) {
                        print_stats();
                        seastar::thread::yield();
                    }
                }
            });
            return 0;
        });
    });
}
