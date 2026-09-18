# /// script
# requires-python = ">=3.11"
# dependencies = [
#     "torch>=2.7.1",
#     "transformers>=4.53.0",
# ]
# ///
"""Local stand-in for a hosted rerank API, for development without a key.

Speaks the request and response shape realontext already sends, so the binary
reaches it through exactly the code path it uses in production; only
--rerank-endpoint differs. Reranking stores nothing, so unlike embeddings there
is no fingerprint to keep these scores away from a provider's.

The model is jina-reranker-v3.5 (open weights, CC-BY-NC-4.0): 0.6B on a Qwen3
base, and listwise — every candidate is judged in one forward pass with the
others in view, which is what D6 picked and what no reachable hosted API gave
us. Its licence is non-commercial, same as the embedding weights next door:
these scores say what the stage is worth, not what ships.

    uv run eval/rerank_server.py [--port 8585] [--device mps]
"""
import argparse
import json
from http.server import BaseHTTPRequestHandler, HTTPServer

MODEL = "jina-reranker-v3.5"


def load(device):
    import torch
    from transformers import AutoModel
    # bfloat16, not float16: fp16 attention on MPS overflows to NaN
    # (pytorch/pytorch#96602).
    model = AutoModel.from_pretrained("jinaai/" + MODEL, dtype=torch.bfloat16, trust_remote_code=True)
    model.to(device)
    model.eval()
    return model


def rank(model, query, documents):
    """(index, score) best first, one entry per document, in the model's order."""
    import torch
    with torch.inference_mode():
        results = model.rerank(query, documents)
    out = []
    for i, r in enumerate(results):
        # The custom code has named this field differently across revisions;
        # a rerank server that silently returns input order is worse than one
        # that refuses to start.
        for key in ("index", "corpus_id", "document_id"):
            if key in r:
                out.append((int(r[key]), float(r["relevance_score"])))
                break
        else:
            raise RuntimeError("rerank result %d has no index field: %s" % (i, sorted(r)))
    if sorted(i for i, _ in out) != list(range(len(documents))):
        raise RuntimeError("rerank returned %d entries for %d documents" % (len(out), len(documents)))
    torch.mps.empty_cache()
    return out


def handler(model, tokenizer):
    class Handler(BaseHTTPRequestHandler):
        def reply(self, code, body):
            data = json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_POST(self):
            if self.path != "/v1/rerank":
                return self.reply(404, {"detail": "not found"})
            req = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            if req.get("model") != MODEL:
                return self.reply(400, {"detail": "this server only serves %s" % MODEL})
            documents = req.get("documents") or []
            try:
                scored = rank(model, req["query"], documents)
            except Exception as e:  # a 500 is retried; a wrong ranking is not noticed
                return self.reply(500, {"detail": str(e)})
            # Billed the way the hosted APIs bill it: the query once per
            # document, plus the documents. Nothing here charges, but the
            # limiter and the reports read this number.
            tokens = len(tokenizer(req["query"])["input_ids"]) * len(documents)
            tokens += sum(len(ids) for ids in tokenizer(documents)["input_ids"]) if documents else 0
            self.reply(200, {
                "model": MODEL,
                "data": [{"index": i, "relevance_score": s} for i, s in scored],
                "usage": {"total_tokens": tokens},
            })

        def log_message(self, fmt, *args):
            pass

    return Handler


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--port", type=int, default=8585)
    p.add_argument("--device", default="mps")
    args = p.parse_args()
    from transformers import AutoTokenizer
    model = load(args.device)
    tokenizer = AutoTokenizer.from_pretrained("jinaai/" + MODEL, trust_remote_code=True)
    print("serving %s on http://127.0.0.1:%d/v1/rerank" % (MODEL, args.port), flush=True)
    HTTPServer(("127.0.0.1", args.port), handler(model, tokenizer)).serve_forever()


if __name__ == "__main__":
    main()
