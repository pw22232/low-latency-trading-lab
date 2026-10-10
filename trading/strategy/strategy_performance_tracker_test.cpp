#include <cassert>
#include <fstream>
#include <sstream>

#include "strategy_performance_tracker.h"

int main() {
  Trading::StrategyPerformanceTracker tracker(100);

  tracker.recordOrderAttempt(0, Common::Side::BUY, 10, true, 0);
  tracker.recordOrderFill(0, Common::Side::BUY, 100, 10, true, 0.10, 0.05, 0);
  tracker.recordMarketMid(0, 101, 1ULL * 1000ULL * 1000ULL * 1000ULL);
  tracker.recordMarketMid(0, 102, 3ULL * 1000ULL * 1000ULL * 1000ULL);

  const auto summary = tracker.computeSummary();

  assert(summary.fill_count_ == 1);
  assert(summary.order_count_ == 1);
  assert(summary.fill_rate_order_ > 0.0);
  assert(summary.fill_rate_qty_ > 0.0);
  assert(summary.markout_seconds_.at(0) >= 0.0);
  assert(summary.transaction_cost_ > 0.0);
  assert(summary.max_drawdown_ >= 0.0);

  const auto csv_path = std::string("performance_tracker_test.csv");
  assert(tracker.exportCsv(csv_path));
  std::ifstream csv(csv_path);
  assert(csv.is_open());
  std::stringstream buffer;
  buffer << csv.rdbuf();
  const auto content = buffer.str();
  assert(content.find("realized_pnl") != std::string::npos);
  assert(content.find("markout_1s") != std::string::npos);

  return 0;
}
