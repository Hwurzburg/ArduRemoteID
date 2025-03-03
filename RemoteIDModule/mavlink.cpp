/*
  mavlink class for handling OpenDroneID messages
 */
#include <Arduino.h>
#include "mavlink.h"

#define SERIAL_BAUD 115200

static HardwareSerial *serial_ports[MAVLINK_COMM_NUM_BUFFERS];

#include <generated/mavlink_helpers.h>

mavlink_system_t mavlink_system = {2,1};

#define dev_printf(fmt, args ...)  do {Serial.printf(fmt, ## args); } while(0)

/*
  send a buffer out a MAVLink channel
 */
void comm_send_buffer(mavlink_channel_t chan, const uint8_t *buf, uint8_t len)
{
    if (chan >= MAVLINK_COMM_NUM_BUFFERS || serial_ports[uint8_t(chan)] == nullptr) {
        return;
    }
    auto &serial = *serial_ports[uint8_t(chan)];
    serial.write(buf, len);
}

/*
  abstraction for MAVLink on a serial port
 */
MAVLinkSerial::MAVLinkSerial(HardwareSerial &_serial, mavlink_channel_t _chan) :
    serial(_serial),
    chan(_chan)
{
    serial_ports[uint8_t(_chan - MAVLINK_COMM_0)] = &serial;
}

void MAVLinkSerial::init(void)
{
}

void MAVLinkSerial::update(void)
{
    update_send();
    update_receive();
}

void MAVLinkSerial::update_send(void)
{
    uint32_t now_ms = millis();
    if (now_ms - last_hb_ms >= 1000) {
        last_hb_ms = now_ms;
        mavlink_msg_heartbeat_send(
            chan,
            MAV_TYPE_ODID,
            MAV_AUTOPILOT_ARDUPILOTMEGA,
            0,
            0,
            0);

        // send arming status
        arm_status_send();
    }
}

void MAVLinkSerial::update_receive(void)
{
    // receive new packets
    mavlink_message_t msg;
    mavlink_status_t status;
    status.packet_rx_drop_count = 0;

    const uint16_t nbytes = serial.available();
    for (uint16_t i=0; i<nbytes; i++) {
        const uint8_t c = (uint8_t)serial.read();
        // Try to get a new message
        if (mavlink_parse_char(chan, c, &msg, &status)) {
            process_packet(status, msg);
        }
    }
}

/*
  printf via MAVLink STATUSTEXT for debugging
 */
void MAVLinkSerial::mav_printf(uint8_t severity, const char *fmt, ...)
{
    va_list arg_list;
    char text[MAVLINK_MSG_STATUSTEXT_FIELD_TEXT_LEN+1] {};
    va_start(arg_list, fmt);
    vsnprintf(text, sizeof(text), fmt, arg_list);
    va_end(arg_list);
    mavlink_msg_statustext_send(chan,
                                severity,
                                text,
                                0, 0);
}

void MAVLinkSerial::process_packet(mavlink_status_t &status, mavlink_message_t &msg)
{
    const uint32_t now_ms = millis();
    switch (msg.msgid) {
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_LOCATION: {
        dev_printf("Got OPEN_DRONE_ID_LOCATION\n");
        mavlink_msg_open_drone_id_location_decode(&msg, &location);
        packets_received_mask |= PKT_LOCATION;
        last_location_ms = now_ms;
        break;
    }
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_BASIC_ID: {
        dev_printf("Got OPEN_DRONE_ID_BASIC_ID\n");
        mavlink_msg_open_drone_id_basic_id_decode(&msg, &basic_id);
        packets_received_mask |= PKT_BASIC_ID;
        last_basic_id_ms = now_ms;
        break;
    }
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_AUTHENTICATION: {
        dev_printf("Got OPEN_DRONE_ID_AUTHENTICATION\n");
        mavlink_msg_open_drone_id_authentication_decode(&msg, &authentication);
        packets_received_mask |= PKT_AUTHENTICATION;
        break;
    }
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_SELF_ID: {
        dev_printf("Got OPEN_DRONE_ID_SELF_ID\n");
        mavlink_msg_open_drone_id_self_id_decode(&msg, &self_id);
        packets_received_mask |= PKT_SELF_ID;
        last_self_id_ms = now_ms;
        break;
    }
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_SYSTEM: {
        dev_printf("Got OPEN_DRONE_ID_SYSTEM\n");
        mavlink_msg_open_drone_id_system_decode(&msg, &system);
        packets_received_mask |= PKT_SYSTEM;
        last_system_ms = now_ms;
        break;
    }
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_OPERATOR_ID: {
        dev_printf("Got OPEN_DRONE_ID_OPERATOR_ID\n");
        mavlink_msg_open_drone_id_operator_id_decode(&msg, &operator_id);
        packets_received_mask |= PKT_OPERATOR_ID;
        last_operator_id_ms = now_ms;
        break;
    }
    default:
        // we don't care about other packets
        break;
    }
}

void MAVLinkSerial::arm_status_send(void)
{
    const uint32_t max_age_location_ms = 3000;
    const uint32_t max_age_other_ms = 22000;
    const uint32_t now_ms = millis();
    const char *reason = "";
    uint8_t status = MAV_ODID_PRE_ARM_FAIL_GENERIC;

    if (last_location_ms == 0 || now_ms - last_location_ms > max_age_location_ms) {
        reason = "missing location message";
    } else if (last_basic_id_ms == 0 || now_ms - last_basic_id_ms > max_age_other_ms) {
        reason = "missing basic_id message";
    } else if (last_self_id_ms == 0  || now_ms - last_self_id_ms > max_age_other_ms) {
        reason = "missing self_id message";
    } else if (last_operator_id_ms == 0 || now_ms - last_operator_id_ms > max_age_other_ms) {
        reason = "missing operator_id message";
    } else if (last_system_ms == 0 || now_ms - last_system_ms > max_age_other_ms) {
        reason = "missing system message";
    } else if (location.latitude == 0 && location.longitude == 0) {
        reason = "Bad location";
    } else if (system.operator_latitude == 0 && system.operator_longitude == 0) {
        reason = "Bad operator location";
    } else {
        status = MAV_ODID_GOOD_TO_ARM;
    }

    mavlink_msg_open_drone_id_arm_status_send(
        chan,
        status,
        reason);
}

/*
  return true when we have the base set of packets
 */
bool MAVLinkSerial::initialised(void)
{
    const uint32_t required = PKT_LOCATION | PKT_BASIC_ID | PKT_SELF_ID | PKT_SYSTEM | PKT_OPERATOR_ID;
    return (packets_received_mask & required) == required;
}

bool MAVLinkSerial::system_valid(void)
{
    uint32_t now_ms = millis();
    uint32_t max_ms = 15000;
    if (last_system_ms == 0 ||
        last_self_id_ms == 0 ||
        last_basic_id_ms == 0 ||
        last_operator_id_ms == 0) {
        return false;
    }
    return (now_ms - last_system_ms < max_ms &&
            now_ms - last_self_id_ms < max_ms &&
            now_ms - last_basic_id_ms < max_ms &&
            now_ms - last_operator_id_ms < max_ms);
}

bool MAVLinkSerial::location_valid(void)
{
    uint32_t now_ms = millis();
    uint32_t max_ms = 2000;
    return last_location_ms != 0 && now_ms - last_location_ms < max_ms;
}
