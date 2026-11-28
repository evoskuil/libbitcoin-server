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
#include "../test.hpp"

BC_AUTO_TEST_SUITE(template_tests)

using namespace system;
using namespace system::chain;

static const system::settings mainnet{ selection::mainnet };
static const system::settings testnet{ selection::testnet3 };
constexpr uint32_t hard = 0x1b0404cb;

// flags, timestamp, median_time_past, height, minimum_block_version,
// work_required, previous_timestamp, retarget_timestamp.

BC_AUTO_TEST_CASE(template__minimum_timestamp__bip113__above_median_time_past)
{
    const context ctx{ flags::bip113_rule, 500, 100, 5, 0, hard, 500, 0 };
    BC_REQUIRE_EQUAL(minimum_timestamp(ctx, mainnet), 101u);
}

BC_AUTO_TEST_CASE(template__minimum_timestamp__before_bip113__context_timestamp)
{
    const context ctx{ flags::no_rules, 500, 100, 5, 0, hard, 500, 0 };
    BC_REQUIRE_EQUAL(minimum_timestamp(ctx, mainnet), 500u);
}

BC_AUTO_TEST_CASE(template__minimum_timestamp__bip94_first_of_period__previous_less_limit)
{
    const context ctx{ flags::bip113_rule | flags::time_warp_patch, 10'000, 100, 2016, 0, hard, 10'000, 0 };
    BC_REQUIRE_EQUAL(minimum_timestamp(ctx, mainnet), 9'400u);
}

BC_AUTO_TEST_CASE(template__minimum_timestamp__bip54_timewarp_first_of_period__previous_less_limit)
{
    const context ctx{ flags::bip113_rule | flags::bip54_timewarp_rule, 10'000, 100, 2016, 0, hard, 10'000, 0 };
    BC_REQUIRE_EQUAL(minimum_timestamp(ctx, mainnet), 2'800u);
}

BC_AUTO_TEST_CASE(template__minimum_timestamp__bip54_timewarp_within_period__above_median_time_past)
{
    const context ctx{ flags::bip113_rule | flags::bip54_timewarp_rule, 10'000, 100, 2017, 0, hard, 10'000, 0 };
    BC_REQUIRE_EQUAL(minimum_timestamp(ctx, mainnet), 101u);
}

BC_AUTO_TEST_CASE(template__minimum_timestamp__bip94_floor_below_median_time_past__above_median_time_past)
{
    const context ctx{ flags::bip113_rule | flags::time_warp_patch, 300, 100, 2016, 0, hard, 300, 0 };
    BC_REQUIRE_EQUAL(minimum_timestamp(ctx, mainnet), 101u);
}

BC_AUTO_TEST_CASE(template__minimum_timestamp__bip54_timespan_last_of_period__retarget_timestamp)
{
    const context ctx{ flags::bip113_rule | flags::bip54_timespan_rule, 9'000, 100, 4031, 0, hard, 9'000, 5'000 };
    BC_REQUIRE_EQUAL(minimum_timestamp(ctx, mainnet), 5'000u);
}

BC_AUTO_TEST_CASE(template__minimum_timestamp__bip54_timespan_within_period__above_median_time_past)
{
    const context ctx{ flags::bip113_rule | flags::bip54_timespan_rule, 9'000, 100, 4030, 0, hard, 9'000, 5'000 };
    BC_REQUIRE_EQUAL(minimum_timestamp(ctx, mainnet), 101u);
}

BC_AUTO_TEST_CASE(template__easy_timestamp__mainnet__empty)
{
    const context ctx{ flags::bip113_rule, 1'000, 100, 5, 0, hard, 1'000, 0 };
    BC_REQUIRE(!easy_timestamp(ctx, mainnet).has_value());
}

BC_AUTO_TEST_CASE(template__easy_timestamp__testnet_retarget_height__empty)
{
    const context ctx{ flags::bip113_rule, 1'000, 100, 2016, 0, hard, 1'000, 0 };
    BC_REQUIRE(!easy_timestamp(ctx, testnet).has_value());
}

BC_AUTO_TEST_CASE(template__easy_timestamp__testnet__above_two_spacings_after_previous)
{
    const context ctx{ flags::bip113_rule, 1'000, 100, 5, 0, hard, 1'000, 0 };
    BC_REQUIRE_EQUAL(easy_timestamp(ctx, testnet).value(), 2'201u);
}

BC_AUTO_TEST_CASE(template__work_required__mainnet_late__context_work)
{
    const context ctx{ flags::bip113_rule, 1'000, 100, 5, 0, hard, 1'000, 0 };
    BC_REQUIRE_EQUAL(work_required(ctx, 9'000, mainnet), hard);
}

BC_AUTO_TEST_CASE(template__work_required__testnet_two_spacings__context_work)
{
    const context ctx{ flags::bip113_rule, 1'000, 100, 5, 0, hard, 1'000, 0 };
    BC_REQUIRE_EQUAL(work_required(ctx, 2'200, testnet), hard);
}

BC_AUTO_TEST_CASE(template__work_required__testnet_above_two_spacings__proof_of_work_limit)
{
    const context ctx{ flags::bip113_rule, 1'000, 100, 5, 0, hard, 1'000, 0 };
    BC_REQUIRE_EQUAL(work_required(ctx, 2'201, testnet), testnet.proof_of_work_limit);
}

BC_AUTO_TEST_CASE(template__work_required__testnet_retarget_height_late__context_work)
{
    const context ctx{ flags::bip113_rule, 1'000, 100, 2016, 0, hard, 1'000, 0 };
    BC_REQUIRE_EQUAL(work_required(ctx, 9'000, testnet), hard);
}

BC_AUTO_TEST_SUITE_END()
