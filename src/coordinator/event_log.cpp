// Experiment event log — steps 2.23-2.26. See event_log.hpp.

#include "p2pgpu/coordinator/event_log.hpp"

#include <memory>

namespace p2pgpu::coordinator {
namespace {
using protocol::ErrorCode;
using protocol::MakeError;
}  // namespace

protocol::Result<std::unique_ptr<EventLog>> EventLog::Open(const std::string& path) {
    // "w", not "a": an experiment appending to a previous run's file would
    // silently merge two fleets into one dataset, and the analysis would have
    // no way to tell.
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (f == nullptr) {
        return MakeError(ErrorCode::Internal, "could not open events csv: " + path);
    }
    std::fprintf(f, "t_ms,event,task_id,worker_id,unit_count,duration_ms,"
                    "predicted_ms,correction,speculative\n");
    return std::unique_ptr<EventLog>(new EventLog(f));
}

EventLog::~EventLog() {
    if (f_ != nullptr) {
        std::fclose(f_);
    }
}

void EventLog::Row(std::uint64_t t_ms, const char* event, std::uint64_t task,
                   std::uint64_t worker, std::uint64_t unit_count, double duration_ms,
                   double predicted_ms, double correction, int speculative) {
    if (f_ == nullptr) {
        return;
    }
    std::fprintf(f_, "%llu,%s,%llu,%llu,%llu,%.3f,%.3f,%.4f,%d\n",
                 static_cast<unsigned long long>(t_ms), event,
                 static_cast<unsigned long long>(task),
                 static_cast<unsigned long long>(worker),
                 static_cast<unsigned long long>(unit_count), duration_ms, predicted_ms,
                 correction, speculative);
}

void EventLog::Grant(std::uint64_t t_ms, protocol::TaskId task,
                     protocol::WorkerId worker, std::uint64_t unit_count,
                     double predicted_ms, bool speculative) {
    Row(t_ms, "grant", task.lo(), worker.hi(), unit_count, 0.0, predicted_ms, 0.0,
        speculative ? 1 : 0);
}

void EventLog::Accept(std::uint64_t t_ms, protocol::TaskId task,
                      protocol::WorkerId worker, std::uint64_t unit_count,
                      double duration_ms, double predicted_ms, double correction) {
    Row(t_ms, "accept", task.lo(), worker.hi(), unit_count, duration_ms, predicted_ms,
        correction, 0);
}

void EventLog::Expire(std::uint64_t t_ms, protocol::TaskId task,
                      protocol::WorkerId worker, std::uint64_t unit_count) {
    Row(t_ms, "expire", task.lo(), worker.hi(), unit_count, 0.0, 0.0, 0.0, 0);
}

void EventLog::Cancel(std::uint64_t t_ms, protocol::TaskId task,
                      protocol::WorkerId worker, std::uint64_t unit_count) {
    Row(t_ms, "cancel", task.lo(), worker.hi(), unit_count, 0.0, 0.0, 0.0, 1);
}

void EventLog::WorkerJoin(std::uint64_t t_ms, protocol::WorkerId worker) {
    Row(t_ms, "worker_join", 0, worker.hi(), 0, 0.0, 0.0, 0.0, 0);
}

void EventLog::WorkerLost(std::uint64_t t_ms, protocol::WorkerId worker) {
    Row(t_ms, "worker_lost", 0, worker.hi(), 0, 0.0, 0.0, 0.0, 0);
}

}  // namespace p2pgpu::coordinator
