#include "scene.h"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
#include <glad/glad.h>
#pragma clang diagnostic pop
#else
#include <glad/glad.h>
#endif

#include "mesh.h"
#include "texture.h"

namespace marlon {
namespace graphics {
Gl_scene::Impl::Impl(Scene_create_info const &) noexcept {}

Gl_scene::Impl::~Impl() {
  for (auto const surface_instance : _surface_instances) {
    delete surface_instance;
  }
  for (auto const camera_instance : _camera_instances) {
    delete camera_instance;
  }
  for (auto const camera : _cameras) {
    delete camera;
  }
  for (auto const scene_node : _scene_nodes) {
    delete scene_node;
  }
}

void Gl_scene::Impl::acquire_scene_node(
    std::unique_ptr<Gl_scene_node> scene_node) {
  _scene_nodes.emplace(scene_node.get());
  scene_node.release();
}

bool Gl_scene::Impl::release_scene_node(Gl_scene_node *scene_node) {
  return _scene_nodes.erase(scene_node) != 0;
}

void Gl_scene::Impl::acquire_camera(std::unique_ptr<Gl_camera> camera) {
  _cameras.emplace(camera.get());
  camera.release();
}

bool Gl_scene::Impl::release_camera(Gl_camera *camera) {
  return _cameras.erase(camera) != 0;
}

void Gl_scene::Impl::acquire_camera_instance(
    std::unique_ptr<Gl_camera_instance> camera_instance) {
  _camera_instances.emplace(camera_instance.get());
  camera_instance.release();
}

bool Gl_scene::Impl::release_camera_instance(
    Gl_camera_instance *camera_instance) {
  return _camera_instances.erase(camera_instance) != 0;
}

void Gl_scene::Impl::acquire_surface_instance(
    std::unique_ptr<Gl_surface_instance> surface_instance) {
  _surface_instances.emplace(surface_instance.get());
  surface_instance.release();
}

bool Gl_scene::Impl::release_surface_instance(
    Gl_surface_instance *surface_instance) {
  return _surface_instances.erase(surface_instance) != 0;
}

void Gl_scene::Impl::draw_surface_instances(
    std::uint32_t shader_program, std::uint32_t default_base_color_texture,
    std::int32_t model_view_matrix_location,
    std::int32_t model_view_clip_matrix_location, std::int32_t albedo_location,
    math::Mat4x4f const &view_matrix, math::Mat4x4f const &view_clip_matrix) {
  for (auto const surface_instance : _surface_instances) {
    auto const model_matrix_3x4 = surface_instance->_impl.get_scene_node()
                                      ->_impl.calculate_model_matrix();
    auto const model_matrix_4x4 = math::Mat4x4f{model_matrix_3x4[0],
                                                model_matrix_3x4[1],
                                                model_matrix_3x4[2],
                                                {0.0f, 0.0f, 0.0f, 1.0f}};
    auto const model_view_matrix = view_matrix * model_matrix_4x4;
    auto const model_view_clip_matrix = view_clip_matrix * model_matrix_4x4;
    glProgramUniformMatrix4fv(shader_program, model_view_matrix_location, 1,
                              GL_TRUE, &model_view_matrix[0][0]);
    glProgramUniformMatrix4fv(shader_program, model_view_clip_matrix_location,
                              1, GL_TRUE, &model_view_clip_matrix[0][0]);
    auto const &surface = surface_instance->_impl.get_surface();
    auto const &material = surface.material;
    if (material.base_color_texture != nullptr) {
      glBindTextureUnit(0,
                        static_cast<Gl_texture *>(material.base_color_texture)
                            ->_handle.get());
    } else {
      glBindTextureUnit(0, default_base_color_texture);
    }
    glProgramUniform3f(shader_program, albedo_location,
                       material.base_color_tint.r, material.base_color_tint.g,
                       material.base_color_tint.b);
    auto const mesh = static_cast<Gl_mesh *>(surface.mesh);
    mesh->_impl.bind_vertex_array();
    mesh->_impl.draw();
  }
}

Gl_scene::Gl_scene(Scene_create_info const &create_info) noexcept
    : _impl{create_info} {}
} // namespace graphics
} // namespace marlon