#pragma once
#include <deque>
#include <memory>

#include "asio/as_tuple.hpp"
#include "asio/co_spawn.hpp"
#include "asio/connect.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/ip/tcp.hpp"
#include "asio/signal_set.hpp"
#include "asio/steady_timer.hpp"
#include "asio/write.hpp"

#include "config.hpp"
#include "data_block.hpp"
#include "easylog.hpp"
#include "local_tcp.hpp"

#ifdef _WIN32
constexpr char unix_domian_ip[] = "./cpp_proxy_uds";
#else
constexpr char unix_domian_ip[] = "/dev/shm/cpp_proxy_uds";
#endif
constexpr uint16_t local_port = 23456;

namespace cpp_proxy {

using asio::ip::tcp;

class session : public std::enable_shared_from_this<session> {
public:
    using default_token = asio::as_tuple_t<asio::use_awaitable_t<>>;
    using tcp_acceptor = default_token::as_default_on_t<tcp::acceptor>;
    using tcp_socket = default_token::as_default_on_t<tcp::socket>;
    using tcp_resolver = default_token::as_default_on_t<tcp::resolver>;

private:
    tcp_socket proxy_client_socket_;
    tcp_socket proxy_server_socket_;
    local::tcp_socket proxy_process_socket_;
    inline static size_t count_ = 0;
    std::vector<cpp_proxy::server_info>::iterator server_it_;

    bool reconnecting_ = false;
    bool bypass_ = true;
    bool has_closed_ = false;
    bool eof_ = false;
    std::string client_proxy_buf_;
    std::string proxy_server_buf_;

    struct packet {
        char* buf;
        size_t len;
        std::shared_ptr<void> buf_keeper;
    };
    std::deque<packet> data_from_client_to_server_;
    std::deque<packet> data_from_server_to_client_;

    std::string server_ip_;
    uint16_t server_port_;
    std::string client_ip_;
    uint16_t client_port_;
    std::string proxy_ip_;
    uint16_t proxy_port_;

public:
    session(tcp_socket client_2proxy_socket)
        : proxy_client_socket_(std::move(client_2proxy_socket)),
          proxy_server_socket_(proxy_client_socket_.get_executor()),
          proxy_process_socket_(proxy_client_socket_.get_executor()) {
        client_proxy_buf_.resize(cpp_proxy::config::instance().get_connection_buf_size(), '\0');
        proxy_server_buf_.resize(cpp_proxy::config::instance().get_connection_buf_size(), '\0');
        LOG_INFO("new session, now:{}", ++count_);
    }

    ~session() {
        LOG_INFO("session gone, now:{}", --count_);
        (*(server_it_->conns_))--;
        grace_close_all();
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

    void grace_close_all() {
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
            executor, [self = shared_from_this()] { return self->client_2proxy_2process(); },
            asio::detached);

        asio::co_spawn(
            executor, [self = shared_from_this()] { return self->server_2proxy_2process(); },
            asio::detached);
    }

    asio::awaitable<void> reconnect_process() {
        auto executor = co_await asio::this_coro::executor;
        using frequency_timer = default_token::as_default_on_t<asio::steady_timer>;
        frequency_timer timer(executor);
        reset_local_socket();

        for (; !eof_;) {
#ifdef UDS
            auto [ec] =
                co_await proxy_process_socket_.async_connect(local::tcp_endpoint(unix_domian_ip));
#else
            local::tcp_endpoint endpoint(asio::ip::address{}.from_string("127.0.0.1"), local_port);
            auto [ec] = co_await proxy_process_socket_.async_connect(endpoint);
#endif
            if (!ec) {
                LOG_INFO("reconnect_process ok");
                asio::co_spawn(
                    executor,
                    [self = shared_from_this()] {
                        return self->process_2proxy_2client_or_2server();
                    },
                    asio::detached);
                co_return;
            }
            LOG_ERROR("reconnect_process error: {}", ec.message());
            timer.expires_from_now(std::chrono::seconds(3));
            co_await timer.async_wait();
        }
    }

    asio::awaitable<void> process_2proxy_2client_or_2server() {
        bypass_ = false;
        for (;;) {
            // Proxy read data from process

            // Proxy write data to server if bypass
            // auto [ec, _] = co_await asio::async_write(proxy_server_socket_,
            //                                          asio::buffer(client_proxy_buf_, n));
            // if (ec) [[unlikely]] {
            //    LOG_ERROR("proxy async_write to server error: {}", ec.message());
            //    grace_close_all();
            //    co_return;
            //}

            //// proxy write data to client if bypass
            // auto [wec, _] = co_await asio::async_write(proxy_client_socket_,
            //                                            asio::buffer(proxy_server_buf_, n));
            // if (wec) [[unlikely]] {
            //     LOG_ERROR("proxy async_write to client error: {}", wec.message());
            //     grace_close_all();
            //     co_return;
            // }
        }
    }

    asio::awaitable<void> client_2proxy_2server() {
    }

    asio::awaitable<void> client_2proxy_2process() {
        auto block = std::make_shared<data_block>(default_block_size);

        for (;;) {
            if (eof_) [[unlikely]] {
                LOG_INFO("remote peer (client) close the connection");
                grace_close_all();
                co_return;
            }
            // Prepare data block if available size < 64+5 byte, head_len is 5
            auto available = block->get_available();
            if (available < 69) {
                block = std::make_shared<data_block>(default_block_size);
                available = default_block_size;
            }
            // for head(1 + 4) byte
            auto data_pos = block->get_current_write_pos() + 5;
            available = available - 5;

            // Proxy read data from client
            auto [rec, n] =
                co_await proxy_client_socket_.async_read_some(asio::buffer(data_pos, available));
            if (rec) [[unlikely]] {
                if (rec != asio::error::eof) {
                    LOG_ERROR("proxy async_read_some from client error: {}", rec.message());
                    grace_close_all();
                    co_return;
                }
                // Connection was closed by the remote peer if error is eof
                // Still the buffer holds "n" bytes of the received data
                eof_ = true;
            }
            // Store the packet from client for bypass if need
            data_from_client_to_server_.emplace_back(packet{data_pos, n, block});

            // Proxy write data to process
            if (!bypass_) [[likely]] {
                *(uint8_t*)data_pos = (uint8_t)local::direction::from_client_to_server;
                *(uint32_t*)(data_pos + 1) = n;
                auto [ec, _] = co_await asio::async_write(proxy_process_socket_,
                                                          asio::buffer(data_pos - 5, n + 5));
                if (!ec) [[likely]] {
                    continue;
                }
                LOG_ERROR("enable bypass, proxy async_write to process error: {}", ec.message());

                // 1. Transfer the data_from_client_to_server_ to server
                // 2. Enable bypass, then client_2proxy_2server
                // 3. Try to reconnect process, then disable bypass if reconnect ok

                bypass_ = true;
                if (!reconnecting_) {
                    reconnecting_ = true;
                    auto executor = co_await asio::this_coro::executor;
                    asio::co_spawn(
                        executor, [self = shared_from_this()] { return self->reconnect_process(); },
                        asio::detached);
                }
            }
            else {
                // Proxy write data to server if bypass
                auto [ec, _] = co_await asio::async_write(proxy_server_socket_,
                                                          asio::buffer(client_proxy_buf_, n));
                if (ec) [[unlikely]] {
                    LOG_ERROR("proxy async_write to server error: {}", ec.message());
                    grace_close_all();
                    co_return;
                }
            }
        }
    }

    asio::awaitable<void> server_2proxy_2process() {
        for (;;) {
            if (eof_) [[unlikely]] {
                LOG_INFO("remote peer (server) close the connection");
                grace_close_all();
                co_return;
            }

            // Proxy read data from server
            auto [rec, n] =
                co_await proxy_server_socket_.async_read_some(asio::buffer(proxy_server_buf_));
            if (rec) [[unlikely]] {
                if (rec != asio::error::eof) {
                    LOG_ERROR("proxy async_read_some from server: {}", rec.message());
                    grace_close_all();
                    co_return;
                }
                eof_ = true;
            }

            // Proxy write data to process
            if (!bypass_) [[likely]] {
                auto [ec, _] = co_await asio::async_write(proxy_process_socket_,
                                                          asio::buffer(proxy_server_buf_, n));
                if (!ec) [[likely]] {
                    continue;
                }
                LOG_ERROR("proxy async_write to process error: {}", ec.message());

                // 1. Transfer the data_from_server_to_client_ to client
                // 2. Enable bypass, then server_2proxy_2client
                // 3. Try to reconnect process, then disable bypass if reconnect ok

                bypass_ = true;
                if (!reconnecting_) {
                    reconnecting_ = true;
                    auto executor = co_await asio::this_coro::executor;
                    asio::co_spawn(
                        executor, [self = shared_from_this()] { return self->reconnect_process(); },
                        asio::detached);
                }
            }
            else {
                // Proxy write data to client if bypass
                auto [wec, _] = co_await asio::async_write(proxy_client_socket_,
                                                           asio::buffer(proxy_server_buf_, n));
                if (wec) [[unlikely]] {
                    LOG_ERROR("proxy async_write to client error: {}", wec.message());
                    grace_close_all();
                    co_return;
                }
            }
        }
    }
};
}  // namespace cpp_proxy