#pragma once

#include <chainbase/allocators.hpp>

#include <steem/chain/database.hpp>
#include <steem/chain/index.hpp>
#include <steem/chain/account_object.hpp>

namespace steem { namespace chain {

struct votes_update_data
{
   bool                    withdraw_executer = false;
   mutable int64_t         val = 0;

   const account_object*   account = nullptr;
};

struct votes_update_data_less
{
   bool operator()( const votes_update_data& obj1, const votes_update_data& obj2 ) const 
   {
      FC_ASSERT( obj1.account && obj2.account, "unexpected error: ${error}", ("error", delayed_voting_messages::object_is_null ) );
      return obj1.account->id < obj2.account->id;
   }
};

class delayed_voting
{
   public:

      using votes_update_data_items = std::set< votes_update_data, votes_update_data_less >;

   private:

      chain::database& db;

   public:

      delayed_voting( chain::database& _db ) : db( _db ){}

      void save_delayed_value( const account_object& account, const time_point_sec& head_time, uint64_t val );
      void erase_delayed_value( const account_object& account, uint64_t val );
      void add_votes( votes_update_data_items& items, bool withdraw_executer, int64_t val, const account_object& account );
      void update_votes( const votes_update_data_items& items, const time_point_sec& head_time );

      void run( const block_notification& note );
};

} } // namespace steem::chain
