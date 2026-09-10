#ifndef MQTT_PROTOCOL_H
#define MQTT_PROTOCOL_H


#include "protocol.h"
#include <mqtt.h>
#include <udp.h>
#include <cJSON.h>
#include <mbedtls/aes.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include <functional>
#include <string>
#include <map>
#include <mutex>
#include <vector>

#define MQTT_PING_INTERVAL_SECONDS 90
#define MQTT_RECONNECT_INTERVAL_MS 60000

#define MQTT_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

class MqttProtocol : public Protocol {
public:
    MqttProtocol();
    ~MqttProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel() override;
    bool IsAudioChannelOpened() const override;

private:
    EventGroupHandle_t event_group_handle_;

    std::string publish_topic_;

    mutable std::mutex channel_mutex_;
    std::unique_ptr<Mqtt> mqtt_;
    std::unique_ptr<Udp> udp_;
    mbedtls_aes_context aes_ctx_;
    std::string aes_nonce_;
    std::string udp_server_;
    int udp_port_;
    uint32_t local_sequence_;
    uint32_t remote_sequence_;
    static constexpr size_t kAudioReorderWindowPackets = 3;
    std::mutex audio_reorder_mutex_;
    std::mutex audio_delivery_mutex_;
    std::map<uint32_t, std::unique_ptr<AudioStreamPacket>> remote_audio_reorder_;
    uint32_t audio_reordered_packets_ = 0;
    uint32_t audio_skipped_packets_ = 0;
    uint32_t audio_duplicate_packets_ = 0;
    uint32_t audio_delivered_packets_ = 0;
    esp_timer_handle_t reconnect_timer_;

    bool StartMqttClient(bool report_error=false);
    void ParseServerHello(const cJSON* root);
    std::string DecodeHexString(const std::string& hex_string);
    bool SendAudioLocked(const AudioStreamPacket& packet);
    void HandleIncomingAudioPacket(uint32_t sequence, std::unique_ptr<AudioStreamPacket> packet);
    void DrainAudioReorderLocked(std::vector<std::unique_ptr<AudioStreamPacket>>& ready, bool force);
    void FlushAudioReorderBuffer();
    void ResetAudioReorderState();
    void DeliverIncomingAudioPackets(std::vector<std::unique_ptr<AudioStreamPacket>>&& packets);

    bool SendText(const std::string& text) override;
    std::string GetHelloMessage();
};


#endif // MQTT_PROTOCOL_H
