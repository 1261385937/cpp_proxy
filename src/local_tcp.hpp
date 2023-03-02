#pragma once

#define UDS
#ifdef UDS
#include "asio/local/stream_protocol.hpp"
#endif

#include "asio/as_tuple.hpp"
#include "asio/ip/tcp.hpp"

namespace local {
using default_token = asio::as_tuple_t<asio::use_awaitable_t<>>;

#ifdef UDS
using tcp_type = asio::local::stream_protocol;
#else
using tcp_type = asio::ip::tcp;
#endif

using tcp_acceptor = default_token::as_default_on_t<tcp_type::acceptor>;
using tcp_socket = default_token::as_default_on_t<tcp_type::socket>;
using tcp_endpoint = tcp_type::endpoint;

enum class direction : uint8_t {
    from_client_to_server,
    from_server_to_client
};

}  // namespace local
