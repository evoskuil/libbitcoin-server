/**
 * Copyright (c) 2011-2017 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/server/web/socket.hpp>

#include <string>
#include <utility>

#include <bitcoin/protocol.hpp>
#include <bitcoin/server/configuration.hpp>
#include <bitcoin/server/define.hpp>
#include <bitcoin/server/server_node.hpp>
#include <bitcoin/server/web/json_string.hpp>

#ifdef WITH_MBEDTLS
extern "C"
{
int https_random(void*, uint8_t* buffer, size_t length)
{
    bc::data_chunk random(length);
    bc::pseudo_random_fill(random);
    std::memcpy(buffer, reinterpret_cast<void*>(random.data()), length);
    return 0;
}
}
#endif

#include "../src/web/http/http.hpp"
#include "../src/web/http/utilities.hpp"
#include "../src/web/http/manager.hpp"
#include "../src/web/http/connection.hpp"

namespace libbitcoin {
namespace server {

using namespace asio;
using namespace bc::chain;
using namespace bc::protocol;
using namespace boost::filesystem;
using namespace boost::iostreams;
using namespace boost::property_tree;
using namespace http;
using role = zmq::socket::role;

// Local class.
class task_sender
  : public http::manager::task
{
  public:
    task_sender(connection_ptr connection, const std::string& data)
      : connection_(connection), data_(data)
    {
    }

    bool run()
    {
        if (connection_ == nullptr || connection_->closed())
            return false;

        if (connection_->json_rpc())
        {
            http_reply reply;
            const auto header = reply.generate(protocol_status::ok, {},
                data_.size(), false);

            LOG_VERBOSE(LOG_SERVER_HTTP)
                << "Writing JSON-RPC response: " << header;

            return connection_->write(header) == static_cast<int32_t>(header.size());
        }

        // BUGBUG: unguarded narrowing cast.
        return connection_->write(data_) == static_cast<int32_t>(data_.size());
    }

    connection_ptr& connection()
    {
        return connection_;
    }

  private:
    connection_ptr connection_;
    const std::string data_;
};

// The protocol message_size_limit is on the order of 1M.  The maximum
// websocket frame size is set to a much smaller fraction of this
// because our websocket protocol implementation does not contain
// large incoming messages, and also to help avoid DoS attacks via
// large incoming messages.
static constexpr auto maximum_incoming_websocket_message_size = 4096u;

// static
// Callback made internally via socket::poll on the web socket thread.
bool socket::handle_event(connection_ptr& connection, const http::event event,
    const void* data)
{
    switch (event)
    {
        case http::event::accepted:
        {
            // This connection is newly accepted and is either an HTTP
            // JSON-RPC connection, or an already upgraded websocket.
            // Returning false here will cause the service to stop
            // accepting new connections.
            auto instance = static_cast<socket*>(connection->user_data());
            BITCOIN_ASSERT(instance != nullptr);
            instance->add_connection(connection);

            const auto connection_type = connection->json_rpc() ? "JSON-RPC" :
                "Websocket";
            LOG_DEBUG(LOG_SERVER)
                << connection_type << " client connection established ["
                << connection << "] (" << instance->connection_count() << ")";
            break;
        }

        case http::event::json_rpc:
        {
            // Process new incoming user json_rpc request.  Returning
            // false here will cause this connection to be closed.
            auto instance = static_cast<socket*>(connection->user_data());
            BITCOIN_ASSERT(instance != nullptr);
            BITCOIN_ASSERT(data != nullptr);
            const auto& request = *reinterpret_cast<const http_request*>(data);
            BITCOIN_ASSERT(request.json_rpc);

            // Use default-value get to avoid exceptions on invalid input.
            const auto id = request.json_tree.get<uint32_t>("id", 0);
            const auto method = request.json_tree.get<std::string>("method", "");
            std::string parameters{};

            const auto child = request.json_tree.get_child("params");
            std::vector<std::string> parameter_list;
            for (const auto& parameter: child)
                parameter_list.push_back(
                    parameter.second.get_value<std::string>());

            // TODO: Support full parameter lists?
            if (!parameter_list.empty())
                parameters = parameter_list[0];

            LOG_VERBOSE(LOG_SERVER)
                << "method " << method << ", parameters " << parameters
                << ", id " << id;

            instance->notify_query_work(connection, method, id, parameters);
            break;
        }

        case http::event::websocket_frame:
        {
            // Process new incoming user websocket data. Returning false
            // will cause this connection to be closed.
            auto instance = static_cast<socket*>(connection->user_data());
            if (instance == nullptr)
                return false;

            BITCOIN_ASSERT(data != nullptr);
            auto message = reinterpret_cast<const websocket_message*>(data);

            ptree input_tree;
            if (!bc::property_tree(input_tree,
                { message->data, message->data + message->size }))
            {
                http_reply reply;
                connection->write(reply.generate(
                    protocol_status::internal_server_error, {}, 0, false));
                return false;
            }

            // Use default-value get to avoid exceptions on invalid input.
            const auto id = input_tree.get<uint32_t>("id", 0);
            const auto method = input_tree.get<std::string>("method", "");
            std::string parameters{};

            const auto child = input_tree.get_child("params");
            std::vector<std::string> parameter_list;
            for (const auto& parameter: child)
                parameter_list.push_back(
                    parameter.second.get_value<std::string>());

            // TODO: Support full parameter lists?
            if (!parameter_list.empty())
                parameters = parameter_list[0];

            LOG_VERBOSE(LOG_SERVER)
                << "method " << method << ", parameters " << parameters
                << ", id " << id;

            instance->notify_query_work(connection, method, id, parameters);
            break;
        }

        case http::event::closing:
        {
            // This connection is going away after this handling.
            auto instance = static_cast<socket*>(connection->user_data());
            BITCOIN_ASSERT(instance != nullptr);
            instance->remove_connection(connection);

            if (connection->websocket())
            {
                const auto connection_type = connection->json_rpc() ?
                    "JSON-RPC" : "Websocket";
                LOG_DEBUG(LOG_SERVER)
                    << connection_type << " client disconnected [" << connection
                    << "] (" << instance->connection_count() << ")";
            }

            break;
        }

        // No specific handling required for other events.
        case http::event::read:
        case http::event::error:
        case http::event::websocket_control_frame:
        default:
            break;
    }

    return true;
}

socket::socket(zmq::authenticator& authenticator, server_node& node,
    bool secure, const std::string& domain)
  : worker(priority(node.server_settings().priority)),
    authenticator_(authenticator),
    secure_(secure),
    security_(secure ? "secure" : "public"),
    server_settings_(node.server_settings()),
    protocol_settings_(node.protocol_settings()),
    sequence_(0),
    domain_(domain),
    document_root_(node.server_settings().websockets_root)
{
}

bool socket::start()
{
    if (!exists(document_root_))
    {
        LOG_ERROR(LOG_SERVER)
            << "Configured HTTP root path '" << document_root_
            << "' does not exist.";
        return false;
    }

    if (secure_)
    {
        if (!exists(server_settings_.websockets_server_certificate))
        {
            LOG_ERROR(LOG_SERVER)
                << "Required server certificate '"
                << server_settings_.websockets_server_certificate
                << "' does not exist.";
            return false;
        }

        if (!exists(server_settings_.websockets_server_private_key))
        {
            LOG_ERROR(LOG_SERVER)
                << "Required server private key '"
                << server_settings_.websockets_server_private_key
                << "' does not exist.";
            return false;
        }
    }

    return zmq::worker::start();
}

void socket::handle_websockets()
{
    const auto& endpoint = websocket_endpoint();
    http::bind_options options;
    manager_ = std::make_shared<http::manager>(secure_, &socket::handle_event,
        document_root_);

    if (manager_ == nullptr || !manager_->initialize())
    {
        LOG_ERROR(LOG_SERVER)
            << "Failed to initialize websocket manager";
        thread_status_.set_value(false);
        return;
    }

    if (secure_)
    {
        options.ssl_ca_certificate =
            (exists(server_settings_.websockets_ca_certificate) ?
             server_settings_.websockets_ca_certificate.generic_string() : "*");
        options.ssl_key =
            server_settings_.websockets_server_private_key.generic_string();
        options.ssl_certificate =
            server_settings_.websockets_server_certificate.generic_string();
    }

    options.user_data = static_cast<void*>(this);
    if (!manager_->bind(endpoint.to_local().host(), endpoint.port(), options))
    {
        LOG_ERROR(LOG_SERVER)
            << "Failed to bind listener websocket to port " << endpoint.port();
        thread_status_.set_value(false);
        return;
    }

    LOG_INFO(LOG_SERVER)
        << "Bound " << security_ << " " << domain_ << " websocket to port "
        << endpoint.port();

    manager_->set_maximum_incoming_frame_length(
        maximum_incoming_websocket_message_size);
    thread_status_.set_value(true);

    manager_->start();
}

const std::shared_ptr<zmq::socket> socket::service() const
{
    return nullptr;
}

bool socket::start_websocket_handler()
{
    std::future<bool> status = thread_status_.get_future();
    thread_ = std::make_shared<asio::thread>(&socket::handle_websockets, this);
    status.wait();
    return status.get();
}

bool socket::stop_websocket_handler()
{
    BITCOIN_ASSERT(manager_ != nullptr);
    manager_->stop();
    thread_->join();
    return true;
}

size_t socket::connection_count() const
{
    // BUGBUG: use of connections_ in this method is not thread safe.
    return connections_.size();
}

// Called by the websocket handling thread via handle_event.
void socket::add_connection(connection_ptr& connection)
{
    // BUGBUG: use of connections_ in this method is not thread safe.
    BITCOIN_ASSERT(connections_.find(connection) == connections_.end());

    // Initialize a new query_work_map for this connection.
    connections_[connection].clear();
}

// Called by the websocket handling thread via handle_event.
// Correlation lock usage is required because it protects the shared
// correlation map of ids, which can also used by the zmq service
// thread on response handling (i.e. query_socket::handle_query).
void socket::remove_connection(connection_ptr& connection)
{
    // BUGBUG: use of connections_ in this method is not thread safe.
    if (connections_.empty())
        return;

    const auto it = connections_.find(connection);
    if (it != connections_.end())
    {
        // Tearing down a connection is O(n) where n is the amount of
        // remaining outstanding queries.

        ///////////////////////////////////////////////////////////////////////
        // Critical Section
        correlation_lock_.lock_upgrade();

        auto& query_work_map = it->second;
        for (const auto& query_work: query_work_map)
        {
            const auto correlation = correlations_.find(
                query_work.second.correlation_id);

            if (correlation != correlations_.end())
            {
                correlation_lock_.unlock_upgrade_and_lock();
                // ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
                correlations_.erase(correlation);
                // ------------------------------------------------------------
                correlation_lock_.unlock_and_lock_upgrade();
            }
        }

        correlation_lock_.unlock_upgrade();
        ///////////////////////////////////////////////////////////////////////

        // Clear the query_work_map for this connection before removal.
        query_work_map.clear();
        connections_.erase(it);
    }
}

// Called by the websocket handling thread via handle_event.
// Correlation lock usage is required because it protects the shared
// correlation map of ids, which is also used by the zmq service
// thread on query response handling.
//
// Errors write directly on the connection since this is called from
// the event_handler, which is called on the websocket thread.
void socket::notify_query_work(connection_ptr& connection,
    const std::string& method, const uint32_t id,
    const std::string& parameters)
{
    typedef std::pair<int32_t, std::string> error;
    static const error invalid_request{ -32600, "Invalid Request." };
    static const error not_found{ -32601, "Method not found." };
    static const error internal_error{ -32603, "Internal error." };

    auto send_error_reply = [&connection, id](const protocol_status status,
        int code, const std::string message)
    {
        http_reply reply;
        const auto error = web::to_json(code, message, id);
        const auto response = reply.generate(status, {}, error.size(), false);
        LOG_VERBOSE(LOG_SERVER) << error + response;
        connection->write(error + response);
    };

    if (handlers_.empty())
    {
        send_error_reply(protocol_status::service_unavailable,
            invalid_request.first, invalid_request.second);
        // This error will most commonly present when a client
        // connects to one of the other websocket services (other than
        // query_socket), so no handlers are available.
        LOG_ERROR(LOG_SERVER)
            << "No method handlers available; ensure you're connected to the "
            << "query websocket service";
        return;
    }

    const auto handler = handlers_.find(method);
    if (handler == handlers_.end())
    {
        send_error_reply(protocol_status::not_found, not_found.first,
            not_found.second);
        LOG_VERBOSE(LOG_SERVER) << "Method " << method << " not found";
        return;
    }

    // BUGBUG: use of connections_ in this method is not thread safe.
    // BUGBUG: this includes modification of query_work_map below.
    auto it = connections_.find(connection);
    if (it == connections_.end())
    {
        LOG_ERROR(LOG_SERVER)
            << "Query work provided for unknown connection " << connection;
        return;
    }

    auto& query_work_map = it->second;
    if (query_work_map.find(id) != query_work_map.end())
    {
        send_error_reply(protocol_status::internal_server_error,
            internal_error.first, internal_error.second);
        return;
    }

    query_work_map.emplace(std::piecewise_construct,
        std::forward_as_tuple(id),
        std::forward_as_tuple(id, sequence_, connection, method, parameters));

    // Encode request based on query work and send to query_websocket.
    zmq::message request;
    handler->second.encode(request, handler->second.command, parameters,
        sequence_);

    ///////////////////////////////////////////////////////////////////////////
    // Critical Section
    correlation_lock_.lock();

    // While each connection has its own id map (meaning correlation ids passed
    // from the web client are unique on a per connection basis, potentially
    // utilizing the full range available), we need an internal mapping that
    // will allow us to correlate each zmq request/response pair with the
    // connection and original id number that originated it. The client never
    // sees this sequence_ value.
    correlations_[sequence_++] = { connection, id };

    correlation_lock_.unlock();
    ///////////////////////////////////////////////////////////////////////////

    const auto ec = service()->send(request);

    if (ec)
    {
        LOG_WARNING(LOG_SERVER)
            << "Query send failure: " << ec.message();
        send_error_reply(protocol_status::internal_server_error,
           internal_error.first, internal_error.second);
        return;
    }
}

// Sends json strings to websockets or connections waiting on json_rpc
// replies (only).
void socket::send(connection_ptr connection, const std::string& json)
{
    if (connection == nullptr || connection->closed() ||
        (!connection->websocket() && !connection->json_rpc()))
        return;
    // By using a task_sender via the manager's execute method, we guarantee
    // that the write is performed on the manager's websocket thread (at the
    // expense of copied json send and response payloads).
    manager_->execute(std::make_shared<task_sender>(connection, json));
}

// Sends json strings to all connected websockets.
void socket::broadcast(const std::string& json)
{
    auto sender = [this, &json](std::pair<connection_ptr, query_work_map> entry)
    {
        send(entry.first, json);
    };

    // BUGBUG: use of connections_ in this method is not thread safe.
    std::for_each(connections_.begin(), connections_.end(), sender);
}

} // namespace server
} // namespace libbitcoin
