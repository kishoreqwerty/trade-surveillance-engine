import wrds

db = wrds.Connection()

trades = db.raw_sql("""
    SELECT sym_root, date, time_m, price, size, tr_corr, tr_scond
    FROM taqmsec.ctm_2026
    WHERE sym_root IN ('AAPL','MSFT','JPM','XOM','TGT','GILD','DE','EBAY','HAS','RF')
    AND date IN ('2026-06-03','2026-06-10','2026-06-17','2026-06-24')
    AND time_m BETWEEN '11:00:00' AND '12:30:00'
""")

print(len(trades), "rows")
trades.to_csv('trades_with_corr.csv', index=False)
print("done")
