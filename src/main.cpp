// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026, Amit Gefen

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <print>
#include <string>
#include <thread>

#include "ntp_client.hpp"

constexpr std::chrono::milliseconds kSleepDuration{500};

// NTP counts seconds from 1900-01-01, Unix time from 1970-01-01. Deriving the
// epoch from the calendar types makes the offset self-evidently correct,
// instead of hand-counting leap years.
constexpr auto kNtpEpoch =
    std::chrono::sys_days{std::chrono::year{1900} / std::chrono::January / 1};

// Only the integer seconds component is used; the fraction is irrelevant for
// display. The signed cast keeps the addition below from wrapping, because
// the NTP epoch offset is negative.
static constexpr std::chrono::sys_seconds
ToUnixEpoch(amitgdev::network::NtpTimestamp ntp_timestamp) noexcept {
  return std::chrono::sys_seconds{
      std::chrono::seconds{static_cast<int64_t>(ntp_timestamp.seconds)} +
      kNtpEpoch.time_since_epoch()};
}

int main() {
  try {
    const std::array<std::string, 3> ntp_hosts = {
        "time.google.com",
        "time.facebook.com",
        "time.apple.com",
    };

    std::println("test GetNtpTimestamp API (with error handling):");
    for (const auto& hostname : ntp_hosts) {
      // transform() keeps the error channel untouched, so conversion and
      // error handling stay in one place.
      const auto result =
          amitgdev::network::GetNtpTimestamp(hostname).transform(ToUnixEpoch);
      if (result) {
        std::println("SUCCESS: {:%F %T} UTC (host: {})", *result, hostname);
      } else {
        std::println("ERROR: {} (host: {})", result.error().message(),
                     hostname);
      }
      // Spacing requests avoids tripping the servers' rate limiting.
      std::this_thread::sleep_for(kSleepDuration);
    }

    // Demonstrates the failure path with a host that cannot resolve.
    std::println("\ntest GetNtpTimestamp API (invalid NTP host):");
    if (const auto invalid_result =
            amitgdev::network::GetNtpTimestamp("invalid.host.example");
        !invalid_result) {
      std::println("Expected error occurred: {}",
                   invalid_result.error().message());
    }
    return EXIT_SUCCESS;
  } catch (const std::exception& e) {
    // fputs instead of std::println: the error path must not throw again.
    std::fputs(e.what(), stderr);
    std::fputs("\n", stderr);
    return EXIT_FAILURE;
  }
}
