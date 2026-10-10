#pragma once

#include <algorithm>
#include <cmath>

#include "common/macros.h"
#include "common/logging.h"

#include "order_manager.h"
#include "feature_engine.h"

using namespace Common;

namespace Trading {
  class MarketMaker {
  public:
    MarketMaker(Common::Logger *logger, TradeEngine *trade_engine, const FeatureEngine *feature_engine,
                const PositionKeeper *position_keeper, OrderManager *order_manager,
                const TradeEngineCfgHashMap &ticker_cfg);

    auto onOrderBookUpdate(TickerId ticker_id, Price price, Side side, const MarketOrderBook *book) noexcept -> void {
      logger_->log("%:% %() % ticker:% price:% side:%\n", __FILE__, __LINE__, __FUNCTION__,
                   Common::getCurrentTimeStr(&time_str_), ticker_id, Common::priceToString(price).c_str(),
                   Common::sideToString(side).c_str());

      const auto bbo = book->getBBO();
      const auto fair_price = feature_engine_->getMktPrice();

      if (LIKELY(bbo->bid_price_ != Price_INVALID && bbo->ask_price_ != Price_INVALID && fair_price != Feature_INVALID)) {
        const auto cfg = ticker_cfg_.at(ticker_id);
        const auto current_position = position_keeper_ ? position_keeper_->getNetPosition(ticker_id) : 0;
        const auto target_position = static_cast<double>(cfg.target_position_);
        const auto inventory_gap = static_cast<double>(current_position) - target_position;
        const auto adjusted_fair = fair_price - cfg.inventory_k_ * inventory_gap;
        const auto mid_vol = std::max(0.0, feature_engine_->getVolatility());
        const auto target_spread = std::max(cfg.base_spread_ + cfg.vol_k_ * mid_vol, cfg.tick_size_);
        const auto tick = std::max(1.0, cfg.tick_size_);

        const auto bid_target = adjusted_fair - 0.5 * target_spread;
        const auto ask_target = adjusted_fair + 0.5 * target_spread;
        const auto price_step = static_cast<Price>(tick);
        const auto bid_price = std::max<Price>(1, static_cast<Price>(std::floor(bid_target / price_step) * price_step));
        const auto ask_price = std::max<Price>(1, static_cast<Price>(std::ceil(ask_target / price_step) * price_step));
        const auto final_bid = std::min<Price>(bbo->bid_price_, bid_price);
        const auto final_ask = std::max<Price>(bbo->ask_price_, ask_price);
        const auto safe_bid = (final_bid >= final_ask) ? std::max<Price>(bbo->bid_price_ - 1, Price_INVALID) : final_bid;
        const auto safe_ask = (safe_bid == Price_INVALID || safe_bid >= final_ask) ? std::max<Price>(final_ask, bbo->ask_price_ + 1) : final_ask;

        const auto base_qty = std::max<Qty>(1, cfg.clip_);
        const auto quantity = dynamicQuoteQty(base_qty, current_position, cfg.target_position_, cfg.risk_cfg_.max_position_);

        logger_->log("%:% %() % % fair-price:% adjusted-fair:% spread:% bid:% ask:% qty:(%/% )\n", __FILE__, __LINE__, __FUNCTION__,
                     Common::getCurrentTimeStr(&time_str_), bbo->toString().c_str(), fair_price, adjusted_fair,
                     target_spread, priceToString(safe_bid).c_str(), priceToString(safe_ask).c_str(),
                     quantity.first, quantity.second);

        START_MEASURE(Trading_OrderManager_moveOrders);
        order_manager_->moveOrders(ticker_id, safe_bid, safe_ask, quantity.first, quantity.second);
        END_MEASURE(Trading_OrderManager_moveOrders, (*logger_));
      }
    }

    // 处理交易事件，对于做市算法而言无实际操作
    auto onTradeUpdate(const Exchange::MEMarketUpdate *market_update, MarketOrderBook * /* book */) noexcept -> void {
      logger_->log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                   market_update->toString().c_str());
    }

    // 处理策略订单的客户端响应
    auto onOrderUpdate(const Exchange::MEClientResponse *client_response) noexcept -> void {
      logger_->log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                   client_response->toString().c_str());

      START_MEASURE(Trading_OrderManager_onOrderUpdate);
      order_manager_->onOrderUpdate(client_response);
      END_MEASURE(Trading_OrderManager_onOrderUpdate, (*logger_));
    }

    MarketMaker() = delete;
    MarketMaker(const MarketMaker &) = delete;
    MarketMaker(const MarketMaker &&) = delete;
    MarketMaker &operator=(const MarketMaker &) = delete;
    MarketMaker &operator=(const MarketMaker &&) = delete;

  private:
    auto dynamicQuoteQty(Qty base_qty, int32_t current_position, int32_t target_position, Qty max_position) const noexcept -> std::pair<Qty, Qty> {
      const auto inventory_gap = static_cast<double>(current_position) - static_cast<double>(target_position);
      const auto exposure_cap = std::max<Qty>(1, max_position);
      const auto normalized_gap = std::clamp(inventory_gap / std::max(1.0, static_cast<double>(exposure_cap)), -1.0, 1.0);
      const auto bid_multiplier = std::clamp(1.0 - normalized_gap, 0.2, 1.0);
      const auto ask_multiplier = std::clamp(1.0 + normalized_gap, 0.2, 1.0);

      const auto bid_qty = std::max<Qty>(1, static_cast<Qty>(std::round(static_cast<double>(base_qty) * bid_multiplier)));
      const auto ask_qty = std::max<Qty>(1, static_cast<Qty>(std::round(static_cast<double>(base_qty) * ask_multiplier)));
      return {bid_qty, ask_qty};
    }

    const FeatureEngine *feature_engine_ = nullptr;
    const PositionKeeper *position_keeper_ = nullptr;
    OrderManager *order_manager_ = nullptr;

    std::string time_str_;
    Common::Logger *logger_ = nullptr;

    const TradeEngineCfgHashMap ticker_cfg_;
  };
}