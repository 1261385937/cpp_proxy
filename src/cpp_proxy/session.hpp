#pragma once
#include <deque>
#include <memory>

#include "asio/as_tuple.hpp"
#include "asio/co_spawn.hpp"
#include "asio/connect.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/ip/tcp.hpp"
#include "asio/read.hpp"
#include "asio/signal_set.hpp"
#include "asio/steady_timer.hpp"
#include "asio/write.hpp"

#include "config.hpp"
#include "data_block.hpp"
#include "easylog.hpp"
#include "local_tcp.hpp"

namespace cpp_proxy {

using asio::ip::tcp;

class session : public std::enable_shared_from_this<session> {
public:
   using default_token = asio::as_tuple_t<asio::use_awaitable_t<>>;
   using tcp_socket = default_token::as_default_on_t<tcp::socket>;
   using tcp_resolver = default_token::as_default_on_t<tcp::resolver>;

private:
   tcp_socket proxy_client_socket_;
   tcp_socket proxy_server_socket_;
   local::tcp_socket proxy_process_socket_;
   bool has_closed_ = false;
   inline static std::atomic<size_t> count_ = 0;
   std::vector<cpp_proxy::server_info>::iterator server_it_;

   bool eof_ = false;
   bool reconnecting_ = false;
   bool bypass_ = true;

   std::string proxy_client_buf_;
   std::string proxy_server_buf_;

   struct packet {
      char* buf;
      uint32_t len;
      std::shared_ptr<void> buf_keeper;
   };
   std::deque<packet> data_from_client_to_server_;
   std::deque<packet> data_from_server_to_client_;
   uint64_t req_handled_total_pkg_len_ = 0;
   uint64_t res_handled_total_pkg_len_ = 0;

   std::string server_ip_;
   uint16_t server_port_{};
   std::string client_ip_;
   uint16_t client_port_{};
   std::string proxy_ip_;
   uint16_t proxy_port_{};

public:
   session(tcp_socket client_2proxy_socket)
       : proxy_client_socket_(std::move(client_2proxy_socket)),
         proxy_server_socket_(proxy_client_socket_.get_executor()),
         proxy_process_socket_(proxy_client_socket_.get_executor()) {
      proxy_client_buf_.resize(cpp_proxy::config::instance().get_connection_buf_size(), '\0');
      proxy_server_buf_.resize(cpp_proxy::config::instance().get_connection_buf_size(), '\0');
      LOG_INFO("new session, now:{}", ++count_);
   }

   ~session() {
      LOG_INFO("session gone, now:{}. client: {}:{} --- proxy: {}:{} --- server: {}:{}", --count_,
               client_ip_, client_port_, proxy_ip_, proxy_port_, server_ip_, server_port_);
      (*(server_it_->conns_))--;
      grace_close();
   }

   void start() {
      asio::co_spawn(
          proxy_client_socket_.get_executor(),
          [self = shared_from_this()] { return self->setup_proxy(); }, asio::detached);
   }

private:
   void reset_local_socket() {
      asio::error_code ignored_ec;
      proxy_process_socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignored_ec);
      proxy_process_socket_.close(ignored_ec);
      proxy_process_socket_ = local::tcp_socket{proxy_client_socket_.get_executor()};
   }

   void grace_close() {
      if (!has_closed_) {
         has_closed_ = true;
         asio::error_code ignored_ec;
         proxy_client_socket_.shutdown(tcp_socket::shutdown_both, ignored_ec);
         proxy_client_socket_.close(ignored_ec);
         proxy_server_socket_.shutdown(tcp_socket::shutdown_both, ignored_ec);
         proxy_server_socket_.close(ignored_ec);
         proxy_process_socket_.shutdown(local::tcp_socket::shutdown_both, ignored_ec);
         proxy_process_socket_.close(ignored_ec);
      }
   }

   asio::awaitable<void> setup_proxy() {
      auto listen_port = proxy_client_socket_.local_endpoint().port();
      auto it = cpp_proxy::config::instance().get_proxy_info().find(listen_port);
      auto& servers = it->second;
      server_it_ = std::min_element(servers.begin(), servers.end());
      server_ip_ = server_it_->ip;
      server_port_ = server_it_->port;

      auto executor = co_await asio::this_coro::executor;
      auto [ec_r, ep] =
          co_await tcp_resolver(executor).async_resolve(server_ip_, std::to_string(server_port_));
      if (ec_r) {
         LOG_ERROR("tcp_resolver exception: {}", ec_r.message());
         co_return;
      }

      auto [ec_c, _] = co_await asio::async_connect(proxy_server_socket_, ep);
      if (ec_c) {
         LOG_ERROR("async_connect exception:{}, ip: {}, port: {}", ec_c.message(), server_ip_,
                   server_port_);
         co_return;
      }
      (*(server_it_->conns_))++;

      client_port_ = proxy_client_socket_.remote_endpoint().port();
      client_ip_ = proxy_client_socket_.remote_endpoint().address().to_string();
      proxy_ip_ = proxy_client_socket_.local_endpoint().address().to_string();
      proxy_port_ = proxy_client_socket_.local_endpoint().port();
      LOG_INFO("client: {}:{} --- proxy: {}:{} --- server: {}:{}", client_ip_, client_port_,
               proxy_ip_, proxy_port_, server_ip_, server_port_);

      asio::co_spawn(
          executor, [sf = shared_from_this()] { return sf->connect_process(); }, asio::detached);

      asio::co_spawn(
          executor, [sf = shared_from_this()] { return sf->client_request(); }, asio::detached);

      asio::co_spawn(
          executor, [sf = shared_from_this()] { return sf->server_response(); }, asio::detached);
   }

   std::string make_four_tuple() {
      nlohmann::json j;
      j["client_ip"] = client_ip_;
      j["client_port"] = client_port_;
      j["server_ip"] = server_ip_;
      j["server_port"] = server_port_;
      return j.dump();
   }

   asio::awaitable<void> connect_process() {
      reset_local_socket();
      req_handled_total_pkg_len_ = 0;
      res_handled_total_pkg_len_ = 0;

      auto executor = co_await asio::this_coro::executor;
      using frequency_timer = default_token::as_default_on_t<asio::steady_timer>;
      frequency_timer timer(executor);
#ifndef UDS
      local::tcp_endpoint endpoint(asio::ip::address{}.from_string("127.0.0.1"), local::local_port);
#else
      local::tcp_endpoint endpoint(local::unix_domian_ip)
#endif

      for (; !eof_;) {
         auto [ec] = co_await proxy_process_socket_.async_connect(endpoint);
         if (!ec) {
            LOG_INFO("connect_process ok");
            reconnecting_ = false;

            auto str = make_four_tuple();
            uint32_t len = (uint32_t)str.length();
            auto type = local::data_type::four_tuple;
            std::vector<asio::const_buffer> write_buffers;
            write_buffers.emplace_back(asio::buffer(&type, sizeof(type)));
            write_buffers.emplace_back(asio::buffer(&len, 4));
            write_buffers.emplace_back(asio::buffer(str.data(), str.length()));
            co_await asio::async_write(proxy_process_socket_, write_buffers);

            asio::co_spawn(
                executor,
                [self = shared_from_this()] { return self->process_2proxy_2client_or_2server(); },
                asio::detached);
            co_return;
         }
         LOG_ERROR("connect_process error: {}", ec.message());
         timer.expires_from_now(std::chrono::seconds(3));
         co_await timer.async_wait();
      }
   }

   auto prepare_transfer_packet(local::response_head* head, const std::deque<packet>& stored_data,
                                uint64_t& handled_total_pkg_len, int& handled_pkg_count,
                                int& next_pkg_used_size) {
      std::vector<asio::const_buffer> write_buffers;
      write_buffers.reserve(stored_data.size());

      for (size_t i = 0; i < stored_data.size(); ++i) {
         auto& pack = stored_data[i];
         if (handled_total_pkg_len + pack.len > head->handled_total_pkg_len) [[unlikely]] {
            next_pkg_used_size = (int)(head->handled_total_pkg_len - handled_total_pkg_len);
            if (next_pkg_used_size == 0) {
               break;
            }
            write_buffers.emplace_back(asio::buffer(pack.buf, next_pkg_used_size));  // data
            break;
         }
         write_buffers.emplace_back(asio::buffer(pack.buf, pack.len));
         handled_total_pkg_len += pack.len;
         handled_pkg_count++;
      }
      return write_buffers;
   }

   void clear_handled_data(std::deque<packet>& stored_data, int handled_pkg_count,
                           int next_pkg_used_size) {
      for (int i = 0; i < handled_pkg_count; i++) {
         stored_data.pop_front();
      }

      if (next_pkg_used_size == 0) {
         return;
      }
      auto& pkg = stored_data.front();
      pkg.buf = pkg.buf + next_pkg_used_size;
      pkg.len -= next_pkg_used_size;
   }

   asio::awaitable<int> proxy_2client(local::response_head* head,
                                      std::string_view proxy_process_buf) {
      // Process make a new data (rewrite or create), transfer the new data
      if (head->new_data_len != 0) {
         auto [ec, _] = co_await asio::async_write(
             proxy_client_socket_, asio::buffer(proxy_process_buf.data(), head->new_data_len));
         if (ec) {
            LOG_ERROR("proxy async_write to client error: {}", ec.message());
            grace_close();
            co_return -1;
         }
      }
      // Get block action
      if (head->act == local::action::block) {
         LOG_WARN("get block action, close all connections");
         grace_close();
         co_return -1;
      }

      // Get pass action
      //
      // Calculate the handled packet count and the tail in next packet
      int handled_pkg_count = 0;
      int next_pkg_used_size = 0;
      auto write_buffers =
          prepare_transfer_packet(head, data_from_server_to_client_, res_handled_total_pkg_len_,
                                  handled_pkg_count, next_pkg_used_size);

      // Transfer the handled data
      auto [wec, w_] = co_await asio::async_write(proxy_client_socket_, write_buffers);
      if (wec) [[unlikely]] {
         LOG_ERROR("proxy async_write to client error: {}", wec.message());
         grace_close();
         co_return -1;
      }

      // clear the handled data
      clear_handled_data(data_from_server_to_client_, handled_pkg_count, next_pkg_used_size);
      co_return 0;
   }

   asio::awaitable<int> proxy_2server(local::response_head* head,
                                      std::string_view proxy_process_buf) {
      // Process make a new data (rewrite or create), transfer the new data
      if (head->new_data_len != 0) {
         auto [ec, _] = co_await asio::async_write(
             proxy_server_socket_, asio::buffer(proxy_process_buf.data(), head->new_data_len));
         if (ec) [[unlikely]] {
            LOG_ERROR("proxy async_write to server error: {}", ec.message());
            grace_close();
            co_return -1;
         }
      }
      // Get block action
      if (head->act == local::action::block) {
         LOG_WARN("get block action, close all connections");
         grace_close();
         co_return -1;
      }

      // Get pass action
      //
      // Calculate the handled packet count and the tail in next packet
      int handled_pkg_count = 0;
      int next_pkg_used_size = 0;
      auto write_buffers =
          prepare_transfer_packet(head, data_from_client_to_server_, req_handled_total_pkg_len_,
                                  handled_pkg_count, next_pkg_used_size);

      // Transfer the handled data
      auto [wec, w_] = co_await asio::async_write(proxy_server_socket_, write_buffers);
      if (wec) [[unlikely]] {
         LOG_ERROR("proxy async_write to server error: {}", wec.message());
         grace_close();
         co_return -1;
      }

      // clear the handled data
      clear_handled_data(data_from_client_to_server_, handled_pkg_count, next_pkg_used_size);
      co_return 0;
   }

   asio::awaitable<int> enable_bypass() {
      bypass_ = true;
      auto ret = co_await transfer_client_data_and_reconnect();
      if (ret == -1) [[unlikely]] {
         co_return -1;
      }
      ret = co_await transfer_server_data_and_reconnect();
      if (ret == -1) [[unlikely]] {
         co_return -1;
      }
      co_return 0;
   }

   asio::awaitable<void> process_2proxy_2client_or_2server() {
      // Connect process ok, then disable bypass
      bypass_ = false;
      std::string proxy_process_buf;

      // Proxy read data from process
      for (;;) {
         //**Read head
         char head_buf[local::response_head_len];
         auto [rec, r_] = co_await asio::async_read(
             proxy_process_socket_, asio::buffer(head_buf, local::response_head_len));
         if (rec) [[unlikely]] {
            if (eof_) {
               co_return;
            }
            LOG_ERROR("enable bypass, async_read head from process error: {}", rec.message());
            enable_bypass();
            co_return;
         }
         auto head = (local::response_head*)head_buf;
         //LOG_INFO("head:{} body_len: {}", head->type, head->new_data_len);

         //**Read body if has
         auto body_len = head->new_data_len;
         if (body_len != 0) [[unlikely]] {
            if (body_len > proxy_process_buf.size()) {
               proxy_process_buf.resize(body_len);
            }
            auto [ec, _] = co_await asio::async_read(
                proxy_process_socket_, asio::buffer(proxy_process_buf.data(), body_len));
            if (ec) [[unlikely]] {
               if (eof_) {
                  co_return;
               }
               LOG_ERROR("enable bypass, async_read body from process error: {}", ec.message());
               enable_bypass();
               co_return;
            }
         }

         switch (head->type) {
            using enum local::data_type;
            case from_client_to_server: {
               auto ret = co_await proxy_2server(head, proxy_process_buf);
               if (ret == -1) [[unlikely]] {
                  co_return;
               }
            } break;
            case from_server_to_client: {
               auto ret = co_await proxy_2client(head, proxy_process_buf);
               if (ret == -1) [[unlikely]] {
                  co_return;
               }
            } break;
            default:
               break;
         }
      }
   }

   asio::awaitable<int> client_2proxy_2server() {
      // Proxy read data from client
      auto [rec, n] =
          co_await proxy_client_socket_.async_read_some(asio::buffer(proxy_client_buf_));
      if (rec) [[unlikely]] {
         if (rec != asio::error::eof) {
            LOG_ERROR("async_read_some from client error: {}", rec.message());
            grace_close();
            co_return -1;
         }
         // Connection was closed by the remote peer if error is eof
         // Still the buffer holds "n" bytes of the received data
         eof_ = true;
         if (n == 0) {
            co_return 0;
         }
      }
      // Proxy write data to server
      auto [wec, _] =
          co_await asio::async_write(proxy_server_socket_, asio::buffer(proxy_client_buf_, n));
      if (wec) [[unlikely]] {
         LOG_ERROR("async_write to server error: {}", wec.message());
         grace_close();
         co_return -1;
      }
      co_return 0;
   }

   asio::awaitable<int> client_2proxy_2process(std::shared_ptr<data_block>& block) {
      // Prepare data_block if available size < 64+5 byte, head_len is 5
      // For head(1 + 4) byte
      if (!block || block->get_available() < 69) {
         block = std::make_shared<data_block>(default_block_size);
      }
      auto data_pos = block->get_current_write_pos();
      auto available = block->get_available();

      // Proxy read data from client
      auto [rec, n] =
          co_await proxy_client_socket_.async_read_some(asio::buffer(data_pos + 5, available - 5));
      if (rec) [[unlikely]] {
         if (rec != asio::error::eof) {
            LOG_ERROR("async_read_some from client error: {}", rec.message());
            grace_close();
            co_return -1;
         }
         eof_ = true;
         if (n == 0) {
            co_return 0;
         }
      }
      // Store the packet from client for bypass if need
      block->update_available((uint32_t)n + 5);
      data_from_client_to_server_.emplace_back(packet{data_pos + 5, (uint32_t)n, block});

      // Proxy write data to process
      *(uint8_t*)data_pos = (uint8_t)local::data_type::from_client_to_server;
      *(uint32_t*)(data_pos + 1) = static_cast<uint32_t>(n);
      auto [ec, _] =
          co_await asio::async_write(proxy_process_socket_, asio::buffer(data_pos, n + 5));
      if (ec) [[unlikely]] {
         LOG_ERROR("async_write to process error: {}", ec.message());
         co_return -2;
      }
      co_return 0;
   }

   asio::awaitable<int> transfer_client_data_and_reconnect() {
      // Transfer the all data to server in data_from_client_to_server_
      if (data_from_client_to_server_.empty()) {
         co_return 0;
      }

      std::vector<asio::const_buffer> write_buffers;
      write_buffers.reserve(data_from_client_to_server_.size());
      for (size_t i = 0; i < data_from_client_to_server_.size(); ++i) {
         auto& pack = data_from_client_to_server_[i];
         write_buffers.emplace_back(asio::buffer(pack.buf, pack.len));
      }
      auto [wec, w_] = co_await asio::async_write(proxy_server_socket_, write_buffers);
      if (wec) [[unlikely]] {
         LOG_ERROR("async_write to server error: {}", wec.message());
         grace_close();
         co_return -1;
      }
      data_from_client_to_server_.clear();

      // Try to reconnect process, then disable bypass if reconnect ok
      if (!reconnecting_) {
         reconnecting_ = true;
         LOG_ERROR("try to reconnect process");
         auto executor = co_await asio::this_coro::executor;
         asio::co_spawn(
             executor, [self = shared_from_this()] { return self->connect_process(); },
             asio::detached);
      }
      co_return 0;
   }

   asio::awaitable<void> client_request() {
      std::shared_ptr<data_block> block = nullptr;
      for (;;) {
         if (eof_) [[unlikely]] {
            LOG_INFO("remote peer (client) close the connection");
            grace_close();
            co_return;
         }

         // ***Here bypass, data from client to server
         if (bypass_) [[unlikely]] {
            auto ret = co_await client_2proxy_2server();
            if (ret == 0) [[likely]] {
               continue;
            }
            co_return;
         }

         // ***Here, data from client to process
         auto ret = co_await client_2proxy_2process(block);
         if (ret == 0) [[likely]] {
            continue;
         }
         if (ret == -1) {
            co_return;
         }

         // ret is -2
         LOG_WARN("process error, enable bypass");
         ret = co_await enable_bypass();
         if (ret == 0) [[likely]] {
            continue;
         }
         co_return;
      }
   }

   asio::awaitable<int> server_2proxy_2client() {
      // proxy read data from server
      auto [rec, n] =
          co_await proxy_server_socket_.async_read_some(asio::buffer(proxy_server_buf_));
      if (rec) [[unlikely]] {
         if (rec != asio::error::eof) {
            LOG_ERROR("async_read_some from server error: {}", rec.message());
            grace_close();
            co_return -1;
         }
         eof_ = true;
         if (n == 0) {
            co_return 0;
         }
      }
      // proxy write data to client
      auto [wec, _] =
          co_await asio::async_write(proxy_client_socket_, asio::buffer(proxy_server_buf_, n));
      if (wec) [[unlikely]] {
         LOG_ERROR("async_write to client error: {}", wec.message());
         grace_close();
         co_return -1;
      }
      co_return 0;
   }

   asio::awaitable<int> server_2proxy_2process(std::shared_ptr<data_block>& block) {
      // Prepare data_block if available size < 64+5 byte, head_len is 5
      if (!block || block->get_available() < 69) {
         block = std::make_shared<data_block>(default_block_size);
      }
      auto data_pos = block->get_current_write_pos();
      auto available = block->get_available();

      // Proxy read data from server, (data_pos + 5) for head(1 + 4) byte
      auto [rec, n] =
          co_await proxy_server_socket_.async_read_some(asio::buffer(data_pos + 5, available - 5));
      if (rec) [[unlikely]] {
         if (rec != asio::error::eof) {
            LOG_ERROR("proxy async_read_some from server: {}", rec.message());
            grace_close();
            co_return -1;
         }
         eof_ = true;
         if (n == 0) {
            co_return 0;
         }
      }
      // Store the packet from server for bypass if need
      block->update_available((uint32_t)n + 5);
      data_from_server_to_client_.emplace_back(packet{data_pos + 5, (uint32_t)n, block});

      // Proxy write data to process
      *(uint8_t*)data_pos = (uint8_t)local::data_type::from_server_to_client;
      *(uint32_t*)(data_pos + 1) = static_cast<uint32_t>(n);
      auto [ec, _] =
          co_await asio::async_write(proxy_process_socket_, asio::buffer(data_pos, n + 5));
      if (ec) [[unlikely]] {
         LOG_ERROR("async_write to process error: {}", ec.message());
         co_return -2;
      }
      co_return 0;
   }

   asio::awaitable<int> transfer_server_data_and_reconnect() {
      // Transfer the all data to client in data_from_server_to_client_
      if (data_from_server_to_client_.empty()) {
         co_return 0;
      }

      std::vector<asio::const_buffer> write_buffers;
      write_buffers.reserve(data_from_server_to_client_.size());
      for (size_t i = 0; i < data_from_server_to_client_.size(); ++i) {
         auto& pack = data_from_server_to_client_.front();
         data_from_server_to_client_.pop_front();
         write_buffers.emplace_back(asio::buffer(pack.buf, pack.len));
      }
      auto [wec, w_] = co_await asio::async_write(proxy_client_socket_, write_buffers);
      if (wec) [[unlikely]] {
         LOG_ERROR("async_write to client error: {}", wec.message());
         grace_close();
         co_return -1;
      }

      // Try to reconnect process, then disable bypass if reconnect ok
      if (!reconnecting_) {
         reconnecting_ = true;
         LOG_ERROR("try to reconnect process");
         auto executor = co_await asio::this_coro::executor;
         asio::co_spawn(
             executor, [self = shared_from_this()] { return self->connect_process(); },
             asio::detached);
      }
      co_return 0;
   }

   asio::awaitable<void> server_response() {
      std::shared_ptr<data_block> block = nullptr;
      for (;;) {
         if (eof_) [[unlikely]] {
            LOG_INFO("remote peer (server) close the connection");
            grace_close();
            co_return;
         }

         // ***Here bypass, data from server to client
         if (bypass_) [[unlikely]] {
            auto ret = co_await server_2proxy_2client();
            if (ret == 0) [[likely]] {
               continue;
            }
            co_return;
         }

         // ***Here, data from server to process
         auto ret = co_await server_2proxy_2process(block);
         if (ret == 0) [[likely]] {
            continue;
         }
         if (ret == -1) {
            co_return;
         }

         // ret is -2
         LOG_WARN("process error, enable bypass");
         ret = co_await enable_bypass();
         if (ret == 0) [[likely]] {
            continue;
         }
         co_return;
      }
   }
};
}  // namespace cpp_proxy