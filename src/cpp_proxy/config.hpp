#pragma once
#include <atomic>
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

template <size_t UpDepth = 0>
inline std::string get_executable_path() {
   // the lengh is enough
   constexpr size_t len = 2048;
   char full_path[len]{};
   std::string_view path;
#ifdef _WIN32
   GetModuleFileNameA(nullptr, full_path, len);
   char split_ident = R"(\)";
#else
   readlink("/proc/self/exe", full_path, len);
   char split_ident = R"(/)";
#endif
   path = full_path;
   path = path.substr(0, path.find_last_of(split_ident) + 1);
   if constexpr (UpDepth == 0) {
      return std::string{path.data(), path.length()};
   }
   else {
      for (size_t i = 0; i < UpDepth; i++) {
         path = path.substr(0, path.length() - 1);
         path = path.substr(0, path.find_last_of(split_ident) + 1);
      }
      return std::string{path.data(), path.length()};
   }
}

struct server_info {
   std::string ip;
   uint16_t port;
   std::shared_ptr<std::atomic<uint64_t>> conns_{};

   bool operator<(const server_info& s) {
      return this->conns_ < s.conns_;
   }
};

struct log_info {
#ifndef _WIN32
   std::string log_dir = "/var/log/cpp_proxy/";
#else
   std::string log_dir = get_executable_path() + "/log/";
#endif
   std::string log_name = "cpp_proxy";
   uint32_t log_max_files = 10;
   uint32_t log_max_size = 10 * 1024 * 1024;
};

class config {
private:
   std::unordered_map<uint16_t, std::vector<server_info>> proxy_info_;
   log_info log_info_;
   std::atomic<size_t> connection_buf_size_ = 8192;
   std::string conf_path_;

public:
   static auto& instance() {
      static config c;
      return c;
   }

   void parse_conf(std::string_view conf_path) {
      conf_path_ = conf_path;
      auto file_size = std::filesystem::file_size(conf_path_);
      auto file = fopen(conf_path.data(), "rb");
      std::string content(file_size, '\0');
      fread(content.data(), content.length(), 1, file);
      fclose(file);
      auto j = nlohmann::json::parse(content);

      //Log config will not be monitored
      log_info log_info_{};
      if (j.contains("log_dir")) {
         log_info_.log_dir = j["log_dir"];
      }
      if (j.contains("log_name")) {
         log_info_.log_name = j["log_name"];
      }
      if (j.contains("log_max_files")) {
         log_info_.log_max_files = j["log_max_files"];
      }
      if (j.contains("log_max_size")) {
         log_info_.log_max_size = j["log_max_size"];
      }

      // Proxy config will be monitored, cpp_proxy will auto deal the changed config
      connection_buf_size_ = j["connection_buf_size"];
      for (auto& proxy : j["tcp_proxy"]) {
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
      return connection_buf_size_.load();
   }

private:
   config() = default;
   config(const config&) = delete;
   config& operator=(const config&) = delete;
};

}  // namespace cpp_proxy