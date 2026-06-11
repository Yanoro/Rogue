#pragma once

// TODO: Maybe make all debug windows this component
class Window {
public:
  virtual void Draw() = 0;
  virtual ~Window() = default;
};
