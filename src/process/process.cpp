
#include "asio/as_tuple.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/read.hpp"
#include "asio/signal_set.hpp"
#include "asio/write.hpp"

#include "io_context_pool.hpp"
#include "local_tcp.hpp"
#include "nlohmann/json.hpp"

class session : public std::enable_shared_from_this<session> {
private:
   local::tcp_socket socket_;
   std::string buf_;
   bool has_closed_ = false;
   bool eof_ = false;

   uint64_t req_handled_total_pkg_len_ = 0;
   uint64_t res_handled_total_pkg_len_ = 0;

public:
   session(local::tcp_socket sock) : socket_(std::move(sock)) {
   }

   void start() {
      asio::co_spawn(
          socket_.get_executor(), [self = shared_from_this()] { return self->setup(); },
          asio::detached);
   }

private:
   void grace_close() {
      if (!has_closed_) {
         has_closed_ = true;
         socket_.shutdown(local::tcp_socket::shutdown_both);
         socket_.close();
         handle_end();
      }
   }

   void handle_init(std::string_view data) {
      auto j = nlohmann::json::parse(data);
      std::string client_ip = j["client_ip"];
      uint16_t client_port = j["client_port"];
      std::string server_ip = j["server_ip"];
      uint16_t server_port = j["server_port"];
   }

   void handle_request(std::string_view data) {
   }

   void handle_response(std::string_view data) {
   }

   void handle_end() {
   }

   asio::awaitable<void> setup() {
      for (;;) {
         if (eof_) [[unlikely]] {
            LOG_ERROR("remote peer close the connection");
            grace_close();
            co_return;
         }
         // Read data from proxy

         // Read head 1byte + 4byte
         char head[5];
         auto [rec, r_] = co_await asio::async_read(socket_, asio::buffer(head, 5));
         if (rec) [[unlikely]] {
            if (rec != asio::error::eof) {
               LOG_ERROR("async_read head from proxy error: {}", rec.message());
               grace_close();
               co_return;
            }
            eof_ = true;
         }

         auto type = (local::data_type)(head[0]);
         auto body_len = *(uint32_t*)(head + 1);
         if (body_len > buf_.size()) [[unlikely]] {
            buf_.resize(body_len);
         }
         //LOG_INFO("head:{} body_len: {}", type, body_len);

         // body_len is 0 when response_bypass_ is true by default
         if (body_len != 0) {
            auto [ec, _] = co_await asio::async_read(socket_, asio::buffer(buf_.data(), body_len));
            if (ec) [[unlikely]] {
               if (ec != asio::error::eof) {
                  LOG_ERROR("async_read body from proxy error: {}", ec.message());
                  grace_close();
                  co_return;
               }
               eof_ = true;
            }
         }

         switch (type) {
            using enum local::data_type;
            using enum local::action;
            case from_client_to_server: {
               req_handled_total_pkg_len_ += body_len;
               handle_request({buf_.data(), body_len});
               local::response_head req_head{};
               req_head.type = from_client_to_server;
               req_head.act = pass;
               req_head.handled_total_pkg_len = req_handled_total_pkg_len_;

               std::vector<asio::const_buffer> write_buffers;
               write_buffers.emplace_back(asio::buffer(&req_head, sizeof(req_head)));
               // write_buffers.emplace_back(asio::buffer(buf_.data(), body_len));

               auto [wec, w_] = co_await asio::async_write(socket_, write_buffers);
               if (wec) [[unlikely]] {
                  LOG_ERROR("async_write error: {}", wec.message());
                  grace_close();
                  co_return;
               }
            } break;
            case from_server_to_client: {
               res_handled_total_pkg_len_ += body_len;
               handle_response({buf_.data(), body_len});
               if (body_len == 0) {
                  // handle_response will return no data, just reset inner flag
                  break;
               }

               local::response_head res_head{};
               res_head.type = from_server_to_client;
               res_head.act = pass;
               res_head.handled_total_pkg_len = res_handled_total_pkg_len_;

               std::vector<asio::const_buffer> write_buffers;
               write_buffers.emplace_back(asio::buffer(&res_head, sizeof(res_head)));
               // write_buffers.emplace_back(asio::buffer(buf_.data(), body_len));

               auto [wec, w_] = co_await asio::async_write(socket_, write_buffers);
               if (wec) [[unlikely]] {
                  LOG_ERROR("async_write error: {}", wec.message());
                  grace_close();
                  co_return;
               }
            } break;
            case four_tuple: {
               handle_init({buf_.data(), body_len});
            } break;
            default:
               break;
         }
      }
   }
};

template <typename ExecutorPool>
asio::awaitable<void> listener(ExecutorPool&& pool) {
   auto executor = co_await asio::this_coro::executor;
   using namespace local;
#ifdef UDS
   local::tcp_acceptor acceptor(executor, tcp_endpoint(local::unix_domian_ip), false);
#else
   local::tcp_acceptor acceptor(executor, tcp_endpoint(tcp_type::v6(), local::local_port));
#endif

   for (;;) {
      local::tcp_socket socket(pool.get_executor());
      auto [ec] = co_await acceptor.async_accept(socket);
      acceptor.set_option(tcp_type::acceptor::reuse_address(true));
      if (ec) {
         continue;
      }
      std::make_shared<session>(std::move(socket))->start();
   }
}

int main() {
   easylog::easylog_options opt{};
   opt.log_dir = "./";
   opt.log_name = "process";
   opt.max_size = 10485760;
   opt.max_files = 10;
   easylog::setup_logger(opt);
   easylog::set_logger_module_level("LOG", easylog::level::info);
   LOG_WARN("process start");

   try {
      io_context_pool icp(std::thread::hardware_concurrency());
      icp.start();

      asio::io_context io_context;
      asio::signal_set signals(io_context, SIGINT, SIGTERM);
      signals.add(SIGSEGV);
      signals.add(SIGABRT);
      signals.add(SIGILL);
      signals.add(SIGFPE);
      signals.async_wait([&](auto, auto sig) {
         SPDLOG_ERROR("receive signal: {}, process exit", sig);
         io_context.stop();
      });

      std::remove(local::unix_domian_ip);
      asio::co_spawn(io_context, listener(icp), asio::detached);
      io_context.run();
   } catch (std::exception& e) {
      LOG_ERROR("io_context exit, exception:{}", e.what());
   }

   LOG_WARN("process exit");
   return 0;
}