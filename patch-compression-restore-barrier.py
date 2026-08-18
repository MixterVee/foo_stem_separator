from pathlib import Path

p = Path('stem_dsp.cpp')
s = p.read_text(encoding='utf-8-sig')

def repl(old, new, label):
    global s
    n = s.count(old)
    if n != 1:
        raise RuntimeError(f'{label}: expected 1 marker, found {n}')
    s = s.replace(old, new, 1)

old_new_track = '''        if (!m_path.empty()) {
            m_jobs.emplace_back(cache_job{
                m_generation, m_path, 0.0, false, false, false, true});
            m_job_pending = true;

            const stemmode::mode mode = stemmode::get();
            const bool warm_stems =
                mode != stemmode::mode::original || stem_precache::enabled();
            if (warm_stems) {
                // Pre-cache now means PRE-cache: start Spleeter as soon as a new
                // track begins even while Original is selected. The generated
                // segment contains Original + Vocals + Instrumental, so no
                // separate Original decoder job is needed for this region.
                queue_job_locked(0.0, true);
            } else {
                // Pre-cache was explicitly disabled: retain the cheap Original
                // decoder-only behavior and leave ONNX lazy.
                m_jobs.emplace_back(cache_job{
                    m_generation, m_path, 0.0, true, false, false});
                m_job_pending = true;
            }
        }
'''

new_new_track = '''        if (!m_path.empty()) {
            // Persistent restore is a startup barrier. Do not queue live Spleeter
            // work at the same time: compressed cache restore can take long enough
            // for playback/mode switching to observe a half-initialized cache.
            // The restore job publishes all disk segments atomically, then queues
            // only whatever initial work is still missing.
            m_jobs.emplace_back(cache_job{
                m_generation, m_path, 0.0, false, false, false, true});
            m_job_pending = true;
        }
'''
repl(old_new_track, new_new_track, 'new-track restore barrier')

old_restore = '''                    if (job.restore_persisted) {
                        auto disk_segments = persistent_stem_cache::load(job.path);
                        {
                            std::lock_guard<std::mutex> lock(m_mutex);
                            if (job.generation == m_generation &&
                                _wcsicmp(job.path.c_str(), m_path.c_str()) == 0) {
                                for (auto& disk : disk_segments) {
                                    cache_segment seg;
                                    seg.generation = job.generation;
                                    seg.start_seconds = static_cast<double>(disk.start_frame) /
                                        static_cast<double>(kCacheRate);
                                    const size_t frames = disk.vocals.size() / kCacheChannels;
                                    seg.end_seconds = seg.start_seconds +
                                        static_cast<double>(frames) / static_cast<double>(kCacheRate);
                                    seg.vocals = std::move(disk.vocals);
                                    seg.instrumental = std::move(disk.instrumental);
                                    seg.external_waveform = false;
                                    m_segments.push_back(
                                        std::make_shared<cache_segment>(std::move(seg)));
                                }
                            }
                            m_job_pending = !m_jobs.empty();
                        }
                        m_ready_cv.notify_all();
                        continue;
                    }
'''

new_restore = '''                    if (job.restore_persisted) {
                        auto disk_segments = persistent_stem_cache::load(job.path);
                        {
                            std::lock_guard<std::mutex> lock(m_mutex);
                            if (job.generation == m_generation &&
                                _wcsicmp(job.path.c_str(), m_path.c_str()) == 0) {
                                // Publish the complete restored cache as one state
                                // transition. Ordinary forward stem playback never
                                // sees a partially restored track.
                                for (auto& disk : disk_segments) {
                                    cache_segment seg;
                                    seg.generation = job.generation;
                                    seg.start_seconds = static_cast<double>(disk.start_frame) /
                                        static_cast<double>(kCacheRate);
                                    const size_t frames = disk.vocals.size() / kCacheChannels;
                                    seg.end_seconds = seg.start_seconds +
                                        static_cast<double>(frames) / static_cast<double>(kCacheRate);
                                    seg.vocals = std::move(disk.vocals);
                                    seg.instrumental = std::move(disk.instrumental);
                                    seg.external_waveform = false;
                                    m_segments.push_back(
                                        std::make_shared<cache_segment>(std::move(seg)));
                                }

                                const stemmode::mode mode = stemmode::get();
                                const bool warm_stems =
                                    mode != stemmode::mode::original || stem_precache::enabled();

                                if (warm_stems) {
                                    // If disk restore already covers the opening
                                    // live block, do not immediately run Spleeter
                                    // over the same samples again.
                                    if (!internal_stem_range_ready_locked(
                                            0.0, kCacheSeconds)) {
                                        queue_job_locked(0.0, true);
                                    }
                                } else {
                                    // Pre-cache disabled: retain the cheap Original
                                    // decoder-only startup behavior after restore.
                                    m_jobs.emplace_back(cache_job{
                                        m_generation, m_path, 0.0, true, false, false});
                                    m_job_pending = true;
                                }
                            }
                            m_job_pending = !m_jobs.empty();
                        }
                        m_ready_cv.notify_all();
                        continue;
                    }
'''
repl(old_restore, new_restore, 'restore publication and post-restore queue')

p.write_text(s, encoding='utf-8')
print('Applied compressed-cache restore barrier patch.')
