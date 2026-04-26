#pragma once
//
// MeetingRegistry — manages per-meeting directory + metadata.
//
// Concept
// -------
// Every H.323 call is associated with a "meeting" identified by caller_id
// (the E.164/alias of the calling party). All recordings for one meeting
// live under a single directory:
//
//     <output_dir>/<YYYYMMDD>_<callerId>/
//         main_01.mp4
//         main_02.mp4      <- after reconnect within 3-day window
//         aux_01.mp4
//         aux_02.mp4       <- per H.239 OLC (presenter switch)
//         meeting.json     <- segment list + wall-clock alignment
//
// A global index file lives at <output_dir>/index.json, recording every
// meeting's folder name, caller_id, and start/last-activity wall times.
// When a new call comes in, we look up recent meetings (within 3 days)
// with the same caller_id and reuse the folder if found. This handles:
//
//   - reconnect within the same day (drop + redial)
//   - a meeting that crosses midnight (same folder, continues numbering)
//   - a meeting that crosses midnight then drops and redials on the
//     next day (still same folder, because index lookup matches)
//
// The "3 days" window is based on the user's constraint that meeting
// numbers are guaranteed unique within the past 3 days.
//
// Timecode alignment
// ------------------
// Each segment records wall_start_ms / wall_end_ms (system_clock
// milliseconds since epoch). A player reconstructs the timeline by
// sorting segments by wall_start_ms; aux segments carry a parent_main
// reference and an offset_in_main_ms for PIP overlay rendering.
//
// Thread safety
// -------------
// All public methods are mutex-protected. MeetingRegistry is owned by
// RecorderEndpoint and lives as long as the endpoint.

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class MeetingContext;   // forward — opaque handle returned to caller

/// Type of a recording segment.
enum class SegmentType {
    Main,   // primary video/audio stream (one per H.323 connection)
    Aux     // H.239 extended video stream (one per OLC)
};

/// One recorded file within a meeting.
struct MeetingSegment {
    SegmentType type         = SegmentType::Main;
    std::string file;                    // basename, e.g. "main_01.mp4"
    int         index        = 0;        // 1-based sequence within its type
    int         connectionIdx = 0;       // which H.323 connection (1,2,...)
    int64_t     wallStartMs  = 0;        // system_clock ms since epoch
    int64_t     wallEndMs    = 0;        // 0 = still open
    // For aux segments only:
    std::string parentMain;              // filename of active main at open time
    int64_t     offsetInMainMs = 0;      // wallStartMs - parentMain.wallStartMs
};

/// One meeting's in-memory state. Opaque to callers — they only hold
/// a shared_ptr returned from openOrJoin().
class MeetingContext {
public:
    MeetingContext(std::string meetingId,
                   std::string callerId,
                   std::string dirPath,
                   int64_t     startWallMs,
                   std::string outputRoot);

    const std::string& meetingId() const { return meetingId_; }
    const std::string& callerId()  const { return callerId_;  }
    const std::string& dirPath()   const { return dirPath_;   }
    int64_t startWallMs()          const { return startWallMs_; }

    // Allocate the next numbered segment file (does NOT create the file on disk).
    // Updates internal counters. Returns absolute path.
    std::string allocMainPath(int connectionIdx);
    std::string allocAuxPath(int connectionIdx);

    // Record segment start/end (caller passes the path previously returned
    // by allocMainPath / allocAuxPath). Writes meeting.json after each
    // mutation so a crash leaves a recoverable record.
    void recordSegmentStart(SegmentType type,
                            const std::string& absPath,
                            int connectionIdx,
                            int64_t wallStartMs);
    void recordSegmentEnd(const std::string& absPath, int64_t wallEndMs);

    // Returns the basename of the currently-open main segment, or "" if none.
    std::string activeMainFile() const;
    int64_t     activeMainWallStartMs() const;

    // Write meeting.json atomically. Called automatically after each
    // start/end event; exposed for final flush on shutdown.
    void flushJson();

private:
    static std::string basename(const std::string& absPath);

    mutable std::mutex          mu_;
    std::string                 meetingId_;
    std::string                 callerId_;
    std::string                 dirPath_;    // absolute
    std::string                 outputRoot_; // absolute, for index.json
    int64_t                     startWallMs_ = 0;
    int64_t                     lastActivityMs_ = 0;
    int                         mainCounter_ = 0;
    int                         auxCounter_  = 0;
    std::string                 activeMainFile_;
    int64_t                     activeMainWallStartMs_ = 0;
    std::vector<MeetingSegment> segments_;
};

/// Global registry. One per endpoint. Thread-safe.
class MeetingRegistry {
public:
    explicit MeetingRegistry(std::string outputRoot);

    /// Find an existing meeting for this caller_id within the last 3 days,
    /// or create a new one. Returns a shared_ptr the caller holds for the
    /// duration of the H.323 connection.
    ///
    /// @param callerId    Non-empty. If empty, a synthetic "unknown" id is used.
    /// @param nowWallMs   Current wall-clock time in ms.
    std::shared_ptr<MeetingContext> openOrJoin(const std::string& callerId,
                                               int64_t nowWallMs);

    /// Called after any segment in the given meeting changes, so we can
    /// refresh the global index.json (last_activity_ms, end_ms).
    void touchIndex(const std::shared_ptr<MeetingContext>& ctx,
                    int64_t nowWallMs);

    /// Write index.json atomically.
    void flushIndex();

    const std::string& outputRoot() const { return outputRoot_; }

private:
    struct IndexEntry {
        std::string meetingId;      // folder basename, e.g. "20260423_820617"
        std::string callerId;
        int64_t     startWallMs = 0;
        int64_t     lastActivityMs = 0;
    };

    void loadIndex();                    // read index.json on startup
    IndexEntry* findRecent(const std::string& callerId, int64_t nowWallMs);

    mutable std::mutex               mu_;
    std::string                      outputRoot_;
    std::vector<IndexEntry>          index_;
    // Active contexts (keyed by meetingId) so concurrent calls with the
    // same caller_id share one context. Weak_ptr so completed meetings
    // can be garbage-collected once the last connection drops.
    std::vector<std::weak_ptr<MeetingContext>> active_;
};
