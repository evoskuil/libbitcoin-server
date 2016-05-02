/**
 * Copyright (c) 2011-2015 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin-server.
 *
 * libbitcoin-server is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Affero General Public License with
 * additional permissions to the one published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version. For more information see LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/server/worker.hpp>

#include <cstdint>
#include <vector>
#include <czmq++/czmqpp.hpp>
#include <bitcoin/node.hpp>
#include <bitcoin/server/config/config.hpp>
#include <bitcoin/server/config/settings_type.hpp>

namespace libbitcoin {
namespace server {
    
namespace posix_time = boost::posix_time;
using posix_time::milliseconds;
using posix_time::seconds;
using posix_time::microsec_clock;
using std::placeholders::_1;

static constexpr int zmq_true = 1;
static constexpr int zmq_false = 0;
static constexpr int zmq_fail = -1;

auto now = []() { return microsec_clock::universal_time(); };

send_worker::send_worker(czmqpp::context& context)
  : context_(context)
{
}

void send_worker::queue_send(const outgoing_message& message)
{
    czmqpp::socket socket(context_, ZMQ_PUSH);
    BITCOIN_ASSERT(socket.self() != nullptr);

    // Returns 0 if OK, -1 if the endpoint was invalid.
    int rc = socket.connect("inproc://trigger-send");
    if (rc == zmq_fail)
    {
        log_error(LOG_SERVICE)
            << "Failed to connect to send queue.";
        return;
    }

    message.send(socket);
    socket.destroy(context_);
}

request_worker::request_worker(bool log_requests, 
    uint32_t heartbeat_interval_seconds,
    uint32_t polling_interval_milliseconds)
  : socket_(context_, ZMQ_ROUTER),
    wakeup_socket_(context_, ZMQ_PULL),
    heartbeat_socket_(context_, ZMQ_PUB),
    authenticate_(context_),
    sender_(context_),
    log_requests_(log_requests),
    heartbeat_interval_(heartbeat_interval_seconds),
    polling_interval_milliseconds_(polling_interval_milliseconds)
{
    BITCOIN_ASSERT(socket_.self() != nullptr);
    BITCOIN_ASSERT(wakeup_socket_.self() != nullptr);
    BITCOIN_ASSERT(heartbeat_socket_.self() != nullptr);

    // Returns 0 if OK, -1 if the endpoint was invalid.
    int rc = wakeup_socket_.bind("inproc://trigger-send");
    if (rc == zmq_fail)
    {
        log_error(LOG_SERVICE)
            << "Failed to connect to request queue.";
        return;
    }
}

bool request_worker::start(const settings_type& config)
{
    // Use config values.
    log_requests_ = config.server.log_requests;

////#ifdef _MSC_VER
////    if (log_requests_)
////        log_debug(LOG_SERVICE)
////            << "Authentication logging disabled on Windows.";
////#else
    // This exposes the log stream to non-utf8 text on Windows.
    // TODO: fix zeromq/czmq/czmqpp to be utf8 everywhere.
    if (log_requests_)
        authenticate_.set_verbose(true);
////#endif 
////
////    if (!config.server.whitelists.empty())
////        whitelist(config.server.whitelists);
////
////    if (config.server.certificate_file.empty())
////        socket_.set_zap_domain("global");

    if (!enable_crypto(config))
    {
        log_error(LOG_SERVICE)
            << "Invalid server certificate.";
        return false;
    }

    // This binds the query service.
    if (!create_new_socket(config))
    {
        log_error(LOG_SERVICE)
            << "Failed to bind query service on "
            << config.server.query_endpoint;
        return false;
    }

    log_info(LOG_SERVICE)
        << "Bound query service on "
        << config.server.query_endpoint;

    ////// This binds the heartbeat service.
    ////const auto rc = heartbeat_socket_.bind(
    ////    config.server.heartbeat_endpoint.to_string());
    ////if (rc == 0)
    ////{
    ////    log_error(LOG_SERVICE)
    ////        << "Failed to bind heartbeat service on "
    ////        << config.server.heartbeat_endpoint;
    ////    return false;
    ////}

    ////log_info(LOG_SERVICE)
    ////    << "Bound heartbeat service on "
    ////    << config.server.heartbeat_endpoint;

    ////heartbeat_at_ = now() + heartbeat_interval_;
    return true;
}

bool request_worker::stop()
{
    return true;
}

static std::string format_whitelist(const config::authority& authority)
{
    auto formatted = authority.to_string();
    if (authority.port() == 0)
        formatted += ":*";

    return formatted;
}

void request_worker::whitelist(const config::authority::list& addresses)
{
    for (const auto& ip_address: addresses)
    {
        log_info(LOG_SERVICE)
            << "Whitelisted client [" << format_whitelist(ip_address) << "]";
        authenticate_.allow(ip_address.to_string());
    }
}

// hintjens.com/blog:45
bool request_worker::enable_crypto(const settings_type& config)
{
    if (config.server.certificate_file.empty())
        return true;

    std::string client_certs(CURVE_ALLOW_ANY);
    if (!config.server.client_certificates_path.empty())
        client_certs = config.server.client_certificates_path.string();
    authenticate_.configure_curve("*", client_certs);

    czmqpp::certificate cert(config.server.certificate_file.string());
    if (cert.valid())
    {
        static constexpr int as_server = zmq_true;
        socket_.set_curve_server(as_server);
        cert.apply(socket_);
        return true;
    }

    return false;
}

bool request_worker::create_new_socket(const settings_type& config)
{
    // Not sure what we would use this for, so disabled for now.
    // Set the socket identity name.
    ////if (!config.unique_name.get_host().empty())
    ////    socket_.set_identity(config.unique_name.get_host());

    // Connect...
    // Returns port number if connected.
    const auto rc = socket_.bind(config.server.query_endpoint.to_string());
    if (rc != 0)
    {
        static constexpr int zmq_socket_no_linger = zmq_false;
        socket_.set_linger(zmq_socket_no_linger);
        return true;
    }

    return false;
}

void request_worker::attach(const std::string& command,
    command_handler handler)
{
    handlers_[command] = handler;
}

void request_worker::update()
{
    poll();
}

void request_worker::poll()
{
    // Poll for network updates.
    czmqpp::poller poller(socket_, wakeup_socket_);
    BITCOIN_ASSERT(poller.self() != nullptr);
    czmqpp::socket which = poller.wait(polling_interval_milliseconds_);
    BITCOIN_ASSERT(socket_.self() != nullptr);
    BITCOIN_ASSERT(wakeup_socket_.self() != nullptr);

    if (which == socket_)
    {
        // Get message: 6-part envelope + content -> request
        incoming_message request;
        request.recv(socket_);

        // Perform request if handler exists.
        auto it = handlers_.find(request.command());
        if (it != handlers_.end())
        {
            if (log_requests_)
                log_debug(LOG_REQUEST)
                    << "Service request [" << request.command() << "] from "
                    << encode_base16(request.origin());

            it->second(request,
                std::bind(&send_worker::queue_send,
                    &sender_, _1));
        }
        else
        {
            log_warning(LOG_SERVICE)
                << "Unhandled service request [" << request.command()
                << "] from " << encode_base16(request.origin());
        }
    }
    else if (which == wakeup_socket_)
    {
        // Send queued message.
        czmqpp::message message;
        message.receive(wakeup_socket_);
        message.send(socket_);
    }

    // Publish heartbeat.
    if (now() > heartbeat_at_)
    {
        heartbeat_at_ = now() + heartbeat_interval_;
        log_debug(LOG_SERVICE) << "Publish service heartbeat";
        publish_heartbeat();
    }
}

void request_worker::publish_heartbeat()
{
    static uint32_t counter = 0;
    czmqpp::message message;
    const auto raw_counter = to_data_chunk(to_little_endian(counter));
    message.append(raw_counter);
    message.send(heartbeat_socket_);
    ++counter;
}

} // namespace server
} // namespace libbitcoin
