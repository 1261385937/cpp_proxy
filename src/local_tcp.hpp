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

#ifdef _WIN32
constexpr char unix_domian_ip[] = "D:\\cpp_proxy_uds";
#else
constexpr char unix_domian_ip[] = "/dev/shm/cpp_proxy_uds";
#endif
constexpr uint16_t local_port = 23456;

enum class data_type : uint8_t {
   from_client_to_server,
   from_server_to_client,
   four_tuple
};

enum class action : uint8_t {
   pass,
   block
};

struct response_head {
   data_type type;
   action act;
   uint8_t unused2 = 0;
   uint8_t unused3 = 0;
   uint32_t new_data_len = 0;
   uint64_t handled_total_pkg_len;
};

constexpr auto response_head_len = sizeof(local::response_head);

}  // namespace local
