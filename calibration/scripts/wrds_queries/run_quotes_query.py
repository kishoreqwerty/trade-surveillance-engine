import wrds

db = wrds.Connection()

quotes = db.raw_sql("""
    SELECT sym_root, date, time_m, best_bid AS bid, best_ask AS ask, best_bidsiz AS bidsiz, best_asksiz AS asksiz
    FROM taqmsec.nbbom_20260311
    WHERE sym_root IN ('AAPL','MSFT','JPM','XOM','TGT','GILD','DE','EBAY','HAS','RF')
    AND sym_suffix IS NULL
    AND time_m BETWEEN '11:00:00' AND '12:30:00'

    UNION ALL

    SELECT sym_root, date, time_m, best_bid AS bid, best_ask AS ask, best_bidsiz AS bidsiz, best_asksiz AS asksiz
    FROM taqmsec.nbbom_20260325
    WHERE sym_root IN ('AAPL','MSFT','JPM','XOM','TGT','GILD','DE','EBAY','HAS','RF')
    AND sym_suffix IS NULL
    AND time_m BETWEEN '11:00:00' AND '12:30:00'

    UNION ALL

    SELECT sym_root, date, time_m, best_bid AS bid, best_ask AS ask, best_bidsiz AS bidsiz, best_asksiz AS asksiz
    FROM taqmsec.nbbom_20260401
    WHERE sym_root IN ('AAPL','MSFT','JPM','XOM','TGT','GILD','DE','EBAY','HAS','RF')
    AND sym_suffix IS NULL
    AND time_m BETWEEN '11:00:00' AND '12:30:00'

    UNION ALL

    SELECT sym_root, date, time_m, best_bid AS bid, best_ask AS ask, best_bidsiz AS bidsiz, best_asksiz AS asksiz
    FROM taqmsec.nbbom_20260408
    WHERE sym_root IN ('AAPL','MSFT','JPM','XOM','TGT','GILD','DE','EBAY','HAS','RF')
    AND sym_suffix IS NULL
    AND time_m BETWEEN '11:00:00' AND '12:30:00'
""")

print(len(quotes), "quote rows")
quotes.to_csv('quotes.csv', index=False)
print("done")
