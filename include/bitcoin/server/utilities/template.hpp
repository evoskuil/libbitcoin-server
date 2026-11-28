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
#ifndef LIBBITCOIN_SERVER_UTILITIES_TEMPLATE_HPP
#define LIBBITCOIN_SERVER_UTILITIES_TEMPLATE_HPP

#include <optional>
#include <bitcoin/server/define.hpp>

namespace libbitcoin {
namespace server {

/// Earliest timestamp of a valid block of the template context.
BCS_API uint32_t minimum_timestamp(const system::chain::context& ctx,
    const system::settings& settings) NOEXCEPT;

/// Earliest timestamp of a minimum difficulty block of the template context,
/// empty where the context allows none (testnet).
BCS_API std::optional<uint32_t> easy_timestamp(
    const system::chain::context& ctx,
    const system::settings& settings) NOEXCEPT;

/// Work required of a block of the template context with the timestamp.
BCS_API uint32_t work_required(const system::chain::context& ctx,
    uint32_t timestamp, const system::settings& settings) NOEXCEPT;

} // namespace server
} // namespace libbitcoin

#endif
