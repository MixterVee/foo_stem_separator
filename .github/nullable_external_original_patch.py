from pathlib import Path

p = Path('stem_dsp.cpp')
s = p.read_text(encoding='utf-8')
old = '''        std::vector<float> cache_original;\n        std::vector<float> cache_vocals;\n        std::vector<float> cache_instrumental;\n        if (!convert_to_cache_stereo(original, frames, channels, sample_rate, cache_original) ||\n            !convert_to_cache_stereo(vocals, frames, channels, sample_rate, cache_vocals) ||\n            !convert_to_cache_stereo(instrumental, frames, channels, sample_rate, cache_instrumental)) {\n            return false;\n        }\n        if (cache_original.empty() || cache_vocals.size() != cache_original.size() ||\n            cache_instrumental.size() != cache_original.size()) return false;\n\n        const size_t cache_frames = cache_original.size() / kCacheChannels;'''
new = '''        std::vector<float> cache_original;\n        std::vector<float> cache_vocals;\n        std::vector<float> cache_instrumental;\n\n        // Spectral Waveform may omit Original for persisted transport blocks.\n        // Original is cheap to decode on demand; the separated stems are the\n        // expensive data that must survive a restart.\n        if (original != nullptr &&\n            !convert_to_cache_stereo(original, frames, channels, sample_rate, cache_original)) {\n            return false;\n        }\n        if (!convert_to_cache_stereo(vocals, frames, channels, sample_rate, cache_vocals) ||\n            !convert_to_cache_stereo(instrumental, frames, channels, sample_rate, cache_instrumental)) {\n            return false;\n        }\n        if (cache_vocals.empty() || cache_instrumental.size() != cache_vocals.size()) return false;\n        if (!cache_original.empty() && cache_original.size() != cache_vocals.size()) return false;\n\n        const size_t cache_frames = cache_vocals.size() / kCacheChannels;'''
assert old in s
s = s.replace(old, new, 1)

old = '''    // Keep the decoded original beside both stems. Normal playback still uses\n    // foobar's incoming chunk; this copy is only for jog/reverse preview.\n    std::vector<float> original;'''
new = '''    // Original is optional for Spectral Waveform-published blocks. Normal\n    // playback has the source chunk already, while Original transport can be\n    // decoded cheaply without invoking Spleeter.\n    std::vector<float> original;'''
assert old in s
s = s.replace(old, new, 1)
p.write_text(s, encoding='utf-8')
