#ifndef MARLON_MATH_QUAT_H
#define MARLON_MATH_QUAT_H

#include <cassert>
#include <cmath>

#include "vec.h"

namespace marlon {
namespace math {
template <typename T> struct Quat {
  T w;
  Vec<T, 3> v;

  static auto zero() { return Quat<T>{T(0), Vec<T, 3>::zero()}; }

  static auto identity() { return Quat<T>{T(1), Vec<T, 3>::zero()}; }

  static auto axis_angle(Vec<T, 3> const &axis, T angle) {
    const auto half_angle = angle / T(2);
    return Quat{std::cos(half_angle), std::sin(half_angle) * axis};
  }

  constexpr Quat(T w, Vec<T, 3> const &v) noexcept : w{w}, v{v} {}
};

using Quatf = Quat<float>;
using Quatd = Quat<double>;

template <typename T>
constexpr auto operator==(Quat<T> const &p, Quat<T> const &q) noexcept {
  return p.w == q.w && p.v == q.v;
}

template <typename T> constexpr auto operator+(Quat<T> const &q) noexcept {
  return q;
}

template <typename T> constexpr auto operator-(Quat<T> const &q) noexcept {
  return Quat<T>{-q.w, -q.v};
}

template <typename T>
constexpr auto operator*(Quat<T> const &q1, Quat<T> const &q2) noexcept {
  return Quat<T>{q1.w * q2.w - dot(q1.v, q2.v),
                 q1.w * q2.v + q2.w * q1.v + cross(q1.v, q2.v)};
}

template <typename T>
constexpr auto &operator*=(Quat<T> &q1, Quat<T> const &q2) noexcept {
  return q1 = (q1 * q2);
}
} // namespace math
} // namespace marlon

#endif