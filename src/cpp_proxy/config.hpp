#pragma once
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <set>
#include <shared_mutex>
#include <string>
#include <vector>
#include "load_balance.hpp"
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
   std::string_view split_ident = R"(\)";
#else
   readlink("/proc/self/exe", full_path, len);
   std::string_view split_ident = R"(/)";
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
   size_t conn_buf_size = 8124;
   bool response_bypass = true;
};

struct proxy_entity {
   uint16_t listen_port = 0;
   std::vector<std::shared_ptr<server_info>> servers;
   bool operator<(const proxy_entity& s) const {
      return this->listen_port < s.listen_port;
   }
};

struct log_info {
#ifndef _WIN32
   std::string log_dir = "/var/log/cpp_proxy/";
#else
   std::string log_dir = get_executable_path() + "/log/";
#endif
   std::string log_name = "cpp_proxy";
   uint32_t log_max_size = 10 * 1024 * 1024;
   uint32_t log_max_files = 10;
};

class config {
private:
   std::string conf_path_;
   log_info log_info_;
   std::shared_mutex shared_mtx_;
   std::set<proxy_entity> entities_;
   std::set<proxy_entity> del_;
   std::set<proxy_entity> add_;

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

      // Proxy config will be monitored, cpp_proxy will deal the changed config automatically
      std::set<proxy_entity> entities{};
      for (auto& proxy : j["tcp_proxy"]) {
         std::vector<std::shared_ptr<server_info>> servers;
         for (auto& server : proxy["servers"]) {
            auto si = std::make_shared<server_info>();
            si->ip = server["ip"];
            si->port = server["port"];
            if (server.contains("conn_buf_size")) {
               si->conn_buf_size = server["conn_buf_size"];
            }
            if (server.contains("response_bypass")) {
               si->response_bypass = server["response_bypass"];
            }
            servers.emplace_back(std::move(si));
         }
         entities.emplace(proxy_entity{proxy["listen_port"], std::move(servers)});
      }

      del_.clear();
      std::set_difference(entities_.begin(), entities_.end(), entities.begin(), entities.end(),
                          std::inserter(del_, del_.begin()));
      add_.clear();
      std::set_difference(entities.begin(), entities.end(), entities_.begin(), entities_.end(),
                          std::inserter(add_, add_.begin()));

      std::unique_lock lock(shared_mtx_);
      entities_ = std::move(entities);
   }

   auto& get_log_info() {
      return log_info_;
   }

   auto& get_proxy_entities() {
      return entities_;
   }

   auto get_proxy_server(uint16_t listen_port) {
      std::shared_lock share_lock(shared_mtx_);
      auto it = entities_.find(proxy_entity{listen_port});
      if (it == entities_.end()) {
         return std::shared_ptr<server_info>{};
      }
      return (*it).servers[0];
   }

private:
   config() = default;
   config(const config&) = delete;
   config& operator=(const config&) = delete;
};

}  // namespace cpp_proxy