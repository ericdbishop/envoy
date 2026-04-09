#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>

#include "envoy/common/resource.h"
#include "envoy/event/dispatcher.h"
#include "envoy/event/timer.h"
#include "envoy/runtime/runtime.h"
#include "envoy/upstream/resource_manager.h"
#include "envoy/upstream/upstream.h"

#include "source/common/common/assert.h"
#include "source/common/common/basic_resource_impl.h"

namespace Envoy {
namespace Upstream {

struct ManagedResourceImpl : public BasicResourceLimitImpl {
  ManagedResourceImpl(uint64_t max, Runtime::Loader& runtime, const std::string& runtime_key,
                      Stats::Gauge& open_gauge, Stats::Gauge& remaining)
      : BasicResourceLimitImpl(max, runtime, runtime_key), open_gauge_(open_gauge),
        remaining_(remaining) {
    remaining_.set(max);
  }

  // BasicResourceLimitImpl
  void inc() override {
    BasicResourceLimitImpl::inc();
    updateRemaining();
    open_gauge_.set(BasicResourceLimitImpl::canCreate() ? 0 : 1);
    if (on_inc_cb_) {
      on_inc_cb_();
    }
  }
  void decBy(uint64_t amount) override {
    BasicResourceLimitImpl::decBy(amount);
    updateRemaining();
    open_gauge_.set(BasicResourceLimitImpl::canCreate() ? 0 : 1);
  }

  // Optional callback function used for counting new requests in retry budget.
  std::function<void()> on_inc_cb_;

  /**
   * We set the gauge instead of incrementing and decrementing because,
   * though atomics are used, it is possible for the current resource count
   * to be greater than the supplied max.
   */
  void updateRemaining() {
    /**
     * We cannot use std::max here because max() and current_ are
     * unsigned and subtracting them may overflow.
     */
    const uint64_t current_copy = current_;
    remaining_.set(max() > current_copy ? max() - current_copy : 0);
  }

  /**
   * A gauge to notify the live circuit breaker state. The gauge is set to 0
   * to notify that the circuit breaker is not yet triggered.
   */
  Stats::Gauge& open_gauge_;

  /**
   * The number of resources remaining before the circuit breaker opens.
   */
  Stats::Gauge& remaining_;
};

/**
 * Implementation of ResourceManager.
 * NOTE: This implementation makes some assumptions which favor simplicity over correctness.
 * 1) Primarily, it assumes that traffic will be mostly balanced over all the worker threads since
 *    no attempt is made to balance resources between them. It is possible that starvation can
 *    occur during high contention.
 * 2) Though atomics are used, it is possible for resources to temporarily go above the supplied
 *    maximums. This should not effect overall behavior.
 */
class ResourceManagerImpl : public ResourceManager {
public:
  ResourceManagerImpl(Runtime::Loader& runtime, const std::string& runtime_key,
                      uint64_t max_connections, uint64_t max_pending_requests,
                      uint64_t max_requests, uint64_t max_retries, uint64_t max_connection_pools,
                      uint64_t max_connections_per_host, ClusterCircuitBreakersStats cb_stats,
                      absl::optional<double> budget_percent,
                      absl::optional<uint64_t> budget_interval,
                      absl::optional<uint32_t> min_retry_concurrency, Event::Dispatcher& dispatcher)
      : connections_(max_connections, runtime, runtime_key + "max_connections", cb_stats.cx_open_,
                     cb_stats.remaining_cx_),
        pending_requests_(max_pending_requests, runtime, runtime_key + "max_pending_requests",
                          cb_stats.rq_pending_open_, cb_stats.remaining_pending_),
        requests_(max_requests, runtime, runtime_key + "max_requests", cb_stats.rq_open_,
                  cb_stats.remaining_rq_),
        connection_pools_(max_connection_pools, runtime, runtime_key + "max_connection_pools",
                          cb_stats.cx_pool_open_, cb_stats.remaining_cx_pools_),
        max_connections_per_host_(max_connections_per_host),
        retries_(budget_percent, budget_interval, min_retry_concurrency, max_retries, runtime,
                 runtime_key + "retry_budget.", runtime_key + "max_retries",
                 cb_stats.rq_retry_open_, cb_stats.remaining_retries_, requests_, pending_requests_,
                 dispatcher) {
    // Count active requests when retry budget is configured.
    // Pending requests are not counted to avoid counting them twice.
    if (budget_percent.has_value() || min_retry_concurrency.has_value()) {
      requests_.on_inc_cb_ = [this]() { retries_.writeRequest(); };
    }
  }

  // Upstream::ResourceManager
  ResourceLimit& connections() override { return connections_; }
  ResourceLimit& pendingRequests() override { return pending_requests_; }
  ResourceLimit& requests() override { return requests_; }
  ResourceLimit& retries() override { return retries_; }
  ResourceLimit& connectionPools() override { return connection_pools_; }
  uint64_t maxConnectionsPerHost() override { return max_connections_per_host_; }

private:
  class RetryBudgetImpl : public ResourceLimit {
  public:
    RetryBudgetImpl(absl::optional<double> budget_percent, absl::optional<uint64_t> budget_interval,
                    absl::optional<uint32_t> min_retry_concurrency, uint64_t max_retries,
                    Runtime::Loader& runtime, const std::string& retry_budget_runtime_key,
                    const std::string& max_retries_runtime_key, Stats::Gauge& open_gauge,
                    Stats::Gauge& remaining, const ResourceLimit& requests,
                    const ResourceLimit& pending_requests, Event::Dispatcher& dispatcher)
        : runtime_(runtime),
          max_retry_resource_(max_retries, runtime, max_retries_runtime_key, open_gauge, remaining),
          budget_percent_(budget_percent), budget_interval_(budget_interval),
          min_retry_concurrency_(min_retry_concurrency),
          budget_percent_key_(retry_budget_runtime_key + "budget_percent"),
          budget_interval_key_(retry_budget_runtime_key + "budget_interval"),
          min_retry_concurrency_key_(retry_budget_runtime_key + "min_retry_concurrency"),
          requests_(requests), pending_requests_(pending_requests), remaining_(remaining),
          budget_expiration_interval_ms_(budget_interval.has_value() ? budget_interval.value() / 10
                                                                     : 0),
          req_in_interval_(0), req_to_expire_(0), dispatcher_(dispatcher) {
      if (budget_expiration_interval_ms_ > 0 && budget_interval.has_value()) {
        const auto expiration_interval = std::chrono::milliseconds(budget_expiration_interval_ms_);
        const auto budget_interval_ms = std::chrono::milliseconds(budget_interval.value());

        main_timer_ = dispatcher_.createTimer([this, expiration_interval, budget_interval_ms]() {
          // Schedule a function to decrement req_to_expire_ from req_in_interval_ and reset
          // req_to_expire_ to 0, every budget_interval / 10 seconds.
          const uint64_t to_expire = req_to_expire_.exchange(0, std::memory_order_seq_cst);

          if (to_expire > 0) {
            // Schedule expiration in budget_interval_ms.
            auto expire_timer = dispatcher_.createTimer([this, to_expire]() {
              req_in_interval_.fetch_sub(to_expire, std::memory_order_seq_cst);
              expire_timers_.pop_front();
            });
            expire_timer->enableTimer(budget_interval_ms);
            expire_timers_.push_back(std::move(expire_timer));
          }

          main_timer_->enableTimer(expiration_interval);
        });
        main_timer_->enableTimer(expiration_interval);
      }
    }

    // Envoy::ResourceLimit
    bool canCreate() override {
      if (!useRetryBudget()) {
        return max_retry_resource_.canCreate();
      }
      clearRemainingGauge();
      return count() < max();
    }
    void inc() override {
      max_retry_resource_.inc();
      clearRemainingGauge();
    }
    void dec() override {
      max_retry_resource_.dec();
      clearRemainingGauge();
    }
    void decBy(uint64_t amount) override {
      max_retry_resource_.decBy(amount);
      clearRemainingGauge();
    }
    void writeRequest() {
      req_in_interval_.fetch_add(1, std::memory_order_relaxed);
      req_to_expire_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t max() override {
      if (!useRetryBudget()) {
        return max_retry_resource_.max();
      }

      const double budget_percent = runtime_.snapshot().getDouble(
          budget_percent_key_, budget_percent_ ? *budget_percent_ : 20.0);
      const uint64_t budget_interval = runtime_.snapshot().getInteger(
          budget_interval_key_, budget_interval_ ? *budget_interval_ : 0);
      const uint32_t min_retry_concurrency = runtime_.snapshot().getInteger(
          min_retry_concurrency_key_, min_retry_concurrency_ ? *min_retry_concurrency_ : 3);

      clearRemainingGauge();

      // Use the in-flight request count when budget_interval is explicitly set to 0.
      if (budget_interval == 0) {
        // We enforce that the retry concurrency is never allowed to go below the
        // min_retry_concurrency, even if the configured percent of the current active requests
        // yields a value that is smaller.
        const uint64_t current_active = requests_.count() + pending_requests_.count();
        return std::max<uint64_t>(budget_percent / 100.0 * current_active, min_retry_concurrency);
      }

      // Retry budget implementation across a fixed window inspired by tower.rs:
      // https://github.com/tower-rs/tower/blob/master/tower/src/retry/budget/tps_budget.rs
      const uint64_t requests_for_budget = requestsInBudgetInterval();
      return std::max<uint64_t>(budget_percent / 100.0 * requests_for_budget,
                                min_retry_concurrency);
    }
    uint64_t count() const override { return max_retry_resource_.count(); }

  private:
    bool useRetryBudget() const {
      return runtime_.snapshot().get(budget_percent_key_).has_value() ||
             runtime_.snapshot().get(min_retry_concurrency_key_).has_value() || budget_percent_ ||
             min_retry_concurrency_;
    }

    // If the retry budget is in use, the stats tracking remaining retries do not make sense since
    // they would dependent on other resources that can change without a call to this object.
    // Therefore, the gauge should just be reset to 0.
    void clearRemainingGauge() {
      if (useRetryBudget()) {
        remaining_.set(0);
      }
    }

    uint64_t requestsInBudgetInterval() { return req_in_interval_.load(std::memory_order_seq_cst); }

    Runtime::Loader& runtime_;
    // The max_retry resource is nested within the budget to maintain state if the retry budget is
    // toggled.
    ManagedResourceImpl max_retry_resource_;
    const absl::optional<double> budget_percent_;
    const absl::optional<uint64_t> budget_interval_;
    const absl::optional<uint32_t> min_retry_concurrency_;
    const std::string budget_percent_key_;
    const std::string budget_interval_key_;
    const std::string min_retry_concurrency_key_;
    const ResourceLimit& requests_;
    const ResourceLimit& pending_requests_;
    Stats::Gauge& remaining_;

    const uint64_t budget_expiration_interval_ms_;
    std::atomic<uint64_t> req_in_interval_;
    std::atomic<uint64_t> req_to_expire_;
    Event::Dispatcher& dispatcher_;
    Event::TimerPtr main_timer_;
    std::deque<Event::TimerPtr> expire_timers_;
  };

  ManagedResourceImpl connections_;
  ManagedResourceImpl pending_requests_;
  ManagedResourceImpl requests_;
  ManagedResourceImpl connection_pools_;
  uint64_t max_connections_per_host_;
  RetryBudgetImpl retries_;
};

using ResourceManagerImplPtr = std::unique_ptr<ResourceManagerImpl>;

} // namespace Upstream
} // namespace Envoy
