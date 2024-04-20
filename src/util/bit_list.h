#ifndef MARLON_UTIL_BIT_LIST_H
#define MARLON_UTIL_BIT_LIST_H

// #include "list.h"

#include <cstddef>
#include <cstdint>

#include <limits>
#include <span>

#include "capacity_error.h"
#include "memory.h"

namespace marlon {
namespace util {
class Bit_list {
public:
  static constexpr std::size_t
  memory_requirement(std::size_t max_size) noexcept {
    return (max_size + 63) / 64 * sizeof(std::uint64_t);
  }

  constexpr Bit_list() noexcept = default;

  explicit Bit_list(Block block, std::size_t max_size) noexcept
      : Bit_list{block.begin, max_size} {}

  explicit Bit_list(void *block, std::size_t max_size) noexcept
      : _data{static_cast<std::uint64_t *>(block), (max_size + 63) / 64} {}

  Bit_list(Bit_list const &other) = delete;

  Bit_list &operator=(Bit_list const &other) = delete;

  constexpr Bit_list(Bit_list &&other) noexcept
      : _data{std::exchange(other._data, std::span<std::uint64_t>{})},
        _size{std::exchange(other._size, std::size_t{})} {}

  constexpr Bit_list &operator=(Bit_list &&other) noexcept {
    auto temp{std::move(other)};
    swap(temp);
    return *this;
  }

  constexpr bool get(std::size_t index) const noexcept {
    auto const n = index >> 6;
    auto const m = index & 63;
    return _data[n] >> m & 1;
  }

  constexpr void set(std::size_t index, bool value) noexcept {
    if (value) {
      set(index);
    } else {
      reset(index);
    }
  }

  constexpr void set(std::size_t index) noexcept {
    auto const n = index >> 6;
    auto const m = index & 63;
    _data[n] |= std::uint64_t{1} << m;
  }

  constexpr void set() noexcept {
    for (auto &word : _data) {
      word = std::numeric_limits<std::uint64_t>::max();
    }
  }

  constexpr void reset(std::size_t index) noexcept {
    auto const n = index >> 6;
    auto const m = index & 63;
    _data[n] &= ~(std::uint64_t{1} << m);
  }

  constexpr void reset() noexcept {
    for (auto &word : _data) {
      word = std::uint64_t{};
    }
  }

  constexpr void flip(std::size_t index) noexcept {
    auto const n = index >> 6;
    auto const m = index & 63;
    _data[n] ^= std::uint64_t{1} << m;
  }

  constexpr bool empty() const noexcept { return size() == 0; }

  constexpr std::size_t size() const noexcept { return _size; }

  constexpr std::size_t max_size() const noexcept { return _data.size() << 6; }

  constexpr std::size_t capacity() const noexcept { return max_size(); }

  void clear() noexcept { _size = 0; }

  void push_back(bool value) {
    if (size() < max_size()) {
      auto const n = _size >> 6;
      auto const m = _size & 63;
      if (m == 0) {
        new (&_data[n]) std::uint64_t{};
      }
      set(_size++, value);
    } else {
      throw Capacity_error{"Capacity_error in Bit_list::push_back"};
    }
  }

  void pop_back() noexcept { --_size; }

  void resize(std::size_t count) {
    if (max_size() < count) {
      throw Capacity_error{"Capacity_error in Bit_list::resize"};
    }
    if (_size < count) {
      if (_size >> 6 == count >> 6) {
        do {
          push_back(false);
        } while (_size < count);
      } else {
        if ((_size & 63) != 0) {
          _data[_size >> 6] &= (std::uint64_t{1} << (_size & 63)) - 1;
          _size += 64 - (_size & 63);
        }
        while (_size >> 6 < count >> 6) {
          new (&_data[_size >> 6]) std::uint64_t{};
          _size += 64;
        }
        while (size() < count) {
          push_back(false);
        }
      }
    } else {
      _size = count;
    }
  }

private:
  constexpr void swap(Bit_list &other) noexcept {
    std::swap(_data, other._data);
    std::swap(_size, other._size);
  }

  std::span<std::uint64_t> _data;
  std::size_t _size{};
};

template <typename Allocator>
std::pair<Block, Bit_list> make_bit_list(Allocator &allocator,
                                         std::size_t max_size) {
  auto const block = allocator.alloc(Bit_list::memory_requirement(max_size));
  return {block, Bit_list{block, max_size}};
}
} // namespace util
} // namespace marlon

#endif