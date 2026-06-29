#include "rtsp_session.hpp"

#include <limits>
#include "mbedtls/md.h"
#include <cstdio>
#include <cstring>

using namespace espp;

namespace {

std::string make_rtsp_url(std::string_view server_address, std::string_view path) {
  while (!path.empty() && path.front() == '/') {
    path.remove_prefix(1);
  }
  if (path.empty()) {
    return "rtsp://" + std::string(server_address);
  }
  return "rtsp://" + std::string(server_address) + "/" + std::string(path);
}

bool parse_decimal_int(std::string_view text, int &value) {
  if (text.empty()) {
    return false;
  }
  int parsed = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    int digit = c - '0';
    if (parsed > (std::numeric_limits<int>::max() - digit) / 10) {
      return false;
    }
    parsed = parsed * 10 + digit;
  }
  value = parsed;
  return true;
}

static std::string md5_hex(const std::string &input) {
  uint8_t output[16];
  const auto *info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
  if (!info) return {};
  mbedtls_md(info, reinterpret_cast<const uint8_t*>(input.data()), input.size(), output);
  char hex[33];
  for (int i = 0; i < 16; i++) {
    snprintf(hex + i*2, 3, "%02x", output[i]);
  }
  return std::string(hex, 32);
}

static std::string make_nonce() {
  uint32_t val = esp_random();
  char buf[9];
  snprintf(buf, sizeof(buf), "%08x", (unsigned int)val);
  return std::string(buf);
}

static std::string make_unauthorized_response(const std::string &realm) {
  return "WWW-Authenticate: Digest realm=\"" + realm + "\", nonce=\"" + make_nonce()
       + "\", algorithm=MD5\r\n";
}

} // namespace

RtspSession::RtspSession(std::shared_ptr<TcpSocket> control_socket, const Config &config)
    : BaseComponent("RtspSession", config.log_level)
    , control_socket_(std::move(control_socket))
    , session_id_(generate_session_id())
    , server_address_(config.server_address)
    , rtsp_path_(config.rtsp_path)
    , client_address_(control_socket_->get_remote_info().address)
    , sdp_generator_(config.sdp_generator)
    , auth_username_(config.auth_username)
    , auth_password_(config.auth_password)
    , auth_realm_(config.auth_realm.empty() ? "RTSP" : config.auth_realm) {
  // set the logger tag to include the session id
  logger_.set_tag("RtspSession " + std::to_string(session_id_));
  // ensure there is a timeout on the control socket receive
  control_socket_->set_receive_timeout(config.receive_timeout);
  // start the session task to handle RTSP commands
  using namespace std::placeholders;
  control_task_ = std::make_unique<Task>(Task::Config{
      .callback = std::bind(&RtspSession::control_task_fn, this, _1, _2, _3),
      .task_config =
          {
              .name = "RtspSession " + std::to_string(session_id_),
              .stack_size_bytes = config.control_task_stack_size_bytes == 0
                                      ? RtspSession::Config::default_control_task_stack_size_bytes
                                      : config.control_task_stack_size_bytes,
          },
      .log_level = Logger::Verbosity::WARN,
  });
  if (!control_task_->start()) {
    logger_.error("Failed to start RTSP control task");
    teardown();
    control_socket_->close();
  }
}

RtspSession::~RtspSession() {
  teardown();
  if (control_socket_) {
    control_socket_->close();
  }
  // stop the session task
  if (control_task_ && control_task_->is_started()) {
    logger_.info("Stopping control task");
    control_task_->stop();
  }
}

uint32_t RtspSession::get_session_id() const { return session_id_; }

bool RtspSession::is_closed() const { return closed_; }

bool RtspSession::is_connected() const {
  return control_socket_ && control_socket_->is_connected();
}

bool RtspSession::is_active() const { return session_active_; }

void RtspSession::play() { session_active_ = true; }

void RtspSession::pause() { session_active_ = false; }

void RtspSession::teardown() {
  session_active_ = false;
  closed_ = true;
}

bool RtspSession::send_rtp_packet(int track_id, const RtpPacket &packet) {
  return send_rtp_packet(track_id, packet.get_data());
}

bool RtspSession::send_rtp_packet(int track_id, std::span<const uint8_t> packet_data) {
  auto it = tracks_.find(track_id);
  if (it == tracks_.end() || !it->second) {
    logger_.debug("Skipping RTP packet for unconfigured track {}", track_id);
    return true;
  }
  auto &track = *it->second;

  // TCP interleaved transport: send RTP over the RTSP control connection
  if (track.is_tcp_interleaved) {
    if (!control_socket_ || !control_socket_->is_connected()) {
      logger_.warn("Control socket closed, cannot send interleaved RTP for track {}", track_id);
      return false;
    }
    // Interleaved binary frame: '$' (0x24) + channel + length(16-bit BE) + RTP data
    size_t rtp_len = packet_data.size();
    std::vector<uint8_t> framed;
    framed.reserve(4 + rtp_len);
    framed.push_back(0x24); // '$'
    framed.push_back(static_cast<uint8_t>(track.interleaved_rtp_channel));
    framed.push_back(static_cast<uint8_t>((rtp_len >> 8) & 0xFF));
    framed.push_back(static_cast<uint8_t>(rtp_len & 0xFF));
    framed.insert(framed.end(), packet_data.begin(), packet_data.end());
    return control_socket_->transmit(std::span<const uint8_t>(framed));
  }

  // UDP transport
  if (track.client_rtp_port <= 0) {
    logger_.debug("Skipping RTP packet for track {} without RTP transport", track_id);
    return true;
  }
  logger_.debug("Sending RTP packet on track {}", track_id);
  return track.rtp_socket.send(packet_data, {
                                                .ip_address = client_address_,
                                                .port = (size_t)track.client_rtp_port,
                                            });
}

bool RtspSession::send_rtp_packet(const RtpPacket &packet) { return send_rtp_packet(0, packet); }

bool RtspSession::send_rtp_packet(std::span<const uint8_t> packet_data) {
  return send_rtp_packet(0, packet_data);
}

bool RtspSession::send_rtcp_packet(int track_id, const RtcpPacket &packet) {
  auto it = tracks_.find(track_id);
  if (it == tracks_.end() || !it->second) {
    logger_.debug("Skipping RTCP packet for unconfigured track {}", track_id);
    return true;
  }
  auto &track = *it->second;
  // TCP interleaved transport: send RTCP over the RTSP control connection
  if (track.is_tcp_interleaved) {
    if (!control_socket_ || !control_socket_->is_connected()) {
      return false;
    }
    auto rtcp_data = packet.get_data();
    size_t rtcp_len = rtcp_data.size();
    std::vector<uint8_t> framed;
    framed.reserve(4 + rtcp_len);
    framed.push_back(0x24);
    framed.push_back(static_cast<uint8_t>(track.interleaved_rtcp_channel));
    framed.push_back(static_cast<uint8_t>((rtcp_len >> 8) & 0xFF));
    framed.push_back(static_cast<uint8_t>(rtcp_len & 0xFF));
    framed.insert(framed.end(), rtcp_data.begin(), rtcp_data.end());
    return control_socket_->transmit(std::span<const uint8_t>(framed));
  }
  // UDP transport
  if (track.client_rtcp_port <= 0) {
    logger_.debug("Skipping RTCP packet for track {} without RTCP transport", track_id);
    return true;
  }
  logger_.debug("Sending RTCP packet on track {}", track_id);
  return track.rtcp_socket.send(packet.get_data(), {
                                                       .ip_address = client_address_,
                                                       .port = (size_t)track.client_rtcp_port,
                                                   });
}

bool RtspSession::send_rtcp_packet(const RtcpPacket &packet) { return send_rtcp_packet(0, packet); }

bool RtspSession::send_response(int code, std::string_view message, int sequence_number,
                                std::string_view headers, std::string_view body) {
  // create a response
  std::string response = "RTSP/1.0 " + std::to_string(code) + " " + std::string(message) + "\r\n";
  if (sequence_number != -1) {
    response += "CSeq: " + std::to_string(sequence_number) + "\r\n";
  }
  if (!headers.empty()) {
    response += headers;
  }
  if (!body.empty()) {
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "\r\n";
    response += body;
  } else {
    response += "\r\n";
  }
  logger_.info("Sending RTSP response");
  logger_.debug("{}", response);
  // send the response
  return control_socket_->transmit(response);
}

/* ------------------------------------------------------------------ */
/*  Digest Authentication                                              */
/* ------------------------------------------------------------------ */

bool RtspSession::check_auth(std::string_view request, int cseq, const std::string &method) {
  if (auth_username_.empty()) {
    return true; // auth disabled
  }

  // Look for Authorization: Digest header
  auto auth_pos = request.find("Authorization: Digest ");
  if (auth_pos == std::string_view::npos) {
    // No auth header - send 401 with WWW-Authenticate challenge
    logger_.info("Auth: no credentials, sending 401 challenge");
    send_response(401, "Unauthorized", cseq,
                         make_unauthorized_response(auth_realm_));
    return false;
  }

  // Parse the digest parameters from the Authorization value
  auto val_start = auth_pos + strlen("Authorization: Digest ");
  auto val_end = request.find('\r', val_start);
  if (val_end == std::string_view::npos) {
    val_end = request.size();
  }
  std::string_view auth_val = request.substr(val_start, val_end - val_start);

  // Helper to extract a quoted parameter value
  auto extract = [&](const std::string &key) -> std::string {
    auto kp = auth_val.find(key + "=\"");
    if (kp == std::string_view::npos) return {};
    auto vs = kp + key.length() + 2;
    if (vs >= auth_val.size()) return {};
    auto ve = auth_val.find('"', vs);
    if (ve == std::string_view::npos) return {};
    return std::string(auth_val.substr(vs, ve - vs));
  };

  std::string client_user  = extract("username");
  std::string client_realm = extract("realm");
  std::string client_nonce = extract("nonce");
  std::string client_uri   = extract("uri");
  std::string client_resp  = extract("response");

  if (client_user.empty() || client_resp.empty()) {
    logger_.info("Auth: missing username or response");
    send_response(401, "Unauthorized", cseq,
                         make_unauthorized_response(auth_realm_));
    return false;
  }

  if (client_user != auth_username_ || client_realm != auth_realm_) {
    logger_.info("Auth: username/realm mismatch");
    send_response(401, "Unauthorized", cseq,
                         make_unauthorized_response(auth_realm_));
    return false;
  }

  // RFC 2617 digest without qop:
  //   HA1 = MD5(username:realm:password)
  //   HA2 = MD5(method:uri)
  //   response = MD5(HA1:nonce:HA2)
  std::string ha1 = md5_hex(auth_username_ + ":" + auth_realm_ + ":" + auth_password_);
  std::string ha2 = md5_hex(method + ":" + client_uri);
  std::string expected = md5_hex(ha1 + ":" + client_nonce + ":" + ha2);

  if (strcasecmp(client_resp.c_str(), expected.c_str()) != 0) {
    logger_.info("Auth: digest mismatch");
    send_response(401, "Unauthorized", cseq,
                         make_unauthorized_response(auth_realm_));
    return false;
  }

  logger_.info("Auth: credentials accepted");
  return true;
}

/* ------------------------------------------------------------------ */
/*  RTSP Request Handlers                                              */
/* ------------------------------------------------------------------ */

bool RtspSession::handle_rtsp_options(std::string_view request) {
  int sequence_number = 0;
  if (!parse_rtsp_command_sequence(request, sequence_number)) {
    return handle_rtsp_invalid_request(request);
  }
  logger_.info("RTSP OPTIONS request");
  // create a response
  int code = 200;
  std::string message = "OK";
  std::string headers = "Public: DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE\r\n";
  return send_response(code, message, sequence_number, headers);
}

bool RtspSession::handle_rtsp_describe(std::string_view request) {
  int sequence_number = 0;
  if (!parse_rtsp_command_sequence(request, sequence_number)) {
    return handle_rtsp_invalid_request(request);
  }
  if (!check_auth(request, sequence_number, "DESCRIBE")) {
    return false;
  }
  logger_.info("RTSP DESCRIBE request");
  // create a response
  int code = 200;
  std::string message = "OK";
  std::string rtsp_path = make_rtsp_url(server_address_, rtsp_path_);

  std::string body;
  if (sdp_generator_) {
    body = sdp_generator_(rtsp_path, session_id_, server_address_);
  } else {
    // Default SDP description for an MJPEG stream (backward compatibility)
    body = "v=0\r\n" // version (0)
           "o=- " +
           std::to_string(session_id_) + " 1 IN IP4 " + server_address_ +
           "\r\n"               // username (none), session id, version, network type (internet),
                                // address type, address
           "s=MJPEG Stream\r\n" // session name (can be anything)
           "i=MJPEG Stream\r\n" // session name (can be anything)
           "t=0 0\r\n"          // start / stop
           "a=control:" +
           rtsp_path +
           "\r\n"                                          // the RTSP path
           "a=mimetype:string;\"video/x-motion-jpeg\"\r\n" // MIME type
           "m=video 0 RTP/AVP 26\r\n"                      // MJPEG
           "c=IN IP4 0.0.0.0\r\n"                          // client will use the RTSP address
           "b=AS:256\r\n"                                  // 256kbps
           "a=control:" +
           rtsp_path +
           "\r\n";
  }

  std::string headers = "Content-Type: application/sdp\r\n"
                        "Content-Base: " +
                        rtsp_path + "\r\n";
  return send_response(code, message, sequence_number, headers, body);
}

bool RtspSession::handle_rtsp_setup(std::string_view request) {
  // parse the sequence number from the request (before other parsing so auth works)
  int sequence_number = 0;
  if (!parse_rtsp_command_sequence(request, sequence_number)) {
    return handle_rtsp_invalid_request(request);
  }
  if (!check_auth(request, sequence_number, "SETUP")) {
    return false;
  }
  // parse the rtsp path and transport parameters from the request
  std::string_view rtsp_path;
  int client_rtp_port = 0;
  int client_rtcp_port = 0;
  bool is_tcp_interleaved = false;
  int interleaved_rtp_channel = -1;
  int interleaved_rtcp_channel = -1;
  if (!parse_rtsp_setup_request(request, rtsp_path, client_rtp_port, client_rtcp_port,
                                is_tcp_interleaved, interleaved_rtp_channel, interleaved_rtcp_channel)) {
    // the parse function will send the response, so we just need to
    // teardown the session since setup failed and streaming cannot proceed
    teardown();
    return false;
  }
  logger_.info("RTSP SETUP request (transport: {})", is_tcp_interleaved ? "TCP interleaved" : "UDP");

  // extract track ID from the RTSP URL (e.g., "rtsp://ip:port/path/trackID=0")
  int track_id = 0; // default to track 0 for backward compatibility
  auto track_id_pos = rtsp_path.find("trackID=");
  if (track_id_pos != std::string_view::npos) {
    auto track_id_str = rtsp_path.substr(track_id_pos + 8);
    auto end_pos = track_id_str.find_first_not_of("0123456789");
    if (end_pos != std::string_view::npos) {
      track_id_str = track_id_str.substr(0, end_pos);
    }
    int parsed_track_id = 0;
    if (!track_id_str.empty() && parse_decimal_int(track_id_str, parsed_track_id)) {
      track_id = parsed_track_id;
    } else if (!track_id_str.empty()) {
      logger_.warn("Invalid track ID '{}', defaulting to track 0", track_id_str);
    }
  }

  // create or find the track
  auto &track_ptr = tracks_[track_id];
  if (!track_ptr) {
    track_ptr = std::make_unique<Track>();
  }
  auto &track = *track_ptr;
  track.track_id = track_id;
  track.control_path = "trackID=" + std::to_string(track_id);
  track.setup_complete = true;

  // Build transport header and configure track based on transport mode
  std::string transport_header;
  if (is_tcp_interleaved) {
    track.is_tcp_interleaved = true;
    track.interleaved_rtp_channel = interleaved_rtp_channel;
    track.interleaved_rtcp_channel = interleaved_rtcp_channel;
    transport_header = "Transport: RTP/AVP/TCP;unicast;interleaved=" +
                       std::to_string(interleaved_rtp_channel) + "-" +
                       std::to_string(interleaved_rtcp_channel) + "\r\n";
  } else {
    track.is_tcp_interleaved = false;
    track.client_rtp_port = client_rtp_port;
    track.client_rtcp_port = client_rtcp_port;
    transport_header = "Transport: RTP/AVP;unicast;client_port=" +
                       std::to_string(client_rtp_port) + "-" +
                       std::to_string(client_rtcp_port) + "\r\n";
  }

  // create a response
  int code = 200;
  std::string message = "OK";
  std::string headers =
      "Session: " + std::to_string(session_id_) + "\r\n" + transport_header;
  return send_response(code, message, sequence_number, headers);
}

bool RtspSession::handle_rtsp_play(std::string_view request) {
  int sequence_number = 0;
  if (!parse_rtsp_command_sequence(request, sequence_number)) {
    return handle_rtsp_invalid_request(request);
  }
  if (!check_auth(request, sequence_number, "PLAY")) {
    return false;
  }
  logger_.info("RTSP PLAY request");
  play();
  int code = 200;
  std::string message = "OK";
  std::string headers =
      "Session: " + std::to_string(session_id_) + "\r\n" + "Range: npt=0.000-\r\n";
  return send_response(code, message, sequence_number, headers);
}

bool RtspSession::handle_rtsp_pause(std::string_view request) {
  int sequence_number = 0;
  if (!parse_rtsp_command_sequence(request, sequence_number)) {
    return handle_rtsp_invalid_request(request);
  }
  if (!check_auth(request, sequence_number, "PAUSE")) {
    return false;
  }
  logger_.info("RTSP PAUSE request");
  pause();
  int code = 200;
  std::string message = "OK";
  std::string headers = "Session: " + std::to_string(session_id_) + "\r\n";
  return send_response(code, message, sequence_number, headers);
}

bool RtspSession::handle_rtsp_teardown(std::string_view request) {
  int sequence_number = 0;
  if (!parse_rtsp_command_sequence(request, sequence_number)) {
    return handle_rtsp_invalid_request(request);
  }
  if (!check_auth(request, sequence_number, "TEARDOWN")) {
    return false;
  }
  logger_.info("RTSP TEARDOWN request");
  teardown();
  int code = 200;
  std::string message = "OK";
  std::string headers = "Session: " + std::to_string(session_id_) + "\r\n";
  return send_response(code, message, sequence_number, headers);
}

bool RtspSession::handle_rtsp_invalid_request(std::string_view request, int code,
                                              std::string_view message) {
  logger_.info("RTSP invalid request");
  // create a response
  int sequence_number = 0;
  if (!parse_rtsp_command_sequence(request, sequence_number)) {
    return send_response(code, message);
  }
  return send_response(code, message, sequence_number);
}

bool RtspSession::handle_rtsp_request(std::string_view request) {
  logger_.debug("RTSP request:\n{}", request);
  // store indices of the first and second spaces
  // to extract the method and the rtsp path
  auto first_space_index = request.find(' ');
  auto second_space_index = request.find(' ', first_space_index + 1);
  auto end_of_line_index = request.find('\r');
  if (first_space_index == std::string::npos || second_space_index == std::string::npos ||
      end_of_line_index == std::string::npos) {
    return handle_rtsp_invalid_request(request);
  }
  // extract the method and the rtsp path
  // where the request looks like "METHOD RTSP_PATH RTSP_VERSION"
  std::string_view method = request.substr(0, first_space_index);
  // TODO: we should probably check that the rtsp path is correct
  [[maybe_unused]] std::string_view rtsp_path =
      request.substr(first_space_index + 1, second_space_index - first_space_index - 1);
  // TODO: we should probably check that the rtsp version is correct
  [[maybe_unused]] std::string_view rtsp_version =
      request.substr(second_space_index + 1, end_of_line_index - second_space_index - 1);
  // extract the request body, which is separated by an empty line (\r\n)
  // from the request header
  std::string_view request_body = request.substr(end_of_line_index + 2);

  // handle the request
  if (method == "OPTIONS") {
    return handle_rtsp_options(request_body);
  } else if (method == "DESCRIBE") {
    return handle_rtsp_describe(request_body);
  } else if (method == "SETUP") {
    return handle_rtsp_setup(request);
  } else if (method == "PLAY") {
    return handle_rtsp_play(request_body);
  } else if (method == "PAUSE") {
    return handle_rtsp_pause(request_body);
  } else if (method == "TEARDOWN") {
    return handle_rtsp_teardown(request_body);
  }

  // if the method is not supported, return an error
  return handle_rtsp_invalid_request(request_body);
}

bool RtspSession::control_task_fn(std::mutex &m, std::condition_variable &cv, bool &task_notified) {
  if (closed_) {
    logger_.info("Session is closed, stopping control task");
    // return true to stop the task
    return true;
  }
  if (!control_socket_) {
    logger_.warn("Control socket is no longer valid, stopping control task");
    teardown();
    // return true to stop the task
    return true;
  }
  if (!control_socket_->is_connected()) {
    logger_.warn("Control socket is not connected, stopping control task");
    teardown();
    // if the control socket is not connected, return true to stop the task
    return true;
  }
  static size_t max_request_size = 1024;
  std::vector<uint8_t> buffer;
  logger_.info("Waiting for RTSP request");
  if (control_socket_->receive(buffer, max_request_size)) {
    // Skip any interleaved binary data frames from the client (RTCP feedback)
    // Interleaved frame format: '$' (0x24) + channel(1) + length(2 BE) + data
    size_t offset = 0;
    while (offset + 4 <= buffer.size() && buffer[offset] == 0x24) {
      uint16_t frame_len = (uint16_t(buffer[offset + 2]) << 8) | buffer[offset + 3];
      size_t total_frame = 4 + frame_len;
      if (offset + total_frame > buffer.size()) {
        // Incomplete interleaved frame — discard entire buffer
        logger_.debug("Incomplete interleaved frame ({}+{} > buf {}), skipping",
                     offset, total_frame, buffer.size());
        offset = buffer.size();
        break;
      }
      offset += total_frame;
    }
    if (offset >= buffer.size()) {
      // Buffer was entirely interleaved data — nothing to parse
      return false;
    }
    // parse the remaining data as an RTSP request
    std::string_view request(reinterpret_cast<char *>(buffer.data()) + offset,
                             buffer.size() - offset);
    // handle the request
    if (!handle_rtsp_request(request)) {
      logger_.warn("Failed to handle RTSP request");
    }
  } else {
    // if the receive failed, then let's wait a little / check the task_notified
    // flag to know if we should stop or not.
    using namespace std::chrono_literals;
    std::unique_lock<std::mutex> lk(m);
    auto stop_requested = cv.wait_for(lk, 1ms, [&task_notified] { return task_notified; });
    task_notified = false;
    if (stop_requested) {
      return true;
    }
  }
  // the receive handles most of the blocking, so we don't need to sleep
  // here, just return false to keep the task running
  return false;
}

uint32_t RtspSession::generate_session_id() {
#if defined(ESP_PLATFORM)
  return esp_random();
#else
  static std::random_device rd;
  static std::mt19937 gen(rd());
  static std::uniform_int_distribution<> dis(0, std::numeric_limits<int>::max());
  return dis(gen);
#endif
}

bool RtspSession::parse_rtsp_command_sequence(std::string_view request, int &cseq) {
  // parse the cseq from the request
  auto cseq_index = request.find("CSeq: ");
  if (cseq_index == std::string::npos) {
    return false;
  }
  auto cseq_end_index = request.find('\r', cseq_index);
  if (cseq_end_index == std::string::npos) {
    return false;
  }
  std::string_view cseq_str = request.substr(cseq_index + 6, cseq_end_index - cseq_index - 6);
  if (cseq_str.empty()) {
    return false;
  }
  // convert the cseq to an integer
  return parse_decimal_int(cseq_str, cseq);
}

std::string_view RtspSession::parse_rtsp_path(std::string_view request) {
  // parse the rtsp path from the request
  // where the request looks like "METHOD RTSP_PATH RTSP_VERSION"
  std::string_view rtsp_path = request.substr(
      request.find(' ') + 1, request.find(' ', request.find(' ') + 1) - request.find(' ') - 1);
  return rtsp_path;
}

bool RtspSession::parse_rtsp_setup_request(std::string_view request, std::string_view &rtsp_path,
                                           int &client_rtp_port, int &client_rtcp_port,
                                           bool &is_tcp_interleaved,
                                           int &interleaved_rtp_channel, int &interleaved_rtcp_channel) {
  // defaults
  is_tcp_interleaved = false;
  interleaved_rtp_channel = -1;
  interleaved_rtcp_channel = -1;

  // parse the rtsp path from the request
  rtsp_path = parse_rtsp_path(request);
  if (rtsp_path.empty()) {
    logger_.error("Failed to parse RTSP path from request");
    handle_rtsp_invalid_request(request);
    return false;
  }
  logger_.debug("Parsing setup request:\n{}", request);
  // parse the transport header from the request
  auto transport_index = request.find("Transport: ");
  if (transport_index == std::string::npos) {
    logger_.error("Failed to parse Transport header (start) from request");
    handle_rtsp_invalid_request(request);
    return false;
  }
  auto transport_end_index = request.find('\r', transport_index);
  if (transport_end_index == std::string::npos) {
    logger_.error("Failed to parse Transport header (end) from request");
    handle_rtsp_invalid_request(request);
    return false;
  }
  std::string_view transport =
      request.substr(transport_index + 11, transport_end_index - transport_index - 11);
  if (transport.empty()) {
    logger_.error("Transport header is empty");
    handle_rtsp_invalid_request(request);
    return false;
  }
  logger_.debug("Transport header: {}", transport);

  // Check for TCP interleaved transport
  bool is_tcp = (transport.find("RTP/AVP/TCP") != std::string::npos);

  if (is_tcp) {
    // Parse interleaved=X-Y from the transport header
    auto interleaved_index = transport.find("interleaved=");
    if (interleaved_index == std::string::npos) {
      // TCP without interleaved — default to channels based on track count (0-1, 2-3, etc.)
      // but we need the track info which isn't available here yet.
      // Return error; client must specify interleaved channels.
      logger_.error("TCP transport without interleaved= parameter");
      handle_rtsp_invalid_request(request, 461, "Unsupported Transport");
      return false;
    }
    // parse the two channel numbers
    auto chan_start = interleaved_index + 12; // len of "interleaved="
    auto dash_pos = transport.find('-', chan_start);
    if (dash_pos == std::string::npos) {
      // single channel (RTP only, no RTCP)
      std::string_view chan_str = transport.substr(chan_start);
      // trim trailing semicolons/whitespace
      auto sc = chan_str.find_first_of("; \t");
      if (sc != std::string::npos) chan_str = chan_str.substr(0, sc);
      if (!parse_decimal_int(chan_str, interleaved_rtp_channel)) {
        logger_.error("Failed to parse interleaved channel from request");
        handle_rtsp_invalid_request(request);
        return false;
      }
      interleaved_rtcp_channel = interleaved_rtp_channel + 1; // default
    } else {
      std::string_view rtp_chan = transport.substr(chan_start, dash_pos - chan_start);
      auto rtcp_end = transport.find_first_of("; \t\r", dash_pos + 1);
      if (rtcp_end == std::string::npos) rtcp_end = transport.size();
      std::string_view rtcp_chan = transport.substr(dash_pos + 1, rtcp_end - dash_pos - 1);
      if (!parse_decimal_int(rtp_chan, interleaved_rtp_channel) ||
          !parse_decimal_int(rtcp_chan, interleaved_rtcp_channel)) {
        logger_.error("Failed to parse interleaved channels from request");
        handle_rtsp_invalid_request(request);
        return false;
      }
    }
    is_tcp_interleaved = true;
    client_rtp_port = 0;
    client_rtcp_port = 0;
    logger_.info("TCP interleaved transport: RTP channel={}, RTCP channel={}",
                 interleaved_rtp_channel, interleaved_rtcp_channel);
    return true;
  }

  // UDP transport: parse client_port=X-Y
  auto client_port_index = request.find("client_port=");
  if (client_port_index == std::string::npos) {
    logger_.error("Failed to parse client_port from request");
    handle_rtsp_invalid_request(request);
    return false;
  }
  auto dash_index = request.find('-', client_port_index);
  if (dash_index == std::string::npos || dash_index <= client_port_index + 12) {
    logger_.error("Failed to parse client RTP/RTCP ports from request");
    handle_rtsp_invalid_request(request);
    return false;
  }
  std::string_view rtp_port =
      request.substr(client_port_index + 12, dash_index - client_port_index - 12);
  if (rtp_port.empty()) {
    logger_.error("Failed to parse client RTP port from request");
    handle_rtsp_invalid_request(request);
    return false;
  }
  // parse the rtcp port from the request
  auto rtcp_end_index = request.find('\r', client_port_index);
  if (rtcp_end_index == std::string::npos || rtcp_end_index <= dash_index + 1) {
    logger_.error("Failed to parse client RTCP port from request");
    handle_rtsp_invalid_request(request);
    return false;
  }
  std::string_view rtcp_port = request.substr(dash_index + 1, rtcp_end_index - dash_index - 1);
  if (rtcp_port.empty()) {
    logger_.error("Empty client RTCP port in request");
    handle_rtsp_invalid_request(request);
    return false;
  }
  // convert the rtp and rtcp ports to integers
  if (!parse_decimal_int(rtp_port, client_rtp_port) ||
      !parse_decimal_int(rtcp_port, client_rtcp_port)) {
    logger_.error("Failed to parse client RTP/RTCP ports from request");
    handle_rtsp_invalid_request(request);
    return false;
  }
  return true;
}
