#pragma once

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include "arca/iterator.hpp"
#include "db/dbformat.hpp"

namespace arca {

std::unique_ptr<Iterator> NewErrorIterator(Status status);
std::unique_ptr<Iterator> NewEmptyIterator();
std::unique_ptr<Iterator> NewMergingIterator(const Comparator* cmp, std::vector<std::unique_ptr<Iterator>> children);

using BlockFunction = std::function<std::unique_ptr<Iterator>(std::string_view index_value)>;
std::unique_ptr<Iterator> NewTwoLevelIterator(std::unique_ptr<Iterator> index_iter, BlockFunction block_function);

}
