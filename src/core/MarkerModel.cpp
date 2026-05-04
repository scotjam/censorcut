#include "MarkerModel.h"

#include <QTime>
#include <algorithm>

namespace censorcut {

namespace {

QString formatTime(qint64 ms)
{
    const qint64 totalSec = ms / 1000;
    const int h = static_cast<int>(totalSec / 3600);
    const int m = static_cast<int>((totalSec % 3600) / 60);
    const int s = static_cast<int>(totalSec % 60);
    return QStringLiteral("%1:%2:%3")
        .arg(h, 2, 10, QChar('0'))
        .arg(m, 2, 10, QChar('0'))
        .arg(s, 2, 10, QChar('0'));
}

} // namespace

MarkerModel::MarkerModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int MarkerModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_markers.size();
}

QVariant MarkerModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_markers.size())
        return {};
    const Marker& m = m_markers.at(index.row());

    switch (role) {
        case Qt::DisplayRole: {
            const QString status = m.status == Status::Confirmed ? QStringLiteral("✓")
                                : m.status == Status::Pending   ? QStringLiteral("?")
                                : QStringLiteral("✗");
            return QStringLiteral("%1  %2 – %3   %4")
                .arg(status, formatTime(m.startMs), formatTime(m.endMs), m.category);
        }
        case IdRole:         return m.id;
        case StartMsRole:    return m.startMs;
        case EndMsRole:      return m.endMs;
        case CategoryRole:   return m.category;
        case NoteRole:       return m.note;
        case SourceRole:     return static_cast<int>(m.source);
        case ConfidenceRole: return m.confidence;
        case StatusRole:     return static_cast<int>(m.status);
        default:             return {};
    }
}

bool MarkerModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_markers.size())
        return false;
    Marker& m = m_markers[index.row()];
    bool changed = false;
    switch (role) {
        case StartMsRole:
            if (m.startMs != value.toLongLong()) { m.startMs = value.toLongLong(); changed = true; }
            break;
        case EndMsRole:
            if (m.endMs != value.toLongLong()) { m.endMs = value.toLongLong(); changed = true; }
            break;
        case CategoryRole:
            if (m.category != value.toString()) { m.category = value.toString(); changed = true; }
            break;
        case NoteRole:
            if (m.note != value.toString()) { m.note = value.toString(); changed = true; }
            break;
        case StatusRole: {
            const auto s = static_cast<Status>(value.toInt());
            if (m.status != s) { m.status = s; changed = true; }
            break;
        }
        default:
            return false;
    }
    if (changed) {
        emit dataChanged(index, index, { role, Qt::DisplayRole });
        emit markerChanged(m.id);
        if (role == StartMsRole || role == EndMsRole) {
            // Resort by startMs but preserve indices via reset
            const QUuid id = m.id;
            beginResetModel();
            resort();
            endResetModel();
            Q_UNUSED(id);
        }
    }
    return changed;
}

Qt::ItemFlags MarkerModel::flags(const QModelIndex& index) const
{
    if (!index.isValid()) return Qt::NoItemFlags;
    return Qt::ItemIsSelectable | Qt::ItemIsEnabled;
}

QHash<int, QByteArray> MarkerModel::roleNames() const
{
    return {
        { Qt::DisplayRole, "display" },
        { IdRole,          "id" },
        { StartMsRole,     "startMs" },
        { EndMsRole,       "endMs" },
        { CategoryRole,    "category" },
        { NoteRole,        "note" },
        { SourceRole,      "source" },
        { ConfidenceRole,  "confidence" },
        { StatusRole,      "status" },
    };
}

void MarkerModel::setMarkers(QList<Marker> markers)
{
    beginResetModel();
    m_markers = std::move(markers);
    resort();
    endResetModel();
}

void MarkerModel::addMarker(const Marker& m)
{
    if (!m.isValid()) return;
    // Find insertion point by startMs
    auto it = std::lower_bound(m_markers.begin(), m_markers.end(), m,
        [](const Marker& a, const Marker& b) { return a.startMs < b.startMs; });
    const int row = static_cast<int>(it - m_markers.begin());
    beginInsertRows({}, row, row);
    m_markers.insert(it, m);
    endInsertRows();
    emit markerAdded(m.id);
}

void MarkerModel::removeMarkerAt(int row)
{
    if (row < 0 || row >= m_markers.size()) return;
    const QUuid id = m_markers.at(row).id;
    beginRemoveRows({}, row, row);
    m_markers.removeAt(row);
    endRemoveRows();
    emit markerRemoved(id);
}

void MarkerModel::removeMarkerById(const QUuid& id)
{
    const int row = indexOfId(id);
    if (row >= 0) removeMarkerAt(row);
}

void MarkerModel::updateMarkerById(const QUuid& id, const Marker& replacement)
{
    const int row = indexOfId(id);
    if (row < 0) return;
    m_markers[row] = replacement;
    m_markers[row].id = id;  // preserve id
    const QModelIndex ix = index(row);
    emit dataChanged(ix, ix);
    emit markerChanged(id);
    // Re-sort if startMs moved
    beginResetModel();
    resort();
    endResetModel();
}

void MarkerModel::clear()
{
    if (m_markers.isEmpty()) return;
    beginResetModel();
    m_markers.clear();
    endResetModel();
}

int MarkerModel::indexOfId(const QUuid& id) const
{
    for (int i = 0; i < m_markers.size(); ++i)
        if (m_markers.at(i).id == id) return i;
    return -1;
}

std::optional<Marker> MarkerModel::markerAt(int row) const
{
    if (row < 0 || row >= m_markers.size()) return std::nullopt;
    return m_markers.at(row);
}

std::optional<Marker> MarkerModel::findById(const QUuid& id) const
{
    const int row = indexOfId(id);
    return row >= 0 ? std::optional<Marker>{m_markers.at(row)} : std::nullopt;
}

QList<Marker> MarkerModel::confirmedMarkers() const
{
    QList<Marker> out;
    for (const auto& m : m_markers)
        if (m.status == Status::Confirmed && m.isValid()) out.append(m);
    return out;
}

qint64 MarkerModel::totalConfirmedCutMs() const
{
    // Merge overlapping confirmed markers, then sum.
    auto cs = confirmedMarkers();
    if (cs.isEmpty()) return 0;
    std::sort(cs.begin(), cs.end(),
        [](const Marker& a, const Marker& b) { return a.startMs < b.startMs; });
    qint64 total = 0;
    qint64 mergedStart = cs.first().startMs;
    qint64 mergedEnd   = cs.first().endMs;
    for (int i = 1; i < cs.size(); ++i) {
        const auto& m = cs.at(i);
        if (m.startMs <= mergedEnd) {
            mergedEnd = std::max(mergedEnd, m.endMs);
        } else {
            total += mergedEnd - mergedStart;
            mergedStart = m.startMs;
            mergedEnd   = m.endMs;
        }
    }
    total += mergedEnd - mergedStart;
    return total;
}

void MarkerModel::resort()
{
    std::sort(m_markers.begin(), m_markers.end(),
        [](const Marker& a, const Marker& b) {
            if (a.startMs != b.startMs) return a.startMs < b.startMs;
            return a.endMs < b.endMs;
        });
}

} // namespace censorcut
