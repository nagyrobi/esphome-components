#include "vnc_display.h"

#if defined(USE_HOST) || defined(USE_ESP32)

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include "esphome/components/display/display_color_utils.h"
#include "esphome/components/network/util.h"

#include "vnc_des.h"

#ifdef USE_ESP32
#include <freertos/task.h>
#endif

namespace esphome::vnc {

static const char *const TAG = "vnc";

static const size_t VERSION_LEN = 12;
/// Give up on a partial socket write after this long and drop the client.
static const uint32_t WRITE_TIMEOUT_MS = 2000;
/// Refuse new connections for this long after a failed password, to slow down guessing.
static const uint32_t AUTH_RETRY_DELAY_MS = 3000;
static const size_t CHALLENGE_LEN = 16;

static const uint8_t RFB_MAGIC[VERSION_LEN] = {
    'R', 'F', 'B', ' ', '0', '0', '3', '.', '0', '0', '3', '\n',
};

/// Pack an 8-8-8 colour into big-endian RGB565, the framebuffer's native format.
static inline uint16_t pack_pixel(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t) (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/// Store one big-endian RGB565 pixel.
static inline void store_pixel(uint8_t *dst, uint16_t px) {
  dst[0] = (uint8_t) (px >> 8);
  dst[1] = (uint8_t) px;
}

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

/// True for the errnos that just mean the peer went away. A VNC client closing mid-stream
/// almost always has unread framebuffer data buffered, and TCP requires it to answer with RST
/// in that case, so ECONNRESET here is the normal way a session ends rather than a fault.
static inline bool errno_is_disconnect() {
  switch (errno) {
    case ECONNRESET:
    case ECONNABORTED:
    case ENOTCONN:
    case EPIPE:
#ifdef ESHUTDOWN
    case ESHUTDOWN:
#endif
      return true;
    default:
      return false;
  }
}

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
static void buf_copy(circ_buf_t &buf, uint8_t *dest, size_t len) {
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
static bool buf_add(circ_buf_t &buf, const uint8_t *src, size_t len) {
  if (len > buf_space(buf))
    return false;
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

// ---------------------------------------------------------------------- touchscreen

void VNCTouchscreen::update_pointer(bool touching, uint16_t x, uint16_t y) {
  if (touching != this->touching_ || (touching && (this->xpos_ != x || this->ypos_ != y))) {
    this->store_.touched = true;
    this->updated_ = true;
  }
  this->touching_ = touching;
  this->xpos_ = x;
  this->ypos_ = y;
}

void VNCTouchscreen::setup() {
  if (this->display_ != nullptr) {
    this->x_raw_max_ = this->display_->get_width();
    this->y_raw_max_ = this->display_->get_height();
  }
  // Pointer events arrive from the network, so there is nothing to poll for.
  this->store_.init = true;
}

void VNCTouchscreen::update_touches() {
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

// ------------------------------------------------------------------- component setup

void VNCDisplay::add_touchscreen(VNCTouchscreen *tp) {
  this->touchscreens_.add([tp](bool touching, uint16_t x, uint16_t y) { tp->update_pointer(touching, x, y); });
}

void VNCDisplay::set_password(const char *password) {
  // Only the first 8 characters take part in the DES key; display.py rejects anything longer.
  strncpy(this->password_, password, sizeof(this->password_) - 1);
  this->password_[sizeof(this->password_) - 1] = '\0';
}

float VNCDisplay::get_setup_priority() const { return setup_priority::HARDWARE; }

void VNCDisplay::setup() {
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

void VNCDisplay::dump_config() {
  ESP_LOGCONFIG(TAG,
                "VNC Display:\n"
                "  Dimensions: %dpx x %dpx\n"
                "  Port: %u\n"
                "  Authentication: %s\n"
                "  Touchscreens: %u",
                this->get_width(), this->get_height(), this->port_,
                this->password_[0] != '\0' ? "VNC challenge-response" : "none", (unsigned) this->touchscreens_.size());
}

void VNCDisplay::loop() {
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

void VNCDisplay::update() {
  this->do_update_();
  this->update_frame_();
}

// ------------------------------------------------------------------------- drawing

void VNCDisplay::draw_pixel_at(int x, int y, Color color) {
  if (x < 0 || y < 0 || x >= this->width_ || y >= this->height_)
    return;
  store_pixel(this->pixel_ptr_(x, y), pack_pixel(color.r, color.g, color.b));
  if (!this->internal_update_)
    this->mark_dirty_(x, y, 1, 1);
}

/// Overridden so a full-screen clear is a straight buffer write rather than
/// width * height calls to draw_pixel_at().
void VNCDisplay::fill(Color color) {
  if (this->display_buffer_ == nullptr)
    return;
  uint16_t px = pack_pixel(color.r, color.g, color.b);
  size_t count = (size_t) this->width_ * this->height_;
  // Both bytes are equal for greys and for any colour whose high and low halves match;
  // memset is markedly faster over PSRAM, so use it when we can.
  uint8_t hi = (uint8_t) (px >> 8), lo = (uint8_t) px;
  if (hi == lo) {
    memset(this->display_buffer_, hi, count * PIXEL_BYTES);
  } else {
    uint8_t *dst = this->display_buffer_;
    for (size_t i = 0; i != count; i++, dst += PIXEL_BYTES)
      store_pixel(dst, px);
  }
  if (!this->internal_update_)
    this->mark_dirty_(0, 0, this->width_, this->height_);
}

void VNCDisplay::draw_pixels_at(int x_start, int y_start, int w, int h, const uint8_t *ptr, display::ColorOrder order,
                                display::ColorBitness bitness, bool big_endian, int x_offset, int y_offset, int x_pad) {
  if (w <= 0 || h <= 0)
    return;
  // Clip to the framebuffer instead of dropping the update. LVGL's software rotation pads the
  // rotated width up to draw_rounding, so any invalidated area reaching the far edge arrives up
  // to draw_rounding - 1 pixels too wide. Display::draw_pixels_at() clips per pixel and the
  // esp_lcd based drivers let IDF clip for them, so a hard reject here loses updates that every
  // other display shows.
  const size_t src_pixels = (size_t) (w + x_offset + x_pad);
  const int clip_left = std::max(-x_start, 0);
  const int clip_top = std::max(-y_start, 0);
  const int dst_x = x_start + clip_left;
  const int dst_y = y_start + clip_top;
  const int cw = std::min(w - clip_left, this->width_ - dst_x);
  const int ch = std::min(h - clip_top, this->height_ - dst_y);
  if (cw <= 0 || ch <= 0)
    return;
  const int src_x = x_offset + clip_left;
  const int src_y = y_offset + clip_top;
  bool handled = false;

  if (this->rotation_ == display::DISPLAY_ROTATION_0_DEGREES && order == display::COLOR_ORDER_RGB) {
    if (bitness == display::COLOR_BITNESS_565) {
      // This is the path LVGL takes at color_depth: 16, and it is now the framebuffer's own
      // format. With big-endian source it is a plain memcpy per row - no per-pixel work at all.
      for (int y = 0; y != ch; y++) {
        const uint8_t *src = ptr + ((size_t) (y + src_y) * src_pixels + src_x) * 2;
        uint8_t *dst = this->pixel_ptr_(dst_x, y + dst_y);
        if (big_endian) {
          memcpy(dst, src, (size_t) cw * PIXEL_BYTES);
        } else {
          for (int x = 0; x != cw; x++, src += 2, dst += 2) {
            dst[0] = src[1];
            dst[1] = src[0];
          }
        }
      }
      handled = true;
    } else if (bitness == display::COLOR_BITNESS_888 && !big_endian) {
      // COLOR_BITNESS_888 is three bytes per pixel; little-endian RGB puts them in B, G, R
      // order. Downconvert to 565.
      for (int y = 0; y != ch; y++) {
        const uint8_t *src = ptr + ((size_t) (y + src_y) * src_pixels + src_x) * 3;
        uint8_t *dst = this->pixel_ptr_(dst_x, y + dst_y);
        for (int x = 0; x != cw; x++, src += 3, dst += PIXEL_BYTES)
          store_pixel(dst, pack_pixel(src[2], src[1], src[0]));
      }
      handled = true;
    }
  }

  if (!handled) {
    // Rotation, 332, or a channel order we do not special-case: let the base class convert
    // pixel by pixel. It clips per pixel, so it gets the unclipped rectangle. internal_update_
    // suppresses per-pixel dirty marking, since the whole rectangle is queued below anyway.
    this->internal_update_ = true;
    display::Display::draw_pixels_at(x_start, y_start, w, h, ptr, order, bitness, big_endian, x_offset, y_offset,
                                     x_pad);
    this->internal_update_ = false;
  }

  // Queue the clipped rectangle - send_framebuffer_() copies straight out of the framebuffer,
  // so an out-of-range rectangle would read past the end of it and desync the RFB stream.
  if (this->state_ == STATE_READY) {
    rect_t r{(int16_t) dst_x, (int16_t) dst_y, (int16_t) (dst_x + cw - 1), (int16_t) (dst_y + ch - 1)};
    if (!this->queue_rect_(r))
      this->mark_dirty_(dst_x, dst_y, cw, ch);
  } else {
    this->mark_dirty_(dst_x, dst_y, cw, ch);
  }
}

// -------------------------------------------------------------------- socket setup

void VNCDisplay::start_socket_() {
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
  ESP_LOGI(TAG, "VNC listening on port %u", this->port_);
}

void VNCDisplay::end_socket_() {
  this->disconnect_();
  if (this->listen_sock_ != nullptr) {
    this->listen_sock_->close();
    this->listen_sock_ = nullptr;
  }
}

void VNCDisplay::accept_client_() {
  if (this->auth_retry_at_ != 0) {
    if ((int32_t) (millis() - this->auth_retry_at_) < 0)
      return;  // still backing off from a failed password
    this->auth_retry_at_ = 0;
  }
  struct sockaddr_storage source_addr;
  socklen_t addr_len = sizeof(source_addr);
  auto sock = this->listen_sock_->accept((struct sockaddr *) &source_addr, &addr_len);
  if (sock == nullptr)
    return;
  sock->setblocking(false);
  // Framebuffer updates are latency sensitive and are written in bursts that often end on a
  // partial segment; Nagle would hold that back waiting for an ACK, adding a round trip to
  // every screen update. Not fatal if the client or stack refuses it.
  {
    int enable = 1;
    if (sock->setsockopt(IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(int)) != 0)
      ESP_LOGW(TAG, "Could not set TCP_NODELAY: errno %d", errno);
  }
  {
    LockGuard guard(this->sock_mutex_);
    this->client_sock_ = std::move(sock);
  }
  this->disconnect_pending_ = false;
  this->state_ = STATE_VERSION;
  buf_clr(this->inq_);
  this->skip_bytes_ = 0;
  this->handshake_have_ = 0;
  ESP_LOGD(TAG, "Client connected");
  if (this->write_(RFB_MAGIC, sizeof RFB_MAGIC) < 0)
    this->disconnect_pending_ = true;
}

void VNCDisplay::disconnect_() {
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
  this->handshake_have_ = 0;
  this->mark_clean_();
  if (!was_connected)
    return;
  ESP_LOGD(TAG, "Client disconnected");
  if (this->on_disconnect_ != nullptr)
    this->defer([this]() { this->on_disconnect_(); });
}

// ----------------------------------------------------------------------- socket IO

ssize_t VNCDisplay::read_(uint8_t *buffer, size_t len) {
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
  if (errno_is_disconnect()) {
    ESP_LOGD(TAG, "Connection closed by peer while reading (errno %d)", errno);
  } else {
    ESP_LOGW(TAG, "Socket read failed: errno %d", errno);
  }
  this->disconnect_pending_ = true;
  return -1;
}

ssize_t VNCDisplay::write_(const uint8_t *buffer, size_t len) {
  const uint8_t *ptr = buffer;
  size_t remaining = len;
  uint32_t started = millis();
  while (remaining != 0) {
    ssize_t res;
    {
      // The lock is released between retries so a stalled client cannot block the main loop.
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
      if (errno_is_disconnect()) {
        ESP_LOGD(TAG, "Connection closed by peer while writing (errno %d)", errno);
      } else {
        ESP_LOGW(TAG, "Socket write failed: errno %d", errno);
      }
      this->disconnect_pending_ = true;
      return -1;
    }
    remaining -= res;
    ptr += res;
  }
  return (ssize_t) len;
}

// ----------------------------------------------------------------- transmit pipeline

bool VNCDisplay::queue_rect_(const rect_t &r) {
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

void VNCDisplay::tx_8(uint8_t value) { this->tx_buf_[this->tx_buflen_++] = value; }

void VNCDisplay::tx_16(uint16_t value) {
  put16_be(this->tx_buf_ + this->tx_buflen_, value);
  this->tx_buflen_ += 2;
}

bool VNCDisplay::tx_flush_() {
  if (this->tx_buflen_ == 0)
    return true;
  ssize_t res = this->write_(this->tx_buf_, this->tx_buflen_);
  this->tx_buflen_ = 0;  // never retry stale data, the RFB stream is byte-exact
  return res >= 0;
}

/// Pack one rectangle into the transmit buffer, flushing as required. Returns false once the
/// socket has failed, so a client that disappears mid-frame does not cost us the rest of the
/// framebuffer in pointless PSRAM reads.
bool VNCDisplay::send_framebuffer_(const rect_t &r) {
  size_t x_start = r.x_min;
  size_t y_start = r.y_min;
  size_t w = r.x_max - r.x_min + 1;
  size_t h = r.y_max - r.y_min + 1;
  ESP_LOGV(TAG, "Send framebuffer %u/%u %ux%u", (unsigned) x_start, (unsigned) y_start, (unsigned) w, (unsigned) h);
  if (this->tx_rem_() < 12 && !this->tx_flush_())
    return false;
  this->tx_16(x_start);
  this->tx_16(y_start);
  this->tx_16(w);
  this->tx_16(h);
  this->tx_16(0);  // raw encoding
  this->tx_16(0);
  const bool full_width = (x_start == 0 && w == (size_t) this->width_);
  if (full_width && h * w * PIXEL_BYTES >= TX_BUF_SIZE) {
    // Full-width rectangles are contiguous in the framebuffer, so send them directly rather
    // than copying through the staging buffer. This is the common case for a full refresh.
    if (!this->tx_flush_())
      return false;
    const uint8_t *src = this->display_buffer_ + y_start * this->width_ * PIXEL_BYTES;
    return this->write_(src, h * w * PIXEL_BYTES) >= 0;
  }
  for (size_t y = 0; y != h; y++) {
    const uint8_t *src = this->display_buffer_ + ((y + y_start) * this->width_ + x_start) * PIXEL_BYTES;
    size_t remaining = w * PIXEL_BYTES;
    // A row can be wider than the staging buffer, so copy it in chunks rather than assuming
    // it fits after a flush.
    while (remaining != 0) {
      if (this->tx_rem_() == 0 && !this->tx_flush_())
        return false;
      size_t chunk = std::min(remaining, this->tx_rem_());
      memcpy(this->tx_buf_ + this->tx_buflen_, src, chunk);
      this->tx_buflen_ += chunk;
      src += chunk;
      remaining -= chunk;
    }
  }
  return true;
}

void VNCDisplay::send_batch_(const rect_t *rects, size_t count) {
  // state_ can change underneath us at any point; the null check inside write_() is what
  // actually makes this safe, this is just an early-out for the common disconnected case.
  if (count == 0 || this->state_ != STATE_READY)
    return;
  this->tx_buflen_ = 0;
  this->tx_8(0);  // FramebufferUpdate
  this->tx_8(0);  // padding
  this->tx_16(count);
  for (size_t i = 0; i != count; i++) {
    if (!this->send_framebuffer_(rects[i]))
      return;  // socket gone - the main loop tears it down and drains the queue
  }
  this->tx_flush_();
}

void VNCDisplay::tx_task_() {
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

// ------------------------------------------------------------------- dirty tracking

void VNCDisplay::mark_dirty_(int x, int y, int w, int h) {
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

void VNCDisplay::mark_clean_() {
  this->dirty_rect_.x_min = this->width_;
  this->dirty_rect_.y_min = this->height_;
  this->dirty_rect_.x_max = 0;
  this->dirty_rect_.y_max = 0;
}

bool VNCDisplay::is_dirty_() const {
  return this->dirty_rect_.x_max >= this->dirty_rect_.x_min && this->dirty_rect_.y_max >= this->dirty_rect_.y_min;
}

void VNCDisplay::update_frame_() {
  if (this->is_dirty_() && this->state_ == STATE_READY && this->queue_rect_(this->dirty_rect_))
    this->mark_clean_();
}

// -------------------------------------------------------------------- RFB protocol

bool VNCDisplay::read_exact_(size_t len) {
  while (this->handshake_have_ < len) {
    ssize_t err = this->read_(this->handshake_buf_ + this->handshake_have_, len - this->handshake_have_);
    if (err <= 0)
      return false;  // would block, or the client went away
    this->handshake_have_ += err;
  }
  this->handshake_have_ = 0;
  return true;
}

size_t VNCDisplay::build_init_(uint8_t *buffer) {
  uint8_t *sp = buffer;
  sp = put16_be(sp, this->width_);
  sp = put16_be(sp, this->height_);
  *sp++ = 16;               // bits per pixel
  *sp++ = 16;               // bit depth
  *sp++ = 1;                // big-endian
  *sp++ = 1;                // true colour
  sp = put16_be(sp, 0x1F);  // red max   (5 bits)
  sp = put16_be(sp, 0x3F);  // green max (6 bits)
  sp = put16_be(sp, 0x1F);  // blue max  (5 bits)
  *sp++ = 11;               // red shift
  *sp++ = 5;                // green shift
  *sp++ = 0;                // blue shift
  *sp++ = 0;                // padding
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
bool VNCDisplay::process_() {
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
        if (buffer[4] != 16 || buffer[6] == 0 || buffer[7] == 0) {
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
        ESP_LOGV(TAG, "Discarding %u byte cut buffer", (unsigned) this->skip_bytes_);
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

void VNCDisplay::client_loop_() {
  uint8_t buffer[128];
  ssize_t err;

  switch (this->state_) {
    case STATE_VERSION: {
      if (!this->read_exact_(VERSION_LEN))
        break;
      ESP_LOGD(TAG, "Read %.*s as version", (int) VERSION_LEN - 1, this->handshake_buf_);
      // RFB 3.3: the server dictates the security type as a 4 byte big-endian word.
      buffer[0] = 0;
      buffer[1] = 0;
      buffer[2] = 0;
      if (this->password_[0] == '\0') {
        buffer[3] = AUTH_NONE;
        if (this->write_(buffer, 4) < 0)
          break;
        this->state_ = STATE_CLIENT_INIT;
        break;
      }
      // VNC Authentication: the security type is followed by a 16 byte random challenge.
      buffer[3] = AUTH_VNC;
      if (!random_bytes(this->challenge_, CHALLENGE_LEN)) {
        ESP_LOGE(TAG, "Could not generate an authentication challenge");
        this->disconnect_pending_ = true;
        break;
      }
      memcpy(buffer + 4, this->challenge_, CHALLENGE_LEN);
      if (this->write_(buffer, 4 + CHALLENGE_LEN) < 0)
        break;
      this->state_ = STATE_AUTH;
      break;
    }

    case STATE_AUTH: {
      // The client returns the challenge DES-encrypted with the password.
      if (!this->read_exact_(CHALLENGE_LEN))
        break;
      uint8_t expected[CHALLENGE_LEN];
      vnc_auth_response(this->password_, strlen(this->password_), this->challenge_, expected);
      // Constant-time compare so a wrong password leaks nothing through timing.
      uint8_t diff = 0;
      for (size_t i = 0; i != CHALLENGE_LEN; i++)
        diff |= this->handshake_buf_[i] ^ expected[i];
      buffer[0] = 0;
      buffer[1] = 0;
      buffer[2] = 0;
      buffer[3] = diff == 0 ? 0 : 1;  // SecurityResult: 0 = OK, 1 = failed
      this->write_(buffer, 4);
      if (diff != 0) {
        // RFB 3.3 has no reason string - the server just closes the connection.
        ESP_LOGW(TAG, "Authentication failed, dropping client");
        this->auth_retry_at_ = millis() + AUTH_RETRY_DELAY_MS;
        if (this->auth_retry_at_ == 0)
          this->auth_retry_at_ = 1;
        this->disconnect_pending_ = true;
        break;
      }
      ESP_LOGD(TAG, "Client authenticated");
      this->state_ = STATE_CLIENT_INIT;
      break;
    }

    case STATE_CLIENT_INIT: {
      // ClientInit is a single byte: the shared-desktop flag.
      if (!this->read_exact_(1))
        break;
      ESP_LOGV(TAG, "ClientInit shared flag %u", this->handshake_buf_[0]);
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
        // Never read more than will fit, so nothing that leaves the socket is ever dropped.
        size_t space = std::min(sizeof buffer, buf_space(this->inq_));
        if (space == 0)
          break;
        err = this->read_(buffer, space);
        if (err <= 0)
          break;
        buf_add(this->inq_, buffer, err);
      }
      break;

    default:
      break;
  }
}

}  // namespace esphome::vnc

#endif  // defined(USE_HOST) || defined(USE_ESP32)
