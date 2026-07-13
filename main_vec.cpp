#include <bits/stdc++.h>
#include <random>
#include <variant>
#include <thread>



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
    bool cancelled;
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
map<int,Order> Order_info;
vector<vector<Order>> ask(conv_price_to_idx(max_price) + 5 );
vector<vector<Order>> bid(conv_price_to_idx(max_price) + 5);
vector<int> ask_actv_count(conv_price_to_idx(max_price) + 5);
vector<int> bid_actv_count(conv_price_to_idx(max_price) + 5);
vector<int> activ_bid_idx;
vector<int> activ_ask_idx;
unordered_map<int,pair<double,int>> Order_index;

vector<Trade> Trade_log;


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
     Order new_order = {id_gen,req.quantity,req.side,false};
     Order_info[new_order.id] = new_order;
    
     if(new_order.side == Side::Bid){
            int idx = conv_price_to_idx(req.price);
            bid[idx].push_back(new_order);
            Order_index[new_order.id] =  {req.price,bid[idx].size() - 1};
            bid_actv_count[idx]++;
            if(bid_actv_count[idx] == 1){
            auto ins = lower_bound(activ_bid_idx.begin(),activ_bid_idx.end(),idx);
            activ_bid_idx.insert(ins,idx);
            }
     }
     else if(new_order.side == Side::Ask){
            int idx = conv_price_to_idx(req.price);
            ask[idx].push_back(new_order);
            Order_index[new_order.id] =  {req.price,ask[idx].size() - 1};
            ask_actv_count[idx]++;
            if(ask_actv_count[idx] == 1){
            auto ins = lower_bound(activ_ask_idx.begin(),activ_ask_idx.end(),idx);
            activ_ask_idx.insert(ins,idx);
            }
            
     }
     return new_order.id;
     
}
int cancel_order(CancelOrderRequest req){
    
    Order to_cancel = Order_info[req.id];
    if(to_cancel.side == Side::Bid){
        auto [price,idx] =  Order_index[req.id];
        int price_index = conv_price_to_idx(price);
        bid[price_index][idx].cancelled = true;
        bid_actv_count[price_index]--;
        if(bid_actv_count[price_index] == 0){
        auto ins = lower_bound(activ_bid_idx.begin(),activ_bid_idx.end(),price_index);
        activ_bid_idx.erase(ins);
        }
        
    }
    else if(to_cancel.side == Side::Ask){
        auto [price,idx] =  Order_index[req.id];
        int price_index = conv_price_to_idx(price);
        ask[price_index][idx].cancelled = true;
        ask_actv_count[price_index]--;
        if(ask_actv_count[price_index] == 0){
        auto ins = lower_bound(activ_ask_idx.begin(),activ_ask_idx.end(),price_index);
        activ_ask_idx.erase(ins);
        }
    }
    Order_info[req.id].cancelled = true;
    return - 1;
    
}



optional<NewOrderRequest> matching_loop(NewOrderRequest req){
        if(req.side == Side::Bid){
              for(auto it = activ_ask_idx.begin();it != activ_ask_idx.end();){
                 int idx = (*it);
                 double price = conv_idx_to_price(idx) ;
                 if(price <= req.price ){
                      for(auto &item : ask[idx]){
                         if(item.cancelled){   //break if item cancelled
                            continue;
                         }
                         if(req.quantity == 0){
                            break;
                         }
                         if(item.quantity > req.quantity){
                            item.quantity -= req.quantity;
                            Trade_log.push_back(Trade{price,req.quantity,id_gen,item.id});
                            req.quantity = 0;
                            
                            break;
                         }
                         else{
                            req.quantity -= item.quantity;
                            Trade_log.push_back(Trade{price,item.quantity,id_gen,item.id});
                            item.cancelled = true;
                            ask_actv_count[idx]--;
                         }
                      }
                      if(ask_actv_count[idx] == 0){
                          it = activ_ask_idx.erase(it);
                      }else{
                          it++;
                      }
                      
                 }
                 else{
                    break;
                 }
                
              }
        }

        else if(req.side == Side::Ask){
              for(auto it = activ_bid_idx.begin();it != activ_bid_idx.end();){
                  int idx = (*it);
                  double price = conv_idx_to_price(idx);
                 if(price >= req.price ){
                      for(auto &item : bid[idx]){
                         if(item.cancelled){   
                            continue;
                         }
                         if(req.quantity == 0){
                            break;
                         }
                         if(item.quantity > req.quantity){
                            item.quantity -= req.quantity;

                            Trade_log.push_back(Trade{price,req.quantity,id_gen,item.id});
                            req.quantity = 0;
                            
                            break;
                         }
                         else{
                            req.quantity -= item.quantity;
                            int cancel_id = item.id;

                           Trade_log.push_back(Trade{price,item.quantity,id_gen,item.id});
                            item.cancelled = true;
                            bid_actv_count[idx]--;
                         }
                      }
                      if(bid_actv_count[idx] == 0){
                          it = activ_bid_idx.erase(it);
                      }else{
                          it++;
                      }
                      
                 }
                 else {
                    break;
                 }

                 
                
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
            if(Order_info.find(req.id) == Order_info.end()) return -1;
            return cancel_order(req);
        }else if constexpr (is_same_v<T, MarketOrderRequest>) {
           id_gen++;
           NewOrderRequest Sentinel_req;
           if(req.side == Side::Bid){
                  Sentinel_req = {999999999,req.quantity,req.side};
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

mt19937 rng;
 Request random_request() {
    
    uniform_real_distribution<double> price_dist(95.0,105.0);
    uniform_int_distribution<int> qty_dist(1, 100);
    uniform_int_distribution<int> type_dist(1, 10);
    
    vector<int> active_ids;
    
    
    
        int type = type_dist(rng);
        Side side = (rng() % 2) ? Side::Bid : Side::Ask;
        
        if (type <= 8 || active_ids.empty()) {
            return NewOrderRequest{
                price_dist(rng),
                qty_dist(rng),
                side
            };
        } else if (type <= 10) {
            return MarketOrderRequest{qty_dist(rng), side};
        } 
        return NewOrderRequest{0,0,Side::Bid};
    }
    







void consumer_func(SPSC_queue &spsc_q,int N){
      int count = 0;
      while(count != N){
          auto match_req = spsc_q.pop();
          if(match_req){
              process_order(*match_req);
              count++;
          }
      }
}

void producer_func(SPSC_queue &spsc_q,int N){
     int count = 0;
     
     while(count != N){
          Request req = random_request();
          while(!spsc_q.push(req));
          count++;
     }
}


int main(){
    auto start = chrono::high_resolution_clock::now();
    SPSC_queue spsc_q{.head = 0,.tail = 0};
    int n = 100000;
    thread producer_t(producer_func,ref(spsc_q),n);
    thread consumer_t(consumer_func,ref(spsc_q),n);
    producer_t.join();
    consumer_t.join();


    auto end = chrono::high_resolution_clock::now();
    auto duration = chrono::duration_cast<chrono::microseconds>(end - start);
    
    cout << "Orders: " << n << endl;
    cout << "Total time: " << duration.count() << " microseconds" << endl;
    cout << "Avg latency: " << (double)duration.count() / n << " microseconds/order" << endl;
    cout << "Throughput: " << (long long)n * 1000000 / duration.count() << " orders/sec" << endl;
    
}
