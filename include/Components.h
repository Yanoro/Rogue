#pragma once

#include "Defaults.h"
#include "raylib-cpp.hpp"
#include <flecs.h>
#include <string>
#include <vector>

struct Render {};

struct ScreenPosition : public raylib::Vector2 {
  ScreenPosition(float x = 0.0f, float y = 0.0f) : raylib::Vector2(x, y) {}
  ScreenPosition(const raylib::Vector2 &v) : raylib::Vector2(v) {}

  bool operator==(const ScreenPosition &other) const {
    return x == other.x and y == other.y;
  }
};

struct GamePosition {
  int x;
  int y;

  operator raylib::Vector2() const {
    return raylib::Vector2(static_cast<float>(x), static_cast<float>(y));
  }

  bool operator==(const GamePosition &other) const {
    return x == other.x && y == other.y;
  }
  GamePosition &operator+=(const GamePosition &other) {
    this->x += other.x;
    this->y += other.y;
    return *this;
  }
  GamePosition operator+(const GamePosition &other) const {
    GamePosition result = *this;
    result += other;
    return result;
  }
  GamePosition &operator-=(const GamePosition &other) {
    this->x -= other.x;
    this->y -= other.y;
    return *this;
  }
  GamePosition operator-(const GamePosition &other) const {
    GamePosition result = *this;
    result -= other;
    return result;
  }
};

struct MaxSpeed {
  float value;
};

struct Velocity : public raylib::Vector2 {
  Velocity(float x = 0.0f, float y = 0.0f) : raylib::Vector2(x, y) {}
  Velocity(const raylib::Vector2 &v) : raylib::Vector2(v) {}
  Velocity(const ::Vector2 &v) : raylib::Vector2(v) {}

  bool operator==(const Velocity &other) const {
    return x == other.x and y == other.y;
  }
};

struct Acceleration : public raylib::Vector2 {
  Acceleration(float x = 0.0f, float y = 0.0f) : raylib::Vector2(x, y) {}
  Acceleration(const raylib::Vector2 &v) : raylib::Vector2(v) {}
  Acceleration(const ::Vector2 &v) : raylib::Vector2(v) {}

  bool operator==(const Acceleration &other) const {
    return x == other.x and y == other.y;
  }
};

struct Friction {
  float value;
};

struct BlocksTile {};

// Entity can walk through Blocked tiles
struct Intangible {};

struct Frame {
  raylib::Rectangle frameRect;
  double duration;
};

struct TargetPath {
  std::vector<GamePosition> path;
};

struct DrawAscii {
  char ch;
  Color characterColor;
  Color backgroundColor;
  size_t width;
  size_t height;
};

// Whats actually taken into account when handling collision
struct Hitbox {
  size_t width;
  size_t height;
};

// TODO: Right now it's not very clear who owns drawAscii,
// probably should create a resourceManager class in the future
struct Tile {
  std::string name;
  bool blocksTile;
  DrawAscii *ascii;
  Hitbox hitbox;
};

struct Location {
  std::string name;
  std::string description;
  GamePosition pos;
  int width;
  int height;
};

// Reusable reflection support for std::vector
template <typename Elem, typename Vector = std::vector<Elem>>
inline flecs::opaque<Vector, Elem> std_vector_support(flecs::world &world) {
  return flecs::opaque<Vector, Elem>()
      .as_type(world.vector<Elem>())
      .serialize([](const flecs::serializer *s, const Vector *data) {
        for (const auto &el : *data) {
          s->value(el);
        }
        return 0;
      })
      .count([](const Vector *data) { return data->size(); })
      .resize([](Vector *data, size_t size) { data->resize(size); })
      .ensure_element([](Vector *data, size_t elem) {
        if (data->size() <= elem) {
          data->resize(elem + 1);
        }
        return &data->data()[elem];
      });
}

inline void RegisterComponents(flecs::world &ecs) {
  ecs.component<Render>();

  ecs.component<raylib::Vector2>().member<float>("x").member<float>("y");

  ecs.component<ScreenPosition>().member<float>("x").member<float>("y");

  ecs.component<GamePosition>().member<int>("x").member<int>("y");

  ecs.component<MaxSpeed>().member<float>("value");

  ecs.component<Velocity>().member<float>("x").member<float>("y");

  ecs.component<Acceleration>().member<float>("x").member<float>("y");

  ecs.component<Friction>().member<float>("value");

  ecs.component<BlocksTile>();
  ecs.component<Intangible>();

  ecs.component<raylib::Rectangle>()
      .member<float>("x")
      .member<float>("y")
      .member<float>("width")
      .member<float>("height");

  ecs.component<Frame>()
      .member<raylib::Rectangle>("frameRect")
      .member<double>("duration");

  ecs.component<std::vector<GamePosition>>().opaque(
      std_vector_support<GamePosition>);

  ecs.component<TargetPath>().member<std::vector<GamePosition>>("path");

  ecs.component<std::vector<Frame>>().opaque(std_vector_support<Frame>);

  ecs.component<Color>()
      .member<unsigned char>("r")
      .member<unsigned char>("g")
      .member<unsigned char>("b")
      .member<unsigned char>("a");

  ecs.component<std::string>()
      .opaque(flecs::String)
      .serialize([](const flecs::serializer *s, const std::string *data) {
        const char *str = data->c_str();
        return s->value(flecs::String, &str);
      })
      .assign_string(
          [](std::string *data, const char *value) { *data = value; });

  ecs.component<Tile>()
      .member<std::string>("name")
      .member<char>("ch")
      .member<Color>("characterColor")
      .member<Color>("backgroundColor");
}
