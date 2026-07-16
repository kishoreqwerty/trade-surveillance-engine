from fastapi import FastAPI

app = FastAPI(title="trade-surveillance-ml-service")


@app.get("/health")
def health() -> dict:
    return {"status": "ok"}
