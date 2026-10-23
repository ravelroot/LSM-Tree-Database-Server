#pragma once

#include <string_view>

#include "arca/status.hpp"

namespace arca {

class Iterator {
 public:
  Iterator() = default;
  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;
  virtual ~Iterator() = default;

  virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  virtual void Seek(std::string_view target) = 0;
  virtual void Next() = 0;
  virtual std::string_view key() const = 0;
  virtual std::string_view value() const = 0;
  virtual Status status() const = 0;
};

}
