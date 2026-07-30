//
// VNC server display + touchscreen for ESPHome.
// Created by Clyde Stubbs on 3/1/2024.
//
// Only builds on targets that have a real threading primitive available: ESP32 (FreeRTOS)
// and the host platform (pthreads). display.py rejects other platforms during validation.
//

#pragma once

#include "esphome/core/defines.h"

#if defined(USE_HOST) || defined(USE_ESP32)

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "esphome/components/display/display.h"
#include "esphome/components/socket/socket.h"
#include "esphome/components/touchscreen/touchscreen.h"

#ifdef USE_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#else
#include <pthread.h>
#endif

namespace esphome::vnc {

/// Bytes per pixel in the local framebuffer. Stored as B, G, R, X to match the pixel
/// format advertised in build_init_() (32bpp, little-endian, r<<16 | g<<8 | b).
static const size_t PIXEL_BYTES = 4;
/// Size of the client command ring buffer. Must stay 256 so the uint8_t read/write
/// indices wrap naturally.
static const size_t RING_SIZE = 256;
/// Depth of the rectangle queue handed to the transmit task.
static const size_t QUEUE_DEPTH = 200;
/// Staging buffer for outgoing framebuffer data.
static const size_t TX_BUF_SIZE = 4096;

enum ClientState {
  STATE_INVALID,
  STATE_VERSION,
  STATE_AUTH,
  STATE_CLIENT_INIT,
  STATE_READY,
};

enum AuthType {
  AUTH_FAILED = 0x00,
  AUTH_NONE = 0x01,
  AUTH_VNC = 0x02,
};

struct circ_buf_t {
  uint8_t data[RING_SIZE];
  uint8_t inp;
  uint8_t outp;
};

struct rect_t {
  int16_t x_min;
  int16_t y_min;
  int16_t x_max;
  int16_t y_max;
};

class VNCDisplay;

class VNCTrigger : public Trigger<>, public Parented<VNCDisplay> {
 public:
  VNCTrigger() = default;
  explicit VNCTrigger(VNCDisplay *parent) : Parented<VNCDisplay>(parent) {}
};

class VNCTouchscreen : public touchscreen::Touchscreen {
 public:
  void update_pointer(bool touching, uint16_t x, uint16_t y);
  void setup() override;

 protected:
  void update_touches() override;

  bool touching_{};
  bool updated_{true};
  uint16_t xpos_{};
  uint16_t ypos_{};
};

class VNCDisplay : public display::Display {
 public:
  void add_touchscreen(VNCTouchscreen *tp);

  void setup() override;
  void dump_config() override;
  void loop() override;
  void update() override;
  float get_setup_priority() const override;

  void set_dimensions(size_t width, size_t height) {
    this->width_ = width;
    this->height_ = height;
  }
  void set_port(uint16_t port) { this->port_ = port; }
  /// Enable RFB VNC Authentication (security type 2). At most 8 characters are used.
  void set_password(const char *password);
  void set_on_connect(std::function<void()> &&on_connect) { this->on_connect_ = std::move(on_connect); }
  void set_on_disconnect(std::function<void()> &&on_disconnect) { this->on_disconnect_ = std::move(on_disconnect); }

  int get_height() override { return this->height_; }
  int get_width() override { return this->width_; }
  display::DisplayType get_display_type() override { return display::DISPLAY_TYPE_COLOR; }

  void draw_pixel_at(int x, int y, Color color) override;
  void fill(Color color) override;
  void draw_pixels_at(int x_start, int y_start, int w, int h, const uint8_t *ptr, display::ColorOrder order,
                      display::ColorBitness bitness, bool big_endian, int x_offset, int y_offset, int x_pad) override;

 protected:
  int get_height_internal() override { return this->height_; }
  int get_width_internal() override { return this->width_; }

  inline uint8_t *pixel_ptr_(int x, int y) {
    return this->display_buffer_ + ((size_t) y * this->width_ + x) * PIXEL_BYTES;
  }

  // socket setup
  void start_socket_();
  void end_socket_();
  void accept_client_();
  void disconnect_();

  // socket IO - the only places the client socket is touched. Both take sock_mutex_ around
  // each individual syscall, so the transmit task and the main loop can share it safely.
  ssize_t read_(uint8_t *buffer, size_t len);
  ssize_t write_(const uint8_t *buffer, size_t len);

  // transmit pipeline
  bool queue_rect_(const rect_t &r);
  size_t tx_rem_() const { return TX_BUF_SIZE - this->tx_buflen_; }
  void tx_8(uint8_t value);
  void tx_16(uint16_t value);
  /// Write out whatever is staged. Returns false if the socket failed, so callers can stop
  /// pushing data at a client that has gone away.
  bool tx_flush_();
  bool send_framebuffer_(const rect_t &r);
  void send_batch_(const rect_t *rects, size_t count);
  void tx_task_();

  // dirty tracking
  void mark_dirty_(int x, int y, int w, int h);
  void mark_clean_();
  bool is_dirty_() const;
  void update_frame_();

  // RFB protocol
  /// Accumulate exactly len bytes of handshake into handshake_buf_ across loop
  /// iterations. Returns true once the whole lot has arrived.
  bool read_exact_(size_t len);
  size_t build_init_(uint8_t *buffer);
  bool process_();
  void client_loop_();

  int16_t width_{};
  int16_t height_{};
  uint16_t port_{5900};
  uint8_t *display_buffer_{nullptr};
  rect_t dirty_rect_{};
  bool internal_update_{false};

  std::unique_ptr<socket::Socket> listen_sock_{};
  std::unique_ptr<socket::Socket> client_sock_{};
  Mutex sock_mutex_;
  volatile bool disconnect_pending_{false};
  volatile ClientState state_{STATE_INVALID};

  circ_buf_t inq_{};
  size_t skip_bytes_{0};

  /// NUL-terminated, empty when authentication is disabled. Only 8 characters are significant.
  char password_[9]{};
  uint8_t challenge_[16]{};
  /// Staging area for the fixed-size handshake reads, which can arrive split across segments.
  uint8_t handshake_buf_[16]{};
  size_t handshake_have_{0};
  /// millis() value before which no new client is accepted, 0 when there is no delay pending.
  uint32_t auth_retry_at_{0};

  uint8_t tx_buf_[TX_BUF_SIZE];
  size_t tx_buflen_{0};

  CallbackManager<void(bool, uint16_t, uint16_t)> touchscreens_;
  std::function<void()> on_connect_{};
  std::function<void()> on_disconnect_{};

#ifdef USE_HOST
  Mutex queue_mutex_;
  std::vector<rect_t> queue_{};
#else
  QueueHandle_t queue_{nullptr};
#endif
};

}  // namespace esphome::vnc

#endif  // defined(USE_HOST) || defined(USE_ESP32)
