#include "MeetingRegistry.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

// ── helpers ─────────────────────────────────────────────────────────────────
namespace {

std::string formatDate(int64_t wallMs)
{
    std::time_t t = static_cast<std::time_t>(wallMs / 1000);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y%m%d");
    return oss.str();
}

constexpr int64_t kThreeDaysMs = 3LL * 24 * 60 * 60 * 1000;

// Write file atomically: write to tmp, then rename.
bool writeAtomic(const std::string& path, const std::string& content)
{
    std::string tmp = path + ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(content.data(), content.size());
        if (!ofs) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        // fallback: copy + remove
        fs::copy_file(tmp, path, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp, ec);
    }
    return !ec;
}

json segmentToJson(const MeetingSegment& s)
{
    json j;
    j["type"]           = (s.type == SegmentType::Main ? "main" : "aux");
    j["file"]           = s.file;
    j["index"]          = s.index;
    j["connection_idx"] = s.connectionIdx;
    j["wall_start_ms"]  = s.wallStartMs;
    j["wall_end_ms"]    = s.wallEndMs;
    if (s.wallEndMs > 0 && s.wallEndMs >= s.wallStartMs)
        j["duration_ms"] = s.wallEndMs - s.wallStartMs;
    if (s.type == SegmentType::Aux) {
        j["parent_main"]        = s.parentMain;
        j["offset_in_main_ms"]  = s.offsetInMainMs;
    }
    return j;
}

} // namespace

// ── MeetingContext ──────────────────────────────────────────────────────────
MeetingContext::MeetingContext(std::string meetingId,
                               std::string callerId,
                               std::string dirPath,
                               int64_t     startWallMs,
                               std::string outputRoot)
    : meetingId_(std::move(meetingId))
    , callerId_(std::move(callerId))
    , dirPath_(std::move(dirPath))
    , outputRoot_(std::move(outputRoot))
    , startWallMs_(startWallMs)
    , lastActivityMs_(startWallMs)
{
    std::error_code ec;
    fs::create_directories(dirPath_, ec);

    // If meeting.json already exists (reconnect case), load prior state so
    // we continue numbering where we left off.
    std::string jsonPath = dirPath_ + "/meeting.json";
    std::ifstream ifs(jsonPath);
    if (ifs) {
        try {
            json j;
            ifs >> j;
            if (j.contains("start_wall_ms")) {
                startWallMs_ = j["start_wall_ms"].get<int64_t>();
            }
            if (j.contains("segments") && j["segments"].is_array()) {
                for (const auto& js : j["segments"]) {
                    MeetingSegment s;
                    std::string t = js.value("type", "main");
                    s.type  = (t == "aux" ? SegmentType::Aux : SegmentType::Main);
                    s.file  = js.value("file", "");
                    s.index = js.value("index", 0);
                    s.connectionIdx = js.value("connection_idx", 0);
                    s.wallStartMs   = js.value("wall_start_ms", int64_t{0});
                    s.wallEndMs     = js.value("wall_end_ms",   int64_t{0});
                    s.parentMain    = js.value("parent_main", "");
                    s.offsetInMainMs = js.value("offset_in_main_ms", int64_t{0});
                    if (s.type == SegmentType::Main)
                        mainCounter_ = std::max(mainCounter_, s.index);
                    else
                        auxCounter_  = std::max(auxCounter_,  s.index);
                    segments_.push_back(std::move(s));
                }
            }
            spdlog::info("MeetingContext: resumed '{}' with {} prior segments",
                         meetingId_, segments_.size());
        } catch (const std::exception& e) {
            spdlog::warn("MeetingContext: failed to parse existing meeting.json: {}",
                         e.what());
        }
    }
}

std::string MeetingContext::basename(const std::string& absPath)
{
    auto pos = absPath.find_last_of("/\\");
    return (pos == std::string::npos) ? absPath : absPath.substr(pos + 1);
}

std::string MeetingContext::allocMainPath(int /*connectionIdx*/)
{
    std::lock_guard<std::mutex> lk(mu_);
    ++mainCounter_;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "main_%02d.mp4", mainCounter_);
    return dirPath_ + "/" + buf;
}

std::string MeetingContext::allocAuxPath(int /*connectionIdx*/)
{
    std::lock_guard<std::mutex> lk(mu_);
    ++auxCounter_;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "aux_%02d.mp4", auxCounter_);
    return dirPath_ + "/" + buf;
}

void MeetingContext::recordSegmentStart(SegmentType type,
                                        const std::string& absPath,
                                        int connectionIdx,
                                        int64_t wallStartMs)
{
    {
        std::lock_guard<std::mutex> lk(mu_);
        MeetingSegment s;
        s.type          = type;
        s.file          = basename(absPath);
        s.connectionIdx = connectionIdx;
        s.wallStartMs   = wallStartMs;
        s.wallEndMs     = 0;

        // Derive index from filename suffix, e.g. "main_03.mp4" → 3
        auto us = s.file.find_last_of('_');
        auto dot = s.file.find_last_of('.');
        if (us != std::string::npos && dot != std::string::npos && dot > us + 1) {
            try { s.index = std::stoi(s.file.substr(us + 1, dot - us - 1)); }
            catch (...) { s.index = 0; }
        }

        if (type == SegmentType::Main) {
            activeMainFile_        = s.file;
            activeMainWallStartMs_ = wallStartMs;
        } else {
            s.parentMain     = activeMainFile_;
            s.offsetInMainMs = (activeMainWallStartMs_ > 0)
                ? (wallStartMs - activeMainWallStartMs_) : 0;
        }
        lastActivityMs_ = wallStartMs;
        segments_.push_back(std::move(s));
    }
    flushJson();
}

void MeetingContext::recordSegmentEnd(const std::string& absPath, int64_t wallEndMs)
{
    std::string fn = basename(absPath);
    {
        std::lock_guard<std::mutex> lk(mu_);
        // End the most recent segment matching the filename that is still open.
        for (auto it = segments_.rbegin(); it != segments_.rend(); ++it) {
            if (it->file == fn && it->wallEndMs == 0) {
                it->wallEndMs = wallEndMs;
                break;
            }
        }
        if (activeMainFile_ == fn) {
            // main closed — clear the anchor for subsequent aux segments
            activeMainFile_.clear();
            activeMainWallStartMs_ = 0;
        }
        lastActivityMs_ = wallEndMs;
    }
    flushJson();
}

std::string MeetingContext::activeMainFile() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return activeMainFile_;
}

int64_t MeetingContext::activeMainWallStartMs() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return activeMainWallStartMs_;
}

void MeetingContext::flushJson()
{
    json j;
    std::vector<MeetingSegment> copy;
    int64_t startMs, lastMs;
    {
        std::lock_guard<std::mutex> lk(mu_);
        j["meeting_id"]    = meetingId_;
        j["caller_id"]     = callerId_;
        j["start_wall_ms"] = startWallMs_;
        j["last_activity_ms"] = lastActivityMs_;
        copy    = segments_;
        startMs = startWallMs_;
        lastMs  = lastActivityMs_;
        (void)startMs; (void)lastMs;
    }
    j["segments"] = json::array();
    for (const auto& s : copy) j["segments"].push_back(segmentToJson(s));

    std::string path = dirPath_ + "/meeting.json";
    if (!writeAtomic(path, j.dump(2))) {
        spdlog::warn("MeetingContext: failed to write {}", path);
    }
}

// ── MeetingRegistry ─────────────────────────────────────────────────────────
MeetingRegistry::MeetingRegistry(std::string outputRoot)
    : outputRoot_(std::move(outputRoot))
{
    std::error_code ec;
    fs::create_directories(outputRoot_, ec);
    loadIndex();
}

void MeetingRegistry::loadIndex()
{
    std::string path = outputRoot_ + "/index.json";
    std::ifstream ifs(path);
    if (!ifs) return;
    try {
        json j; ifs >> j;
        if (!j.contains("meetings") || !j["meetings"].is_array()) return;
        for (const auto& jm : j["meetings"]) {
            IndexEntry e;
            e.meetingId       = jm.value("meeting_id", "");
            e.callerId        = jm.value("caller_id",  "");
            e.startWallMs     = jm.value("start_wall_ms",    int64_t{0});
            e.lastActivityMs  = jm.value("last_activity_ms", int64_t{0});
            if (!e.meetingId.empty()) index_.push_back(std::move(e));
        }
        spdlog::info("MeetingRegistry: loaded {} meeting(s) from index.json",
                     index_.size());
    } catch (const std::exception& e) {
        spdlog::warn("MeetingRegistry: failed to parse index.json: {}", e.what());
    }
}

MeetingRegistry::IndexEntry*
MeetingRegistry::findRecent(const std::string& callerId, int64_t nowWallMs)
{
    // Scan backwards — most recent entries tend to be at the end
    IndexEntry* best = nullptr;
    int64_t bestLast = 0;
    for (auto& e : index_) {
        if (e.callerId != callerId) continue;
        int64_t age = nowWallMs - e.lastActivityMs;
        if (age < 0) age = 0;
        if (age > kThreeDaysMs) continue;
        if (e.lastActivityMs >= bestLast) {
            bestLast = e.lastActivityMs;
            best = &e;
        }
    }
    return best;
}

std::shared_ptr<MeetingContext>
MeetingRegistry::openOrJoin(const std::string& callerIdIn, int64_t nowWallMs)
{
    std::lock_guard<std::mutex> lk(mu_);

    std::string callerId = callerIdIn.empty() ? std::string("unknown") : callerIdIn;

    // 1) Check if there's already an active context for this caller_id
    for (auto it = active_.begin(); it != active_.end();) {
        auto sp = it->lock();
        if (!sp) { it = active_.erase(it); continue; }
        if (sp->callerId() == callerId) {
            spdlog::info("MeetingRegistry: reusing active meeting '{}' for caller_id='{}'",
                         sp->meetingId(), callerId);
            return sp;
        }
        ++it;
    }

    // 2) Check index.json for a meeting within the last 3 days
    IndexEntry* recent = findRecent(callerId, nowWallMs);
    std::string meetingId;
    std::string dirPath;
    int64_t     startWallMs = nowWallMs;

    if (recent) {
        meetingId   = recent->meetingId;
        startWallMs = recent->startWallMs;
        dirPath     = outputRoot_ + "/" + meetingId;
        recent->lastActivityMs = nowWallMs;
        spdlog::info("MeetingRegistry: rejoining recent meeting '{}' for caller_id='{}' "
                     "(age={}h)",
                     meetingId, callerId,
                     (nowWallMs - recent->startWallMs) / 3600000);
    } else {
        // 3) Brand new meeting
        meetingId = formatDate(nowWallMs) + "_" + callerId;
        dirPath   = outputRoot_ + "/" + meetingId;

        IndexEntry e;
        e.meetingId      = meetingId;
        e.callerId       = callerId;
        e.startWallMs    = nowWallMs;
        e.lastActivityMs = nowWallMs;
        index_.push_back(e);
        spdlog::info("MeetingRegistry: created new meeting '{}' for caller_id='{}'",
                     meetingId, callerId);
    }

    auto ctx = std::make_shared<MeetingContext>(meetingId, callerId, dirPath,
                                                startWallMs, outputRoot_);
    active_.push_back(ctx);
    // Persist index now so crash between creation and first segment doesn't lose it
    flushIndex();  // internal: assumes mu_ already held
    return ctx;
}

void MeetingRegistry::touchIndex(const std::shared_ptr<MeetingContext>& ctx,
                                 int64_t nowWallMs)
{
    if (!ctx) return;
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& e : index_) {
        if (e.meetingId == ctx->meetingId()) {
            e.lastActivityMs = nowWallMs;
            break;
        }
    }
    flushIndex();
}

void MeetingRegistry::flushIndex()
{
    // Assumes caller already holds mu_ (or is during construction where
    // there are no readers yet). Both call sites satisfy this.
    json j;
    j["meetings"] = json::array();
    for (const auto& e : index_) {
        json je;
        je["meeting_id"]       = e.meetingId;
        je["caller_id"]        = e.callerId;
        je["start_wall_ms"]    = e.startWallMs;
        je["last_activity_ms"] = e.lastActivityMs;
        j["meetings"].push_back(je);
    }
    std::string path = outputRoot_ + "/index.json";
    if (!writeAtomic(path, j.dump(2))) {
        spdlog::warn("MeetingRegistry: failed to write {}", path);
    }
}
