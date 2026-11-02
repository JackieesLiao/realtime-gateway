#pragma once
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
namespace gateway {
inline void log(nlohmann::json fields) { spdlog::info("{}", fields.dump()); }
}
