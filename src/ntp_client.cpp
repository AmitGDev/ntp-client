/*
    ntp_client.cpp
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

#include "ntp_client.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
// NOLINTNEXTLINE(misc-include-cleaner)
#include <Windows.h>  // Must be included after Winsock2.h
#include <bcrypt.h>   // Requires Windows base type declarations

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Bcrypt.lib")

namespace amitgdev::network {

using namespace std::chrono_literals;

namespace {

// Connection constants
constexpr uint16_t kNtpPort = 123;
constexpr auto kDefaultTimeout = 5s;
constexpr size_t kMaxHostnameLength = 253;

// NTP Protocol constants
constexpr uint8_t kNtpVersion = 4;  // RFC 5905
// Servers echo the request version, but some legacy servers answer a v4 request
// with v3. The 48-byte header is identical, so both are safe to accept.
constexpr uint8_t kMinAcceptedVersion = 3;
constexpr uint8_t kClientMode = 3;
constexpr uint8_t kServerMode = 4;
constexpr uint8_t kAlarmCondition = 3;
constexpr uint8_t kMaxValidStratum = 15;

// Leap Indicator Shifts and Masks
constexpr uint8_t kLeapMask = 0x03;
constexpr uint8_t kLeapShift = 6;
constexpr uint8_t kLeapInvMask = 0x3F;

// Version Shifts and Masks
constexpr uint8_t kVersionMask = 0x07;
constexpr uint8_t kVersionShift = 3;
constexpr uint8_t kVersionInvMask = 0xC7;

// Mode Shifts and Masks
constexpr uint8_t kModeMask = 0x07;
constexpr uint8_t kModeInvMask = 0xF8;

}  // namespace

// **** Error Handling Implementation ****

[[nodiscard]] const char* ErrorCategory::name() const noexcept {
  return "ntp";
}

[[nodiscard]] std::string ErrorCategory::message(const int error_value) const {
  switch (static_cast<NtpError>(error_value)) {
    case NtpError::kSuccess:
      return "Success";
    case NtpError::kWsaInitFailed:
      return "WSA initialization failed";
    case NtpError::kInvalidHostname:
      return "Invalid hostname";
    case NtpError::kHostResolutionFailed:
      return "Host resolution failed";
    case NtpError::kSocketCreationFailed:
      return "Socket creation failed";
    case NtpError::kTimeoutFailed:
      return "Failed to set socket timeout";
    case NtpError::kSendFailed:
      return "Send failed";
    case NtpError::kReceiveFailed:
      return "Receive failed";
    case NtpError::kInvalidResponse:
      return "Invalid response";
    case NtpError::kConnectFailed:
      return "Failed to connect UDP socket to server";
    case NtpError::kRandomFailed:
      return "Failed to generate request nonce";
    default:
      return "Unknown error";
  }
}

const ErrorCategory& GetErrorCategory() noexcept {
  static const ErrorCategory instance;
  return instance;
}

std::error_code MakeErrorCode(const NtpError ntp_error) noexcept {
  return {std::to_underlying(ntp_error), GetErrorCategory()};
}

namespace {

// **** Timestamp class ****

// NTP Fixed-Point Timestamp Format.
// Note: RFC 5905 (http://tools.ietf.org/html/rfc5905).
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
class Timestamp final {
 public:
  uint32_t seconds_{0};   // Seconds since Jan 1, 1900.
  uint32_t fraction_{0};  // Fractional part of seconds.

  // Used to match a reply's origin timestamp against our request nonce.
  constexpr bool operator==(const Timestamp&) const noexcept = default;

  // Returns a cryptographically random, non-zero timestamp to use as the
  // request's transmit timestamp.
  //
  // The client does not need its real clock in the request: the server copies
  // the transmit timestamp into the origin field of its reply, so an
  // unpredictable value acts as a nonce that makes off-path response spoofing
  // harder (RFC 5905, Section 8). It also avoids disclosing the client's
  // local clock to the server.
  [[nodiscard]] static std::optional<Timestamp> MakeRandom() noexcept {
    std::array<uint32_t, 2> words{};

    // NOLINTNEXTLINE(misc-include-cleaner)
    const NTSTATUS status = BCryptGenRandom(
        nullptr, reinterpret_cast<uint8_t*>(words.data()),
        static_cast<uint32_t>(sizeof(words)), BCRYPT_USE_SYSTEM_PREFERRED_RNG);

    if (!BCRYPT_SUCCESS(status)) {
      return std::nullopt;
    }

    Timestamp result{};
    result.seconds_ = words.at(0);
    result.fraction_ = words.at(1);

    // An all-zero transmit timestamp means "not set" in NTP, and the reply
    // validation rejects it, so never produce it.
    if (result.Zero()) {
      result.fraction_ = 1;
    }

    return result;
  }

  // Swap the timestamp fields between Network Byte Order
  // (Big-Endian) and the Host's Native Byte Order. If the
  // Native Byte Order is Big-Endian (like Network Byte Order),
  // no operation is performed, as the formats already match.
  constexpr void SwapEndiansIfNLE() noexcept {
    if constexpr (std::endian::native == std::endian::little) {
      seconds_ = std::byteswap(seconds_);
      fraction_ = std::byteswap(fraction_);
    }
  }

  [[nodiscard]] constexpr bool Zero() const noexcept {
    return seconds_ == 0 && fraction_ == 0;
  }
};

// NOLINTEND(misc-non-private-member-variables-in-classes)

// **** NtpMessage class ****

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
class NtpMessage final {
 public:
  //
  // The NTP packet header format, depicted in Figure 8 of RFC 5905, is as
  // follows. NTPv3 and NTPv4 share the same 48-byte header; only the VN field
  // differs. This client sends and expects the bare header: the extension
  // fields and the MAC (Key Identifier + digest) below are never requested, so
  // a conforming server doesn't add them to its reply.
  //
  //       0                   1                   2                   3
  //       0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
  //      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  //      |LI | VN  |Mode |    Stratum     |     Poll      |  Precision   |
  //      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  //      |                         Root Delay                            |
  //      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  //      |                         Root Dispersion                       |
  //      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  //      |                          Reference ID                         |
  //      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  //      |                                                               |
  //      +                     Reference Timestamp (64)                  +
  //      |                                                               |
  //      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  //      |                                                               |
  //      +                      Origin Timestamp (64)                    +
  //      |                                                               |
  //      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  //      |                                                               |
  //      +                      Receive Timestamp (64)                   +
  //      |                                                               |
  //      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  //      |                                                               |
  //      +                      Transmit Timestamp (64)                  +
  //      |                                                               |
  //      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  //      |                                                               |
  //      .                                                               .
  //      .                    Extension Field 1 (variable)               .
  //      .                                                               .
  //      |                                                               |
  //      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  //      |                                                               |
  //      .                                                               .
  //      .                    Extension Field 2 (variable)               .
  //      .                                                               .
  //      |                                                               |
  //      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  //      |                          Key Identifier                       |
  //      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  //      |                                                               |
  //      |                            dgst (128)                         |
  //      |                                                               |
  //      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  //

  uint8_t li_vn_mode_{0};

  uint8_t stratum_{0};
  uint8_t poll_{0};
  uint8_t precision_{0};

  uint32_t root_delay_{0};
  uint32_t root_dispersion_{0};
  uint8_t reference_id_[4]{0};

  Timestamp ref_{};
  Timestamp orig_{};
  Timestamp rx_{};
  Timestamp tx_{};

  constexpr NtpMessage() = default;

  constexpr void SwapEndiansIfNLE() noexcept {
    ref_.SwapEndiansIfNLE();   // Reference Timestamp (64)
    orig_.SwapEndiansIfNLE();  // Origin Timestamp (64)
    rx_.SwapEndiansIfNLE();    // Receive Timestamp (64)
    tx_.SwapEndiansIfNLE();    // Transmit Timestamp (64)

    // Root Delay and Root Dispersion are multi-byte fields too. Reference ID
    // is deliberately left alone: it is an opaque byte string, not a number.
    if constexpr (std::endian::native == std::endian::little) {
      root_delay_ = std::byteswap(root_delay_);
      root_dispersion_ = std::byteswap(root_dispersion_);
    }
  }

  // NOLINTEND(misc-non-private-member-variables-in-classes)

  // Sets the Mode (e.g., 3 for client request).
  void SetMode(const uint8_t mode) noexcept {
    li_vn_mode_ = static_cast<uint8_t>(
        (static_cast<unsigned int>(li_vn_mode_) & kModeInvMask) |
        (static_cast<unsigned int>(mode) & kModeMask));
  }

  // Returns the Mode (Provides access for validation,
  // e.g., checking for Mode 4 server reply).
  [[nodiscard]] uint8_t Mode() const noexcept {
    return li_vn_mode_ & kModeMask;
  }

  // Sets the Version (3 or 4; this client sends 4).
  void SetVersion(const uint8_t version) noexcept {
    li_vn_mode_ = static_cast<uint8_t>(
        (static_cast<unsigned int>(li_vn_mode_) & kVersionInvMask) |
        ((static_cast<unsigned int>(version) & kVersionMask) << kVersionShift));
  }

  // Returns the Version (Provides access for validation/logging).
  [[nodiscard]] uint8_t Version() const noexcept {
    return (static_cast<uint8_t>(li_vn_mode_ >> kVersionShift)) & kVersionMask;
  }

  // Sets the Leap Indicator (e.g., 0 for no warning).
  void SetLeapIndicator(const uint8_t leap_indicator) noexcept {
    li_vn_mode_ = static_cast<uint8_t>(
        (static_cast<unsigned int>(li_vn_mode_) & kLeapInvMask) |
        ((static_cast<unsigned int>(leap_indicator) & kLeapMask)
         << kLeapShift));
  }

  // Returns the Leap Indicator (Provides access for validation, e.g., checking
  // for alarm condition LI=3).
  [[nodiscard]] uint8_t LeapIndicator() const noexcept {
    return (static_cast<uint8_t>(li_vn_mode_ >> kLeapShift)) & kLeapMask;
  }

  // Validates the NTP response message against the request that triggered it.
  //
  // `request_tx` is the transmit timestamp we sent. A genuine server copies it
  // into the origin field, so a mismatch means the packet is a spoof, a replay
  // or a late reply to an earlier request. Stratum 0 (Kiss-o'-Death) and
  // unsynchronized servers (LI=3) are rejected as they carry no usable time.
  [[nodiscard]] bool IsValid(const Timestamp& request_tx) const noexcept {
    const auto version = Version();

    return Mode() == kServerMode && version >= kMinAcceptedVersion &&
           version <= kNtpVersion && LeapIndicator() != kAlarmCondition &&
           stratum_ > 0 && stratum_ <= kMaxValidStratum &&
           orig_ == request_tx && !tx_.Zero();
  }

  // Receives the NTP message from the given socket
  // and returns the number of bytes received or an error code.
  // The socket must be connect()ed to the server so the OS drops datagrams
  // from any other source.
  [[nodiscard]] std::expected<int, std::error_code>
  Receive(const SOCKET socket) noexcept {
    const auto received_count = recv(socket, reinterpret_cast<char*>(this),
                                     static_cast<int>(sizeof(*this)), 0);

    if (received_count == SOCKET_ERROR) {
      return std::unexpected{
          std::error_code{WSAGetLastError(), std::system_category()}};
    }

    if (std::cmp_less(received_count, sizeof(*this))) {
      return std::unexpected{MakeErrorCode(NtpError::kInvalidResponse)};
    }

    // Network Byte Order -> Host Native Byte Order (after reception)
    SwapEndiansIfNLE();

    return received_count;
  }

  // Sends the NTP message on the given connected socket
  // and returns the number of bytes sent or an error code.
  [[nodiscard]] std::expected<int, std::error_code>
  Send(const SOCKET socket) const noexcept {
    // Work on a copy so this object keeps host byte order for later use.
    NtpMessage network_msg = *this;

    // Host Native Byte Order -> Network Byte Order (before send)
    network_msg.SwapEndiansIfNLE();

    const auto sent_count =
        send(socket, reinterpret_cast<const char*>(&network_msg),
             static_cast<int>(sizeof(network_msg)), 0);

    if (sent_count == SOCKET_ERROR) {
      return std::unexpected{
          std::error_code{WSAGetLastError(), std::system_category()}};
    }

    return sent_count;
  }
};

// **** RAII Socket Wrapper ****

class Socket final {
 public:
  explicit Socket(const SOCKET sock = INVALID_SOCKET) noexcept
      : socket_{sock} {}

  ~Socket() noexcept {
    if (std::cmp_not_equal(socket_, INVALID_SOCKET)) {
      closesocket(socket_);
    }
  }

  // Non-copyable
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  // Move constructor
  Socket(Socket&& other) noexcept
      : socket_{std::exchange(other.socket_, INVALID_SOCKET)} {}

  // Move assignment operator
  Socket& operator=(Socket&& other) noexcept {
    if (this != &other) {
      if (std::cmp_not_equal(socket_, INVALID_SOCKET)) {
        closesocket(socket_);
      }
      socket_ = std::exchange(other.socket_, INVALID_SOCKET);
    }
    return *this;
  }

  [[nodiscard]] SOCKET Get() const noexcept { return socket_; }

  [[nodiscard]] bool Valid() const noexcept {
    return std::cmp_not_equal(socket_, INVALID_SOCKET);
  }

  [[nodiscard]] explicit operator bool() const noexcept { return Valid(); }

  // Configures send and receive timeouts for Winsock socket
  [[nodiscard]] bool
  SetTimeout(std::chrono::milliseconds timeout_ms) const noexcept {
    // NOLINTNEXTLINE(misc-include-cleaner)
    const auto timeout = static_cast<DWORD>(timeout_ms.count());

    const auto recv_result =
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    const auto send_result =
        setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    return recv_result != SOCKET_ERROR && send_result != SOCKET_ERROR;
  }

  // Associates the UDP socket with a single peer. No handshake happens; the
  // point is that the OS then discards inbound datagrams from any other
  // address or port, so only the queried server can answer us.
  [[nodiscard]] bool Connect(const sockaddr_in& server_addr) const noexcept {
    return connect(socket_, reinterpret_cast<const sockaddr*>(&server_addr),
                   static_cast<int>(sizeof(server_addr))) != SOCKET_ERROR;
  }

 private:
  SOCKET socket_;
};

// **** RAII Windows Sockets API (WSA) Wrapper ****

class WinsockScope final {
 public:
  WinsockScope() noexcept
      // NOLINTNEXTLINE(misc-include-cleaner)
      : error_code_(WSAStartup(MAKEWORD(2, 2), &data_)) {}

  ~WinsockScope() noexcept {
    if (error_code_ == 0) {
      WSACleanup();
    }
  }

  WinsockScope(const WinsockScope&) = delete;
  WinsockScope& operator=(const WinsockScope&) = delete;
  WinsockScope(WinsockScope&&) = delete;
  WinsockScope& operator=(WinsockScope&&) = delete;

  [[nodiscard]] const WSADATA& Data() const noexcept { return data_; }

  [[nodiscard]] int ErrorCode() const noexcept { return error_code_; }

  [[nodiscard]] bool Valid() const noexcept { return error_code_ == 0; }

  [[nodiscard]] explicit operator bool() const noexcept { return Valid(); }

 private:
  WSADATA data_{};
  int error_code_{0};
};

}  // anonymous namespace

// **** Main API ****

[[nodiscard]] std::expected<NtpTimestamp, std::error_code>
GetNtpTimestamp(const std::string& hostname) noexcept {
  if (hostname.empty() || hostname.size() > kMaxHostnameLength) {
    return std::unexpected{MakeErrorCode(NtpError::kInvalidHostname)};
  }

  // Initialize Winsock API
  const WinsockScope windows_sockets_api;
  if (!windows_sockets_api) {
    return std::unexpected{MakeErrorCode(NtpError::kWsaInitFailed)};
  }

  // The random transmit timestamp doubles as the nonce the server must echo.
  const auto request_nonce = Timestamp::MakeRandom();
  if (!request_nonce) {
    return std::unexpected{MakeErrorCode(NtpError::kRandomFailed)};
  }

  // Prepare NTP request message
  NtpMessage ntp_message{};
  ntp_message.SetVersion(kNtpVersion);
  ntp_message.SetMode(kClientMode);
  ntp_message.tx_ = *request_nonce;

  const addrinfo hints{
      .ai_flags = 0,
      .ai_family = AF_INET,
      .ai_socktype = SOCK_DGRAM,
      .ai_protocol = IPPROTO_UDP,
      .ai_addrlen = 0,
      .ai_canonname = nullptr,
      .ai_addr = nullptr,
      .ai_next = nullptr,
  };

  // Resolve hostname
  addrinfo* result = nullptr;
  if (getaddrinfo(hostname.c_str(), nullptr, &hints, &result) != 0) {
    return std::unexpected{MakeErrorCode(NtpError::kHostResolutionFailed)};
  }

  // Ensure addrinfo is freed
  const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addr_guard{
      result, freeaddrinfo};

  // Validate resolved address
  if (result == nullptr || result->ai_family != AF_INET) {
    return std::unexpected{MakeErrorCode(NtpError::kHostResolutionFailed)};
  }

  // Set server port
  auto* server_addr = reinterpret_cast<sockaddr_in*>(result->ai_addr);
  server_addr->sin_port = htons(kNtpPort);

  // Create UDP socket
  const Socket udp_socket{socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)};
  if (!udp_socket) {
    return std::unexpected{MakeErrorCode(NtpError::kSocketCreationFailed)};
  }

  // Set socket timeouts
  if (!udp_socket.SetTimeout(kDefaultTimeout)) {
    return std::unexpected{MakeErrorCode(NtpError::kTimeoutFailed)};
  }

  // Restrict the socket to the server so spoofed datagrams from other sources
  // never reach recv().
  if (!udp_socket.Connect(*server_addr)) {
    return std::unexpected{MakeErrorCode(NtpError::kConnectFailed)};
  }

  // Send NTP request
  if (const auto send_result = ntp_message.Send(udp_socket.Get());
      !send_result) {
    return std::unexpected{MakeErrorCode(NtpError::kSendFailed)};
  }

  // Receive NTP response
  NtpMessage response{};
  if (const auto recv_result = response.Receive(udp_socket.Get());
      !recv_result) {
    return std::unexpected{MakeErrorCode(NtpError::kReceiveFailed)};
  }

  // Validate NTP response, including that it answers this very request
  if (!response.IsValid(*request_nonce)) {
    return std::unexpected{MakeErrorCode(NtpError::kInvalidResponse)};
  }

  return NtpTimestamp{
      .seconds = response.tx_.seconds_,
      .fraction = response.tx_.fraction_,
  };
}

}  // namespace amitgdev::network
