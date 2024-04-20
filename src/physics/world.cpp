#include "world.h"

#include <cstdint>

#include <latch>

#include "../math/scalar.h"
#include "../util/bit_list.h"
#include "../util/list.h"
#include "../util/map.h"
#include "aabb_tree.h"

namespace marlon {
using math::Mat3x3f;
using math::Mat3x4f;
using math::Quatf;
using math::Vec3f;

using math::abs;
using math::max;
using math::min;
using math::pow;

using util::Bit_list;
using util::Block;
using util::List;
using util::Map;
using util::Queue;
using util::Stack_allocator;

using util::make_block;
namespace physics {

namespace {
using Aabb_tree_payload_t =
    std::variant<Particle_handle, Static_body_handle, Rigid_body_handle>;

enum class Object_type : std::uint8_t { particle, rigid_body, static_body };

enum class Object_pair_type : std::uint8_t {
  particle_particle,
  particle_rigid_body,
  particle_static_body,
  rigid_body_rigid_body,
  rigid_body_static_body
};

auto constexpr color_unmarked{static_cast<std::uint16_t>(-1)};
auto constexpr color_marked{static_cast<std::uint16_t>(-2)};
auto constexpr reserved_colors{std::size_t{2}};
auto constexpr max_colors = (std::size_t{1} << 16) - reserved_colors;

struct Neighbor_pair {
  std::array<std::uint32_t, 2> objects;
  Object_pair_type type;
  std::uint16_t color{color_unmarked};
};

struct Particle_data {
  Aabb_tree<Aabb_tree_payload_t>::Node *aabb_tree_node{};
  Neighbor_pair **neighbor_pairs{};
  Particle_motion_callback *motion_callback{};
  float radius{};
  float inverse_mass{};
  Material material{};
  Vec3f previous_position{};
  Vec3f position{};
  Vec3f velocity{};
  float waking_motion{};
  std::uint16_t neighbor_count{};
  bool marked{};
  bool awake{};
};

struct Rigid_body_data {
  Aabb_tree<Aabb_tree_payload_t>::Node *aabb_tree_node{};
  Neighbor_pair **neighbor_pairs{};
  Rigid_body_motion_callback *motion_callback{};
  Shape shape;
  float inverse_mass{};
  Mat3x3f inverse_inertia_tensor{};
  Material material{};
  Vec3f previous_position{};
  Vec3f position{};
  Vec3f velocity{};
  Quatf previous_orientation{};
  Quatf orientation{};
  Vec3f angular_velocity{};
  float waking_motion;
  std::uint16_t neighbor_count{};
  bool marked{};
  bool awake{};
};

struct Static_body_data {
  Aabb_tree<Aabb_tree_payload_t>::Node *aabb_tree_node;
  Shape shape;
  Material material;
  Mat3x4f transform;
  Mat3x4f inverse_transform;
};

class Particle_storage {
public:
  explicit Particle_storage(std::size_t size) noexcept
      : _data{std::make_unique<std::byte[]>(size * sizeof(Particle_data))},
        _free_indices(size),
        _occupancy_bits(size) {
    for (auto i = std::size_t{}; i != size; ++i) {
      _free_indices[i] = static_cast<std::uint32_t>(size - i - 1);
    }
  }

  Particle_handle create(Particle_data const &data) {
    if (_free_indices.empty()) {
      throw std::runtime_error{"Out of space for particles"};
    }
    auto const index = _free_indices.back();
    new (_data.get() + sizeof(Particle_data) * index) Particle_data{data};
    _free_indices.pop_back();
    _occupancy_bits[index] = true;
    return Particle_handle{index};
  }

  void destroy(Particle_handle particle) {
    _free_indices.emplace_back(particle.value);
    _occupancy_bits[particle.value] = false;
  }

  Particle_data const *data(Particle_handle handle) const noexcept {
    return std::launder(reinterpret_cast<Particle_data const *>(
        _data.get() + sizeof(Particle_data) * handle.value));
  }

  Particle_data *data(Particle_handle handle) noexcept {
    return std::launder(reinterpret_cast<Particle_data *>(
        _data.get() + sizeof(Particle_data) * handle.value));
  }

  template <typename F> void for_each(F &&f) {
    auto const n = _occupancy_bits.size();
    auto const m = _occupancy_bits.size() - _free_indices.size();
    auto k = std::size_t{};
    for (auto i = std::size_t{}; i != n && k != m; ++i) {
      if (_occupancy_bits[i]) {
        auto const handle = Particle_handle{static_cast<std::uint32_t>(i)};
        f(handle, data(handle));
        ++k;
      }
    }
  }

private:
  std::unique_ptr<std::byte[]> _data;
  std::vector<std::uint32_t> _free_indices;
  std::vector<bool> _occupancy_bits;
};

class Rigid_body_storage {
public:
  explicit Rigid_body_storage(std::size_t size)
      : _data{std::make_unique<std::byte[]>(size * sizeof(Rigid_body_data))},
        _free_indices(size),
        _occupancy_bits(size) {
    for (auto i = std::size_t{}; i != size; ++i) {
      _free_indices[i] = static_cast<std::uint32_t>(size - i - 1);
    }
  }

  Rigid_body_handle create(Rigid_body_data const &data) {
    if (_free_indices.empty()) {
      throw std::runtime_error{"Out of space for static rigid bodies"};
    }
    auto const index = _free_indices.back();
    new (_data.get() + sizeof(Rigid_body_data) * index) Rigid_body_data{data};
    _free_indices.pop_back();
    _occupancy_bits[index] = true;
    return Rigid_body_handle{index};
  }

  void destroy(Rigid_body_handle rigid_body) {
    _free_indices.emplace_back(rigid_body.value);
    _occupancy_bits[rigid_body.value] = false;
  }

  Rigid_body_data const *data(Rigid_body_handle handle) const noexcept {
    return std::launder(reinterpret_cast<Rigid_body_data const *>(
        _data.get() + sizeof(Rigid_body_data) * handle.value));
  }

  Rigid_body_data *data(Rigid_body_handle handle) noexcept {
    return std::launder(reinterpret_cast<Rigid_body_data *>(
        _data.get() + sizeof(Rigid_body_data) * handle.value));
  }

  template <typename F> void for_each(F &&f) {
    auto const n = _occupancy_bits.size();
    auto const m = _occupancy_bits.size() - _free_indices.size();
    auto k = std::size_t{};
    for (auto i = std::size_t{}; i != n && k != m; ++i) {
      if (_occupancy_bits[i]) {
        auto const handle = Rigid_body_handle{static_cast<std::uint32_t>(i)};
        f(handle, data(handle));
        ++k;
      }
    }
  }

private:
  std::unique_ptr<std::byte[]> _data;
  std::vector<std::uint32_t> _free_indices;
  std::vector<bool> _occupancy_bits;
};

class Static_body_storage {
public:
  explicit Static_body_storage(std::size_t size) noexcept
      : _data{std::make_unique<std::byte[]>(size * sizeof(Static_body_data))},
        _free_indices(size),
        _occupancy_bits(size) {
    for (auto i = std::size_t{}; i != size; ++i) {
      _free_indices[i] = static_cast<std::uint32_t>(size - i - 1);
    }
  }

  Static_body_handle create(Static_body_data const &data) {
    if (_free_indices.empty()) {
      throw std::runtime_error{"Out of space for static rigid bodies"};
    }
    auto const index = _free_indices.back();
    new (_data.get() + sizeof(Static_body_data) * index) Static_body_data{data};
    _free_indices.pop_back();
    _occupancy_bits[index] = true;
    return Static_body_handle{index};
  }

  void destroy(Static_body_handle static_body) {
    _free_indices.emplace_back(static_body.value);
    _occupancy_bits[static_body.value] = false;
  }

  Static_body_data const *data(Static_body_handle handle) const noexcept {
    return std::launder(reinterpret_cast<Static_body_data const *>(
        _data.get() + sizeof(Static_body_data) * handle.value));
  }

  Static_body_data *data(Static_body_handle handle) noexcept {
    return std::launder(reinterpret_cast<Static_body_data *>(
        _data.get() + sizeof(Static_body_data) * handle.value));
  }

  template <typename F> void for_each(F &&f) {
    auto const n = _occupancy_bits.size();
    auto const m = _occupancy_bits.size() - _free_indices.size();
    auto k = std::size_t{};
    for (auto i = std::size_t{}; i != n; ++i) {
      if (_occupancy_bits[i]) {
        auto const handle = Static_body_handle{static_cast<std::uint32_t>(i)};
        f(handle, data(handle));
        if (++k == m) {
          return;
        }
      }
    }
  }

private:
  std::unique_ptr<std::byte[]> _data;
  std::vector<std::uint32_t> _free_indices;
  std::vector<bool> _occupancy_bits;
};

class Dynamic_object_list {
  using Allocator = Stack_allocator<>;

public:
  static constexpr std::size_t
  memory_requirement(std::size_t max_size) noexcept {
    return Allocator::memory_requirement(
        {decltype(_object_types)::memory_requirement(max_size),
         decltype(_object_handles)::memory_requirement(max_size)});
  }

  constexpr Dynamic_object_list() noexcept = default;

  explicit Dynamic_object_list(Block block, std::size_t max_size) noexcept
      : Dynamic_object_list{block.begin, max_size} {}

  explicit Dynamic_object_list(void *block_begin,
                               std::size_t max_size) noexcept {
    auto allocator =
        Allocator{make_block(block_begin, memory_requirement(max_size))};
    _object_types = make_list<Object_type>(allocator, max_size).second;
    _object_handles = make_list<Object_handle>(allocator, max_size).second;
  }

  std::variant<Particle_handle, Rigid_body_handle>
  at(std::size_t i) const noexcept {
    auto const object_type = _object_types[i];
    auto const object_handle = _object_handles[i];
    switch (object_type) {
    case Object_type::particle:
      return Particle_handle{object_handle};
    case Object_type::rigid_body:
      return Rigid_body_handle{object_handle};
    case Object_type::static_body:
    default:
      math::unreachable();
    }
  }

  std::variant<Particle_handle, Rigid_body_handle> front() const noexcept {
    assert(!empty());
    return at(0);
  }

  std::variant<Particle_handle, Rigid_body_handle> back() const noexcept {
    assert(&_object_types.back() == &_object_types[size() - 1]);
    return at(size() - 1);
  }

  bool empty() const noexcept { return _object_types.empty(); }

  std::size_t size() const noexcept { return _object_types.size(); }

  std::size_t max_size() const noexcept { return _object_types.max_size(); }

  std::size_t capacity() const noexcept { return _object_types.capacity(); }

  void clear() noexcept {
    _object_types.clear();
    _object_handles.clear();
  }

  void push_back(Particle_handle h) {
    _object_types.push_back(Object_type::particle);
    _object_handles.push_back(h.value);
  }

  void push_back(Rigid_body_handle h) {
    _object_types.push_back(Object_type::rigid_body);
    _object_handles.push_back(h.value);
  }

  void push_back(std::variant<Particle_handle, Rigid_body_handle> h) {
    std::visit([this](auto &&arg) { push_back(arg); }, h);
  }

  void pop_back() {
    _object_types.pop_back();
    _object_handles.pop_back();
  }

private:
  List<Object_type> _object_types;
  List<Object_handle> _object_handles;
};

template <typename Allocator>
std::pair<Block, Dynamic_object_list>
make_dynamic_object_list(Allocator &allocator, std::size_t max_size) {
  auto const block =
      allocator.alloc(Dynamic_object_list::memory_requirement(max_size));
  return {block, Dynamic_object_list{block, max_size}};
}

class Neighbor_group_storage {
  using Allocator = Stack_allocator<>;

public:
  struct Group {
    std::uint32_t objects_begin;
    std::uint32_t objects_end;
    std::uint32_t neighbor_pairs_begin;
    std::uint32_t neighbor_pairs_end;
  };

  static constexpr std::size_t
  memory_requirement(std::size_t max_object_count,
                     std::size_t max_neighbor_pair_count,
                     std::size_t max_group_count) {
    return Allocator::memory_requirement({
        decltype(_objects)::memory_requirement(max_object_count),
        decltype(_neighbor_pairs)::memory_requirement(max_neighbor_pair_count),
        decltype(_groups)::memory_requirement(max_group_count),
    });
  }

  constexpr Neighbor_group_storage() noexcept = default;

  Neighbor_group_storage(Block block,
                         std::size_t max_object_count,
                         std::size_t max_neighbor_pair_count,
                         std::size_t max_group_count)
      : Neighbor_group_storage{block.begin,
                               max_object_count,
                               max_neighbor_pair_count,
                               max_group_count} {}

  Neighbor_group_storage(void *block_begin,
                         std::size_t max_object_count,
                         std::size_t max_neighbor_pair_count,
                         std::size_t max_group_count) {
    auto allocator = Allocator{make_block(
        block_begin,
        memory_requirement(
            max_object_count, max_neighbor_pair_count, max_group_count))};
    _objects = make_dynamic_object_list(allocator, max_object_count).second;
    _neighbor_pairs =
        util::make_list<Neighbor_pair *>(allocator, max_neighbor_pair_count)
            .second;
    _groups = util::make_list<Group>(allocator, max_group_count).second;
  }

  std::size_t object_count() const noexcept { return _objects.size(); }

  std::variant<Particle_handle, Rigid_body_handle>
  object(std::size_t object_index) const noexcept {
    return _objects.at(object_index);
  }

  std::size_t neighbor_pair_count() const noexcept {
    return _neighbor_pairs.size();
  }

  Neighbor_pair *neighbor_pair(std::size_t neighbor_pair_index) const noexcept {
    return _neighbor_pairs[neighbor_pair_index];
  }

  std::size_t group_count() const noexcept { return _groups.size(); }

  Group const &group(std::size_t group_index) const noexcept {
    return _groups[group_index];
  }

  void clear() noexcept {
    _objects.clear();
    _neighbor_pairs.clear();
    _groups.clear();
  }

  void begin_group() {
    auto const objects_index = static_cast<std::uint32_t>(_objects.size());
    auto const neighbor_pairs_index =
        static_cast<std::uint32_t>(_neighbor_pairs.size());
    _groups.push_back({
        .objects_begin = objects_index,
        .objects_end = objects_index,
        .neighbor_pairs_begin = neighbor_pairs_index,
        .neighbor_pairs_end = neighbor_pairs_index,
    });
  }

  void add_to_group(Particle_handle object) {
    _objects.push_back(object);
    ++_groups.back().objects_end;
  }

  void add_to_group(Rigid_body_handle object) {
    _objects.push_back(object);
    ++_groups.back().objects_end;
  }

  void add_to_group(std::variant<Particle_handle, Rigid_body_handle> object) {
    std::visit([this](auto &&arg) { add_to_group(arg); }, object);
  }

  void add_to_group(Neighbor_pair *neighbor_pair) {
    _neighbor_pairs.emplace_back(neighbor_pair);
    ++_groups.back().neighbor_pairs_end;
  }

private:
  Dynamic_object_list _objects;
  List<Neighbor_pair *> _neighbor_pairs;
  List<Group> _groups;
};

template <typename Allocator>
std::pair<Block, Neighbor_group_storage>
make_neighbor_group_storage(Allocator &allocator,
                            std::size_t max_object_count,
                            std::size_t max_neighbor_pair_count,
                            std::size_t max_group_count) {
  auto const block = allocator.alloc(Neighbor_group_storage::memory_requirement(
      max_object_count, max_neighbor_pair_count, max_group_count));
  return {
      block,
      Neighbor_group_storage{
          block, max_object_count, max_neighbor_pair_count, max_group_count}};
}

class Color_group_storage {
  using Allocator = Stack_allocator<>;

public:
  static constexpr std::size_t
  memory_requirement(std::size_t max_neighbor_pairs) noexcept {
    return Allocator::memory_requirement({
        decltype(_neighbor_pairs)::memory_requirement(max_neighbor_pairs),
        decltype(_groups)::memory_requirement(max_colors),
    });
  }

  constexpr Color_group_storage() = default;

  explicit Color_group_storage(Block block, std::size_t max_neighbor_pairs)
      : Color_group_storage{block.begin, max_neighbor_pairs} {}

  explicit Color_group_storage(void *block, std::size_t max_neighbor_pairs) {
    auto allocator =
        Allocator{make_block(block, memory_requirement(max_neighbor_pairs))};
    _neighbor_pairs =
        util::make_list<Neighbor_pair *>(allocator, max_neighbor_pairs).second;
    _groups = util::make_list<Group>(allocator, max_colors).second;
    _groups.resize(max_colors);
  }

  std::span<Neighbor_pair *const> group(std::uint16_t color) const noexcept {
    return {_neighbor_pairs.data() + _groups[color].neighbor_pairs_begin,
            _neighbor_pairs.data() + _groups[color].neighbor_pairs_end};
  }

  void clear() noexcept {
    _neighbor_pairs.clear();
    _groups.clear();
    _groups.resize(max_colors);
  }

  void count(std::uint16_t color) noexcept {
    ++_groups[color].neighbor_pairs_end;
  }

  void reserve() {
    for (auto &group : _groups) {
      if (group.neighbor_pairs_end == 0) {
        return;
      }
      auto const index = static_cast<std::uint32_t>(_neighbor_pairs.size());
      _neighbor_pairs.resize(_neighbor_pairs.size() + group.neighbor_pairs_end);
      group.neighbor_pairs_begin = index;
      group.neighbor_pairs_end = index;
    }
  }

  void push_back(Neighbor_pair *neighbor_pair) {
    _neighbor_pairs[_groups[neighbor_pair->color].neighbor_pairs_end++] =
        neighbor_pair;
  }

private:
  struct Group {
    std::uint32_t neighbor_pairs_begin{};
    std::uint32_t neighbor_pairs_end{};
  };

  List<Neighbor_pair *> _neighbor_pairs;
  List<Group> _groups;
};

template <typename Allocator>
std::pair<Block, Color_group_storage>
make_color_group_storage(Allocator &allocator, std::size_t max_neighbor_pairs) {
  auto const block = allocator.alloc(
      Color_group_storage::memory_requirement(max_neighbor_pairs));
  return {block, Color_group_storage{block, max_neighbor_pairs}};
}

struct Positional_constraint_problem {
  Vec3f direction;
  float distance;
  std::array<Vec3f, 2> relative_position;
  std::array<float, 2> inverse_mass;
  std::array<Mat3x3f, 2> inverse_inertia_tensor;
};

struct Positional_constraint_solution {
  std::array<Vec3f, 2> delta_position;
  std::array<Vec3f, 2> delta_orientation;
  float delta_lambda;
};

Positional_constraint_solution
solve_positional_constraint(Positional_constraint_problem const &problem) {
  auto const &n = problem.direction;
  auto const c = problem.distance;
  auto const &r_1 = problem.relative_position[0];
  auto const &r_2 = problem.relative_position[1];
  auto const m_inv_1 = problem.inverse_mass[0];
  auto const m_inv_2 = problem.inverse_mass[1];
  auto const &I_inv_1 = problem.inverse_inertia_tensor[0];
  auto const &I_inv_2 = problem.inverse_inertia_tensor[1];
  auto const r_1_cross_n = cross(r_1, n);
  auto const r_2_cross_n = cross(r_2, n);
  auto const w_1 = m_inv_1 + dot(r_1_cross_n, I_inv_1 * r_1_cross_n);
  auto const w_2 = m_inv_2 + dot(r_2_cross_n, I_inv_2 * r_2_cross_n);
  auto const delta_lambda = c / (w_1 + w_2);
  auto const p = delta_lambda * n;
  return {
      .delta_position =
          {
              p * m_inv_1,
              -p * m_inv_2,
          },
      .delta_orientation =
          {
              I_inv_1 * cross(r_1, p),
              I_inv_2 * cross(r_2, -p),
          },
      .delta_lambda = delta_lambda,
  };
}

struct Contact {
  Vec3f normal;
  std::array<Vec3f, 2> relative_positions;
  float separating_velocity;
  float lambda_n;
  float lambda_t;
};

struct Solve_state {
  std::latch *latch;
  Particle_storage *particles;
  Rigid_body_storage *rigid_bodies;
  Static_body_storage *static_bodies;
  // Vec3f gravitational_delta_velocity;
  float inverse_delta_time;
  float restitution_separating_velocity_threshold;
};

auto constexpr max_solve_chunk_size = std::size_t{16};

struct Solve_chunk {
  Neighbor_pair const *const *pairs;
  Contact *contacts;
  std::size_t size;
};

class Position_solve_task : public util::Task {
public:
  explicit Position_solve_task(Solve_state const *state,
                               Solve_chunk const *chunk) noexcept
      : _state{state}, _chunk{chunk} {}

  void run(unsigned) final {
    for (auto i = std::size_t{}; i != _chunk->size; ++i) {
      auto const pair = _chunk->pairs[i];
      auto contact = std::optional<Contact>{};
      switch (pair->type) {
      case Object_pair_type::particle_particle: {
        contact = solve_neighbor_pair({Particle_handle{pair->objects[0]},
                                       Particle_handle{pair->objects[1]}});
        break;
      }
      case Object_pair_type::particle_rigid_body: {
        contact = solve_neighbor_pair({Particle_handle{pair->objects[0]},
                                       Rigid_body_handle{pair->objects[1]}});
        break;
      }
      case Object_pair_type::particle_static_body: {
        contact = solve_neighbor_pair({Particle_handle{pair->objects[0]},
                                       Static_body_handle{pair->objects[1]}});
        break;
      }
      case Object_pair_type::rigid_body_rigid_body: {
        contact = solve_neighbor_pair({Rigid_body_handle{pair->objects[0]},
                                       Rigid_body_handle{pair->objects[1]}});
        break;
      }
      case Object_pair_type::rigid_body_static_body: {
        contact = solve_neighbor_pair({Rigid_body_handle{pair->objects[0]},
                                       Static_body_handle{pair->objects[1]}});
        break;
      }
      default:
        math::unreachable();
      }
      if (contact) {
        _chunk->contacts[i] = *contact;
      } else {
        _chunk->contacts[i].normal = Vec3f::zero();
      }
    }
    _state->latch->count_down();
  }

private:
  std::optional<Contact> solve_neighbor_pair(
      std::pair<Particle_handle, Particle_handle> objects) const noexcept {
    if (auto const contact_geometry = get_contact_geometry(objects)) {
      auto contact = Contact{
          .normal = contact_geometry->normal,
          .separating_velocity =
              dot(get_velocity(objects.first) - get_velocity(objects.second),
                  contact_geometry->normal),
          .lambda_n = 0.0f,
          .lambda_t = 0.0f,
      };
      solve_contact(objects, contact, contact_geometry->separation);
      return contact;
    } else {
      return std::nullopt;
    }
  }

  std::optional<Contact> solve_neighbor_pair(
      std::pair<Particle_handle, Rigid_body_handle> objects) const noexcept {
    if (auto const contact_geometry = get_contact_geometry(objects)) {
      auto const relative_position =
          contact_geometry->position - get_position(objects.second);
      auto contact = Contact{
          .normal = contact_geometry->normal,
          .relative_positions = {Vec3f{}, relative_position},
          .separating_velocity =
              dot(get_velocity(objects.first) -
                      get_velocity(objects.second, relative_position),
                  contact_geometry->normal),
          .lambda_n = 0.0f,
          .lambda_t = 0.0f,
      };
      solve_contact(objects, contact, contact_geometry->separation);
      return contact;
    } else {
      return std::nullopt;
    }
  }

  std::optional<Contact> solve_neighbor_pair(
      std::pair<Particle_handle, Static_body_handle> objects) const noexcept {
    if (auto const contact_geometry = get_contact_geometry(objects)) {
      auto contact = Contact{
          .normal = contact_geometry->normal,
          .separating_velocity =
              dot(get_velocity(objects.first), contact_geometry->normal),
          .lambda_n = 0.0f,
          .lambda_t = 0.0f,
      };
      solve_contact(objects, contact, contact_geometry->separation);
      return contact;
    } else {
      return std::nullopt;
    }
  }

  std::optional<Contact> solve_neighbor_pair(
      std::pair<Rigid_body_handle, Rigid_body_handle> objects) const noexcept {
    if (auto const contact_geometry = get_contact_geometry(objects)) {
      auto const relative_positions = std::array<Vec3f, 2>{
          contact_geometry->position - get_position(objects.first),
          contact_geometry->position - get_position(objects.second),
      };
      auto contact = Contact{
          .normal = contact_geometry->normal,
          .relative_positions = relative_positions,
          .separating_velocity =
              dot(get_velocity(objects.first, relative_positions[0]) -
                      get_velocity(objects.second, relative_positions[1]),
                  contact_geometry->normal),
          .lambda_n = 0.0f,
          .lambda_t = 0.0f,
      };
      solve_contact(objects, contact, contact_geometry->separation);
      return contact;
    } else {
      return std::nullopt;
    }
  }

  std::optional<Contact> solve_neighbor_pair(
      std::pair<Rigid_body_handle, Static_body_handle> objects) const noexcept {
    if (auto const contact_geometry = get_contact_geometry(objects)) {
      auto const relative_position =
          contact_geometry->position - get_position(objects.first);
      auto contact = Contact{
          .normal = contact_geometry->normal,
          .relative_positions = {relative_position, Vec3f{}},
          .separating_velocity =
              dot(get_velocity(objects.first, relative_position),
                  contact_geometry->normal),
          .lambda_n = 0.0f,
          .lambda_t = 0.0f,
      };
      solve_contact(objects, contact, contact_geometry->separation);
      return contact;
    } else {
      return std::nullopt;
    }
  }

  std::optional<Positionless_contact_geometry> get_contact_geometry(
      std::pair<Particle_handle, Particle_handle> objects) const noexcept {
    auto const data = std::array<Particle_data *, 2>{
        get_data(objects.first),
        get_data(objects.second),
    };
    auto const displacement = data[0]->position - data[1]->position;
    auto const distance2 = length_squared(displacement);
    auto const contact_distance = data[0]->radius + data[1]->radius;
    auto const contact_distance2 = contact_distance * contact_distance;
    if (distance2 < contact_distance2) {
      auto const [normal, separation] = [&]() {
        if (distance2 == 0.0f) {
          // particles coincide, pick arbitrary contact normal
          Vec3f const contact_normal{1.0f, 0.0f, 0.0f};
          auto const separation = -contact_distance;
          return std::tuple{contact_normal, separation};
        } else {
          auto const distance = std::sqrt(distance2);
          auto const normal = displacement / distance;
          auto const separation = distance - contact_distance;
          return std::tuple{normal, separation};
        }
      }();
      return Positionless_contact_geometry{
          .normal = normal,
          .separation = separation,
      };
    } else {
      return std::nullopt;
    }
  }

  std::optional<Positionful_contact_geometry> get_contact_geometry(
      std::pair<Particle_handle, Rigid_body_handle> objects) const noexcept {
    auto const data =
        std::pair{get_data(objects.first), get_data(objects.second)};
    auto const transform =
        Mat3x4f::rigid(data.second->position, data.second->orientation);
    auto const inverse_transform = rigid_inverse(transform);
    return particle_shape_positionful_contact_geometry(data.first->position,
                                                       data.first->radius,
                                                       data.second->shape,
                                                       transform,
                                                       inverse_transform);
  }

  std::optional<Positionless_contact_geometry> get_contact_geometry(
      std::pair<Particle_handle, Static_body_handle> objects) const noexcept {
    auto const data =
        std::pair{get_data(objects.first), get_data(objects.second)};
    return particle_shape_positionless_contact_geometry(
        data.first->position,
        data.first->radius,
        data.second->shape,
        data.second->transform,
        data.second->inverse_transform);
  }

  std::optional<Positionful_contact_geometry> get_contact_geometry(
      std::pair<Rigid_body_handle, Rigid_body_handle> objects) const noexcept {
    auto const data = std::array<Rigid_body_data *, 2>{
        get_data(objects.first),
        get_data(objects.second),
    };
    auto const transforms = std::array<Mat3x4f, 2>{
        Mat3x4f::rigid(data[0]->position, data[0]->orientation),
        Mat3x4f::rigid(data[1]->position, data[1]->orientation),
    };
    auto const inverse_transforms = std::array<Mat3x4f, 2>{
        rigid_inverse(transforms[0]),
        rigid_inverse(transforms[1]),
    };
    return shape_shape_contact_geometry(data[0]->shape,
                                        transforms[0],
                                        inverse_transforms[0],
                                        data[1]->shape,
                                        transforms[1],
                                        inverse_transforms[1]);
  }

  std::optional<Positionful_contact_geometry> get_contact_geometry(
      std::pair<Rigid_body_handle, Static_body_handle> objects) const noexcept {
    auto const data =
        std::pair{get_data(objects.first), get_data(objects.second)};
    auto const transform =
        Mat3x4f::rigid(data.first->position, data.first->orientation);
    auto const inverse_transform = rigid_inverse(transform);
    return shape_shape_contact_geometry(data.first->shape,
                                        transform,
                                        inverse_transform,
                                        data.second->shape,
                                        data.second->transform,
                                        data.second->inverse_transform);
  }

  void solve_contact(std::pair<Particle_handle, Particle_handle> objects,
                     Contact &contact,
                     float separation) const noexcept {
    auto const particles = std::array<Particle_handle, 2>{
        objects.first,
        objects.second,
    };
    auto const particle_datas = std::array<Particle_data *, 2>{
        _state->particles->data(particles[0]),
        _state->particles->data(particles[1]),
    };
    auto const distance_per_impulse =
        particle_datas[0]->inverse_mass + particle_datas[1]->inverse_mass;
    auto const impulse_per_distance = 1.0f / distance_per_impulse;
    contact.lambda_n = -separation * impulse_per_distance;
    contact.lambda_t = 0.0f;
    auto const impulse = contact.lambda_n * contact.normal;
    auto const particle_position_deltas =
        std::array<Vec3f, 2>{impulse * particle_datas[0]->inverse_mass,
                             impulse * -particle_datas[1]->inverse_mass};
    particle_datas[0]->position += particle_position_deltas[0];
    particle_datas[1]->position += particle_position_deltas[1];
  }

  void solve_contact(std::pair<Particle_handle, Rigid_body_handle> objects,
                     Contact &contact,
                     float separation) const noexcept {
    auto const particle_data = get_data(objects.first);
    auto const body_data = get_data(objects.second);
    auto const rotation = Mat3x3f::rotation(body_data->orientation);
    auto const inverse_rotation = transpose(rotation);
    auto const inverse_inertia_tensor =
        rotation * body_data->inverse_inertia_tensor * inverse_rotation;
    auto const separation_solution = solve_positional_constraint({
        .direction = contact.normal,
        .distance = -separation,
        .relative_position = {Vec3f::zero(), contact.relative_positions[1]},
        .inverse_mass = {particle_data->inverse_mass, body_data->inverse_mass},
        .inverse_inertia_tensor = {Mat3x3f::zero(), inverse_inertia_tensor},
    });
    contact.lambda_n = separation_solution.delta_lambda;
    auto const contact_movement =
        (particle_data->position - particle_data->previous_position) -
        ((body_data->position + contact.relative_positions[1]) -
         (body_data->previous_position +
          Mat3x3f::rotation(body_data->previous_orientation) *
              inverse_rotation * contact.relative_positions[1]));
    auto const tangential_contact_movement =
        perp_unit(contact_movement, contact.normal);
    auto delta_position = separation_solution.delta_position;
    auto delta_orientation = separation_solution.delta_orientation[1];
    if (tangential_contact_movement != Vec3f::zero()) {
      auto const correction_distance = length(tangential_contact_movement);
      auto const correction_direction =
          tangential_contact_movement / -correction_distance;
      auto const friction_solution = solve_positional_constraint({
          .direction = correction_direction,
          .distance = correction_distance,
          .relative_position = {Vec3f::zero(), contact.relative_positions[1]},
          .inverse_mass = {particle_data->inverse_mass,
                           body_data->inverse_mass},
          .inverse_inertia_tensor = {Mat3x3f::zero(), inverse_inertia_tensor},
      });
      auto const static_friction_coefficient =
          0.5f * (particle_data->material.static_friction_coefficient +
                  body_data->material.static_friction_coefficient);
      if (friction_solution.delta_lambda <
          static_friction_coefficient * contact.lambda_n) {
        contact.lambda_t = friction_solution.delta_lambda;
        delta_position[0] += friction_solution.delta_position[0];
        delta_position[1] += friction_solution.delta_position[1];
        delta_orientation += friction_solution.delta_orientation[1];
      }
    }
    update_position(objects.first, delta_position[0]);
    update_position(objects.second, delta_position[1], delta_orientation);
  }

  void solve_contact(std::pair<Particle_handle, Static_body_handle> objects,
                     Contact &contact,
                     float separation) const noexcept {
    auto const particle_data = _state->particles->data(objects.first);
    auto const body_data = _state->static_bodies->data(objects.second);
    auto const separation_solution = solve_positional_constraint({
        .direction = contact.normal,
        .distance = -separation,
        .relative_position = {Vec3f::zero(), Vec3f::zero()},
        .inverse_mass = {particle_data->inverse_mass, 0.0f},
        .inverse_inertia_tensor = {Mat3x3f::zero(), Mat3x3f::zero()},
    });
    contact.lambda_n = separation_solution.delta_lambda;
    auto const contact_movement =
        particle_data->position - particle_data->previous_position;
    auto const tangential_contact_movement =
        perp_unit(contact_movement, contact.normal);
    auto delta_position = separation_solution.delta_position[0];
    if (tangential_contact_movement != Vec3f::zero()) {
      auto const correction_distance = length(tangential_contact_movement);
      auto const correction_direction =
          tangential_contact_movement / -correction_distance;
      auto const friction_solution = solve_positional_constraint({
          .direction = correction_direction,
          .distance = correction_distance,
          .relative_position = {Vec3f::zero(), Vec3f::zero()},
          .inverse_mass = {particle_data->inverse_mass, 0.0f},
          .inverse_inertia_tensor = {Mat3x3f::zero(), Mat3x3f::zero()},
      });
      auto const static_friction_coefficient =
          0.5f * (particle_data->material.static_friction_coefficient +
                  body_data->material.static_friction_coefficient);
      if (friction_solution.delta_lambda <
          static_friction_coefficient * contact.lambda_n) {
        contact.lambda_t = friction_solution.delta_lambda;
        delta_position += friction_solution.delta_position[0];
      }
    }
    update_position(objects.first, delta_position);
  }

  void solve_contact(std::pair<Rigid_body_handle, Rigid_body_handle> objects,
                     Contact &contact,
                     float separation) const noexcept {
    auto const data = std::array<Rigid_body_data *, 2>{
        _state->rigid_bodies->data(objects.first),
        _state->rigid_bodies->data(objects.second),
    };
    auto const rotation = std::array<Mat3x3f, 2>{
        Mat3x3f::rotation(data[0]->orientation),
        Mat3x3f::rotation(data[1]->orientation),
    };
    auto const inverse_rotation = std::array<Mat3x3f, 2>{
        transpose(rotation[0]),
        transpose(rotation[1]),
    };
    auto const inverse_inertia_tensor = std::array<Mat3x3f, 2>{
        rotation[0] * data[0]->inverse_inertia_tensor * inverse_rotation[0],
        rotation[1] * data[1]->inverse_inertia_tensor * inverse_rotation[1],
    };
    auto const separation_solution = solve_positional_constraint({
        .direction = contact.normal,
        .distance = -separation,
        .relative_position = contact.relative_positions,
        .inverse_mass = {data[0]->inverse_mass, data[1]->inverse_mass},
        .inverse_inertia_tensor = inverse_inertia_tensor,
    });
    contact.lambda_n = separation_solution.delta_lambda;
    auto const relative_contact_movement =
        ((data[0]->position + contact.relative_positions[0]) -
         (data[0]->previous_position +
          Mat3x3f::rotation(data[0]->previous_orientation) *
              inverse_rotation[0] * contact.relative_positions[0])) -
        ((data[1]->position + contact.relative_positions[1]) -
         (data[1]->previous_position +
          Mat3x3f::rotation(data[1]->previous_orientation) *
              inverse_rotation[1] * contact.relative_positions[1]));
    auto const tangential_relative_contact_movement =
        perp_unit(relative_contact_movement, contact.normal);
    auto delta_position = separation_solution.delta_position;
    auto delta_orientation = separation_solution.delta_orientation;
    if (tangential_relative_contact_movement != Vec3f::zero()) {
      auto const correction_distance =
          length(tangential_relative_contact_movement);
      auto const correction_direction =
          tangential_relative_contact_movement / -correction_distance;
      auto const friction_solution = solve_positional_constraint(
          {.direction = correction_direction,
           .distance = correction_distance,
           .relative_position = contact.relative_positions,
           .inverse_mass = {data[0]->inverse_mass, data[1]->inverse_mass},
           .inverse_inertia_tensor = inverse_inertia_tensor});
      auto const static_friction_coefficient =
          0.5f * (data[0]->material.static_friction_coefficient +
                  data[1]->material.static_friction_coefficient);
      if (friction_solution.delta_lambda <
          static_friction_coefficient * contact.lambda_n) {
        contact.lambda_t = friction_solution.delta_lambda;
        for (auto i = 0; i != 2; ++i) {
          delta_position[i] += friction_solution.delta_position[i];
          delta_orientation[i] += friction_solution.delta_orientation[i];
        }
      }
    }
    for (auto i = 0; i != 2; ++i) {
      update_position(i == 0 ? objects.first : objects.second,
                      delta_position[i],
                      delta_orientation[i]);
    }
  }

  void solve_contact(std::pair<Rigid_body_handle, Static_body_handle> objects,
                     Contact &contact,
                     float separation) const noexcept {
    auto const dynamic_body_data = _state->rigid_bodies->data(objects.first);
    auto const static_body_data = _state->static_bodies->data(objects.second);
    auto const rotation = Mat3x3f::rotation(dynamic_body_data->orientation);
    auto const inverse_rotation = transpose(rotation);
    auto const inverse_inertia_tensor =
        rotation * dynamic_body_data->inverse_inertia_tensor * inverse_rotation;
    auto const separation_solution = solve_positional_constraint(
        {.direction = contact.normal,
         .distance = -separation,
         .relative_position = {contact.relative_positions[0], Vec3f::zero()},
         .inverse_mass = {dynamic_body_data->inverse_mass, 0.0f},
         .inverse_inertia_tensor = {inverse_inertia_tensor, Mat3x3f::zero()}});
    contact.lambda_n = separation_solution.delta_lambda;
    auto const contact_movement =
        (dynamic_body_data->position + contact.relative_positions[0]) -
        (dynamic_body_data->previous_position +
         Mat3x3f::rotation(dynamic_body_data->previous_orientation) *
             inverse_rotation * contact.relative_positions[0]);
    auto const tangential_contact_movement =
        perp_unit(contact_movement, contact.normal);
    auto delta_position = separation_solution.delta_position[0];
    auto delta_orientation = separation_solution.delta_orientation[0];
    if (tangential_contact_movement != Vec3f::zero()) {
      auto const correction_distance = length(tangential_contact_movement);
      auto const correction_direction =
          tangential_contact_movement / -correction_distance;
      auto const friction_solution = solve_positional_constraint(
          {.direction = correction_direction,
           .distance = correction_distance,
           .relative_position = {contact.relative_positions[0], Vec3f::zero()},
           .inverse_mass = {dynamic_body_data->inverse_mass, 0.0f},
           .inverse_inertia_tensor = {inverse_inertia_tensor,
                                      Mat3x3f::zero()}});
      auto const static_friction_coefficient =
          0.5f * (dynamic_body_data->material.static_friction_coefficient +
                  static_body_data->material.static_friction_coefficient);
      if (friction_solution.delta_lambda <
          static_friction_coefficient * contact.lambda_n) {
        contact.lambda_t = friction_solution.delta_lambda;
        delta_position += friction_solution.delta_position[0];
        delta_orientation += friction_solution.delta_orientation[0];
      }
    }
    update_position(objects.first, delta_position, delta_orientation);
  }

  void update_position(Particle_handle particle_handle,
                       Vec3f const &delta_position) const noexcept {
    auto const particle_data = _state->particles->data(particle_handle);
    particle_data->position += delta_position;
  }

  void update_position(Rigid_body_handle body_handle,
                       Vec3f const &delta_position,
                       Vec3f const &delta_orientation) const noexcept {
    auto const body_data = _state->rigid_bodies->data(body_handle);
    body_data->position += delta_position;
    body_data->orientation +=
        0.5f * Quatf{0.0f, delta_orientation} * body_data->orientation;
    body_data->orientation = normalize(body_data->orientation);
  }

  Particle_data *get_data(Particle_handle particle) const noexcept {
    return _state->particles->data(particle);
  }

  Rigid_body_data *get_data(Rigid_body_handle rigid_body) const noexcept {
    return _state->rigid_bodies->data(rigid_body);
  }

  Static_body_data *get_data(Static_body_handle static_body) const noexcept {
    return _state->static_bodies->data(static_body);
  }

  Vec3f get_position(Particle_handle particle) const noexcept {
    return get_data(particle)->position;
  }

  Vec3f get_position(Rigid_body_handle rigid_body) const noexcept {
    return get_data(rigid_body)->position;
  }

  Vec3f get_velocity(Particle_handle particle) const noexcept {
    return get_data(particle)->velocity;
  }

  Vec3f get_velocity(Rigid_body_handle rigid_body,
                     Vec3f const &relative_position) const noexcept {
    auto const data = get_data(rigid_body);
    return data->velocity + cross(data->angular_velocity, relative_position);
  }

  Solve_state const *_state;
  Solve_chunk const *_chunk;
};

class Velocity_solve_task : public util::Task {
public:
  explicit Velocity_solve_task(Solve_state const *state,
                               Solve_chunk const *chunk) noexcept
      : _state{state}, _chunk{chunk} {}

  void run(unsigned) final {
    for (auto i = std::size_t{}; i != _chunk->size; ++i) {
      auto const &contact = _chunk->contacts[i];
      auto const &normal = contact.normal;
      if (normal != math::Vec3f::zero()) {
        switch (_chunk->pairs[i]->type) {
        case Object_pair_type::particle_particle:
          solve_contact(
              std::pair{Particle_handle{_chunk->pairs[i]->objects[0]},
                        Particle_handle{_chunk->pairs[i]->objects[1]}},
              contact);
          continue;
        case Object_pair_type::particle_rigid_body:
          solve_contact(
              std::pair{Particle_handle{_chunk->pairs[i]->objects[0]},
                        Rigid_body_handle{_chunk->pairs[i]->objects[1]}},
              contact);
          continue;
        case Object_pair_type::particle_static_body:
          solve_contact(
              std::pair{Particle_handle{_chunk->pairs[i]->objects[0]},
                        Static_body_handle{_chunk->pairs[i]->objects[1]}},
              contact);
          continue;
        case Object_pair_type::rigid_body_rigid_body:
          solve_contact(
              std::pair{Rigid_body_handle{_chunk->pairs[i]->objects[0]},
                        Rigid_body_handle{_chunk->pairs[i]->objects[1]}},
              contact);
          continue;
        case Object_pair_type::rigid_body_static_body:
          solve_contact(
              std::pair{Rigid_body_handle{_chunk->pairs[i]->objects[0]},
                        Static_body_handle{_chunk->pairs[i]->objects[1]}},
              contact);
          continue;
        }
      }
    }
    _state->latch->count_down();
  }

private:
  template <typename T, typename U>
  void solve_contact(std::pair<T, U> objects,
                     Contact const &contact) const noexcept {
    auto const data =
        std::pair{get_data(objects.first), get_data(objects.second)};
    auto const relative_velocity =
        get_velocity(data.first, contact.relative_positions[0]) -
        get_velocity(data.second, contact.relative_positions[1]);
    auto const separating_velocity = dot(contact.normal, relative_velocity);
    auto const tangential_velocity =
        relative_velocity - contact.normal * separating_velocity;
    auto const delta_velocity =
        get_friction_velocity_update(data, contact, tangential_velocity) +
        get_restitution_velocity_update(data, contact, separating_velocity);
    if (delta_velocity != Vec3f::zero()) {
      auto const I_inv_1 = get_inverse_inertia_tensor(data.first);
      auto const I_inv_2 = get_inverse_inertia_tensor(data.second);
      auto const delta_velocity_direction = normalize(delta_velocity);
      auto const w_1 =
          get_generalized_inverse_mass(data.first,
                                       I_inv_1,
                                       contact.relative_positions[0],
                                       delta_velocity_direction);
      auto const w_2 =
          get_generalized_inverse_mass(data.second,
                                       I_inv_2,
                                       contact.relative_positions[1],
                                       delta_velocity_direction);
      auto const impulse = delta_velocity / (w_1 + w_2);
      apply_impulse(
          data.first, I_inv_1, contact.relative_positions[0], impulse);
      apply_impulse(
          data.second, I_inv_2, contact.relative_positions[1], -impulse);
    }
  }

  template <typename T, typename U>
  Vec3f get_friction_velocity_update(
      std::pair<T, U> data,
      Contact const &contact,
      Vec3f const &tangential_velocity) const noexcept {
    if (tangential_velocity != Vec3f::zero()) {
      auto const friction_coefficient =
          0.5f * (get_dynamic_friction_coefficient(data.first) +
                  get_dynamic_friction_coefficient(data.second));
      auto const tangential_speed = length(tangential_velocity);
      auto const delta_velocity_direction =
          -tangential_velocity / tangential_speed;
      return delta_velocity_direction *
             min(friction_coefficient * contact.lambda_n *
                     _state->inverse_delta_time,
                 tangential_speed);
    } else {
      return Vec3f::zero();
    }
  }

  template <typename T, typename U>
  Vec3f
  get_restitution_velocity_update(std::pair<T, U> data,
                                  Contact const &contact,
                                  float separating_velocity) const noexcept {
    auto const restitution_coefficient = [&] {
      if (abs(separating_velocity) >
          _state->restitution_separating_velocity_threshold) {
        return 0.5f * (get_restitution_coefficient(data.first) +
                       get_restitution_coefficient(data.second));
      } else {
        return 0.0f;
      }
    }();
    return contact.normal *
           (-separating_velocity +
            min(-restitution_coefficient * contact.separating_velocity, 0.0f));
  }

  void apply_impulse(Particle_data *particle,
                     Mat3x3f const &,
                     Vec3f const &,
                     Vec3f const &impulse) const noexcept {
    particle->velocity += impulse * particle->inverse_mass;
  }

  void apply_impulse(Rigid_body_data *rigid_body,
                     Mat3x3f const &inverse_inertia_tensor,
                     Vec3f const &relative_position,
                     Vec3f const &impulse) const noexcept {
    rigid_body->velocity += impulse * rigid_body->inverse_mass;
    rigid_body->angular_velocity +=
        inverse_inertia_tensor * cross(relative_position, impulse);
  }

  void apply_impulse(Static_body_data *,
                     Mat3x3f const &,
                     Vec3f const &,
                     Vec3f const &) const noexcept {}

  Vec3f get_velocity(Particle_data *particle, Vec3f const &) const noexcept {
    return particle->velocity;
  }

  Vec3f get_velocity(Rigid_body_data *rigid_body,
                     Vec3f const &relative_position) const noexcept {
    return rigid_body->velocity +
           cross(rigid_body->angular_velocity, relative_position);
  }

  Vec3f get_velocity(Static_body_data *, Vec3f const &) const noexcept {
    return Vec3f::zero();
  }

  Mat3x3f get_inverse_inertia_tensor(Particle_data *) const noexcept {
    return Mat3x3f::zero();
  }

  Mat3x3f
  get_inverse_inertia_tensor(Rigid_body_data *rigid_body) const noexcept {
    auto const rotation = Mat3x3f::rotation(rigid_body->orientation);
    return rotation * rigid_body->inverse_inertia_tensor * transpose(rotation);
  }

  Mat3x3f get_inverse_inertia_tensor(Static_body_data *) const noexcept {
    return Mat3x3f::zero();
  }

  float get_generalized_inverse_mass(Particle_data *particle,
                                     Mat3x3f const &,
                                     Vec3f const &,
                                     Vec3f const &) const noexcept {
    return particle->inverse_mass;
  }

  float get_generalized_inverse_mass(Rigid_body_data *rigid_body,
                                     Mat3x3f const &inverse_inertia_tensor,
                                     Vec3f const &relative_position,
                                     Vec3f const &direction) const noexcept {
    auto const r_cross_n = cross(relative_position, direction);
    return rigid_body->inverse_mass +
           dot(r_cross_n, inverse_inertia_tensor * r_cross_n);
  }

  float get_generalized_inverse_mass(Static_body_data *,
                                     Mat3x3f const &,
                                     Vec3f const &,
                                     Vec3f const &) const noexcept {
    return 0.0f;
  }

  float
  get_dynamic_friction_coefficient(Particle_data *particle) const noexcept {
    return particle->material.dynamic_friction_coefficient;
  }

  float
  get_dynamic_friction_coefficient(Rigid_body_data *rigid_body) const noexcept {
    return rigid_body->material.dynamic_friction_coefficient;
  }

  float get_dynamic_friction_coefficient(
      Static_body_data *static_body) const noexcept {
    return static_body->material.dynamic_friction_coefficient;
  }

  float get_restitution_coefficient(Particle_data *particle) const noexcept {
    return particle->material.restitution_coefficient;
  }

  float
  get_restitution_coefficient(Rigid_body_data *rigid_body) const noexcept {
    return rigid_body->material.restitution_coefficient;
  }

  float
  get_restitution_coefficient(Static_body_data *static_body) const noexcept {
    return static_body->material.restitution_coefficient;
  }

  Particle_data *get_data(Particle_handle particle) const noexcept {
    return _state->particles->data(particle);
  }

  Rigid_body_data *get_data(Rigid_body_handle rigid_body) const noexcept {
    return _state->rigid_bodies->data(rigid_body);
  }

  Static_body_data *get_data(Static_body_handle static_body) const noexcept {
    return _state->static_bodies->data(static_body);
  }

  Solve_state const *_state;
  Solve_chunk const *_chunk;
};

// integration constants
auto constexpr velocity_damping_factor = 0.99f;
auto constexpr waking_motion_epsilon = 1.0f / 256.0f;
auto constexpr waking_motion_initializer = 2.0f * waking_motion_epsilon;
auto constexpr waking_motion_limit = 8.0f * waking_motion_epsilon;
auto constexpr waking_motion_smoothing_factor = 7.0f / 8.0f;
} // namespace

class World::Impl {
public:
  friend class World;

  static constexpr std::size_t
  memory_requirement(World_create_info const &create_info) {
    return Stack_allocator<>::memory_requirement({
        decltype(_aabb_tree)::memory_requirement(
            create_info.max_aabb_tree_leaf_nodes,
            create_info.max_aabb_tree_internal_nodes),
        decltype(_neighbor_pairs)::memory_requirement(
            create_info.max_neighbor_pairs),
        decltype(_neighbor_pair_ptrs)::memory_requirement(
            2 * create_info.max_neighbor_pairs),
        decltype(_neighbor_groups)::memory_requirement(
            create_info.max_particles + create_info.max_rigid_bodies,
            create_info.max_neighbor_pairs,
            create_info.max_neighbor_groups),
        decltype(_neighbor_group_awake_indices)::memory_requirement(
            create_info.max_neighbor_groups),
        decltype(_coloring_bits)::memory_requirement(max_colors),
        decltype(_coloring_fringe)::memory_requirement(
            create_info.max_neighbor_pairs),
        decltype(_color_groups)::memory_requirement(
            create_info.max_neighbor_pairs),
        decltype(_solve_contacts)::memory_requirement(
            create_info.max_neighbor_pairs),
        decltype(_solve_chunks)::memory_requirement(
            create_info.max_neighbor_pairs),
        decltype(_position_solve_tasks)::memory_requirement(
            create_info.max_neighbor_pairs),
        decltype(_velocity_solve_tasks)::memory_requirement(
            create_info.max_neighbor_pairs),
    });
  }

  explicit Impl(World_create_info const &create_info)
      : _particles{create_info.max_particles},
        _static_bodies{create_info.max_static_bodies},
        _rigid_bodies{create_info.max_rigid_bodies},
        _gravitational_acceleration{create_info.gravitational_acceleration} {
    _block = util::System_allocator::instance()->alloc(
        memory_requirement(create_info));
    auto allocator = Stack_allocator<>{_block};
    _aabb_tree = make_aabb_tree<Aabb_tree_payload_t>(
                     allocator,
                     create_info.max_aabb_tree_leaf_nodes,
                     create_info.max_aabb_tree_internal_nodes)
                     .second;
    _neighbor_pairs = util::make_list<Neighbor_pair>(
                          allocator, create_info.max_neighbor_pairs)
                          .second;
    _neighbor_pair_ptrs = util::make_list<Neighbor_pair *>(
                              allocator, 2 * create_info.max_neighbor_pairs)
                              .second;
    _neighbor_groups =
        make_neighbor_group_storage(allocator,
                                    create_info.max_particles +
                                        create_info.max_rigid_bodies,
                                    create_info.max_neighbor_pairs,
                                    create_info.max_neighbor_groups)
            .second;
    _neighbor_group_awake_indices =
        util::make_list<std::uint32_t>(allocator,
                                       create_info.max_neighbor_groups)
            .second;
    _coloring_bits = util::make_bit_list(allocator, max_colors).second;
    _coloring_bits.resize(max_colors);
    _coloring_fringe = util::make_queue<Neighbor_pair *>(
                           allocator, create_info.max_neighbor_pairs)
                           .second;
    _color_groups =
        make_color_group_storage(allocator, create_info.max_neighbor_pairs)
            .second;
    _solve_contacts =
        util::make_list<Contact>(allocator, create_info.max_neighbor_pairs)
            .second;
    _solve_chunks =
        util::make_list<Solve_chunk>(allocator, create_info.max_neighbor_pairs)
            .second;
    _position_solve_tasks = util::make_list<Position_solve_task>(
                                allocator, create_info.max_neighbor_pairs)
                                .second;
    _velocity_solve_tasks = util::make_list<Velocity_solve_task>(
                                allocator, create_info.max_neighbor_pairs)
                                .second;
  }

  ~Impl() {
    _velocity_solve_tasks = {};
    _position_solve_tasks = {};
    _solve_chunks = {};
    _solve_contacts = {};
    _color_groups = {};
    _coloring_fringe = {};
    _coloring_bits = {};
    _neighbor_group_awake_indices = {};
    _neighbor_groups = {};
    _neighbor_pair_ptrs = {};
    _neighbor_pairs = {};
    _aabb_tree = {};
    util::System_allocator::instance()->free(_block);
  }

  Particle_handle create(Particle_create_info const &create_info) {
    auto const bounds =
        Aabb{create_info.position - Vec3f::all(create_info.radius),
             create_info.position + Vec3f::all(create_info.radius)};
    auto const particle = _particles.create({
        .aabb_tree_node = _aabb_tree.create_leaf(bounds, Particle_handle{}),
        .motion_callback = create_info.motion_callback,
        .radius = create_info.radius,
        .inverse_mass = 1.0f / create_info.mass,
        .material = create_info.material,
        .previous_position = create_info.position,
        .position = create_info.position,
        .velocity = create_info.velocity,
        .waking_motion = waking_motion_initializer,
        .marked = false,
        .awake = true,
    });
    _particles.data(particle)->aabb_tree_node->payload = particle;
    return particle;
  }

  void destroy(Particle_handle particle) {
    _aabb_tree.destroy_leaf(_particles.data(particle)->aabb_tree_node);
    _particles.destroy(particle);
  }

  bool is_awake(Particle_handle particle) const noexcept {
    return _particles.data(particle)->awake;
  }

  float get_waking_motion(Particle_handle particle) const noexcept {
    return _particles.data(particle)->waking_motion;
  }

  math::Vec3f get_position(Particle_handle particle) const noexcept {
    return _particles.data(particle)->position;
  }

  Rigid_body_handle create(Rigid_body_create_info const &create_info) {
    auto const transform =
        Mat3x4f::rigid(create_info.position, create_info.orientation);
    auto const bounds = physics::bounds(create_info.shape, transform);
    auto const rigid_body = _rigid_bodies.create({
        .aabb_tree_node = _aabb_tree.create_leaf(bounds, Rigid_body_handle{}),
        .motion_callback = create_info.motion_callback,
        .shape = create_info.shape,
        .inverse_mass = 1.0f / create_info.mass,
        .inverse_inertia_tensor = inverse(create_info.inertia_tensor),
        .material = create_info.material,
        .previous_position = create_info.position,
        .position = create_info.position,
        .velocity = create_info.velocity,
        .previous_orientation = create_info.orientation,
        .orientation = create_info.orientation,
        .angular_velocity = create_info.angular_velocity,
        .waking_motion = waking_motion_initializer,
        .marked = false,
        .awake = true,
    });
    _rigid_bodies.data(rigid_body)->aabb_tree_node->payload = rigid_body;
    return rigid_body;
  }

  void destroy(Rigid_body_handle rigid_body) {
    _aabb_tree.destroy_leaf(_rigid_bodies.data(rigid_body)->aabb_tree_node);
    _rigid_bodies.destroy(rigid_body);
  }

  bool is_awake(Rigid_body_handle rigid_body) const noexcept {
    return _rigid_bodies.data(rigid_body)->awake;
  }

  float get_waking_motion(Rigid_body_handle rigid_body) const noexcept {
    return _rigid_bodies.data(rigid_body)->waking_motion;
  }

  math::Vec3f get_position(Rigid_body_handle rigid_body) const noexcept {
    return _rigid_bodies.data(rigid_body)->position;
  }

  math::Quatf get_orientation(Rigid_body_handle rigid_body) const noexcept {
    return _rigid_bodies.data(rigid_body)->orientation;
  }

  Static_body_handle create(Static_body_create_info const &create_info) {
    auto const transform =
        Mat3x4f::rigid(create_info.position, create_info.orientation);
    auto const transform_inverse = rigid_inverse(transform);
    auto const bounds = physics::bounds(create_info.shape, transform);
    auto const static_body = _static_bodies.create({
        .aabb_tree_node = _aabb_tree.create_leaf(bounds, Static_body_handle{}),
        .shape = create_info.shape,
        .material = create_info.material,
        .transform = transform,
        .inverse_transform = transform_inverse,
    });
    _static_bodies.data(static_body)->aabb_tree_node->payload = static_body;
    return static_body;
  }

  void destroy(Static_body_handle handle) {
    _aabb_tree.destroy_leaf(_static_bodies.data(handle)->aabb_tree_node);
    _static_bodies.destroy(handle);
  }

  void simulate(World const &world, World_simulate_info const &simulate_info) {
    build_aabb_tree(simulate_info.delta_time);
    clear_neighbor_pairs();
    find_neighbor_pairs();
    assign_neighbor_pairs();
    find_neighbor_groups();
    _neighbor_group_awake_indices.clear();
    _color_groups.clear();
    for (auto j = std::size_t{}; j != _neighbor_groups.group_count(); ++j) {
      if (update_neighbor_group_awake_states(j)) {
        _neighbor_group_awake_indices.emplace_back(j);
        color_neighbor_group(j);
      }
    }
    _color_groups.reserve();
    assign_color_groups();
    auto const h = simulate_info.delta_time / simulate_info.substep_count;
    auto const h_inv = 1.0f / h;
    auto solve_state = Solve_state{
        nullptr,
        &_particles,
        &_rigid_bodies,
        &_static_bodies,
        h_inv,
        2.0f * length(_gravitational_acceleration) * h,
    };
    _solve_contacts.clear();
    _solve_chunks.clear();
    _position_solve_tasks.clear();
    _velocity_solve_tasks.clear();
    for (auto i = std::size_t{}; i != max_colors; ++i) {
      auto const color = static_cast<std::uint16_t>(i);
      auto const group = _color_groups.group(color);
      if (!group.empty()) {
        for (auto j = std::size_t{}; j < group.size();
             j += max_solve_chunk_size) {
          auto const chunk_size = min(group.size() - j, max_solve_chunk_size);
          _solve_chunks.push_back(
              {.pairs = group.data() + j,
               .contacts = _solve_contacts.data() + _solve_contacts.size(),
               .size = chunk_size});
          _solve_contacts.resize(_solve_contacts.size() + chunk_size);
          _position_solve_tasks.emplace_back(&solve_state,
                                             &_solve_chunks.back());
          _velocity_solve_tasks.emplace_back(&solve_state,
                                             &_solve_chunks.back());
        }
      } else {
        break;
      }
    }
    auto const time_compensated_velocity_damping_factor =
        pow(velocity_damping_factor, h);
    auto const time_compensating_waking_motion_smoothing_factor =
        1.0f - pow(1.0f - waking_motion_smoothing_factor, h);
    for (auto i = 0; i < simulate_info.substep_count; ++i) {
      integrate(h,
                time_compensated_velocity_damping_factor,
                time_compensating_waking_motion_smoothing_factor);
      solve_positions(*simulate_info.thread_pool, solve_state);
      derive_velocities(h_inv);
      solve_velocities(*simulate_info.thread_pool, solve_state);
    }
    call_particle_motion_callbacks(world);
    call_dynamic_rigid_body_motion_callbacks(world);
  }

private:
  bool is_marked(Particle_handle particle) const noexcept {
    return _particles.data(particle)->marked;
  }

  bool is_marked(Rigid_body_handle rigid_body) const noexcept {
    return _rigid_bodies.data(rigid_body)->marked;
  }

  void set_marked(Particle_handle particle, bool marked = true) noexcept {
    _particles.data(particle)->marked = marked;
  }

  void set_marked(Rigid_body_handle rigid_body, bool marked = true) noexcept {
    _rigid_bodies.data(rigid_body)->marked = marked;
  }

  void set_unmarked(Particle_handle particle) noexcept {
    set_marked(particle, false);
  }

  void set_unmarked(Rigid_body_handle rigid_body) noexcept {
    set_marked(rigid_body, false);
  }

  std::span<Neighbor_pair *const>
  get_neighbor_pairs(Particle_handle particle) const noexcept {
    auto const data = _particles.data(particle);
    return {data->neighbor_pairs, data->neighbor_count};
  }

  std::span<Neighbor_pair *const>
  get_neighbor_pairs(Rigid_body_handle rigid_body) const noexcept {
    auto const data = _rigid_bodies.data(rigid_body);
    return {data->neighbor_pairs, data->neighbor_count};
  }

  void assign_neighbor_pair(Particle_handle particle,
                            Neighbor_pair *neighbor_pair) noexcept {
    auto const data = _particles.data(particle);
    data->neighbor_pairs[data->neighbor_count++] = neighbor_pair;
  }

  void assign_neighbor_pair(Rigid_body_handle rigid_body,
                            Neighbor_pair *neighbor_pair) noexcept {
    auto const data = _rigid_bodies.data(rigid_body);
    data->neighbor_pairs[data->neighbor_count++] = neighbor_pair;
  }

  void build_aabb_tree(float delta_time) {
    auto const constant_safety_term = 0.0f;
    auto const velocity_safety_factor = 2.0f;
    auto const gravity_safety_factor = 2.0f;
    auto const gravity_safety_term = gravity_safety_factor *
                                     length(_gravitational_acceleration) *
                                     delta_time * delta_time;
    _particles.for_each([&](Particle_handle, Particle_data *data) {
      auto const half_extents = Vec3f::all(
          data->radius + constant_safety_term +
          velocity_safety_factor * length(data->velocity) * delta_time +
          gravity_safety_term);
      data->aabb_tree_node->bounds = {data->position - half_extents,
                                      data->position + half_extents};
    });
    _rigid_bodies.for_each([&](Rigid_body_handle, Rigid_body_data *data) {
      data->aabb_tree_node->bounds = expand(
          bounds(data->shape,
                 Mat3x4f::rigid(data->position, data->orientation)),
          constant_safety_term +
              velocity_safety_factor * length(data->velocity) * delta_time +
              gravity_safety_term);
    });
    _aabb_tree.build();
  }

  void clear_neighbor_pairs() {
    auto const reset_neighbor_count = [](auto const, auto const data) {
      data->neighbor_count = 0;
    };
    _particles.for_each(reset_neighbor_count);
    _rigid_bodies.for_each(reset_neighbor_count);
    _neighbor_pair_ptrs.clear();
    _neighbor_pairs.clear();
    _neighbor_groups.clear();
  }

  void find_neighbor_pairs() {
    _aabb_tree.for_each_overlapping_leaf_pair([this](Aabb_tree_payload_t const
                                                         &first_payload,
                                                     Aabb_tree_payload_t const
                                                         &second_payload) {
      std::visit(
          [this, &second_payload](auto &&first_handle) {
            using T = std::decay_t<decltype(first_handle)>;
            if constexpr (std::is_same_v<T, Particle_handle>) {
              std::visit(
                  [this, first_handle](auto &&second_handle) {
                    using U = std::decay_t<decltype(second_handle)>;
                    if constexpr (std::is_same_v<U, Particle_handle>) {
                      _neighbor_pairs.push_back({
                          .objects = {first_handle.value, second_handle.value},
                          .type = Object_pair_type::particle_particle,
                      });
                      ++_particles.data(first_handle)->neighbor_count;
                      ++_particles.data(second_handle)->neighbor_count;
                    } else if constexpr (std::is_same_v<U, Rigid_body_handle>) {
                      _neighbor_pairs.push_back({
                          .objects = {first_handle.value, second_handle.value},
                          .type = Object_pair_type::particle_rigid_body,
                      });
                      ++_particles.data(first_handle)->neighbor_count;
                      ++_rigid_bodies.data(second_handle)->neighbor_count;
                    } else {
                      static_assert(std::is_same_v<U, Static_body_handle>);
                      _neighbor_pairs.push_back({
                          .objects = {first_handle.value, second_handle.value},
                          .type = Object_pair_type::particle_static_body,
                      });
                      ++_particles.data(first_handle)->neighbor_count;
                    }
                  },
                  second_payload);
            } else if constexpr (std::is_same_v<T, Rigid_body_handle>) {
              std::visit(
                  [this, first_handle](auto &&second_handle) {
                    using U = std::decay_t<decltype(second_handle)>;
                    if constexpr (std::is_same_v<U, Particle_handle>) {
                      _neighbor_pairs.push_back(
                          {.objects = {second_handle.value, first_handle.value},
                           .type = Object_pair_type::particle_rigid_body});
                      ++_particles.data(second_handle)->neighbor_count;
                      ++_rigid_bodies.data(first_handle)->neighbor_count;
                    } else if constexpr (std::is_same_v<U, Rigid_body_handle>) {
                      _neighbor_pairs.push_back({
                          .objects = {first_handle.value, second_handle.value},
                          .type = Object_pair_type::rigid_body_rigid_body,
                      });
                      ++_rigid_bodies.data(first_handle)->neighbor_count;
                      ++_rigid_bodies.data(second_handle)->neighbor_count;
                    } else {
                      static_assert(std::is_same_v<U, Static_body_handle>);
                      _neighbor_pairs.push_back({
                          .objects = {first_handle.value, second_handle.value},
                          .type = Object_pair_type::rigid_body_static_body,
                      });
                      ++_rigid_bodies.data(first_handle)->neighbor_count;
                    }
                  },
                  second_payload);
            } else {
              static_assert(std::is_same_v<T, Static_body_handle>);
              std::visit(
                  [this, first_handle](auto &&second_handle) {
                    using U = std::decay_t<decltype(second_handle)>;
                    if constexpr (std::is_same_v<U, Particle_handle>) {
                      _neighbor_pairs.push_back({
                          .objects = {second_handle.value, first_handle.value},
                          .type = Object_pair_type::particle_static_body,
                      });
                      ++_particles.data(second_handle)->neighbor_count;
                    } else if constexpr (std::is_same_v<U, Rigid_body_handle>) {
                      _neighbor_pairs.push_back({
                          .objects = {second_handle.value, first_handle.value},
                          .type = Object_pair_type::rigid_body_static_body,
                      });
                      ++_rigid_bodies.data(second_handle)->neighbor_count;
                    }
                  },
                  second_payload);
            }
          },
          first_payload);
    });
  }

  void assign_neighbor_pairs() {
    auto const alloc_neighbor_pairs = [this](auto const, auto const data) {
      data->neighbor_pairs = _neighbor_pair_ptrs.end();
      _neighbor_pair_ptrs.resize(_neighbor_pair_ptrs.size() +
                                 data->neighbor_count);
      data->neighbor_count = 0;
    };
    _particles.for_each(alloc_neighbor_pairs);
    _rigid_bodies.for_each(alloc_neighbor_pairs);
    for (auto &pair : _neighbor_pairs) {
      switch (pair.type) {
      case Object_pair_type::particle_particle: {
        assign_neighbor_pair(Particle_handle{pair.objects[0]}, &pair);
        assign_neighbor_pair(Particle_handle{pair.objects[1]}, &pair);
        continue;
      }
      case Object_pair_type::particle_rigid_body: {
        assign_neighbor_pair(Particle_handle{pair.objects[0]}, &pair);
        assign_neighbor_pair(Rigid_body_handle{pair.objects[1]}, &pair);
        continue;
      }
      case Object_pair_type::particle_static_body: {
        assign_neighbor_pair(Particle_handle{pair.objects[0]}, &pair);
        continue;
      }
      case Object_pair_type::rigid_body_rigid_body: {
        assign_neighbor_pair(Rigid_body_handle{pair.objects[0]}, &pair);
        assign_neighbor_pair(Rigid_body_handle{pair.objects[1]}, &pair);
        continue;
      }
      case Object_pair_type::rigid_body_static_body: {
        assign_neighbor_pair(Rigid_body_handle{pair.objects[0]}, &pair);
        continue;
      }
      }
    }
  }

  void find_neighbor_groups() {
    auto const unmark = [](auto const, auto const data) {
      data->marked = false;
    };
    _particles.for_each(unmark);
    _rigid_bodies.for_each(unmark);
    auto const visitor = [this](auto &&handle) {
      using T = std::decay_t<decltype(handle)>;
      for (auto const pair : get_neighbor_pairs(handle)) {
        if constexpr (std::is_same_v<T, Particle_handle>) {
          switch (pair->type) {
          case Object_pair_type::particle_particle: {
            auto const neighbor_handle = Particle_handle{
                pair->objects[pair->objects[0] == handle.value]};
            auto const neighbor_data = _particles.data(neighbor_handle);
            if (!neighbor_data->marked) {
              neighbor_data->marked = true;
              _neighbor_groups.add_to_group(neighbor_handle);
            }
            if (pair->color == color_unmarked) {
              pair->color = color_marked;
              _neighbor_groups.add_to_group(pair);
            }
            continue;
          }
          case Object_pair_type::particle_rigid_body: {
            auto const neighbor_handle = Rigid_body_handle{pair->objects[1]};
            auto const neighbor_data = _rigid_bodies.data(neighbor_handle);
            if (!neighbor_data->marked) {
              neighbor_data->marked = true;
              _neighbor_groups.add_to_group(neighbor_handle);
            }
            if (pair->color == color_unmarked) {
              pair->color = color_marked;
              _neighbor_groups.add_to_group(pair);
            }
            continue;
          }
          case Object_pair_type::particle_static_body: {
            _neighbor_groups.add_to_group(pair);
            continue;
          }
          default:
            continue;
          }
        } else {
          static_assert(std::is_same_v<T, Rigid_body_handle>);
          switch (pair->type) {
          case Object_pair_type::particle_rigid_body: {
            auto const neighbor_handle = Particle_handle{pair->objects[0]};
            auto const neighbor_data = _particles.data(neighbor_handle);
            if (!neighbor_data->marked) {
              neighbor_data->marked = true;
              _neighbor_groups.add_to_group(neighbor_handle);
            }
            if (pair->color == color_unmarked) {
              pair->color = color_marked;
              _neighbor_groups.add_to_group(pair);
            }
            continue;
          }
          case Object_pair_type::rigid_body_rigid_body: {
            auto const neighbor_handle = Rigid_body_handle{
                pair->objects[pair->objects[0] == handle.value]};
            auto const neighbor_data = _rigid_bodies.data(neighbor_handle);
            if (!neighbor_data->marked) {
              neighbor_data->marked = true;
              _neighbor_groups.add_to_group(neighbor_handle);
            }
            if (pair->color == color_unmarked) {
              pair->color = color_marked;
              _neighbor_groups.add_to_group(pair);
            }
            continue;
          }
          case Object_pair_type::rigid_body_static_body: {
            _neighbor_groups.add_to_group(pair);
            continue;
          }
          default:
            continue;
          }
        }
      }
    };
    auto fringe_index = std::size_t{};
    auto const find_neighbor_group = [&, this](auto const seed_handle,
                                               auto const seed_data) {
      if (!seed_data->marked) {
        seed_data->marked = true;
        _neighbor_groups.begin_group();
        _neighbor_groups.add_to_group(seed_handle);
        do {
          std::visit(visitor, _neighbor_groups.object(fringe_index));
        } while (++fringe_index != _neighbor_groups.object_count());
      }
    };
    _particles.for_each(find_neighbor_group);
    _rigid_bodies.for_each(find_neighbor_group);
  }

  bool update_neighbor_group_awake_states(std::size_t group_index) {
    auto const &group = _neighbor_groups.group(group_index);
    auto contains_awake = false;
    auto contains_sleeping = false;
    auto sleepable = true;
    for (auto i = group.objects_begin;
         (sleepable || !contains_awake || !contains_sleeping) &&
         i != group.objects_end;
         ++i) {
      auto const object = _neighbor_groups.object(i);
      std::visit(
          [&](auto &&handle) {
            using T = std::decay_t<decltype(handle)>;
            if constexpr (std::is_same_v<T, Particle_handle>) {
              auto const data = _particles.data(handle);
              if (data->awake) {
                contains_awake = true;
                if (data->waking_motion > waking_motion_epsilon) {
                  sleepable = false;
                }
              } else {
                contains_sleeping = true;
              }
            } else {
              static_assert(std::is_same_v<T, Rigid_body_handle>);
              auto const data = _rigid_bodies.data(handle);
              if (data->awake) {
                contains_awake = true;
                if (data->waking_motion > waking_motion_epsilon) {
                  sleepable = false;
                }
              } else {
                contains_sleeping = true;
              }
            }
          },
          object);
    }
    if (contains_awake) {
      if (sleepable) {
        for (auto i = group.objects_begin; i != group.objects_end; ++i) {
          auto const object = _neighbor_groups.object(i);
          std::visit(
              [&](auto &&handle) {
                using T = std::decay_t<decltype(handle)>;
                if constexpr (std::is_same_v<T, Particle_handle>) {
                  auto const data = _particles.data(handle);
                  if (data->awake) {
                    data->velocity = Vec3f::zero();
                    data->awake = false;
                  }
                } else {
                  static_assert(std::is_same_v<T, Rigid_body_handle>);
                  auto const data = _rigid_bodies.data(handle);
                  if (data->awake) {
                    data->velocity = Vec3f::zero();
                    data->angular_velocity = Vec3f::zero();
                    data->awake = false;
                  }
                }
              },
              object);
        }
        return false;
      } else {
        if (contains_sleeping) {
          for (auto i = group.objects_begin; i != group.objects_end; ++i) {
            auto const object = _neighbor_groups.object(i);
            std::visit(
                [&](auto &&handle) {
                  using T = std::decay_t<decltype(handle)>;
                  if constexpr (std::is_same_v<T, Particle_handle>) {
                    auto const data = _particles.data(handle);
                    if (!data->awake) {
                      data->waking_motion = waking_motion_initializer;
                      data->awake = true;
                    }
                  } else {
                    static_assert(std::is_same_v<T, Rigid_body_handle>);
                    auto const data = _rigid_bodies.data(handle);
                    if (!data->awake) {
                      data->waking_motion = waking_motion_initializer;
                      data->awake = true;
                    }
                  }
                },
                object);
          }
        }
        return true;
      }
    } else {
      return false;
    }
  }

  void color_neighbor_group(std::size_t group_index) {
    auto const &group = _neighbor_groups.group(group_index);
    auto const begin = group.neighbor_pairs_begin;
    auto const end = group.neighbor_pairs_end;
    if (begin == end) {
      return;
    }
    for (auto i = begin; i != end; ++i) {
      _neighbor_groups.neighbor_pair(i)->color = color_unmarked;
    }
    auto const seed_pair = _neighbor_groups.neighbor_pair(begin);
    seed_pair->color = color_marked;
    _coloring_fringe.emplace_back(seed_pair);
    do {
      auto const pair = _coloring_fringe.front();
      _coloring_fringe.pop_front();
      auto neighbors = std::array<std::span<Neighbor_pair *const>, 2>{};
      switch (pair->type) {
      case Object_pair_type::particle_particle:
        neighbors[0] = get_neighbor_pairs(Particle_handle{pair->objects[0]});
        neighbors[1] = get_neighbor_pairs(Particle_handle{pair->objects[1]});
        break;
      case Object_pair_type::particle_rigid_body:
        neighbors[0] = get_neighbor_pairs(Particle_handle{pair->objects[0]});
        neighbors[1] = get_neighbor_pairs(Rigid_body_handle{pair->objects[1]});
        break;
      case Object_pair_type::particle_static_body:
        neighbors[0] = get_neighbor_pairs(Particle_handle{pair->objects[0]});
        break;
      case Object_pair_type::rigid_body_rigid_body:
        neighbors[0] = get_neighbor_pairs(Rigid_body_handle{pair->objects[0]});
        neighbors[1] = get_neighbor_pairs(Rigid_body_handle{pair->objects[1]});
        break;
      case Object_pair_type::rigid_body_static_body:
        neighbors[0] = get_neighbor_pairs(Rigid_body_handle{pair->objects[0]});
        break;
      }
      _coloring_bits.reset();
      for (auto i{0}; i != 2; ++i) {
        for (auto const neighbor : neighbors[i]) {
          if (neighbor->color == color_unmarked) {
            neighbor->color = color_marked;
            _coloring_fringe.emplace_back(neighbor);
          } else if (neighbor->color != color_marked) {
            _coloring_bits.set(neighbor->color);
          }
        }
      }
      for (auto i{std::size_t{}}; i != max_colors; ++i) {
        if (!_coloring_bits.get(i)) {
          auto const color = static_cast<std::uint16_t>(i);
          pair->color = color;
          _color_groups.count(color);
          break;
        }
      }
      if (pair->color == color_marked) {
        throw std::runtime_error{"Failed to color neighbor group"};
      }
    } while (!_coloring_fringe.empty());
  }

  void assign_color_groups() {
    for (auto const i : _neighbor_group_awake_indices) {
      auto const &group = _neighbor_groups.group(i);
      auto const begin = group.neighbor_pairs_begin;
      auto const end = group.neighbor_pairs_end;
      for (auto j = begin; j != end; ++j) {
        _color_groups.push_back(_neighbor_groups.neighbor_pair(j));
      }
    }
  }

  void integrate(float delta_time,
                 float velocity_damping_factor,
                 float waking_motion_smoothing_factor) noexcept {
    for (auto const i : _neighbor_group_awake_indices) {
      integrate_neighbor_group(i,
                               delta_time,
                               velocity_damping_factor,
                               waking_motion_smoothing_factor);
    }
  }

  void integrate_neighbor_group(std::size_t group_index,
                                float delta_time,
                                float velocity_damping_factor,
                                float waking_motion_smoothing_factor) {
    auto const &group = _neighbor_groups.group(group_index);
    for (auto i = group.objects_begin; i != group.objects_end; ++i) {
      std::visit(
          [&](auto &&object) {
            integrate(object,
                      delta_time,
                      velocity_damping_factor,
                      waking_motion_smoothing_factor);
          },
          _neighbor_groups.object(i));
    }
  }

  void integrate(Particle_handle particle,
                 float delta_time,
                 float velocity_damping_factor,
                 float waking_motion_smoothing_factor) {
    auto const data = _particles.data(particle);
    data->previous_position = data->position;
    data->velocity += delta_time * _gravitational_acceleration;
    data->velocity *= velocity_damping_factor;
    data->position += delta_time * data->velocity;
    data->waking_motion =
        min((1.0f - waking_motion_smoothing_factor) * data->waking_motion +
                waking_motion_smoothing_factor * length_squared(data->velocity),
            waking_motion_limit);
  }

  void integrate(Rigid_body_handle rigid_body,
                 float delta_time,
                 float velocity_damping_factor,
                 float waking_motion_smoothing_factor) {
    auto const data = _rigid_bodies.data(rigid_body);
    data->previous_position = data->position;
    data->previous_orientation = data->orientation;
    data->velocity += delta_time * _gravitational_acceleration;
    data->velocity *= velocity_damping_factor;
    data->position += delta_time * data->velocity;
    data->angular_velocity *= velocity_damping_factor;
    data->orientation +=
        Quatf{0.0f, 0.5f * delta_time * data->angular_velocity} *
        data->orientation;
    data->orientation = normalize(data->orientation);
    data->waking_motion =
        min((1.0f - waking_motion_smoothing_factor) * data->waking_motion +
                waking_motion_smoothing_factor *
                    (length_squared(data->velocity) +
                     length_squared(data->angular_velocity)),
            waking_motion_limit);
  }

  void solve_positions(util::Thread_pool &thread_pool,
                       Solve_state &solve_state) {
    auto solve_chunk_index = std::size_t{};
    for (auto j = std::size_t{}; j != max_colors; ++j) {
      auto const color = static_cast<std::uint16_t>(j);
      auto const group = _color_groups.group(color);
      if (!group.empty()) {
        auto const solve_chunk_count =
            (group.size() + max_solve_chunk_size - 1) / max_solve_chunk_size;
        auto latch = std::latch{static_cast<std::ptrdiff_t>(solve_chunk_count)};
        solve_state.latch = &latch;
        for (auto k = std::size_t{}; k != solve_chunk_count; ++k) {
          thread_pool.push(&_position_solve_tasks[solve_chunk_index + k]);
        }
        for (;;) {
          if (latch.try_wait()) {
            break;
          }
        }
        solve_chunk_index += solve_chunk_count;
      } else {
        break;
      }
    }
  }

  void derive_velocities(float inverse_delta_time) {
    for (auto const i : _neighbor_group_awake_indices) {
      derive_neighbor_group_velocities(i, inverse_delta_time);
    }
  }

  void derive_neighbor_group_velocities(std::size_t group_index,
                                        float inverse_delta_time) {
    auto const &group = _neighbor_groups.group(group_index);
    for (auto i = group.objects_begin; i != group.objects_end; ++i) {
      std::visit(
          [&](auto &&object) { derive_velocity(object, inverse_delta_time); },
          _neighbor_groups.object(i));
    }
  }

  void derive_velocity(Particle_handle particle, float inverse_delta_time) {
    auto const data = _particles.data(particle);
    data->velocity =
        (data->position - data->previous_position) * inverse_delta_time;
  }

  void derive_velocity(Rigid_body_handle rigid_body, float inverse_delta_time) {
    auto const data = _rigid_bodies.data(rigid_body);
    data->velocity =
        (data->position - data->previous_position) * inverse_delta_time;
    auto const delta_orientation =
        data->orientation * conjugate(data->previous_orientation);
    data->angular_velocity = 2.0f * delta_orientation.v * inverse_delta_time;
    data->angular_velocity *= delta_orientation.w >= 0.0f ? 1.0f : -1.0f;
  }

  void solve_velocities(util::Thread_pool &thread_pool,
                        Solve_state &solve_state) {
    auto solve_chunk_index = std::size_t{};
    for (auto j = std::size_t{}; j != max_colors; ++j) {
      auto const color = static_cast<std::uint16_t>(j);
      auto const group = _color_groups.group(color);
      if (!group.empty()) {
        auto const solve_chunk_count =
            (group.size() + max_solve_chunk_size - 1) / max_solve_chunk_size;
        auto latch = std::latch{static_cast<std::ptrdiff_t>(solve_chunk_count)};
        solve_state.latch = &latch;
        for (auto k = std::size_t{}; k != solve_chunk_count; ++k) {
          thread_pool.push(&_velocity_solve_tasks[solve_chunk_index + k]);
        }
        for (;;) {
          if (latch.try_wait()) {
            break;
          }
        }
        solve_chunk_index += solve_chunk_count;
      } else {
        break;
      }
    }
  }

  void call_particle_motion_callbacks(World const &world) {
    _particles.for_each([&](Particle_handle particle, Particle_data *data) {
      if (data->motion_callback != nullptr) {
        data->motion_callback->on_particle_motion(world, particle);
      }
    });
  }

  void call_dynamic_rigid_body_motion_callbacks(World const &world) {
    _rigid_bodies.for_each(
        [&](Rigid_body_handle rigid_body, Rigid_body_data *data) {
          if (data->motion_callback != nullptr) {
            data->motion_callback->on_rigid_body_motion(world, rigid_body);
          }
        });
  }

  Block _block;
  Aabb_tree<Aabb_tree_payload_t> _aabb_tree;
  Particle_storage _particles;
  Static_body_storage _static_bodies;
  Rigid_body_storage _rigid_bodies;
  List<Neighbor_pair> _neighbor_pairs;
  List<Neighbor_pair *> _neighbor_pair_ptrs;
  Neighbor_group_storage _neighbor_groups;
  List<std::uint32_t> _neighbor_group_awake_indices;
  Bit_list _coloring_bits;
  Queue<Neighbor_pair *> _coloring_fringe;
  Color_group_storage _color_groups;
  List<Contact> _solve_contacts;
  List<Solve_chunk> _solve_chunks;
  List<Position_solve_task> _position_solve_tasks;
  List<Velocity_solve_task> _velocity_solve_tasks;
  Vec3f _gravitational_acceleration;
};

World::World(World_create_info const &create_info)
    : _impl{std::make_unique<Impl>(create_info)} {}

World::~World() {}

Particle_handle World::create(Particle_create_info const &create_info) {
  return _impl->create(create_info);
}

void World::destroy(Particle_handle particle) {
  _impl->destroy(particle);
}

bool World::is_awake(Particle_handle particle) const noexcept {
  return _impl->is_awake(particle);
}

float World::get_waking_motion(Particle_handle particle) const noexcept {
  return _impl->get_waking_motion(particle);
}

math::Vec3f World::get_position(Particle_handle particle) const noexcept {
  return _impl->get_position(particle);
}

Rigid_body_handle World::create(Rigid_body_create_info const &create_info) {
  return _impl->create(create_info);
}

void World::destroy(Rigid_body_handle handle) {
  _impl->destroy(handle);
}

bool World::is_awake(Rigid_body_handle rigid_body) const noexcept {
  return _impl->is_awake(rigid_body);
}

float World::get_waking_motion(Rigid_body_handle rigid_body) const noexcept {
  return _impl->get_waking_motion(rigid_body);
}

math::Vec3f World::get_position(Rigid_body_handle rigid_body) const noexcept {
  return _impl->get_position(rigid_body);
}

math::Quatf
World::get_orientation(Rigid_body_handle rigid_body) const noexcept {
  return _impl->get_orientation(rigid_body);
}

Static_body_handle World::create(Static_body_create_info const &create_info) {
  return _impl->create(create_info);
}

void World::destroy(Static_body_handle static_rigid_body) {
  _impl->destroy(static_rigid_body);
}

void World::simulate(World_simulate_info const &simulate_info) {
  return _impl->simulate(*this, simulate_info);
}
} // namespace physics
} // namespace marlon