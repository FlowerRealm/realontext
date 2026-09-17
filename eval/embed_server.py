# /// script
# requires-python = ">=3.11"
# dependencies = [
#     "torch>=2.7.1",
#     "transformers>=4.53.0",
#     "sentence-transformers>=5.0.0",
# ]
# ///
"""Local stand-in for the Jina embeddings API, for development without a key.

Speaks the request and response shape of POST https://api.jina.ai/v1/embeddings,
so realontext talks to it through exactly the code path it uses in production;
only --endpoint differs. The endpoint is part of the index fingerprint (D22), so
vectors from this server can never mix with vectors from the real API.

The model is jina-code-embeddings-0.5b or -1.5b (open weights, CC-BY-NC-4.0).
0.5b trails 1.5b by 0.6 points on Jina's code retrieval average and runs about
three times faster; its dimensions are 896 at most, so it cannot give the 1024
of D7. Neither is jina-embeddings-v4, the model D6 picks: scores from them say
whether the pipeline works and roughly what the vector route is worth, not what
production will score.

    uv run eval/embed_server.py [--model jina-code-embeddings-0.5b] [--port 8484] [--device mps]
"""
import argparse
import json
from http.server import BaseHTTPRequestHandler, HTTPServer

MODELS = ("jina-code-embeddings-0.5b", "jina-code-embeddings-1.5b")
PROMPTS = {"code.query": "nl2code_query", "code.passage": "nl2code_document"}
# Padded tokens per forward pass. Attention memory grows with the longest input
# in a batch, so inputs are sorted by length and grouped under this budget.
TOKEN_BUDGET = 16384


def load(name, device):
    import torch
    from sentence_transformers import SentenceTransformer
    # bfloat16, not float16: fp16 attention on MPS overflows to NaN
    # (pytorch/pytorch#96602). Last-token pooling reads the wrong token unless
    # padding is on the left, so it is set rather than left to the tokenizer.
    m = SentenceTransformer("jinaai/" + name, device=device,
                            model_kwargs={"torch_dtype": torch.bfloat16, "attn_implementation": "sdpa"},
                            tokenizer_kwargs={"padding_side": "left"})
    m.max_seq_length = 32768
    return m


def encode(model, texts, prompt_name, dim):
    """Vectors and billed tokens for `texts`, in input order."""
    import numpy as np
    import torch
    prompt = model.prompts[prompt_name]
    lengths = [len(ids) for ids in model.tokenizer([prompt + t for t in texts])["input_ids"]]
    order = sorted(range(len(texts)), key=lambda i: lengths[i])
    out = [None] * len(texts)
    group = []
    for i in order + [None]:
        if group and (i is None or lengths[i] * (len(group) + 1) > TOKEN_BUDGET):
            vectors = model.encode([texts[j] for j in group], prompt_name=prompt_name, truncate_dim=dim,
                                   batch_size=len(group), convert_to_numpy=True)
            for j, v in zip(group, vectors):
                out[j] = v / np.linalg.norm(v)
            group = []
        if i is not None:
            group.append(i)
    torch.mps.empty_cache()
    return out, sum(lengths)


def handler(name, model):
    class Handler(BaseHTTPRequestHandler):
        def reply(self, code, body):
            data = json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_POST(self):
            if self.path != "/v1/embeddings":
                return self.reply(404, {"detail": "not found"})
            req = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            if req.get("model") != name:
                return self.reply(400, {"detail": "this server only serves %s" % name})
            if req.get("task") not in PROMPTS:
                return self.reply(400, {"detail": "task must be one of %s" % sorted(PROMPTS)})
            vectors, tokens = encode(model, req["input"], PROMPTS[req["task"]], req.get("dimensions"))
            self.reply(200, {
                "model": req["model"],
                "data": [{"index": i, "embedding": v.tolist()} for i, v in enumerate(vectors)],
                "usage": {"total_tokens": tokens},
            })

        def log_message(self, fmt, *args):
            pass

    return Handler


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--model", choices=MODELS, default=MODELS[0])
    p.add_argument("--port", type=int, default=8484)
    p.add_argument("--device", default="mps")
    args = p.parse_args()
    model = load(args.model, args.device)
    print("serving %s on http://127.0.0.1:%d/v1/embeddings" % (args.model, args.port), flush=True)
    HTTPServer(("127.0.0.1", args.port), handler(args.model, model)).serve_forever()


if __name__ == "__main__":
    main()
