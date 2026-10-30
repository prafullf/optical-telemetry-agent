// ota-agent: command-line entry point for the optical transceiver telemetry
// and alarm agent.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include "ota/agent.hpp"

namespace {

std::atomic<bool> g_stop{false};
std::atomic<bool> g_dump{false};
static_assert(std::atomic<bool>::is_always_lock_free, "signal handlers need lock-free atomics");

extern "C" void on_signal(int sig) {
    if (sig == SIGUSR1) g_dump.store(true);
    else g_stop.store(true);
}

void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGUSR1, &sa, nullptr);
}

void usage(const char* argv0) {
    std::cout
        << "Usage: " << argv0 << " [options]\n\n"
        << "Simulates N optical transceiver ports, polls their DDM telemetry on a\n"
        << "pool of threads and raises/clears alarms with hysteresis and debouncing.\n\n"
        << "Options:\n"
        << "  --ports N            number of transceiver ports (default 16)\n"
        << "  --pollers N          poller threads (default 2)\n"
        << "  --workers N          alarm-evaluation worker threads (default 4)\n"
        << "  --rate HZ            samples per second per port, 0 = unpaced (default 10)\n"
        << "  --duration S         run time in seconds when --samples is 0 (default 10)\n"
        << "  --samples N          stop after N samples per port (default 0 = use --duration)\n"
        << "  --queue N            per-worker queue capacity (default 1024)\n"
        << "  --soak N             consecutive samples to raise/clear an alarm (default 3)\n"
        << "  --seed N             RNG seed for reproducible runs (default 42)\n"
        << "  --alarm-log PATH     JSON-lines alarm log ('-' = stdout)\n"
        << "  --telemetry-log PATH JSON-lines telemetry log\n"
        << "  --summary-json PATH  write run statistics as JSON\n"
        << "  --inject P:FAULT:START:LEN\n"
        << "                       inject FAULT on port P from sample START for LEN samples;\n"
        << "                       FAULT = los | rx-degrade | ber-degrade | over-temp | tx-fail\n"
        << "  -h, --help           show this help\n\n"
        << "Signals: SIGINT/SIGTERM = graceful drain and exit, SIGUSR1 = dump active alarms.\n";
}

ota::Injection parse_injection(const std::string& spec) {
    // P:FAULT:START:LEN
    std::size_t a = spec.find(':');
    std::size_t b = a == std::string::npos ? a : spec.find(':', a + 1);
    std::size_t c = b == std::string::npos ? b : spec.find(':', b + 1);
    if (c == std::string::npos) throw std::invalid_argument("bad --inject spec: " + spec);
    ota::Injection inj;
    inj.port = static_cast<std::uint32_t>(std::stoul(spec.substr(0, a)));
    inj.window.fault = ota::fault_from_string(spec.substr(a + 1, b - a - 1));
    inj.window.start_seq = std::stoull(spec.substr(b + 1, c - b - 1));
    inj.window.length = std::stoull(spec.substr(c + 1));
    return inj;
}

void dump_active(const ota::Agent& agent) {
    const auto active = agent.active_alarms();
    std::cerr << "--- " << active.size() << " active alarm(s) ---\n";
    for (const auto& e : active) {
        std::cerr << "  port " << e.port << "  " << ota::to_string(e.type) << "  "
                  << ota::to_string(e.severity) << "  value=" << e.value << "\n";
    }
}

void write_summary_json(const std::string& path, const ota::AgentStats& s) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) throw std::runtime_error("cannot open summary file: " + path);
    char buf[640];
    std::snprintf(buf, sizeof(buf),
                  "{\n  \"samples_polled\": %llu,\n  \"samples_processed\": %llu,\n"
                  "  \"alarms_raised\": %llu,\n  \"alarms_cleared\": %llu,\n"
                  "  \"active_alarms\": %zu,\n  \"max_queue_depth\": %zu,\n"
                  "  \"elapsed_s\": %.4f,\n  \"throughput_sps\": %.1f,\n"
                  "  \"latency_p50_us\": %.1f,\n  \"latency_p99_us\": %.1f,\n"
                  "  \"latency_max_us\": %.1f\n}\n",
                  static_cast<unsigned long long>(s.samples_polled),
                  static_cast<unsigned long long>(s.samples_processed),
                  static_cast<unsigned long long>(s.alarms_raised),
                  static_cast<unsigned long long>(s.alarms_cleared), s.active_alarms, s.max_queue_depth,
                  s.elapsed_s, s.throughput_sps, s.latency_p50_us, s.latency_p99_us, s.latency_max_us);
    out << buf;
}

}  // namespace

int main(int argc, char** argv) {
    ota::AgentConfig cfg;
    double duration_s = 10.0;
    std::string summary_path;

    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto value = [&]() -> std::string {
                if (i + 1 >= argc) throw std::invalid_argument("missing value for " + arg);
                return argv[++i];
            };
            if (arg == "-h" || arg == "--help") { usage(argv[0]); return 0; }
            else if (arg == "--ports") cfg.ports = static_cast<std::uint32_t>(std::stoul(value()));
            else if (arg == "--pollers") cfg.pollers = static_cast<std::uint32_t>(std::stoul(value()));
            else if (arg == "--workers") cfg.workers = static_cast<std::uint32_t>(std::stoul(value()));
            else if (arg == "--rate") cfg.rate_hz = std::stod(value());
            else if (arg == "--duration") duration_s = std::stod(value());
            else if (arg == "--samples") cfg.samples_per_port = std::stoull(value());
            else if (arg == "--queue") cfg.queue_capacity = std::stoul(value());
            else if (arg == "--soak") cfg.soak_samples = std::stoi(value());
            else if (arg == "--seed") cfg.seed = std::stoull(value());
            else if (arg == "--alarm-log") cfg.alarm_log = value();
            else if (arg == "--telemetry-log") cfg.telemetry_log = value();
            else if (arg == "--summary-json") summary_path = value();
            else if (arg == "--inject") cfg.injections.push_back(parse_injection(value()));
            else throw std::invalid_argument("unknown option: " + arg);
        }

        install_signal_handlers();
        ota::Agent agent(cfg);
        agent.start();

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                  std::chrono::duration<double>(duration_s));
        while (!g_stop.load() && !agent.pollers_finished()) {
            if (cfg.samples_per_port == 0 && std::chrono::steady_clock::now() >= deadline) break;
            if (g_dump.exchange(false)) dump_active(agent);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        agent.join();

        const ota::AgentStats s = agent.stats();
        std::fprintf(stderr,
                     "ports=%u workers=%u  polled=%llu processed=%llu  %.0f samples/s\n"
                     "latency (capture->decision): p50=%.0fus p99=%.0fus max=%.0fus  max queue depth=%zu\n"
                     "alarms raised=%llu cleared=%llu active=%zu\n",
                     agent.config().ports, agent.config().workers,
                     static_cast<unsigned long long>(s.samples_polled),
                     static_cast<unsigned long long>(s.samples_processed), s.throughput_sps,
                     s.latency_p50_us, s.latency_p99_us, s.latency_max_us, s.max_queue_depth,
                     static_cast<unsigned long long>(s.alarms_raised),
                     static_cast<unsigned long long>(s.alarms_cleared), s.active_alarms);
        if (!summary_path.empty()) write_summary_json(summary_path, s);
        return s.samples_polled == s.samples_processed ? 0 : 2;
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }
}
