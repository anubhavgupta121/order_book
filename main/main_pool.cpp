#include <bits/stdc++.h>
#include <random>
#include <variant>
#include <thread>
#include <x86intrin.h>



using namespace std;



enum class Side{Bid,Ask};


struct Trade{
    double price;
    int quantity;
    int aggressor_id;
    int rest_id;

};

struct Order {
    int id;
    int quantity;
    Side side;
};

struct NewOrderRequest{
    double price;
    int quantity;
    Side side;
};

struct CancelOrderRequest{
    int id;
};


struct MarketOrderRequest{
    int quantity;
    Side side;
};


struct Slots{
   int id;
   int quantity;
   int prev;
   int next;
};

struct Levels{
   int head = -1;
   int tail = -1;
};

struct order_loc
{   
    int pool_id = -1;
    int tick_idx;
    Side side;
};


double Min_price = 0;
double max_price = 105;
double tick_size = 0.01;


int conv_price_to_idx(double price){
     return (price - Min_price)/tick_size;
}
double conv_idx_to_price(int idx){
    return (idx*tick_size) + Min_price;
}





int id_gen = 0;
int free_head;
vector<Levels> ask(conv_price_to_idx(max_price) + 5 );
vector<Levels> bid(conv_price_to_idx(max_price) + 5);
vector<int> activ_bid_idx;
vector<int> activ_ask_idx;
vector<order_loc> loc;
vector<Slots> pool;
unordered_map<int,pair<double,int>> Order_index;

vector<Trade> Trade_log;

vector<int> active_ids;
int gen_next_id = 0;



using Request = variant<NewOrderRequest, CancelOrderRequest,MarketOrderRequest>;
int best_ask_indx = 0;
int best_bid_index = bid.size() - 1;
struct SPSC_queue{
    array<Request,1000> arr = {};
    alignas(64) atomic<size_t> head;
    alignas(64) atomic<size_t> tail;


    bool push(Request req){
        size_t h = head.load(memory_order_acquire);
        size_t t = tail.load(memory_order_acquire);
        if(t - h == 1000){
            return false;
        }else{
            arr[t%1000] = req;
            tail.store(t+1, memory_order_release);
            return true;
        }
    }
    optional<Request> pop(){
        size_t t = tail.load(memory_order_acquire);
        size_t h = head.load(memory_order_acquire);
        if(t - h == 0){
            return nullopt;
        }else{

            Request new_req = arr[head%1000];
            head.store(h + 1,memory_order_release);
            return new_req;
        }
    
    }
};


int add_order(NewOrderRequest req){
     Order new_order = {id_gen,req.quantity,req.side};
     int pool_id = free_head;
     free_head = pool[pool_id].next;
     pool[pool_id] = {new_order.id,new_order.quantity,-1,-1};
     int idx = conv_price_to_idx(req.price);
     loc[new_order.id] = {pool_id,idx,new_order.side};



     Levels& lvl = (new_order.side == Side::Bid) ? bid[idx] : ask[idx];
     auto& active = (new_order.side == Side::Bid) ? activ_bid_idx : activ_ask_idx;

     if(lvl.head == -1){
        lvl.head = pool_id;


        auto ins = (new_order.side == Side::Bid) ? lower_bound(active.begin(),active.end(),idx) : lower_bound(active.begin(),active.end(),idx,greater<int>());
        active.insert(ins,idx);
      }else{
        pool[lvl.tail].next = pool_id;
        pool[pool_id].prev = lvl.tail;
       }
      lvl.tail = pool_id;
     return new_order.id;
     
}
int cancel_order(CancelOrderRequest req){
    
    order_loc& cancel_loc = loc[req.id];

    // always need to update the slots, which are accessed through pool id

    int pool_id = cancel_loc.pool_id;

    if(pool_id == -1 || pool_id >= pool.size()){
        return -1;
    }

    Slots& to_cancel  = pool[pool_id];
    

    if(to_cancel.next != -1 && to_cancel.prev != -1){
        pool[to_cancel.prev].next = to_cancel.next;
        pool[to_cancel.next].prev = to_cancel.prev;
        
    }else{
        int idx = cancel_loc.tick_idx;
        Levels& lvl = (cancel_loc.side == Side::Bid) ? bid[idx] : ask[idx];
        auto& active = (cancel_loc.side == Side::Bid) ? activ_bid_idx : activ_ask_idx;

        if(to_cancel.prev == -1 && to_cancel.next == -1){
              lvl.head = -1;
              lvl.tail = -1;

              auto ins = (cancel_loc.side == Side::Bid) ? lower_bound(active.begin(),active.end(),idx) : lower_bound(active.begin(),active.end(),idx,greater<int>());
              active.erase(ins);

        }else if(to_cancel.next == -1){
            lvl.tail = to_cancel.prev;
            pool[to_cancel.prev].next  = -1;

        }else{
            lvl.head = to_cancel.next;
            pool[to_cancel.next].prev = -1;
            

        }
    }
    pool[pool_id].next = free_head;
    free_head = pool_id;
    cancel_loc.pool_id = -1;

    return 0;
    
}



optional<NewOrderRequest> matching_loop(NewOrderRequest req){

        // suppose iterate over the ative_ask in the increaseng manner, we except all ask <= bid;
        // howo it goes we get a price, idx supposed, then we go ffrom head to tail, at each time doing , next, if exhausted fully do the pool

        auto& active = (req.side == Side::Ask) ? activ_bid_idx : activ_ask_idx;
        while(!active.empty()){
                 if(req.quantity == 0){
                    break;
                 }
                 int idx = active.back();
                 int req_idx = conv_price_to_idx(req.price);   
                 double price = conv_idx_to_price(idx);

                 Levels& lvl = (req.side == Side::Ask) ? bid[idx] : ask[idx];
                 bool cond = (req.side == Side::Bid) ? idx <= req_idx : idx >= req_idx;  // comparing indices instead of raw prices to prevent floating point error's
                 if(cond){ 
                      while(lvl.head != -1){
                         Slots& resting = pool[lvl.head];
                         if(req.quantity == 0){
                            break;
                         }
                         if(resting.quantity > req.quantity){
                            resting.quantity -= req.quantity;
                            Trade_log.push_back(Trade{price,req.quantity,id_gen,resting.id});
                            req.quantity = 0;
                            break;
                         }
                         else{
                            req.quantity -= resting.quantity;
                            Trade_log.push_back(Trade{price,resting.quantity,id_gen,resting.id});

                            int next_val = pool[lvl.head].next;
                            pool[lvl.head].next = free_head;
                            free_head = lvl.head;
                            
                            loc[resting.id].pool_id = -1;

                            lvl.head = next_val;
                            if(lvl.head != -1){
                            pool[lvl.head].prev = -1;
                            }else{
                                lvl.tail = -1;
                            }
                            
                         }
                      }
                      if(lvl.head == -1){
                          active.pop_back();
                      }
                      
                 }else{
                    break;
                 }
                
              }
    


            


        if(req.quantity > 0){
            return NewOrderRequest(req);
        }
        return nullopt;
    
}


int process_order(Request request) {
    int id = visit([](auto&& req) {
        using T = decay_t<decltype(req)>;
        if constexpr (is_same_v<T, NewOrderRequest>) {
           id_gen++;
           auto matched_ptr = matching_loop(req);
           if(matched_ptr){
            return add_order(*matched_ptr);
           } 
           return 0;
        } else if constexpr (is_same_v<T, CancelOrderRequest>) {
            int f = cancel_order(req);
            return f;
        }else if constexpr (is_same_v<T, MarketOrderRequest>) {
           id_gen++;
           NewOrderRequest Sentinel_req;
           if(req.side == Side::Bid){
                  Sentinel_req = {max_price +tick_size,req.quantity,req.side}; // no out-of-bounds, and add_order canpotetnailly take it
           }else if(req.side == Side::Ask){
               Sentinel_req = {0,req.quantity,req.side};
           }
           auto matched_ptr = matching_loop(Sentinel_req);
           if(matched_ptr){
           return matched_ptr -> quantity;
           }
           return 0;
           
        }
    }, request);
    return id;
}

mt19937 rng(42);
Request random_request() {
    uniform_real_distribution<double> price_dist(95.0, 105.0);
    uniform_int_distribution<int> qty_dist(1, 100);
    uniform_int_distribution<int> type_dist(1, 10);

    int type = type_dist(rng);
    Side side = (rng() % 2) ? Side::Bid : Side::Ask;

    if (type <= 6 || (type >= 8 && active_ids.empty())) {
        gen_next_id++;
        active_ids.push_back(gen_next_id);
        return NewOrderRequest{round(price_dist(rng) / tick_size) * tick_size, qty_dist(rng), side};
    }
    if (type == 7) {
        gen_next_id++;
        return MarketOrderRequest{qty_dist(rng), side};
    }
    uniform_int_distribution<size_t> pick(0, active_ids.size() - 1);
    size_t i = pick(rng);
    int id = active_ids[i];
    active_ids[i] = active_ids.back();
    active_ids.pop_back();
    return CancelOrderRequest{id};
}



vector<Request> reqs;
vector<unsigned long long> push_tsc;
vector<unsigned long long> engine_cycles;
vector<unsigned long long> e2e_cycles;

void consumer_func(SPSC_queue &spsc_q,int N){
      int count = 0;
      while(count != N){
          auto match_req = spsc_q.pop();
          if(match_req){
              unsigned long long c0 = __rdtsc();
              process_order(*match_req);
              unsigned long long c1 = __rdtsc();
              engine_cycles[count] = c1 - c0;
              e2e_cycles[count] = c1 - push_tsc[count];
              count++;
          }
      }
}

void producer_func(SPSC_queue &spsc_q,int N){
     for(int count = 0; count < N; ++count){
          do {
              push_tsc[count] = __rdtsc();
          } while(!spsc_q.push(reqs[count]));
     }
}


int main(){
    int n = 100000;
    pool.resize(n);
    loc.resize(n+1);

    for(int i = 0;i<n-1;++i){
        pool[i].next = i + 1;
    }
    pool[n-1].next = -1;


    reqs.reserve(n);
    for (int i = 0; i < n; ++i) {
        reqs.push_back(random_request());
    }
    push_tsc.resize(n);
    engine_cycles.resize(n);
    e2e_cycles.resize(n);
    Trade_log.reserve(4 * n);

    // benchmark
    SPSC_queue spsc_q{.head = 0,.tail = 0};
    auto start = chrono::steady_clock::now();
    unsigned long long tsc_start = __rdtsc();

    thread consumer_t(consumer_func,ref(spsc_q),n);
    thread producer_t(producer_func,ref(spsc_q),n);
    producer_t.join();
    consumer_t.join();

    unsigned long long tsc_end = __rdtsc();
    auto end = chrono::steady_clock::now();

    auto total_ns = chrono::duration_cast<chrono::nanoseconds>(end - start).count();
    double ns_per_cycle = (double)total_ns / (double)(tsc_end - tsc_start);

    unsigned long long first_push = push_tsc[0];
    unsigned long long last_done = push_tsc[n - 1] + e2e_cycles[n - 1];
    double run_ns = (last_done - first_push) * ns_per_cycle;

    sort(engine_cycles.begin(), engine_cycles.end());
    sort(e2e_cycles.begin(), e2e_cycles.end());
    auto pct = [&](const vector<unsigned long long>& v, double p) {
        return v[(size_t)(p * (n - 1))] * ns_per_cycle;
    };

    cout << fixed << setprecision(1);
    cout << "Orders: " << n << '\n';
    cout << "Pipeline time: " << run_ns / 1000.0 << " microseconds\n";
    cout << "Throughput: " << (long long)(n * 1e9 / run_ns) << " orders/sec\n";
    cout << "\nEngine latency (process_order only):\n";
    cout << "  p50: " << pct(engine_cycles, 0.50) << " ns   p95: " << pct(engine_cycles, 0.95)
         << " ns   p99: " << pct(engine_cycles, 0.99) << " ns   p99.9: " << pct(engine_cycles, 0.999)
         << " ns   max: " << pct(engine_cycles, 1.0) << " ns\n";
    cout << "End-to-end latency (push into queue -> processed):\n";
    cout << "  p50: " << pct(e2e_cycles, 0.50) << " ns   p95: " << pct(e2e_cycles, 0.95)
         << " ns   p99: " << pct(e2e_cycles, 0.99) << " ns   p99.9: " << pct(e2e_cycles, 0.999)
         << " ns   max: " << pct(e2e_cycles, 1.0) << " ns\n\n";

        long long total_qty = 0;
    for (const Trade& t : Trade_log) {
    total_qty += t.quantity;
        }
    cout << "Trades: " << Trade_log.size() << '\n';
    cout << "Total qty: " << total_qty << '\n';

    ofstream trade_file("pool_trades.txt");
    trade_file << fixed << setprecision(2);
    for (const Trade& t : Trade_log) {
        trade_file << t.aggressor_id << ' ' << t.rest_id << ' ' << t.price << ' ' << t.quantity << '\n';
    }

    
}
