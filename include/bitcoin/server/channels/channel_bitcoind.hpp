/**
 * Copyright (c) 2011-2026 libbitcoin developers
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
#ifndef LIBBITCOIN_SERVER_CHANNELS_CHANNEL_BITCOIND_HPP
#define LIBBITCOIN_SERVER_CHANNELS_CHANNEL_BITCOIND_HPP

#include <bitcoin/server/channels/channel_rpc.hpp>
#include <bitcoin/server/define.hpp>
#include <bitcoin/server/error/error.hpp>
#include <bitcoin/server/interfaces/interfaces.hpp>
#include <bitcoin/server/settings.hpp>

namespace libbitcoin {
namespace server {

/// Channel for the bitcoind service (universal json-rpc).
class BCS_API channel_bitcoind
  : public channel_rpc,
    protected network::tracker<channel_bitcoind>
{
public:
    typedef std::shared_ptr<channel_bitcoind> ptr;
    using options_t = settings::bitcoind_server;

    inline channel_bitcoind(const network::logger& log,
        const network::socket::ptr& socket, uint64_t identifier,
        const node::configuration& config, const options_t& options,
        bool in_band=false) NOEXCEPT
      : channel_rpc(log, socket, identifier, config, options, in_band),
        network::tracker<channel_bitcoind>(log),
        error_status_(options.error_status)
    {
        using namespace server::bitcoind;
        version_ = version_floor(options.protocol_maximum);
    }

    /// The interface version of the channel (requires strand).
    inline server::bitcoind::version version() const NOEXCEPT
    {
        BC_ASSERT(stranded());
        return version_;
    }

    /// Set the interface version of the channel (requires strand).
    inline void set_version(
        server::bitcoind::version version) NOEXCEPT
    {
        BC_ASSERT(stranded());
        version_ = version;
    }

    /// The user agent of the first http request, which for a websocket is its
    /// upgrade (requires strand).
    inline const std::string& client() const NOEXCEPT
    {
        BC_ASSERT(stranded());
        return client_;
    }

    /// The credential username that authorized the channel (requires strand).
    inline const std::string& username() const NOEXCEPT
    {
        BC_ASSERT(stranded());
        return username_;
    }

    /// Set the credential username that authorized the channel (requires
    /// strand).
    inline void set_username(const std::string& username) NOEXCEPT
    {
        BC_ASSERT(stranded());
        username_ = username;
    }

    /// Retain request in dispatch as non-handshake opener, dispatched again
    /// (under its own gate) when channel resumes reader.
    inline void set_opener() NOEXCEPT
    {
        BC_ASSERT(stranded());
        opener_ = current_;
    }

    /// Overridden to dispatch a retained opener before reading on.
    inline void receive() NOEXCEPT override
    {
        BC_ASSERT(stranded());

        if (opener_ && !stopped())
        {
            dispatch(std::exchange(opener_, {}));
            return;
        }

        channel_rpc::receive();
    }

    /// The http status of an unbatched json-rpc response, as bitcoind.
    static inline network::http::status to_status(
        const network::rpc::response_t& model) NOEXCEPT
    {
        using namespace network::http;
        if (!model.error.has_value() ||
            (model.jsonrpc == network::rpc::version::v2))
            return status::ok;

        switch (model.error->code)
        {
            case error::bitcoind::invalid_request:
                return status::bad_request;
            case error::bitcoind::method_not_found:
                return status::not_found;
            default:
                return status::internal_server_error;
        }
    }

    inline network::http::status to_rpc_status(
        const network::rpc::response_t& model) const NOEXCEPT override
    {
        return error_status_ ? to_status(model) : network::http::status::ok;
    }

protected:
    /// bitcoind clients send named params with json-rpc v1 (tolerated laxness).
    inline bool lax_named() const NOEXCEPT override
    {
        return true;
    }

    /// bitcoind clients send json-rpc v1 batch elements (tolerated laxness).
    inline bool lax_batch() const NOEXCEPT override
    {
        return true;
    }

    /// Overridden to capture the client and basic authorization username of
    /// each http request, including a websocket upgrade.
    inline void set_authorized(
        const network::http::request& request) NOEXCEPT override
    {
        BC_ASSERT(stranded());
        using namespace network::http;
        channel_rpc::set_authorized(request);
        if (client_.empty())
            client_ = request[field::user_agent];

        constexpr std::string_view basic{ "Basic " };
        const std::string authorization{ request[field::authorization] };
        system::data_chunk decoded{};
        if (username_.empty() && authorized() &&
            authorization.starts_with(basic) &&
            system::decode_base64(decoded, authorization.substr(basic.size())))
        {
            const auto text = system::to_string(decoded);
            username_ = text.substr(zero, text.find(':'));
        }
    }

    /// Overridden to retain the request in dispatch (see set_opener).
    inline void dispatch(
        const network::http::request_ptr& request) NOEXCEPT override
    {
        BC_ASSERT(stranded());
        current_ = request;
        channel_rpc::dispatch(request);
        current_.reset();
    }

private:
    // This is thread safe.
    const bool error_status_;

    // These are protected by strand.
    server::bitcoind::version version_{};
    std::string client_{};
    std::string username_{};
    network::http::request_ptr current_{};
    network::http::request_ptr opener_{};
};

} // namespace server
} // namespace libbitcoin

#endif
