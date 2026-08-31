#ifndef AMITGDEV_NTPCLIENT_HPP_
#define AMITGDEV_NTPCLIENT_HPP_

/*
    ntp_client.h
    Copyright (c) 2024-2026, Amit Gefen

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to
    deal in the Software without restriction, including without limitation the
    rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
    sell copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
*/

#include <cstdint>
#include <expected>
#include <string>
#include <system_error>

namespace amitgdev::network {

// Holds the NTP timestamp (seconds since Jan 1, 1900)
struct NtpTimestamp {
  uint32_t seconds;
  uint32_t fraction;
};

// Error codes for NTP operations
enum class NtpError : std::uint8_t {
  kSuccess = 0,           // Everything worked
  kWsaInitFailed,         // WSAStartup failed
  kInvalidHostname,       // Input hostname is malformed
  kHostResolutionFailed,  // DNS resolution failed
  kSocketCreationFailed,  // socket() failed
  kTimeoutFailed,         // setsockopt(SO_RCVTIMEO/SO_SNDTIMEO) failed
  kSendFailed,            // sendto() failed
  kReceiveFailed,         // recvfrom() failed
  kInvalidResponse,       // response didn't parse/validate
};

// Error category for NTP errors
struct ErrorCategory : std::error_category {
  [[nodiscard]] const char* name() const noexcept override;
  [[nodiscard]] std::string message(int error_value) const override;
};

// Get the NTP error category instance
const ErrorCategory& GetErrorCategory() noexcept;

// Create an error code from NtpError
std::error_code MakeErrorCode(NtpError ntp_error) noexcept;

// Main API
std::expected<NtpTimestamp, std::error_code>
GetNtpTimestamp(const std::string& hostname) noexcept;

}  // namespace amitgdev::network

#endif  // AMITGDEV_NTPCLIENT_HPP_