#ifndef MARLON_RENDERING_GL_SHADER_H
#define MARLON_RENDERING_GL_SHADER_H

#include <cstdint>

#include <utility>

#include "default_handle_init.h"

namespace marlon {
namespace rendering {
class Gl_shader {
public:
  Gl_shader() noexcept : _handle{0} {}

  explicit Gl_shader(std::uint32_t type);

  ~Gl_shader();

  Gl_shader(Gl_shader &&other) noexcept
      : _handle{std::exchange(other._handle, 0)} {}

  Gl_shader &operator=(Gl_shader &&other) noexcept {
    auto temp{std::move(other)};
    swap(temp);
    return *this;
  }

  std::uint32_t handle() const noexcept { return _handle; }

private:
  void swap(Gl_shader &other) noexcept { std::swap(_handle, other._handle); }

  std::uint32_t _handle;
};
} // namespace rendering
} // namespace marlon

#endif