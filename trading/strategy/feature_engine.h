#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#include "common/macros.h"
#include "common/logging.h"

using namespace Common;

namespace Trading {
  // 用于表示无效/未初始化特征值的标记值
  constexpr auto Feature_INVALID = std::numeric_limits<double>::quiet_NaN();

  class FeatureEngine {
  public:
    FeatureEngine(Common::Logger *logger)
        : logger_(logger) {
    }

    auto onOrderBookUpdate(TickerId ticker_id, Price price, Side side, MarketOrderBook* book) noexcept -> void {
      const auto bbo = book->getBBO();
      if(LIKELY(bbo->bid_price_ != Price_INVALID && bbo->ask_price_ != Price_INVALID)) {
        const auto mid_price = 0.5 * (static_cast<double>(bbo->bid_price_) + static_cast<double>(bbo->ask_price_));
        mkt_price_ = (bbo->bid_price_ * bbo->ask_qty_ + bbo->ask_price_ * bbo->bid_qty_) /
                     static_cast<double>(bbo->bid_qty_ + bbo->ask_qty_);

        if (std::isfinite(last_mid_price_)) {
          const auto delta = std::abs(mid_price - last_mid_price_);
          volatility_ = 0.7 * volatility_ + 0.3 * delta;
        }
        last_mid_price_ = mid_price;

        const auto bid_qty = static_cast<double>(bbo->bid_qty_);
        const auto ask_qty = static_cast<double>(bbo->ask_qty_);
        const auto total_qty = std::max(1.0, bid_qty + ask_qty);
        queue_imbalance_ = (bid_qty - ask_qty) / total_qty;

        if (has_valid_l1_snapshot_) {
          const auto bid_price_delta = static_cast<double>(bbo->bid_price_) - static_cast<double>(last_bid_price_);
          const auto ask_price_delta = static_cast<double>(bbo->ask_price_) - static_cast<double>(last_ask_price_);
          const auto bid_qty_delta = static_cast<double>(bbo->bid_qty_) - static_cast<double>(last_bid_qty_);
          const auto ask_qty_delta = static_cast<double>(bbo->ask_qty_) - static_cast<double>(last_ask_qty_);

          const auto bid_contribution = bid_qty_delta + std::max(0.0, bid_price_delta);
          const auto ask_contribution = ask_qty_delta + std::max(0.0, -ask_price_delta);
          event_ofi_ = (bid_contribution - ask_contribution) / total_qty;
        } else {
          event_ofi_ = 0.0;
        }

        updateRollingOfi(event_ofi_);
        has_valid_l1_snapshot_ = true;
        last_bid_price_ = bbo->bid_price_;
        last_ask_price_ = bbo->ask_price_;
        last_bid_qty_ = bbo->bid_qty_;
        last_ask_qty_ = bbo->ask_qty_;
      } else {
        queue_imbalance_ = 0.0;
        event_ofi_ = 0.0;
        last_bid_price_ = Price_INVALID;
        last_ask_price_ = Price_INVALID;
        last_bid_qty_ = Qty_INVALID;
        last_ask_qty_ = Qty_INVALID;
        has_valid_l1_snapshot_ = false;
      }

      logger_->log("%:% %() % ticker:% price:% side:% mkt-price:% agg-trade-ratio:% vol:% q-imb:% event-ofi:% rolling-ofi:% std-ofi:%\n",
                   __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), ticker_id,
                   Common::priceToString(price).c_str(), Common::sideToString(side).c_str(), mkt_price_,
                   agg_trade_qty_ratio_, volatility_, queue_imbalance_, event_ofi_, ofi_, standardized_ofi_);
    }

    auto onTradeUpdate(const Exchange::MEMarketUpdate *market_update, MarketOrderBook* book) noexcept -> void {
      const auto bbo = book->getBBO();
      if(LIKELY(bbo->bid_price_ != Price_INVALID && bbo->ask_price_ != Price_INVALID)) {
        agg_trade_qty_ratio_ = static_cast<double>(market_update->qty_) /
                               (market_update->side_ == Side::BUY ? bbo->ask_qty_ : bbo->bid_qty_);
      }

      logger_->log("%:% %() % % mkt-price:% agg-trade-ratio:% vol:% q-imb:% event-ofi:% rolling-ofi:% std-ofi:%\n",
                   __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                   market_update->toString().c_str(), mkt_price_, agg_trade_qty_ratio_, volatility_,
                   queue_imbalance_, event_ofi_, ofi_, standardized_ofi_);
    }

    auto getMktPrice() const noexcept { return mkt_price_; }
    auto getAggTradeQtyRatio() const noexcept { return agg_trade_qty_ratio_; }
    auto getVolatility() const noexcept { return volatility_; }
    auto getQueueImbalance() const noexcept { return queue_imbalance_; }
    auto getEventOfi() const noexcept { return event_ofi_; }
    auto getOfi() const noexcept { return ofi_; }
    auto getStandardizedOfi() const noexcept { return standardized_ofi_; }

    FeatureEngine() = delete;
    FeatureEngine(const FeatureEngine &) = delete;
    FeatureEngine(const FeatureEngine &&) = delete;
    FeatureEngine &operator=(const FeatureEngine &) = delete;
    FeatureEngine &operator=(const FeatureEngine &&) = delete;

  private:
    auto updateRollingOfi(double sample) noexcept -> void {
      ofi_window_.at(ofi_index_) = sample;
      ofi_index_ = (ofi_index_ + 1) % ofi_window_.size();
      if (ofi_count_ < ofi_window_.size())
        ++ofi_count_;

      double sum = 0.0;
      double sum_sq = 0.0;
      for (size_t i = 0; i < ofi_count_; ++i) {
        const auto value = ofi_window_.at(i);
        sum += value;
        sum_sq += value * value;
      }

      const auto count = static_cast<double>(ofi_count_);
      ofi_ = sum / count;
      const auto variance = std::max(0.0, (sum_sq / count) - (ofi_ * ofi_));
      ofi_stddev_ = std::sqrt(variance);
      standardized_ofi_ = (ofi_stddev_ > 0.0) ? ((sample - ofi_) / ofi_stddev_) : 0.0;
    }

    std::string time_str_;
    Common::Logger *logger_ = nullptr;

    static constexpr size_t OFI_WINDOW = 32;
    std::array<double, OFI_WINDOW> ofi_window_{};
    size_t ofi_index_ = 0;
    size_t ofi_count_ = 0;

    double mkt_price_ = Feature_INVALID, agg_trade_qty_ratio_ = Feature_INVALID;
    double volatility_ = 0.0;
    double last_mid_price_ = Feature_INVALID;
    double queue_imbalance_ = 0.0;
    double event_ofi_ = 0.0;
    double ofi_ = 0.0;
    double standardized_ofi_ = 0.0;
    double ofi_stddev_ = 0.0;

    bool has_valid_l1_snapshot_ = false;
    Price last_bid_price_ = Price_INVALID;
    Price last_ask_price_ = Price_INVALID;
    Qty last_bid_qty_ = Qty_INVALID;
    Qty last_ask_qty_ = Qty_INVALID;
  };
}
