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
#include <bitcoin/protocol.hpp>
#include <bitcoin/server/define.hpp>

#include "http.hpp"
#include "utilities.hpp"

namespace libbitcoin {
namespace server {
namespace http {

std::string error_string()
{
#ifdef WIN32
    LPVOID buffer{};
    LPVOID display_buffer{};
    DWORD dw = last_error();

    FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, dw,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        static_cast<LPTSTR>(&buffer), 0, nullptr);

    display_buffer = static_cast<LPVOID>(LocalAlloc(LMEM_ZEROINIT,
        (lstrlen(static_cast<LPCTSTR>(buffer)) +
         lstrlen(static_cast<LPCTSTR>(lpszFunction)) + 40) * sizeof(TCHAR)));

    StringCchPrintf(static_cast<LPTSTR>(display_buffer),
                    LocalSize(display_buffer) / sizeof(TCHAR),
                    TEXT("%s failed with error %d: %s"),
                    lpszFunction, dw, buffer);

    const std::string error_string = { display_buffer };

    LocalFree(buffer);
    LocalFree(display_buffer);

    return error_string;
#else
    return { strerror(last_error()) };
#endif
}

#ifdef WITH_MBEDTLS
std::string mbedtls_error_string(int error)
{
    static constexpr size_t error_buffer_length = 256;
    std::array<char, error_buffer_length> data{};
    mbedtls_strerror(error, data.data(), data.size());
    return { data.data() };
}

sha1_hash sha1(const std::string& input)
{
    sha1_hash out{};
    mbedtls_sha1_context context;
    mbedtls_sha1_init(&context);

    const auto source = reinterpret_cast<const unsigned char*>(input.c_str());
    if ((mbedtls_sha1_starts_ret(&context) == 0) &&
        (mbedtls_sha1_update_ret(&context, source, input.size()) == 0) &&
        (mbedtls_sha1_finish_ret(&context, out.data()) == 0))
        return out;

    return {};
}
#endif

std::string to_string(websocket_op code)
{
    static const std::unordered_map<uint8_t, std::string> opcode_map =
    {
        { static_cast<uint8_t>(websocket_op::continuation), "continue" },
        { static_cast<uint8_t>(websocket_op::text), "text" },
        { static_cast<uint8_t>(websocket_op::binary), "binary" },
        { static_cast<uint8_t>(websocket_op::close), "close" },
        { static_cast<uint8_t>(websocket_op::ping), "ping" },
        { static_cast<uint8_t>(websocket_op::pong), "pong" }
    };

    auto it = opcode_map.find(static_cast<uint8_t>(code));
    if (it != opcode_map.end())
        return it->second;

    return { "unknown" };
}

// Generates the RFC6455 handshake response described here:
// https://tools.ietf.org/html/rfc6455#section-1.3
std::string websocket_key_response(const std::string& websocket_key)
{
    static const std::string rfc6455_guid =
        "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

#ifdef WITH_MBEDTLS
    // The required buffer size is for a base64 encoded sha1 hash (20
    // bytes in length).
    static constexpr size_t key_buffer_length = 64;
    std::array<unsigned char, key_buffer_length> buffer{};

    size_t processed_length = 0;
    const auto input = websocket_key + rfc6455_guid;
    const auto hash = sha1(input);

    if ((mbedtls_base64_encode(buffer.data(), buffer.size(), &processed_length,
        hash.data(), sha1_hash_length) != 0) || (processed_length == 0))
        return {};

    return { reinterpret_cast<char*>(buffer.data()), processed_length };
#else
    const auto input = websocket_key + rfc6455_guid;
    const data_chunk input_data(input.begin(), input.end());
    const data_slice slice(bc::sha1_hash(input_data));
    return encode_base64(slice);
#endif
}

bool is_json_request(const std::string& header_value)
{
    return header_value == "application/json-rpc" ||
        header_value == "application/json" ||
        header_value == "application/jsonrequest";
}

bool parse_http(const std::string& request, struct http_request& out)
{
    auto& headers = out.headers;
    auto& parameters = out.parameters;

    out.message_length = request.size();

    // Parse out the first line for: Method, URI, Protocol.
    auto position = request.find("\r\n");
    if (position == std::string::npos)
        return false;

    std::string method_line = request.substr(0, position);
    string_list elements;
    boost::split(elements, method_line, boost::is_any_of(" "));
    if (elements.size() != 3)
        return false;

    for (auto& element: elements)
        boost::algorithm::trim(element);

    // truncate the parameters from the uri (if any)
    position = elements[1].find("?");
    if (position != std::string::npos)
        elements[1] = elements[1].substr(0, position);

    boost::algorithm::to_lower(elements[0]);
    boost::algorithm::to_lower(elements[2]);

    out.method = elements[0];
    out.uri = elements[1];
    out.protocol = elements[2];

    position = out.protocol.find("/");
    if (position != std::string::npos)
    {
        const auto version = out.protocol.substr(position);
        out.protocol_version = strtod(version.c_str(), nullptr);
    }

    LOG_VERBOSE(LOG_SERVER_HTTP)
        << "Parsing HTTP request: Method: " << out.method << ", Uri: "
        << out.uri <<  ", Protocol: " << out.protocol;

    // Parse out remaining lines to build both the header map and
    // parameter map.
    string_list header_lines;
    boost::split(header_lines, request, boost::is_any_of("\r\n"));

    auto parse_line = [&headers](const std::string& line)
    {
        if (line.empty())
            return;

        string_list elements;
        boost::split(elements, line, boost::is_any_of(":"));
        if (elements.size() == 2)
        {
            boost::algorithm::trim(elements[0]);
            boost::algorithm::trim(elements[1]);
            boost::algorithm::to_lower(elements[0]);
            if (elements[0] != "sec-websocket-key")
                boost::algorithm::to_lower(elements[1]);
            headers[elements[0]] = elements[1];
        }
        else if (elements.size() > 2)
        {
            std::stringstream ss;
            for (size_t i = 0; i < elements.size(); i++)
            {
                boost::algorithm::trim(elements[i]);
                if (elements[0] != "sec-websocket-key")
                    boost::algorithm::to_lower(elements[i]);
                if (i > 0)
                {
                    ss << elements[i];
                    if (i != elements.size() -1)
                        ss << ":";
                }
            }

            headers[elements[0]] = ss.str();
        }
    };

    std::for_each(header_lines.begin(), header_lines.end(), parse_line);

    // Parse passed parameters (if any)
    position = method_line.find("?");
    if (position != std::string::npos)
    {
        auto parameter_string = method_line.substr(position + 1);

        string_list parameter_elements;
        boost::split(parameter_elements, parameter_string,
            boost::is_any_of("&"));

        auto parse_parameter = [&parameters](const std::string& line)
        {
            if (line.empty())
                return;

            auto line_end = line.find(" ");
            const auto input_line = ((line_end == std::string::npos) ? line :
                line.substr(0, line_end));

            string_list elements;
            boost::split(elements, input_line, boost::is_any_of("= "));
            if (elements.size() == 2)
            {
                boost::algorithm::trim(elements[0]);
                boost::algorithm::trim(elements[1]);
                boost::algorithm::to_lower(elements[0]);
                boost::algorithm::to_lower(elements[1]);
                parameters[elements[0]] = elements[1];
            }
        };

        std::for_each(parameter_elements.begin(), parameter_elements.end(),
            parse_parameter);
    }

    // Determine if this request contains the content length.
    auto content_length = headers.find("content-length");
    out.content_length = (content_length != headers.end() ?
        std::strtoul(content_length->second.c_str(), nullptr, 0) : 0);

    // Determine if this request is an upgrade request
    auto connection = headers.find("connection");
    out.upgrade_request = ((connection != headers.end()) &&
         (connection->second.find("upgrade") != std::string::npos) &&
         (headers.find("sec-websocket-key") != headers.end()));

    // Determine if this request is a JSON-RPC request, or at least
    // one that we support (i.e. non-standard; may not contain the
    // required accept header nor the optional content-type header),
    // via POST-only, etc.  Instead we try to safely parse the data as
    // JSON.
    if (out.method == "post" && out.content_length > 0)
    {
        const auto json_request = request.substr(request.size() -
            out.content_length, out.content_length);
        LOG_VERBOSE(LOG_SERVER_HTTP)
            << "POST content: " << json_request;
        out.json_rpc = bc::property_tree(out.json_tree, json_request);
    }

    return true;
}

unsigned long resolve_hostname(const std::string& hostname)
{
    unsigned long address = 0;

#ifdef WIN32
#define get_address(host, address) *address = inet_addr(host)
#define validate_address(address) (address != INADDR_NONE)
#define host_info HOSTENT
#else
#define get_address(host, address) inet_pton(AF_INET, host, address)
#define validate_address(address) (address > 0)
#define host_info struct hostent
#endif

    // Resolve dotted ip address.
    if (!validate_address(get_address(hostname.c_str(), &address)))
    {
        // Resolve host name.
        const host_info *resolved = gethostbyname(hostname.c_str());
        if (resolved)
            address = *(reinterpret_cast<unsigned long*>(
                resolved->h_addr_list[0]));
    }

#undef get_address
#undef validate_address
#undef host_info

    return address;
}

std::string mime_type(const std::string& filename)
{
    std::unordered_map<std::string, std::string> mime_type_map =
    {
        { "html", "text/html" },
        { "htm", "text/html" },
        { "shtm", "text/html" },
        { "shtml", "text/html" },
        { "css", "text/css" },
        { "js", "application/x-javascript" },
        { "ico", "image/x-icon" },
        { "gif", "image/gif" },
        { "jpg", "image/jpeg" },
        { "jpeg", "image/jpeg" },
        { "png", "image/png" },
        { "svg", "image/svg+xml" },
        { "md", "text/plain" },
        { "txt", "text/plain" },
        { "torrent", "application/x-bittorrent" },
        { "wav", "audio/x-wav" },
        { "mp3", "audio/x-mp3" },
        { "mid", "audio/mid" },
        { "m3u", "audio/x-mpegurl" },
        { "ogg", "application/ogg" },
        { "ram", "audio/x-pn-realaudio" },
        { "xml", "text/xml" },
        { "ttf", "application/x-font-ttf" },
        { "json", "application/json" },
        { "xslt", "application/xml" },
        { "xsl", "application/xml" },
        { "ra", "audio/x-pn-realaudio" },
        { "doc", "application/msword" },
        { "exe", "application/octet-stream" },
        { "zip", "application/x-zip-compressed" },
        { "xls", "application/excel" },
        { "tgz", "application/x-tar-gz" },
        { "tar", "application/x-tar" },
        { "gz", "application/x-gunzip" },
        { "arj", "application/x-arj-compressed" },
        { "rar", "application/x-rar-compressed" },
        { "rtf", "application/rtf" },
        { "pdf", "application/pdf" },
        { "swf", "application/x-shockwave-flash" },
        { "mpg", "video/mpeg" },
        { "webm", "video/webm" },
        { "mpeg", "video/mpeg" },
        { "mov", "video/quicktime" },
        { "mp4", "video/mp4" },
        { "m4v", "video/x-m4v" },
        { "asf", "video/x-ms-asf" },
        { "avi", "video/x-msvideo" },
        { "bmp", "image/bmp" }
    };

    const auto dot_position = filename.rfind(".");
    if (dot_position != std::string::npos)
    {
        const auto extension = filename.substr(dot_position + 1);
        auto it = mime_type_map.find(extension);
        if (it != mime_type_map.end())
            return it->second;
    }

    return { "text/plain" };
}

} // namespace http
} // namespace server
} // namespace libbitcoin
