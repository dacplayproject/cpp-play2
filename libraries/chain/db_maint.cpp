/*
 * Copyright (c) 2015 Cryptonomex, Inc., and contributors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <boost/multiprecision/integer.hpp>

#include <fc/smart_ref_impl.hpp>
#include <fc/uint128.hpp>

#include <graphene/chain/database.hpp>
#include <graphene/chain/fba_accumulator_id.hpp>
#include <graphene/chain/hardfork.hpp>

#include <graphene/chain/account_object.hpp>
#include <graphene/chain/asset_object.hpp>
#include <graphene/chain/budget_record_object.hpp>
#include <graphene/chain/buyback_object.hpp>
#include <graphene/chain/chain_property_object.hpp>
#include <graphene/chain/committee_member_object.hpp>
#include <graphene/chain/fba_object.hpp>
#include <graphene/chain/global_property_object.hpp>
#include <graphene/chain/market_object.hpp>
#include <graphene/chain/special_authority_object.hpp>
#include <graphene/chain/vesting_balance_object.hpp>
#include <graphene/chain/vote_count.hpp>
#include <graphene/chain/witness_object.hpp>
#include <graphene/chain/hardfork.hpp>

namespace graphene { namespace chain {

template<class Index>
vector<std::reference_wrapper<const typename Index::object_type>> database::sort_votable_objects(size_t count) const
{
   using ObjectType = typename Index::object_type;
   const auto& all_objects = get_index_type<Index>().indices();
   count = std::min(count, all_objects.size());
   vector<std::reference_wrapper<const ObjectType>> refs;
   refs.reserve(all_objects.size());
   std::transform(all_objects.begin(), all_objects.end(),
                  std::back_inserter(refs),
                  [](const ObjectType& o) { return std::cref(o); });
   std::partial_sort(refs.begin(), refs.begin() + count, refs.end(),
                   [this](const ObjectType& a, const ObjectType& b)->bool {
      share_type oa_vote = _vote_tally_buffer[a.vote_id];
      share_type ob_vote = _vote_tally_buffer[b.vote_id];
      if( oa_vote != ob_vote )
         return oa_vote > ob_vote;
      return a.vote_id < b.vote_id;
   });

   refs.resize(count, refs.front());
   return refs;
}

template<class... Types>
void database::perform_account_maintenance(std::tuple<Types...> helpers)
{
   const auto& idx = get_index_type<account_index>().indices().get<by_name>();
   for( const account_object& a : idx )
      detail::for_each(helpers, a, detail::gen_seq<sizeof...(Types)>());
}

/// @brief A visitor for @ref worker_type which calls pay_worker on the worker within
struct worker_pay_visitor
{
   private:
      share_type pay;
      database& db;

   public:
      worker_pay_visitor(share_type pay, database& db)
         : pay(pay), db(db) {}

      typedef void result_type;
      template<typename W>
      void operator()(W& worker)const
      {
         worker.pay_worker(pay, db);
      }
};

void database::update_active_witnesses()
{ try {
   assert( _witness_count_histogram_buffer.size() > 0 );
   share_type stake_target = (_total_voting_stake-_witness_count_histogram_buffer[0]) / 2;

   /// accounts that vote for 0 or 1 witness do not get to express an opinion on
   /// the number of witnesses to have (they abstain and are non-voting accounts)

   share_type stake_tally = 0;

   size_t witness_count = 0;
   if( stake_target > 0 )
   {
      while( (witness_count < _witness_count_histogram_buffer.size() - 1)
             && (stake_tally <= stake_target) )
      {
         stake_tally += _witness_count_histogram_buffer[++witness_count];
      }
   }

   const chain_property_object& cpo = get_chain_properties();
   auto wits = sort_votable_objects<witness_index>(std::max(witness_count*2+1, (size_t)cpo.immutable_parameters.min_witness_count));

   const global_property_object& gpo = get_global_properties();

   const auto& all_witnesses = get_index_type<witness_index>().indices();

   for( const witness_object& wit : all_witnesses )
   {
      modify( wit, [&]( witness_object& obj ){
              obj.total_votes = _vote_tally_buffer[wit.vote_id];
              });
   }

   // Update witness authority
   modify( get(GRAPHENE_WITNESS_ACCOUNT), [&]( account_object& a )
   {
     vote_counter vc;
     for( const witness_object& wit : wits )
        vc.add( wit.witness_account, _vote_tally_buffer[wit.vote_id] );
     vc.finish( a.active );
   } );

   modify(gpo, [&]( global_property_object& gp ){
      gp.active_witnesses.clear();
      gp.active_witnesses.reserve(wits.size());
      std::transform(wits.begin(), wits.end(),
                     std::inserter(gp.active_witnesses, gp.active_witnesses.end()),
                     [](const witness_object& w) {
         return w.id;
      });
   });

} FC_CAPTURE_AND_RETHROW() }

void database::update_active_committee_members()
{ try {
   assert( _committee_count_histogram_buffer.size() > 0 );
   share_type stake_target = (_total_voting_stake-_witness_count_histogram_buffer[0]) / 2;

   /// accounts that vote for 0 or 1 witness do not get to express an opinion on
   /// the number of witnesses to have (they abstain and are non-voting accounts)
   uint64_t stake_tally = 0; // _committee_count_histogram_buffer[0];
   size_t committee_member_count = 0;
   if( stake_target > 0 )
      while( (committee_member_count < _committee_count_histogram_buffer.size() - 1)
             && (stake_tally <= stake_target) )
         stake_tally += _committee_count_histogram_buffer[++committee_member_count];

   const chain_property_object& cpo = get_chain_properties();
   auto committee_members = sort_votable_objects<committee_member_index>(std::max(committee_member_count*2+1, (size_t)cpo.immutable_parameters.min_committee_member_count));

   for( const committee_member_object& del : committee_members )
   {
      modify( del, [&]( committee_member_object& obj ){
              obj.total_votes = _vote_tally_buffer[del.vote_id];
              });
   }

   // Update committee authorities
   if( !committee_members.empty() )
   {
      modify(get(GRAPHENE_COMMITTEE_ACCOUNT), [&](account_object& a)
      {
        vote_counter vc;
        for( const committee_member_object& cm : committee_members )
           vc.add( cm.committee_member_account, _vote_tally_buffer[cm.vote_id] );
        vc.finish( a.active );
      } );
      modify(get(GRAPHENE_RELAXED_COMMITTEE_ACCOUNT), [&](account_object& a) {
         a.active = get(GRAPHENE_COMMITTEE_ACCOUNT).active;
      });
   }
   modify(get_global_properties(), [&](global_property_object& gp) {
      gp.active_committee_members.clear();
      std::transform(committee_members.begin(), committee_members.end(),
                     std::inserter(gp.active_committee_members, gp.active_committee_members.begin()),
                     [](const committee_member_object& d) { return d.id; });
   });
} FC_CAPTURE_AND_RETHROW() }

void database::initialize_budget_record( fc::time_point_sec now, budget_record& rec )const
{
   const dynamic_global_property_object& dpo = get_dynamic_global_properties();
   const asset_object& core = asset_id_type(0)(*this);
   const asset_dynamic_data_object& core_dd = core.dynamic_asset_data_id(*this);

   rec.from_initial_reserve = core.reserved(*this);
   if (this->head_block_time() >= HARDFORK_3_TIME)
   {
      rec.from_initial_reserve = 0;
   }
   rec.from_accumulated_fees = core_dd.accumulated_fees;
   rec.from_unused_witness_budget = dpo.witness_budget;

   if(    (dpo.last_budget_time == fc::time_point_sec())
       || (now <= dpo.last_budget_time) )
   {
      rec.time_since_last_budget = 0;
      return;
   }

   int64_t dt = (now - dpo.last_budget_time).to_seconds();
   rec.time_since_last_budget = uint64_t( dt );

   // We'll consider accumulated_fees to be reserved at the BEGINNING
   // of the maintenance interval.  However, for speed we only
   // call modify() on the asset_dynamic_data_object once at the
   // end of the maintenance interval.  Thus the accumulated_fees
   // are available for the budget at this point, but not included
   // in core.reserved().
   share_type reserve = rec.from_initial_reserve + core_dd.accumulated_fees;
   // Similarly, we consider leftover witness_budget to be burned
   // at the BEGINNING of the maintenance interval.
   reserve += dpo.witness_budget;

   fc::uint128_t budget_u128 = reserve.value < 0 ? 0 : reserve.value;
   budget_u128 *= uint64_t(dt);
   budget_u128 *= GRAPHENE_CORE_ASSET_CYCLE_RATE;
   //round up to the nearest satoshi -- this is necessary to ensure
   //   there isn't an "untouchable" reserve, and we will eventually
   //   be able to use the entire reserve
   budget_u128 += ((uint64_t(1) << GRAPHENE_CORE_ASSET_CYCLE_RATE_BITS) - 1);
   budget_u128 >>= GRAPHENE_CORE_ASSET_CYCLE_RATE_BITS;

   try
   {
   share_type budget;
   if( budget_u128 < reserve.value )
      rec.total_budget = share_type(budget_u128.to_uint64());
   else
      rec.total_budget = reserve;
   }
    FC_CAPTURE_AND_RETHROW((budget_u128)(dt)(reserve.value)(rec)(core_dd))

   return;
}

/**
 * Update the budget for witnesses and workers.
 */
void database::process_budget()
{
   try
   {
      const global_property_object& gpo = get_global_properties();
      const dynamic_global_property_object& dpo = get_dynamic_global_properties();
      const asset_dynamic_data_object& core =
         asset_id_type(0)(*this).dynamic_asset_data_id(*this);
      fc::time_point_sec now = head_block_time();

      int64_t time_to_maint = (dpo.next_maintenance_time - now).to_seconds();
      //
      // The code that generates the next maintenance time should
      //    only produce a result in the future.  If this assert
      //    fails, then the next maintenance time algorithm is buggy.
      //
      assert( time_to_maint > 0 );
      //
      // Code for setting chain parameters should validate
      //    block_interval > 0 (as well as the humans proposing /
      //    voting on changes to block interval).
      //
      assert( gpo.parameters.block_interval > 0 );
      uint64_t blocks_to_maint = (uint64_t(time_to_maint) + gpo.parameters.block_interval - 1) / gpo.parameters.block_interval;

      // blocks_to_maint > 0 because time_to_maint > 0,
      // which means numerator is at least equal to block_interval

      budget_record rec;
      initialize_budget_record( now, rec );
      share_type available_funds = rec.total_budget;

      share_type witness_budget = gpo.parameters.witness_pay_per_block.value * blocks_to_maint;
      rec.requested_witness_budget = witness_budget;
      witness_budget = available_funds;//std::min(witness_budget, available_funds);
      rec.witness_budget = witness_budget;

      share_type witness_pay_per_block = witness_budget / blocks_to_maint;
      rec.witness_pay_per_block = witness_pay_per_block;

      available_funds -= witness_budget;

      /*
      fc::uint128_t worker_budget_u128 = gpo.parameters.worker_budget_per_day.value;
      worker_budget_u128 *= uint64_t(time_to_maint);
      worker_budget_u128 /= 60*60*24;

      share_type worker_budget;
      if( worker_budget_u128 >= available_funds.value )
         worker_budget = available_funds;
      else
         worker_budget = worker_budget_u128.to_uint64();
      rec.worker_budget = worker_budget;
      available_funds -= worker_budget;

      share_type leftover_worker_funds = worker_budget;
      pay_workers(leftover_worker_funds);
      rec.leftover_worker_funds = leftover_worker_funds;
      available_funds += leftover_worker_funds;
      */

      rec.supply_delta = rec.witness_budget
         + rec.worker_budget
         - rec.leftover_worker_funds
         - rec.from_accumulated_fees
         - rec.from_unused_witness_budget;

      modify(core, [&]( asset_dynamic_data_object& _core )
      {
         _core.current_supply = (_core.current_supply + rec.supply_delta );

         assert( rec.supply_delta ==
                                   witness_budget
                                 - _core.accumulated_fees
                                 - dpo.witness_budget
                                );
         _core.accumulated_fees = 0;
      });

      modify(dpo, [&]( dynamic_global_property_object& _dpo )
      {
         // Since initial witness_budget was rolled into
         // available_funds, we replace it with witness_budget
         // instead of adding it.
         _dpo.witness_budget = witness_budget;
         _dpo.witness_pay_per_block = witness_pay_per_block;
         _dpo.last_budget_time = now;
      });

      create< budget_record_object >( [&]( budget_record_object& _rec )
      {
         _rec.time = head_block_time();
         _rec.record = rec;
      });

      // available_funds is money we could spend, but don't want to.
      // we simply let it evaporate back into the reserve.
   }
   FC_CAPTURE_AND_RETHROW()
}

template< typename Visitor >
void visit_special_authorities( const database& db, Visitor visit )
{
   const auto& sa_idx = db.get_index_type< special_authority_index >().indices().get<by_id>();

   for( const special_authority_object& sao : sa_idx )
   {
      const account_object& acct = sao.account(db);
      if( acct.owner_special_authority.which() != special_authority::tag< no_special_authority >::value )
      {
         visit( acct, true, acct.owner_special_authority );
      }
      if( acct.active_special_authority.which() != special_authority::tag< no_special_authority >::value )
      {
         visit( acct, false, acct.active_special_authority );
      }
   }
}

void update_top_n_authorities( database& db )
{
   visit_special_authorities( db,
   [&]( const account_object& acct, bool is_owner, const special_authority& auth )
   {
      if( auth.which() == special_authority::tag< top_holders_special_authority >::value )
      {
         // use index to grab the top N holders of the asset and vote_counter to obtain the weights

         const top_holders_special_authority& tha = auth.get< top_holders_special_authority >();
         vote_counter vc;
         const auto& bal_idx = db.get_index_type< account_balance_index >().indices().get< by_asset_balance >();
         uint8_t num_needed = tha.num_top_holders;
         if( num_needed == 0 )
            return;

         // find accounts
         const auto range = bal_idx.equal_range( boost::make_tuple( tha.asset ) );
         for( const account_balance_object& bal : boost::make_iterator_range( range.first, range.second ) )
         {
             assert( bal.asset_type == tha.asset );
             if( bal.owner == acct.id )
                continue;
             vc.add( bal.owner, bal.balance.value );
             --num_needed;
             if( num_needed == 0 )
                break;
         }

         db.modify( acct, [&]( account_object& a )
         {
            vc.finish( is_owner ? a.owner : a.active );
            if( !vc.is_empty() )
               a.top_n_control_flags |= (is_owner ? account_object::top_n_control_owner : account_object::top_n_control_active);
         } );
      }
   } );
}

void split_fba_balance(
   database& db,
   uint64_t fba_id,
   uint16_t network_pct,
   uint16_t designated_asset_buyback_pct,
   uint16_t designated_asset_issuer_pct
)
{
   FC_ASSERT( uint32_t(network_pct) + uint32_t(designated_asset_buyback_pct) + uint32_t(designated_asset_issuer_pct) == GRAPHENE_100_PERCENT );
   const fba_accumulator_object& fba = fba_accumulator_id_type( fba_id )(db);
   if( fba.accumulated_fba_fees == 0 )
      return;

   const asset_object& core = asset_id_type(0)(db);
   const asset_dynamic_data_object& core_dd = core.dynamic_asset_data_id(db);

   if( !fba.is_configured(db) )
   {
      ilog( "${n} core given to network at block ${b} due to non-configured FBA", ("n", fba.accumulated_fba_fees)("b", db.head_block_time()) );
      db.modify( core_dd, [&]( asset_dynamic_data_object& _core_dd )
      {
         _core_dd.current_supply -= fba.accumulated_fba_fees;
      } );
      db.modify( fba, [&]( fba_accumulator_object& _fba )
      {
         _fba.accumulated_fba_fees = 0;
      } );
      return;
   }

   fc::uint128_t buyback_amount_128 = fba.accumulated_fba_fees.value;
   buyback_amount_128 *= designated_asset_buyback_pct;
   buyback_amount_128 /= GRAPHENE_100_PERCENT;
   share_type buyback_amount = buyback_amount_128.to_uint64();

   fc::uint128_t issuer_amount_128 = fba.accumulated_fba_fees.value;
   issuer_amount_128 *= designated_asset_issuer_pct;
   issuer_amount_128 /= GRAPHENE_100_PERCENT;
   share_type issuer_amount = issuer_amount_128.to_uint64();

   // this assert should never fail
   FC_ASSERT( buyback_amount + issuer_amount <= fba.accumulated_fba_fees );

   share_type network_amount = fba.accumulated_fba_fees - (buyback_amount + issuer_amount);

   const asset_object& designated_asset = (*fba.designated_asset)(db);

   if( network_amount != 0 )
   {
      db.modify( core_dd, [&]( asset_dynamic_data_object& _core_dd )
      {
         _core_dd.current_supply -= network_amount;
      } );
   }

   fba_distribute_operation vop;
   vop.account_id = *designated_asset.buyback_account;
   vop.fba_id = fba.id;
   vop.amount = buyback_amount;
   if( vop.amount != 0 )
   {
      db.adjust_balance( *designated_asset.buyback_account, asset(buyback_amount) );
      db.push_applied_operation(vop);
   }

   vop.account_id = designated_asset.issuer;
   vop.fba_id = fba.id;
   vop.amount = issuer_amount;
   if( vop.amount != 0 )
   {
      db.adjust_balance( designated_asset.issuer, asset(issuer_amount) );
      db.push_applied_operation(vop);
   }

   db.modify( fba, [&]( fba_accumulator_object& _fba )
   {
      _fba.accumulated_fba_fees = 0;
   } );
}

void distribute_fba_balances( database& db )
{
   split_fba_balance( db, fba_accumulator_id_transfer_to_blind  , 20*GRAPHENE_1_PERCENT, 60*GRAPHENE_1_PERCENT, 20*GRAPHENE_1_PERCENT );
   split_fba_balance( db, fba_accumulator_id_blind_transfer     , 20*GRAPHENE_1_PERCENT, 60*GRAPHENE_1_PERCENT, 20*GRAPHENE_1_PERCENT );
   split_fba_balance( db, fba_accumulator_id_transfer_from_blind, 20*GRAPHENE_1_PERCENT, 60*GRAPHENE_1_PERCENT, 20*GRAPHENE_1_PERCENT );
}

void create_buyback_orders( database& db )
{
   const auto& bbo_idx = db.get_index_type< buyback_index >().indices().get<by_id>();
   const auto& bal_idx = db.get_index_type< account_balance_index >().indices().get< by_account_asset >();

   for( const buyback_object& bbo : bbo_idx )
   {
      const asset_object& asset_to_buy = bbo.asset_to_buy(db);
      assert( asset_to_buy.buyback_account.valid() );

      const account_object& buyback_account = (*(asset_to_buy.buyback_account))(db);
      asset_id_type next_asset = asset_id_type();

      if( !buyback_account.allowed_assets.valid() )
      {
         wlog( "skipping buyback account ${b} at block ${n} because allowed_assets does not exist", ("b", buyback_account)("n", db.head_block_num()) );
         continue;
      }

      while( true )
      {
         auto it = bal_idx.lower_bound( boost::make_tuple( buyback_account.id, next_asset ) );
         if( it == bal_idx.end() )
            break;
         if( it->owner != buyback_account.id )
            break;
         asset_id_type asset_to_sell = it->asset_type;
         share_type amount_to_sell = it->balance;
         next_asset = asset_to_sell + 1;
         if( asset_to_sell == asset_to_buy.id )
            continue;
         if( amount_to_sell == 0 )
            continue;
         if( buyback_account.allowed_assets->find( asset_to_sell ) == buyback_account.allowed_assets->end() )
         {
            wlog( "buyback account ${b} not selling disallowed holdings of asset ${a} at block ${n}", ("b", buyback_account)("a", asset_to_sell)("n", db.head_block_num()) );
            continue;
         }

         try
         {
            transaction_evaluation_state buyback_context(&db);
            buyback_context.skip_fee_schedule_check = true;

            limit_order_create_operation create_vop;
            create_vop.fee = asset( 0, asset_id_type() );
            create_vop.seller = buyback_account.id;
            create_vop.amount_to_sell = asset( amount_to_sell, asset_to_sell );
            create_vop.min_to_receive = asset( 1, asset_to_buy.id );
            create_vop.expiration = time_point_sec::maximum();
            create_vop.fill_or_kill = false;

            limit_order_id_type order_id = db.apply_operation( buyback_context, create_vop ).get< object_id_type >();

            if( db.find( order_id ) != nullptr )
            {
               limit_order_cancel_operation cancel_vop;
               cancel_vop.fee = asset( 0, asset_id_type() );
               cancel_vop.order = order_id;
               cancel_vop.fee_paying_account = buyback_account.id;

               db.apply_operation( buyback_context, cancel_vop );
            }
         }
         catch( const fc::exception& e )
         {
            // we can in fact get here, e.g. if asset issuer of buy/sell asset blacklists/whitelists the buyback account
            wlog( "Skipping buyback processing selling ${as} for ${ab} for buyback account ${b} at block ${n}; exception was ${e}",
                  ("as", asset_to_sell)("ab", asset_to_buy)("b", buyback_account)("n", db.head_block_num())("e", e.to_detail_string()) );
            continue;
         }
      }
   }
   return;
}

void database::perform_chain_maintenance(const signed_block& next_block, const global_property_object& global_props)
{
   const auto& gpo = get_global_properties();

   distribute_fba_balances(*this);
   create_buyback_orders(*this);

   struct vote_tally_helper {
      database& d;
      const global_property_object& props;

      vote_tally_helper(database& d, const global_property_object& gpo)
         : d(d), props(gpo)
      {
         d._vote_tally_buffer.resize(props.next_available_vote_id);
         d._witness_count_histogram_buffer.resize(props.parameters.maximum_witness_count / 2 + 1);
         d._committee_count_histogram_buffer.resize(props.parameters.maximum_committee_count / 2 + 1);
         d._total_voting_stake = 0;
      }

      void operator()(const account_object& stake_account) {
         if( props.parameters.count_non_member_votes || stake_account.is_member(d.head_block_time()) )
         {
            // There may be a difference between the account whose stake is voting and the one specifying opinions.
            // Usually they're the same, but if the stake account has specified a voting_account, that account is the one
            // specifying the opinions.
            const account_object& opinion_account =
                  (stake_account.options.voting_account ==
                   GRAPHENE_PROXY_TO_SELF_ACCOUNT)? stake_account
                                     : d.get(stake_account.options.voting_account);

            const auto& stats = stake_account.statistics(d);
            uint64_t voting_stake = stats.total_core_in_orders.value
                  + (stake_account.cashback_vb.valid() ? (*stake_account.cashback_vb)(d).balance.amount.value: 0)
                  + d.get_balance(stake_account.get_id(), asset_id_type()).amount.value;

            for( vote_id_type id : opinion_account.options.votes )
            {
               uint32_t offset = id.instance();
               // if they somehow managed to specify an illegal offset, ignore it.
               if( offset < d._vote_tally_buffer.size() )
                  d._vote_tally_buffer[offset] += voting_stake;
            }

            if( opinion_account.options.num_witness <= props.parameters.maximum_witness_count )
            {
               uint16_t offset = std::min(size_t(opinion_account.options.num_witness/2),
                                          d._witness_count_histogram_buffer.size() - 1);
               // votes for a number greater than maximum_witness_count
               // are turned into votes for maximum_witness_count.
               //
               // in particular, this takes care of the case where a
               // member was voting for a high number, then the
               // parameter was lowered.
               d._witness_count_histogram_buffer[offset] += voting_stake;
            }
            if( opinion_account.options.num_committee <= props.parameters.maximum_committee_count )
            {
               uint16_t offset = std::min(size_t(opinion_account.options.num_committee/2),
                                          d._committee_count_histogram_buffer.size() - 1);
               // votes for a number greater than maximum_committee_count
               // are turned into votes for maximum_committee_count.
               //
               // same rationale as for witnesses
               d._committee_count_histogram_buffer[offset] += voting_stake;
            }

            d._total_voting_stake += voting_stake;
         }
      }
   } tally_helper(*this, gpo);
   struct process_fees_helper {
      database& d;
      const global_property_object& props;

      process_fees_helper(database& d, const global_property_object& gpo)
         : d(d), props(gpo) {}

      void operator()(const account_object& a) {
         a.statistics(d).process_fees(a, d);
      }
   } fee_helper(*this, gpo);

   perform_account_maintenance(std::tie(
      tally_helper,
      fee_helper
      ));

   struct clear_canary {
      clear_canary(vector<uint64_t>& target): target(target){}
      ~clear_canary() { target.clear(); }
   private:
      vector<uint64_t>& target;
   };
   clear_canary a(_witness_count_histogram_buffer),
                b(_committee_count_histogram_buffer),
                c(_vote_tally_buffer);

   update_top_n_authorities(*this);
   update_active_witnesses();
   update_active_committee_members();

   modify(gpo, [this](global_property_object& p) {
      // Remove scaling of account registration fee
      const auto& dgpo = get_dynamic_global_properties();
      p.parameters.current_fees->get<account_create_operation>().basic_fee >>= p.parameters.account_fee_scale_bitshifts *
            (dgpo.accounts_registered_this_interval / p.parameters.accounts_per_fee_scale);

      if( p.pending_parameters )
      {
         p.parameters = std::move(*p.pending_parameters);
         p.pending_parameters.reset();
      }
   });

   auto next_maintenance_time = get<dynamic_global_property_object>(dynamic_global_property_id_type()).next_maintenance_time;
   auto maintenance_interval = gpo.parameters.maintenance_interval;

   if( next_maintenance_time <= next_block.timestamp )
   {
      if( next_block.block_num() == 1 )
         next_maintenance_time = time_point_sec() +
               (((next_block.timestamp.sec_since_epoch() / maintenance_interval) + 1) * maintenance_interval);
      else
      {
         // We want to find the smallest k such that next_maintenance_time + k * maintenance_interval > head_block_time()
         //  This implies k > ( head_block_time() - next_maintenance_time ) / maintenance_interval
         //
         // Let y be the right-hand side of this inequality, i.e.
         // y = ( head_block_time() - next_maintenance_time ) / maintenance_interval
         //
         // and let the fractional part f be y-floor(y).  Clearly 0 <= f < 1.
         // We can rewrite f = y-floor(y) as floor(y) = y-f.
         //
         // Clearly k = floor(y)+1 has k > y as desired.  Now we must
         // show that this is the least such k, i.e. k-1 <= y.
         //
         // But k-1 = floor(y)+1-1 = floor(y) = y-f <= y.
         // So this k suffices.
         //
         auto y = (head_block_time() - next_maintenance_time).to_seconds() / maintenance_interval;
         next_maintenance_time += (y+1) * maintenance_interval;
      }
   }

   const dynamic_global_property_object& dgpo = get_dynamic_global_properties();

   // deprecated in cpp-play2
   //if( (dgpo.next_maintenance_time < HARDFORK_613_TIME) && (next_maintenance_time >= HARDFORK_613_TIME) )
   //  deprecate_annual_members(*this);

   modify(dgpo, [next_maintenance_time](dynamic_global_property_object& d) {
      d.next_maintenance_time = next_maintenance_time;
      d.accounts_registered_this_interval = 0;
   });

   // Reset all BitAsset force settlement volumes to zero
   for( const asset_bitasset_data_object* d : get_index_type<asset_bitasset_data_index>() )
      modify(*d, [](asset_bitasset_data_object& d) { d.force_settled_volume = 0; });

   // process_budget needs to run at the bottom because
   //   it needs to know the next_maintenance_time
   process_budget();
}

} }
