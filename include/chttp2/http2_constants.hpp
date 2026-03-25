#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace chttp2 {
using Flags = uint8_t;
using StreamId = uint32_t;

static const std::string HTTP2_CONNECTION_PREFACE = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

static const std::string GET = "GET";
static const std::string POST = "POST";
static const std::string PUT = "PUT";
static const std::string DELETE = "DELETE";
static const std::string HEAD = "HEAD";
static const std::string OPTIONS = "OPTIONS";
static const std::string PATCH = "PATCH";
static const std::string CONTENT_LENGTH = "content-length";

enum class FrameType : uint8_t {
  DATA = 0x0,
  HEADERS = 0x1,
  PRIORITY = 0x2,
  RST_STREAM = 0x3,
  SETTINGS = 0x4,
  PUSH_PROMISE = 0x5,
  PING = 0x6,
  GOAWAY = 0x7,
  WINDOW_UPDATE = 0x8,
  CONTINUATION = 0x9,
  INVALID = 0xff
};

enum class FrameFlag : uint8_t {
  END_STREAM = 0x1,   // DATA, HEADERS
  ACK = 0x1,          // SETTINGS, PING
  END_HEADERS = 0x4,  // HEADERS, PUSH_PROMISE, CONTINUATION
  PADDED = 0x8,       // DATA, HEADERS
  PRIORITY = 0x20,    // HEADERS
};

enum class ErrorCode : uint32_t {
  NO_ERROR = 0x0,
  PROTOCOL_ERROR = 0x1,
  INTERNAL_ERROR = 0x2,
  FLOW_CONTROL_ERROR = 0x3,
  SETTINGS_TIMEOUT = 0x4,
  STREAM_CLOSED = 0x5,
  FRAME_SIZE_ERROR = 0x6,
  REFUSED_STREAM = 0x7,
  CANCEL = 0x8,
  COMPRESSION_ERROR = 0x9,
  CONNECT_ERROR = 0xa,
  ENHANCE_YOUR_CALM = 0xb,
  INADEQUATE_SECURITY = 0xc,
  HTTP_1_1_REQUIRED = 0xd
};

enum class Http2SettingParameter : uint16_t {
  HEADER_TABLE_SIZE = 0x1,
  ENABLE_PUSH = 0x2,
  MAX_CONCURRENT_STREAMS = 0x3,
  INITIAL_WINDOW_SIZE = 0x4,
  MAX_FRAME_SIZE = 0x5,
  MAX_HEADER_LIST_SIZE = 0x6
};

enum class StreamState {
  IDLE,
  RESERVED_LOCAL,
  RESERVED_REMOTE,
  OPEN,
  HALF_CLOSED_LOCAL,
  HALF_CLOSED_REMOTE,
  CLOSED
};

static const std::vector<std::string> K_HTTP2_PROTOCOL = {"h2"};

inline bool isSupportedHttp2SettingParameter(uint32_t param) {
  return 0 < param && param <= static_cast<uint32_t>(Http2SettingParameter::MAX_HEADER_LIST_SIZE);
}

inline bool isSupportedHttp2SettingParameter(Http2SettingParameter v) {
  return isSupportedHttp2SettingParameter(static_cast<uint32_t>(v));
}

class Http2SettingInfo {
 public:
  static constexpr uint32_t defaultMaxConcurrentStreams() { return 250; };
  static constexpr uint32_t defaultHeaderTableSize() { return 4096; };
  static constexpr bool defaultEnablePush() { return false; };
  static constexpr uint32_t defaultInitialWindowSize() { return 65535; };
  static constexpr uint32_t maximumWindowSize() { return 0x7fffffff; };
  static constexpr uint32_t defaultMaxFrameSize() { return 16384; };

  static constexpr uint32_t minimumMaxFrameSize() { return 16384; };
  static constexpr uint32_t maximumMaxFrameSize() { return (1 << 24) - 1; };
  static constexpr uint32_t defaultMaxHeaderListSize() { return 10 * (1 << 20); };

  static constexpr int32_t defaultUploadBufferSize() { return 1 << 20; };
};

inline std::string frameTypeString(FrameType type) {
  switch (type) {
    case FrameType::DATA:
      return "DATA";  // 0x00
    case FrameType::HEADERS:
      return "HEADERS";  // 0x01
    case FrameType::PRIORITY:
      return "PRIORITY";  // 0x02
    case FrameType::RST_STREAM:
      return "RST_STREAM";  // 0x03
    case FrameType::SETTINGS:
      return "SETTINGS";  // 0x04
    case FrameType::PUSH_PROMISE:
      return "PUSH_PROMISE";  // 0x05
    case FrameType::PING:
      return "PING";  // 0x06
    case FrameType::GOAWAY:
      return "GOAWAY";  // 0x07
    case FrameType::WINDOW_UPDATE:
      return "WINDOW_UPDATE";  // 0x08
    case FrameType::CONTINUATION:
      return "CONTINUATION";  // 0x09
    default:
      return "INVALID";
  }
}

inline std::string errorCodeString(ErrorCode code) {
  switch (code) {
    case ErrorCode::NO_ERROR:
      return "NO_ERROR";
    case ErrorCode::PROTOCOL_ERROR:
      return "PROTOCOL_ERROR";
    case ErrorCode::INTERNAL_ERROR:
      return "INTERNAL_ERROR";
    case ErrorCode::FLOW_CONTROL_ERROR:
      return "FLOW_CONTROL_ERROR";
    case ErrorCode::SETTINGS_TIMEOUT:
      return "SETTINGS_TIMEOUT";
    case ErrorCode::STREAM_CLOSED:
      return "STREAM_CLOSED";
    case ErrorCode::FRAME_SIZE_ERROR:
      return "FRAME_SIZE_ERROR";
    case ErrorCode::REFUSED_STREAM:
      return "REFUSED_STREAM";
    case ErrorCode::CANCEL:
      return "CANCEL";
    case ErrorCode::COMPRESSION_ERROR:
      return "COMPRESSION_ERROR";
    case ErrorCode::CONNECT_ERROR:
      return "CONNECT_ERROR";
    case ErrorCode::ENHANCE_YOUR_CALM:
      return "ENHANCE_YOUR_CALM";
    case ErrorCode::INADEQUATE_SECURITY:
      return "INADEQUATE_SECURITY";
    case ErrorCode::HTTP_1_1_REQUIRED:
      return "HTTP_1_1_REQUIRED";
    default:
      return "UNKNOWN";
  }
}

inline std::string streamStateString(StreamState state) {
  switch (state) {
    case StreamState::IDLE:
      return "IDLE";
    case StreamState::RESERVED_LOCAL:
      return "RESERVED_LOCAL";
    case StreamState::RESERVED_REMOTE:
      return "RESERVED_REMOTE";
    case StreamState::OPEN:
      return "OPEN";
    case StreamState::HALF_CLOSED_LOCAL:
      return "HALF_CLOSED_LOCAL";
    case StreamState::HALF_CLOSED_REMOTE:
      return "HALF_CLOSED_REMOTE";
    case StreamState::CLOSED:
      return "CLOSED";
    default:
      return "UNKNOWN";
  }
}

constexpr uint8_t K_HEADER_SIZE = 9;

struct StatusCode {
  static const std::uint16_t OK = 200;
  static const std::uint16_t CREATED = 201;
  static const std::uint16_t ACCEPTED = 202;
  static const std::uint16_t NO_CONTENT = 204;
  static const std::uint16_t RESET_CONTENT = 205;
  static const std::uint16_t PARTIAL_CONTENT = 206;
  static const std::uint16_t MULTIPLE_CHOICES = 300;
  static const std::uint16_t MOVED_PERMANENTLY = 301;
  static const std::uint16_t FOUND = 302;
  static const std::uint16_t SEE_OTHER = 303;
  static const std::uint16_t NOT_MODIFIED = 304;
  static const std::uint16_t TEMPORARY_REDIRECT = 307;
  static const std::uint16_t PERMANENT_REDIRECT = 308;
  static const std::uint16_t BAD_REQUEST = 400;
  static const std::uint16_t UNAUTHORIZED = 401;
  static const std::uint16_t PAYMENT_REQUIRED = 402;
  static const std::uint16_t FORBIDDEN = 403;
  static const std::uint16_t NOT_FOUND = 404;
  static const std::uint16_t METHOD_NOT_ALLOWED = 405;
  static const std::uint16_t NOT_ACCEPTABLE = 406;
  static const std::uint16_t PROXY_AUTHENTICATION_REQUIRED = 407;
  static const std::uint16_t REQUEST_TIMEOUT = 408;
  static const std::uint16_t CONFLICT = 409;
  static const std::uint16_t GONE = 410;
  static const std::uint16_t LENGTH_REQUIRED = 411;
  static const std::uint16_t PRECONDITION_FAILED = 412;
  static const std::uint16_t PAYLOAD_TOO_LARGE = 413;
  static const std::uint16_t URI_TOO_LONG = 414;
  static const std::uint16_t UNSUPPORTED_MEDIA_TYPE = 415;
  static const std::uint16_t RANGE_NOT_SATISFIABLE = 416;
  static const std::uint16_t EXPECTATION_FAILED = 417;
  static const std::uint16_t IM_A_TEAPOT = 418;
  static const std::uint16_t MISDIRECTED_REQUEST = 421;
  static const std::uint16_t UPGRAGE_REQUIRED = 426;
  static const std::uint16_t PRECONDITION_REQUIRED = 428;
  static const std::uint16_t TOO_MANY_REQUESTS = 429;
  static const std::uint16_t REQUEST_HEADER_FIELDS_TOO_LARGE = 431;
  static const std::uint16_t UNAVAILABLE_FOR_LEGAL_REASONS = 451;
  static const std::uint16_t INTERNAL_SERVER_ERROR = 500;
  static const std::uint16_t NOT_IMPLEMENTED = 501;
  static const std::uint16_t BAD_GATEWAY = 502;
  static const std::uint16_t SERVICE_UNAVAILABLE = 503;
  static const std::uint16_t GATEWAY_TIMEOUT = 504;
  static const std::uint16_t HTTP_VERSION_NOT_SUPPORTED = 505;
  static const std::uint16_t VARIANT_ALSO_NEGOTIATES = 506;
  static const std::uint16_t NETWORK_AUTHENTICATION_REQUIRED = 511;
};

}  // namespace chttp2
