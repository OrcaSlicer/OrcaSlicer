// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

// RFC 6902 JSON Patch implementation
// https://datatracker.ietf.org/doc/html/rfc6902

#include <charconv>
#include <optional>

#include "glaze/json/generic.hpp"

namespace glz
{
   // RFC 6902 operation types
   enum struct patch_op_type : uint8_t { add, remove, replace, move, copy, test };

   // Single patch operation
   // Uses std::optional for fields that are only present for certain operations:
   // - value: required for add, replace, test
   // - from: required for move, copy
   struct patch_op
   {
      patch_op_type op{};
      std::string path{};
      std::optional<generic> value{};
      std::optional<std::string> from{};
   };

   // A patch document is an array of operations
   using patch_document = std::vector<patch_op>;

   // Options for diff generation
   struct diff_opts
   {
      // Generate move operations when a value is removed and added elsewhere
      // Default: false (only generates add/remove/replace like nlohmann)
      bool detect_moves = false;

      // Generate copy operations when identical values appear in target
      // Default: false
      bool detect_copies = false;

      // For arrays: use LCS (longest common subsequence) for smarter diffs
      // Default: false (simpler index-based comparison)
      bool array_lcs = false;
   };

   // Options for patch application
   struct patch_opts
   {
      // If true, create intermediate objects/arrays for add operations
      // Default: false (RFC 6902 compliant - parent must exist)
      bool create_intermediate = false;

      // If true, rollback all changes on any operation failure
      // Default: true (atomic application)
      // Note: Requires O(n) space and time for backup copy of large documents
      bool atomic = true;
   };
}

template <>
struct glz::meta<glz::patch_op_type>
{
   using enum glz::patch_op_type;
   static constexpr auto value = enumerate(add, remove, replace, move, copy, test);
};

namespace glz
{
   // ============================================================================
   // Helper Functions
   // ============================================================================

   // Escape special characters in JSON Pointer tokens (RFC 6901)
   [[nodiscard]] inline std::string escape_json_ptr(std::string_view token)
   {
      std::string result;
      result.reserve(token.size());
      for (char c : token) {
         if (c == '~')
            result += "~0";
         else if (c == '/')
            result += "~1";
         else
            result += c;
      }
      return result;
   }

   // Unescape JSON Pointer tokens (RFC 6901)
   // Returns error for malformed sequences (e.g., "~" at end, "~2")
   [[nodiscard]] inline expected<std::string, error_ctx> unescape_json_ptr(std::string_view token)
   {
      std::string result;
      result.reserve(token.size());
      for (size_t i = 0; i < token.size(); ++i) {
         if (token[i] == '~') {
            if (i + 1 >= token.size()) {
               return unexpected(error_ctx{0, error_code::invalid_json_pointer});
            }
            if (token[i + 1] == '0') {
               result += '~';
               ++i;
            }
            else if (token[i + 1] == '1') {
               result += '/';
               ++i;
            }
            else {
               return unexpected(error_ctx{0, error_code::invalid_json_pointer});
            }
         }
         else {
            result += token[i];
         }
      }
      return result;
   }

   // Deep equality comparison for generic values
   [[nodiscard]] inline bool equal(const generic& a, const generic& b) noexcept
   {
      if (a.data.index() != b.data.index()) {
         return false;
      }

      // After index check, we know both variants hold the same type
      return std::visit(
         [&b](const auto& val_a) -> bool {
            using T = std::decay_t<decltype(val_a)>;
            const auto& val_b = std::get<T>(b.data); // Safe: index check guarantees same type

            if constexpr (std::same_as<T, std::nullptr_t>) {
               return true;
            }
            else if constexpr (std::same_as<T, generic::array_t>) {
               if (val_a.size() != val_b.size()) return false;
               for (size_t i = 0; i < val_a.size(); ++i) {
                  if (!equal(val_a[i], val_b[i])) return false;
               }
               return true;
            }
            else if constexpr (std::same_as<T, generic::object_t>) {
               if (val_a.size() != val_b.size()) return false;
               for (const auto& [key, value] : val_a) {
                  auto it = val_b.find(key);
                  if (it == val_b.end()) return false;
                  if (!equal(value, it->second)) return false;
               }
               return true;
            }
            else {
               return val_a == val_b;
            }
         },
         a.data);
   }

   namespace detail
   {
      // Parse a JSON pointer path into parent path and final token
      // Returns {parent_path, token} or error
      [[nodiscard]] inline expected<std::pair<std::string_view, std::string>, error_ctx> split_path(
         std::string_view path)
      {
         if (path.empty()) {
            return std::pair<std::string_view, std::string>{"", ""};
         }

         if (path[0] != '/') {
            return unexpected(error_ctx{0, error_code::invalid_json_pointer});
         }

         auto last_slash = path.rfind('/');
         std::string_view parent = path.substr(0, last_slash);
         std::string_view token_escaped = path.substr(last_slash + 1);

         auto token = unescape_json_ptr(token_escaped);
         if (!token) {
            return unexpected(token.error());
         }

         return std::pair<std::string_view, std::string>{parent, std::move(*token)};
      }

      // Parse array index from string, returns nullopt for "-" (append) or invalid
      [[nodiscard]] inline std::optional<size_t> parse_array_index(std::string_view token)
      {
         if (token.empty()) return std::nullopt;

         // "-" means append to end
         if (token == "-") return std::nullopt;

         // Leading zeros are not allowed (except "0" itself)
         if (token.size() > 1 && token[0] == '0') return std::nullopt;

         size_t index = 0;
         auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), index);
         if (ec != std::errc{} || ptr != token.data() + token.size()) {
            return std::nullopt;
         }

         return index;
      }
   } // namespace detail

   // Navigate to parent of the target path and return the final key/index
   // Examples:
   //   path=""       → error (root has no parent)
   //   path="/foo"   → returns {&root, "foo"} (root is parent of top-level keys)
   //   path="/a/b"   → returns {&root["a"], "b"}
   [[nodiscard]] inline expected<std::pair<generic*, std::string>, error_ctx> navigate_to_parent(generic& root,
                                                                                                 std::string_view path)
   {
      if (path.empty()) {
         // Empty path refers to root itself, which has no parent
         return unexpected(error_ctx{0, error_code::nonexistent_json_ptr});
      }

      auto split = detail::split_path(path);
      if (!split) {
         return unexpected(split.error());
      }

      auto [parent_path, token] = *split;

      // Determine the parent:
      // - If parent_path is empty (e.g., path="/foo"), the parent is root
      // - Otherwise, navigate to parent_path (e.g., path="/a/b" → navigate to "/a")
      generic* parent = nullptr;
      if (parent_path.empty()) {
         parent = &root;
      }
      else {
         parent = navigate_to(&root, parent_path);
      }

      if (!parent) {
         return unexpected(error_ctx{0, error_code::nonexistent_json_ptr});
      }

      return std::pair<generic*, std::string>{parent, std::move(token)};
   }

   // Insert a value at a JSON Pointer path
   // If create_intermediate is true, creates intermediate objects as needed (like mkdir -p)
   [[nodiscard]] inline error_ctx insert_at(generic& root, std::string_view path, generic value,
                                            bool create_intermediate = false)
   {
      // Empty path means replace root
      if (path.empty()) {
         root = std::move(value);
         return {};
      }

      auto nav = navigate_to_parent(root, path);
      if (!nav) {
         if (!create_intermediate) {
            return nav.error();
         }

         // Create intermediate objects along the path
         // Parse path into segments and create missing intermediate objects
         if (path.empty() || path[0] != '/') {
            return error_ctx{0, error_code::invalid_json_pointer};
         }

         generic* current = &root;
         std::string_view remaining = path.substr(1); // Skip leading '/'

         while (!remaining.empty()) {
            // Find next '/' or end
            size_t slash_pos = remaining.find('/');
            std::string_view segment_escaped =
               (slash_pos == std::string_view::npos) ? remaining : remaining.substr(0, slash_pos);

            auto segment = unescape_json_ptr(segment_escaped);
            if (!segment) {
               return segment.error();
            }

            bool is_last_segment = (slash_pos == std::string_view::npos);

            if (is_last_segment) {
               // Final segment - insert the value
               if (current->is_null()) {
                  current->data = generic::object_t{};
               }
               if (current->is_object()) {
                  current->get_object()[*segment] = std::move(value);
                  return {};
               }
               else if (current->is_array()) {
                  auto& arr = current->get_array();
                  if (*segment == "-") {
                     arr.push_back(std::move(value));
                     return {};
                  }
                  auto index_opt = detail::parse_array_index(*segment);
                  if (!index_opt || *index_opt > arr.size()) {
                     return error_ctx{0, error_code::nonexistent_json_ptr};
                  }
                  arr.insert(arr.begin() + static_cast<ptrdiff_t>(*index_opt), std::move(value));
                  return {};
               }
               else {
                  return error_ctx{0, error_code::nonexistent_json_ptr};
               }
            }
            else {
               // Intermediate segment - navigate or create
               if (current->is_null()) {
                  current->data = generic::object_t{};
               }
               if (current->is_object()) {
                  auto& obj = current->get_object();
                  auto it = obj.find(*segment);
                  if (it == obj.end()) {
                     // Create intermediate object
                     // Note: Use emplace + assignment to avoid brace init creating an array
                     auto [new_it, inserted] = obj.emplace(*segment, generic{});
                     new_it->second.data = generic::object_t{};
                     it = new_it;
                  }
                  current = &it->second;
               }
               else if (current->is_array()) {
                  auto index_opt = detail::parse_array_index(*segment);
                  if (!index_opt) {
                     return error_ctx{0, error_code::nonexistent_json_ptr};
                  }
                  auto& arr = current->get_array();
                  if (*index_opt >= arr.size()) {
                     return error_ctx{0, error_code::nonexistent_json_ptr};
                  }
                  current = &arr[*index_opt];
               }
               else {
                  return error_ctx{0, error_code::nonexistent_json_ptr};
               }

               remaining = remaining.substr(slash_pos + 1);
            }
         }

         return {};
      }

      auto [parent, token] = *nav;

      if (parent->is_object()) {
         auto& obj = parent->get_object();
         obj[token] = std::move(value);
         return {};
      }
      else if (parent->is_array()) {
         auto& arr = parent->get_array();

         // "-" means append
         if (token == "-") {
            arr.push_back(std::move(value));
            return {};
         }

         auto index_opt = detail::parse_array_index(token);
         if (!index_opt) {
            return error_ctx{0, error_code::nonexistent_json_ptr};
         }

         size_t index = *index_opt;
         if (index > arr.size()) {
            return error_ctx{0, error_code::nonexistent_json_ptr};
         }

         arr.insert(arr.begin() + static_cast<ptrdiff_t>(index), std::move(value));
         return {};
      }
      else {
         return error_ctx{0, error_code::nonexistent_json_ptr};
      }
   }

   // Remove a value at a JSON Pointer path
   [[nodiscard]] inline expected<generic, error_ctx> remove_at(generic& root, std::string_view path)
   {
      if (path.empty()) {
         // Cannot remove root
         return unexpected(error_ctx{0, error_code::nonexistent_json_ptr});
      }

      auto nav = navigate_to_parent(root, path);
      if (!nav) {
         return unexpected(nav.error());
      }

      auto [parent, token] = *nav;

      if (parent->is_object()) {
         auto& obj = parent->get_object();
         auto it = obj.find(token);
         if (it == obj.end()) {
            return unexpected(error_ctx{0, error_code::nonexistent_json_ptr});
         }
         generic removed = std::move(it->second);
         obj.erase(it);
         return removed;
      }
      else if (parent->is_array()) {
         auto& arr = parent->get_array();

         auto index_opt = detail::parse_array_index(token);
         if (!index_opt) {
            return unexpected(error_ctx{0, error_code::nonexistent_json_ptr});
         }

         size_t index = *index_opt;
         if (index >= arr.size()) {
            return unexpected(error_ctx{0, error_code::nonexistent_json_ptr});
         }

         generic removed = std::move(arr[index]);
         arr.erase(arr.begin() + static_cast<ptrdiff_t>(index));
         return removed;
      }
      else {
         return unexpected(error_ctx{0, error_code::nonexistent_json_ptr});
      }
   }

   // ============================================================================
   // Patch Operations
   // ============================================================================

   namespace detail
   {
      // Add operation
      [[nodiscard]] inline error_ctx apply_add(generic& doc, std::string_view path, const generic& value,
                                               const patch_opts& opts)
      {
         return insert_at(doc, path, value, opts.create_intermediate);
      }

      // Remove operation
      [[nodiscard]] inline error_ctx apply_remove(generic& doc, std::string_view path)
      {
         auto result = remove_at(doc, path);
         if (!result) {
            return result.error();
         }
         return {};
      }

      // Replace operation
      [[nodiscard]] inline error_ctx apply_replace(generic& doc, std::string_view path, const generic& value)
      {
         if (path.empty()) {
            doc = value;
            return {};
         }

         generic* target = navigate_to(&doc, path);
         if (!target) {
            return error_ctx{0, error_code::nonexistent_json_ptr};
         }

         *target = value;
         return {};
      }

      // Move operation
      [[nodiscard]] inline error_ctx apply_move(generic& doc, std::string_view from, std::string_view path,
                                                const patch_opts& opts)
      {
         // Check for move into self (path cannot start with from)
         if (path.starts_with(from) && (path.size() == from.size() || path[from.size()] == '/')) {
            return error_ctx{0, error_code::syntax_error};
         }

         // Remove from source
         auto removed = remove_at(doc, from);
         if (!removed) {
            return error_ctx{0, error_code::nonexistent_json_ptr};
         }

         // Add to destination
         return insert_at(doc, path, std::move(*removed), opts.create_intermediate);
      }

      // Copy operation
      [[nodiscard]] inline error_ctx apply_copy(generic& doc, std::string_view from, std::string_view path,
                                                const patch_opts& opts)
      {
         const generic* source = navigate_to(&doc, from);
         if (!source) {
            return error_ctx{0, error_code::nonexistent_json_ptr};
         }

         return insert_at(doc, path, *source, opts.create_intermediate);
      }

      // Test operation
      [[nodiscard]] inline error_ctx apply_test(const generic& doc, std::string_view path, const generic& expected)
      {
         if (path.empty()) {
            if (!equal(doc, expected)) {
               return error_ctx{0, error_code::patch_test_failed};
            }
            return {};
         }

         const generic* target = navigate_to(&doc, path);
         if (!target) {
            return error_ctx{0, error_code::nonexistent_json_ptr};
         }

         if (!equal(*target, expected)) {
            return error_ctx{0, error_code::patch_test_failed};
         }

         return {};
      }

      // Apply a single patch operation
      [[nodiscard]] inline error_ctx apply_operation(generic& doc, const patch_op& op, const patch_opts& opts)
      {
         switch (op.op) {
         case patch_op_type::add:
            if (!op.value) {
               return error_ctx{0, error_code::missing_key};
            }
            return apply_add(doc, op.path, *op.value, opts);

         case patch_op_type::remove:
            return apply_remove(doc, op.path);

         case patch_op_type::replace:
            if (!op.value) {
               return error_ctx{0, error_code::missing_key};
            }
            return apply_replace(doc, op.path, *op.value);

         case patch_op_type::move:
            if (!op.from) {
               return error_ctx{0, error_code::missing_key};
            }
            return apply_move(doc, *op.from, op.path, opts);

         case patch_op_type::copy:
            if (!op.from) {
               return error_ctx{0, error_code::missing_key};
            }
            return apply_copy(doc, *op.from, op.path, opts);

         case patch_op_type::test:
            if (!op.value) {
               return error_ctx{0, error_code::missing_key};
            }
            return apply_test(doc, op.path, *op.value);

         default:
            return error_ctx{0, error_code::syntax_error};
         }
      }
   } // namespace detail

   // ============================================================================
   // Diff Algorithm
   // ============================================================================

   namespace detail
   {
      // Note: diff_opts (detect_moves, detect_copies, array_lcs) are reserved for future implementation.
      // Currently only generates add/remove/replace operations (like nlohmann/json default behavior).
      inline void diff_impl(const generic& source, const generic& target, const std::string& path, patch_document& ops,
                            [[maybe_unused]] const diff_opts& opts)
      {
         // Different types -> replace
         if (source.data.index() != target.data.index()) {
            ops.push_back({patch_op_type::replace, path, target, std::nullopt});
            return;
         }

         std::visit(
            [&](const auto& src_val) {
               using T = std::decay_t<decltype(src_val)>;

               if constexpr (std::same_as<T, std::nullptr_t>) {
                  // Both null, nothing to do
               }
               else if constexpr (std::same_as<T, double> || std::same_as<T, std::string> || std::same_as<T, bool>) {
                  const auto& tgt_val = std::get<T>(target.data);
                  if (src_val != tgt_val) {
                     ops.push_back({patch_op_type::replace, path, target, std::nullopt});
                  }
               }
               else if constexpr (std::same_as<T, generic::object_t>) {
                  const auto& src_obj = src_val;
                  const auto& tgt_obj = std::get<generic::object_t>(target.data);

                  // Keys removed from source
                  for (const auto& [key, value] : src_obj) {
                     if (tgt_obj.find(key) == tgt_obj.end()) {
                        ops.push_back(
                           {patch_op_type::remove, path + "/" + escape_json_ptr(key), std::nullopt, std::nullopt});
                     }
                  }

                  // Keys added or modified
                  for (const auto& [key, tgt_value] : tgt_obj) {
                     std::string child_path = path + "/" + escape_json_ptr(key);
                     auto it = src_obj.find(key);
                     if (it == src_obj.end()) {
                        ops.push_back({patch_op_type::add, child_path, tgt_value, std::nullopt});
                     }
                     else {
                        diff_impl(it->second, tgt_value, child_path, ops, opts);
                     }
                  }
               }
               else if constexpr (std::same_as<T, generic::array_t>) {
                  const auto& src_arr = src_val;
                  const auto& tgt_arr = std::get<generic::array_t>(target.data);

                  size_t min_len = std::min(src_arr.size(), tgt_arr.size());

                  // Compare common elements
                  for (size_t i = 0; i < min_len; ++i) {
                     std::string child_path = path + "/" + std::to_string(i);
                     diff_impl(src_arr[i], tgt_arr[i], child_path, ops, opts);
                  }

                  // Target has more elements -> add
                  if (tgt_arr.size() > src_arr.size()) {
                     for (size_t i = min_len; i < tgt_arr.size(); ++i) {
                        std::string child_path = path + "/" + std::to_string(i);
                        ops.push_back({patch_op_type::add, child_path, tgt_arr[i], std::nullopt});
                     }
                  }
                  // Source has more elements -> remove from end to start
                  // IMPORTANT: We iterate backward (highest index first) so that when
                  // the patch is applied, removing element N doesn't shift the indices
                  // of elements we still need to remove (N-1, N-2, etc.)
                  else if (src_arr.size() > tgt_arr.size()) {
                     for (size_t i = src_arr.size(); i > min_len; --i) {
                        std::string child_path = path + "/" + std::to_string(i - 1);
                        ops.push_back({patch_op_type::remove, child_path, std::nullopt, std::nullopt});
                     }
                  }
               }
            },
            source.data);
      }
   } // namespace detail

   // ============================================================================
   // Main API Functions
   // ============================================================================

   // Generate a patch document that transforms 'source' into 'target'
   [[nodiscard]] inline expected<patch_document, error_ctx> diff(const generic& source, const generic& target,
                                                                 diff_opts opts = {})
   {
      patch_document ops;
      detail::diff_impl(source, target, "", ops, opts);
      return ops;
   }

   // Apply a patch document to a JSON value (in-place modification)
   [[nodiscard]] inline error_ctx patch(generic& document, const patch_document& ops, patch_opts opts = {})
   {
      if (opts.atomic) {
         // Deep copy for rollback
         generic backup = document;

         for (const auto& op : ops) {
            auto ec = detail::apply_operation(document, op, opts);
            if (ec) {
               document = std::move(backup); // Rollback
               return ec;
            }
         }
      }
      else {
         for (const auto& op : ops) {
            auto ec = detail::apply_operation(document, op, opts);
            if (ec) {
               return ec;
            }
         }
      }
      return {};
   }

   // Apply a patch document, returning a new value (non-mutating)
   [[nodiscard]] inline expected<generic, error_ctx> patched(const generic& document, const patch_document& ops,
                                                             patch_opts opts = {})
   {
      generic result = document;
      opts.atomic = false; // No need for atomic since we're working on a copy
      auto ec = patch(result, ops, opts);
      if (ec) {
         return unexpected(ec);
      }
      return result;
   }

   // Convenience overload for JSON string input
   [[nodiscard]] inline expected<patch_document, error_ctx> diff(std::string_view source_json,
                                                                 std::string_view target_json, diff_opts opts = {})
   {
      auto source = read_json<generic>(source_json);
      if (!source) {
         return unexpected(source.error());
      }

      auto target = read_json<generic>(target_json);
      if (!target) {
         return unexpected(target.error());
      }

      return diff(*source, *target, opts);
   }

   // Convenience overload for JSON string patch application
   [[nodiscard]] inline expected<std::string, error_ctx> patch_json(std::string_view document_json,
                                                                    std::string_view patch_json_str,
                                                                    patch_opts opts = {})
   {
      auto document = read_json<generic>(document_json);
      if (!document) {
         return unexpected(document.error());
      }

      auto ops = read_json<patch_document>(patch_json_str);
      if (!ops) {
         return unexpected(ops.error());
      }

      auto ec = patch(*document, *ops, opts);
      if (ec) {
         return unexpected(ec);
      }

      return write_json(*document);
   }

   // ============================================================================
   // RFC 7386 JSON Merge Patch
   // https://datatracker.ietf.org/doc/html/rfc7386
   // ============================================================================

   namespace detail
   {
      // Recursive merge patch implementation
      // Modifies target in-place according to RFC 7386 algorithm
      [[nodiscard]] inline error_ctx apply_merge_patch_impl(generic& target, const generic& patch, uint32_t depth = 0)
      {
         if (depth >= max_recursive_depth_limit) [[unlikely]] {
            return error_ctx{0, error_code::exceeded_max_recursive_depth};
         }

         if (patch.is_object()) {
            // If target is not an object, replace with empty object
            if (!target.is_object()) {
               target.data = generic::object_t{};
            }

            auto& target_obj = target.get_object();
            const auto& patch_obj = patch.get_object();

            for (const auto& [key, value] : patch_obj) {
               if (value.is_null()) {
                  // Null means remove
                  target_obj.erase(key);
               }
               else if (value.is_object()) {
                  // Recursively merge objects
                  // Use try_emplace to avoid double lookup
                  auto [it, inserted] = target_obj.try_emplace(key, generic{});
                  auto ec = apply_merge_patch_impl(it->second, value, depth + 1);
                  if (ec) {
                     return ec;
                  }
               }
               else {
                  // Non-object value - direct assignment (no recursion needed)
                  target_obj[key] = value;
               }
            }
         }
         else {
            // Non-object patch replaces target entirely
            target = patch;
         }

         return {};
      }

      // Merge diff implementation - generates a merge patch from source to target
      inline void merge_diff_impl(const generic& source, const generic& target, generic& patch)
      {
         // If types differ or source is not an object, return target as patch
         if (!source.is_object() || !target.is_object()) {
            patch = target;
            return;
         }

         // Both are objects - compute diff
         patch.data = generic::object_t{};
         auto& patch_obj = patch.get_object();
         const auto& source_obj = source.get_object();
         const auto& target_obj = target.get_object();

         // Check for removed keys (in source but not in target)
         for (const auto& [key, value] : source_obj) {
            if (target_obj.find(key) == target_obj.end()) {
               // Key was removed - emit null
               patch_obj.emplace(key, nullptr);
            }
         }

         // Check for added or modified keys
         for (const auto& [key, target_value] : target_obj) {
            auto source_it = source_obj.find(key);
            if (source_it == source_obj.end()) {
               // Key was added
               patch_obj.emplace(key, target_value);
            }
            else if (!equal(source_it->second, target_value)) {
               // Key was modified
               if (source_it->second.is_object() && target_value.is_object()) {
                  // Both objects - recurse
                  generic child_patch;
                  merge_diff_impl(source_it->second, target_value, child_patch);
                  patch_obj.emplace(key, std::move(child_patch));
               }
               else {
                  // Different types or non-objects - replace
                  patch_obj.emplace(key, target_value);
               }
            }
            // If equal, no patch entry needed
         }
      }
   } // namespace detail

   // ============================================================================
   // Merge Patch API Functions
   // ============================================================================

   // Apply a merge patch to a JSON value (in-place modification)
   // Note: Uses template parameter to prevent ambiguity with string_view overload
   // (string literals would otherwise match both const generic& and string_view)
   template <class G>
      requires(std::same_as<std::remove_cvref_t<G>, generic>)
   [[nodiscard]] inline error_ctx merge_patch(generic& target, G&& patch)
   {
      return detail::apply_merge_patch_impl(target, patch);
   }

   // Apply a merge patch, returning a new value (non-mutating)
   template <class G>
      requires(std::same_as<std::remove_cvref_t<G>, generic>)
   [[nodiscard]] inline expected<generic, error_ctx> merge_patched(const generic& target, G&& patch)
   {
      generic result = target;
      auto ec = merge_patch(result, std::forward<G>(patch));
      if (ec) {
         return unexpected(ec);
      }
      return result;
   }

   // Convenience overload: apply merge patch from JSON string
   [[nodiscard]] inline error_ctx merge_patch(generic& target, std::string_view patch_json)
   {
      auto patch = read_json<generic>(patch_json);
      if (!patch) {
         return patch.error();
      }
      return merge_patch(target, *patch);
   }

   // Convenience overload: apply merge patch from JSON strings, return JSON string
   [[nodiscard]] inline expected<std::string, error_ctx> merge_patch_json(std::string_view target_json,
                                                                          std::string_view patch_json)
   {
      auto target = read_json<generic>(target_json);
      if (!target) {
         return unexpected(target.error());
      }

      auto patch = read_json<generic>(patch_json);
      if (!patch) {
         return unexpected(patch.error());
      }

      auto ec = merge_patch(*target, *patch);
      if (ec) {
         return unexpected(ec);
      }

      return write_json(*target);
   }

   // Convenience overload: apply merge patch from JSON strings, return generic
   [[nodiscard]] inline expected<generic, error_ctx> merge_patched(std::string_view target_json,
                                                                   std::string_view patch_json)
   {
      auto target = read_json<generic>(target_json);
      if (!target) {
         return unexpected(target.error());
      }

      auto patch = read_json<generic>(patch_json);
      if (!patch) {
         return unexpected(patch.error());
      }

      auto ec = merge_patch(*target, *patch);
      if (ec) {
         return unexpected(ec);
      }

      return *target;
   }

   // Generate a merge patch that transforms 'source' into 'target'
   // Note: Due to null semantics, this cannot perfectly round-trip if
   // the target contains explicit null values (they would be interpreted as removals)
   [[nodiscard]] inline expected<generic, error_ctx> merge_diff(const generic& source, const generic& target)
   {
      generic patch;
      detail::merge_diff_impl(source, target, patch);
      return patch;
   }

   // Convenience overload: generate merge patch from JSON strings, return JSON string
   [[nodiscard]] inline expected<std::string, error_ctx> merge_diff_json(std::string_view source_json,
                                                                         std::string_view target_json)
   {
      auto source = read_json<generic>(source_json);
      if (!source) {
         return unexpected(source.error());
      }

      auto target = read_json<generic>(target_json);
      if (!target) {
         return unexpected(target.error());
      }

      auto patch = merge_diff(*source, *target);
      if (!patch) {
         return unexpected(patch.error());
      }

      return write_json(*patch);
   }

   // ============================================================================
   // Struct/Type-based Merge Patch API
   // ============================================================================

   // Concept to check if a type is not generic (to avoid ambiguous overloads)
   template <class T>
   concept not_generic = !std::same_as<std::decay_t<T>, generic>;

   // Concept to check if a type is a struct/class suitable for merge patching
   // (excludes generic and string-like types to avoid ambiguous overloads)
   template <class T>
   concept merge_patch_struct =
      not_generic<T> && !std::convertible_to<T, std::string_view> && !std::is_array_v<std::remove_cvref_t<T>>;

   // Apply a merge patch to a C++ struct/type (in-place modification)
   // Glaze's read_json already has merge-patch semantics: it only updates fields
   // present in the source and leaves other fields unchanged.
   // Note: Uses template parameter G constrained to generic to prevent implicit conversion
   // from string literals (which would cause ambiguity with the string_view overload)
   template <class T, class G>
      requires(merge_patch_struct<T> && std::same_as<std::remove_cvref_t<G>, generic>)
   [[nodiscard]] inline error_ctx merge_patch(T& target, G&& patch)
   {
      // Glaze can read directly from generic into structs, updating only fields present
      return read_json(target, patch);
   }

   // Apply a merge patch from JSON string to a C++ struct/type
   // Glaze's read_json already has merge-patch semantics: it only updates fields
   // present in the JSON and leaves other fields unchanged.
   template <class T>
      requires(merge_patch_struct<T>)
   [[nodiscard]] inline error_ctx merge_patch(T& target, std::string_view patch_json)
   {
      // Direct read - Glaze only updates fields present in the JSON
      return read_json(target, patch_json);
   }

   // Apply a merge patch, returning a new struct (non-mutating)
   // Note: Uses template parameter G constrained to generic to prevent implicit conversion
   // from string literals (which would cause ambiguity with the string_view overload)
   template <class T, class G>
      requires(merge_patch_struct<T> && std::is_default_constructible_v<T> &&
               std::same_as<std::remove_cvref_t<G>, generic>)
   [[nodiscard]] inline expected<T, error_ctx> merge_patched(const T& target, G&& patch)
   {
      T result = target;
      auto ec = merge_patch(result, std::forward<G>(patch));
      if (ec) {
         return unexpected(ec);
      }
      return result;
   }

   // Apply a merge patch from JSON string, returning a new struct (non-mutating)
   template <class T>
      requires(merge_patch_struct<T> && std::is_default_constructible_v<T>)
   [[nodiscard]] inline expected<T, error_ctx> merge_patched(const T& target, std::string_view patch_json)
   {
      T result = target;
      auto ec = read_json(result, patch_json);
      if (ec) {
         return unexpected(ec);
      }
      return result;
   }

   // Generate a merge patch that transforms one struct into another
   template <class T>
      requires(merge_patch_struct<T>)
   [[nodiscard]] inline expected<generic, error_ctx> merge_diff(const T& source, const T& target)
   {
      // Serialize both to JSON then to generic
      auto source_json = write_json(source);
      if (!source_json) {
         return unexpected(source_json.error());
      }
      auto source_generic = read_json<generic>(*source_json);
      if (!source_generic) {
         return unexpected(source_generic.error());
      }

      auto target_json = write_json(target);
      if (!target_json) {
         return unexpected(target_json.error());
      }
      auto target_generic = read_json<generic>(*target_json);
      if (!target_generic) {
         return unexpected(target_generic.error());
      }

      return merge_diff(*source_generic, *target_generic);
   }

   // Generate a merge patch between two structs, return as JSON string
   template <class T>
      requires(merge_patch_struct<T>)
   [[nodiscard]] inline expected<std::string, error_ctx> merge_diff_json(const T& source, const T& target)
   {
      auto patch = merge_diff(source, target);
      if (!patch) {
         return unexpected(patch.error());
      }
      return write_json(*patch);
   }
}
