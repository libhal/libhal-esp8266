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
#include <cinttypes>
#include <string_view>

#include <libhal-esp8266/at.hpp>
#include <libhal-util/serial.hpp>
#include <libhal-util/steady_clock.hpp>
#include <libhal-util/streams.hpp>
#include <libhal-util/timeout.hpp>
#include <libhal/timeout.hpp>

#include "../hardware_map.hpp"
#include "helper.hpp"

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
                          const std::string_view p_ssid,
                          const std::string_view p_password,
                          const hal::esp8266::at::socket_config& p_config,
                          const std::string_view p_ip,
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

struct http_header_parser_t
{
  hal::stream_find find_header_start;
  hal::stream_find find_content_length;
  hal::stream_parse<std::uint32_t> parse_content_length;
  hal::stream_find find_end_of_header;
};

http_header_parser_t new_http_header_parser()
{
  using namespace std::literals;

  return http_header_parser_t{
    .find_header_start = hal::stream_find(hal::as_bytes("HTTP/1.1 "sv)),
    .find_content_length =
      hal::stream_find(hal::as_bytes("Content-Length: "sv)),
    .parse_content_length = hal::stream_parse<std::uint32_t>(),
    .find_end_of_header = hal::stream_find(hal::as_bytes("\r\n\r\n"sv)),
  };
}
}  // namespace

void application(hardware_map_t& p_map)
{
  using namespace std::chrono_literals;
  using namespace hal::literals;
  using namespace std::literals;

  auto& counter = *p_map.counter;
  auto& serial = *p_map.serial;
  auto& console = *p_map.console;

  constexpr std::string_view ssid = "Stellic";
  constexpr std::string_view password = "misosoupisgreat";
  constexpr auto socket_config = hal::esp8266::at::socket_config{
    .type = hal::esp8266::at::socket_type::tcp,
    .domain = "httpstat.us",
    .port = 80,
  };

  // Leave empty to not use
  constexpr std::string_view ip = "";

  // 128B buffer to read data into
  std::array<hal::byte, 128> buffer{};

  hal::print(console, "ESP8266 WiFi Client Application Starting...\n");

  // Initialize esp8266 & create driver object
  hal::print(console, "Create & initialize esp8266...\n");
  auto timeout = hal::create_timeout(counter, 10s);
  hal::esp8266::at esp8266(serial, timeout);
  hal::print(console, "esp8266 created & initialized!! \n");

  // Establish connection with AP & web server
  try {
    establish_connection(
      esp8266, console, ssid, password, socket_config, ip, timeout);

  }
  // TODO: Update this to use hal::exception or a more specific exception like
  // hal::timed_out
  catch (...) {
    hal::print(console,
               "esp8266 couldn't establish a connection to AP and/or server, "
               "restarting!! \n");
    throw;
  }

  auto http_header_parser = new_http_header_parser();
  // Skip of 0 means it starts in the finished state.
  auto skip_payload = hal::stream_skip(0);
  bool header_finished = false;
  bool read_complete = true;
  bool write_error = false;
  auto read_timeout = hal::create_timeout(counter, 1000ms);
  constexpr auto graph_cutoff = 2s;
  auto bandwidth_timeout = hal::create_timeout(counter, graph_cutoff);

  std::array<std::string_view, 5> table{
    "\n",
    " TIME |                          RESPONSES                          \n"sv,
    " (2s) |    5    10   15   20   25   30   35   40  45  50  55  60  65\n"sv,
    "------|-------------------------------------------------------------\n"sv,
    "   +  |",
  };

  for (const auto& line : table) {
    hal::print(console, line);
  }

  while (true) {
    if (write_error) {
      hal::print(console, "Reconnecting...\n");
      // Wait 1s before attempting to reconnect
      hal::delay(counter, 1s);

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
      write_error = false;
    }

    if (read_complete) {
      // Minimalist GET request to example.com domain
      static constexpr std::string_view get_request = "GET /200 HTTP/1.1\r\n"
                                                      "Host: httpstat.us:80\r\n"
                                                      "\r\n";

      hal::delay(counter, 50ms);

      // Send out HTTP GET request
      timeout = hal::create_timeout(counter, 500ms);

      try {
        static_cast<void>(
          esp8266.server_write(hal::as_bytes(get_request), timeout));
      }
      // TODO: Update this to use hal::exception or a more specific exception
      // like hal::timed_out
      catch (...) {
        hal::print(console, "\nFailed to write to server!\n");
        write_error = true;
        continue;
      }

      read_complete = false;
      header_finished = false;
      read_timeout = hal::create_timeout(counter, 1000ms);
    }

    auto received = esp8266.server_read(buffer);
    auto remainder = received | http_header_parser.find_header_start |
                     http_header_parser.find_content_length |
                     http_header_parser.parse_content_length |
                     http_header_parser.find_end_of_header;

    if (!header_finished &&
        hal::finished(http_header_parser.find_end_of_header)) {
      auto content_length = http_header_parser.parse_content_length.value();
      skip_payload = hal::stream_skip(content_length);
      header_finished = true;
    }

    if (header_finished && hal::in_progress(skip_payload)) {
      remainder | skip_payload;
      if (hal::finished(skip_payload.state())) {
        read_complete = true;

        http_header_parser = new_http_header_parser();
        skip_payload = hal::stream_skip(0);

        hal::print(console, ".");
      }
    }

    try {
      read_timeout();
      bandwidth_timeout();
    } catch (const hal::timed_out& p_exception) {
      if (&read_timeout == p_exception.instance()) {
        hal::print(console, "X");
        read_complete = true;
      }
      // TODO: Replace this exceptional bandwidth timeout with a variant that
      // simply returns if the timeout has occurred. This is not its intended
      // purpose but does demonstrates proper usage of `p_exception.instance()`.
      else if (&bandwidth_timeout == p_exception.instance()) {
        hal::print(console, "\n   +  |");
        bandwidth_timeout = hal::create_timeout(counter, graph_cutoff);
      } else {
        throw;
      }
    }
  }
}
