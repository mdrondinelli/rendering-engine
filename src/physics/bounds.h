#ifndef MARLON_PHYSICS_BOUNDS_H
#define MARLON_PHYSICS_BOUNDS_H

#include "../math/vec.h"

namespace marlon {
namespace physics {
struct Bounds {
  math::Vec3f min;
  math::Vec3f max;
};

inline math::Vec3f center(Bounds const &box) noexcept {
  return 0.5f * (box.min + box.max);
}

inline math::Vec3f extents(Bounds const &box) noexcept {
  return box.max - box.min;
}

inline float volume(Bounds const &box) noexcept {
  auto const d = extents(box);
  return d.x * d.y * d.z;
}

inline Bounds expand(Bounds const &box, float amount) noexcept {
  return {box.min - math::Vec3f::all(amount),
          box.max + math::Vec3f::all(amount)};
}

inline Bounds merge(Bounds const &first, Bounds const &second) noexcept {
  return {
      {std::min(first.min.x, second.min.x), std::min(first.min.y, second.min.y),
       std::min(first.min.z, second.min.z)},
      {std::max(first.max.x, second.max.x), std::max(first.max.y, second.max.y),
       std::max(first.max.z, second.max.z)}};
}

inline bool overlaps(Bounds const &first, Bounds const &second) noexcept {
  return first.min.x < second.max.x && first.min.y < second.max.y &&
         first.min.z < second.max.z && second.min.x < first.max.x &&
         second.min.y < first.max.y && second.min.z < first.max.z;
}
} // namespace physics
} // namespace marlon

#endif