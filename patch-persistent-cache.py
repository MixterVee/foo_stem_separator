from pathlib import Path

p = Path('stem_dsp.cpp')
s = p.read_text(encoding='utf-8-sig')

def repl(old, new, label):
    global s
    if s.count(old) != 1:
        raise RuntimeError(f'{label}: expected 1 marker, found {s.count(old)}')
    s = s.replace(old, new, 1)

repl(
    '#include "stem_processing_status_service.h"\n',
    '#include "stem_processing_status_service.h"\n#include "persistent_stem_cache.h"\n',
    'include')

repl(
'''    bool transport_preview = false;\n    bool need_stems = true;\n};\n''',
'''    bool transport_preview = false;\n    bool need_stems = true;\n    bool restore_persisted = false;\n};\n''',
    'cache job flag')

repl(
'''        if (!m_path.empty()) {\n            const stemmode::mode mode = stemmode::get();\n''',
'''        if (!m_path.empty()) {\n            m_jobs.emplace_back(cache_job{\n                m_generation, m_path, 0.0, false, false, false, true});\n            m_job_pending = true;\n\n            const stemmode::mode mode = stemmode::get();\n''',
    'new track restore')

repl(
'''    void queue_job_locked(\n        double start_seconds,\n        bool force_reanchor = false) {\n''',
'''    bool internal_stem_range_ready_locked(\n        double start_seconds,\n        double end_seconds) const {\n\n        if (end_seconds <= start_seconds + 1.0e-6) return true;\n        double cursor = start_seconds;\n        while (cursor < end_seconds - 1.0e-6) {\n            double furthest = cursor;\n            for (const auto& seg_ptr : m_segments) {\n                const cache_segment& seg = *seg_ptr;\n                if (seg.external_waveform || seg.vocals.empty() || seg.instrumental.empty()) continue;\n                if (seg.start_seconds <= cursor + 1.0e-6 && seg.end_seconds > furthest) {\n                    furthest = seg.end_seconds;\n                }\n            }\n            if (furthest <= cursor + 1.0e-6) return false;\n            cursor = furthest;\n        }\n        return true;\n    }\n\n    void queue_job_locked(\n        double start_seconds,\n        bool force_reanchor = false) {\n''',
    'coverage helper')

repl(
'''                try {\n                    std::vector<float> input;\n\n                    // Transport preview gets a separate decoder timeline. A random\n''',
'''                try {\n                    if (job.restore_persisted) {\n                        auto disk_segments = persistent_stem_cache::load(job.path);\n                        {\n                            std::lock_guard<std::mutex> lock(m_mutex);\n                            if (job.generation == m_generation &&\n                                _wcsicmp(job.path.c_str(), m_path.c_str()) == 0) {\n                                for (auto& disk : disk_segments) {\n                                    cache_segment seg;\n                                    seg.generation = job.generation;\n                                    seg.start_seconds = static_cast<double>(disk.start_frame) /\n                                        static_cast<double>(kCacheRate);\n                                    const size_t frames = disk.vocals.size() / kCacheChannels;\n                                    seg.end_seconds = seg.start_seconds +\n                                        static_cast<double>(frames) / static_cast<double>(kCacheRate);\n                                    seg.vocals = std::move(disk.vocals);\n                                    seg.instrumental = std::move(disk.instrumental);\n                                    seg.external_waveform = false;\n                                    m_segments.push_back(\n                                        std::make_shared<cache_segment>(std::move(seg)));\n                                }\n                            }\n                            m_job_pending = !m_jobs.empty();\n                        }\n                        m_ready_cv.notify_all();\n                        continue;\n                    }\n\n                    if (job.need_stems) {\n                        bool already_cached = false;\n                        {\n                            std::lock_guard<std::mutex> lock(m_mutex);\n                            if (job.generation == m_generation &&\n                                _wcsicmp(job.path.c_str(), m_path.c_str()) == 0) {\n                                already_cached = internal_stem_range_ready_locked(\n                                    job.start_seconds, job.start_seconds + kCacheSeconds);\n                            }\n                        }\n                        if (already_cached) {\n                            release_waiters(job.generation);\n                            continue;\n                        }\n                    }\n\n                    std::vector<float> input;\n\n                    // Transport preview gets a separate decoder timeline. A random\n''',
    'worker restore')

repl(
'''                    {\n                        std::lock_guard<std::mutex>\n                            lock(m_mutex);\n\n                        // Seek/new-track happened while this job ran.\n                        if (job.generation !=\n                            m_generation) {\n\n                            m_job_pending =\n                                !m_jobs.empty();\n                        }\n                        else {\n                            const bool stem_payload_ok =\n                                !job.need_stems ||\n                                (separated &&\n                                 vocals.size() == input.size() &&\n                                 instrumental.size() == input.size());\n\n                            if (decoded && !input.empty() && stem_payload_ok) {\n                                // Only the first separated live cache block gets this\n                                // tiny fade. Original preview data is never altered.\n                                if (job.need_stems && job.start_seconds <= 0.000001) {\n                                    apply_first_block_fade(vocals);\n                                    apply_first_block_fade(instrumental);\n                                }\n\n                                const size_t frames = input.size() / kCacheChannels;\n''',
'''                    const bool stem_payload_ok =\n                        !job.need_stems ||\n                        (separated &&\n                         vocals.size() == input.size() &&\n                         instrumental.size() == input.size());\n\n                    if (decoded && !input.empty() && stem_payload_ok && job.need_stems) {\n                        if (job.start_seconds <= 0.000001) {\n                            apply_first_block_fade(vocals);\n                            apply_first_block_fade(instrumental);\n                        }\n                        const uint64_t start_frame = static_cast<uint64_t>(\n                            job.start_seconds * static_cast<double>(kCacheRate) + 0.5);\n                        persistent_stem_cache::save(\n                            job.path, start_frame, vocals, instrumental);\n                    }\n\n                    {\n                        std::lock_guard<std::mutex>\n                            lock(m_mutex);\n\n                        // Seek/new-track happened while this job ran.\n                        if (job.generation !=\n                            m_generation) {\n\n                            m_job_pending =\n                                !m_jobs.empty();\n                        }\n                        else {\n                            if (decoded && !input.empty() && stem_payload_ok) {\n                                const size_t frames = input.size() / kCacheChannels;\n''',
    'save completed block')

p.write_text(s, encoding='utf-8')
print('Persistent cache integration applied')
