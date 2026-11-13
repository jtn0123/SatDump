#include "module_network_client.h"
#include "common/net/udp.h"
#include "common/utils.h"
#include "core/exception.h"
#include "imgui/imgui.h"
#include "logger.h"
#include "pipeline/module.h"
#include <nng/nng.h>
#include <nng/protocol/pubsub0/sub.h>

namespace satdump
{
    namespace pipeline
    {
        namespace network
        {
            NetworkClientModule::NetworkClientModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters) : ProcessingModule(input_file, output_file_hint, parameters)
            {
                if (parameters.contains("client_mode"))
                    mode = parameters["client_mode"].get<std::string>();

                if (mode == "default")
                {
                    if (parameters.count("pkt_size") > 0)
                        pkt_size = parameters["pkt_size"].get<int>();
                    else
                        throw satdump_exception("pkt_size parameter must be present!");

                    if (parameters.count("server_address") > 0)
                        address = parameters["server_address"].get<std::string>();
                    else
                        throw satdump_exception("server_address parameter must be present!");
                }

                if (parameters.count("server_port") > 0)
                    port = parameters["server_port"].get<int>();
                else
                    throw satdump_exception("server_port parameter must be present!");

                buffer = new uint8_t[pkt_size * 10];
            }

            std::vector<ModuleDataType> NetworkClientModule::getInputTypes() { return {DATA_FILE, DATA_STREAM}; }

            std::vector<ModuleDataType> NetworkClientModule::getOutputTypes() { return {DATA_FILE, DATA_STREAM}; }

            NetworkClientModule::~NetworkClientModule() { delete[] buffer; }

            void NetworkClientModule::process()
            {
                if (mode == "default")
                {
                    nng_socket sock;
                    const std::string url = "tcp://" + address + ":" + std::to_string(port);
                    logger->info("Opening TCP socket on %s", url.c_str());

                    // Fork: only this worker owns the socket. Closing it here also closes its
                    // dialer, on normal stop and on every error, without racing stop().
                    int rv = nng_sub0_open_raw(&sock);
                    if (rv != 0)
                        throw satdump_exception(std::string("Cannot open network client: ") + nng_strerror(rv));
                    struct SocketCloser
                    {
                        nng_socket sock;
                        ~SocketCloser() { nng_close(sock); }
                    } close_socket{sock};

                    nng_dialer dialer;
                    // Fork: connection setup must not wait for a server's handshake, and a
                    // receive must periodically check the existing atomic input_active flag.
                    // Otherwise stopping the pipeline could wait forever in either call.
                    // Fork: the raw subscriber defaults to one queued message. A short burst
                    // can arrive on the I/O threads before this worker is scheduled again,
                    // dropping valid packets even with no downstream backpressure.
                    if ((rv = nng_socket_set_int(sock, NNG_OPT_RECVBUF, 64)) != 0 ||
                        (rv = nng_socket_set_ms(sock, NNG_OPT_RECVTIMEO, 100)) != 0 ||
                        (rv = nng_dialer_create(&dialer, sock, url.c_str())) != 0 ||
                        (rv = nng_dialer_start(dialer, NNG_FLAG_NONBLOCK)) != 0)
                        throw satdump_exception(std::string("Cannot start network client: ") + nng_strerror(rv));

                    while (input_active.load())
                    {
                        // Fork: nng_recv takes the buffer capacity as input. It was uninitialized,
                        // and errors were ignored before the returned size was read.
                        size_t lpkt_size = static_cast<size_t>(pkt_size);
                        rv = nng_recv(sock, buffer, &lpkt_size, 0);
                        if (rv == NNG_ETIMEDOUT || rv == NNG_EMSGSIZE)
                            continue;
                        if (rv != 0)
                            throw satdump_exception(std::string("Network client receive failed: ") + nng_strerror(rv));
                        if (!input_active.load())
                            break;
                        if (lpkt_size != static_cast<size_t>(pkt_size))
                            continue;

                        output_fifo->write(buffer, pkt_size);
                    }
                }
                else if (mode == "udp_listen")
                {
                    net::UDPServer udp_server(port);

                    while (input_active.load())
                    {
                        int lpkt_size = udp_server.recv(buffer, pkt_size);
                        output_fifo->write(buffer, lpkt_size);
                    }
                }
            }

            void NetworkClientModule::drawUI(bool window)
            {
                ImGui::Begin("Network Client", NULL, window ? 0 : NOWINDOW_FLAGS);

                ImGui::Text("Server Address  : ");
                ImGui::SameLine();
                ImGui::TextColored(style::theme.green, "%s", address.c_str());

                ImGui::Text("Server Port    : ");
                ImGui::SameLine();
                ImGui::TextColored(style::theme.green, UITO_C_STR(port));

                ImGui::End();
            }

            std::string NetworkClientModule::getID() { return "network_client"; }

            std::shared_ptr<ProcessingModule> NetworkClientModule::getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
            {
                return std::make_shared<NetworkClientModule>(input_file, output_file_hint, parameters);
            }
        } // namespace network
    } // namespace pipeline
} // namespace satdump
