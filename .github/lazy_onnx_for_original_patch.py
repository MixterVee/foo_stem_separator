from pathlib import Path

p = Path('stem_dsp.cpp')
s = p.read_text(encoding='utf-8')

old = '''            // Engine construction itself can throw. Keeping it inside this\n            // top-level exception boundary prevents std::terminate().\n            onnxstem::engine engine;\n\n            sequential_decoder_state decoder_state;\n'''
new = '''            // Original platter PCM does not need ONNX at all. Construct the\n            // separation engine lazily so cheap Original prefetch can begin as\n            // soon as Media Foundation is ready instead of waiting for model\n            // initialization first. Construction still stays inside the worker\n            // exception boundary and is additionally covered by the per-job try.\n            std::unique_ptr<onnxstem::engine> engine;\n\n            sequential_decoder_state decoder_state;\n'''
if old not in s:
    raise SystemExit('engine construction anchor not found')
s = s.replace(old, new, 1)

old = '''                            separated =\n                                engine.process_both(\n                                    analysis_input.data(),\n                                    analysis_input.size() / kCacheChannels,\n                                    kCacheChannels,\n                                    kCacheRate,\n                                    vocals,\n                                    instrumental);\n'''
new = '''                            if (!engine) {\n                                engine = std::make_unique<onnxstem::engine>();\n                            }\n\n                            separated =\n                                engine->process_both(\n                                    analysis_input.data(),\n                                    analysis_input.size() / kCacheChannels,\n                                    kCacheChannels,\n                                    kCacheRate,\n                                    vocals,\n                                    instrumental);\n'''
if old not in s:
    raise SystemExit('process_both anchor not found')
s = s.replace(old, new, 1)

p.write_text(s, encoding='utf-8')
print('patched lazy ONNX initialization for Original transport prefetch')
