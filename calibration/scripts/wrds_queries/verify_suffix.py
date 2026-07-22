import wrds

db = wrds.Connection()

result = db.raw_sql("""
    SELECT sym_suffix, COUNT(*) AS n, MIN(price) AS min_price, MAX(price) AS max_price, AVG(price) AS avg_price
    FROM taqmsec.ctm_2026
    WHERE sym_root = 'JPM'
    AND date IN ('2026-03-11','2026-03-25','2026-04-01','2026-04-08')
    AND time_m BETWEEN '11:00:00' AND '12:30:00'
    GROUP BY sym_suffix
    ORDER BY n DESC
""")

print(result)
