// main.cpp : This file contains the 'main' function. Program execution
// begins and ends there.
//

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <thread>

#include "ntp_client.hpp"

constexpr std::chrono::milliseconds kSleepDuration{500};

// Converts the NTP timestamp (seconds since Jan 1, 1900)
// to a std::chrono::sys_seconds point (seconds since Jan 1, 1970).
// Only the integer seconds component is used.
static std::chrono::sys_seconds
ToUnixEpoch(amitgdev::network::NtpTimestamp ntp_timestamp) noexcept {
  // Calculate the NTP to Unix epoch offset (Jan 1, 1900 to Jan 1, 1970).
  // This calculation is performed entirely at compile time.
  constexpr auto kSecondsInDay = std::chrono::seconds{24} * 60 * 60;
  constexpr auto kDaysIn70Years = 365 * 70;
  // 17 leap years between 1900 and 1970 (1904, 1908, ..., 1968).
  constexpr auto kLeapDays = 17;

  // Total seconds to offset:
  // (70 years * 365 days + 17 leap days) * 86400 seconds/day
  constexpr auto kNtpToUnixOffset =
      (kDaysIn70Years + kLeapDays) * kSecondsInDay;

  const auto unix_seconds =
      std::chrono::seconds{static_cast<int64_t>(ntp_timestamp.seconds)} -
      kNtpToUnixOffset;

  // Convert from duration to time_point
  return std::chrono::sys_seconds{unix_seconds};
}

int main() {  // NOLINT(bugprone-exception-escape)
  try {
    // Define the array of hosts once
    const std::array<std::string, 3> ntp_hosts = {
        "time.google.com",
        "time.facebook.com",
        "time.apple.com",
    };

    // **** New API with std::chrono and error handling: ****

    // Test several NTP hosts
    std::cout << "test new GetNtpTimestamp API (with error handling):\n";
    for (const auto& hostname : ntp_hosts) {
      auto result = amitgdev::network::GetNtpTimestamp(hostname);
      if (result) {
        // Convert sys_seconds to time_t for display
        const auto unix_epoch =
            std::chrono::system_clock::to_time_t(ToUnixEpoch(*result));
        std::cout << "SUCCESS: " << unix_epoch << " (host: " << hostname
                  << ")\n";
      } else {
        std::cout << "ERROR: " << result.error().message()
                  << " (host: " << hostname << ")\n";
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kSleepDuration));
    }

    // Test with an invalid NTP host (to demonstrate error handling):
    std::cout << "\ntest new GetNtpTimestamp API (invalid NTP host):\n";
    auto invalid_result =
        amitgdev::network::GetNtpTimestamp("invalid.host.example");
    if (!invalid_result) {
      std::cout << "Expected error occurred: "
                << invalid_result.error().message() << "\n";
    }
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "Unhandled exception: " << ex.what() << "\n";
    return 1;
  }
}