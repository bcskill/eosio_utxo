#include "verifier.hpp"

[[eosio::action]]
void verifier::create(name issuer, asset maximum_supply) {
    require_auth( _self );
  
    auto symbol = maximum_supply.symbol;
    eosio_assert(symbol.is_valid(), "invalid symbol name");
    eosio_assert(maximum_supply.is_valid(), "invalid supply");
    eosio_assert(maximum_supply.amount > 0, "max-supply must be positive");
  
    stats statstable(_self, symbol.raw());
    auto existing = statstable.find(symbol.raw());
    eosio_assert(existing == statstable.end(), "token with symbol already exists");
  
    statstable.emplace(_self, [&](auto& s) {
        s.supply.symbol = maximum_supply.symbol;
        s.max_supply    = maximum_supply;
        s.issuer        = issuer;
    });
}

[[eosio::action]]
void verifier::issue(public_key to, asset quantity, const string memo) {
    auto symbol = quantity.symbol;
    eosio_assert(symbol.is_valid(), "invalid symbol name");
    eosio_assert(memo.size() <= 256, "memo has more than 256 bytes");

    stats statstable(_self, symbol.raw());
    auto existing = statstable.find(symbol.raw());
    eosio_assert(existing != statstable.end(), "token with symbol does not exist, create token before issue");
    const auto& st = *existing;

    require_auth( st.issuer );
    eosio_assert(quantity.is_valid(), "invalid quantity");
    eosio_assert(quantity.amount > 0, "must issue positive quantity");

    eosio_assert(quantity.symbol == st.supply.symbol, "symbol precision mismatch");
    eosio_assert(quantity.amount <= st.max_supply.amount - st.supply.amount, "quantity exceeds available supply");

    statstable.modify(st, _self, [&](auto& s) {
       s.supply += quantity;
    });

    add_balance(to, quantity);
}

[[eosio::action]]
void verifier::transfer(
            public_key relayer,
            public_key from,
            public_key to,
            asset amount,
            asset fee,
            uint64_t nonce,
            string memo,
            signature sig) {

    // get currency information
    auto sym = amount.symbol.raw();
    stats statstable(_self, sym);
    const auto& st = statstable.get(sym);

    // get last nonce
    nonces noncetable( _self, _self.value );
    auto pk_index = noncetable.get_index<name("bypk")>();
    auto nonce_it = pk_index.find(public_key_to_fixed_bytes(from));
    uint64_t last_nonce = 0;
    if (nonce_it != pk_index.end())
        last_nonce = nonce_it->last_nonce;
    
    // validate inputs
    eosio_assert(from != to, "cannot transfer to self");
    eosio_assert(amount.is_valid(), "invalid quantity" );
    eosio_assert(fee.is_valid(), "invalid quantity" );
    eosio_assert(amount.amount > 0, "must transfer positive quantity");
    eosio_assert(fee.amount >= 0, "fee must be non-negative");
    eosio_assert(amount.symbol == st.supply.symbol, "symbol precision mismatch");
    eosio_assert(fee.symbol == st.supply.symbol, "symbol precision mismatch");
    eosio_assert(memo.size() <= 163, "memo has more than 164 bytes");
    eosio_assert(nonce > last_nonce, "Nonce must be greater than last nonce. This transaction may already have been relayed.");
    eosio_assert(nonce < last_nonce + 100, "Nonce cannot jump by more than 100");
    
    // tx meta fields
    uint8_t version = 0x01;
    uint8_t length = 92 + memo.size();
    
    // construct raw transaction
    uint8_t rawtx[length];
    rawtx[0] = version;
    rawtx[1] = length;
    memcpy(rawtx + 2, from.data.data(), 33);
    memcpy(rawtx + 35, to.data.data(), 33);
    memcpy(rawtx + 68, (uint8_t *)&amount.amount, 8);
    memcpy(rawtx + 76, (uint8_t *)&fee.amount, 8);
    memcpy(rawtx + 84, (uint8_t *)&nonce, 8);
    memcpy(rawtx + 92, memo.c_str(), memo.size());
    
    // hash transaction
    checksum256 digest = sha256((const char *)rawtx, length);
    
    // verify signature
    assert_recover_key(digest, sig, from);
    
    // update balances with main amount
    sub_balance(from, amount);
    add_balance(to, amount);
  
    // update balances with fees
    if (fee.amount > 0) {
        sub_balance(from, fee);
        add_balance(relayer, fee);
    }
    
    // update last nonce
    if (nonce_it != pk_index.end()) {
        pk_index.modify(nonce_it, _self, [&]( auto& n ){
            n.last_nonce = nonce;
        });
    } else {
        noncetable.emplace( _self, [&]( auto& n ) {
            n.id = noncetable.available_primary_key();
            n.publickey = from;
            n.last_nonce = nonce;
        });
    }
  
}

void verifier::sub_balance(public_key sender, asset value) {
    accounts from_acts(_self, _self.value);
  
    auto accounts_index = from_acts.get_index<name("bypk")>();
    const auto& from = accounts_index.get(public_key_to_fixed_bytes(sender), "no public key object found");
    eosio_assert(from.balance.amount >= value.amount, "overdrawn balance");
  
    if (from.balance.amount == value.amount) {
        from_acts.erase(from);
    } else {
        from_acts.modify(from, _self, [&]( auto& a ) {
            a.balance -= value;
        });
    }
}

void verifier::add_balance(public_key recipient, asset value) {
    accounts to_acts(_self, _self.value);
  
    auto accounts_index = to_acts.get_index<name("bypk")>();
    auto to = accounts_index.find(public_key_to_fixed_bytes(recipient));
  
    if (to == accounts_index.end()) {
        to_acts.emplace(_self, [&]( auto& a ){
            a.key = to_acts.available_primary_key();
            a.balance = value;
            a.publickey = recipient;
        });
    } else {
        accounts_index.modify(to, _self, [&]( auto& a ) {
            a.balance += value;
        });
    }
}

EOSIO_DISPATCH(verifier, (create)(issue)(transfer))
