"""
generate_order_flow.py

Generates a synthetic stream of orders to feed into your matching engine.

Model:
- Mid-price follows a discrete random walk (simulates natural price drift).
- Order arrivals follow a Poisson process (standard assumption in market
  microstructure models - order flow is well modeled as a point process).
- ~85% of orders are LIMIT orders placed at a random offset from the current
  mid (offset drawn from an exponential distribution, so most orders sit
  close to the touch and fewer sit deep in the book - this matches real
  book shape).
- ~15% of orders are MARKET orders (aggressive/liquidity-taking flow).
- Order sizes are drawn from a log-normal distribution (heavy right tail,
  like real order size distributions).

Output: orders.csv with columns:
    order_id, timestamp, side, type, price, quantity

timestamp is in milliseconds from simulation start (float).
price is empty for MARKET orders (engine should fill at best available price).

Usage:
    python3 generate_order_flow.py --n 5000 --seed 42 --out orders.csv
"""

import argparse
import csv
import random


def generate(n_orders: int, seed: int, start_price: float, tick_size: float):
    rng = random.Random(seed)
    orders = []

    mid = start_price
    t = 0.0

    for order_id in range(1, n_orders + 1):
        # Poisson arrivals -> inter-arrival times are exponential.
        # Mean 50ms between orders (~20 orders/sec); tune to taste.
        t += rng.expovariate(1.0 / 50.0)

        # Random walk on the mid-price. Small drift, occasional jumps.
        step = rng.choice([-1, -1, 0, 0, 0, 1, 1]) * tick_size
        mid = max(tick_size, mid + step)

        side = rng.choice(["BUY", "SELL"])
        is_market = rng.random() < 0.15

        if is_market:
            orders.append({
                "order_id": order_id,
                "timestamp": round(t, 3),
                "side": side,
                "type": "MARKET",
                "price": "",
                "quantity": _lognormal_size(rng),
            })
        else:
            # Exponential distance from mid, in ticks. Most limit orders
            # sit near the touch; some sit deeper.
            distance_ticks = int(rng.expovariate(1.0 / 4.0)) + 1
            offset = distance_ticks * tick_size
            price = mid - offset if side == "BUY" else mid + offset
            orders.append({
                "order_id": order_id,
                "timestamp": round(t, 3),
                "side": side,
                "type": "LIMIT",
                "price": round(price, 2),
                "quantity": _lognormal_size(rng),
            })

    return orders


def _lognormal_size(rng: random.Random) -> int:
    # Median size ~10 lots, heavy tail for occasional large orders.
    size = rng.lognormvariate(mu=2.3, sigma=0.6)
    return max(1, int(round(size)))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--n", type=int, default=5000, help="number of orders")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--start-price", type=float, default=100.0)
    parser.add_argument("--tick-size", type=float, default=0.05)
    parser.add_argument("--out", type=str, default="orders.csv")
    args = parser.parse_args()

    orders = generate(args.n, args.seed, args.start_price, args.tick_size)

    with open(args.out, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["order_id", "timestamp", "side", "type", "price", "quantity"])
        writer.writeheader()
        writer.writerows(orders)

    print(f"Wrote {len(orders)} orders to {args.out}")
    print(f"  LIMIT: {sum(1 for o in orders if o['type'] == 'LIMIT')}")
    print(f"  MARKET: {sum(1 for o in orders if o['type'] == 'MARKET')}")


if __name__ == "__main__":
    main()
