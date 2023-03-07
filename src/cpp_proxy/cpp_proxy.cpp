#include <algorithm>
#include <atomic>
#include <cstdio>
#include "config.hpp"
#include "io_context_pool.hpp"
#include "session.hpp"

template <typename ExecutorPool>
asio::awaitable<void> listener(uint16_t listen_port, ExecutorPool&& pool) {
   using asio::ip::tcp;
   using default_token = asio::as_tuple_t<asio::use_awaitable_t<>>;
   using tcp_acceptor = default_token::as_default_on_t<tcp::acceptor>;

   auto executor = co_await asio::this_coro::executor;
   tcp_acceptor acceptor(executor, {tcp::v6(), listen_port});

   for (;;) {
      tcp::socket proxy_server_socket(pool.get_executor());
      auto [ec] = co_await acceptor.async_accept(proxy_server_socket);
      if (ec) {
         continue;
      }
      std::make_shared<cpp_proxy::session>(std::move(proxy_server_socket))->start();
   }
}

int main() {
   cpp_proxy::config::instance().parse_conf();
   auto [dir, name, size, count] = cpp_proxy::config::instance().get_log_info();
   auto& proxy_info = cpp_proxy::config::instance().get_proxy_info();

   easylog::easylog_options opt{};
   opt.log_dir = std::move(dir);
   opt.log_name = std::move(name);
   opt.max_size = size;
   opt.max_files = count;
   easylog::setup_logger(opt);
   easylog::set_logger_module_level("LOG", easylog::level::info);
   LOG_WARN("cpp_proxy start");

   try {
      io_context_pool icp(1);
      icp.start();

      asio::io_context io_context;
      asio::signal_set signals(io_context, SIGINT, SIGTERM);
      signals.add(SIGSEGV);
      signals.add(SIGABRT);
      signals.add(SIGILL);
      signals.add(SIGFPE);
      signals.async_wait([&](auto, auto) { io_context.stop(); });

      for (auto& info : proxy_info) {
         asio::co_spawn(io_context, listener(info.first, icp), asio::detached);
      }

      io_context.run();
   } catch (std::exception& e) {
      LOG_ERROR("io_context exit, exception:{}", e.what());
   }

   LOG_WARN("cpp_proxy exit");
   return 0;
}
