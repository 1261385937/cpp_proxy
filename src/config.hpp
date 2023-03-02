#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "nlohmann/json.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace cpp_proxy {
struct server_info {
    std::string ip;
    uint16_t port;
    std::shared_ptr<std::atomic<uint64_t>> conns_{};

    bool operator<(const server_info& s) {
        return this->conns_ < s.conns_;
    }
};

class config {
private:
    std::unordered_map<uint16_t, std::vector<server_info>> proxy_info_;
    std::tuple<std::string, std::string, int, int> log_info_;
    size_t connection_buf_size_ = 8192;

public:
    static auto& instance() {
        static config c;
        return c;
    }

    void parse_conf() {
        auto conf_path = get_executable_path() + "cpp_proxy.json";
        auto file_size = std::filesystem::file_size(conf_path);
        auto file = fopen(conf_path.data(), "rb");
        std::string content(file_size, '\0');
        fread(content.data(), content.length(), 1, file);
        fclose(file);

        auto j = nlohmann::json::parse(content);
        log_info_ =
            std::make_tuple(j["log_dir"], j["log_name"], j["log_max_size"], j["log_max_size"]);

        connection_buf_size_ = j["connection_buf_size"];
        for (auto& proxy : j["cpp_proxy"]) {
            uint16_t listen_port = proxy["listen_port"];
            std::vector<server_info> servers_info;
            for (auto& servers : proxy["servers"]) {
                std::string ip = servers["ip"];
                uint16_t port = servers["port"];
                servers_info.emplace_back(
                    server_info{std::move(ip), port, std::make_shared<std::atomic<uint64_t>>(0)});
            }
            proxy_info_.emplace(listen_port, std::move(servers_info));
        }
    }

    auto get_log_info() {
        return log_info_;
    }

    auto& get_proxy_info() {
        return proxy_info_;
    }

    auto get_connection_buf_size() {
        return connection_buf_size_;
    }

private:
    config() = default;
    config(const config&) = delete;
    config& operator=(const config&) = delete;

    template <size_t UpDepth = 0>
    std::string get_executable_path() {
        // the lengh is enough
        constexpr size_t len = 2048;
        char full_path[len]{};
        std::string_view path;
#ifdef _WIN32
        GetModuleFileNameA(nullptr, full_path, len);
#else
        readlink("/proc/self/exe", full_path, len);
#endif
        path = full_path;
        path = path.substr(0, path.find_last_of(R"(\)") + 1);
        if constexpr (UpDepth == 0) {
            return std::string{path.data(), path.length()};
        } else {
            for (size_t i = 0; i < UpDepth; i++) {
                path = path.substr(0, path.length() - 1);
                path = path.substr(0, path.find_last_of(R"(\)") + 1);
            }
            return std::string{path.data(), path.length()};
        }
    }
};

}  // namespace cpp_proxy