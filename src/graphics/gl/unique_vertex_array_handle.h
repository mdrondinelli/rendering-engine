#ifndef MARLON_GRAPHICS_GL_UNIQUE_VERTEX_ARRAY_H
#define MARLON_GRAPHICS_GL_UNIQUE_VERTEX_ARRAY_H

#include <cstdint>

#include <utility>

namespace marlon {
namespace graphics {
class Gl_unique_vertex_array_handle {
public:
  constexpr Gl_unique_vertex_array_handle() noexcept : _handle{0} {}

  constexpr explicit Gl_unique_vertex_array_handle(
      std::uint32_t handle) noexcept
      : _handle{handle} {}

  ~Gl_unique_vertex_array_handle();

  Gl_unique_vertex_array_handle(Gl_unique_vertex_array_handle &&other) noexcept
      : _handle{std::exchange(other._handle, 0)} {}

  Gl_unique_vertex_array_handle &
  operator=(Gl_unique_vertex_array_handle &&other) noexcept {
    auto temp{std::move(other)};
    swap(temp);
  }

  std::uint32_t get() const noexcept { return _handle; }

private:
  void swap(Gl_unique_vertex_array_handle &other) noexcept {
    std::swap(_handle, other._handle);
  }

  std::uint32_t _handle;
};

Gl_unique_vertex_array_handle gl_make_unique_vertex_array();
} // namespace graphics
} // namespace marlon

#endif