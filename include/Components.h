#pragma once

#include "raylib-cpp.hpp"
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

const float DEFAULT_MAXSPEED = 100.0f;

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

const float DEFAULT_FRICTION = 10.0f;

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

struct CharacterAnimation {
  raylib::Texture *texture;
  unsigned int frameCount;
  std::vector<Frame> frames;
  unsigned int currentFrame = 0;
  double lastFrameTime = 0;

  CharacterAnimation(raylib::Texture *tex = nullptr,
                     unsigned int frameCount = 1)
      : texture(tex), frameCount(frameCount), currentFrame(0),
        lastFrameTime(0.0) {
    frames.reserve(frameCount);
  }
};

struct Drawable {
  raylib::Texture *texture;
  raylib::Rectangle srcRect;
};

struct Tile {};
