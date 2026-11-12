// An MQTT-C client connected to the test through a local socket pair (no network): the test
// plays the broker, writing packets to one end, and mqtt_sync() reads them from the other.
#pragma once

#include "libs/mqttc/mqtt.h"
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace test_mqtt
{
    class SocketPairClient
    {
    private:
        std::vector<uint8_t> sendbuf, recvbuf;

        // MQTT-C's publish callback: counts what arrives, and keeps the last topic and payload
        static void on_publish(void **state, struct mqtt_response_publish *published)
        {
            SocketPairClient *tthis = (SocketPairClient *)(*state);
            tthis->published++;
            tthis->last_topic = std::string((const char *)published->topic_name, published->topic_name_size);
            tthis->last_payload = std::string((const char *)published->application_message, published->application_message_size);
        }

    public:
        int client_fd = -1;
        int broker_fd = -1;
        bool ready = false;
        mqtt_client client;
        int published = 0;
        std::string last_topic, last_payload;

        // As MQTTClient sets it up: a non-blocking socket, then CONNECT queued (sent on the first mqtt_sync)
        explicit SocketPairClient(size_t bufsize = 1024) : sendbuf(bufsize), recvbuf(bufsize)
        {
            int fds[2];
            if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
                return;
            client_fd = fds[0];
            broker_fd = fds[1];
            if (fcntl(client_fd, F_SETFL, fcntl(client_fd, F_GETFL) | O_NONBLOCK) != 0)
                return;
            if (mqtt_init(&client, client_fd, sendbuf.data(), sendbuf.size(), recvbuf.data(), recvbuf.size(), on_publish) != MQTT_OK)
                return;
            client.publish_response_callback_state = this;
            ready = mqtt_connect(&client, "test", NULL, NULL, 0, NULL, NULL, MQTT_CONNECT_CLEAN_SESSION, 400) == MQTT_OK;
        }

        ~SocketPairClient()
        {
            close_broker();
            if (client_fd != -1)
                close(client_fd);
        }

        // Sends bytes as the broker; true when all were written
        bool broker_sends(const std::vector<uint8_t> &bytes)
        {
            return send(broker_fd, bytes.data(), bytes.size(), 0) == (ssize_t)bytes.size();
        }

        // The broker drops the connection
        void close_broker()
        {
            if (broker_fd != -1)
                close(broker_fd);
            broker_fd = -1;
        }
    };
} // namespace test_mqtt
