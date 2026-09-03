"""
analyze_results.py

Reads the outputs of a matching-engine simulation run and computes market
microstructure metrics:

  1. Fill rate         - % of submitted orders (and volume) that got filled
  2. Time-to-fill       - latency distribution for resting limit orders
  3. Quoted spread      - best_ask - best_bid over time
  4. Slippage            - for MARKET orders, fill price vs. mid price at submission
  5. Price impact        - mid-price move immediately after a marketable trade

Expected input files (see LOGGING_SPEC.md):
  - orders.csv           (order_id, timestamp, side, type, price, quantity)
  - fills.csv             (fill_id, order_id, timestamp, side, price, quantity, [is_maker])
  - book_snapshots.csv    (timestamp, best_bid, best_ask)   [optional but recommended]

Usage:
    python3 analyze_results.py --orders orders.csv --fills fills.csv \
        --snapshots book_snapshots.csv --outdir report/
"""

import argparse
import os

import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def load_data(orders_path, fills_path, snapshots_path):
    orders = pd.read_csv(orders_path)
    fills = pd.read_csv(fills_path)

    snapshots = None
    if snapshots_path and os.path.exists(snapshots_path):
        snapshots = pd.read_csv(snapshots_path)
        snapshots = snapshots.dropna(subset=["best_bid", "best_ask"])
        snapshots["mid"] = (snapshots["best_bid"] + snapshots["best_ask"]) / 2.0
        snapshots["spread"] = snapshots["best_ask"] - snapshots["best_bid"]
        snapshots = snapshots.sort_values("timestamp").reset_index(drop=True)

    return orders, fills, snapshots


def fill_rate(orders: pd.DataFrame, fills: pd.DataFrame) -> dict:
    filled_order_ids = set(fills["order_id"].unique())
    n_orders = len(orders)
    n_filled = orders["order_id"].isin(filled_order_ids).sum()

    submitted_qty = orders["quantity"].sum()
    filled_qty = fills.groupby("order_id")["quantity"].sum()
    filled_qty = filled_qty.reindex(orders["order_id"]).fillna(0)
    total_filled_qty = filled_qty.sum()

    return {
        "orders_submitted": int(n_orders),
        "orders_filled_at_all": int(n_filled),
        "fill_rate_by_count_pct": round(100.0 * n_filled / n_orders, 2) if n_orders else 0.0,
        "quantity_submitted": float(submitted_qty),
        "quantity_filled": float(total_filled_qty),
        "fill_rate_by_volume_pct": round(100.0 * total_filled_qty / submitted_qty, 2) if submitted_qty else 0.0,
    }


def time_to_fill(orders: pd.DataFrame, fills: pd.DataFrame) -> pd.Series:
    limit_orders = orders[orders["type"] == "LIMIT"][["order_id", "timestamp"]].rename(
        columns={"timestamp": "submit_time"}
    )
    first_fill = fills.groupby("order_id")["timestamp"].min().rename("first_fill_time")
    merged = limit_orders.merge(first_fill, on="order_id", how="inner")
    merged["ttf_ms"] = merged["first_fill_time"] - merged["submit_time"]
    return merged["ttf_ms"].clip(lower=0)


def nearest_snapshot(snapshots: pd.DataFrame, ts: float, tolerance=None):
    """Return the snapshot row with the largest timestamp <= ts (last known book state)."""
    idx = snapshots["timestamp"].searchsorted(ts, side="right") - 1
    if idx < 0:
        return None
    return snapshots.iloc[idx]


def slippage_and_impact(orders: pd.DataFrame, fills: pd.DataFrame, snapshots: pd.DataFrame):
    market_orders = orders[orders["type"] == "MARKET"]
    results = []

    for _, order in market_orders.iterrows():
        order_fills = fills[fills["order_id"] == order["order_id"]]
        if order_fills.empty:
            continue

        pre = nearest_snapshot(snapshots, order["timestamp"])
        if pre is None:
            continue

        # volume-weighted average fill price
        vwap = np.average(order_fills["price"], weights=order_fills["quantity"])
        mid_before = pre["mid"]
        slip = (vwap - mid_before) if order["side"] == "BUY" else (mid_before - vwap)

        # price impact: mid a short time after the last fill vs. mid before
        last_fill_ts = order_fills["timestamp"].max()
        post = nearest_snapshot(snapshots, last_fill_ts + 1e-6)
        impact = None
        if post is not None:
            impact = (post["mid"] - mid_before) if order["side"] == "BUY" else (mid_before - post["mid"])

        results.append({
            "order_id": order["order_id"],
            "side": order["side"],
            "mid_before": mid_before,
            "vwap_fill_price": vwap,
            "slippage": slip,
            "price_impact": impact,
        })

    return pd.DataFrame(results)


def make_plots(snapshots, ttf, slip_df, outdir):
    os.makedirs(outdir, exist_ok=True)

    if snapshots is not None and not snapshots.empty:
        plt.figure(figsize=(9, 4))
        plt.plot(snapshots["timestamp"], snapshots["spread"], linewidth=0.8)
        plt.xlabel("Time (ms)")
        plt.ylabel("Quoted spread")
        plt.title("Quoted spread over time")
        plt.tight_layout()
        plt.savefig(os.path.join(outdir, "spread_over_time.png"), dpi=130)
        plt.close()

    if ttf is not None and len(ttf) > 0:
        plt.figure(figsize=(7, 4))
        plt.hist(ttf, bins=40)
        plt.xlabel("Time to fill (ms)")
        plt.ylabel("Count")
        plt.title("Time-to-fill distribution (limit orders)")
        plt.tight_layout()
        plt.savefig(os.path.join(outdir, "time_to_fill_hist.png"), dpi=130)
        plt.close()

    if slip_df is not None and not slip_df.empty:
        plt.figure(figsize=(7, 4))
        plt.hist(slip_df["slippage"], bins=30)
        plt.xlabel("Slippage (price units, positive = adverse)")
        plt.ylabel("Count")
        plt.title("Market order slippage distribution")
        plt.tight_layout()
        plt.savefig(os.path.join(outdir, "slippage_hist.png"), dpi=130)
        plt.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--orders", default="orders.csv")
    parser.add_argument("--fills", default="fills.csv")
    parser.add_argument("--snapshots", default="book_snapshots.csv")
    parser.add_argument("--outdir", default="report")
    args = parser.parse_args()

    orders, fills, snapshots = load_data(args.orders, args.fills, args.snapshots)

    print("=" * 50)
    print("FILL RATE")
    print("=" * 50)
    for k, v in fill_rate(orders, fills).items():
        print(f"  {k}: {v}")

    ttf = time_to_fill(orders, fills)
    if len(ttf) > 0:
        print()
        print("=" * 50)
        print("TIME TO FILL (limit orders, ms)")
        print("=" * 50)
        print(f"  median: {ttf.median():.2f}")
        print(f"  p90:    {ttf.quantile(0.9):.2f}")
        print(f"  p99:    {ttf.quantile(0.99):.2f}")

    slip_df = pd.DataFrame()
    if snapshots is not None:
        print()
        print("=" * 50)
        print("QUOTED SPREAD")
        print("=" * 50)
        print(f"  mean:   {snapshots['spread'].mean():.4f}")
        print(f"  median: {snapshots['spread'].median():.4f}")

        slip_df = slippage_and_impact(orders, fills, snapshots)
        if not slip_df.empty:
            print()
            print("=" * 50)
            print("SLIPPAGE (market orders, positive = adverse to taker)")
            print("=" * 50)
            print(f"  mean:   {slip_df['slippage'].mean():.4f}")
            print(f"  median: {slip_df['slippage'].median():.4f}")

            impact = slip_df["price_impact"].dropna()
            if len(impact) > 0:
                print()
                print("=" * 50)
                print("PRICE IMPACT (mid move after marketable trade)")
                print("=" * 50)
                print(f"  mean:   {impact.mean():.4f}")
                print(f"  median: {impact.median():.4f}")
    else:
        print()
        print("(No book_snapshots.csv found - skipping spread/slippage/impact.")
        print(" Add snapshot logging per LOGGING_SPEC.md to unlock these.)")

    make_plots(snapshots, ttf, slip_df, args.outdir)
    print(f"\nPlots saved to {args.outdir}/")


if __name__ == "__main__":
    main()
