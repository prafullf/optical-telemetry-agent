#include "ota/agent.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace ota {

namespace {

constexpr std::size_t kBatchSize = 256;

void write_alarm_json(std::ostream& os, const AlarmEvent& e, double t_ms) {
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "{\"t_ms\":%.3f,\"seq\":%llu,\"port\":%u,\"alarm\":\"%s\",\"severity\":\"%s\","
                  "\"state\":\"%s\",\"masked\":%s,\"value\":%.6g}\n",
                  t_ms, static_cast<unsigned long long>(e.seq), e.port, to_string(e.type),
                  to_string(e.severity), e.raised ? "RAISED" : "CLEARED",
                  e.masked ? "true" : "false", e.value);
    os << buf;
}

void write_sample_json(std::ostream& os, const Sample& s, double t_ms) {
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "{\"t_ms\":%.3f,\"seq\":%llu,\"port\":%u,\"rx_dbm\":%.4f,\"tx_dbm\":%.4f,"
                  "\"osnr_db\":%.4f,\"pre_fec_ber\":%.6g,\"temp_c\":%.4f}\n",
                  t_ms, static_cast<unsigned long long>(s.seq), s.port, s.rx_dbm, s.tx_dbm,
                  s.osnr_db, s.pre_fec_ber, s.temp_c);
    os << buf;
}

}  // namespace

Agent::Agent(AgentConfig config)
    : config_(std::move(config)), alarm_queue_(std::max<std::size_t>(config_.queue_capacity, 1)) {
    if (config_.ports == 0) throw std::invalid_argument("ports must be > 0");
    if (config_.queue_capacity == 0) throw std::invalid_argument("queue capacity must be > 0");
    config_.workers = std::max<std::uint32_t>(config_.workers, 1);
    config_.pollers = std::clamp<std::uint32_t>(config_.pollers, 1, config_.ports);

    for (std::uint32_t p = 0; p < config_.ports; ++p) {
        transceivers_.push_back(std::make_unique<SimulatedTransceiver>(p, config_.model, config_.seed));
    }
    for (const auto& inj : config_.injections) {
        if (inj.port >= config_.ports) {
            throw std::invalid_argument("fault injection targets port " + std::to_string(inj.port) +
                                        " but only " + std::to_string(config_.ports) + " ports exist");
        }
        transceivers_[inj.port]->inject(inj.window);
    }
    for (std::uint32_t w = 0; w < config_.workers; ++w) {
        worker_queues_.push_back(std::make_unique<BoundedQueue<Sample>>(config_.queue_capacity));
    }
    worker_latency_.resize(config_.workers);
    if (!config_.telemetry_log.empty()) {
        telemetry_queue_ = std::make_unique<BoundedQueue<Sample>>(config_.queue_capacity);
    }
}

Agent::~Agent() { join(); }

void Agent::start() {
    if (started_) throw std::logic_error("agent already started");
    if (!config_.alarm_log.empty() && config_.alarm_log != "-") {
        alarm_file_.open(config_.alarm_log, std::ios::out | std::ios::trunc);
        if (!alarm_file_) throw std::runtime_error("cannot open alarm log: " + config_.alarm_log);
    }
    if (telemetry_queue_) {
        telemetry_file_.open(config_.telemetry_log, std::ios::out | std::ios::trunc);
        if (!telemetry_file_) throw std::runtime_error("cannot open telemetry log: " + config_.telemetry_log);
    }

    started_ = true;
    started_at_ = Clock::now();

    // Start consumers before producers.
    publisher_ = std::thread(&Agent::publisher_loop, this);
    if (telemetry_queue_) recorder_ = std::thread(&Agent::recorder_loop, this);
    for (std::size_t w = 0; w < config_.workers; ++w) workers_.emplace_back(&Agent::worker_loop, this, w);
    pollers_running_.store(config_.pollers);
    for (std::size_t p = 0; p < config_.pollers; ++p) pollers_.emplace_back(&Agent::poller_loop, this, p);
}

void Agent::request_stop() { stop_.store(true, std::memory_order_relaxed); }

void Agent::join() {
    if (!started_ || joined_) return;
    request_stop();

    // Ordered shutdown: stop producers, then drain each stage before closing
    // the next one, so no sample or alarm already in flight is lost.
    for (auto& t : pollers_) t.join();
    for (auto& q : worker_queues_) q->close();
    for (auto& t : workers_) t.join();
    finished_at_ = Clock::now();

    alarm_queue_.close();
    if (telemetry_queue_) telemetry_queue_->close();
    publisher_.join();
    if (recorder_.joinable()) recorder_.join();

    if (alarm_file_.is_open()) alarm_file_.flush();
    if (telemetry_file_.is_open()) telemetry_file_.flush();
    if (config_.alarm_log == "-") std::cout.flush();
    joined_ = true;
}

double Agent::ms_since_start(Clock::time_point t) const {
    return std::chrono::duration<double, std::milli>(t - started_at_).count();
}

void Agent::poller_loop(std::size_t index) {
    std::vector<SimulatedTransceiver*> mine;
    for (std::size_t p = index; p < transceivers_.size(); p += config_.pollers) {
        mine.push_back(transceivers_[p].get());
    }

    const bool paced = config_.rate_hz > 0.0;
    const auto period = paced ? std::chrono::duration_cast<Clock::duration>(
                                    std::chrono::duration<double>(1.0 / config_.rate_hz))
                              : Clock::duration::zero();
    auto next_tick = Clock::now();

    for (std::uint64_t round = 0; !stop_.load(std::memory_order_relaxed); ++round) {
        if (config_.samples_per_port != 0 && round >= config_.samples_per_port) break;
        for (SimulatedTransceiver* t : mine) {
            Sample s = t->read();
            if (telemetry_queue_) telemetry_queue_->push(s);
            worker_queues_[s.port % worker_queues_.size()]->push(s);
            polled_.fetch_add(1, std::memory_order_relaxed);
        }
        if (paced) {
            next_tick += period;  // fixed cadence, no drift from processing time
            std::this_thread::sleep_until(next_tick);
        }
    }
    pollers_running_.fetch_sub(1);
}

void Agent::worker_loop(std::size_t index) {
    AlarmProfile profile = AlarmProfile::default_profile();
    profile.soak_samples = config_.soak_samples;
    AlarmEngine engine(profile);  // owned by this thread only

    BoundedQueue<Sample>& queue = *worker_queues_[index];
    LatencyHistogram& latency = worker_latency_[index];
    std::vector<Sample> batch;
    batch.reserve(kBatchSize);
    std::vector<AlarmEvent> events;

    while (queue.pop_batch(batch, kBatchSize) > 0) {
        for (const Sample& s : batch) {
            events.clear();
            engine.evaluate(s, events);
            latency.record(Clock::now() - s.captured);
            for (const AlarmEvent& e : events) {
                active_.apply(e);
                (e.raised ? raised_ : cleared_).fetch_add(1, std::memory_order_relaxed);
                alarm_queue_.push(e);
            }
        }
        processed_.fetch_add(batch.size(), std::memory_order_relaxed);
    }
}

void Agent::publisher_loop() {
    std::ostream* out = nullptr;
    if (config_.alarm_log == "-") out = &std::cout;
    else if (alarm_file_.is_open()) out = &alarm_file_;

    while (auto e = alarm_queue_.pop()) {
        {
            std::lock_guard<std::mutex> lock(history_mutex_);
            history_.push_back(*e);
        }
        if (out) write_alarm_json(*out, *e, ms_since_start(e->when));
    }
}

void Agent::recorder_loop() {
    while (auto s = telemetry_queue_->pop()) {
        write_sample_json(telemetry_file_, *s, ms_since_start(s->captured));
    }
}

std::vector<AlarmEvent> Agent::alarm_history() const {
    std::lock_guard<std::mutex> lock(history_mutex_);
    return history_;
}

AgentStats Agent::stats() const {
    AgentStats st;
    st.samples_polled = polled_.load();
    st.samples_processed = processed_.load();
    st.alarms_raised = raised_.load();
    st.alarms_cleared = cleared_.load();
    st.active_alarms = active_.size();
    for (const auto& q : worker_queues_) st.max_queue_depth = std::max(st.max_queue_depth, q->high_watermark());

    if (started_) {
        const auto end = joined_ ? finished_at_ : Clock::now();
        st.elapsed_s = std::chrono::duration<double>(end - started_at_).count();
        if (st.elapsed_s > 0.0) st.throughput_sps = static_cast<double>(st.samples_processed) / st.elapsed_s;
    }
    if (joined_) {
        LatencyHistogram merged;
        for (const auto& h : worker_latency_) merged.merge(h);
        st.latency_p50_us = merged.percentile_us(0.50);
        st.latency_p99_us = merged.percentile_us(0.99);
        st.latency_max_us = merged.max_us();
    }
    return st;
}

}  // namespace ota
