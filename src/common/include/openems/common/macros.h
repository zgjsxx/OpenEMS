// src/common/include/openems/common/macros.h
#pragma once

#include <memory>

#define OPENEMS_DISABLE_COPY(Class) \
  Class(const Class&) = delete; \
  Class& operator=(const Class&) = delete;

#define OPENEMS_DISABLE_MOVE(Class) \
  Class(Class&&) = delete; \
  Class& operator=(Class&&) = delete;

#define OPENEMS_DISABLE_COPY_AND_MOVE(Class) \
  OPENEMS_DISABLE_COPY(Class) \
  OPENEMS_DISABLE_MOVE(Class)

#define OPENEMS_DECLARE_PTR(Class) \
  using Class##Ptr = std::shared_ptr<Class>; \
  using Class##WeakPtr = std::weak_ptr<Class>;

#define OPENEMS_CREATE_PTR(Class, ...) \
  template <typename... Args> \
  static Class##Ptr Class##Create(Args&&... args) { \
    return std::make_shared<Class>(std::forward<Args>(args)...); \
  }
