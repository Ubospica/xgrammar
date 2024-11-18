
## Run Benchmark

### Benchmark Grammar Compile and Mask Generation

#### Dependencies
```
outlines                          0.0.46 (for Python backend)
outlines                          0.1.3  (for Rust backend, named as outlines_new in script)
outlines_core                     0.1.14
lm-format-enforcer                0.10.6
```

#### Run
```bash
python3 bench_json_schema.py [-h] [--backend {xgrammar,outlines,outlines_new,lmformatenforcer}]
                             [--num_iters NUM_ITERS] [--num_warmup NUM_WARMUP]
```

#### Results

Hardware:

```
CPU: AMD Ryzen 9 7950X 16-Core Processor
GPU: NVIDIA GeForce RTX 4090
```

Dataset: `NousResearch/json-mode-eval`

Model: `meta-llama/Llama-3.1-8B-Instruct`

Results:

JSON Schema:

```
Backend: xgrammar
Fail count: 0 / 99
Grammar preprocessing time (ms): 61.9149
Mask generation time (us/token): 35.7277
Backend: outlines
Fail count: 7 / 99
Grammar preprocessing time (ms): 2302.6504
Mask generation time (us/token): 7771.7369
Backend: outlines_new
Fail count (per iter): 7 / 99
Grammar preprocessing time (ms): 1333.1387
Mask generation time (us/token): 125.2214
Backend: lmformatenforcer
Fail count: 6 / 99
Grammar preprocessing time (ms): 2.7900
Mask generation time (us/token): 6147.1414
```

Unconstrained JSON (Context-free Grammar):

```
Backend: xgrammar
Fail count: 0 / 100
Grammar preprocessing time (ms): 101.3966
Mask generation time (us/token): 128.9123
Backend: outlines

Backend: outlines_new
Grammar preprocessing time (ms): 97.2
Mask generation time (us/token): >2.8s/token
```
