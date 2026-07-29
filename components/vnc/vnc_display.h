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

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>

#include "esphome/core/application.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "esphome/components/display/display.h"
#include "esphome/components/display/display_color_utils.h"
#include "esphome/components/network/util.h"
#include "esphome/components/socket/socket.h"
#include "esphome/components/touchscreen/touchscreen.h"

#ifdef USE_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#else
#include <pthread.h>
#endif

namespace esphome::vnc {

static const char *const TAG = "vnc";

static const size_t VERSION_LEN = 12;
/// Bytes per pixel in the local framebuffer. Stored as B, G, R, X to match the pixel
/// format advertised in build_init_() (32bpp, little-endian, r<<16 | g<<8 | b).
static const size_t PIXEL_BYTES = 4;
/// Size of the client command ring buffer. Must stay 256 so the uint8_t read/write
/// indices wrap naturally.
static const size_t RING_SIZE = 256;
/// Depth of the rectangle queue handed to the transmit task.
static const size_t QUEUE_DEPTH = 200;
/// Give up on a partial socket write after this long and drop the client.
static const uint32_t WRITE_TIMEOUT_MS = 2000;

static const uint8_t RFB_MAGIC[VERSION_LEN] = {
    'R', 'F', 'B', ' ', '0', '0', '3', '.', '0', '0', '3', '\n',
};

static inline uint8_t *put16_be(uint8_t *buf, uint16_t value) {
  buf[0] = value >> 8;
  buf[1] = value;
  return buf + 2;
}

static inline uint8_t *put32_be(uint8_t *buf, uint32_t value) {
  buf[0] = value >> 24;
  buf[1] = value >> 16;
  buf[2] = value >> 8;
  buf[3] = value;
  return buf + 4;
}

static inline uint16_t get16_be(const uint8_t *buf) { return buf[1] + (buf[0] << 8); }

static inline uint32_t get32_be(const uint8_t *buf) {
  return buf[3] + (buf[2] << 8) + (buf[1] << 16) + ((uint32_t) buf[0] << 24);
}

enum ClientState {
  STATE_INVALID,
  STATE_VERSION,
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

static inline void buf_clr(circ_buf_t &buf) {
  buf.inp = 0;
  buf.outp = 0;
}

static inline size_t buf_size(const circ_buf_t &buf) { return (uint8_t) (buf.inp - buf.outp); }

/// One slot is always left free so that inp == outp unambiguously means "empty".
static inline size_t buf_space(const circ_buf_t &buf) { return RING_SIZE - 1 - buf_size(buf); }

static inline uint8_t buf_peek(const circ_buf_t &buf) { return buf.data[buf.outp]; }

/// Discard len bytes. The caller must ensure len <= buf_size(buf).
static inline void buf_skip(circ_buf_t &buf, size_t len) { buf.outp += (uint8_t) len; }

/// Copy len bytes out of the buffer. The caller must ensure len <= buf_size(buf).
static inline void buf_copy(circ_buf_t &buf, uint8_t *dest, size_t len) {
  size_t rem = RING_SIZE - buf.outp;
  if (rem < len) {
    memcpy(dest, buf.data + buf.outp, rem);
    dest += rem;
    len -= rem;
    buf.outp = 0;
  }
  memcpy(dest, buf.data + buf.outp, len);
  buf.outp += (uint8_t) len;
}

/// Add data to the buffer. Fails without copying anything if it will not fit.
static inline bool buf_add(circ_buf_t &buf, const uint8_t *src, size_t len) {
  if (len > buf_space(buf)) {
    ESP_LOGW(TAG, "Could not add %u bytes to buffer", (unsigned) len);
    return false;
  }
  size_t rem = RING_SIZE - buf.inp;
  if (rem < len) {
    memcpy(buf.data + buf.inp, src, rem);
    src += rem;
    len -= rem;
    buf.inp = 0;
  }
  memcpy(buf.data + buf.inp, src, len);
  buf.inp += (uint8_t) len;
  return true;
}

class VNCDisplay;

class VNCTrigger : public Trigger<>, public Parented<VNCDisplay> {
 public:
  VNCTrigger() = default;
  explicit VNCTrigger(VNCDisplay *parent) : Parented<VNCDisplay>(parent) {}
};

class VNCTouchscreen : public touchscreen::Touchscreen {
 public:
  void update_pointer(bool touching, uint16_t x, uint16_t y) {
    if (touching != this->touching_ || (touching && (this->xpos_ != x || this->ypos_ != y))) {
      this->store_.touched = true;
      this->updated_ = true;
    }
    this->touching_ = touching;
    this->xpos_ = x;
    this->ypos_ = y;
  }

  void setup() override {
    if (this->display_ != nullptr) {
      this->x_raw_max_ = this->display_->get_width();
      this->y_raw_max_ = this->display_->get_height();
    }
    // Pointer events arrive from the network, so there is nothing to poll for.
    this->store_.init = true;
  }

 protected:
  void update_touches() override {
    if (!this->updated_) {
      this->skip_update_ = true;
      return;
    }
    this->updated_ = false;
    if (this->touching_) {
      ESP_LOGV(TAG, "Sending touch %u/%u", this->xpos_, this->ypos_);
      this->add_raw_touch_position_(0, this->xpos_, this->ypos_);
    }
  }

  bool touching_{};
  bool updated_{true};
  uint16_t xpos_{};
  uint16_t ypos_{};
};

class VNCDisplay : public display::Display {
 public:
  void add_touchscreen(VNCTouchscreen *tp) {
    this->touchscreens_.add([tp](bool touching, uint16_t x, uint16_t y) { tp->update_pointer(touching, x, y); });
  }

  void setup() override {
    ESP_LOGCONFIG(TAG, "Setting up VNC server...");
    size_t buffer_length = (size_t) this->width_ * this->height_ * PIXEL_BYTES;
    // Default flags prefer external (PSRAM) and fall back to internal RAM.
    RAMAllocator<uint8_t> allocator;
    this->display_buffer_ = allocator.allocate(buffer_length);
    if (this->display_buffer_ == nullptr) {
      ESP_LOGE(TAG, "Could not allocate %u bytes for the display buffer - PSRAM is required at this size",
               (unsigned) buffer_length);
      this->mark_failed();
      return;
    }
    // clear to grey
    memset(this->display_buffer_, 0x80, buffer_length);
    this->mark_clean_();

#ifdef USE_HOST
    pthread_t tid;
    int err = pthread_create(
        &tid, nullptr,
        [](void *arg) -> void * {
          static_cast<VNCDisplay *>(arg)->tx_task_();
          return nullptr;
        },
        this);
    if (err != 0) {
      ESP_LOGE(TAG, "Failed to start transmit thread: err=%d", err);
      this->mark_failed();
    }
#else
    this->queue_ = xQueueCreate(QUEUE_DEPTH, sizeof(rect_t));
    if (this->queue_ == nullptr) {
      ESP_LOGE(TAG, "Failed to create transmit queue");
      this->mark_failed();
      return;
    }
    if (xTaskCreatePinnedToCore([](void *arg) { static_cast<VNCDisplay *>(arg)->tx_task_(); }, "vnc_tx", 8192, this, 2,
                                nullptr, 0) != pdPASS) {
      ESP_LOGE(TAG, "Failed to start transmit task");
      this->mark_failed();
    }
#endif
  }

  void dump_config() override {
    ESP_LOGCONFIG(TAG,
                  "VNC Display:\n"
                  "  Dimensions: %dpx x %dpx\n"
                  "  Port: %u\n"
                  "  Touchscreens: %u",
                  this->get_width(), this->get_height(), this->port_, (unsigned) this->touchscreens_.size());
  }

  void loop() override {
    if (!network::is_connected()) {
      if (this->listen_sock_ != nullptr)
        this->end_socket_();
      return;
    }
    if (this->listen_sock_ == nullptr) {
      this->start_socket_();
      return;
    }
    if (this->disconnect_pending_)
      this->disconnect_();
    if (this->client_sock_ == nullptr)
      this->accept_client_();
    this->client_loop_();
    if (this->disconnect_pending_)
      this->disconnect_();
  }

  void update() override {
    this->do_update_();
    this->update_frame_();
  }

  void set_dimensions(size_t width, size_t height) {
    this->width_ = width;
    this->height_ = height;
  }

  int get_height() override { return this->height_; }
  int get_width() override { return this->width_; }

  display::DisplayType get_display_type() override { return display::DISPLAY_TYPE_COLOR; }
  void set_port(uint16_t port) { this->port_ = port; }
  void set_on_connect(std::function<void()> &&on_connect) { this->on_connect_ = std::move(on_connect); }
  void set_on_disconnect(std::function<void()> &&on_disconnect) { this->on_disconnect_ = std::move(on_disconnect); }

  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  void draw_pixel_at(int x, int y, Color color) override {
    if (x < 0 || y < 0 || x >= this->width_ || y >= this->height_)
      return;
    uint8_t *dst = this->display_buffer_ + ((size_t) y * this->width_ + x) * PIXEL_BYTES;
    dst[0] = color.b;
    dst[1] = color.g;
    dst[2] = color.r;
    dst[3] = 0;
    if (!this->internal_update_)
      this->mark_dirty_(x, y, 1, 1);
  }

  /// Overridden so a full-screen clear is a straight buffer write rather than
  /// width * height calls to draw_pixel_at().
  void fill(Color color) override {
    if (this->display_buffer_ == nullptr)
      return;
    const uint8_t pixel[PIXEL_BYTES] = {color.b, color.g, color.r, 0};
    size_t count = (size_t) this->width_ * this->height_;
    uint8_t *dst = this->display_buffer_;
    if (pixel[0] == pixel[1] && pixel[1] == pixel[2] && pixel[2] == pixel[3]) {
      memset(dst, pixel[0], count * PIXEL_BYTES);
    } else {
      for (size_t i = 0; i != count; i++, dst += PIXEL_BYTES)
        memcpy(dst, pixel, PIXEL_BYTES);
    }
    if (!this->internal_update_)
      this->mark_dirty_(0, 0, this->width_, this->height_);
  }

  void draw_pixels_at(int x_start, int y_start, int w, int h, const uint8_t *ptr, display::ColorOrder order,
                      display::ColorBitness bitness, bool big_endian, int x_offset, int y_offset, int x_pad) override {
    if (w <= 0 || h <= 0 || x_start < 0 || y_start < 0 || x_start + w > this->width_ || y_start + h > this->height_)
      return;
    // COLOR_BITNESS_888 is three bytes per pixel. With little-endian byte order and COLOR_ORDER_RGB
    // those bytes are already B, G, R - the same channel order as the framebuffer - so the copy is a
    // plain 3 -> 4 byte expansion. Everything else (rotation, 565/332, byte swapping, other channel
    // orders) is handed to the generic per-pixel implementation, which converts via draw_pixel_at().
    if (this->rotation_ != display::DISPLAY_ROTATION_0_DEGREES || bitness != display::COLOR_BITNESS_888 || big_endian ||
        order != display::COLOR_ORDER_RGB) {
      this->internal_update_ = true;
      display::Display::draw_pixels_at(x_start, y_start, w, h, ptr, order, bitness, big_endian, x_offset, y_offset,
                                       x_pad);
      this->internal_update_ = false;
    } else {
      const size_t src_stride = (size_t) (w + x_offset + x_pad) * 3;
      for (int y = 0; y != h; y++) {
        const uint8_t *src = ptr + (size_t) (y + y_offset) * src_stride + (size_t) x_offset * 3;
        uint8_t *dst = this->display_buffer_ + ((size_t) (y + y_start) * this->width_ + x_start) * PIXEL_BYTES;
        for (int x = 0; x != w; x++, src += 3, dst += PIXEL_BYTES) {
          dst[0] = src[0];
          dst[1] = src[1];
          dst[2] = src[2];
          dst[3] = 0;
        }
      }
    }
    // Push this rectangle straight to the transmit task; fall back to the coalesced dirty
    // rectangle if the queue is full, so nothing is ever silently dropped.
    if (this->state_ == STATE_READY) {
      rect_t r{(int16_t) x_start, (int16_t) y_start, (int16_t) (x_start + w - 1), (int16_t) (y_start + h - 1)};
      if (!this->queue_rect_(r))
        this->mark_dirty_(x_start, y_start, w, h);
    } else {
      this->mark_dirty_(x_start, y_start, w, h);
    }
  }

 protected:
  int get_height_internal() override { return this->height_; }
  int get_width_internal() override { return this->width_; }

  // ---------------------------------------------------------------- socket setup

  void start_socket_() {
    this->listen_sock_ = socket::socket_ip(SOCK_STREAM, 0);
    if (this->listen_sock_ == nullptr) {
      ESP_LOGW(TAG, "Could not create socket");
      this->mark_failed();
      return;
    }
    int enable = 1;
    if (this->listen_sock_->setsockopt(SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) != 0) {
      ESP_LOGW(TAG, "Socket unable to set reuseaddr: errno %d", errno);
      // not fatal
    }
    if (this->listen_sock_->setblocking(false) != 0) {
      ESP_LOGW(TAG, "Socket unable to set nonblocking mode: errno %d", errno);
      this->listen_sock_ = nullptr;
      return;
    }

    struct sockaddr_storage server;
    socklen_t sl = socket::set_sockaddr_any((struct sockaddr *) &server, sizeof(server), this->port_);
    if (sl == 0) {
      ESP_LOGW(TAG, "Socket unable to set sockaddr: errno %d", errno);
      this->listen_sock_ = nullptr;
      return;
    }
    if (this->listen_sock_->bind((struct sockaddr *) &server, sl) != 0) {
      ESP_LOGW(TAG, "Socket unable to bind: errno %d", errno);
      this->listen_sock_ = nullptr;
      return;
    }
    if (this->listen_sock_->listen(1) != 0) {
      ESP_LOGW(TAG, "Socket unable to listen: errno %d", errno);
      this->listen_sock_ = nullptr;
      return;
    }
    ESP_LOGD(TAG, "Listening on port %u", this->port_);
  }

  void end_socket_() {
    this->disconnect_();
    if (this->listen_sock_ != nullptr) {
      this->listen_sock_->close();
      this->listen_sock_ = nullptr;
    }
  }

  void accept_client_() {
    struct sockaddr_storage source_addr;
    socklen_t addr_len = sizeof(source_addr);
    auto sock = this->listen_sock_->accept((struct sockaddr *) &source_addr, &addr_len);
    if (sock == nullptr)
      return;
    sock->setblocking(false);
    {
      LockGuard guard(this->sock_mutex_);
      this->client_sock_ = std::move(sock);
    }
    this->disconnect_pending_ = false;
    this->state_ = STATE_VERSION;
    buf_clr(this->inq_);
    this->skip_bytes_ = 0;
    this->high_freq_.start();
    ESP_LOGD(TAG, "Client connected");
    if (this->write_(RFB_MAGIC, sizeof RFB_MAGIC) < 0)
      this->disconnect_pending_ = true;
  }

  void disconnect_() {
    this->disconnect_pending_ = false;
    // Stop the transmit task from touching the socket before it goes away.
    this->state_ = STATE_INVALID;
    bool was_connected;
    {
      LockGuard guard(this->sock_mutex_);
      was_connected = this->client_sock_ != nullptr;
      if (was_connected) {
        this->client_sock_->close();
        this->client_sock_ = nullptr;
      }
    }
    buf_clr(this->inq_);
    this->skip_bytes_ = 0;
    this->mark_clean_();
    if (!was_connected)
      return;
    this->high_freq_.stop();
    ESP_LOGD(TAG, "Client disconnected");
    if (this->on_disconnect_ != nullptr)
      this->defer([this]() { this->on_disconnect_(); });
  }

  // ------------------------------------------------------------------- socket IO
  //
  // read_() and write_() are the only places the client socket is touched, and both take
  // sock_mutex_ around each individual syscall. write_() also runs on the transmit task,
  // so the lock is released between retries to keep the main loop responsive.

  ssize_t read_(uint8_t *buffer, size_t len) {
    ssize_t res;
    {
      LockGuard guard(this->sock_mutex_);
      if (this->client_sock_ == nullptr)
        return -1;
      res = this->client_sock_->read(buffer, len);
    }
    if (res > 0)
      return res;
    if (res == 0) {  // orderly shutdown by the peer
      this->disconnect_pending_ = true;
      return -1;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return 0;
    ESP_LOGW(TAG, "Socket read failed: errno %d", errno);
    this->disconnect_pending_ = true;
    return -1;
  }

  ssize_t write_(const uint8_t *buffer, size_t len) {
    const uint8_t *ptr = buffer;
    size_t remaining = len;
    uint32_t started = millis();
    while (remaining != 0) {
      ssize_t res;
      {
        LockGuard guard(this->sock_mutex_);
        if (this->client_sock_ == nullptr)
          return -1;
        res = this->client_sock_->write(ptr, remaining);
      }
      if (res < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          if (millis() - started > WRITE_TIMEOUT_MS) {
            ESP_LOGW(TAG, "Socket write timed out");
            this->disconnect_pending_ = true;
            return -1;
          }
          delay(1);
          continue;
        }
        ESP_LOGW(TAG, "Socket write failed: errno %d", errno);
        this->disconnect_pending_ = true;
        return -1;
      }
      remaining -= res;
      ptr += res;
    }
    return (ssize_t) len;
  }

  // ------------------------------------------------------------ transmit pipeline

  bool queue_rect_(const rect_t &r) {
#ifdef USE_HOST
    LockGuard guard(this->queue_mutex_);
    if (this->queue_.size() >= QUEUE_DEPTH)
      return false;
    this->queue_.push_back(r);
    return true;
#else
    return xQueueSend(this->queue_, &r, 0) == pdTRUE;
#endif
  }

  inline size_t tx_rem_() { return sizeof(this->tx_buf_) - this->tx_buflen_; }
  inline void tx_16(uint16_t value) {
    put16_be(this->tx_buf_ + this->tx_buflen_, value);
    this->tx_buflen_ += 2;
  }
  inline void tx_8(uint8_t value) { this->tx_buf_[this->tx_buflen_++] = value; }
  void tx_flush_() {
    if (this->tx_buflen_ != 0) {
      this->write_(this->tx_buf_, this->tx_buflen_);
      this->tx_buflen_ = 0;
    }
  }

  /// Pack one rectangle into the transmit buffer, flushing as required.
  void send_framebuffer_(const rect_t &r) {
    size_t x_start = r.x_min;
    size_t y_start = r.y_min;
    size_t w = r.x_max - r.x_min + 1;
    size_t h = r.y_max - r.y_min + 1;
    ESP_LOGV(TAG, "Send framebuffer %u/%u %ux%u", (unsigned) x_start, (unsigned) y_start, (unsigned) w, (unsigned) h);
    if (this->tx_rem_() < 12)
      this->tx_flush_();
    this->tx_16(x_start);
    this->tx_16(y_start);
    this->tx_16(w);
    this->tx_16(h);
    this->tx_16(0);  // raw encoding
    this->tx_16(0);
    for (size_t y = 0; y != h; y++) {
      size_t bytes = w * PIXEL_BYTES;
      if (this->tx_rem_() < bytes)
        this->tx_flush_();
      memcpy(this->tx_buf_ + this->tx_buflen_,
             this->display_buffer_ + ((y + y_start) * this->width_ + x_start) * PIXEL_BYTES, bytes);
      this->tx_buflen_ += bytes;
    }
  }

  void tx_task_() {
    std::vector<rect_t> batch;
    batch.reserve(QUEUE_DEPTH);
    for (;;) {  // NOLINT
#ifdef USE_HOST
      {
        LockGuard guard(this->queue_mutex_);
        batch.swap(this->queue_);
      }
      if (!batch.empty()) {
        this->send_batch_(batch.data(), batch.size());
        batch.clear();
      }
      delay(10);
#else
      rect_t rp;
      if (xQueuePeek(this->queue_, &rp, pdMS_TO_TICKS(1000)) != pdPASS)
        continue;
      batch.clear();
      while (xQueueReceive(this->queue_, &rp, 0) == pdPASS)
        batch.push_back(rp);
      this->send_batch_(batch.data(), batch.size());
#endif
    }
  }

  void send_batch_(const rect_t *rects, size_t count) {
    // state_ can change underneath us at any point; the null check inside write_() is what
    // actually makes this safe, this is just an early-out for the common disconnected case.
    if (count == 0 || this->state_ != STATE_READY)
      return;
    this->tx_buflen_ = 0;
    this->tx_8(0);  // FramebufferUpdate
    this->tx_8(0);  // padding
    this->tx_16(count);
    for (size_t i = 0; i != count; i++)
      this->send_framebuffer_(rects[i]);
    this->tx_flush_();
  }

  // -------------------------------------------------------------- dirty tracking

  void mark_dirty_(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0)
      return;
    int x_max = std::min(x + w - 1, this->width_ - 1);
    int y_max = std::min(y + h - 1, this->height_ - 1);
    x = std::max(x, 0);
    y = std::max(y, 0);
    if (x > x_max || y > y_max)
      return;
    this->dirty_rect_.x_min = std::min<int16_t>(this->dirty_rect_.x_min, x);
    this->dirty_rect_.y_min = std::min<int16_t>(this->dirty_rect_.y_min, y);
    this->dirty_rect_.x_max = std::max<int16_t>(this->dirty_rect_.x_max, x_max);
    this->dirty_rect_.y_max = std::max<int16_t>(this->dirty_rect_.y_max, y_max);
  }

  void mark_clean_() {
    this->dirty_rect_.x_min = this->width_;
    this->dirty_rect_.y_min = this->height_;
    this->dirty_rect_.x_max = 0;
    this->dirty_rect_.y_max = 0;
  }

  inline bool is_dirty_() const {
    return this->dirty_rect_.x_max >= this->dirty_rect_.x_min && this->dirty_rect_.y_max >= this->dirty_rect_.y_min;
  }

  void update_frame_() {
    if (this->is_dirty_() && this->state_ == STATE_READY && this->queue_rect_(this->dirty_rect_))
      this->mark_clean_();
  }

  // ------------------------------------------------------------------ RFB protocol

  size_t build_init_(uint8_t *buffer) {
    uint8_t *sp = buffer;
    sp = put16_be(sp, this->width_);
    sp = put16_be(sp, this->height_);
    *sp++ = 32;                       // bits per pixel
    *sp++ = 24;                       // bit depth
    *sp++ = 0;                        // little-endian
    *sp++ = 1;                        // true colour
    sp = put16_be(sp, (1 << 8) - 1);  // red max
    sp = put16_be(sp, (1 << 8) - 1);  // green max
    sp = put16_be(sp, (1 << 8) - 1);  // blue max
    *sp++ = 16;                       // red shift
    *sp++ = 8;                        // green shift
    *sp++ = 0;                        // blue shift
    *sp++ = 0;                        // padding
    *sp++ = 0;
    *sp++ = 0;
    const auto &name = App.get_name();
    size_t len = std::min<size_t>(name.size(), 64);
    sp = put32_be(sp, len);
    memcpy(sp, name.c_str(), len);
    sp += len;
    return sp - buffer;
  }

  /// Consume one complete client message. Returns true if progress was made.
  bool process_() {
    uint8_t buffer[32];
    size_t len;

    if (this->skip_bytes_ != 0) {
      size_t skip = std::min(buf_size(this->inq_), this->skip_bytes_);
      buf_skip(this->inq_, skip);
      this->skip_bytes_ -= skip;
      if (this->skip_bytes_ != 0)
        return false;  // need more data before the next message starts
    }
    if (buf_size(this->inq_) == 0)
      return false;

    switch (buf_peek(this->inq_)) {
      case 0:  // SetPixelFormat
        if (buf_size(this->inq_) >= 20) {
          buf_copy(this->inq_, buffer, 20);
          if (buffer[4] != 32 || buffer[5] != 24 || buffer[6] != 0 || buffer[7] == 0) {
            ESP_LOGW(TAG, "Client requested unsupported pixel format (bits %u, depth %u, %s endian, true colour %s)",
                     buffer[4], buffer[5], buffer[6] ? "big" : "little", buffer[7] ? "yes" : "no");
          }
          return true;
        }
        break;

      case 2:  // SetEncodings
        if (buf_size(this->inq_) >= 4) {
          buf_copy(this->inq_, buffer, 4);
          len = get16_be(buffer + 2);
          // Only raw encoding is implemented, so the list is discarded rather than buffered -
          // it can be far larger than the ring buffer.
          this->skip_bytes_ = len * 4;
          ESP_LOGD(TAG, "Client offered %u encodings", (unsigned) len);
          return true;
        }
        break;

      case 3:  // FramebufferUpdateRequest
        if (buf_size(this->inq_) >= 10) {
          buf_copy(this->inq_, buffer, 10);
          uint8_t incremental = buffer[1];
          uint16_t xpos = get16_be(buffer + 2);
          uint16_t ypos = get16_be(buffer + 4);
          uint16_t width = get16_be(buffer + 6);
          uint16_t height = get16_be(buffer + 8);
          ESP_LOGV(TAG, "Framebuffer %s update request %u/%u %ux%u", incremental ? "incremental" : "immediate", xpos,
                   ypos, width, height);
          if (!incremental) {
            this->mark_dirty_(xpos, ypos, width, height);
            this->update_frame_();
          }
          return true;
        }
        break;

      case 4:  // KeyEvent
        if (buf_size(this->inq_) >= 8) {
          buf_copy(this->inq_, buffer, 8);
          ESP_LOGV(TAG, "Key event %08X %s", (unsigned) get32_be(buffer + 4), buffer[1] ? "down" : "up");
          return true;
        }
        break;

      case 5:  // PointerEvent
        if (buf_size(this->inq_) >= 6) {
          buf_copy(this->inq_, buffer, 6);
          uint8_t mask = buffer[1];
          uint16_t xpos = get16_be(buffer + 2);
          uint16_t ypos = get16_be(buffer + 4);
          ESP_LOGV(TAG, "Pointer event %02X %u/%u", mask, xpos, ypos);
          this->touchscreens_.call((mask & 1) != 0, xpos, ypos);
          return true;
        }
        break;

      case 6:  // ClientCutText
        if (buf_size(this->inq_) >= 8) {
          buf_copy(this->inq_, buffer, 8);
          this->skip_bytes_ = get32_be(buffer + 4);
          ESP_LOGD(TAG, "Discarding %u byte cut buffer", (unsigned) this->skip_bytes_);
          return true;
        }
        break;

      default:
        ESP_LOGW(TAG, "Unknown command %u, dropping client", buf_peek(this->inq_));
        this->disconnect_pending_ = true;
        break;
    }
    return false;
  }

  void client_loop_() {
    uint8_t buffer[128];
    ssize_t err;

    switch (this->state_) {
      case STATE_VERSION:
        err = this->read_(buffer, VERSION_LEN);
        if (err <= 0)
          break;
        ESP_LOGD(TAG, "Read %.*s as version", (int) err, buffer);
        // RFB 3.3: the server dictates the security type as a 4 byte big-endian word.
        buffer[0] = 0;
        buffer[1] = 0;
        buffer[2] = 0;
        buffer[3] = AUTH_NONE;
        if (this->write_(buffer, 4) < 0)
          break;
        this->state_ = STATE_CLIENT_INIT;
        break;

      case STATE_CLIENT_INIT: {
        // ClientInit is a single byte: the shared-desktop flag.
        err = this->read_(buffer, 1);
        if (err <= 0)
          break;
        ESP_LOGV(TAG, "ClientInit shared flag %u", buffer[0]);
        size_t len = this->build_init_(buffer);
        if (this->write_(buffer, len) < 0)
          break;
        this->state_ = STATE_READY;
        buf_clr(this->inq_);
        this->skip_bytes_ = 0;
        // Always push a full frame so the client has something to show, whether or not an
        // on_connect automation is configured.
        this->mark_dirty_(0, 0, this->width_, this->height_);
        this->update_frame_();
        if (this->on_connect_ != nullptr)
          this->defer([this]() { this->on_connect_(); });
        break;
      }

      case STATE_READY:
        // Drain the socket until it would block, so no data is left sitting unread.
        for (;;) {
          while (this->process_())
            continue;
          if (this->disconnect_pending_)
            break;
          err = this->read_(buffer, sizeof buffer);
          if (err <= 0)
            break;
          if (!buf_add(this->inq_, buffer, err))
            break;  // ring full - process what we have and come back next loop
        }
        break;

      default:
        break;
    }
  }

  // ------------------------------------------------------------------------ state

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

  uint8_t tx_buf_[4096];
  size_t tx_buflen_{0};

  CallbackManager<void(bool, uint16_t, uint16_t)> touchscreens_;
  HighFrequencyLoopRequester high_freq_;
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
