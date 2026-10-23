#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "arca/status.hpp"

namespace arca {

class WriteBatch {
 public:
  class Handler {
   public:
    virtual ~Handler() = default;
    virtual void Put(std::string_view key, std::string_view value) = 0;
    virtual void Delete(std::string_view key) = 0;
  };

  WriteBatch();

  void Put(std::string_view key, std::string_view value);
  void Delete(std::string_view key);
  void Clear();
  void Append(const WriteBatch& source);

  uint32_t Count() const;
  size_t ApproximateSize() const { return rep_.size(); }
  bool Empty() const { return Count() == 0; }

  Status Iterate(Handler* handler) const;

 private:
  friend class WriteBatchInternal;

  std::string rep_;
};

}
