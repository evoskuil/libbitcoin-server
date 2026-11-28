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
#include <bitcoin/server/utilities/template.hpp>

#include <algorithm>
#include <optional>
#include <bitcoin/server/define.hpp>

namespace libbitcoin {
namespace server {

using namespace system;
using namespace system::chain;

// The pool judges locktime at the context timestamp before bip113.
uint32_t minimum_timestamp(const context& ctx,
    const system::settings& settings) NOEXCEPT
{
    const auto interval = settings.retargeting_interval();
    const auto position = ctx.height % interval;
    auto minimum = add1(ctx.median_time_past);

    if (!ctx.is_enabled(flags::bip113_rule))
        minimum = std::max(minimum, ctx.timestamp);

    const auto bip94 = ctx.is_enabled(flags::time_warp_patch);
    const auto bip54 = ctx.is_enabled(flags::bip54_timewarp_rule);
    if ((bip94 || bip54) && is_zero(position))
    {
        const auto limit = bip94 ? max_timewarp : max_timewarp_bip54;
        const auto earliest =
            floored_subtract<uint32_t>(ctx.previous_timestamp, limit);

        minimum = std::max(minimum, earliest);
    }

    if (ctx.is_enabled(flags::bip54_timespan_rule) &&
        (position == sub1(interval)))
        minimum = std::max(minimum, ctx.retarget_timestamp);

    return minimum;
}

// Minimum difficulty is allowed off the retarget height, of a block more than
// two spacings after its parent.
std::optional<uint32_t> easy_timestamp(const context& ctx,
    const system::settings& settings) NOEXCEPT
{
    const auto& forks = settings.forks;
    const auto interval = settings.retargeting_interval();
    if (!forks.retarget || forks.difficult || is_zero(ctx.height % interval))
        return {};

    const auto spacing = shift_left(settings.block_spacing_seconds);
    return ceilinged_add(ctx.previous_timestamp, add1(spacing));
}

uint32_t work_required(const context& ctx, uint32_t timestamp,
    const system::settings& settings) NOEXCEPT
{
    const auto easy = easy_timestamp(ctx, settings);
    return easy && (timestamp >= easy.value()) ?
        settings.proof_of_work_limit : ctx.work_required;
}

} // namespace server
} // namespace libbitcoin
