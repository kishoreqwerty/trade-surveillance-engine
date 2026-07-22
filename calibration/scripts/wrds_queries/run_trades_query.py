import wrds

db = wrds.Connection()

trades = db.raw_sql("""
    SELECT sym_root, date, time_m, price, size, tr_corr, tr_scond
    FROM taqmsec.ctm_2026
    WHERE sym_root IN ('AAPL','MSFT','JPM','XOM','TGT','GILD','DE','EBAY','HAS','RF')
    AND sym_suffix IS NULL
    AND date IN ('2026-03-11','2026-03-25','2026-04-01','2026-04-08')
    AND time_m BETWEEN '11:00:00' AND '12:30:00'
""")

print(len(trades), "trade rows")
trades.to_csv('trades.csv', index=False)
print("done")
