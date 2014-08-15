/*
    Copyright (c) 2011-2014 Andrey Sibiryov <me@kobology.ru>
    Copyright (c) 2011-2014 Other contributors as noted in the AUTHORS file.

    This file is part of Cocaine.

    Cocaine is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    Cocaine is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef COCAINE_IO_ENCODER_HPP
#define COCAINE_IO_ENCODER_HPP

#include "cocaine/rpc/protocol.hpp"

#include "cocaine/traits.hpp"
#include "cocaine/traits/tuple.hpp"

#include <cstring>

namespace cocaine { namespace io {

struct encoder_t;

namespace aux {

struct encoded_buffers_t {
    friend struct encoded_message_t;

    static const size_t kInitialBufferSize = 4096;

    encoded_buffers_t():
        offset(0)
    {
        vector.resize(kInitialBufferSize);
    }

    void
    write(const char* data, size_t size) {
        while(size > vector.size() - offset) {
            vector.resize(vector.size() * 2);
        }

        std::memcpy(vector.data() + offset, data, size);

        offset += size;
    }

    // Movable

    encoded_buffers_t(encoded_buffers_t&&) = default;

    encoded_buffers_t&
    operator=(encoded_buffers_t&&) = default;

    COCAINE_DECLARE_NONCOPYABLE(encoded_buffers_t)

private:
    std::vector<char> vector;
    std::vector<char>::difference_type offset;
};

struct encoded_message_t {
    friend struct io::encoder_t;

    const char*
    data() const {
        return buffer.vector.data();
    }

    size_t
    size() const {
        return buffer.offset;
    }

protected:
    encoded_buffers_t buffer;
};

} // namespace aux

template<class Event>
struct encoded:
    public aux::encoded_message_t
{
    template<typename... Args>
    encoded(uint64_t span, Args&&... args) {
        typedef event_traits<Event> traits;

        msgpack::packer<aux::encoded_buffers_t> packer(buffer);

        packer.pack_array(3);

        packer.pack_uint64(span);
        packer.pack_uint32(traits::id);

        typedef typename traits::tuple_type tuple_type;

        type_traits<tuple_type>::pack(packer, std::forward<Args>(args)...);
    }
};

struct encoder_t {
    typedef aux::encoded_message_t message_type;
};

}} // namespace cocaine::io

#endif
