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
#ifndef LIBBITCOIN_SERVER_WEB_HTTP_HPP
#define LIBBITCOIN_SERVER_WEB_HTTP_HPP

#include <array>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>
#include <fcntl.h>

#ifdef WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #include <strsafe.h>
    #include <process.h>
#else
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <netinet/in.h>
    #include <unistd.h>
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <sys/select.h>
#endif

// Explicitly use std::placeholders here for usage internally to the
// boost parsing helpers included from json_parser.hpp.
// See: https://svn.boost.org/trac10/ticket/12621
////// TODO: DO NOT USE 'using namespace' IN HEADER.
////using namespace std::placeholders;

#include <boost/bind.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/random.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#ifdef WITH_MBEDTLS
    #include <mbedtls/base64.h>
    #include <mbedtls/error.h>
    #include <mbedtls/ecp.h>
    #include <mbedtls/platform.h>
    #include <mbedtls/sha1.h>
    #include <mbedtls/ssl.h>
    #include <mbedtls/x509_crt.h>
#endif

namespace libbitcoin {
namespace server {
namespace http {

#ifdef WIN32
    typedef SOCKET sock_t;
    typedef uint32_t in_addr_t;
    #define CLOSE_SOCKET closesocket
#else
    typedef int sock_t;
    #define CLOSE_SOCKET ::close
#endif

static const size_t sha1_hash_length = 20;
static const size_t default_buffer_length = 1 << 10; // 1KB
static const size_t transfer_buffer_length = 1 << 18; // 256KB

#ifdef WITH_MBEDTLS
static const int default_ciphers[] =
{
    MBEDTLS_TLS_RSA_WITH_AES_128_CBC_SHA,
    MBEDTLS_TLS_RSA_WITH_AES_128_CBC_SHA256,
    MBEDTLS_TLS_RSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDH_RSA_WITH_AES_128_CBC_SHA,
    MBEDTLS_TLS_ECDH_RSA_WITH_AES_128_CBC_SHA256,
    MBEDTLS_TLS_ECDH_RSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDH_ECDSA_WITH_AES_128_CBC_SHA,
    MBEDTLS_TLS_ECDH_ECDSA_WITH_AES_128_CBC_SHA256,
    MBEDTLS_TLS_ECDH_ECDSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_DHE_RSA_WITH_AES_128_CBC_SHA256,
    MBEDTLS_TLS_DHE_RSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    0
};
#endif

////typedef std::vector<char> data_buffer;
typedef std::array<uint8_t, default_buffer_length> read_buffer;
typedef std::array<uint8_t, sha1_hash_length> sha1_hash;
typedef std::vector<std::string> string_list;
typedef std::unordered_map<std::string, std::string> string_map;

// TODO: move each of these enum and struct declarations to own file.

enum class connection_state
{
    error = -1,
    connecting = 99,
    connected = 100,
    listening = 101,
    ssl_handshake = 102,
    closed = 103,
    disconnect_immediately = 200,
    unknown
};

enum class event : uint8_t
{
    read,
    write,
    listen,
    accepted,
    error,
    closing,
    websocket_frame,
    websocket_control_frame,
    json_rpc
};

enum class protocol_status : uint16_t
{
    switching = 101,
    ok = 200,
    created = 201,
    accepted = 202,
    no_content = 204,
    multiple_choices = 300,
    moved_permanently = 301,
    moved_temporarily = 302,
    not_modified = 304,
    bad_request = 400,
    unauthorized = 401,
    forbidden = 403,
    not_found = 404,
    internal_server_error = 500,
    not_implemented = 501,
    bad_gateway = 502,
    service_unavailable = 503
};

enum class websocket_op : uint8_t
{
    continuation = 0,
    text = 1,
    binary = 2,
    close = 8,
    ping = 9,
    pong = 10,
};

struct websocket_message
{
    const std::string& endpoint;
    const uint8_t* data;
    size_t size;
    uint8_t flags;
    websocket_op code;
};

class websocket_frame
{
public:
    websocket_frame()
      : header_length_(0), data_length_(0)
    {
    }

    static data_chunk to_data(size_t length, websocket_op code)
    {
        if (length < 126)
        {
            return build_chunk(
            {
                to_array(0x80 | static_cast<uint8_t>(code)),
                to_array(static_cast<uint8_t>(length))
            });
        }
        else if (length < max_uint16)
        {
            return build_chunk(
            {
                to_array(0x80 | static_cast<uint8_t>(code)),
                to_array(uint8_t(126)),
                to_big_endian(static_cast<uint16_t>(length))
            });
        }
        else
        {
            return build_chunk(
            {
                to_array(0x80 | static_cast<uint8_t>(code)),
                to_array(uint8_t(127)),
                to_big_endian(static_cast<uint64_t>(length))
            });
        }
    }

    bool from_data(const uint8_t* data, size_t size)
    {
        static constexpr auto prefix_length = 2u;
        static constexpr auto mask_length = 4u;

        header_length_ = 0;
        data_length_ = 0;

        if (size < prefix_length || (data[1] & 0x80) == 0)
            return false;

        const size_t length = (data[1] & 0x7f);

        if (size >= mask_length && length < 126u)
        {
            data_length_ = length;
            header_length_ = prefix_length + mask_length;
        }
        else if (size >= prefix_length + sizeof(uint16_t) + mask_length &&
            length == 126u)
        {
            data_length_ = from_big_endian<uint16_t>(&data[prefix_length],
                &data[prefix_length + sizeof(uint16_t)]);

            header_length_ = prefix_length + sizeof(uint16_t) + mask_length;
        }
        else if (size >= prefix_length + sizeof(uint64_t) + mask_length)
        {
            data_length_ = from_big_endian<uint64_t>(&data[prefix_length],
                &data[prefix_length + sizeof(uint64_t)]);

            header_length_ = prefix_length + sizeof(uint64_t) + mask_length;
        }

        return true;
    }

    size_t header_length() const
    {
        return header_length_;
    }

    size_t data_length() const
    {
        return data_length_;
    }

private:
    size_t header_length_;
    size_t data_length_;
};

struct ssl
{
    bool enabled;
    std::string hostname;
#ifdef WITH_MBEDTLS
    mbedtls_ssl_context context;
    mbedtls_ssl_config configuration;
    mbedtls_pk_context key;
    mbedtls_x509_crt certificate;
    mbedtls_x509_crt ca_certificate;
#endif
};

struct bind_options
{
    void* user_data;
    uint32_t flags;
    std::string ssl_key;
    std::string ssl_certificate;
    std::string ssl_ca_certificate;
    std::string ssl_cipers;
};

struct file_transfer
{
    bool in_progress;
    FILE* descriptor;
    size_t offset;
    size_t length;
};

struct websocket_transfer
{
    bool in_progress;
    size_t offset;
    size_t length;
    size_t header_length;
    data_chunk mask;
    data_chunk data;
};

class http_request
{
public:
    std::string find(const string_map& haystack,
        const std::string& needle) const
    {
        const auto it = haystack.find(needle);
        return it == haystack.end() ? std::string{} : it->second;
    }

    std::string header(std::string header) const
    {
        boost::algorithm::to_lower(header);
        return find(headers, header);
    }

    std::string parameter(std::string parameter) const
    {
        boost::algorithm::to_lower(parameter);
        return find(parameters, parameter);
    }

    std::string method;
    std::string uri;
    std::string protocol;
    double protocol_version;
    size_t message_length;
    size_t content_length;
    string_map headers;
    string_map parameters;
    bool upgrade_request;
    bool json_rpc;
    boost::property_tree::ptree json_tree;
};

struct http_reply
{
    static std::string to_string(protocol_status status)
    {
        typedef std::unordered_map<protocol_status, std::string> status_map;
        static const status_map status_strings
        {
            { protocol_status::switching, "HTTP/1.1 101 Switching Protocols\r\n" },
            { protocol_status::ok, "HTTP/1.0 200 OK\r\n" },
            { protocol_status::created, "HTTP/1.0 201 Created\r\n" },
            { protocol_status::accepted, "HTTP/1.0 202 Accepted\r\n" },
            { protocol_status::no_content, "HTTP/1.0 204 No Content\r\n" },
            { protocol_status::multiple_choices, "HTTP/1.0 300 Multiple Choices\r\n" },
            { protocol_status::moved_permanently, "HTTP/1.0 301 Moved Permanently\r\n" },
            { protocol_status::moved_temporarily, "HTTP/1.0 302 Moved Temporarily\r\n" },
            { protocol_status::not_modified, "HTTP/1.0 304 Not Modified\r\n" },
            { protocol_status::bad_request, "HTTP/1.0 400 Bad Request\r\n" },
            { protocol_status::unauthorized, "HTTP/1.0 401 Unauthorized\r\n" },
            { protocol_status::forbidden, "HTTP/1.0 403 Forbidden\r\n" },
            { protocol_status::not_found, "HTTP/1.0 404 Not Found\r\n" },
            { protocol_status::internal_server_error, "HTTP/1.0 500 Internal Server Error\r\n" },
            { protocol_status::not_implemented, "HTTP/1.0 501 Not Implemented\r\n" },
            { protocol_status::bad_gateway, "HTTP/1.0 502 Bad Gateway\r\n" },
            { protocol_status::service_unavailable, "HTTP/1.0 503 Service Unavailable\r\n" }
        };

        const auto it = status_strings.find(status);
        return it == status_strings.end() ? std::string{} : it->second;
    }

    static std::string generate(protocol_status status, std::string mime_type,
        size_t content_length, bool keep_alive)
    {
        static const size_t max_date_time_length = 32;
        std::array<char, max_date_time_length> time_buffer{};
        const auto current_time = std::time(nullptr);

        // BUGBUG: std::gmtime may not be thread safe.
        std::strftime(time_buffer.data(), time_buffer.size(),
            "%a, %d %b %Y %H:%M:%S GMT", std::gmtime(&current_time));

        std::stringstream response;
        response
            << to_string(status)
            << "Date: " << time_buffer.data() << "\r\n"
            << "Accept-Ranges: none\r\n"
            << "Connection: " << (keep_alive ? "keep-alive" : "close")
            << "\r\n";

        if (!mime_type.empty())
            response << "Content-Type: " << mime_type << "\r\n";

        if (content_length > 0)
            response << "Content-Length: " << content_length << "\r\n";

        response << "\r\n";
        return response.str();
    }

    static std::string generate_upgrade(const std::string& key_response,
        const std::string& protocol)
    {
        std::stringstream response;
        response
            << to_string(protocol_status::switching)
            << "Upgrade: websocket\r\n"
            << "Connection: Upgrade" << "\r\n";

        if (!protocol.empty())
            response << protocol << "\r\n";

        response << "Sec-WebSocket-Accept: " << key_response << "\r\n\r\n";
        return response.str();
    }

    protocol_status status;
    string_map headers;
    std::string content;
};

} // namespace http
} // namespace server
} // namespace libbitcoin

#endif
