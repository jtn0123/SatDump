// The bundled MQTT-C library (src-core/libs/mqttc), with no network: packets as a broker sends
// them are unpacked from byte buffers, and one test feeds mqtt_sync() through a local socket
// pair. Malformed packets must be refused, above all a PUBLISH whose topic length runs past
// the packet (CVE-2026-54412, fixed in MQTT-C 1.1.7). The other 1.1.7 fixes that work without
// a connection are checked too. Every packet is copied into a heap block of exactly its size,
// so the sanitizer build reports any read past its end.
#include "doctest.h"
#include "libs/mqttc/mqtt.h"
#include "mqtt_socket_pair.h"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{
    typedef std::vector<uint8_t> Bytes;

    // mqtt_unpack_response on an exact-size heap copy of the packet bytes. The decoded
    // response points into that copy, so both are kept together.
    struct Unpacked
    {
        std::unique_ptr<uint8_t[]> data;
        size_t size;
        mqtt_response response{};
        ssize_t rv;

        explicit Unpacked(const Bytes &bytes) : data(new uint8_t[bytes.size()]), size(bytes.size())
        {
            if (size > 0)
                std::memcpy(data.get(), bytes.data(), size);
            rv = mqtt_unpack_response(&response, data.get(), size);
        }

        const mqtt_response_publish &publish() const { return response.decoded.publish; }

        std::string topic() const
        {
            return std::string((const char *)publish().topic_name, publish().topic_name_size);
        }

        Bytes payload() const
        {
            const uint8_t *p = (const uint8_t *)publish().application_message;
            return Bytes(p, p + publish().application_message_size);
        }
    };

    Bytes text(const char *s) { return Bytes(s, s + std::strlen(s)); }

    struct Malformed
    {
        std::string what;
        Bytes bytes;
        ssize_t expected;
    };
} // namespace

TEST_CASE("mqtt: a well-formed QoS 0 PUBLISH gives its topic and payload")
{
    // Topic "a/b/c", payload "hello": remaining length 2 + 5 + 5 = 12
    Unpacked u({0x30, 0x0C, 0x00, 0x05, 'a', '/', 'b', '/', 'c', 'h', 'e', 'l', 'l', 'o'});
    REQUIRE(u.rv == 14);
    CHECK(u.response.fixed_header.control_type == MQTT_CONTROL_PUBLISH);
    CHECK(u.response.fixed_header.remaining_length == 12);
    CHECK(u.publish().qos_level == 0);
    CHECK(u.publish().dup_flag == 0);
    CHECK(u.publish().retain_flag == 0);
    CHECK(u.topic() == "a/b/c");
    CHECK(u.payload() == text("hello"));
}

TEST_CASE("mqtt: a well-formed QoS 1 PUBLISH gives its flags and packet id as well as topic and payload")
{
    // DUP, QoS 1 and RETAIN set; topic "a/b", packet id 0x1234, payload "xyz"
    Unpacked u({0x3B, 0x0A, 0x00, 0x03, 'a', '/', 'b', 0x12, 0x34, 'x', 'y', 'z'});
    REQUIRE(u.rv == 12);
    CHECK(u.publish().dup_flag == 1);
    CHECK(u.publish().qos_level == 1);
    CHECK(u.publish().retain_flag == 1);
    CHECK(u.publish().packet_id == 0x1234);
    CHECK(u.topic() == "a/b");
    CHECK(u.payload() == text("xyz"));
}

TEST_CASE("mqtt: a PUBLISH whose topic fills the packet exactly is accepted")
{
    SUBCASE("QoS 0, empty payload")
    {
        Unpacked u({0x30, 0x04, 0x00, 0x02, 'a', 'b'});
        REQUIRE(u.rv == 6);
        CHECK(u.topic() == "ab");
        CHECK(u.publish().application_message_size == 0);
    }
    SUBCASE("QoS 1, packet id and empty payload")
    {
        Unpacked u({0x32, 0x05, 0x00, 0x01, 'a', 0x00, 0x07});
        REQUIRE(u.rv == 7);
        CHECK(u.topic() == "a");
        CHECK(u.publish().packet_id == 7);
        CHECK(u.publish().application_message_size == 0);
    }
}

TEST_CASE("mqtt: only the first packet in the buffer is unpacked")
{
    // The receive buffer can hold a PUBLISH followed by the start of the next packet
    Unpacked u({0x30, 0x05, 0x00, 0x01, 't', 'x', 'y', 0xD0, 0x00, 0x30});
    REQUIRE(u.rv == 7);
    CHECK(u.topic() == "t");
    CHECK(u.payload() == text("xy"));
}

TEST_CASE("mqtt: a packed PUBLISH unpacks to the same message")
{
    // As GGAKToMQTTModule sends them: QoS 1, with a payload long enough for a two-byte
    // remaining length
    const std::string topic = "ggak/l2/all";
    Bytes payload(300);
    for (size_t i = 0; i < payload.size(); i++)
        payload[i] = uint8_t(i * 7 + 3);

    Bytes buf(512);
    ssize_t n = mqtt_pack_publish_request(buf.data(), buf.size(), topic.c_str(), 0x4321, payload.data(), payload.size(),
                                          uint8_t(MQTT_PUBLISH_QOS_1 | MQTT_PUBLISH_RETAIN));
    REQUIRE(n == ssize_t(1 + 2 + 2 + topic.size() + 2 + payload.size()));
    buf.resize(size_t(n));

    Unpacked u(buf);
    REQUIRE(u.rv == n);
    CHECK(u.response.fixed_header.control_type == MQTT_CONTROL_PUBLISH);
    CHECK(u.publish().qos_level == 1);
    CHECK(u.publish().retain_flag == 1);
    CHECK(u.publish().packet_id == 0x4321);
    CHECK(u.topic() == topic);
    CHECK(u.payload() == payload);
}

TEST_CASE("mqtt: a PUBLISH whose topic length runs past the packet is refused (CVE-2026-54412)")
{
    // Before MQTT-C 1.1.7 the topic length was not checked against the remaining length: the
    // parser read the QoS 1 packet id past the end of the packet, and the payload size
    // underflowed to about 4 GB, which the receive loop then used to move memory
    const std::vector<Malformed> cases = {
        {"QoS 0, topic length 65535 in a 4-byte packet", {0x30, 0x04, 0xFF, 0xFF, 'a', 'b'}, MQTT_ERROR_MALFORMED_RESPONSE},
        {"QoS 0, topic one byte longer than the packet", {0x30, 0x06, 0x00, 0x05, 'a', 'b', 'c', 'd'}, MQTT_ERROR_MALFORMED_RESPONSE},
        {"QoS 1, topic longer than the packet", {0x32, 0x04, 0x00, 0x03, 'a', 'b'}, MQTT_ERROR_MALFORMED_RESPONSE},
        {"QoS 1, topic fills the packet, no room for the packet id", {0x32, 0x05, 0x00, 0x03, 'a', 'b', 'c'}, MQTT_ERROR_MALFORMED_RESPONSE},
        {"QoS 2, topic length 65535", {0x34, 0x06, 0xFF, 0xFF, 'a', 'b', 'c', 'd'}, MQTT_ERROR_MALFORMED_RESPONSE},
    };
    for (const Malformed &c : cases)
    {
        INFO(c.what);
        Unpacked u(c.bytes);
        CHECK(u.rv == c.expected);
    }
}

TEST_CASE("mqtt: the receive loop refuses that PUBLISH on every later call and calls no handler for it")
{
    // The whole receive path of CVE-2026-54412: before 1.1.7, mqtt_sync() passed a payload of
    // about 4 GB to the publish callback and then moved memory with a wrapped size
    test_mqtt::SocketPairClient s;
    REQUIRE(s.ready);
    REQUIRE(mqtt_sync(&s.client) == MQTT_OK); // sends CONNECT

    // CONNACK (accepted), then a good QoS 0 PUBLISH
    REQUIRE(s.broker_sends({0x20, 0x02, 0x00, 0x00, 0x30, 0x05, 0x00, 0x01, 't', 'o', 'k'}));
    REQUIRE(mqtt_sync(&s.client) == MQTT_OK);
    CHECK(s.published == 1);
    CHECK(s.last_topic == "t");
    CHECK(s.last_payload == "ok");

    // QoS 0, topic length 65535 in a 4-byte packet. The bytes stay in the receive buffer, so
    // every later call fails the same way (MQTTClient's refresh thread calls it every 100 us)
    REQUIRE(s.broker_sends({0x30, 0x04, 0xFF, 0xFF, 'a', 'b'}));
    int refused = 0;
    for (int i = 0; i < 100; i++)
        if (mqtt_sync(&s.client) == MQTT_ERROR_MALFORMED_RESPONSE)
            refused++;
    CHECK(refused == 100);
    CHECK(s.client.error == MQTT_ERROR_MALFORMED_RESPONSE);
    CHECK(s.published == 1);

    // The broker then drops the connection
    s.close_broker();
    CHECK(mqtt_sync(&s.client) == MQTT_ERROR_SOCKET_ERROR);
    CHECK(s.published == 1);
}

TEST_CASE("mqtt: a truncated fixed header is not taken as a packet")
{
    // 0 means "not a whole packet yet": nothing is consumed and the client waits for more bytes
    const std::vector<Malformed> cases = {
        {"empty buffer", {}, 0},
        {"type byte only", {0x30}, 0},
        {"length byte with the continuation bit, then nothing", {0x30, 0x80}, 0},
        {"three length bytes, all continued", {0x30, 0xFF, 0xFF, 0xFF}, 0},
        {"body shorter than the remaining length", {0x30, 0x0A, 0x00, 0x01, 'a'}, 0},
        {"largest legal remaining length, body missing", {0x30, 0xFF, 0xFF, 0xFF, 0x7F}, 0},
    };
    for (const Malformed &c : cases)
    {
        INFO(c.what);
        Unpacked u(c.bytes);
        CHECK(u.rv == c.expected);
    }
}

TEST_CASE("mqtt: a remaining length longer than four bytes is refused")
{
    // MQTT 3.1.1 section 2.2.3: at most four length bytes (28 bits)
    const std::vector<Malformed> cases = {
        {"five length bytes, 0xFF", {0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0x01}, MQTT_ERROR_INVALID_REMAINING_LENGTH},
        {"five length bytes, 0x80", {0x30, 0x80, 0x80, 0x80, 0x80, 0x00}, MQTT_ERROR_INVALID_REMAINING_LENGTH},
        {"fifth length byte not yet received", {0x30, 0x80, 0x80, 0x80, 0x80}, MQTT_ERROR_INVALID_REMAINING_LENGTH},
    };
    for (const Malformed &c : cases)
    {
        INFO(c.what);
        Unpacked u(c.bytes);
        CHECK(u.rv == c.expected);
    }
}

TEST_CASE("mqtt: other malformed packets are refused")
{
    const std::vector<Malformed> cases = {
        {"reserved type 0", {0x00, 0x00}, MQTT_ERROR_CONTROL_FORBIDDEN_TYPE},
        {"reserved type 15", {0xF0, 0x00}, MQTT_ERROR_CONTROL_FORBIDDEN_TYPE},
        {"PUBREL without its required flags", {0x60, 0x02, 0x00, 0x01}, MQTT_ERROR_CONTROL_INVALID_FLAGS},
        {"PUBLISH shorter than 4 bytes", {0x30, 0x03, 0x00, 0x01, 'a'}, MQTT_ERROR_MALFORMED_RESPONSE},
        {"PUBACK of 3 bytes", {0x40, 0x03, 0x00, 0x01, 0x02}, MQTT_ERROR_MALFORMED_RESPONSE},
        {"SUBACK without a return code", {0x90, 0x02, 0x00, 0x01}, MQTT_ERROR_MALFORMED_RESPONSE},
        {"CONNACK of 3 bytes", {0x20, 0x03, 0x00, 0x00, 0x00}, MQTT_ERROR_MALFORMED_RESPONSE},
        {"CONNACK with an unknown return code", {0x20, 0x02, 0x00, 0x06}, MQTT_ERROR_CONNACK_FORBIDDEN_CODE},
        {"CONNECT, which only a client sends", {0x10, 0x00}, MQTT_ERROR_RESPONSE_INVALID_CONTROL_TYPE},
    };
    for (const Malformed &c : cases)
    {
        INFO(c.what);
        Unpacked u(c.bytes);
        CHECK(u.rv == c.expected);
    }
}

TEST_CASE("mqtt: every error code has a message (an unknown one included)")
{
    // Before 1.1.7, a negative value that is not an error code indexed past the end of the
    // message table, and a positive one overflowed a signed subtraction
    CHECK(std::string(mqtt_error_str(MQTT_ERROR_MALFORMED_RESPONSE)) == "MQTT_ERROR_MALFORMED_RESPONSE");
    CHECK(std::string(mqtt_error_str(MQTT_ERROR_RECONNECTING)) == "MQTT_ERROR_RECONNECTING");
    CHECK(std::string(mqtt_error_str(MQTT_ERROR_UNKNOWN)) == "MQTT_UNKNOWN_ERROR");
    CHECK(std::string(mqtt_error_str(static_cast<MQTTErrors>(0))) == "MQTT_ERROR: Buffer too small.");
    CHECK(std::string(mqtt_error_str(static_cast<MQTTErrors>(MQTT_ERROR_RECONNECTING + 1))) == "MQTT_UNKNOWN_ERROR");
    CHECK(std::string(mqtt_error_str(MQTT_OK)) == "MQTT_OK");
    CHECK(std::string(mqtt_error_str(static_cast<MQTTErrors>(-1))) == "MQTT_UNKNOWN_ERROR");
}

TEST_CASE("mqtt: the send queue keeps its entries aligned whatever the buffer")
{
    // The queue's bookkeeping entries grow down from mem_end; before 1.1.7 an odd buffer
    // address or size left them misaligned
    const size_t align = alignof(mqtt_queued_message);
    alignas(alignof(std::max_align_t)) uint8_t storage[512];
    for (size_t offset = 0; offset < 16; offset++)
    {
        for (size_t size : {size_t(64), size_t(101), size_t(255)})
        {
            INFO("offset " << offset << ", size " << size);
            uint8_t *start = storage + offset;
            mqtt_message_queue mq;
            mqtt_mq_init(&mq, start, size);
            uint8_t *end = (uint8_t *)mq.mem_end;
            CHECK((uintptr_t)end % align == 0);
            CHECK(end <= start + size);
            CHECK(size_t(start + size - end) < align);
            CHECK(mq.curr == start);
            CHECK(mq.queue_tail == (mqtt_queued_message *)mq.mem_end);
        }
    }
}

TEST_CASE("mqtt: MQTTClient's send buffers are used whole as before 1.1.7")
{
    // MQTTClient mallocs its buffers (aligned for any type) with 1024 or 1000000 bytes: the
    // alignment fix takes nothing from them
    for (size_t size : {size_t(1024), size_t(1000000)})
    {
        INFO("size " << size);
        uint8_t *buf = (uint8_t *)std::malloc(size);
        REQUIRE(buf != nullptr);
        mqtt_message_queue mq;
        mqtt_mq_init(&mq, buf, size);
        CHECK((uint8_t *)mq.mem_end == buf + size);
        CHECK(mq.curr_sz == size - sizeof(mqtt_queued_message));
        std::free(buf);
    }
}

TEST_CASE("mqtt: reinit starts sending from the start of the new buffer")
{
    Bytes send1(256), recv1(256), send2(256), recv2(256);
    mqtt_client client;
    REQUIRE(mqtt_init(&client, -1, send1.data(), send1.size(), recv1.data(), recv1.size(), nullptr) == MQTT_OK);
    // As if the connection dropped part-way through sending a message
    client.send_offset = 7;
    mqtt_reinit(&client, -1, send2.data(), send2.size(), recv2.data(), recv2.size());
    CHECK(client.send_offset == 0);
    CHECK(client.mq.curr == send2.data());
    CHECK(client.recv_buffer.mem_start == recv2.data());
    // mqtt_init leaves the client locked until CONNECT
    MQTT_PAL_MUTEX_UNLOCK(&client.mutex);
}
