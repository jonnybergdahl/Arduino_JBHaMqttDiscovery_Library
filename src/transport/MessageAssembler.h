#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

/**
 * @brief Puts together incoming messages that an MQTT client hands over in parts.
 *
 * AsyncMqttClient and espMqttClient call their message callback once per received
 * chunk, with the chunk's offset (index) and the full payload length (total). Payloads
 * of more than a few hundred bytes, like retained discovery configs, often come in
 * several parts. The transports collect them here and pass on whole messages only.
 *
 * Parts of one message arrive in order and aren't mixed with other messages, so one
 * buffer is enough. Used from the client's callback only, so it needs no locking.
 */
class MessageAssembler {
public:
  /** @brief Default for the largest message that is put together. */
  static constexpr size_t DEFAULT_MAX_SIZE = 4096;

  enum class Result {
    Complete,   ///< A whole message is ready in topic() and payload(); call release() after use
    Pending,    ///< Part stored, more to come
    TooLarge,   ///< First part of a message larger than the limit; the rest is ignored
    Ignored,    ///< Later part of a message that was too large or broken off
    Broken      ///< Part out of order or for another topic; the message was dropped
  };

  explicit MessageAssembler(size_t maxSize = DEFAULT_MAX_SIZE) : _maxSize(maxSize) {}

  /** @brief Set the largest message that is put together (in bytes). */
  void setMaxSize(size_t maxSize) { _maxSize = maxSize; }

  /** @brief The largest message that is put together (in bytes). */
  size_t maxSize() const { return _maxSize; }

  /**
   * @brief Add one part.
   *
   * @param topic Topic of the message
   * @param data  This part's bytes
   * @param len   Length of this part
   * @param index Offset of this part in the payload
   * @param total Length of the whole payload
   */
  Result add(const char* topic, const uint8_t* data, size_t len, size_t index, size_t total) {
    if (index == 0) {
      release();
      if (total > _maxSize) {
        _skipping = true;
        return Result::TooLarge;
      }
      _skipping = false;
      _active = true;
      _topic = topic ? topic : "";
      _total = total;
      _payload.reserve(total);
    } else if (_skipping) {
      return Result::Ignored;
    } else if (!_active) {
      return Result::Ignored;
    }

    if (_topic != (topic ? topic : "") || index != _payload.size() || index + len > _total) {
      release();
      return Result::Broken;
    }
    if (data && len) {
      _payload.append(reinterpret_cast<const char*>(data), len);
    }
    return _payload.size() == _total ? Result::Complete : Result::Pending;
  }

  /** @brief Topic of the completed message. */
  const std::string& topic() const { return _topic; }

  /** @brief Payload of the completed message. */
  const std::string& payload() const { return _payload; }

  /** @brief Drop the buffered message and free its memory. */
  void release() {
    _active = false;
    _total = 0;
    std::string().swap(_topic);
    std::string().swap(_payload);
  }

private:
  size_t _maxSize;
  bool _active = false;    // collecting a message
  bool _skipping = false;  // ignoring the parts of a message that is too large
  size_t _total = 0;
  std::string _topic;
  std::string _payload;
};
