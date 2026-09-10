#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mqtt.h>
#include <memory>

class ReminderPoller;

class ReminderMqttWake {
public:
    ReminderMqttWake();
    ~ReminderMqttWake();

    void Start(ReminderPoller* poller);
    void Stop();

    static void EnsureNvsConfigured();

private:
    ReminderPoller* poller_ = nullptr;
    bool running_ = false;
    TaskHandle_t task_handle_ = nullptr;
    std::unique_ptr<Mqtt> mqtt_;

    void MqttTask();
    bool ConnectOnce();
    static std::string ExpandDeviceIdInTopic(const std::string& topic_template);
};
