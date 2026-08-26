#pragma once

#include <memory>

namespace exchange::matching_engine {

class CommandResultBatch;
using ImmutableCommandResultBatch = std::shared_ptr<const CommandResultBatch>;

} // namespace exchange::matching_engine
