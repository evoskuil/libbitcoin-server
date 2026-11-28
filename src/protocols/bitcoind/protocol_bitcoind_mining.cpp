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
#include <bitcoin/server/protocols/protocol_bitcoind_mining.hpp>

#include <bitcoin/server/define.hpp>
#include <bitcoin/server/interfaces/interfaces.hpp>
#include <bitcoin/server/parsers/parsers.hpp>
#include <bitcoin/server/serializers/serializers.hpp>
#include <bitcoin/server/utilities/utilities.hpp>

namespace libbitcoin {
namespace server {

#define CLASS protocol_bitcoind_mining
#define SUBSCRIBE_BITCOIND(method, ...) \
    subscribe<CLASS>(&CLASS::method, __VA_ARGS__)

using namespace system;
using namespace network;
using namespace network::rpc;
using namespace network::messages;
using namespace std::placeholders;
using namespace system::json;

BC_PUSH_WARNING(NO_THROW_IN_NOEXCEPT)
BC_PUSH_WARNING(SMART_PTR_NOT_NEEDED)
BC_PUSH_WARNING(NO_VALUE_OR_CONST_REF_SHARED_PTR)

// bitcoind names the deployments of bip9 bits zero to two csv, segwit and
// taproot, and marks as required (!) a rule a client cannot ignore.
struct named_deployment
{
    std::string_view name;
    const chain::chain_state::deployment& deployment;
    bool required;
};

using named_deployments = std::array<named_deployment, 8>;

static named_deployments to_named(const system::settings& bitcoin) NOEXCEPT
{
    return
    {{
        { "csv", bitcoin.bip9_bit0_deployment, false },
        { "segwit", bitcoin.bip9_bit1_deployment, true },
        { "taproot", bitcoin.bip9_bit2_deployment, false },
        { "bip54_timewarp", bitcoin.bip54_timewarp_deployment, true },
        { "bip54_timespan", bitcoin.bip54_timespan_deployment, true },
        { "bip54_sigops", bitcoin.bip54_sigops_deployment, true },
        { "bip54_txsize", bitcoin.bip54_txsize_deployment, true },
        { "bip54_coinbase", bitcoin.bip54_coinbase_deployment, true }
    }};
}

// The flags of the deployments named by the rules of the request.
static uint32_t to_declared(const object_t& request,
    const named_deployments& named) NOEXCEPT
{
    uint32_t declared{};
    const auto rules = request.find("rules");
    if (rules == request.end())
        return declared;

    const auto names = std::get_if<array_t>(&rules->second.value());
    if (is_null(names))
        return declared;

    for (const auto& rule: *names)
    {
        const auto name = std::get_if<string_t>(&rule.value());
        if (is_null(name))
            continue;

        for (const auto& item: named)
            if (*name == item.name)
                declared |= item.deployment.flags;
    }

    return declared;
}

// Start.
// ----------------------------------------------------------------------------

void protocol_bitcoind_mining::start() NOEXCEPT
{
    BC_ASSERT(stranded());

    if (started())
        return;

    SUBSCRIBE_BITCOIND(handle_get_network_hash_ps, _1, _2, _3, _4);
    SUBSCRIBE_BITCOIND(handle_get_mining_info, _1, _2);
    SUBSCRIBE_BITCOIND(handle_submit_block, _1, _2, _3, _4);
    SUBSCRIBE_BITCOIND(handle_submit_header, _1, _2, _3);
    SUBSCRIBE_BITCOIND(handle_get_block_template, _1, _2, _3);
    SUBSCRIBE_BITCOIND(handle_get_prioritised_transactions, _1, _2);
    SUBSCRIBE_BITCOIND(handle_prioritise_transaction, _1, _2);
    submit_t::start();
}

void protocol_bitcoind_mining::stopping(const code& ec) NOEXCEPT
{
    BC_ASSERT(stranded());
    if (!miner_.empty())
        mining()->remove(miner_);

    submit_t::stopping(ec);
}

std::string protocol_bitcoind_mining::service() const NOEXCEPT
{
    return "bitcoind";
}

// Mining methods.
// ----------------------------------------------------------------------------

void protocol_bitcoind_mining::handle_get_network_hash_ps(const code& ec,
    rpc_interface::get_network_hash_ps, double nblocks,
    double height) NOEXCEPT
{
    if (stopped(ec))
        return;

    const auto& query = archive();
    const auto top = query.get_top_confirmed();

    // A negative height selects the confirmed top.
    size_t target{};
    if (height < 0)
    {
        target = top;
    }
    else if (!to_integer(target, height))
    {
        send_error(error::bitcoind::invalid_parameter);
        return;
    }
    else
    {
        target = std::min(target, top);
    }

    // A non-positive window selects the span since the last retarget.
    size_t window{};
    if (nblocks <= 0)
    {
        window = add1(target % system_settings().retargeting_interval());
    }
    else if (!to_integer(window, nblocks))
    {
        send_error(error::bitcoind::invalid_parameter);
        return;
    }

    window = std::min(window, target);
    if (is_zero(window))
    {
        send_result(to64(zero));
        return;
    }

    // The pinned ancestry makes the window walk reorg-stable.
    database::header_links branch{};
    if (!query.get_ancestry(branch, query.to_confirmed(target), add1(window)))
    {
        send_error(error::bitcoind::internal_error);
        return;
    }

    // The window timespan is bounded by its observed timestamps.
    auto minimum = max_uint32;
    auto maximum = min_uint32;
    for (const auto& ancestor: branch)
    {
        const auto header = query.get_header(ancestor);
        if (!header)
        {
            send_error(error::bitcoind::internal_error);
            return;
        }

        minimum = std::min(minimum, header->timestamp());
        maximum = std::max(maximum, header->timestamp());
    }

    if (minimum == maximum)
    {
        send_result(to64(zero));
        return;
    }

    uint256_t start_work{}, end_work{};
    if (!query.get_branch_work(start_work, branch.back()) ||
        !query.get_branch_work(end_work, branch.front()))
    {
        send_error(error::bitcoind::internal_error);
        return;
    }

    const auto work = (end_work - start_work).convert_to<double>();
    send_result(work / (maximum - minimum));
}

void protocol_bitcoind_mining::handle_get_mining_info(const code& ec,
    rpc_interface::get_mining_info) NOEXCEPT
{
    if (stopped(ec))
        return;

    get_confirmable_txs(BIND(handle_mining_info, _1, _2, gate()));
}

void protocol_bitcoind_mining::handle_mining_info(const code& ec,
    const database::tx_links& links, const gate_t::ptr& gate) NOEXCEPT
{
    if (stopped())
        return;

    POST(do_mining_info, ec, links, gate);
}

// bitcoind omits currentblockweight/currentblocktx until block assembly.
void protocol_bitcoind_mining::do_mining_info(const code& ec,
    const database::tx_links& links, const gate_t::ptr&) NOEXCEPT
{
    BC_ASSERT(stranded());
    using namespace chain;

    if (stopped())
        return;

    if (ec && ec != node::error::pooling_disabled)
    {
        send_error(error::bitcoind::internal_error);
        return;
    }

    size_t height{};
    const auto& query = archive();
    const auto link = query.to_top_confirmed();
    const auto top = query.get_header(link);
    if (!top || !query.get_height(height, link))
    {
        send_error(error::bitcoind::internal_error);
        return;
    }

    // The pool state over the top block carries the next work required.
    const auto& bitcoin = system_settings();
    const auto key = query.get_header_key(link);
    const auto state = query.get_confirmed_chain_state(bitcoin, key);
    if (!state)
    {
        send_error(error::bitcoind::internal_error);
        return;
    }

    const chain_state pool{ *state, bitcoin };
    const header header{ 0, {}, {}, 0, pool.work_required(), 0 };
    object_t next_block
    {
        { "height", to64(add1(height)) },
        { "bits", encode_base16(to_big_endian(header.bits())) },
        { "difficulty", header.difficulty() },
        { "target", encode_hash(from_uintx(chain::compact::expand(header.bits()))) }
    };

    const auto minimum = node_settings().pool.minimum_fee_rate_();
    const auto span = to_floating(power2<uint64_t>(32u));
    const auto period = bitcoin.block_spacing_seconds;

    // bitcoind "blocks" is a height, not a count.
    send_result(object_t
    {
        { "blocks", to64(height) },
        { "bits", encode_base16(to_big_endian(top->bits())) },
        { "difficulty", top->difficulty() },
        { "target", encode_hash(from_uintx(chain::compact::expand(top->bits()))) },
        { "networkhashps", top->difficulty() * span / period },
        { "pooledtx", to64(links.size()) },
        { "blockmintxfee", to_btc(minimum) },
        { "chain", chain_name(bitcoin) },
        { "next", std::move(next_block) },
        { "warnings", array_t{} }
    });
}

void protocol_bitcoind_mining::handle_submit_block(const code& ec,
    rpc_interface::submit_block, const std::string& hexdata,
    const std::string&) NOEXCEPT
{
    if (stopped(ec))
        return;

    data_chunk data{};
    if (!decode_base16(data, hexdata))
    {
        send_error(error::bitcoind::deserialization_error);
        return;
    }

    const auto header = emplace_shared<const chain::header>(data);
    const auto block = emplace_shared<const chain::view::block>(
        std::move(data), true);

    if (!header->is_valid() || !block->is_valid())
    {
        send_error(error::bitcoind::deserialization_error);
        return;
    }

    submit_block(header, block, options().trust_blocks, gate());
}

// bip22: null when accepted, otherwise duplicate, inconclusive or reject token.
void protocol_bitcoind_mining::handle_submitted(const hash_digest& hash,
    submission outcome, const code& ec) NOEXCEPT
{
    BC_ASSERT(stranded());

    std::string result{};
    switch (outcome)
    {
        case submission::accepted:
        {
            result = "submitted";
            break;
        }
        case submission::duplicate:
        {
            result = "duplicate";
            break;
        }
        case submission::duplicate_inconclusive:
        {
            result = "duplicate-inconclusive";
            break;
        }
        case submission::duplicate_invalid:
        {
            result = "duplicate-invalid";
            break;
        }
        case submission::inconclusive:
        {
            result = "inconclusive";
            break;
        }
        case submission::bad_parent:
        {
            result = "bad-prevblk";
            break;
        }
        case submission::rejected:
        {
            result = error::bitcoind::reject(ec);
            break;
        }
        case submission::failed:
        {
            result = "failed";
            break;
        }
    }

    record_submitted(hash, outcome, result);

    if (outcome == submission::failed)
        send_error(error::bitcoind::internal_error);
    else if (outcome == submission::accepted)
        send_result(null_t{});
    else
        send_result(string_t{ result });
}

// The response defers to organize completion (see dispatch).
void protocol_bitcoind_mining::handle_submit_header(const code& ec,
    rpc_interface::submit_header, const std::string& hexdata) NOEXCEPT
{
    if (stopped(ec))
        return;

    data_chunk data{};
    if (!decode_base16(data, hexdata))
    {
        send_error(error::bitcoind::deserialization_error);
        return;
    }

    const auto header = emplace_shared<chain::header>(data);
    if (!header->is_valid())
    {
        send_error(error::bitcoind::deserialization_error);
        return;
    }

    if (!archive().to_header(header->hash()).is_terminal())
    {
        send_result(null_t{});
        return;
    }

    organize(header, BIND(handle_organize_header, _1, _2, gate()));
}

void protocol_bitcoind_mining::handle_organize_header(const code& ec,
    size_t, const gate_t::ptr& gate) NOEXCEPT
{
    if (stopped())
        return;

    POST(do_submit_header, ec, gate);
}

void protocol_bitcoind_mining::do_submit_header(const code& ec,
    const gate_t::ptr&) NOEXCEPT
{
    BC_ASSERT(stranded());
    using namespace error::bitcoind;
    if (ec)
        send_error(translate(ec, verify_error));
    else
        send_result(null_t{});
}

// bip22: template mode only (not bip23 proposals).
// bip145: client must declare segwit rule.
void protocol_bitcoind_mining::handle_get_block_template(const code& ec,
    rpc_interface::get_block_template,
    const object_t& template_request) NOEXCEPT
{
    if (stopped(ec))
        return;

    const auto mode = template_request.find("mode");
    if (mode != template_request.end())
    {
        const auto name = std::get_if<string_t>(&mode->second.value());
        if (is_null(name) || *name != "template")
        {
            send_error(error::bitcoind::invalid_parameter);
            return;
        }
    }

    const auto& bitcoin = system_settings();
    const auto declared = to_declared(template_request, to_named(bitcoin));
    if (!to_bool(declared & bitcoin.bip9_bit1_deployment.flags))
    {
        send_error(error::bitcoind::invalid_parameter);
        return;
    }

    uint64_t sequence{};
    const auto longpollid = template_request.find("longpollid");
    if (longpollid != template_request.end() &&
        is_current(sequence, longpollid->second))
    {
        arm_longpoll(sequence, declared);
        return;
    }

    get_template(BIND(handle_template, _1, _2, declared, gate()));
}

void protocol_bitcoind_mining::handle_template(const code& ec,
    const node::block_template::cptr& held, uint32_t declared,
    const gate_t::ptr& gate) NOEXCEPT
{
    if (stopped())
        return;

    POST(do_template, ec, held, declared, gate);
}

// A template not over the confirmed top is requested again.
void protocol_bitcoind_mining::do_template(const code& ec,
    const node::block_template::cptr& held, uint32_t declared,
    const gate_t::ptr& gate) NOEXCEPT
{
    BC_ASSERT(stranded());
    using namespace chain;

    if (stopped())
        return;

    if (ec == node::error::pooling_disabled)
    {
        send_error(error::bitcoind::client_in_initial_download);
        return;
    }

    if (ec)
    {
        send_error(error::bitcoind::internal_error);
        return;
    }

    const auto& query = archive();
    if (held->previous != query.get_top_confirmed_hash())
    {
        get_template(BIND(handle_template, _1, _2, declared, gate));
        return;
    }

    const auto& bitcoin = system_settings();
    const auto& ctx = held->context;
    const auto height = ctx.height;
    const auto bip141 = ctx.is_enabled(flags::bip141_rule);

    // Tx depends on template txs it spends, by position from one (coinbase 0).
    array_t transactions{};
    transactions.reserve(held->txs.size());
    for (size_t index{}; index < held->txs.size(); ++index)
    {
        size_t light{};
        size_t heavy{};
        const auto& tx = held->txs.at(index);
        const auto& link = held->links.at(index);
        const auto data = query.get_wire_tx(link, bip141);
        if (data.empty() || !query.get_tx_sizes(light, heavy, link))
        {
            send_error(error::bitcoind::internal_error);
            return;
        }

        array_t depends{};
        depends.reserve(tx.parents.size());
        for (const auto parent: tx.parents)
            depends.emplace_back(to64(add1(parent)));

        const auto weight = ceilinged_add(
            ceilinged_multiply(base_size_contribution, light),
            ceilinged_multiply(total_size_contribution, heavy));

        transactions.emplace_back(object_t
        {
            { "data", encode_base16(data) },
            { "txid", encode_hash(tx.txid) },
            { "hash", encode_hash(tx.wtxid) },
            { "depends", std::move(depends) },
            { "fee", tx.fee },
            { "sigops", to64(tx.sigops) },
            { "weight", to64(weight) }
        });
    }

    // A deployment is a rule once active, and available while signaled.
    array_t rules{};
    object_t available{};
    for (const auto& item: to_named(bitcoin))
    {
        const string_t name{ item.name };
        const auto& deployment = item.deployment;
        if (to_bool(ctx.flags & deployment.flags))
        {
            if (item.required && !to_bool(declared & deployment.flags))
            {
                send_error(error::bitcoind::invalid_parameter);
                return;
            }

            rules.emplace_back(item.required ? "!" + name : name);
        }
        else if (chain_state::is_signal(ctx.version,
            bitcoin.bip9_version_base, deployment.bit))
        {
            available.emplace(name, deployment.bit);
        }
    }

    const auto minimum = minimum_timestamp(ctx, bitcoin);
    const auto current = std::max(unix_time(), minimum);
    const auto bits = work_required(ctx, current, bitcoin);
    const auto subsidy = block::subsidy(height,
        bitcoin.subsidy_interval_blocks, bitcoin.initial_subsidy(),
        bitcoin.forks.bip42);

    const auto previous = encode_hash(held->previous);
    const auto longpoll = previous + std::to_string(held->sequence);

    object_t result
    {
        { "capabilities", array_t{} },
        { "version", ctx.version },
        { "rules", std::move(rules) },
        { "vbavailable", std::move(available) },
        { "vbrequired", to64(zero) },
        { "previousblockhash", previous },
        { "transactions", std::move(transactions) },
        { "coinbaseaux", object_t{} },
        { "coinbasevalue", ceilinged_add(subsidy, held->fees) },
        { "longpollid", longpoll },
        { "target", encode_hash(from_uintx(chain::compact::expand(bits))) },
        { "mintime", minimum },
        { "mutable", array_t
            {
                string_t{ "time" },
                string_t{ "transactions" },
                string_t{ "prevblock" }
            }
        },
        { "noncerange", string_t{ "00000000ffffffff" } },
        { "sigoplimit", to64(bip141 ? max_fast_sigops : max_block_sigops) },
        { "sizelimit", to64(bip141 ? max_block_weight : max_block_size) },
        { "curtime", current },
        { "bits", encode_base16(to_big_endian(bits)) },
        { "height", to64(height) }
    };

    // bip141: commitment output script of coinbase, over witness hashes of
    // template with reserved value of zero.
    if (bip141)
    {
        const data_chunk prefix{ 0x6a, 0x24, 0xaa, 0x21, 0xa9, 0xed };
        result.emplace("weightlimit", to64(max_block_weight));
        result.emplace("default_witness_commitment",
            encode_base16(splice(prefix, held->commitment)));
    }

    height_ = height;
    difficulty_ = header{ 0, {}, {}, 0, bits, 0 }.difficulty();
    register_miner();

    finish_template(result);
    send_result(std::move(result));
}

void protocol_bitcoind_mining::finish_template(object_t&) const NOEXCEPT
{
}

void protocol_bitcoind_mining::handle_get_prioritised_transactions(
    const code& ec, rpc_interface::get_prioritised_transactions) NOEXCEPT
{
    if (stopped(ec)) return;
    send_error(error::bitcoind::client_mempool_disabled);
}

void protocol_bitcoind_mining::handle_prioritise_transaction(const code& ec,
    rpc_interface::prioritise_transaction) NOEXCEPT
{
    if (stopped(ec)) return;
    send_error(error::bitcoind::client_mempool_disabled);
}

// Long polls.
// ----------------------------------------------------------------------------
// A long poll is raced by its chase subscription and the channel stop.

// The id is the previous block hash and the template sequence. Another type
// waits on the current template, and an id not current is answered at once.
bool protocol_bitcoind_mining::is_current(uint64_t& sequence,
    const value_t& longpollid) const NOEXCEPT
{
    const auto text = std::get_if<string_t>(&longpollid.value());
    if (is_null(text))
    {
        sequence = template_generation();
        return true;
    }

    constexpr auto size = two * hash_size;
    hash_digest previous{};
    return text->size() > size &&
        decode_hash(previous, text->substr(zero, size)) &&
        deserialize(sequence, text->substr(size)) &&
        previous == archive().get_top_confirmed_hash() &&
        sequence == template_generation();
}

void protocol_bitcoind_mining::arm_longpoll(uint64_t sequence,
    uint32_t declared) NOEXCEPT
{
    BC_ASSERT(stranded());

    const auto poll = emplace_shared<longpoll>(longpoll
    {
        sequence, declared, gate()
    });
    const auto race = emplace_shared<longpoll_race>(
        BIND(handle_longpoll_complete, _1, poll));

    demand_template(true);
    poll->stop = subscribe_stop(BIND(handle_longpoll_stop, _1, race));
    subscribe_events(BIND(handle_longpoll_event, _1, _2, poll, race),
        BIND(handle_longpoll_subscribed, _1, _2, poll, race));
}

// Invoked on the node strand.
// The event code is not a call status.
bool protocol_bitcoind_mining::handle_longpoll_event(const code&,
    node::event_value value, const longpoll::ptr& poll,
    const longpoll_race::ptr& race) NOEXCEPT
{
    const auto chase = node::to_chase(value);
    if (chase == node::chase::stop)
    {
        race->finish(network::error::service_stopped);
        return false;
    }

    if (chase == node::chase::template_ && longpoll_done(*poll))
    {
        race->finish(error::success);
        return false;
    }

    return true;
}

// Invoked on the node strand.
void protocol_bitcoind_mining::handle_longpoll_subscribed(const code& ec,
    node::object_key key, const longpoll::ptr& poll,
    const longpoll_race::ptr& race) NOEXCEPT
{
    POST(do_longpoll_subscribed, ec, key, poll, race);
}

// The template may have been replaced before the subscription.
void protocol_bitcoind_mining::do_longpoll_subscribed(const code& ec,
    node::object_key key, const longpoll::ptr& poll,
    const longpoll_race::ptr& race) NOEXCEPT
{
    BC_ASSERT(stranded());
    if (ec)
        return;

    poll->events = key;
    if (poll->done)
    {
        unsubscribe_events(key);
        return;
    }

    if (longpoll_done(*poll))
        race->finish(error::success);
}

void protocol_bitcoind_mining::handle_longpoll_stop(const code& ec,
    const longpoll_race::ptr& race) NOEXCEPT
{
    race->finish(ec);
}

void protocol_bitcoind_mining::handle_longpoll_complete(const code& ec,
    const longpoll::ptr& poll) NOEXCEPT
{
    POST(do_longpoll_complete, ec, poll);
}

void protocol_bitcoind_mining::do_longpoll_complete(const code& ec,
    const longpoll::ptr& poll) NOEXCEPT
{
    BC_ASSERT(stranded());
    poll->done = true;
    demand_template(false);
    unsubscribe_stop(poll->stop);

    if (!is_zero(poll->events))
        unsubscribe_events(poll->events);

    if (ec || stopped())
        return;

    get_template(BIND(handle_template, _1, _2, poll->declared, poll->gate));
}

bool protocol_bitcoind_mining::longpoll_done(
    const longpoll& poll) const NOEXCEPT
{
    return template_generation() > poll.sequence;
}

// Miners registry.
// ----------------------------------------------------------------------------
// A connection that requests a template or submits a block is a channel of
// the registry, until it stops.

void protocol_bitcoind_mining::register_miner() NOEXCEPT
{
    BC_ASSERT(stranded());
    if (!miner_.empty())
    {
        mining()->set_difficulty(miner_, difficulty_);
        return;
    }

    miner_ = miners::to_id(identifier(), {});
    mining()->add(
    {
        .id = miner_,
        .service = service(),
        .transport = websocket() ? miners::to_transport(false, true,
            secure()) : (secure() ? "https" : "http"),
        .address = opposite().to_string(),
        .client = client(),
        .workers = username().empty() ? string_list{} :
            string_list{ username() },
        .connected = miners::now(),
        .difficulty = difficulty_
    });
}

// A submitted block is a share of the network difficulty.
void protocol_bitcoind_mining::record_submitted(const hash_digest& hash,
    submission outcome, const std::string& result) NOEXCEPT
{
    BC_ASSERT(stranded());
    register_miner();

    const auto accepted = outcome == submission::accepted;
    mining()->add_share(miner_,
    {
        miners::now(), height_, difficulty_, result
    }, accepted);

    if (accepted || outcome == submission::duplicate)
        mining()->add_block({ hash, height_, miners::now(), miner_ });
}

BC_POP_WARNING()
BC_POP_WARNING()
BC_POP_WARNING()

} // namespace server
} // namespace libbitcoin
