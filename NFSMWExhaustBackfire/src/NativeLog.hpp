#pragma once

namespace nfsmw_exhaust::native_log {

bool open(const char* path) noexcept;
void write(const char* format, ...) noexcept;

}  // namespace nfsmw_exhaust::native_log
