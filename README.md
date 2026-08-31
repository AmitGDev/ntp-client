# NTPClient

Offers a simple way to retrieve the current timestamp from Network Time Protocol (NTP) servers on Windows.

## Overview

NtpClient is a modern C++23 implementation for retrieving the current timestamp from an NTP server on Windows systems. It exposes a single function, `GetNtpTimestamp()`, that handles DNS resolution, the UDP exchange, and response validation, and returns the server's transmit timestamp as an `NtpTimestamp` (seconds + fraction fields) or an error.

## Features

- Simplicity: Acquiring the current timestamp from an NTP server requires only one function call
- Efficiency: Uses a single UDP request/response exchange
- Platform: Windows only (Winsock2)
- Protocol: Packet layout follows RFC 5905; requests are sent as NTP version 4, client mode
- Spoofing resistance: Each request carries a random nonce that the reply must echo, and the socket accepts datagrams only from the queried server
- Error Handling: Returns `std::expected` with a `std::error_code` from a dedicated NTP error category, no exceptions

## Usage

1. Add `ntp_client.hpp` and `ntp_client.cpp` to your project and include the header:

```cpp
#include "ntp_client.hpp"
```

2. Call `GetNtpTimestamp`, passing the NTP server hostname:

```cpp
auto ntp_timestamp = amitgdev::network::GetNtpTimestamp("time.google.com");
```

The function returns `std::expected<NtpTimestamp, std::error_code>`: the NTP timestamp on success, or an error code whose `message()` describes the failure.

## Example Usage

See `main.cpp` for a comprehensive example. It queries several public NTP servers, converts the NTP timestamp (seconds since 1900-01-01) to a Unix-epoch `std::chrono::sys_seconds` for display, and demonstrates the error path with a hostname that cannot be resolved.

## Technical Features

### Modern C++ Features

- C++23 `std::expected<T, E>`: Type-safe error handling without exceptions
- C++23 `std::byteswap()`: Byte order conversion for the NTP packet fields
- C++23 `std::to_underlying()`: Enum-to-integer conversion when building error codes
- C++20 `std::endian`: Compile-time endianness detection, so no byte swapping is compiled in on big-endian targets
- C++20 integer comparison (`std::cmp_less`, `std::cmp_not_equal`): Sign-safe comparisons of socket handles and byte counts
- Designated initializers: Readable struct initialization (`addrinfo hints{.ai_family = AF_INET, ...}`)
- `constexpr`: Compile-time evaluation for the timestamp and message helpers

### Error Handling

- Custom error category: `ErrorCategory` implements `std::error_category` (name `"ntp"`)
- Strongly-typed error enum: `NtpError` enum class with descriptive error codes:
  - `kSuccess`: Operation completed successfully
  - `kWsaInitFailed`: Winsock initialization failed
  - `kInvalidHostname`: Hostname is empty or longer than 253 characters
  - `kHostResolutionFailed`: DNS resolution failed, or the result is not an IPv4 address
  - `kSocketCreationFailed`: Could not create the UDP socket
  - `kTimeoutFailed`: Failed to configure socket timeouts
  - `kSendFailed`: Failed to send the NTP request
  - `kReceiveFailed`: Failed to receive the NTP response (including a timeout, or an ICMP "port unreachable" from a server that is not listening)
  - `kInvalidResponse`: The response failed validation, including a packet shorter than a full NTP message and an origin timestamp that does not echo the request
  - `kConnectFailed`: Could not associate the UDP socket with the server address
  - `kRandomFailed`: The OS random generator failed while creating the request nonce
- Stable values: New error codes are appended at the end of the enum, so existing numeric values do not change
- Public helpers: `GetErrorCategory()` and `MakeErrorCode()` let callers compare returned errors against `NtpError` values
- Consistent propagation: The public API and the internal send/receive steps return `std::expected`
- Note: Winsock error codes are captured internally, but the public API reports only the `NtpError` value, so the underlying `WSAGetLastError()` code is not exposed

### RAII and Resource Management

- `Socket` class: RAII wrapper that closes the socket automatically; non-copyable, movable using `std::exchange()`
- `WinsockScope` class: Guarantees `WSAStartup`/`WSACleanup` pairing; neither copyable nor movable
- `std::unique_ptr` with a custom deleter: Automatic `freeaddrinfo()` cleanup

### API Design

- Separation of Concerns:
  - Returns the raw `NtpTimestamp` structure (seconds + fraction fields)
  - Client code controls time interpretation and format conversion (see the epoch conversion in `main.cpp`)
  - Clear boundary between protocol handling and application logic

- Networking:
  - `getaddrinfo()` for DNS resolution with hint-based filtering (IPv4, UDP)
  - `<winsock2.h>` and `<ws2tcpip.h>` included before `<Windows.h>`
  - IPv4 only: Non-IPv4 results are rejected, and only the first resolved address is used
  - Connected UDP socket: `connect()` pins the socket to the server, so the OS drops datagrams from any other source

- Bit Manipulation:
  - The leap indicator, version, and mode share one byte, handled with explicit masks and shifts
  - Setters and getters: `SetLeapIndicator()`/`LeapIndicator()`, `SetVersion()`/`Version()`, `SetMode()`/`Mode()`

### Robustness and Safety

- Timeouts:
  - Send and receive timeouts (`SO_SNDTIMEO`, `SO_RCVTIMEO`) are set on the socket, so a silent server cannot block the call indefinitely
  - The timeout is a fixed 5 second default, applied through `Socket::SetTimeout()`; it is not a caller-facing parameter

- Request nonce:
  - The request's transmit timestamp is a random, non-zero 64-bit value from the Windows CNG generator (`BCryptGenRandom`)
  - A genuine server copies it into the origin timestamp of its reply, so a spoofed, replayed, or late packet is rejected
  - Using a random value instead of the local clock also avoids disclosing the client's clock to the server

- Validation:
  - `NtpMessage::IsValid()` checks the response:
    - Server mode (mode == 4)
    - Version 3 or 4 (some legacy servers answer a v4 request with v3)
    - Leap indicator is not the alarm condition (LI != 3)
    - Stratum within 1-15 (stratum 0, Kiss-o'-Death, is rejected)
    - Origin timestamp equals the request's transmit timestamp
    - Non-zero transmit timestamp
  - Hostname validation: Rejects empty hostnames and hostnames longer than 253 characters
  - Response size verification: Ensures a complete NTP packet was received
  - Address family validation: Confirms IPv4 address resolution

- Type Safety:
  - `[[nodiscard]]` on accessors and internal operations
  - `noexcept` on the public API and internal helpers
  - Const-correctness throughout

### Known Limitations

- The returned value is the server's transmit timestamp. Round-trip delay and clock offset are not computed, so the accuracy is limited by network latency
- The response is not cryptographically authenticated: The nonce and the connected socket defend against off-path spoofing, but an attacker who can observe or modify traffic between you and the server can still forge a reply (NTP symmetric-key MACs and NTS are not supported)
- The timestamp is interpreted as NTP era 0 (32-bit seconds since 1900-01-01), which wraps in 2036

### Code Organization

- Constants and Configuration:
  - Named constants for ports, timeouts, protocol values, and bit masks
  - Kept in an anonymous namespace together with the internal classes

- Namespace Structure:
  - Primary namespace: `amitgdev::network`
  - Anonymous namespace for internal implementation details (`Timestamp`, `NtpMessage`, `Socket`, `WinsockScope`)
  - Public API: `GetNtpTimestamp()`, `NtpTimestamp`, `NtpError`, `ErrorCategory`, `GetErrorCategory()`, `MakeErrorCode()`

- Endianness Handling:
  - `SwapEndiansIfNLE()` ("Swap Endians If Native [is] Little Endian") converts between network byte order and host order
  - Implemented with `if constexpr`, so it costs nothing on big-endian systems

## Requirements

- A C++23-compatible compiler and standard library
- Windows with the Winsock2 and CNG (`bcrypt`) libraries
- `ntp_client.cpp` links `Ws2_32.lib` and `Bcrypt.lib` through `#pragma comment(lib, ...)`, which MSVC and clang-cl honor; with other toolchains, link against `ws2_32` and `bcrypt` manually

## Build and Run

- [Build on Windows](https://github.com/AmitGDev/cpp-ci-template#build-on-windows)

## Code Style

- [C++](https://github.com/AmitGDev/cpp-coding-standard)
- [Python](https://github.com/AmitGDev/cpp-ci-template#python)
- [CMake](https://github.com/AmitGDev/cpp-ci-template#cmake)

## License

MIT License. Copyright © 2024-2026 Amit Gefen.

---

**Last Updated:** 2026-10-31  
**Maintainer:** AmitGDev
