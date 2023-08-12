#include "graphics.h"

#include <cassert>

#include <array>
#include <stdexcept>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
#include <glad/glad.h>
#pragma clang diagnostic pop
#else
#include <glad/glad.h>
#endif

#include "unique_shader_handle.h"

namespace marlon {
namespace graphics {
namespace {
constexpr auto vert_src = R"(#version 460 core
layout(location = 0) in vec3 model_space_position;
layout(location = 1) in vec2 texcoord;

out Vertex_data {
  vec3 view_space_position;
  vec2 texcoord;
} vertex_data;

layout(location = 0) uniform mat4 model_view_matrix;
layout(location = 1) uniform mat4 model_view_clip_matrix;

void main() {
  vertex_data.view_space_position = (model_view_matrix * vec4(model_space_position, 1.0)).xyz;
  vertex_data.texcoord = texcoord;
  gl_Position = model_view_clip_matrix * vec4(model_space_position, 1.0);
}
)";

constexpr auto frag_src = R"(#version 460 core
in Vertex_data {
  vec3 view_space_position;
  vec2 texcoord;
} vertex_data;

layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2D base_color_texture;
layout(location = 2) uniform vec3 base_color_tint;

float luminance(vec3 v) {
  return dot(v, vec3(0.2126, 0.7152, 0.0722));
}

vec3 tonemap(vec3 v) {
  float l = luminance(v);
  vec3 tv = v / (v + vec3(1.0));
  return mix(v / (l + vec3(1.0)), tv, tv);
}

void main() {
  vec3 base_color = texture(base_color_texture, vertex_data.texcoord).rgb * base_color_tint;
  fragColor = vec4(base_color, 1.0);
}
)";
} // namespace

Gl_graphics::Gl_graphics()
    : _default_render_target{std::make_unique<Gl_default_render_target>()},
      _shader_program{gl_make_unique_shader_program()},
      _default_base_color_texture{gl_make_unique_texture(GL_TEXTURE_2D)} {
  GLint status;
  auto const vertex_shader{gl_make_unique_shader(GL_VERTEX_SHADER)};
  glShaderSource(vertex_shader.get(), 1, &vert_src, nullptr);
  glCompileShader(vertex_shader.get());
  glGetShaderiv(vertex_shader.get(), GL_COMPILE_STATUS, &status);
  if (status == GL_FALSE) {
    GLint log_size = 0;
    glGetShaderiv(vertex_shader.get(), GL_INFO_LOG_LENGTH, &log_size);
    std::vector<char> log;
    log.resize(log_size);
    glGetShaderInfoLog(vertex_shader.get(), log_size, nullptr, log.data());
    throw std::runtime_error{log.data()};
  }
  auto const fragment_shader{gl_make_unique_shader(GL_FRAGMENT_SHADER)};
  glShaderSource(fragment_shader.get(), 1, &frag_src, nullptr);
  glCompileShader(fragment_shader.get());
  glGetShaderiv(fragment_shader.get(), GL_COMPILE_STATUS, &status);
  if (status == GL_FALSE) {
    GLint log_size;
    glGetShaderiv(fragment_shader.get(), GL_INFO_LOG_LENGTH, &log_size);
    std::vector<char> log;
    log.resize(log_size);
    glGetShaderInfoLog(fragment_shader.get(), log_size, nullptr, log.data());
    throw std::runtime_error{log.data()};
  }
  glAttachShader(_shader_program.get(), vertex_shader.get());
  glAttachShader(_shader_program.get(), fragment_shader.get());
  glLinkProgram(_shader_program.get());
  glDetachShader(_shader_program.get(), vertex_shader.get());
  glDetachShader(_shader_program.get(), fragment_shader.get());
  glGetProgramiv(_shader_program.get(), GL_LINK_STATUS, &status);
  if (status == GL_FALSE) {
    GLint log_size;
    glGetProgramiv(_shader_program.get(), GL_INFO_LOG_LENGTH, &log_size);
    std::vector<char> log;
    log.resize(log_size);
    glGetProgramInfoLog(_shader_program.get(), log_size, nullptr, log.data());
    throw std::runtime_error{log.data()};
  }
  auto const default_base_color_texture_pixels =
      std::array<std::uint8_t, 4>{0xFF, 0xFF, 0xFF, 0xFF};
  glTextureStorage2D(_default_base_color_texture.get(), 1, GL_RGBA8, 1, 1);
  glTextureSubImage2D(_default_base_color_texture.get(), 0, 0, 0, 1, 1, GL_RGBA,
                      GL_UNSIGNED_BYTE,
                      default_base_color_texture_pixels.data());
}

Gl_texture *
Gl_graphics::create_texture(Texture_create_info const &create_info) {
  return new Gl_texture{create_info};
}

void Gl_graphics::destroy_texture(Texture *texture) noexcept {
  delete static_cast<Gl_texture *>(texture);
}

// Gl_material *
// Gl_graphics::create_material(Material_create_info const &create_info) {
//   return new Gl_material{create_info};
// }

// void Gl_graphics::destroy_material(Material *material) noexcept {
//   delete static_cast<Gl_material *>(material);
// }

Gl_mesh *Gl_graphics::create_mesh(Mesh_create_info const &create_info) {
  return new Gl_mesh{create_info};
}

void Gl_graphics::destroy_mesh(Mesh *mesh) noexcept {
  delete static_cast<Gl_mesh *>(mesh);
}

// Gl_surface *
// Gl_graphics::create_surface(Surface_create_info const &create_info) {
//   return new Gl_surface{create_info};
// }

// void Gl_graphics::destroy_surface(Surface *surface) noexcept {
//   delete static_cast<Gl_surface *>(surface);
// }

Gl_scene *Gl_graphics::create_scene(Scene_create_info const &create_info) {
  return new Gl_scene{create_info};
}

void Gl_graphics::destroy_scene(Scene *scene) noexcept {
  delete static_cast<Gl_scene *>(scene);
}

Gl_scene_diff *
Gl_graphics::create_scene_diff(Scene_diff_create_info const &create_info) {
  return new Gl_scene_diff{create_info};
}

void Gl_graphics::destroy_scene_diff(Scene_diff *scene_diff) noexcept {
  delete static_cast<Gl_scene_diff *>(scene_diff);
}

void Gl_graphics::apply_scene_diff(Scene_diff *scene_diff) {
  static_cast<Gl_scene_diff *>(scene_diff)->_impl.apply();
}

void Gl_graphics::apply_scene_diff(Scene_diff *scene_diff, float factor) {
  static_cast<Gl_scene_diff *>(scene_diff)->_impl.apply(factor);
}

Gl_default_render_target *Gl_graphics::get_default_render_target() noexcept {
  return _default_render_target.get();
}

void Gl_graphics::destroy_render_target(Render_target *) noexcept {
  // delete static_cast<Gl_render_target *>(target);
}

void Gl_graphics::render(Scene *source_scene,
                         Camera_instance *source_camera_instance,
                         Render_target *target) {
  auto const gl_source_scene = static_cast<Gl_scene *>(source_scene);
  auto const gl_source_camera_instance =
      static_cast<Gl_camera_instance *>(source_camera_instance);
  auto const gl_target = static_cast<Gl_render_target *>(target);
  auto const view_matrix_3x4 =
      gl_source_camera_instance->_impl.get_scene_node()
          ->_impl.calculate_model_matrix_inv();
  auto const view_matrix = math::Mat4x4f{view_matrix_3x4[0],
                                         view_matrix_3x4[1],
                                         view_matrix_3x4[2],
                                         {0.0f, 0.0f, 0.0f, 1.0f}};
  auto const clip_matrix = gl_source_camera_instance->_impl.get_camera()
                               ->_impl.calculate_clip_matrix();
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_FRAMEBUFFER_SRGB);
  glBindFramebuffer(GL_FRAMEBUFFER, gl_target->get_framebuffer());
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glUseProgram(_shader_program.get());
  gl_source_scene->_impl.draw_surface_instances(
      _shader_program.get(), _default_base_color_texture.get(), 0, 1, 2,
      view_matrix, clip_matrix * view_matrix);
}
} // namespace graphics
} // namespace marlon