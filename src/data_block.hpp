#pragma once
#include <atomic>
#include <memory>
#include <string>

namespace cpp_proxy {
constexpr uint32_t default_block_size = 1024 * 1024 * 2;

struct data_block {
private:
   uint32_t used_size_;
   const uint32_t capacity_;
   std::shared_ptr<char[]> buffer_;

   inline static std::atomic<uint64_t> total_free_bytes_{0};

public:
   explicit data_block(uint32_t capacity)
       : used_size_(0), capacity_(capacity), buffer_(new char[capacity_], [this](char* p) {
            total_free_bytes_ += used_size_;
            delete[] p;
         }) {
   }

   auto get_available() const {
      return capacity_ - used_size_;
   }

   void update_available(uint32_t used_size) {
      used_size_ += used_size;
   }

   auto get_current_write_pos() const {
      return &buffer_[used_size_];
   }

   void append(std::string_view str) {
      const auto pos = &buffer_[used_size_];
      memcpy(pos, str.data(), str.length());
      used_size_ += static_cast<uint32_t>(str.length());
   }

   static auto get_total_free_bytes() {
      return total_free_bytes_.load();
   }
};

}  // namespace cpp_proxy
