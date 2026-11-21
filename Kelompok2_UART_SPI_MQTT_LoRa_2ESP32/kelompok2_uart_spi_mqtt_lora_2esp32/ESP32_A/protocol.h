#pragma once
#include <Arduino.h>

static const size_t FRAME_SIZE = 200;
static const size_t HEADER_SIZE = 19;
static const size_t PAYLOAD_SIZE = FRAME_SIZE - HEADER_SIZE;
static const uint8_t TYPE_START = 1;
static const uint8_t TYPE_DATA  = 2;
static const uint8_t TYPE_END   = 3;
static const uint8_t TYPE_ACK   = 4;
static const uint8_t TYPE_ABORT = 5;
static const uint8_t TYPE_ERROR = 6;
static const uint8_t MAGIC[4] = {'K','2','I','2'};

static inline uint32_t crc32Step(uint32_t crc, uint8_t value) {
  crc ^= value;
  for (uint8_t i = 0; i < 8; i++) {
    crc = (crc & 1) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
  }
  return crc;
}

static inline uint32_t frameCRC(const uint8_t *frame) {
  uint16_t len = (uint16_t)frame[9] | ((uint16_t)frame[10] << 8);
  uint32_t crc = 0xFFFFFFFFUL;
  for (uint8_t i = 4; i <= 14; i++) crc = crc32Step(crc, frame[i]);
  for (uint16_t i = 0; i < len; i++) crc = crc32Step(crc, frame[HEADER_SIZE + i]);
  return ~crc;
}

static inline uint16_t frameLen(const uint8_t *f) {
  return (uint16_t)f[9] | ((uint16_t)f[10] << 8);
}

static inline uint32_t frameSeq(const uint8_t *f) {
  return (uint32_t)f[5] |
         ((uint32_t)f[6] << 8) |
         ((uint32_t)f[7] << 16) |
         ((uint32_t)f[8] << 24);
}

static inline uint8_t frameType(const uint8_t *f) {
  return f[4];
}

static inline uint32_t readLE32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void writeLE32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v);
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static inline void makeFrame(uint8_t type, uint32_t session, uint32_t seq,
                             const uint8_t *payload, uint16_t len,
                             uint8_t *out) {
  memset(out, 0, FRAME_SIZE);
  memcpy(out, MAGIC, 4);
  out[4] = type;
  writeLE32(out + 5, seq);
  if (len > PAYLOAD_SIZE) len = PAYLOAD_SIZE;
  writeLE32(out + 11, session);
  out[9] = (uint8_t)(len & 0xFF);
  out[10] = (uint8_t)(len >> 8);
  if (len > PAYLOAD_SIZE) len = PAYLOAD_SIZE;
  if (payload && len) memcpy(out + HEADER_SIZE, payload, len);
  uint32_t crc = frameCRC(out);
  writeLE32(out + 15, crc);
}

static inline bool validFrame(const uint8_t *f) {
  if (memcmp(f, MAGIC, 4) != 0) return false;
  if (!readLE32(f + 11)) return false;
  uint16_t len = frameLen(f);
  if (len > PAYLOAD_SIZE) return false;
  uint32_t expected = readLE32(f + 15);
  return frameCRC(f) == expected;
}

static inline bool isImageFrame(const uint8_t *f) {
  uint8_t t = frameType(f);
  return t == TYPE_START || t == TYPE_DATA || t == TYPE_END;
}

static inline uint32_t frameSession(const uint8_t *f) { return readLE32(f + 11); }
