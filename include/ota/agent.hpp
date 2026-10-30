#pragma once

#include <atomic>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ota/alarm_engine.hpp"
#include "ota/bounded_queue.hpp"
#include "ota/latency_histogram.hpp"
#include "ota/transceiver.hpp"
#include "ota/types.hpp"

namespace ota {

struct Injection {
    std::uint32_t port = 0;
    FaultWindow window;
};

struct AgentConfig {
    std::uint32_t ports = 16;
    std::uint32_t pollers = 2;
    std::uint32_t workers = 4;
    double rate_hz = 10.0;              // samples per second per port; 0 = as fast as possible
    std::uint64_t samples_per_port = 0; // 0 = run until stop requested
    std::size_t queue_capacity = 1024;
    int soak_samples = 3;
    std::uint64_t seed = 42;
    std::string model = "QSFP28-100G-LR4";
    std::string alarm_log;              // "" = none, "-" = stdout, else JSON-lines file
    std::string telemetry_log;          // "" = none, else JSON-lines file
    std::vector<Injection> injections;
};

struct AgentStats {
    std::uint64_t samples_polled = 0;
    std::uint64_t samples_processed = 0;
    std::uint64_t alarms_raised = 0;
    std::uint64_t alarms_cleared = 0;
    std::size_t active_alarms = 0;
    std::size_t max_queue_depth = 0;
    double elapsed_s = 0.0;
    double throughput_sps = 0.0;        // samples processed per second
    double latency_p50_us = 0.0;        // sample capture -> alarm decision
    double latency_p99_us = 0.0;
    double latency_max_us = 0.0;
};

// Pipeline:
//   poller threads --(shard by port)--> per-worker BoundedQueue<Sample>
//   worker threads (own an AlarmEngine each) --> BoundedQueue<AlarmEvent>
//   publisher thread --> JSON-lines alarm log
//   (optional) recorder thread --> JSON-lines telemetry log
class Agent {
public:
    explicit Agent(AgentConfig config);
    ~Agent();

    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;

    void start();
    void request_stop();        // pollers stop producing; pipeline drains
    void join();                // drain every queue and join all threads
    bool pollers_finished() const { return pollers_running_.load() == 0; }

    AgentStats stats() const;   // latency figures are final after join()
    std::vector<AlarmEvent> active_alarms() const { return active_.snapshot(); }
    std::vector<AlarmEvent> alarm_history() const;
    const AgentConfig& config() const { return config_; }

private:
    void poller_loop(std::size_t index);
    void worker_loop(std::size_t index);
    void publisher_loop();
    void recorder_loop();
    double ms_since_start(Clock::time_point t) const;

    AgentConfig config_;
    std::vector<std::unique_ptr<SimulatedTransceiver>> transceivers_;
    std::vector<std::unique_ptr<BoundedQueue<Sample>>> worker_queues_;
    BoundedQueue<AlarmEvent> alarm_queue_;
    std::unique_ptr<BoundedQueue<Sample>> telemetry_queue_;
    ActiveAlarmTable active_;
    std::vector<LatencyHistogram> worker_latency_;

    std::atomic<bool> stop_{false};
    std::atomic<std::size_t> pollers_running_{0};
    std::atomic<std::uint64_t> polled_{0};
    std::atomic<std::uint64_t> processed_{0};
    std::atomic<std::uint64_t> raised_{0};
    std::atomic<std::uint64_t> cleared_{0};

    std::vector<std::thread> pollers_;
    std::vector<std::thread> workers_;
    std::thread publisher_;
    std::thread recorder_;

    mutable std::mutex history_mutex_;
    std::vector<AlarmEvent> history_;

    std::ofstream alarm_file_;
    std::ofstream telemetry_file_;
    Clock::time_point started_at_{};
    Clock::time_point finished_at_{};
    bool started_ = false;
    bool joined_ = false;
};

}  // namespace ota
