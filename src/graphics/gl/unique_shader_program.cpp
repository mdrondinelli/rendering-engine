#include "unique_shader_program.h"

#include <glad/glad.h>

namespace marlon {
namespace graphics {
Gl_unique_shader_program_handle::~Gl_unique_shader_program_handle() {
  glDeleteProgram(_handle);
}

Gl_unique_shader_program_handle gl_make_unique_shader_program() {
  return Gl_unique_shader_program_handle{glCreateProgram()};
}
} // namespace graphics
} // namespace marlon