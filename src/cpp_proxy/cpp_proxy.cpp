#include <algorithm>
#include <atomic>
#include <cstdio>

#include "asio/as_tuple.hpp"
#include "asio/co_spawn.hpp"
#include "asio/steady_timer.hpp"
#include "config.hpp"
#include "io_context_pool.hpp"
#include "session.hpp"

using asio::ip::tcp;
using default_token = asio::as_tuple_t<asio::use_awaitable_t<>>;
using tcp_acceptor = default_token::as_default_on_t<tcp::acceptor>;
using port_acceptor = std::unordered_map<uint16_t, std::shared_ptr<tcp_acceptor>>;

template <typename ExecutorPool>
inline asio::awaitable<void> listener(uint16_t listen_port, port_acceptor& acceptors,
                                      ExecutorPool&& pool) {
   auto executor = co_await asio::this_coro::executor;
   auto acceptor =
       std::make_shared<tcp_acceptor>(executor, asio::ip::tcp::endpoint{tcp::v6(), listen_port});
   acceptor->set_option(tcp::acceptor::reuse_address(true));
   acceptors.emplace(listen_port, acceptor);

   for (;;) {
      tcp::socket client_proxy_socket(pool.get_executor());
      auto [ec] = co_await acceptor->async_accept(client_proxy_socket);
      if (ec) {
         if (!acceptor->is_open()) {
            co_return;
         }
      }
      std::make_shared<cpp_proxy::session>(std::move(client_proxy_socket))->start();
   }
}

template <typename ExecutorPool>
inline asio::awaitable<void> handle_config_change(std::string_view config_path,
                                                  port_acceptor& acceptors, ExecutorPool&& pool) {
   using default_token = asio::as_tuple_t<asio::use_awaitable_t<>>;
   using frequency_timer = default_token::as_default_on_t<asio::steady_timer>;
   auto executor = co_await asio::this_coro::executor;
   frequency_timer timer{executor};
   auto last_time = std::filesystem::last_write_time(config_path);

   for (;;) {
      timer.expires_from_now(std::chrono::seconds(3));
      auto [ec] = co_await timer.async_wait();
      if (ec) {
         LOG_ERROR("handle_config_change exception:{}", ec.message());
         co_return;
      }
      auto now_time = std::filesystem::last_write_time(config_path);
      if (last_time == now_time) {
         continue;
      }

      LOG_WARN("cpp_proxy config changed");
      last_time = now_time;
      cpp_proxy::config::instance().parse_conf(config_path);

      auto& add = cpp_proxy::config::instance().get_add_proxy_entities();
      for (auto& a : add) {
         LOG_WARN("add listen_port:{}", a.listen_port);
         asio::co_spawn(executor, listener(a.listen_port, acceptors, pool), asio::detached);
      }
      auto& del = cpp_proxy::config::instance().get_del_proxy_entities();
      for (auto& d : del) {
         if (auto it = acceptors.find(d.listen_port); it != acceptors.end()) {
            LOG_WARN("del listen_port:{}", d.listen_port);
            asio::error_code _;
            it->second->close(_);
            acceptors.erase(it);
         }
      }
   }
}

int main(int, char* argv[]) {
   std::string config_path = argv[1];
   cpp_proxy::config::instance().parse_conf(config_path);

   auto& [dir, name, size, count] = cpp_proxy::config::instance().get_log_info();
   easylog::easylog_options opt{};
   opt.log_dir = std::move(dir);
   opt.log_name = std::move(name);
   opt.max_size = size;
   opt.max_files = count;
   easylog::setup_logger(opt);
   easylog::set_logger_module_level("LOG", easylog::level::info);
   LOG_WARN("cpp_proxy start");

   try {
      io_context_pool icp(std::thread::hardware_concurrency());
      icp.start();

      asio::io_context io_context;
      asio::signal_set signals(io_context, SIGINT, SIGTERM);
      signals.add(SIGSEGV);
      signals.add(SIGABRT);
      signals.add(SIGILL);
      signals.add(SIGFPE);
      signals.async_wait([&](auto, auto) { io_context.stop(); });

      port_acceptor acceptors;
      auto& proxy_entities = cpp_proxy::config::instance().get_proxy_entities();
      for (auto& proxy : proxy_entities) {
         asio::co_spawn(io_context, listener(proxy.listen_port, acceptors, icp), asio::detached);
      }

      asio::co_spawn(io_context.get_executor(), handle_config_change(config_path, acceptors, icp),
                     asio::detached);
      io_context.run();
   } catch (std::exception& e) {
      LOG_ERROR("io_context exit, exception:{}", e.what());
   }

   LOG_WARN("cpp_proxy exit");
   return 0;
}
