// Copyright 2024 Khalil Estell
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <array>
#include <string_view>

#include <libhal-esp8266/at.hpp>
#include <libhal-util/as_bytes.hpp>
#include <libhal-util/serial.hpp>
#include <libhal-util/steady_clock.hpp>
#include <libhal-util/streams.hpp>
#include <libhal-util/timeout.hpp>
#include <libhal/timeout.hpp>

#include "../hardware_map.hpp"

namespace {
enum class connection_state
{
  check_ap_connection,
  connecting_to_ap,
  set_ip_address,
  check_server_connection,
  connecting_to_server,
  connection_established,
};

/**
 * @brief Connect to WiFi and a server
 *
 * @param p_esp8266 - esp8266 driver
 * @param p_console - console to log print messages to. Use an inert
 * @param p_ssid - target WiFi AP's SSID
 * @param p_password - target WiFi AP's password
 * @param p_config - socket configuration
 * @param p_ip - WiFi IP address
 * @param p_timeout - time allowed to attempt this
 */
void establish_connection(hal::esp8266::at& p_esp8266,
                          hal::serial& p_console,
                          std::string_view const p_ssid,
                          std::string_view const p_password,
                          hal::esp8266::at::socket_config const& p_config,
                          std::string_view const p_ip,
                          hal::timeout auto& p_timeout)
{
  connection_state state = connection_state::check_ap_connection;

  while (state != connection_state::connection_established) {
    switch (state) {
      case connection_state::check_ap_connection:
        hal::print(p_console, "Checking if AP \"");
        hal::print(p_console, p_ssid);
        hal::print(p_console, "\" is connected... ");
        if (p_esp8266.is_connected_to_ap(p_timeout)) {
          state = connection_state::check_server_connection;
          hal::print(p_console, "Connected!\n");
        } else {
          state = connection_state::connecting_to_ap;
          hal::print(p_console, "NOT Connected!\n");
        }
        break;
      case connection_state::connecting_to_ap:
        hal::print(p_console, "Connecting to AP: \"");
        hal::print(p_console, p_ssid);
        hal::print(p_console, "\" ...\n");
        p_esp8266.connect_to_ap(p_ssid, p_password, p_timeout);
        state = connection_state::set_ip_address;
        break;
      case connection_state::set_ip_address:
        if (!p_ip.empty()) {
          hal::print(p_console, "Setting IP Address to: ");
          hal::print(p_console, p_ip);
          hal::print(p_console, " ...\n");
          p_esp8266.set_ip_address(p_ip, p_timeout);
        }
        state = connection_state::check_server_connection;
        break;
      case connection_state::check_server_connection:
        hal::print(p_console, "Checking if server \"");
        hal::print(p_console, p_config.domain);
        hal::print(p_console, "\" is connected... ");
        if (p_esp8266.is_connected_to_server(p_timeout)) {
          state = connection_state::connection_established;
          hal::print(p_console, "Connected!\n");
        } else {
          state = connection_state::connecting_to_server;
          hal::print(p_console, "NOT Connected!\n");
        }
        break;
      case connection_state::connecting_to_server:
        hal::print(p_console, "Connecting to server: \"");
        hal::print(p_console, p_config.domain);
        hal::print(p_console, "\" ...\n");
        p_esp8266.connect_to_server(p_config, p_timeout);
        state = connection_state::check_server_connection;
        break;
      case connection_state::connection_established:
        // Do nothing, allow next iteration to break while loop
        break;
      default:
        state = connection_state::connecting_to_ap;
    }
  }
}

class stream_http_get
{
public:
  friend std::span<hal::byte const> operator|(
    std::span<hal::byte const> const& p_input_data,
    stream_http_get& p_self)
  {
    if (hal::finished(p_self)) {
      return {};
    }

    auto const remaining = p_input_data | p_self.m_find_start |
                           p_self.m_find_length | p_self.m_parse_length |
                           p_self.m_find_end | p_self.m_skip_final;

    if (hal::finished(p_self.m_skip_final)) {
      auto const length = p_self.m_parse_length.value();
      auto const transmit_size = std::min(remaining.size(), length);
      auto const result = remaining.first(transmit_size);
      p_self.m_body_bytes_transmitted += transmit_size;
      return result;
    }

    return {};
  }

  hal::work_state state()
  {
    if (hal::finished(m_find_end) and
        m_body_bytes_transmitted >= m_parse_length.value()) {
      return hal::work_state::finished;
    }
    return hal::work_state::in_progress;
  }

private:
  static constexpr std::string_view start_sv{ "HTTP/1.1 " };
  static constexpr std::string_view length_sv{ "Content-Length: " };
  static constexpr std::string_view end_sv{ "\r\n\r\n" };

  hal::stream_find m_find_start{ hal::as_bytes(start_sv) };
  hal::stream_find m_find_length{ hal::as_bytes(length_sv) };
  hal::stream_parse<size_t> m_parse_length{};
  hal::stream_find m_find_end{ hal::as_bytes(end_sv) };
  hal::stream_skip m_skip_final{ 1 };
  size_t m_body_bytes_transmitted = 0;
};
}  // namespace

void application(hardware_map_t& p_map)
{
  using namespace std::chrono_literals;
  using namespace hal::literals;
  using namespace std::literals;

  auto& counter = *p_map.counter;
  auto& serial = *p_map.serial;
  auto& console = *p_map.console;

  constexpr std::string_view ssid = "<insert ssid here>";
  constexpr std::string_view password = "<insert password here>";
  constexpr auto socket_config = hal::esp8266::at::socket_config{
    .type = hal::esp8266::at::socket_type::tcp,
    .domain = "httpstat.us",
    .port = 80,
  };

  // Leave empty to not use
  constexpr std::string_view ip = "";

  // 128B buffer to read data into
  std::array<hal::byte, 64> buffer{};

  hal::print(console, "ESP8266 WiFi Client Application Starting...\n");

  // Initialize esp8266 & create driver object
  hal::print(console, "Create & initialize esp8266...\n");
  auto timeout = hal::create_timeout(counter, 10s);
  hal::esp8266::at esp8266(serial, timeout);
  hal::print(console, "esp8266 created & initialized!! \n");

  constexpr auto graph_cutoff = 2s;
  stream_http_get http_get;
  auto read_timeout = hal::create_timeout(counter, 1000ms);
  auto bandwidth_timeout = hal::create_timeout(counter, graph_cutoff);
  enum class benchmark_state
  {
    connect,
    make_request,
    read,
  };
  benchmark_state state = benchmark_state::connect;

  std::array<std::string_view, 5> table{
    "\n",
    " TIME |                          RESPONSES                          \n"sv,
    " (2s) |    5    10   15   20   25   30   35   40  45  50  55  60  65\n"sv,
    "------|-------------------------------------------------------------\n"sv,
    "   +  |",
  };

  while (true) {
    switch (state) {
      case benchmark_state::connect: {
        hal::print(console, "Connecting...\n");
        timeout = hal::create_timeout(counter, 20s);
        try {
          establish_connection(
            esp8266, console, ssid, password, socket_config, ip, timeout);
        }
        // TODO: Update this to use hal::exception or a more specific exception
        // like hal::timed_out
        catch (...) {
          continue;
        }
        for (auto const& line : table) {
          hal::print(console, line);
        }
        state = benchmark_state::make_request;
        [[fallthrough]];
      }
      case benchmark_state::make_request: {
        hal::delay(counter, 10ms);

        try {
          // Send out HTTP GET request
          timeout = hal::create_timeout(counter, 500ms);
          // Minimalist GET request to example.com domain
          std::string_view get_request = "GET /200 HTTP/1.1\r\n"
                                         "Host: httpstat.us:80\r\n"
                                         "\r\n";
          [[maybe_unused]] auto const transmitted =
            esp8266.server_write(hal::as_bytes(get_request), timeout);
        }
        // TODO: Update this to use hal::exception or a more specific exception
        // like hal::timed_out
        catch (...) {
          hal::print(console, "\nFailed to write to server!\n");
          state = benchmark_state::connect;
          continue;
        }
        read_timeout = hal::create_timeout(counter, 1000ms);
        http_get = stream_http_get();
        state = benchmark_state::read;
        [[fallthrough]];
      }
      case benchmark_state::read: {
        // Take data received from server and pass it to http_get
        // hal::delay(counter, 1s);
        auto const received = esp8266.server_read(buffer);
        [[maybe_unused]] auto const body_parts = received | http_get;

        if (hal::finished(http_get)) {
          hal::print(console, ".");
          state = benchmark_state::make_request;
        }

        break;
      }
    }

    try {
      read_timeout();
      bandwidth_timeout();
    } catch (hal::timed_out const& p_exception) {
      if (&read_timeout == p_exception.instance()) {
        hal::print(console, "X\n\n");
        state = benchmark_state::connect;
      }
      // TODO: Replace this exceptional bandwidth timeout with a variant
      // that simply returns if the timeout has occurred. This is not its
      // intended purpose but does demonstrates proper usage of
      // `p_exception.instance()`.
      else if (&bandwidth_timeout == p_exception.instance()) {
        hal::print(console, "\n   +  |");
        bandwidth_timeout = hal::create_timeout(counter, graph_cutoff);
      } else {
        throw;
      }
    }
  }
}