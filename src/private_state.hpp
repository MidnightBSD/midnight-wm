// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace midnight {
// Private, directory-relative, bounded reads and atomic replacement. Callers
// validate their format before writing and decide whether to preserve bad data.
std::expected<std::optional<std::string>, std::string> read_private_state(
    const std::filesystem::path& path, std::size_t limit);
std::expected<void, std::string> write_private_state(
    const std::filesystem::path& path, std::string_view contents);
std::expected<void, std::string> clear_private_state(const std::filesystem::path& path);
} // namespace midnight
