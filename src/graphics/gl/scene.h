#ifndef MARLON_GRAPHICS_GL_SCENE_H
#define MARLON_GRAPHICS_GL_SCENE_H

#include "../../math/mat.h"
#include "../../util/pool.h"
#include "../../util/set.h"
#include "../scene.h"
#include "surface.h"
#include "wireframe.h"

namespace marlon {
namespace graphics {
class Gl_scene final : public Scene {
public:
  explicit Gl_scene(Scene_create_info const &create_info) noexcept;

  ~Gl_scene();

  Rgb_spectrum get_ambient_irradiance() const noexcept final;

  void set_ambient_irradiance(Rgb_spectrum ambient_irradiance) noexcept final;

  std::optional<Directional_light> const &
  get_directional_light() const noexcept final;

  void set_directional_light(
      std::optional<Directional_light> const &directional_light) noexcept final;

  Surface *create_surface(Surface_create_info const &create_info) final;

  void destroy_surface(Surface *surface) noexcept final;

  Wireframe *create_wireframe(Wireframe_create_info const &create_info) final;

  void destroy_wireframe(Wireframe *wireframe) noexcept final;

  void draw_surfaces(std::uint32_t shader_program,
                     std::uint32_t default_base_color_texture,
                     std::int32_t model_matrix_location,
                     std::int32_t model_view_clip_matrix_location,
                     std::int32_t base_color_tint_location,
                     std::int32_t ambient_irradiance_location,
                     std::int32_t directional_light_irradiance_location,
                     std::int32_t directional_light_direction_location,
                     std::int32_t exposure_location,
                     math::Mat4x4f const &view_clip_matrix,
                     float exposure) const;

  void draw_wireframes(std::uint32_t shader_program,
                       std::int32_t model_view_clip_matrix_location,
                       std::int32_t color_location,
                       math::Mat4x4f const &view_clip_matrix) const;

private:
  Rgb_spectrum _ambient_irradiance{Rgb_spectrum::black()};
  std::optional<Directional_light> _directional_light;
  util::Block _memory;
  util::Pool<Gl_surface> _surface_pool;
  util::Pool<Gl_wireframe> _wireframe_pool;
  util::Set<Gl_surface *> _surfaces;
  util::Set<Gl_wireframe *> _wireframes;
  // std::unique_ptr<Impl> _impl;
};
} // namespace graphics
} // namespace marlon

#endif