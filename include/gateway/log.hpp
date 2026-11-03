#pragma once

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace gateway {

inline void log(nlohmann::json fields, spdlog::level::level_enum level = spdlog::level::info) {
    spdlog::log(level, "{}", fields.dump());
}

} // namespace gateway
