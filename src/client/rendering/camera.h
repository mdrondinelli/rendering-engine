#ifndef MARLON_RENDERING_CAMERA_H
#define MARLON_RENDERING_CAMERA_H

namespace marlon {
namespace rendering {
struct Camera_create_info {
  float near_plane_distance;
  float far_plane_distance;
  float aspect_ratio;
  float vertical_fov;
};

class Camera {};
} // namespace rendering
} // namespace marlon

#endif