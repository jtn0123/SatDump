// MQTTClient (src-core/utils/mqtt_client.cpp), with no network. Its refresh thread used to drop
// what mqtt_sync() returned, so a lost connection or a bad packet from the broker stopped every
// MQTT message without a log line. Each new error is now logged once (the thread retries every
// 100 us and the error stays), and so is a return to working.
#include "doctest.h"
#include "log_capture.h"
#include "mqtt_socket_pair.h"
#include "utils/mqtt_client.h"
#include <vector>

using satdump::MQTTClient;
using test_log::LogCapture;

TEST_CASE("mqtt client: each new mqtt_sync() error is logged once and so is the return to working")
{
    LogCapture c;
    const std::vector<MQTTErrors> results = {
        MQTT_OK,
        MQTT_OK,
        MQTT_ERROR_MALFORMED_RESPONSE,
        MQTT_ERROR_MALFORMED_RESPONSE,
        MQTT_ERROR_MALFORMED_RESPONSE,
        MQTT_ERROR_SOCKET_ERROR,
        MQTT_ERROR_SOCKET_ERROR,
        MQTT_OK,
        MQTT_OK,
        MQTT_ERROR_MALFORMED_RESPONSE,
    };
    MQTTErrors last = MQTT_OK;
    for (MQTTErrors result : results)
        last = MQTTClient::log_sync_result(result, last);
    CHECK(last == MQTT_ERROR_MALFORMED_RESPONSE);

    REQUIRE(c.count() == 4);
    CHECK(c.sink->msgs[0].lvl == slog::LOG_ERROR);
    CHECK(c.text(0) == "MQTT connection failed: MQTT_ERROR_MALFORMED_RESPONSE");
    CHECK(c.sink->msgs[1].lvl == slog::LOG_ERROR);
    CHECK(c.text(1) == "MQTT connection failed: MQTT_ERROR_SOCKET_ERROR");
    CHECK(c.sink->msgs[2].lvl == slog::LOG_INFO);
    CHECK(c.text(2) == "MQTT connection working again");
    CHECK(c.sink->msgs[3].lvl == slog::LOG_ERROR);
    CHECK(c.text(3) == "MQTT connection failed: MQTT_ERROR_MALFORMED_RESPONSE");
}

TEST_CASE("mqtt client: a working connection logs nothing")
{
    LogCapture c;
    MQTTErrors last = MQTT_OK;
    for (int i = 0; i < 1000; i++)
        last = MQTTClient::log_sync_result(MQTT_OK, last);
    CHECK(last == MQTT_OK);
    CHECK(c.count() == 0);
}

TEST_CASE("mqtt client: a bad packet from the broker and then a lost connection give one log line each")
{
    // The refresh thread's loop with real mqtt_sync() results
    test_mqtt::SocketPairClient s;
    REQUIRE(s.ready);
    LogCapture c;
    MQTTErrors last = MQTT_OK;
    auto refresh = [&](int times)
    {
        for (int i = 0; i < times; i++)
            last = MQTTClient::log_sync_result(mqtt_sync(&s.client), last);
    };

    // CONNECT out; CONNACK (accepted) and a good QoS 0 PUBLISH in
    refresh(10);
    REQUIRE(s.broker_sends({0x20, 0x02, 0x00, 0x00, 0x30, 0x05, 0x00, 0x01, 't', 'o', 'k'}));
    refresh(10);
    CHECK(s.published == 1);
    CHECK(c.count() == 0);

    // A PUBLISH whose topic length runs past the packet (CVE-2026-54412)
    REQUIRE(s.broker_sends({0x30, 0x04, 0xFF, 0xFF, 'a', 'b'}));
    refresh(1000);
    REQUIRE(c.count() == 1);
    CHECK(c.sink->msgs[0].lvl == slog::LOG_ERROR);
    CHECK(c.text(0) == "MQTT connection failed: MQTT_ERROR_MALFORMED_RESPONSE");

    // The broker drops the connection
    s.close_broker();
    refresh(1000);
    REQUIRE(c.count() == 2);
    CHECK(c.sink->msgs[1].lvl == slog::LOG_ERROR);
    CHECK(c.text(1) == "MQTT connection failed: MQTT_ERROR_SOCKET_ERROR");
    CHECK(s.published == 1);
}
