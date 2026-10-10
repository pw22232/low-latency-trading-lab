#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "common/types.h"

namespace Trading {
  using namespace Common;

  struct StrategyFillRecord {
    TickerId ticker_id_ = TickerId_INVALID;
    Side side_ = Side::INVALID;
    Price price_ = Price_INVALID;
    Qty qty_ = 0;
    bool maker_ = false;
    double fee_ = 0.0;
    double slippage_ = 0.0;
    uint64_t timestamp_ns_ = 0;
  };

  struct StrategyOrderRecord {
    TickerId ticker_id_ = TickerId_INVALID;
    Side side_ = Side::INVALID;
    Qty qty_ = 0;
    bool maker_ = false;
    bool filled_ = false;
    bool canceled_ = false;
    uint64_t timestamp_ns_ = 0;
  };

  struct MarketMidSnapshot {
    TickerId ticker_id_ = TickerId_INVALID;
    Price mid_price_ = Price_INVALID;
    uint64_t timestamp_ns_ = 0;
  };

  struct StrategyPerformanceSummary {
    double realized_pnl_ = 0.0;
    double unrealized_pnl_ = 0.0;
    double net_pnl_ = 0.0;
    double fill_rate_order_ = 0.0;
    double fill_rate_qty_ = 0.0;
    std::array<double, 6> markout_seconds_{};
    double max_drawdown_ = 0.0;
    double inventory_exposure_ = 0.0;
    double inventory_utilization_ = 0.0;
    double average_holding_time_ns_ = 0.0;
    size_t order_count_ = 0;
    size_t fill_count_ = 0;
    size_t cancel_count_ = 0;
    size_t maker_order_count_ = 0;
    size_t taker_order_count_ = 0;
    size_t maker_fill_count_ = 0;
    size_t taker_fill_count_ = 0;
    Qty filled_qty_ = 0;
    Qty requested_qty_ = 0;
    double transaction_cost_ = 0.0;
    double fees_ = 0.0;
    double slippage_ = 0.0;
  };

  class StrategyPerformanceTracker {
   public:
    explicit StrategyPerformanceTracker(size_t max_position = 1000) noexcept
        : max_position_(max_position) {
    }

    auto recordOrderAttempt(TickerId ticker_id, Side side, Qty qty, bool maker, uint64_t timestamp_ns) noexcept -> void {
      order_records_.push_back({ticker_id, side, qty, maker, false, false, timestamp_ns});
      requested_qty_ += qty;
      if (maker) ++maker_order_count_; else ++taker_order_count_;
      ++order_count_;
    }

    auto recordOrderFill(TickerId ticker_id, Side side, Price price, Qty qty, bool maker, double fee, double slippage,
                         uint64_t timestamp_ns) noexcept -> void {
      fills_.push_back({ticker_id, side, price, qty, maker, fee, slippage, timestamp_ns});
      if (maker) ++maker_fill_count_; else ++taker_fill_count_;
      ++fill_count_;
      filled_qty_ += qty;
      fees_ += fee;
      slippage_ += slippage;
      transaction_cost_ = fees_ + slippage_;

      // mark the order as filled if it was attempted before
      if (!order_records_.empty()) {
        auto &last = order_records_.back();
        if (last.ticker_id_ == ticker_id && last.side_ == side && !last.filled_) {
          last.filled_ = true;
        }
      }
    }

    auto recordOrderCancel(TickerId ticker_id, Side side, Qty qty, bool maker, uint64_t timestamp_ns) noexcept -> void {
      order_records_.push_back({ticker_id, side, qty, maker, false, true, timestamp_ns});
      ++cancel_count_;
      if (maker) ++maker_order_count_; else ++taker_order_count_;
    }

    auto recordMarketMid(TickerId ticker_id, Price mid_price, uint64_t timestamp_ns) noexcept -> void {
      market_mids_[ticker_id].push_back({ticker_id, mid_price, timestamp_ns});
    }

    auto computeSummary() const -> StrategyPerformanceSummary {
      StrategyPerformanceSummary summary{};
      summary.order_count_ = order_count_;
      summary.fill_count_ = fill_count_;
      summary.cancel_count_ = cancel_count_;
      summary.maker_order_count_ = maker_order_count_;
      summary.taker_order_count_ = taker_order_count_;
      summary.maker_fill_count_ = maker_fill_count_;
      summary.taker_fill_count_ = taker_fill_count_;
      summary.filled_qty_ = filled_qty_;
      summary.requested_qty_ = requested_qty_;
      summary.fees_ = fees_;
      summary.slippage_ = slippage_;
      summary.transaction_cost_ = transaction_cost_;

      if (order_count_ > 0)
        summary.fill_rate_order_ = static_cast<double>(fill_count_) / static_cast<double>(order_count_);
      if (requested_qty_ > 0)
        summary.fill_rate_qty_ = static_cast<double>(filled_qty_) / static_cast<double>(requested_qty_);

      std::map<TickerId, int32_t> position_by_ticker;
      std::map<TickerId, double> avg_entry_by_ticker;
      std::map<TickerId, int32_t> position_qty_by_ticker;

      for (const auto &fill: fills_) {
        const auto direction = (fill.side_ == Side::BUY ? 1 : -1);
        const auto ticker = fill.ticker_id_;
        const auto current_pos = position_by_ticker[ticker];
        if (direction > 0 && current_pos < 0) {
          const auto close_qty = std::min(static_cast<int32_t>(fill.qty_), std::abs(current_pos));
          summary.realized_pnl_ += (avg_entry_by_ticker[ticker] - static_cast<double>(fill.price_)) * close_qty;
        } else if (direction < 0 && current_pos > 0) {
          const auto close_qty = std::min(static_cast<int32_t>(fill.qty_), static_cast<int32_t>(current_pos));
          summary.realized_pnl_ += (static_cast<double>(fill.price_) - avg_entry_by_ticker[ticker]) * close_qty;
        }

        const auto next_pos = current_pos + direction * static_cast<int32_t>(fill.qty_);
        if (next_pos == 0) {
          avg_entry_by_ticker[ticker] = 0.0;
          position_qty_by_ticker[ticker] = 0;
        } else if (current_pos == 0) {
          avg_entry_by_ticker[ticker] = static_cast<double>(fill.price_);
          position_qty_by_ticker[ticker] = static_cast<int32_t>(fill.qty_);
        } else if ((current_pos > 0 && direction > 0) || (current_pos < 0 && direction < 0)) {
          const auto prev_cost = avg_entry_by_ticker[ticker] * std::abs(static_cast<double>(current_pos));
          const auto next_cost = prev_cost + static_cast<double>(fill.price_) * static_cast<double>(fill.qty_);
          const auto total_qty = std::abs(static_cast<double>(next_pos));
          avg_entry_by_ticker[ticker] = next_cost / total_qty;
          position_qty_by_ticker[ticker] = std::abs(next_pos);
        } else {
          const auto total_qty_after = std::abs(static_cast<double>(next_pos));
          position_qty_by_ticker[ticker] = static_cast<int32_t>(total_qty_after);
          avg_entry_by_ticker[ticker] = static_cast<double>(fill.price_);
        }
        position_by_ticker[ticker] = next_pos;
      }

      for (const auto &entry: position_by_ticker) {
        const auto ticker = entry.first;
        const auto pos = entry.second;
        if (pos == 0) continue;

        const auto &mids = market_mids_.find(ticker) != market_mids_.end() ? market_mids_.at(ticker) : std::vector<MarketMidSnapshot>{};
        Price mark_price = Price_INVALID;
        if (!mids.empty())
          mark_price = mids.back().mid_price_;
        if (mark_price == Price_INVALID)
          mark_price = static_cast<Price>(avg_entry_by_ticker[ticker]);

        if (pos > 0)
          summary.unrealized_pnl_ += (static_cast<double>(mark_price) - avg_entry_by_ticker[ticker]) * static_cast<double>(pos);
        else
          summary.unrealized_pnl_ += (avg_entry_by_ticker[ticker] - static_cast<double>(mark_price)) * static_cast<double>(std::abs(pos));
      }

      summary.net_pnl_ = summary.realized_pnl_ + summary.unrealized_pnl_;

      // Markout: use future mid-price at horizon and skip unexpired samples.
      const std::array<uint64_t, 6> horizons_ns = {1ULL * 1000ULL * 1000ULL * 1000ULL,
                                                 3ULL * 1000ULL * 1000ULL * 1000ULL,
                                                 5ULL * 1000ULL * 1000ULL * 1000ULL,
                                                 10ULL * 1000ULL * 1000ULL * 1000ULL,
                                                 20ULL * 1000ULL * 1000ULL * 1000ULL,
                                                 30ULL * 1000ULL * 1000ULL * 1000ULL};
      for (size_t i = 0; i < horizons_ns.size(); ++i) {
        double total = 0.0;
        size_t count = 0;
        for (const auto &fill: fills_) {
          const auto matches = market_mids_.find(fill.ticker_id_);
          if (matches == market_mids_.end()) continue;
          const auto target_ts = fill.timestamp_ns_ + horizons_ns.at(i);
          auto future_mid = Price_INVALID;
          for (const auto &snapshot: matches->second) {
            if (snapshot.timestamp_ns_ >= target_ts) {
              future_mid = snapshot.mid_price_;
              break;
            }
          }
          if (future_mid == Price_INVALID) continue;
          const auto direction = (fill.side_ == Side::BUY ? 1 : -1);
          const auto markout = (direction > 0) ? static_cast<double>(future_mid - fill.price_) : static_cast<double>(fill.price_ - future_mid);
          total += markout * static_cast<double>(fill.qty_);
          ++count;
        }
        summary.markout_seconds_[i] = (count > 0) ? total / static_cast<double>(count) : 0.0;
      }

      double max_equity = 0.0;
      double min_equity = 0.0;
      double running_equity = 0.0;
      for (const auto &fill: fills_) {
        running_equity += static_cast<double>(fill.qty_) * static_cast<double>(fill.price_) * ((fill.side_ == Side::BUY) ? 1.0 : -1.0);
        if (running_equity > max_equity) max_equity = running_equity;
        if (running_equity < min_equity) min_equity = running_equity;
      }
      summary.max_drawdown_ = std::max(0.0, max_equity - min_equity);

      // Inventory exposure and utilization are derived from the largest absolute position seen in the tracker.
      size_t max_abs_position = 0;
      for (const auto &entry: position_by_ticker) {
        max_abs_position = std::max(max_abs_position, static_cast<size_t>(std::abs(entry.second)));
      }
      summary.inventory_exposure_ = static_cast<double>(max_abs_position);
      if (max_position_ > 0)
        summary.inventory_utilization_ = static_cast<double>(max_abs_position) / static_cast<double>(max_position_);

      if (fills_.size() > 0) {
        uint64_t total_hold = 0;
        for (const auto &fill: fills_) {
          total_hold += fill.timestamp_ns_;
        }
        summary.average_holding_time_ns_ = total_hold / fills_.size();
      }

      return summary;
    }

    auto exportCsv(const std::string &path) const -> bool {
      std::ofstream out(path, std::ios::out | std::ios::trunc);
      if (!out.is_open()) return false;
      const auto summary = computeSummary();
      out << "metric,value\n";
      out << "realized_pnl," << summary.realized_pnl_ << "\n";
      out << "unrealized_pnl," << summary.unrealized_pnl_ << "\n";
      out << "net_pnl," << summary.net_pnl_ << "\n";
      out << "fill_rate_order," << summary.fill_rate_order_ << "\n";
      out << "fill_rate_qty," << summary.fill_rate_qty_ << "\n";
      out << "markout_1s," << summary.markout_seconds_.at(0) << "\n";
      out << "markout_3s," << summary.markout_seconds_.at(1) << "\n";
      out << "markout_5s," << summary.markout_seconds_.at(2) << "\n";
      out << "markout_10s," << summary.markout_seconds_.at(3) << "\n";
      out << "markout_20s," << summary.markout_seconds_.at(4) << "\n";
      out << "markout_30s," << summary.markout_seconds_.at(5) << "\n";
      out << "max_drawdown," << summary.max_drawdown_ << "\n";
      out << "inventory_exposure," << summary.inventory_exposure_ << "\n";
      out << "inventory_utilization," << summary.inventory_utilization_ << "\n";
      out << "average_holding_time_ns," << summary.average_holding_time_ns_ << "\n";
      out << "order_count," << summary.order_count_ << "\n";
      out << "fill_count," << summary.fill_count_ << "\n";
      out << "cancel_count," << summary.cancel_count_ << "\n";
      out << "maker_order_count," << summary.maker_order_count_ << "\n";
      out << "taker_order_count," << summary.taker_order_count_ << "\n";
      out << "maker_fill_count," << summary.maker_fill_count_ << "\n";
      out << "taker_fill_count," << summary.taker_fill_count_ << "\n";
      out << "filled_qty," << summary.filled_qty_ << "\n";
      out << "requested_qty," << summary.requested_qty_ << "\n";
      out << "transaction_cost," << summary.transaction_cost_ << "\n";
      out << "fees," << summary.fees_ << "\n";
      out << "slippage," << summary.slippage_ << "\n";
      return out.good();
    }

   private:
    size_t max_position_ = 0;
    std::vector<StrategyOrderRecord> order_records_;
    std::vector<StrategyFillRecord> fills_;
    std::map<TickerId, std::vector<MarketMidSnapshot>> market_mids_;
    size_t order_count_ = 0;
    size_t fill_count_ = 0;
    size_t cancel_count_ = 0;
    size_t maker_order_count_ = 0;
    size_t taker_order_count_ = 0;
    size_t maker_fill_count_ = 0;
    size_t taker_fill_count_ = 0;
    Qty filled_qty_ = 0;
    Qty requested_qty_ = 0;
    double fees_ = 0.0;
    double slippage_ = 0.0;
    double transaction_cost_ = 0.0;
  };
}
