// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <version>

#include "glaze/core/common.hpp"

// Provides a thread safe wrapper around a std::string, which Glaze knows how to serialize/deserialize safely

namespace glz
{
   struct async_string
   {
     private:
      std::string str;
      mutable std::shared_mutex mutex;

     public:
      async_string() = default;
      async_string(const char* s) : str(s) {}
      async_string(const std::string& s) : str(s) {}
      async_string(std::string&& s) : str(std::move(s)) {}
      async_string(const std::string_view sv) : str(sv) {}

      async_string(const async_string& other)
      {
         std::shared_lock lock(other.mutex);
         str = other.str;
      }

      async_string(async_string&& other) noexcept
      {
         std::unique_lock lock(other.mutex);
         str = std::move(other.str);
         other.str.clear(); // Ensure moved-from state is empty for consistent behavior across ABIs
      }

      async_string& operator=(const async_string& other)
      {
         if (this != &other) {
            std::unique_lock lock1(mutex, std::defer_lock);
            std::shared_lock lock2(other.mutex, std::defer_lock);
            std::lock(lock1, lock2);
            str = other.str;
         }
         return *this;
      }

      async_string& operator=(async_string&& other) noexcept
      {
         if (this != &other) {
            std::unique_lock lock1(mutex, std::defer_lock);
            std::unique_lock lock2(other.mutex, std::defer_lock);
            std::lock(lock1, lock2);
            str = std::move(other.str);
            other.str.clear(); // Ensure moved-from state is empty for consistent behavior across ABIs
         }
         return *this;
      }

      async_string& operator=(const std::string& s)
      {
         std::unique_lock lock(mutex);
         str = s;
         return *this;
      }

      async_string& operator=(std::string&& s)
      {
         std::unique_lock lock(mutex);
         str = std::move(s);
         return *this;
      }

      async_string& operator=(const char* s)
      {
         std::unique_lock lock(mutex);
         str = s;
         return *this;
      }

      async_string& operator=(const std::string_view sv)
      {
         std::unique_lock lock(mutex);
         str = sv;
         return *this;
      }

      explicit operator std::string() const
      {
         std::shared_lock lock{mutex};
         return str;
      }

      struct proxy
      {
         std::string* ptr{};
         std::unique_lock<std::shared_mutex> lock{};

        public:
         proxy(std::string& p, std::unique_lock<std::shared_mutex>&& lock) noexcept : ptr{&p}, lock(std::move(lock)) {}

         operator std::string_view() { return *ptr; }

         std::string* operator->() noexcept { return ptr; }
         const std::string* operator->() const noexcept { return ptr; }

         std::string& operator*() noexcept { return *ptr; }
         const std::string& operator*() const noexcept { return *ptr; }

         std::string& value() noexcept { return *ptr; }
         const std::string& value() const noexcept { return *ptr; }

         // Size/capacity
         [[nodiscard]] size_t size() const noexcept { return ptr->size(); }
         [[nodiscard]] size_t length() const noexcept { return ptr->length(); }
         [[nodiscard]] size_t max_size() const noexcept { return ptr->max_size(); }
         [[nodiscard]] size_t capacity() const noexcept { return ptr->capacity(); }
         [[nodiscard]] bool empty() const noexcept { return ptr->empty(); }
         void reserve(size_t new_cap) { ptr->reserve(new_cap); }
         void shrink_to_fit() { ptr->shrink_to_fit(); }

         // Element access
         char& operator[](size_t pos) { return (*ptr)[pos]; }
         const char& operator[](size_t pos) const { return (*ptr)[pos]; }
         char& at(size_t pos) { return ptr->at(pos); }
         const char& at(size_t pos) const { return ptr->at(pos); }
         char& front() { return ptr->front(); }
         const char& front() const { return ptr->front(); }
         char& back() { return ptr->back(); }
         const char& back() const { return ptr->back(); }
         char* data() noexcept { return ptr->data(); }
         const char* data() const noexcept { return ptr->data(); }
         const char* c_str() const noexcept { return ptr->c_str(); }

         // Modifiers
         void clear() noexcept { ptr->clear(); }
         void insert(size_t pos, const std::string& str) { ptr->insert(pos, str); }
         void insert(size_t pos, const std::string_view sv) { ptr->insert(pos, sv); }
         void insert(size_t pos, const char* s) { ptr->insert(pos, s); }
         void insert(size_t pos, const char* s, size_t n) { ptr->insert(pos, s, n); }
         void insert(size_t pos, size_t n, char c) { ptr->insert(pos, n, c); }
         void erase(size_t pos = 0, size_t count = std::string::npos) { ptr->erase(pos, count); }
         void push_back(char c) { ptr->push_back(c); }
         void pop_back() { ptr->pop_back(); }

         proxy& append(const std::string& str)
         {
            ptr->append(str);
            return *this;
         }
         proxy& append(const std::string_view sv)
         {
            ptr->append(sv);
            return *this;
         }
         proxy& append(const char* s)
         {
            ptr->append(s);
            return *this;
         }
         proxy& append(const char* s, size_t n)
         {
            ptr->append(s, n);
            return *this;
         }
         proxy& append(size_t n, char c)
         {
            ptr->append(n, c);
            return *this;
         }

         proxy& operator+=(const std::string& str)
         {
            ptr->operator+=(str);
            return *this;
         }
         proxy& operator+=(const std::string_view sv)
         {
            ptr->operator+=(sv);
            return *this;
         }
         proxy& operator+=(const char* s)
         {
            ptr->operator+=(s);
            return *this;
         }
         proxy& operator+=(char c)
         {
            ptr->operator+=(c);
            return *this;
         }

         void resize(size_t count) { ptr->resize(count); }
         void resize(size_t count, char ch) { ptr->resize(count, ch); }
         void swap(std::string& other) { ptr->swap(other); }

         // String operations
         int compare(const std::string& str) const noexcept { return ptr->compare(str); }
         int compare(const std::string_view sv) const noexcept { return ptr->compare(sv); }
         int compare(const char* s) const noexcept { return ptr->compare(s); }
         int compare(size_t pos1, size_t count1, const std::string& str) const
         {
            return ptr->compare(pos1, count1, str);
         }

         std::string substr(size_t pos = 0, size_t count = std::string::npos) const { return ptr->substr(pos, count); }

         bool starts_with(const std::string_view sv) const noexcept { return ptr->starts_with(sv); }
         bool starts_with(char c) const noexcept { return ptr->starts_with(c); }
         bool starts_with(const char* s) const { return ptr->starts_with(s); }

         bool ends_with(const std::string_view sv) const noexcept { return ptr->ends_with(sv); }
         bool ends_with(char c) const noexcept { return ptr->ends_with(c); }
         bool ends_with(const char* s) const { return ptr->ends_with(s); }

         // Search
         size_t find(const std::string& str, size_t pos = 0) const noexcept { return ptr->find(str, pos); }
         size_t find(const std::string_view sv, size_t pos = 0) const noexcept { return ptr->find(sv, pos); }
         size_t find(const char* s, size_t pos = 0) const { return ptr->find(s, pos); }
         size_t find(const char* s, size_t pos, size_t count) const { return ptr->find(s, pos, count); }
         size_t find(char ch, size_t pos = 0) const noexcept { return ptr->find(ch, pos); }

         size_t rfind(const std::string& str, size_t pos = std::string::npos) const noexcept
         {
            return ptr->rfind(str, pos);
         }
         size_t rfind(const std::string_view sv, size_t pos = std::string::npos) const noexcept
         {
            return ptr->rfind(sv, pos);
         }
         size_t rfind(const char* s, size_t pos = std::string::npos) const { return ptr->rfind(s, pos); }
         size_t rfind(const char* s, size_t pos, size_t count) const { return ptr->rfind(s, pos, count); }
         size_t rfind(char ch, size_t pos = std::string::npos) const noexcept { return ptr->rfind(ch, pos); }

         size_t find_first_of(const std::string& str, size_t pos = 0) const noexcept
         {
            return ptr->find_first_of(str, pos);
         }
         size_t find_first_of(const std::string_view sv, size_t pos = 0) const noexcept
         {
            return ptr->find_first_of(sv, pos);
         }
         size_t find_first_of(const char* s, size_t pos = 0) const { return ptr->find_first_of(s, pos); }
         size_t find_first_of(const char* s, size_t pos, size_t count) const
         {
            return ptr->find_first_of(s, pos, count);
         }
         size_t find_first_of(char ch, size_t pos = 0) const noexcept { return ptr->find_first_of(ch, pos); }

         size_t find_last_of(const std::string& str, size_t pos = std::string::npos) const noexcept
         {
            return ptr->find_last_of(str, pos);
         }
         size_t find_last_of(const std::string_view sv, size_t pos = std::string::npos) const noexcept
         {
            return ptr->find_last_of(sv, pos);
         }
         size_t find_last_of(const char* s, size_t pos = std::string::npos) const { return ptr->find_last_of(s, pos); }
         size_t find_last_of(const char* s, size_t pos, size_t count) const { return ptr->find_last_of(s, pos, count); }
         size_t find_last_of(char ch, size_t pos = std::string::npos) const noexcept
         {
            return ptr->find_last_of(ch, pos);
         }

         size_t find_first_not_of(const std::string& str, size_t pos = 0) const noexcept
         {
            return ptr->find_first_not_of(str, pos);
         }
         size_t find_first_not_of(const std::string_view sv, size_t pos = 0) const noexcept
         {
            return ptr->find_first_not_of(sv, pos);
         }
         size_t find_first_not_of(const char* s, size_t pos = 0) const { return ptr->find_first_not_of(s, pos); }
         size_t find_first_not_of(const char* s, size_t pos, size_t count) const
         {
            return ptr->find_first_not_of(s, pos, count);
         }
         size_t find_first_not_of(char ch, size_t pos = 0) const noexcept { return ptr->find_first_not_of(ch, pos); }

         size_t find_last_not_of(const std::string& str, size_t pos = std::string::npos) const noexcept
         {
            return ptr->find_last_not_of(str, pos);
         }
         size_t find_last_not_of(const std::string_view sv, size_t pos = std::string::npos) const noexcept
         {
            return ptr->find_last_not_of(sv, pos);
         }
         size_t find_last_not_of(const char* s, size_t pos = std::string::npos) const
         {
            return ptr->find_last_not_of(s, pos);
         }
         size_t find_last_not_of(const char* s, size_t pos, size_t count) const
         {
            return ptr->find_last_not_of(s, pos, count);
         }
         size_t find_last_not_of(char ch, size_t pos = std::string::npos) const noexcept
         {
            return ptr->find_last_not_of(ch, pos);
         }

         // Additional functionality
         proxy& replace(size_t pos, size_t count, const std::string& str)
         {
            ptr->replace(pos, count, str);
            return *this;
         }
         proxy& replace(size_t pos, size_t count, const std::string_view sv)
         {
            ptr->replace(pos, count, sv);
            return *this;
         }
         proxy& replace(size_t pos, size_t count, const char* s)
         {
            ptr->replace(pos, count, s);
            return *this;
         }
         proxy& replace(size_t pos, size_t count, const char* s, size_t count2)
         {
            ptr->replace(pos, count, s, count2);
            return *this;
         }
         proxy& replace(size_t pos, size_t count, size_t count2, char ch)
         {
            ptr->replace(pos, count, count2, ch);
            return *this;
         }
      };

      proxy write() { return {str, std::unique_lock{mutex}}; }

      struct const_proxy
      {
         const std::string* ptr{};
         std::shared_lock<std::shared_mutex> lock{};

        public:
         const_proxy(const std::string& p, std::shared_lock<std::shared_mutex>&& lock) noexcept
            : ptr{&p}, lock(std::move(lock))
         {}

         operator const std::string_view() const { return *ptr; }

         const std::string* operator->() const noexcept { return ptr; }

         const std::string& operator*() const noexcept { return *ptr; }

         const std::string& value() const noexcept { return *ptr; }

         // Size/capacity
         [[nodiscard]] size_t size() const noexcept { return ptr->size(); }
         [[nodiscard]] size_t length() const noexcept { return ptr->length(); }
         [[nodiscard]] size_t max_size() const noexcept { return ptr->max_size(); }
         [[nodiscard]] size_t capacity() const noexcept { return ptr->capacity(); }
         [[nodiscard]] bool empty() const noexcept { return ptr->empty(); }

         // Element access
         const char& operator[](size_t pos) const { return (*ptr)[pos]; }
         const char& at(size_t pos) const { return ptr->at(pos); }
         const char& front() const { return ptr->front(); }
         const char& back() const { return ptr->back(); }
         const char* data() const noexcept { return ptr->data(); }
         const char* c_str() const noexcept { return ptr->c_str(); }

         // String operations
         int compare(const std::string& str) const noexcept { return ptr->compare(str); }
         int compare(const std::string_view sv) const noexcept { return ptr->compare(sv); }
         int compare(const char* s) const noexcept { return ptr->compare(s); }
         int compare(size_t pos1, size_t count1, const std::string& str) const
         {
            return ptr->compare(pos1, count1, str);
         }

         std::string substr(size_t pos = 0, size_t count = std::string::npos) const { return ptr->substr(pos, count); }

         bool starts_with(const std::string_view sv) const noexcept { return ptr->starts_with(sv); }
         bool starts_with(char c) const noexcept { return ptr->starts_with(c); }
         bool starts_with(const char* s) const { return ptr->starts_with(s); }

         bool ends_with(const std::string_view sv) const noexcept { return ptr->ends_with(sv); }
         bool ends_with(char c) const noexcept { return ptr->ends_with(c); }
         bool ends_with(const char* s) const { return ptr->ends_with(s); }

         // Search
         size_t find(const std::string& str, size_t pos = 0) const noexcept { return ptr->find(str, pos); }
         size_t find(const std::string_view sv, size_t pos = 0) const noexcept { return ptr->find(sv, pos); }
         size_t find(const char* s, size_t pos = 0) const { return ptr->find(s, pos); }
         size_t find(const char* s, size_t pos, size_t count) const { return ptr->find(s, pos, count); }
         size_t find(char ch, size_t pos = 0) const noexcept { return ptr->find(ch, pos); }

         size_t rfind(const std::string& str, size_t pos = std::string::npos) const noexcept
         {
            return ptr->rfind(str, pos);
         }
         size_t rfind(const std::string_view sv, size_t pos = std::string::npos) const noexcept
         {
            return ptr->rfind(sv, pos);
         }
         size_t rfind(const char* s, size_t pos = std::string::npos) const { return ptr->rfind(s, pos); }
         size_t rfind(const char* s, size_t pos, size_t count) const { return ptr->rfind(s, pos, count); }
         size_t rfind(char ch, size_t pos = std::string::npos) const noexcept { return ptr->rfind(ch, pos); }

         size_t find_first_of(const std::string& str, size_t pos = 0) const noexcept
         {
            return ptr->find_first_of(str, pos);
         }
         size_t find_first_of(const std::string_view sv, size_t pos = 0) const noexcept
         {
            return ptr->find_first_of(sv, pos);
         }
         size_t find_first_of(const char* s, size_t pos = 0) const { return ptr->find_first_of(s, pos); }
         size_t find_first_of(const char* s, size_t pos, size_t count) const
         {
            return ptr->find_first_of(s, pos, count);
         }
         size_t find_first_of(char ch, size_t pos = 0) const noexcept { return ptr->find_first_of(ch, pos); }

         size_t find_last_of(const std::string& str, size_t pos = std::string::npos) const noexcept
         {
            return ptr->find_last_of(str, pos);
         }
         size_t find_last_of(const std::string_view sv, size_t pos = std::string::npos) const noexcept
         {
            return ptr->find_last_of(sv, pos);
         }
         size_t find_last_of(const char* s, size_t pos = std::string::npos) const { return ptr->find_last_of(s, pos); }
         size_t find_last_of(const char* s, size_t pos, size_t count) const { return ptr->find_last_of(s, pos, count); }
         size_t find_last_of(char ch, size_t pos = std::string::npos) const noexcept
         {
            return ptr->find_last_of(ch, pos);
         }

         size_t find_first_not_of(const std::string& str, size_t pos = 0) const noexcept
         {
            return ptr->find_first_not_of(str, pos);
         }
         size_t find_first_not_of(const std::string_view sv, size_t pos = 0) const noexcept
         {
            return ptr->find_first_not_of(sv, pos);
         }
         size_t find_first_not_of(const char* s, size_t pos = 0) const { return ptr->find_first_not_of(s, pos); }
         size_t find_first_not_of(const char* s, size_t pos, size_t count) const
         {
            return ptr->find_first_not_of(s, pos, count);
         }
         size_t find_first_not_of(char ch, size_t pos = 0) const noexcept { return ptr->find_first_not_of(ch, pos); }

         size_t find_last_not_of(const std::string& str, size_t pos = std::string::npos) const noexcept
         {
            return ptr->find_last_not_of(str, pos);
         }
         size_t find_last_not_of(const std::string_view sv, size_t pos = std::string::npos) const noexcept
         {
            return ptr->find_last_not_of(sv, pos);
         }
         size_t find_last_not_of(const char* s, size_t pos = std::string::npos) const
         {
            return ptr->find_last_not_of(s, pos);
         }
         size_t find_last_not_of(const char* s, size_t pos, size_t count) const
         {
            return ptr->find_last_not_of(s, pos, count);
         }
         size_t find_last_not_of(char ch, size_t pos = std::string::npos) const noexcept
         {
            return ptr->find_last_not_of(ch, pos);
         }
      };

      const_proxy read() const { return {str, std::shared_lock{mutex}}; }

      // Capacity
      size_t size() const noexcept
      {
         std::shared_lock lock(mutex);
         return str.size();
      }

      size_t length() const noexcept
      {
         std::shared_lock lock(mutex);
         return str.length();
      }

      bool empty() const noexcept
      {
         std::shared_lock lock(mutex);
         return str.empty();
      }

      // Modifiers
      void clear() noexcept
      {
         std::unique_lock lock(mutex);
         str.clear();
      }

      void push_back(char c)
      {
         std::unique_lock lock(mutex);
         str.push_back(c);
      }

      void pop_back()
      {
         std::unique_lock lock(mutex);
         str.pop_back();
      }

      template <class RHS>
         requires(std::same_as<std::remove_cvref_t<RHS>, std::string>)
      async_string& append(RHS&& s)
      {
         std::unique_lock lock(mutex);
         str.append(std::forward<RHS>(s));
         return *this;
      }

      async_string& append(const char* s, size_t count)
      {
         std::unique_lock lock(mutex);
         str.append(s, count);
         return *this;
      }

      async_string& append(const std::string_view sv)
      {
         std::unique_lock lock(mutex);
         str.append(sv);
         return *this;
      }

      template <class RHS>
         requires(std::same_as<std::remove_cvref_t<RHS>, async_string>)
      async_string& append(RHS&& other)
      {
         std::unique_lock lock(mutex, std::defer_lock);
         std::shared_lock lock2(other.mutex, std::defer_lock);
         std::lock(lock, lock2);
         str.append(other.str);
         return *this;
      }

      template <class RHS>
         requires(std::same_as<std::remove_cvref_t<RHS>, std::string>)
      async_string& insert(size_t pos, RHS&& s)
      {
         std::unique_lock lock(mutex);
         str.insert(pos, std::forward<RHS>(s));
         return *this;
      }

      async_string& insert(size_t pos, const std::string_view sv)
      {
         std::unique_lock lock(mutex);
         str.insert(pos, sv);
         return *this;
      }

      template <class RHS>
         requires(std::same_as<std::remove_cvref_t<RHS>, async_string>)
      async_string& insert(size_t pos, RHS&& other)
      {
         std::unique_lock lock(mutex, std::defer_lock);
         std::shared_lock lock2(other.mutex, std::defer_lock);
         std::lock(lock, lock2);
         str.insert(pos, other.str);
         return *this;
      }

      template <class RHS>
         requires(std::same_as<std::remove_cvref_t<RHS>, std::string>)
      async_string& operator+=(RHS&& s)
      {
         return append(std::forward<RHS>(s));
      }

      async_string& operator+=(const std::string_view sv) { return append(sv); }

      async_string& operator+=(char c)
      {
         std::unique_lock lock(mutex);
         str += c;
         return *this;
      }

      void reserve(size_t count)
      {
         std::unique_lock lock(mutex);
         str.reserve(count);
      }

      void resize(size_t count)
      {
         std::unique_lock lock(mutex);
         str.resize(count);
      }

      void resize(size_t count, const char ch)
      {
         std::unique_lock lock(mutex);
         str.resize(count, ch);
      }

      // Element access
      char at(size_t pos) const
      {
         std::shared_lock lock(mutex);
         return str.at(pos);
      }

      char operator[](size_t pos) const
      {
         std::shared_lock lock(mutex);
         return str[pos];
      }

      char front() const
      {
         std::shared_lock lock(mutex);
         return str.front();
      }

      char back() const
      {
         std::shared_lock lock(mutex);
         return str.back();
      }

      int compare(const async_string& other) const
      {
         std::scoped_lock lock{mutex, other.mutex};
         return str.compare(other.str);
      }

      bool starts_with(const std::string_view other) const
      {
         std::shared_lock lock{mutex};
         return str.starts_with(other);
      }

      bool ends_with(const std::string_view other) const
      {
         std::shared_lock lock{mutex};
         return str.ends_with(other);
      }

      std::string substr(size_t pos = 0, size_t len = std::string::npos) const
      {
         std::shared_lock lock{mutex};
         return str.substr(pos, len);
      }

      friend bool operator==(const async_string& lhs, const async_string& rhs)
      {
         if (&lhs == &rhs) return true;
         std::shared_lock lock(lhs.mutex, std::defer_lock);
         std::shared_lock lock2(rhs.mutex, std::defer_lock);
         std::lock(lock, lock2);
         return lhs.str == rhs.str;
      }

      // async_string != async_string
      friend bool operator!=(const async_string& lhs, const async_string& rhs) { return !(lhs == rhs); }

      // async_string == std::string_view
      friend bool operator==(const async_string& lhs, std::string_view rhs)
      {
         std::shared_lock lock(lhs.mutex);
         return lhs.str == rhs;
      }

      // std::string_view == async_string
      friend bool operator==(std::string_view lhs, const async_string& rhs)
      {
         std::shared_lock lock(rhs.mutex);
         return lhs == rhs.str;
      }

      // async_string != std::string_view
      friend bool operator!=(const async_string& lhs, std::string_view rhs) { return !(lhs == rhs); }

      // std::string_view != async_string
      friend bool operator!=(std::string_view lhs, const async_string& rhs) { return !(lhs == rhs); }

      // async_string == std::string
      friend bool operator==(const async_string& lhs, const std::string& rhs)
      {
         std::shared_lock lock(lhs.mutex);
         return lhs.str == rhs;
      }

      // std::string == async_string
      friend bool operator==(const std::string& lhs, const async_string& rhs)
      {
         std::shared_lock lock(rhs.mutex);
         return lhs == rhs.str;
      }

      // async_string != std::string
      friend bool operator!=(const async_string& lhs, const std::string& rhs) { return !(lhs == rhs); }

      // std::string != async_string
      friend bool operator!=(const std::string& lhs, const async_string& rhs) { return !(lhs == rhs); }

      // async_string == const char*
      friend bool operator==(const async_string& lhs, const char* rhs)
      {
         std::shared_lock lock(lhs.mutex);
         return lhs.str == rhs;
      }

      // const char* == async_string
      friend bool operator==(const char* lhs, const async_string& rhs)
      {
         std::shared_lock lock(rhs.mutex);
         return lhs == rhs.str;
      }

      // async_string != const char*
      friend bool operator!=(const async_string& lhs, const char* rhs) { return !(lhs == rhs); }

      // const char* != async_string
      friend bool operator!=(const char* lhs, const async_string& rhs) { return !(lhs == rhs); }

      friend bool operator<(const async_string& lhs, const async_string& rhs)
      {
         std::scoped_lock lock{lhs.mutex, rhs.mutex};
         return lhs.str < rhs.str;
      }

      friend bool operator<=(const async_string& lhs, const async_string& rhs) { return !(rhs < lhs); }

      friend bool operator>(const async_string& lhs, const async_string& rhs) { return rhs < lhs; }

      friend bool operator>=(const async_string& lhs, const async_string& rhs) { return !(lhs < rhs); }

      void swap(async_string& other)
      {
         if (this == &other) return;
         std::scoped_lock lock{mutex, other.mutex};
         str.swap(other.str);
      }

      friend void swap(async_string& lhs, async_string& rhs) { lhs.swap(rhs); }
   };

}

namespace glz
{
   template <uint32_t Format>
   struct from<Format, glz::async_string>
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end) noexcept
      {
         auto proxy = value.write();
         parse<Format>::template op<Opts>(*proxy, ctx, it, end);
      }
   };

   template <uint32_t Format>
   struct to<Format, glz::async_string>
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&&... args) noexcept
      {
         auto proxy = value.read();
         serialize<Format>::template op<Opts>(*proxy, ctx, args...);
      }
   };
}

// Allow formatting via std::format
#ifdef __cpp_lib_format
#include <format>
namespace std
{
   template <>
   struct formatter<glz::async_string>
   {
      std::formatter<std::string> formatter;

      constexpr auto parse(format_parse_context& ctx) -> decltype(ctx.begin()) { return formatter.parse(ctx); }

      template <class FormatContext>
      auto format(const glz::async_string& s, FormatContext& ctx) const -> decltype(ctx.out())
      {
         return formatter.format(*s.read(), ctx);
      }
   };
}
#endif
