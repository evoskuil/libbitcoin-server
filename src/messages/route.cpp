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
#include <bitcoin/server/messages/route.hpp>

#include <utility>

namespace libbitcoin {
namespace server {

//////route::route(zmq::message)
//////{
//////}

route::route(bool secure, bool delimited)
  : secure_(secure), delimited_(delimited)
{
}

bool route::secure()
{
    return secure_;
}

bool route::delimited()
{
    return delimited_;
}

void route::enqueue(data_chunk&& identity)
{
    identities_.emplace(identity);
}

bool route::dequeue(data_chunk& identity)
{
    if (identities_.empty())
        return false;

    identities_.front().swap(identity);
    identities_.pop();
    return true;
}

////zmq::message route::message()
////{
////}

} // namespace server
} // namespace libbitcoin
