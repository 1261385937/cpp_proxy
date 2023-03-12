#pragma once

namespace cpp_proxy {

enum class lb_type {
   round_robin,
   weight,
   ip_hash,
   minimum_conn
};

struct load_balance {

};

}