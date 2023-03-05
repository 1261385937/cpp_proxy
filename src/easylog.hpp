#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace easylog {
struct easylog_options {
   std::string log_name;
   std::string log_dir;
   int max_size = 10 * 1024 * 1024;
   int max_files = 10;
   int flush_periodic_ms = 3000;
};
namespace level {
enum logger_level {
   trace = spdlog::level::trace,
   debug = spdlog::level::debug,
   info = spdlog::level::info,
   warn = spdlog::level::warn,
   err = spdlog::level::err,
   critical = spdlog::level::critical,
   off = spdlog::level::off
};
}  // namespace level

namespace detail {
//[2022-12-15 14:47:41.305] [EXAMPLE] [info] [tid:19372 main.cpp:129@main] 123456
// constexpr char common_pattern[] = "[%Y-%m-%d %T.%e] [%n] [%l] [tid:%t %s:%#@%!] %v";

//[2022-12-15 14:47:41.305] [info] [tid:19372 main.cpp:129] 123456
constexpr char common_pattern[] = "[%Y-%m-%d %T.%e] [%l] [tid:%t %s:%#] %v";

class logger_modules {
private:
   easylog_options options_;
   std::unordered_map<std::string, spdlog::logger*> modules_;
   std::vector<spdlog::sink_ptr> sinks_;
   std::mutex mtx_;
   std::thread flush_thread_;
   std::atomic<bool> run_ = true;
   std::condition_variable cv_;

public:
   static logger_modules& instance() {
      static logger_modules lm;
      return lm;
   }

   void set_options(const easylog_options& opt) {
      options_ = opt;
   }

   void add_logger_module(std::string&& module_name, spdlog::logger* p) {
      auto it = modules_.find(module_name);
      if (it != modules_.end()) {
         throw std::runtime_error(std::string(module_name) +
                                  " already exist, please change to another module name");
      }
      std::lock_guard l(mtx_);
      modules_.emplace(std::move(module_name), p);
   }

   void set_module_level(const std::string& module_name, level::logger_level level) {
      auto it = modules_.find(module_name);
      if (it == modules_.end()) {
         throw std::runtime_error(std::string(module_name) +
                                  " not exist, please declare the logger module");
      }
      // check initialization or not
      if (sinks_.empty()) {
         sinks_ = get_sinks();
      }
      if (it->second->sinks().empty()) {
         it->second->sinks() = sinks_;
         it->second->set_pattern(common_pattern);
         it->second->flush_on(spdlog::level::warn);
      }
      it->second->set_level(static_cast<spdlog::level::level_enum>(level));
   }

private:
   logger_modules() {
      flush_thread_ = std::thread([this]() {
         while (run_) {
            using namespace std::chrono;
            std::unique_lock lock(mtx_);
            if (cv_.wait_for(lock, milliseconds(options_.flush_periodic_ms),
                             [this] { return !run_; })) {
               return;
            }

            auto modules = modules_;
            lock.unlock();
            for (auto& m : modules) {
               m.second->flush();
            }
         }
      });
   }

   ~logger_modules() {
      run_ = false;
      cv_.notify_one();
      if (flush_thread_.joinable()) {
         flush_thread_.join();
      }
   }

   std::vector<spdlog::sink_ptr> get_sinks() {
      if (options_.log_dir.empty() || options_.log_name.empty()) {
         throw std::logic_error("miss log_dir or log_name");
      }

      std::vector<spdlog::sink_ptr> sinks;
#ifdef _WIN32
      int pid = _getpid();
#else
      pid_t pid = getpid();
#endif
      std::string filename = options_.log_dir;
      std::string name = options_.log_name;
      name.append("_").append(std::to_string(pid)).append(".log");
      filename.append(name);
      auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
          filename, options_.max_size, options_.max_files);
      sinks.push_back(file_sink);

      auto err_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
      err_sink->set_level(spdlog::level::info);
      sinks.push_back(err_sink);

      return sinks;
   }
};
}  // namespace detail

inline void setup_logger(const easylog_options& options) {
   detail::logger_modules::instance().set_options(options);
}

inline auto set_logger_module_level(const std::string& module_name, level::logger_level level) {
   detail::logger_modules::instance().set_module_level(module_name, level);
}
}  // namespace easylog

// module define
#define DECLARE_LOGGER_MODULE(module_name)                                                  \
   namespace easylog::modules {                                                             \
   namespace module_name {                                                                  \
   class logger_module_##module_name : public spdlog::logger {                              \
   public:                                                                                  \
      logger_module_##module_name() : spdlog::logger(#module_name) {                        \
         easylog::detail::logger_modules::instance().add_logger_module(#module_name, this); \
      }                                                                                     \
   };                                                                                       \
   inline std::shared_ptr<logger_module_##module_name> g_logger_module_##module_name =      \
       std::make_shared<logger_module_##module_name>();                                     \
   }                                                                                        \
   }

#define MODULE_TRACE(module_name, ...)                                               \
   easylog::modules::module_name::g_logger_module_##module_name->log(                \
       spdlog::source_loc{__builtin_FILE(), __builtin_LINE(), __builtin_FUNCTION()}, \
       spdlog::level::trace, __VA_ARGS__);
#define MODULE_DEBUG(module_name, ...)                                               \
   easylog::modules::module_name::g_logger_module_##module_name->log(                \
       spdlog::source_loc{__builtin_FILE(), __builtin_LINE(), __builtin_FUNCTION()}, \
       spdlog::level::debug, __VA_ARGS__);
#define MODULE_INFO(module_name, ...)                                                \
   easylog::modules::module_name::g_logger_module_##module_name->log(                \
       spdlog::source_loc{__builtin_FILE(), __builtin_LINE(), __builtin_FUNCTION()}, \
       spdlog::level::info, __VA_ARGS__);
#define MODULE_WARN(module_name, ...)                                                \
   easylog::modules::module_name::g_logger_module_##module_name->log(                \
       spdlog::source_loc{__builtin_FILE(), __builtin_LINE(), __builtin_FUNCTION()}, \
       spdlog::level::warn, __VA_ARGS__);
#define MODULE_ERROR(module_name, ...)                                               \
   easylog::modules::module_name::g_logger_module_##module_name->log(                \
       spdlog::source_loc{__builtin_FILE(), __builtin_LINE(), __builtin_FUNCTION()}, \
       spdlog::level::err, __VA_ARGS__);
#define MODULE_CRITICAL(module_name, ...)                                            \
   easylog::modules::module_name::g_logger_module_##module_name->log(                \
       spdlog::source_loc{__builtin_FILE(), __builtin_LINE(), __builtin_FUNCTION()}, \
       spdlog::level::critical, __VA_ARGS__);

// This is a example, module_name should be uppercase for replacement.
// Replace All 'LOG' with your module_name(ex: AGENT)
// If you want to use a global logger, just do like this:
// 1. Uncomment the below code, the global logger should be LOG_XXX
// 2. In main.cpp, set the log level.
//	 easylog::set_logger_module_level("LOG", easylog::level::trace);

DECLARE_LOGGER_MODULE(LOG);
#define LOG_TRACE(...) MODULE_TRACE(LOG, __VA_ARGS__);
#define LOG_DEBUG(...) MODULE_DEBUG(LOG, __VA_ARGS__);
#define LOG_INFO(...) MODULE_INFO(LOG, __VA_ARGS__);
#define LOG_WARN(...) MODULE_WARN(LOG, __VA_ARGS__);
#define LOG_ERROR(...) MODULE_ERROR(LOG, __VA_ARGS__);
#define LOG_CRITICAL(...) MODULE_CRITICAL(LOG, __VA_ARGS__);
