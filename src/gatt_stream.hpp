#pragma once

// GATT-over-stream protocol shared by the USB CDC and TCP transports.
//
// A `Stream` must provide:
//      bool recv(std::span<uint8_t>);      // fill entire span; false if the connection is lost
//      void send(std::span<char const>);   // may buffer; must tolerate a lost connection
//      void flush();
//      char const* name() const;           // for logging
//      gatt::Writer writer() const;        // who's writing, see `gatt::Writer`

#include "gatt.hpp"
#include <array>
#include <algorithm>
#include <limits>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <type_traits>

namespace nevermore::gatt_stream {

using PayloadSize = uint16_t;

enum class Cmd : uint8_t {
    db_read = 0xfa,
    attr_read = 0xfb,
    attr_write = 0xfc,
};

static_assert(gatt::ATTR_SIZE_MAX <= std::numeric_limits<PayloadSize>::max());
static_assert(sizeof(PayloadSize) == 2, "protocol req");
static_assert(sizeof(Cmd) == 1, "protocol req");

// Implemented in `gatt_stream.cpp`.
// `read`/`write` serialise GATT access between stream transports (USB CDC, TCP).
void init();  // call before launching any stream transport
std::span<uint8_t const> database();
std::optional<uint16_t> read(uint16_t attr, std::span<uint8_t>);
std::optional<uint16_t> write(uint16_t attr, std::span<uint8_t const>, gatt::Writer);

namespace detail {

template <typename A>
    requires std::is_standard_layout_v<A> && std::is_trivial_v<A>
std::span<char const> raw_view(A const& a) {
    return std::span{reinterpret_cast<char const*>(&a), reinterpret_cast<char const*>(&a + 1)};
}

template <typename A, typename Stream>
    requires std::is_standard_layout_v<A> && std::is_trivial_v<A>
std::optional<A> recv(Stream& stream) {
    std::array<uint8_t, sizeof(A)> raw{};
    if (!stream.recv(raw)) return {};

    return *reinterpret_cast<A*>(raw.data());
}

template <typename A, typename Stream>
    requires std::is_standard_layout_v<A> && std::is_trivial_v<A>
void send(Stream& stream, A const& data) {
    stream.send(raw_view(data));
}

template <typename Stream>
void db_read(Stream& stream) {
    auto const db = database();
    send<Cmd>(stream, Cmd::db_read);
    send<PayloadSize>(stream, PayloadSize(db.size()));
    stream.send(std::span{reinterpret_cast<char const*>(db.data()), db.size()});
    stream.flush();
}

// on avg it takes ~800us to recv, handle, and send (USB CDC)
template <typename Stream>
void attr_read(Stream& stream) {
    auto const attr = recv<uint16_t>(stream);
    if (!attr) return;  // something went wrong, ignore

    std::array<uint8_t, gatt::ATTR_SIZE_MAX> data{};
    auto const size = gatt_stream::read(*attr, data);
    if (!size) {
        printf("ERR - %s - read to unknown attr=%d\n", stream.name(), *attr);
    }

    send<Cmd>(stream, Cmd::attr_read);
    send<uint16_t>(stream, *attr);
    send<bool>(stream, bool(size));
    send<PayloadSize>(stream, size ? *size : 0);
    if (size) {
        stream.send(std::span{reinterpret_cast<char const*>(data.begin()),
                reinterpret_cast<char const*>(data.begin()) + *size});
    }
    stream.flush();
}

// consume data in chunks instead of sucking it down char by char
template <typename Stream>
bool recv_drop(Stream& stream, size_t const amount, std::span<uint8_t> const scratch_pad) {
    for (auto n = amount; 0 < n;) {
        auto const amt = std::min<size_t>(scratch_pad.size(), n);
        if (!stream.recv(std::span{scratch_pad.begin(), scratch_pad.begin() + amt}))
            return false;  // lost connection, give up

        n -= amt;
    }

    return true;
}

template <typename Stream>
void attr_write(Stream& stream) {
    auto const attr = recv<uint16_t>(stream);
    if (!attr) return;
    auto const size = recv<uint16_t>(stream);
    if (!size) return;

    std::array<uint8_t, gatt::ATTR_SIZE_MAX> data{};

    // too big? eat data until completed, then report failure.
    if (gatt::ATTR_SIZE_MAX < *size) {
        printf("ERR - %s - oversized write attr=%d size=%d\n", stream.name(), *attr, *size);
        if (!recv_drop(stream, *size, data)) return;

        send<Cmd>(stream, Cmd::attr_write);
        send<uint16_t>(stream, *attr);
        send<bool>(stream, false);
        stream.flush();
        return;
    }

    std::span data_subset{data.begin(), data.begin() + *size};
    if (!stream.recv(data_subset)) return;  // EOF while reading payload

    auto result = gatt_stream::write(*attr, data_subset, stream.writer());
    if (!result) {
        printf("ERR - %s - write to unknown attr=%d size=%d\n", stream.name(), *attr, *size);
    }

    send<Cmd>(stream, Cmd::attr_write);
    send<uint16_t>(stream, *attr);
    send<bool>(stream, result == 0);
    stream.flush();
}

}  // namespace detail

// Handle one command byte. Non-command bytes are ignored.
template <typename Stream>
void dispatch(Stream& stream, uint8_t const c) {
    switch (Cmd(c)) {
    case Cmd::db_read: detail::db_read(stream); break;
    case Cmd::attr_read: detail::attr_read(stream); break;
    case Cmd::attr_write: detail::attr_write(stream); break;
    default: break;  // ignore, not a command escape code
    }
}

}  // namespace nevermore::gatt_stream
