#pragma once

#include <QString>
#include <QStringList>
#include <QUuid>
#include <cstdint>
#include <nlohmann/json_fwd.hpp>

namespace censorcut {

enum class Source : int {
    Manual    = 0,
    Suggested = 1,
    Imported  = 2,
};

enum class Status : int {
    Pending   = 0,
    Confirmed = 1,
    Rejected  = 2,
};

/// A single cut range. Times are in milliseconds. The range is half-open:
/// [startMs, endMs). endMs must be strictly greater than startMs.
struct Marker {
    QUuid   id          = QUuid::createUuid();
    qint64  startMs     = 0;
    qint64  endMs       = 0;
    QString category    = QStringLiteral("Manual");
    QString note;
    Source  source      = Source::Manual;
    double  confidence  = 1.0;
    Status  status      = Status::Confirmed;
    /// Pubkey hexes of peers whose accept-decision feedback rows
    /// near-matched frames inside this marker's range. Populated by
    /// the analyzer when reading peers.jsonl. Used by MainWindow on
    /// confirm/reject to drive TrustLedger reward/penalty.
    QStringList contributingAuthors;

    [[nodiscard]] qint64 durationMs() const noexcept { return endMs - startMs; }
    [[nodiscard]] bool   isValid()    const noexcept { return endMs > startMs; }
    [[nodiscard]] bool   contains(qint64 ms) const noexcept {
        return ms >= startMs && ms < endMs;
    }
    [[nodiscard]] bool   overlaps(const Marker& other) const noexcept {
        return startMs < other.endMs && other.startMs < endMs;
    }
};

// JSON helpers — defined in Marker.cpp using nlohmann::json
void to_json(nlohmann::json& j, const Marker& m);
void from_json(const nlohmann::json& j, Marker& m);

QString sourceToString(Source s);
Source  sourceFromString(QStringView s);
QString statusToString(Status s);
Status  statusFromString(QStringView s);

} // namespace censorcut
