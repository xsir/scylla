
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

#include <core/reactor.hh>
#include <core/app-template.hh>
#include <seastar/core/sstring.hh>
#include <seastar/rpc/rpc_types.hh>
#include "message/messaging_service.hh"
#include "gms/gossip_digest_syn.hh"
#include "gms/gossip_digest_ack.hh"
#include "gms/gossip_digest_ack2.hh"
#include "gms/gossip_digest.hh"
#include "api/api.hh"

#include "disk-error-handler.hh"

thread_local disk_error_signal_type commit_error;
thread_local disk_error_signal_type general_disk_error;

using namespace std::chrono_literals;
using namespace net;

class tester {
private:
    messaging_service& ms;
    gms::inet_address _server;
    uint32_t _cpuid;
public:
    tester()
       : ms(get_local_messaging_service()) {
    }
    using msg_addr = net::messaging_service::msg_addr;
    using inet_address = gms::inet_address;
    using endpoint_state = gms::endpoint_state;
    msg_addr get_msg_addr() {
        return msg_addr{_server, _cpuid};
    }
    void set_server_ip(sstring ip) {
        _server = inet_address(ip);
    }
    void set_server_cpuid(uint32_t cpu) {
        _cpuid = cpu;
    }
    future<> stop() {
        return make_ready_future<>();
    }
    promise<> digest_test_done;
public:
    void init_handler() {
        ms.register_gossip_digest_syn([this] (const rpc::client_info& cinfo, gms::gossip_digest_syn msg) {
            print("Server got syn msg = %s\n", msg);

            auto from = net::messaging_service::get_source(cinfo);
            auto ep1 = inet_address("1.1.1.1");
            auto ep2 = inet_address("2.2.2.2");
            int32_t gen = 800;
            int32_t ver = 900;
            std::vector<gms::gossip_digest> digests{
                {ep1, gen++, ver++},
                {ep2, gen++, ver++},
            };
            std::map<inet_address, endpoint_state> eps{
                {ep1, endpoint_state()},
                {ep2, endpoint_state()},
            };
            gms::gossip_digest_ack ack(std::move(digests), std::move(eps));
            ms.send_gossip_digest_ack(from, std::move(ack)).handle_exception([] (auto ep) {
                print("Fail to send ack : %s", ep);
            });
            return messaging_service::no_wait();
        });

        ms.register_gossip_digest_ack([this] (const rpc::client_info& cinfo, gms::gossip_digest_ack msg) {
            print("Server got ack msg = %s\n", msg);
            auto from = net::messaging_service::get_source(cinfo);
            // Prepare gossip_digest_ack2 message
            auto ep1 = inet_address("3.3.3.3");
            std::map<inet_address, endpoint_state> eps{
                {ep1, endpoint_state()},
            };
            gms::gossip_digest_ack2 ack2(std::move(eps));
            ms.send_gossip_digest_ack2(from, std::move(ack2)).handle_exception([] (auto ep) {
                print("Fail to send ack2 : %s", ep);
            });
            digest_test_done.set_value();
            return messaging_service::no_wait();
        });

        ms.register_gossip_digest_ack2([] (gms::gossip_digest_ack2 msg) {
            print("Server got ack2 msg = %s\n", msg);
            return messaging_service::no_wait();
        });

        ms.register_gossip_shutdown([] (inet_address from) {
            print("Server got shutdown msg = %s\n", from);
            return messaging_service::no_wait();
        });

        ms.register_gossip_echo([] {
            print("Server got gossip echo msg\n");
            throw std::runtime_error("I'm throwing runtime_error exception");
            return make_ready_future<>();
        });
    }

public:
    future<> test_gossip_digest() {
        print("=== %s ===\n", __func__);
        // Prepare gossip_digest_syn message
        auto id = get_msg_addr();
        auto ep1 = inet_address("1.1.1.1");
        auto ep2 = inet_address("2.2.2.2");
        int32_t gen = 100;
        int32_t ver = 900;
        std::vector<gms::gossip_digest> digests{
            {ep1, gen++, ver++},
            {ep2, gen++, ver++},
        };
        gms::gossip_digest_syn syn("my_cluster", "my_partition", digests);
        return ms.send_gossip_digest_syn(id, std::move(syn)).then([this] {
            return digest_test_done.get_future();
        });
    }

    future<> test_gossip_shutdown() {
        print("=== %s ===\n", __func__);
        auto id = get_msg_addr();
        inet_address from("127.0.0.1");
        return ms.send_gossip_shutdown(id, from).then([] () {
            print("Client sent gossip_shutdown got reply = void\n");
            return make_ready_future<>();
        });
    }

    future<> test_echo() {
        print("=== %s ===\n", __func__);
        auto id = get_msg_addr();
        return ms.send_gossip_echo(id).then_wrapped([] (auto&& f) {
            try {
                f.get();
                return make_ready_future<>();
            } catch (std::runtime_error& e) {
                print("test_echo: %s\n", e.what());
            }
            return make_ready_future<>();
        });
    }
};

namespace bpo = boost::program_options;

int main(int ac, char ** av) {
    app_template app;
    app.add_options()
        ("server", bpo::value<std::string>(), "Server ip")
        ("listen-address", bpo::value<std::string>()->default_value("0.0.0.0"), "IP address to listen")
        ("api-port", bpo::value<uint16_t>()->default_value(10000), "Http Rest API port")
        ("stay-alive", bpo::value<bool>()->default_value(false), "Do not kill the test server after the test")
        ("cpuid", bpo::value<uint32_t>()->default_value(0), "Server cpuid");

    distributed<database> db;

    return app.run_deprecated(ac, av, [&app] {
        auto config = app.configuration();
        uint16_t api_port = config["api-port"].as<uint16_t>();
        bool stay_alive = config["stay-alive"].as<bool>();
        if (config.count("server")) {
            api_port++;
        }
        const gms::inet_address listen = gms::inet_address(config["listen-address"].as<std::string>());
        utils::fb_utilities::set_broadcast_address(listen);
        net::get_messaging_service().start(listen).then([config, api_port, stay_alive] () {
            auto testers = new distributed<tester>;
            testers->start().then([testers]{
                auto& server = net::get_local_messaging_service();
                auto port = server.port();
                std::cout << "Messaging server listening on port " << port << " ...\n";
                return testers->invoke_on_all(&tester::init_handler);
            }).then([testers, config, stay_alive] {
                auto t = &testers->local();
                if (!config.count("server")) {
                    return;
                }
                auto ip = config["server"].as<std::string>();
                auto cpuid = config["cpuid"].as<uint32_t>();
                t->set_server_ip(ip);
                t->set_server_cpuid(cpuid);
                print("=============TEST START===========\n");
                print("Sending to server ....\n");
                t->test_gossip_digest().then([testers, t] {
                    return t->test_gossip_shutdown();
                }).then([testers, t] {
                    return t->test_echo();
                }).then([testers, t, stay_alive] {
                    if (stay_alive) {
                        return;
                    }
                    print("=============TEST DONE===========\n");
                    testers->stop().then([testers] {
                        delete testers;
                        net::get_messaging_service().stop().then([]{
                            engine().exit(0);
                        });
                    });
                });
            });
        });
    });
}
