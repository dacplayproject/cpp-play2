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

#include <boost/test/unit_test.hpp>

#include <graphene/chain/database.hpp>
#include <graphene/chain/exceptions.hpp>
#include <graphene/chain/hardfork.hpp>

#include <graphene/chain/asset_object.hpp>
#include <graphene/chain/account_object.hpp>
#include <graphene/chain/balance_object.hpp>
#include <graphene/chain/budget_record_object.hpp>
#include <graphene/chain/committee_member_object.hpp>
#include <graphene/chain/market_object.hpp>
#include <graphene/chain/vesting_balance_object.hpp>
#include <graphene/chain/withdraw_permission_object.hpp>
#include <graphene/chain/witness_object.hpp>

#include <graphene/utilities/tempdir.hpp>

#include <fc/crypto/digest.hpp>

#include "../common/database_fixture.hpp"

using namespace graphene::chain;
using namespace graphene::chain::test;

BOOST_FIXTURE_TEST_SUITE( operation_tests, database_fixture )

BOOST_AUTO_TEST_CASE( withdraw_permission_create )
{ try {
   auto nathan_private_key = generate_private_key("nathan");
   auto dan_private_key = generate_private_key("dan");
   account_id_type nathan_id = create_account("nathan", nathan_private_key.get_public_key()).id;
   account_id_type dan_id = create_account("dan", dan_private_key.get_public_key()).id;

   transfer(account_id_type(), nathan_id, asset(1000));
   generate_block();
   set_expiration( db, trx );

   {
      withdraw_permission_create_operation op;
      op.authorized_account = dan_id;
      op.withdraw_from_account = nathan_id;
      op.withdrawal_limit = asset(5);
      op.withdrawal_period_sec = fc::hours(1).to_seconds();
      op.periods_until_expiration = 5;
      op.period_start_time = db.head_block_time() + db.get_global_properties().parameters.block_interval*5;
      trx.operations.push_back(op);
      REQUIRE_OP_VALIDATION_FAILURE(op, withdrawal_limit, asset());
      REQUIRE_OP_VALIDATION_FAILURE(op, periods_until_expiration, 0);
      REQUIRE_OP_VALIDATION_FAILURE(op, withdraw_from_account, dan_id);
      REQUIRE_OP_VALIDATION_FAILURE(op, withdrawal_period_sec, 0);
      REQUIRE_THROW_WITH_VALUE(op, withdrawal_limit, asset(10, asset_id_type(10)));
      REQUIRE_THROW_WITH_VALUE(op, authorized_account, account_id_type(1000));
      REQUIRE_THROW_WITH_VALUE(op, period_start_time, fc::time_point_sec(10000));
      REQUIRE_THROW_WITH_VALUE(op, withdrawal_period_sec, 1);
      trx.operations.back() = op;
   }
   sign( trx, nathan_private_key );
   db.push_transaction( trx );
   trx.clear();
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( withdraw_permission_test )
{ try {
   INVOKE(withdraw_permission_create);

   auto nathan_private_key = generate_private_key("nathan");
   auto dan_private_key = generate_private_key("dan");
   account_id_type nathan_id = get_account("nathan").id;
   account_id_type dan_id = get_account("dan").id;
   withdraw_permission_id_type permit;
   set_expiration( db, trx );

   fc::time_point_sec first_start_time;
   {
      const withdraw_permission_object& permit_object = permit(db);
      BOOST_CHECK(permit_object.authorized_account == dan_id);
      BOOST_CHECK(permit_object.withdraw_from_account == nathan_id);
      BOOST_CHECK(permit_object.period_start_time > db.head_block_time());
      first_start_time = permit_object.period_start_time;
      BOOST_CHECK(permit_object.withdrawal_limit == asset(5));
      BOOST_CHECK(permit_object.withdrawal_period_sec == fc::hours(1).to_seconds());
      BOOST_CHECK(permit_object.expiration == first_start_time + permit_object.withdrawal_period_sec*5 );
   }

   generate_blocks(2);

   {
      withdraw_permission_claim_operation op;
      op.withdraw_permission = permit;
      op.withdraw_from_account = nathan_id;
      op.withdraw_to_account = dan_id;
      op.amount_to_withdraw = asset(1);
      set_expiration( db, trx );

      trx.operations.push_back(op);
      //Throws because we haven't entered the first withdrawal period yet.
      GRAPHENE_REQUIRE_THROW(PUSH_TX( db, trx ), fc::exception);
      //Get to the actual withdrawal period
      generate_blocks(permit(db).period_start_time);

      REQUIRE_THROW_WITH_VALUE(op, withdraw_permission, withdraw_permission_id_type(5));
      REQUIRE_THROW_WITH_VALUE(op, withdraw_from_account, dan_id);
      REQUIRE_THROW_WITH_VALUE(op, withdraw_from_account, account_id_type());
      REQUIRE_THROW_WITH_VALUE(op, withdraw_to_account, nathan_id);
      REQUIRE_THROW_WITH_VALUE(op, withdraw_to_account, account_id_type());
      REQUIRE_THROW_WITH_VALUE(op, amount_to_withdraw, asset(10));
      REQUIRE_THROW_WITH_VALUE(op, amount_to_withdraw, asset(6));
      set_expiration( db, trx );
      trx.clear();
      trx.operations.push_back(op);
      sign( trx, dan_private_key );
      PUSH_TX( db, trx );

      // would be legal on its own, but doesn't work because trx already withdrew
      REQUIRE_THROW_WITH_VALUE(op, amount_to_withdraw, asset(5));

      // Make sure we can withdraw again this period, as long as we're not exceeding the periodic limit
      trx.clear();
      // withdraw 1
      trx.operations = {op};
      // make it different from previous trx so it's non-duplicate
      trx.expiration += fc::seconds(1);
      sign( trx, dan_private_key );
      PUSH_TX( db, trx );
      trx.clear();
   }

   BOOST_CHECK_EQUAL(get_balance(nathan_id, asset_id_type()), 998);
   BOOST_CHECK_EQUAL(get_balance(dan_id, asset_id_type()), 2);

   {
      const withdraw_permission_object& permit_object = permit(db);
      BOOST_CHECK(permit_object.authorized_account == dan_id);
      BOOST_CHECK(permit_object.withdraw_from_account == nathan_id);
      BOOST_CHECK(permit_object.period_start_time == first_start_time);
      BOOST_CHECK(permit_object.withdrawal_limit == asset(5));
      BOOST_CHECK(permit_object.withdrawal_period_sec == fc::hours(1).to_seconds());
      BOOST_CHECK_EQUAL(permit_object.claimed_this_period.value, 2 );
      BOOST_CHECK(permit_object.expiration == first_start_time + 5*permit_object.withdrawal_period_sec);
      generate_blocks(first_start_time + permit_object.withdrawal_period_sec);
      // lazy update:  verify period_start_time isn't updated until new trx occurs
      BOOST_CHECK(permit_object.period_start_time == first_start_time);
   }

   {
      transfer(nathan_id, dan_id, asset(997));
      withdraw_permission_claim_operation op;
      op.withdraw_permission = permit;
      op.withdraw_from_account = nathan_id;
      op.withdraw_to_account = dan_id;
      op.amount_to_withdraw = asset(5);
      trx.operations.push_back(op);
      set_expiration( db, trx );
      sign( trx, dan_private_key );
      //Throws because nathan doesn't have the money
      GRAPHENE_CHECK_THROW(PUSH_TX( db, trx ), fc::exception);
      op.amount_to_withdraw = asset(1);
      trx.clear();
      trx.operations = {op};
      set_expiration( db, trx );
      sign( trx, dan_private_key );
      PUSH_TX( db, trx );
   }

   BOOST_CHECK_EQUAL(get_balance(nathan_id, asset_id_type()), 0);
   BOOST_CHECK_EQUAL(get_balance(dan_id, asset_id_type()), 1000);
   trx.clear();
   transfer(dan_id, nathan_id, asset(1000));

   {
      const withdraw_permission_object& permit_object = permit(db);
      BOOST_CHECK(permit_object.authorized_account == dan_id);
      BOOST_CHECK(permit_object.withdraw_from_account == nathan_id);
      BOOST_CHECK(permit_object.period_start_time == first_start_time + permit_object.withdrawal_period_sec);
      BOOST_CHECK(permit_object.expiration == first_start_time + 5*permit_object.withdrawal_period_sec);
      BOOST_CHECK(permit_object.withdrawal_limit == asset(5));
      BOOST_CHECK(permit_object.withdrawal_period_sec == fc::hours(1).to_seconds());
      generate_blocks(permit_object.expiration);
   }
   // Ensure the permit object has been garbage collected
   BOOST_CHECK(db.find_object(permit) == nullptr);

   {
      withdraw_permission_claim_operation op;
      op.withdraw_permission = permit;
      op.withdraw_from_account = nathan_id;
      op.withdraw_to_account = dan_id;
      op.amount_to_withdraw = asset(5);
      trx.operations.push_back(op);
      set_expiration( db, trx );
      sign( trx, dan_private_key );
      //Throws because the permission has expired
      GRAPHENE_CHECK_THROW(PUSH_TX( db, trx ), fc::exception);
   }
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( withdraw_permission_nominal_case )
{ try {
   INVOKE(withdraw_permission_create);

   auto nathan_private_key = generate_private_key("nathan");
   auto dan_private_key = generate_private_key("dan");
   account_id_type nathan_id = get_account("nathan").id;
   account_id_type dan_id = get_account("dan").id;
   withdraw_permission_id_type permit;

   while(true)
   {
      const withdraw_permission_object& permit_object = permit(db);
      //wdump( (permit_object) );
      withdraw_permission_claim_operation op;
      op.withdraw_permission = permit;
      op.withdraw_from_account = nathan_id;
      op.withdraw_to_account = dan_id;
      op.amount_to_withdraw = asset(5);
      trx.operations.push_back(op);
      set_expiration( db, trx );
      sign( trx, dan_private_key );
      PUSH_TX( db, trx );
      // tx's involving withdraw_permissions can't delete it even
      // if no further withdrawals are possible
      BOOST_CHECK(db.find_object(permit) != nullptr);
      BOOST_CHECK( permit_object.claimed_this_period == 5 );
      trx.clear();
      generate_blocks(
           permit_object.period_start_time
         + permit_object.withdrawal_period_sec );
      if( db.find_object(permit) == nullptr )
         break;
   }

   BOOST_CHECK_EQUAL(get_balance(nathan_id, asset_id_type()), 975);
   BOOST_CHECK_EQUAL(get_balance(dan_id, asset_id_type()), 25);
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( withdraw_permission_update )
{ try {
   INVOKE(withdraw_permission_create);

   auto nathan_private_key = generate_private_key("nathan");
   account_id_type nathan_id = get_account("nathan").id;
   account_id_type dan_id = get_account("dan").id;
   withdraw_permission_id_type permit;
   set_expiration( db, trx );

   {
      withdraw_permission_update_operation op;
      op.permission_to_update = permit;
      op.authorized_account = dan_id;
      op.withdraw_from_account = nathan_id;
      op.periods_until_expiration = 2;
      op.period_start_time = db.head_block_time() + 10;
      op.withdrawal_period_sec = 10;
      op.withdrawal_limit = asset(12);
      trx.operations.push_back(op);
      REQUIRE_THROW_WITH_VALUE(op, periods_until_expiration, 0);
      REQUIRE_THROW_WITH_VALUE(op, withdrawal_period_sec, 0);
      REQUIRE_THROW_WITH_VALUE(op, withdrawal_limit, asset(1, asset_id_type(12)));
      REQUIRE_THROW_WITH_VALUE(op, withdrawal_limit, asset(0));
      REQUIRE_THROW_WITH_VALUE(op, withdraw_from_account, account_id_type(0));
      REQUIRE_THROW_WITH_VALUE(op, authorized_account, account_id_type(0));
      REQUIRE_THROW_WITH_VALUE(op, period_start_time, db.head_block_time() - 50);
      trx.operations.back() = op;
      sign( trx, nathan_private_key );
      PUSH_TX( db, trx );
   }

   {
      const withdraw_permission_object& permit_object = db.get(permit);
      BOOST_CHECK(permit_object.authorized_account == dan_id);
      BOOST_CHECK(permit_object.withdraw_from_account == nathan_id);
      BOOST_CHECK(permit_object.period_start_time == db.head_block_time() + 10);
      BOOST_CHECK(permit_object.withdrawal_limit == asset(12));
      BOOST_CHECK(permit_object.withdrawal_period_sec == 10);
      // BOOST_CHECK(permit_object.remaining_periods == 2);
   }
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( withdraw_permission_delete )
{ try {
   INVOKE(withdraw_permission_update);

   withdraw_permission_delete_operation op;
   op.authorized_account = get_account("dan").id;
   op.withdraw_from_account = get_account("nathan").id;
   set_expiration( db, trx );
   trx.operations.push_back(op);
   sign( trx, generate_private_key("nathan" ));
   PUSH_TX( db, trx );
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( mia_feeds )
{ try {
   ACTORS((nathan)(dan)(ben)(vikram));
   asset_id_type bit_usd_id = create_bitasset("USDBIT").id;

   {
      asset_update_operation op;
      const asset_object& obj = bit_usd_id(db);
      op.asset_to_update = bit_usd_id;
      op.issuer = obj.issuer;
      op.new_issuer = nathan_id;
      op.new_options = obj.options;
      op.new_options.flags &= ~witness_fed_asset;
      trx.operations.push_back(op);
      PUSH_TX( db, trx, ~0 );
      generate_block();
      trx.clear();
   }
   {
      asset_update_feed_producers_operation op;
      op.asset_to_update = bit_usd_id;
      op.issuer = nathan_id;
      op.new_feed_producers = {dan_id, ben_id, vikram_id};
      trx.operations.push_back(op);
      sign( trx, nathan_private_key );
      PUSH_TX( db, trx );
      generate_block(database::skip_nothing);
   }
   {
      const asset_bitasset_data_object& obj = bit_usd_id(db).bitasset_data(db);
      BOOST_CHECK_EQUAL(obj.feeds.size(), 3);
      BOOST_CHECK(obj.current_feed == price_feed());
   }
   {
      const asset_object& bit_usd = bit_usd_id(db);
      asset_publish_feed_operation op;
      op.publisher = vikram_id;
      op.asset_id = bit_usd_id;
      op.feed.settlement_price = op.feed.core_exchange_rate = ~price(asset(GRAPHENE_BLOCKCHAIN_PRECISION),bit_usd.amount(30));

      // We'll expire margins after a month
      // Accept defaults for required collateral
      trx.operations.emplace_back(op);
      PUSH_TX( db, trx, ~0 );

      const asset_bitasset_data_object& bitasset = bit_usd.bitasset_data(db);
      BOOST_CHECK(bitasset.current_feed.settlement_price.to_real() == 30.0 / GRAPHENE_BLOCKCHAIN_PRECISION);
      BOOST_CHECK(bitasset.current_feed.maintenance_collateral_ratio == GRAPHENE_DEFAULT_MAINTENANCE_COLLATERAL_RATIO);

      op.publisher = ben_id;
      op.feed.settlement_price = op.feed.core_exchange_rate = ~price(asset(GRAPHENE_BLOCKCHAIN_PRECISION),bit_usd.amount(25));
      trx.operations.back() = op;
      PUSH_TX( db, trx, ~0 );

      BOOST_CHECK_EQUAL(bitasset.current_feed.settlement_price.to_real(), 30.0 / GRAPHENE_BLOCKCHAIN_PRECISION);
      BOOST_CHECK(bitasset.current_feed.maintenance_collateral_ratio == GRAPHENE_DEFAULT_MAINTENANCE_COLLATERAL_RATIO);

      op.publisher = dan_id;
      op.feed.settlement_price = op.feed.core_exchange_rate = ~price(asset(GRAPHENE_BLOCKCHAIN_PRECISION),bit_usd.amount(40));
      op.feed.maximum_short_squeeze_ratio = 1001;
      op.feed.maintenance_collateral_ratio = 1001;
      trx.operations.back() = op;
      PUSH_TX( db, trx, ~0 );

      BOOST_CHECK_EQUAL(bitasset.current_feed.settlement_price.to_real(), 30.0 / GRAPHENE_BLOCKCHAIN_PRECISION);
      BOOST_CHECK(bitasset.current_feed.maintenance_collateral_ratio == GRAPHENE_DEFAULT_MAINTENANCE_COLLATERAL_RATIO);

      op.publisher = nathan_id;
      trx.operations.back() = op;
      GRAPHENE_CHECK_THROW(PUSH_TX( db, trx, ~0 ), fc::exception);
   }
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( feed_limit_test )
{ try {
   INVOKE( mia_feeds );
   const asset_object& bit_usd = get_asset("USDBIT");
   const asset_bitasset_data_object& bitasset = bit_usd.bitasset_data(db);
   GET_ACTOR(nathan);

   BOOST_CHECK(!bitasset.current_feed.settlement_price.is_null());

   BOOST_TEST_MESSAGE("Setting minimum feeds to 4");
   asset_update_bitasset_operation op;
   op.new_options.minimum_feeds = 4;
   op.asset_to_update = bit_usd.get_id();
   op.issuer = bit_usd.issuer;
   trx.operations = {op};
   sign( trx, nathan_private_key );
   db.push_transaction(trx);

   BOOST_TEST_MESSAGE("Checking current_feed is null");
   BOOST_CHECK(bitasset.current_feed.settlement_price.is_null());

   BOOST_TEST_MESSAGE("Setting minimum feeds to 3");
   op.new_options.minimum_feeds = 3;
   trx.clear();
   trx.operations = {op};
   sign( trx, nathan_private_key );
   db.push_transaction(trx);

   BOOST_TEST_MESSAGE("Checking current_feed is not null");
   BOOST_CHECK(!bitasset.current_feed.settlement_price.is_null());
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( witness_create )
{ try {
   ACTOR(nathan);
   upgrade_to_lifetime_member(nathan_id);
   trx.clear();
   witness_id_type nathan_witness_id = create_witness(nathan_id, nathan_private_key).id;
   // Give nathan some voting stake
   transfer(committee_account, nathan_id, asset(10000000));
   generate_block();
   set_expiration( db, trx );

   {
      account_update_operation op;
      op.account = nathan_id;
      op.new_options = nathan_id(db).options;
      op.new_options->votes.insert(nathan_witness_id(db).vote_id);
      op.new_options->num_witness = std::count_if(op.new_options->votes.begin(), op.new_options->votes.end(),
                                                  [](vote_id_type id) { return id.type() == vote_id_type::witness; });
      op.new_options->num_committee = std::count_if(op.new_options->votes.begin(), op.new_options->votes.end(),
                                                    [](vote_id_type id) { return id.type() == vote_id_type::committee; });
      trx.operations.push_back(op);
      sign( trx, nathan_private_key );
      PUSH_TX( db, trx );
      trx.clear();
   }

   generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);
   const auto& witnesses = db.get_global_properties().active_witnesses;

   // make sure we're in active_witnesses
   auto itr = std::find(witnesses.begin(), witnesses.end(), nathan_witness_id);
   BOOST_CHECK(itr != witnesses.end());

   // generate blocks until we are at the beginning of a round
   while( ((db.get_dynamic_global_properties().current_aslot + 1) % witnesses.size()) != 0 )
      generate_block();

   int produced = 0;
   // Make sure we get scheduled at least once in witnesses.size()*2 blocks
   // may take this many unless we measure where in the scheduling round we are
   // TODO:  intense_test that repeats this loop many times
   for( size_t i=0, n=witnesses.size()*2; i<n; i++ )
   {
      signed_block block = generate_block();
      if( block.witness == nathan_witness_id )
         produced++;
   }
   BOOST_CHECK_GE( produced, 1 );
} FC_LOG_AND_RETHROW() }

/**
 *  This test should verify that the asset_global_settle operation works as expected,
 *  make sure that global settling cannot be performed by anyone other than the
 *  issuer and only if the global settle bit is set.
 */
BOOST_AUTO_TEST_CASE( global_settle_test )
{
   try {
   ACTORS((nathan)(ben)(valentine)(dan));
   asset_id_type bit_usd_id = create_bitasset("USDBIT", nathan_id, 100, global_settle | charge_market_fee).get_id();

   update_feed_producers( bit_usd_id(db), { nathan_id } );

   price_feed feed;
   feed.settlement_price = price( asset( 1000, bit_usd_id ), asset( 500 ) );
   feed.maintenance_collateral_ratio = 175 * GRAPHENE_COLLATERAL_RATIO_DENOM / 100;
   feed.maximum_short_squeeze_ratio = 150 * GRAPHENE_COLLATERAL_RATIO_DENOM / 100;
   publish_feed( bit_usd_id(db), nathan, feed );

   transfer(committee_account, ben_id, asset(10000));
   transfer(committee_account, valentine_id, asset(10000));
   transfer(committee_account, dan_id, asset(10000));
   borrow(ben, asset(1000, bit_usd_id), asset(1000));
   BOOST_CHECK_EQUAL(get_balance(ben_id, bit_usd_id), 1000);
   BOOST_CHECK_EQUAL(get_balance(ben_id, asset_id_type()), 9000);

   create_sell_order(ben_id, asset(1000, bit_usd_id), asset(1000));
   BOOST_CHECK_EQUAL(get_balance(ben_id, bit_usd_id), 0);
   BOOST_CHECK_EQUAL(get_balance(ben_id, asset_id_type()), 9000);

   create_sell_order(valentine_id, asset(1000), asset(1000, bit_usd_id));
   BOOST_CHECK_EQUAL(get_balance(ben_id, bit_usd_id), 0);
   BOOST_CHECK_EQUAL(get_balance(ben_id, asset_id_type()), 10000);
   BOOST_CHECK_EQUAL(get_balance(valentine_id, bit_usd_id), 990);
   BOOST_CHECK_EQUAL(get_balance(valentine_id, asset_id_type()), 9000);

   borrow(valentine, asset(500, bit_usd_id), asset(600));
   BOOST_CHECK_EQUAL(get_balance(valentine_id, bit_usd_id), 1490);
   BOOST_CHECK_EQUAL(get_balance(valentine_id, asset_id_type()), 8400);

   create_sell_order(valentine_id, asset(500, bit_usd_id), asset(600));
   BOOST_CHECK_EQUAL(get_balance(valentine_id, bit_usd_id), 990);
   BOOST_CHECK_EQUAL(get_balance(valentine_id, asset_id_type()), 8400);

   create_sell_order(dan_id, asset(600), asset(500, bit_usd_id));
   BOOST_CHECK_EQUAL(get_balance(valentine_id, bit_usd_id), 990);
   BOOST_CHECK_EQUAL(get_balance(valentine_id, asset_id_type()), 9000);
   BOOST_CHECK_EQUAL(get_balance(ben_id, bit_usd_id), 0);
   BOOST_CHECK_EQUAL(get_balance(ben_id, asset_id_type()), 10000);
   BOOST_CHECK_EQUAL(get_balance(dan_id, bit_usd_id), 495);
   BOOST_CHECK_EQUAL(get_balance(dan_id, asset_id_type()), 9400);

   // add some collateral
   borrow(ben, asset(0, bit_usd_id), asset(1000));
   BOOST_CHECK_EQUAL(get_balance(ben_id, asset_id_type()), 9000);

   {
      asset_global_settle_operation op;
      op.asset_to_settle = bit_usd_id;
      op.issuer = nathan_id;
      op.settle_price = ~price(asset(10), asset(11, bit_usd_id));
      trx.clear();
      trx.operations.push_back(op);
      REQUIRE_THROW_WITH_VALUE(op, settle_price, ~price(asset(2001), asset(1000, bit_usd_id)));
      REQUIRE_THROW_WITH_VALUE(op, asset_to_settle, asset_id_type());
      REQUIRE_THROW_WITH_VALUE(op, asset_to_settle, asset_id_type(100));
      REQUIRE_THROW_WITH_VALUE(op, issuer, account_id_type(2));
      trx.operations.back() = op;
      sign( trx, nathan_private_key );
      PUSH_TX( db, trx );
   }

   force_settle(valentine_id(db), asset(990, bit_usd_id));
   force_settle(dan_id(db), asset(495, bit_usd_id));

   BOOST_CHECK_EQUAL(get_balance(valentine_id, bit_usd_id), 0);
   BOOST_CHECK_EQUAL(get_balance(valentine_id, asset_id_type()), 10045);
   BOOST_CHECK_EQUAL(get_balance(ben_id, bit_usd_id), 0);
   BOOST_CHECK_EQUAL(get_balance(ben_id, asset_id_type()), 10091);
   BOOST_CHECK_EQUAL(get_balance(dan_id, bit_usd_id), 0);
   BOOST_CHECK_EQUAL(get_balance(dan_id, asset_id_type()), 9849);
} FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( force_settle_test )
{
   try
   {
      ACTORS( (nathan)(shorter1)(shorter2)(shorter3)(shorter4)(shorter5) );

      int64_t initial_balance = 100000000;

      transfer(account_id_type()(db), shorter1_id(db), asset(initial_balance));
      transfer(account_id_type()(db), shorter2_id(db), asset(initial_balance));
      transfer(account_id_type()(db), shorter3_id(db), asset(initial_balance));
      transfer(account_id_type()(db), shorter4_id(db), asset(initial_balance));
      transfer(account_id_type()(db), shorter5_id(db), asset(initial_balance));

      asset_id_type bitusd_id = create_bitasset(
         "USDBIT",
         nathan_id,
         100,
         disable_force_settle
         ).id;

      asset_id_type core_id = asset_id_type();

      auto update_bitasset_options = [&]( asset_id_type asset_id,
         std::function< void(bitasset_options&) > update_function )
      {
         const asset_object& _asset = asset_id(db);
         asset_update_bitasset_operation op;
         op.asset_to_update = asset_id;
         op.issuer = _asset.issuer;
         op.new_options = (*_asset.bitasset_data_id)(db).options;
         update_function( op.new_options );
         signed_transaction tx;
         tx.operations.push_back( op );
         set_expiration( db, tx );
         PUSH_TX( db, tx, ~0 );
      } ;

      auto update_asset_options = [&]( asset_id_type asset_id,
         std::function< void(asset_options&) > update_function )
      {
         const asset_object& _asset = asset_id(db);
         asset_update_operation op;
         op.asset_to_update = asset_id;
         op.issuer = _asset.issuer;
         op.new_options = _asset.options;
         update_function( op.new_options );
         signed_transaction tx;
         tx.operations.push_back( op );
         set_expiration( db, tx );
         PUSH_TX( db, tx, ~0 );
      } ;

      BOOST_TEST_MESSAGE( "Update maximum_force_settlement_volume = 9000" );

      BOOST_CHECK( bitusd_id(db).is_market_issued() );
      update_bitasset_options( bitusd_id, [&]( bitasset_options& new_options )
      { new_options.maximum_force_settlement_volume = 9000; } );

      BOOST_TEST_MESSAGE( "Publish price feed" );

      update_feed_producers( bitusd_id, { nathan_id } );
      {
         price_feed feed;
         feed.settlement_price = price( asset( 1, bitusd_id ), asset( 1, core_id ) );
         publish_feed( bitusd_id, nathan_id, feed );
      }

      BOOST_TEST_MESSAGE( "First short batch" );

      call_order_id_type call1_id = borrow( shorter1_id, asset(1000, bitusd_id), asset(2*1000, core_id) )->id;   // 2.0000
      call_order_id_type call2_id = borrow( shorter2_id, asset(2000, bitusd_id), asset(2*1999, core_id) )->id;   // 1.9990
      call_order_id_type call3_id = borrow( shorter3_id, asset(3000, bitusd_id), asset(2*2890, core_id) )->id;   // 1.9267
      call_order_id_type call4_id = borrow( shorter4_id, asset(4000, bitusd_id), asset(2*3950, core_id) )->id;   // 1.9750
      call_order_id_type call5_id = borrow( shorter5_id, asset(5000, bitusd_id), asset(2*4900, core_id) )->id;   // 1.9600

      transfer( shorter1_id, nathan_id, asset(1000, bitusd_id) );
      transfer( shorter2_id, nathan_id, asset(2000, bitusd_id) );
      transfer( shorter3_id, nathan_id, asset(3000, bitusd_id) );
      transfer( shorter4_id, nathan_id, asset(4000, bitusd_id) );
      transfer( shorter5_id, nathan_id, asset(5000, bitusd_id) );

      BOOST_CHECK_EQUAL( get_balance(nathan_id, bitusd_id), 15000);
      BOOST_CHECK_EQUAL( get_balance(nathan_id, core_id), 0);
      BOOST_CHECK_EQUAL( get_balance(shorter1_id, core_id), initial_balance-2000 );
      BOOST_CHECK_EQUAL( get_balance(shorter2_id, core_id), initial_balance-3998 );
      BOOST_CHECK_EQUAL( get_balance(shorter3_id, core_id), initial_balance-5780 );
      BOOST_CHECK_EQUAL( get_balance(shorter4_id, core_id), initial_balance-7900 );
      BOOST_CHECK_EQUAL( get_balance(shorter5_id, core_id), initial_balance-9800 );

      BOOST_TEST_MESSAGE( "Update force_settlement_delay_sec = 100, force_settlement_offset_percent = 1%" );

      update_bitasset_options( bitusd_id, [&]( bitasset_options& new_options )
      { new_options.force_settlement_delay_sec = 100;
        new_options.force_settlement_offset_percent = GRAPHENE_1_PERCENT; } );

      // Force settlement is disabled; check that it fails
      GRAPHENE_REQUIRE_THROW( force_settle( nathan_id, asset( 50, bitusd_id ) ), fc::exception );

      update_asset_options( bitusd_id, [&]( asset_options& new_options )
      { new_options.flags &= ~disable_force_settle; } );

      // Can't settle more BitUSD than you own
      GRAPHENE_REQUIRE_THROW( force_settle( nathan_id, asset( 999999, bitusd_id ) ), fc::exception );

      // settle3 should be least collateralized order according to index
      BOOST_CHECK( db.get_index_type<call_order_index>().indices().get<by_collateral>().begin()->id == call3_id );
      BOOST_CHECK_EQUAL( call3_id(db).debt.value, 3000 );

      BOOST_TEST_MESSAGE( "Verify partial settlement of call" );
      // Partially settle a call
      force_settlement_id_type settle_id = force_settle( nathan_id, asset( 50, bitusd_id ) ).get< object_id_type >();

      // Call does not take effect immediately
      BOOST_CHECK_EQUAL( get_balance(nathan_id, bitusd_id), 14950);
      BOOST_CHECK_EQUAL( settle_id(db).balance.amount.value, 50);
      BOOST_CHECK_EQUAL( call3_id(db).debt.value, 3000 );
      BOOST_CHECK_EQUAL( call3_id(db).collateral.value, 5780 );
      BOOST_CHECK( settle_id(db).owner == nathan_id );

      // Wait for settlement to take effect
      generate_blocks(settle_id(db).settlement_date);
      BOOST_CHECK(db.find(settle_id) == nullptr);
      BOOST_CHECK_EQUAL( bitusd_id(db).bitasset_data(db).force_settled_volume.value, 50 );
      BOOST_CHECK_EQUAL( get_balance(nathan_id, bitusd_id), 14950);
      BOOST_CHECK_EQUAL( get_balance(nathan_id, core_id), 49 );   // 1% force_settlement_offset_percent (rounded unfavorably)
      BOOST_CHECK_EQUAL( call3_id(db).debt.value, 2950 );
      BOOST_CHECK_EQUAL( call3_id(db).collateral.value, 5731 );  // 5731 == 5780-49

      BOOST_CHECK( db.get_index_type<call_order_index>().indices().get<by_collateral>().begin()->id == call3_id );

      BOOST_TEST_MESSAGE( "Verify pending settlement is cancelled when asset's force_settle is disabled" );
      // Ensure pending settlement is cancelled when force settle is disabled
      settle_id = force_settle( nathan_id, asset( 50, bitusd_id ) ).get< object_id_type >();

      BOOST_CHECK( !db.get_index_type<force_settlement_index>().indices().empty() );
      update_asset_options( bitusd_id, [&]( asset_options& new_options )
      { new_options.flags |= disable_force_settle; } );
      BOOST_CHECK(  db.get_index_type<force_settlement_index>().indices().empty() );
      update_asset_options( bitusd_id, [&]( asset_options& new_options )
      { new_options.flags &= ~disable_force_settle; } );

      BOOST_TEST_MESSAGE( "Perform iterative settlement" );
      settle_id = force_settle( nathan_id, asset( 12500, bitusd_id ) ).get< object_id_type >();

      // c3 2950 : 5731   1.9427   fully settled
      // c5 5000 : 9800   1.9600   fully settled
      // c4 4000 : 7900   1.9750   fully settled
      // c2 2000 : 3998   1.9990   550 settled
      // c1 1000 : 2000   2.0000

      generate_blocks( settle_id(db).settlement_date );

      int64_t call1_payout =                0;
      int64_t call2_payout =       550*99/100;
      int64_t call3_payout = 49 + 2950*99/100;
      int64_t call4_payout =      4000*99/100;
      int64_t call5_payout =      5000*99/100;

      BOOST_CHECK_EQUAL( get_balance(shorter1_id, core_id), initial_balance-2*1000 );  // full collat still tied up
      BOOST_CHECK_EQUAL( get_balance(shorter2_id, core_id), initial_balance-2*1999 );  // full collat still tied up
      BOOST_CHECK_EQUAL( get_balance(shorter3_id, core_id), initial_balance-call3_payout );  // initial balance minus transfer to Nathan (as BitUSD)
      BOOST_CHECK_EQUAL( get_balance(shorter4_id, core_id), initial_balance-call4_payout );  // initial balance minus transfer to Nathan (as BitUSD)
      BOOST_CHECK_EQUAL( get_balance(shorter5_id, core_id), initial_balance-call5_payout );  // initial balance minus transfer to Nathan (as BitUSD)

      BOOST_CHECK_EQUAL( get_balance(nathan_id, core_id),
           call1_payout + call2_payout + call3_payout + call4_payout + call5_payout );

      BOOST_CHECK( db.find(call3_id) == nullptr );
      BOOST_CHECK( db.find(call4_id) == nullptr );
      BOOST_CHECK( db.find(call5_id) == nullptr );

      BOOST_REQUIRE( db.find(call1_id) != nullptr );
      BOOST_REQUIRE( db.find(call2_id) != nullptr );

      BOOST_CHECK_EQUAL( call1_id(db).debt.value, 1000 );
      BOOST_CHECK_EQUAL( call1_id(db).collateral.value, 2000 );

      BOOST_CHECK_EQUAL( call2_id(db).debt.value, 2000-550 );
      BOOST_CHECK_EQUAL( call2_id(db).collateral.value, 3998-call2_payout );
   }
   catch(fc::exception& e)
   {
      edump((e.to_detail_string()));
      throw;
   }
}

BOOST_AUTO_TEST_CASE( assert_op_test )
{
   try {
   // create some objects
   auto nathan_private_key = generate_private_key("nathan");
   public_key_type nathan_public_key = nathan_private_key.get_public_key();
   account_id_type nathan_id = create_account("nathan", nathan_public_key).id;

   assert_operation op;

   // nathan checks that his public key is equal to the given value.
   op.fee_paying_account = nathan_id;
   op.predicates.emplace_back(account_name_eq_lit_predicate{ nathan_id, "nathan" });
   trx.operations.push_back(op);
   sign( trx, nathan_private_key );
   PUSH_TX( db, trx );

   // nathan checks that his public key is not equal to the given value (fail)
   trx.clear();
   op.predicates.emplace_back(account_name_eq_lit_predicate{ nathan_id, "dan" });
   trx.operations.push_back(op);
   sign( trx, nathan_private_key );
   GRAPHENE_CHECK_THROW( PUSH_TX( db, trx ), fc::exception );
   } FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( balance_object_test )
{ try {
   // Intentionally overriding the fixture's db; I need to control genesis on this one.
   database db;
   const uint32_t skip_flags = database::skip_undo_history_check;
   fc::temp_directory td( graphene::utilities::temp_directory_path() );
   genesis_state.initial_balances.push_back({generate_private_key("n").get_public_key(), GRAPHENE_SYMBOL, 1});
   genesis_state.initial_balances.push_back({generate_private_key("x").get_public_key(), GRAPHENE_SYMBOL, 1});
   fc::time_point_sec starting_time = genesis_state.initial_timestamp + 3000;

   auto n_key = generate_private_key("n");
   auto x_key = generate_private_key("x");
   auto v1_key = generate_private_key("v1");
   auto v2_key = generate_private_key("v2");

   genesis_state_type::initial_vesting_balance_type vest;
   vest.owner = v1_key.get_public_key();
   vest.asset_symbol = GRAPHENE_SYMBOL;
   vest.amount = 500;
   vest.begin_balance = vest.amount;
   vest.begin_timestamp = starting_time;
   vest.vesting_duration_seconds = 60;
   genesis_state.initial_vesting_balances.push_back(vest);
   vest.owner = v2_key.get_public_key();
   vest.begin_timestamp -= fc::seconds(30);
   vest.amount = 400;
   genesis_state.initial_vesting_balances.push_back(vest);

   genesis_state.initial_accounts.emplace_back("n", n_key.get_public_key());

   auto _sign = [&]( signed_transaction& tx, const private_key_type& key )
   {  tx.sign( key, db.get_chain_id() );   };

   db.open(td.path(), [this]{return genesis_state;});
   const balance_object& balance = balance_id_type()(db);
   BOOST_CHECK_EQUAL(balance.balance.amount.value, 1);
   BOOST_CHECK_EQUAL(balance_id_type(1)(db).balance.amount.value, 1);

   balance_claim_operation op;
   op.deposit_to_account = db.get_index_type<account_index>().indices().get<by_name>().find("n")->get_id();
   op.total_claimed = asset(1);
   op.balance_to_claim = balance_id_type(1);
   op.balance_owner_key = x_key.get_public_key();
   trx.operations = {op};
   _sign( trx, n_key );
   // Fail because I'm claiming from an address which hasn't signed
   GRAPHENE_CHECK_THROW(db.push_transaction(trx), tx_missing_other_auth);
   trx.clear();
   op.balance_to_claim = balance_id_type();
   op.balance_owner_key = n_key.get_public_key();
   trx.operations = {op};
   _sign( trx, n_key );
   db.push_transaction(trx);

   // Not using fixture's get_balance() here because it uses fixture's db, not my override
   BOOST_CHECK_EQUAL(db.get_balance(op.deposit_to_account, asset_id_type()).amount.value, 1);
   BOOST_CHECK(db.find_object(balance_id_type()) == nullptr);
   BOOST_CHECK(db.find_object(balance_id_type(1)) != nullptr);

   auto slot = db.get_slot_at_time(starting_time);
   db.generate_block(starting_time, db.get_scheduled_witness(slot), init_account_priv_key, skip_flags);
   set_expiration( db, trx );

   const balance_object& vesting_balance_1 = balance_id_type(2)(db);
   const balance_object& vesting_balance_2 = balance_id_type(3)(db);
   BOOST_CHECK(vesting_balance_1.is_vesting_balance());
   BOOST_CHECK_EQUAL(vesting_balance_1.balance.amount.value, 500);
   BOOST_CHECK_EQUAL(vesting_balance_1.available(db.head_block_time()).amount.value, 0);
   BOOST_CHECK(vesting_balance_2.is_vesting_balance());
   BOOST_CHECK_EQUAL(vesting_balance_2.balance.amount.value, 400);
   BOOST_CHECK_EQUAL(vesting_balance_2.available(db.head_block_time()).amount.value, 150);

   op.balance_to_claim = vesting_balance_1.id;
   op.total_claimed = asset(1);
   op.balance_owner_key = v1_key.get_public_key();
   trx.clear();
   trx.operations = {op};
   _sign( trx, n_key );
   _sign( trx, v1_key );
   // Attempting to claim 1 from a balance with 0 available
   GRAPHENE_CHECK_THROW(db.push_transaction(trx), balance_claim_invalid_claim_amount);

   op.balance_to_claim = vesting_balance_2.id;
   op.total_claimed.amount = 151;
   op.balance_owner_key = v2_key.get_public_key();
   trx.operations = {op};
   trx.signatures.clear();
   _sign( trx, n_key );
   _sign( trx, v2_key );
   // Attempting to claim 151 from a balance with 150 available
   GRAPHENE_CHECK_THROW(db.push_transaction(trx), balance_claim_invalid_claim_amount);

   op.balance_to_claim = vesting_balance_2.id;
   op.total_claimed.amount = 100;
   op.balance_owner_key = v2_key.get_public_key();
   trx.operations = {op};
   trx.signatures.clear();
   _sign( trx, n_key );
   _sign( trx, v2_key );
   db.push_transaction(trx);
   BOOST_CHECK_EQUAL(db.get_balance(op.deposit_to_account, asset_id_type()).amount.value, 101);
   BOOST_CHECK_EQUAL(vesting_balance_2.balance.amount.value, 300);

   op.total_claimed.amount = 10;
   trx.operations = {op};
   trx.signatures.clear();
   _sign( trx, n_key );
   _sign( trx, v2_key );
   // Attempting to claim twice within a day
   GRAPHENE_CHECK_THROW(db.push_transaction(trx), balance_claim_claimed_too_often);

   db.generate_block(db.get_slot_time(1), db.get_scheduled_witness(1), init_account_priv_key, skip_flags);
   slot = db.get_slot_at_time(vesting_balance_1.vesting_policy->begin_timestamp + 60);
   db.generate_block(db.get_slot_time(slot), db.get_scheduled_witness(slot), init_account_priv_key, skip_flags);
   set_expiration( db, trx );

   op.balance_to_claim = vesting_balance_1.id;
   op.total_claimed.amount = 500;
   op.balance_owner_key = v1_key.get_public_key();
   trx.operations = {op};
   trx.signatures.clear();
   _sign( trx, n_key );
   _sign( trx, v1_key );
   db.push_transaction(trx);
   BOOST_CHECK(db.find_object(op.balance_to_claim) == nullptr);
   BOOST_CHECK_EQUAL(db.get_balance(op.deposit_to_account, asset_id_type()).amount.value, 601);

   op.balance_to_claim = vesting_balance_2.id;
   op.balance_owner_key = v2_key.get_public_key();
   op.total_claimed.amount = 10;
   trx.operations = {op};
   trx.signatures.clear();
   _sign( trx, n_key );
   _sign( trx, v2_key );
   // Attempting to claim twice within a day
   GRAPHENE_CHECK_THROW(db.push_transaction(trx), balance_claim_claimed_too_often);

   db.generate_block(db.get_slot_time(1), db.get_scheduled_witness(1), init_account_priv_key, skip_flags);
   slot = db.get_slot_at_time(db.head_block_time() + fc::days(1));
   db.generate_block(db.get_slot_time(slot), db.get_scheduled_witness(slot), init_account_priv_key, skip_flags);
   set_expiration( db, trx );

   op.total_claimed = vesting_balance_2.balance;
   trx.operations = {op};
   trx.signatures.clear();
   _sign( trx, n_key );
   _sign( trx, v2_key );
   db.push_transaction(trx);
   BOOST_CHECK(db.find_object(op.balance_to_claim) == nullptr);
   BOOST_CHECK_EQUAL(db.get_balance(op.deposit_to_account, asset_id_type()).amount.value, 901);
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE(transfer_with_memo) {
   try {
      ACTOR(alice);
      ACTOR(bob);
      transfer(account_id_type(), alice_id, asset(1000));
      BOOST_CHECK_EQUAL(get_balance(alice_id, asset_id_type()), 1000);

      transfer_operation op;
      op.from = alice_id;
      op.to = bob_id;
      op.amount = asset(500);
      op.memo = memo_data();
      op.memo->set_message(alice_private_key, bob_public_key, "Dear Bob,\n\nMoney!\n\nLove, Alice");
      trx.operations = {op};
      trx.sign(alice_private_key, db.get_chain_id());
      db.push_transaction(trx);

      BOOST_CHECK_EQUAL(get_balance(alice_id, asset_id_type()), 500);
      BOOST_CHECK_EQUAL(get_balance(bob_id, asset_id_type()), 500);

      auto memo = db.get_recent_transaction(trx.id()).operations.front().get<transfer_operation>().memo;
      BOOST_CHECK(memo);
      BOOST_CHECK_EQUAL(memo->get_message(bob_private_key, alice_public_key), "Dear Bob,\n\nMoney!\n\nLove, Alice");
   } FC_LOG_AND_RETHROW()
}

// TODO:  Write linear VBO tests

BOOST_AUTO_TEST_CASE( top_n_special )
{
   ACTORS( (alice)(bob)(chloe)(dan)(izzy)(stan) );

   try
   {
      {
         //
         // Izzy (issuer)
         // Stan (special authority)
         // Alice, Bob, Chloe, Dan (ABCD)
         //

         asset_id_type topn_id = create_user_issued_asset( "TOPN", izzy_id(db), 0 ).id;
         authority stan_owner_auth = stan_id(db).owner;
         authority stan_active_auth = stan_id(db).active;

         // set SA, wait for maint interval
         // TODO:  account_create_operation
         // TODO:  multiple accounts with different n for same asset

         {
            top_holders_special_authority top2, top3;

            top2.num_top_holders = 2;
            top2.asset = topn_id;

            top3.num_top_holders = 3;
            top3.asset = topn_id;

            account_update_operation op;
            op.account = stan_id;
            op.extensions.value.active_special_authority = top3;
            op.extensions.value.owner_special_authority = top2;

            signed_transaction tx;
            tx.operations.push_back( op );

            set_expiration( db, tx );
            sign( tx, stan_private_key );

            PUSH_TX( db, tx );

            // TODO:  Check special_authority is properly set
            // TODO:  Do it in steps
         }

         // wait for maint interval
         // make sure we don't have any authority as account hasn't gotten distributed yet
         generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);

         BOOST_CHECK( stan_id(db).owner  == stan_owner_auth );
         BOOST_CHECK( stan_id(db).active == stan_active_auth );

         // issue some to Alice, make sure she gets control of Stan

         // we need to set_expiration() before issue_uia() because the latter doens't call it #11
         set_expiration( db, trx );  // #11
         issue_uia( alice_id, asset( 1000, topn_id ) );

         BOOST_CHECK( stan_id(db).owner  == stan_owner_auth );
         BOOST_CHECK( stan_id(db).active == stan_active_auth );

         generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);

         /*  NOTE - this was an old check from an earlier implementation that only allowed SA for LTM's
         // no boost yet, we need to upgrade to LTM before mechanics apply to Stan
         BOOST_CHECK( stan_id(db).owner  == stan_owner_auth );
         BOOST_CHECK( stan_id(db).active == stan_active_auth );

         set_expiration( db, trx );  // #11
         upgrade_to_lifetime_member(stan_id);
         generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);
         */

         BOOST_CHECK( stan_id(db).owner  == authority(  501, alice_id, 1000 ) );
         BOOST_CHECK( stan_id(db).active == authority(  501, alice_id, 1000 ) );

         // give asset to Stan, make sure owner doesn't change at all
         set_expiration( db, trx );  // #11
         transfer( alice_id, stan_id, asset( 1000, topn_id ) );

         generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);

         BOOST_CHECK( stan_id(db).owner  == authority(  501, alice_id, 1000 ) );
         BOOST_CHECK( stan_id(db).active == authority(  501, alice_id, 1000 ) );

         set_expiration( db, trx );  // #11
         issue_uia( chloe_id, asset( 131000, topn_id ) );

         // now Chloe has 131,000 and Stan has 1k.  Make sure change occurs at next maintenance interval.
         // NB, 131072 is a power of 2; the number 131000 was chosen so that we need a bitshift, but
         // if we put the 1000 from Stan's balance back into play, we need a different bitshift.

         // we use Chloe so she can be displaced by Bob later (showing the tiebreaking logic).

         // Check Alice is still in control, because we're deferred to next maintenance interval
         BOOST_CHECK( stan_id(db).owner  == authority(  501, alice_id, 1000 ) );
         BOOST_CHECK( stan_id(db).active == authority(  501, alice_id, 1000 ) );

         generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);

         BOOST_CHECK( stan_id(db).owner  == authority( 32751, chloe_id, 65500 ) );
         BOOST_CHECK( stan_id(db).active == authority( 32751, chloe_id, 65500 ) );

         // put Alice's stake back in play
         set_expiration( db, trx );  // #11
         transfer( stan_id, alice_id, asset( 1000, topn_id ) );

         generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);

         BOOST_CHECK( stan_id(db).owner  == authority( 33001, alice_id, 500, chloe_id, 65500 ) );
         BOOST_CHECK( stan_id(db).active == authority( 33001, alice_id, 500, chloe_id, 65500 ) );

         // issue 200,000 to Dan to cause another bitshift.
         set_expiration( db, trx );  // #11
         issue_uia( dan_id, asset( 200000, topn_id ) );
         generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);

         // 200000 Dan
         // 131000 Chloe
         // 1000 Alice

         BOOST_CHECK( stan_id(db).owner  == authority( 41376,                chloe_id, 32750, dan_id, 50000 ) );
         BOOST_CHECK( stan_id(db).active == authority( 41501, alice_id, 250, chloe_id, 32750, dan_id, 50000 ) );

         // have Alice send all but 1 back to Stan, verify that we clamp Alice at one vote
         set_expiration( db, trx );  // #11
         transfer( alice_id, stan_id, asset( 999, topn_id ) );
         generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);

         BOOST_CHECK( stan_id(db).owner  == authority( 41376,                chloe_id, 32750, dan_id, 50000 ) );
         BOOST_CHECK( stan_id(db).active == authority( 41376, alice_id,   1, chloe_id, 32750, dan_id, 50000 ) );

         // send 131k to Bob so he's tied with Chloe, verify he displaces Chloe in top2
         set_expiration( db, trx );  // #11
         issue_uia( bob_id, asset( 131000, topn_id ) );
         generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);

         BOOST_CHECK( stan_id(db).owner  == authority( 41376, bob_id, 32750,                  dan_id, 50000 ) );
         BOOST_CHECK( stan_id(db).active == authority( 57751, bob_id, 32750, chloe_id, 32750, dan_id, 50000 ) );

         // TODO more rounding checks
      }

   } FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( buyback )
{
   ACTORS( (alice)(bob)(chloe)(dan)(izzy)(philbin) );
   upgrade_to_lifetime_member(philbin_id);

   try
   {
      {
         //
         // Izzy (issuer)
         // Alice, Bob, Chloe, Dan (ABCD)
         // Rex (recycler -- buyback account)
         // Philbin (registrar)
         //

         asset_id_type nono_id = create_user_issued_asset( "NONO", izzy_id(db), 0 ).id;
         asset_id_type buyme_id = create_user_issued_asset( "BUYME", izzy_id(db), 0 ).id;

         // Create a buyback account
         account_id_type rex_id;
         {
            buyback_account_options bbo;
            bbo.asset_to_buy = buyme_id;
            bbo.asset_to_buy_issuer = izzy_id;
            bbo.markets.emplace( asset_id_type() );
            account_create_operation create_op = make_account( "rex" );
            create_op.registrar = philbin_id;
            create_op.extensions.value.buyback_options = bbo;
            create_op.owner = authority::null_authority();
            create_op.active = authority::null_authority();

            // Let's break it...

            signed_transaction tx;
            tx.operations.push_back( create_op );
            set_expiration( db, tx );

            tx.operations.back().get< account_create_operation >().extensions.value.buyback_options->asset_to_buy_issuer = alice_id;
            sign( tx, alice_private_key );
            sign( tx, philbin_private_key );

            // Alice and Philbin signed, but asset issuer is invalid
            GRAPHENE_CHECK_THROW( db.push_transaction(tx), account_create_buyback_incorrect_issuer );

            tx.signatures.clear();
            tx.operations.back().get< account_create_operation >().extensions.value.buyback_options->asset_to_buy_issuer = izzy_id;
            sign( tx, philbin_private_key );

            // Izzy didn't sign
            GRAPHENE_CHECK_THROW( db.push_transaction(tx), tx_missing_active_auth );
            sign( tx, izzy_private_key );

            // OK
            processed_transaction ptx = db.push_transaction( tx );
            rex_id = ptx.operation_results.back().get< object_id_type >();

            // Try to create another account rex2 which is bbo on same asset
            tx.signatures.clear();
            tx.operations.back().get< account_create_operation >().name = "rex2";
            sign( tx, izzy_private_key );
            sign( tx, philbin_private_key );
            GRAPHENE_CHECK_THROW( db.push_transaction(tx), account_create_buyback_already_exists );
         }

         // issue some BUYME to Alice
         // we need to set_expiration() before issue_uia() because the latter doens't call it #11
         set_expiration( db, trx );  // #11
         issue_uia( alice_id, asset( 1000, buyme_id ) );
         issue_uia( alice_id, asset( 1000, nono_id ) );

         // Alice wants to sell 100 BUYME for 1000 BTS, a middle price.
         limit_order_id_type order_id_mid = create_sell_order( alice_id, asset( 100, buyme_id ), asset( 1000, asset_id_type() ) )->id;

         generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);
         generate_block();

         // no success because buyback has none for sale
         BOOST_CHECK( order_id_mid(db).for_sale == 100 );

         // but we can send some to buyback
         fund( rex_id(db), asset( 100, asset_id_type() ) );
         // no action until next maint
         BOOST_CHECK( order_id_mid(db).for_sale == 100 );
         generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);
         generate_block();

         // partial fill, Alice now sells 90 BUYME for 900 BTS.
         BOOST_CHECK( order_id_mid(db).for_sale == 90 );

         // TODO check burn amount

         // aagh more state in trx
         set_expiration( db, trx );  // #11

         // Selling 10 BUYME for 50 BTS, a low price.
         limit_order_id_type order_id_low  = create_sell_order( alice_id, asset( 10, buyme_id ), asset(  50, asset_id_type() ) )->id;
         // Selling 10 BUYME for 150 BTS, a high price.
         limit_order_id_type order_id_high = create_sell_order( alice_id, asset( 10, buyme_id ), asset( 150, asset_id_type() ) )->id;

         fund( rex_id(db), asset( 250, asset_id_type() ) );
         generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);
         generate_block();

         BOOST_CHECK( db.find( order_id_low  ) == nullptr );
         BOOST_CHECK( db.find( order_id_mid  ) != nullptr );
         BOOST_CHECK( db.find( order_id_high ) != nullptr );

         // 250 CORE in rex                 90 BUYME in mid order    10 BUYME in low order
         //  50 CORE goes to low order, buy 10 for 50 CORE
         // 200 CORE goes to mid order, buy 20 for 200 CORE
         //                                 70 BUYME in mid order     0 BUYME in low order

         idump( (order_id_mid(db)) );
         BOOST_CHECK( order_id_mid(db).for_sale == 70 );
         BOOST_CHECK( order_id_high(db).for_sale == 10 );

         BOOST_CHECK( get_balance( rex_id, asset_id_type() ) == 0 );

         // clear out the books -- 700 left on mid order, 150 left on high order, so 2000 BTS should result in 1150 left over

         fund( rex_id(db), asset( 2000, asset_id_type() ) );
         generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);

         idump( (get_balance( rex_id, asset_id_type() )) );

         BOOST_CHECK( get_balance( rex_id, asset_id_type() ) == 1150 );

         GRAPHENE_CHECK_THROW( transfer( alice_id, rex_id, asset( 1, nono_id ) ), fc::exception );
         // TODO: Check cancellation works for account which is BTS-restricted
      }

   } FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_SUITE_END()
