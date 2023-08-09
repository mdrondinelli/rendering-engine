#ifndef MARLON_PHYSICS_SHAPE_H
#define MARLON_PHYSICS_SHAPE_H

#include <algorithm>
#include <array>
#include <optional>
#include <type_traits>
#include <variant>

#include "../math/vec.h"
#include "../math/mat.h"
#include "particle.h"
#include "bounding_box.h"

namespace marlon {
namespace physics {
struct Ball {
  float radius;
};

struct Box {
  float half_width;
  float half_height;
  float half_depth;
};

inline Bounding_box bounds(Ball const &ball, math::Vec3f const &position) {
  return {.min = position - math::Vec3f::all(ball.radius),
          .max = position + math::Vec3f::all(ball.radius)};
}

inline Bounding_box bounds(Box const &box,
                           math::Mat3x4f const &transform) noexcept {
  auto const shape_space_points = std::array<math::Vec3f, 8>{
      math::Vec3f{-box.half_width, -box.half_height, -box.half_depth},
      math::Vec3f{-box.half_width, -box.half_height, box.half_depth},
      math::Vec3f{-box.half_width, box.half_height, -box.half_depth},
      math::Vec3f{-box.half_width, box.half_height, box.half_depth},
      math::Vec3f{box.half_width, -box.half_height, -box.half_depth},
      math::Vec3f{box.half_width, -box.half_height, box.half_depth},
      math::Vec3f{box.half_width, box.half_height, -box.half_depth},
      math::Vec3f{box.half_width, box.half_height, box.half_depth}};
  auto world_space_points = std::array<math::Vec3f, 8>{};
  for (auto i = 0; i < 8; ++i) {
    auto const &shape_space_point = shape_space_points[i];
    auto &world_space_point = world_space_points[i];
    world_space_point.x = transform[0][0] * shape_space_point.x +
                          transform[0][1] * shape_space_point.y +
                          transform[0][2] * shape_space_point.z +
                          transform[0][3];
    world_space_point.y = transform[1][0] * shape_space_point.x +
                          transform[1][1] * shape_space_point.y +
                          transform[1][2] * shape_space_point.z +
                          transform[1][3];
    world_space_point.z = transform[2][0] * shape_space_point.x +
                          transform[2][1] * shape_space_point.y +
                          transform[2][2] * shape_space_point.z +
                          transform[2][3];
  }
  auto bounds = Bounding_box{world_space_points[0], world_space_points[0]};
  for (auto i = 1; i < 8; ++i) {
    auto const &world_space_point = world_space_points[i];
    bounds.min.x = std::min(bounds.min.x, world_space_point.x);
    bounds.min.y = std::min(bounds.min.y, world_space_point.y);
    bounds.min.z = std::min(bounds.min.z, world_space_point.z);
    bounds.max.x = std::max(bounds.max.x, world_space_point.x);
    bounds.max.y = std::max(bounds.max.y, world_space_point.y);
    bounds.max.z = std::max(bounds.max.z, world_space_point.z);
  }
  return bounds;
}

inline std::optional<Particle_contact>
find_particle_contact(math::Vec3f const &particle_position,
                      float particle_radius, Ball const &ball,
                      math::Vec3f const &ball_position) noexcept {
  auto const displacement = particle_position - ball_position;
  auto const distance2 = math::length2(displacement);
  auto const contact_distance = ball.radius + particle_radius;
  auto const contact_distance2 = contact_distance * contact_distance;
  if (distance2 <= contact_distance2) {
    auto const distance = std::sqrt(distance2);
    auto const normal = displacement / distance;
    return Particle_contact{.normal = normal,
                            .separation = distance - contact_distance};
  } else {
    return std::nullopt;
  }
}

inline std::optional<Particle_contact>
find_particle_contact(math::Vec3f const &particle_position,
                      float particle_radius, Box const &box,
                      math::Mat3x4f const &box_transform,
                      math::Mat3x4f const &box_transform_inverse) noexcept {
  auto const shape_space_particle_position =
      math::Vec3f{box_transform_inverse[0][0] * particle_position[0] +
                      box_transform_inverse[0][1] * particle_position[1] +
                      box_transform_inverse[0][2] * particle_position[2] +
                      box_transform_inverse[0][3],
                  box_transform_inverse[1][0] * particle_position[0] +
                      box_transform_inverse[1][1] * particle_position[1] +
                      box_transform_inverse[1][2] * particle_position[2] +
                      box_transform_inverse[1][3],
                  box_transform_inverse[2][0] * particle_position[0] +
                      box_transform_inverse[2][1] * particle_position[1] +
                      box_transform_inverse[2][2] * particle_position[2] +
                      box_transform_inverse[2][3]};
  auto const shape_space_clamped_particle_position =
      math::Vec3f{std::clamp(shape_space_particle_position.x, -box.half_width,
                             box.half_width),
                  std::clamp(shape_space_particle_position.y, -box.half_height,
                             box.half_height),
                  std::clamp(shape_space_particle_position.z, -box.half_depth,
                             box.half_depth)};
  auto const displacement =
      shape_space_particle_position - shape_space_clamped_particle_position;
  auto const distance2 = math::length2(displacement);
  auto const particle_radius2 = particle_radius * particle_radius;
  if (distance2 == 0.0f) {
    auto const face_distances = std::array<float, 6>{
        shape_space_clamped_particle_position.x + box.half_width,
        box.half_width - shape_space_clamped_particle_position.x,
        shape_space_clamped_particle_position.y + box.half_height,
        box.half_height - shape_space_clamped_particle_position.y,
        shape_space_clamped_particle_position.z + box.half_depth,
        box.half_depth - shape_space_clamped_particle_position.z};
    auto const face_normals = std::array<math::Vec3f, 6>{
        -math::Vec3f{box_transform[0][0], box_transform[1][0],
                     box_transform[2][0]},
        math::Vec3f{box_transform[0][0], box_transform[1][0],
                    box_transform[2][0]},
        -math::Vec3f{box_transform[0][1], box_transform[1][1],
                     box_transform[2][1]},
        math::Vec3f{box_transform[0][1], box_transform[1][1],
                    box_transform[2][1]},
        -math::Vec3f{box_transform[0][2], box_transform[1][2],
                     box_transform[2][2]},
        math::Vec3f{box_transform[0][2], box_transform[1][2],
                    box_transform[2][2]}};
    auto const face_index =
        std::min_element(face_distances.begin(), face_distances.end()) -
        face_distances.begin();
    auto const separation = -face_distances[face_index] - particle_radius;
    auto const normal = face_normals[face_index];
    return Particle_contact{.normal = normal, .separation = separation};
  } else if (distance2 <= particle_radius2) {
    auto const normal =
        math::normalize(math::Vec3f{box_transform[0][0] * displacement[0] +
                                        box_transform[0][1] * displacement[1] +
                                        box_transform[0][2] * displacement[2],
                                    box_transform[1][0] * displacement[0] +
                                        box_transform[1][1] * displacement[1] +
                                        box_transform[1][2] * displacement[2],
                                    box_transform[2][0] * displacement[0] +
                                        box_transform[2][1] * displacement[1] +
                                        box_transform[2][2] * displacement[2]});
    auto const distance = std::sqrt(distance2);
    auto const separation = distance - particle_radius;
    return Particle_contact{.normal = normal, .separation = separation};
  } else {
    return std::nullopt;
  }
}

class Shape {
public:
  Shape(Ball const &ball) noexcept : _v{ball} {}

  Shape(Box const &box) noexcept : _v{box} {}

  friend Bounding_box bounds(Shape const &shape,
                             math::Mat3x4f const &shape_transform) noexcept {
    return std::visit(
        [&](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, Ball>) {
            return bounds(arg, math::Vec3f{shape_transform[0][3],
                                           shape_transform[1][3],
                                           shape_transform[2][3]});
          } else {
            static_assert(std::is_same_v<T, Box>);
            return bounds(arg, shape_transform);
          }
        },
        shape._v);
  }

  friend std::optional<Particle_contact>
  find_particle_contact(math::Vec3f const &particle_position,
                        float particle_radius, Shape const &shape,
                        math::Mat3x4f const &shape_transform,
                        math::Mat3x4f const &shape_transform_inverse) noexcept {
    return std::visit(
        [&](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, Ball>) {
            return find_particle_contact(
                particle_position, particle_radius, arg,
                math::Vec3f{shape_transform[0][3], shape_transform[1][3],
                            shape_transform[2][3]});
          } else {
            static_assert(std::is_same_v<T, Box>);
            return find_particle_contact(particle_position, particle_radius,
                                         arg, shape_transform,
                                         shape_transform_inverse);
          }
        },
        shape._v);
  }

private:
  std::variant<Ball, Box> _v;
};
} // namespace physics
} // namespace marlon

#endif